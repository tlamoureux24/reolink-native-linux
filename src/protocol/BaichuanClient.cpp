#include "BaichuanClient.h"

#include "core/Log.h"
#include "protocol/BcCrypto.h"
#include "protocol/BcSession.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QQueue>
#include <QThread>
#include <QtEndian>

#include <map>
#include <mutex>
#include <tuple>

extern "C" {
#include <libavutil/error.h>
}

namespace rl {

namespace {

constexpr int kMaxRing = 8 * 1024 * 1024; // backpressure cap for the decoded buffer

// cmd 3 live request, as the official SDK writes it (BaichuanPreviewer): the
// handle is (stream type << 8) | channel, and "newestIFrame" makes the device
// start from its latest key frame instead of waiting for the next one.
QByteArray previewXml(int channel, bool mainStream)
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body><Preview version=\"1.1\">\n<channelId>"
        + QByteArray::number(channel) + "</channelId>\n<handle>"
        + QByteArray::number(((mainStream ? 0 : 1) << 8) | channel) + "</handle>\n<streamType>"
        + (mainStream ? "mainStream" : "subStream")
        + "</streamType>\n<policy>newestIFrame</policy>\n</Preview></body>\n";
}

// cmd 4: close a live stream, under the message id that opened it.
QByteArray previewStopXml(int channel, bool mainStream)
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body><Preview version=\"1.1\">\n<channelId>"
        + QByteArray::number(channel) + "</channelId>\n<handle>"
        + QByteArray::number(((mainStream ? 0 : 1) << 8) | channel) + "</handle>\n</Preview></body>\n";
}

// The media bytes of one live/playback message: the extension says how many
// leading payload bytes are AES-encrypted (encryptLen); the rest is clear.
QByteArray mediaBytes(const QByteArray &body, quint32 payloadOffset, const QByteArray &aesKey)
{
    int encLen = 0;
    if (payloadOffset > 0) {
        const QByteArray ext = bc::aesCfb(body.left(int(payloadOffset)), aesKey, true);
        const int p = ext.indexOf("<encryptLen>");
        if (p >= 0) {
            const int q = ext.indexOf("</encryptLen>", p);
            encLen = ext.mid(p + 12, q - p - 12).toInt();
        }
    }
    const QByteArray payload = body.mid(int(payloadOffset));
    return encLen > 0 ? bc::aesCfb(payload.left(encLen), aesKey, true) + payload.mid(encLen) : payload;
}

} // namespace

BaichuanClient::BaichuanClient(Params params) : m_p(std::move(params)) {}

BaichuanClient::~BaichuanClient()
{
    stop();
    // Join (not detach): the worker touches our members, so it must finish before
    // they are destroyed. It never holds a reference to us, so this can't
    // self-deadlock, and every network wait is short + abort-checked, so the join
    // is bounded (sub-second). The last owning reference is dropped by the
    // StreamPlayer decode worker, so this runs off the GUI thread in practice.
    if (m_thread.joinable())
        m_thread.join();
}

void BaichuanClient::start()
{
    if (m_started)
        return;
    m_started = true;
    m_thread = std::thread([this] { run(); });
}

void BaichuanClient::stop()
{
    m_abort.store(true);
    QMutexLocker lock(&m_mutex);
    m_finished = true;
    m_cond.wakeAll();
}

QString BaichuanClient::ffmpegFormat()
{
    QMutexLocker lock(&m_mutex);
    return m_format;
}

int BaichuanClient::read(unsigned char *buf, int size)
{
    QMutexLocker lock(&m_mutex);
    while (m_ring.isEmpty() && !m_finished && !m_abort.load())
        m_cond.wait(&m_mutex);
    if (m_abort.load() && m_ring.isEmpty())
        return AVERROR_EOF;
    if (m_ring.isEmpty())
        return m_finished ? AVERROR_EOF : 0;
    const int n = qMin(size, static_cast<int>(m_ring.size()));
    memcpy(buf, m_ring.constData(), n);
    m_ring.remove(0, n);
    m_cond.wakeAll(); // unblock the producer if it was backpressured
    return n;
}

void BaichuanClient::pushAnnexB(const QByteArray &bytes, bool block)
{
    QMutexLocker lock(&m_mutex);
    // Backpressure: block the producer if the reader falls far behind.
    if (!block && m_ring.size() > kMaxRing)
        m_ring.clear(); // a live view that far behind restarts from here
    while (m_ring.size() > kMaxRing && !m_abort.load())
        m_cond.wait(&m_mutex);
    if (m_abort.load())
        return;
    m_ring.append(bytes);
    m_cond.wakeAll();
}

void BaichuanClient::run()
{
    if (m_p.startEpoch <= 0)
        runSessionLive();
    else
        runSessionReplay();
    QMutexLocker lock(&m_mutex);
    m_finished = true;
    m_cond.wakeAll();
}

void BaichuanClient::setRate(double rate)
{
    m_rate.store(qBound(0.125, rate, 16.0));
}

bool BaichuanClient::seek(qint64 startEpoch)
{
    if (startEpoch <= 0)
        return false;
    QMutexLocker lock(&m_mutex);
    if (m_finished || m_abort.load())
        return false;
    m_seekEpoch = startEpoch;
    return true;
}

} // namespace rl

namespace rl {

// Live streams open inside the device's session. As in the official SDK, a
// device streams each (channel, stream type) once per session — opening it
// again replaces it — so every client watching it shares the one stream.
struct SharedLive {
    std::shared_ptr<BcSession> session;
    quint32 id = 0;
    int channel = 0;
    bool main = true;
    BcMediaParser parser; // fed on the session worker only
    QMutex mutex;         // subscribers
    QList<BaichuanClient *> subscribers;
    std::atomic<qint64> lastMedia{0};
    std::atomic<bool> refused{false};
};
static std::mutex g_liveMutex;
static std::map<std::tuple<BcSession *, int, bool>, std::shared_ptr<SharedLive>> g_live;

void BaichuanClient::deliverShared(const BcMediaParser::VideoFrame &f)
{
    if (f.codec != BcMediaParser::Codec::Unknown) {
        QMutexLocker lock(&m_mutex);
        if (m_format.isEmpty())
            m_format = (f.codec == BcMediaParser::Codec::H265) ? QStringLiteral("hevc")
                                                               : QStringLiteral("h264");
    }
    if (!m_sharedKeyFrame && !f.keyFrame)
        return; // joined mid-stream: start at the next key frame
    m_sharedKeyFrame = true;
    pushAnnexB(f.annexB, /*block=*/false);
}

void BaichuanClient::deliverSharedAudio(const BcMediaParser::AudioFrame &a)
{
    if (m_sharedKeyFrame && m_p.onAudio)
        m_p.onAudio(a);
}

// Live view inside the device's session, opened the way the official client
// does (BaichuanPreviewer): an id of its own, cmd 3 to open, cmd 4 to close
// when its last viewer leaves.
void BaichuanClient::runSessionLive()
{
    BcSession::Params sp;
    sp.target = {m_p.host, m_p.port, m_p.linkUid, false};
    sp.username = m_p.username;
    sp.password = m_p.password;
    const std::shared_ptr<BcSession> s = BcSession::acquire(sp);
    if (!s->waitReady(30000, &m_abort))
        return;

    std::shared_ptr<SharedLive> live;
    {
        std::lock_guard lock(g_liveMutex);
        auto &slot = g_live[{s.get(), m_p.channel, m_p.mainStream}];
        if (!slot) {
            slot = std::make_shared<SharedLive>();
            slot->session = s;
            slot->channel = m_p.channel;
            slot->main = m_p.mainStream;
            slot->id = s->newMessId(m_p.mainStream ? 0 : 1);
            slot->lastMedia = QDateTime::currentMSecsSinceEpoch();
            SharedLive *l = slot.get();
            l->parser.onVideo = [l](const BcMediaParser::VideoFrame &f) {
                QMutexLocker lock(&l->mutex);
                for (BaichuanClient *c : std::as_const(l->subscribers))
                    c->deliverShared(f);
            };
            l->parser.onAudio = [l](const BcMediaParser::AudioFrame &a) {
                QMutexLocker lock(&l->mutex);
                for (BaichuanClient *c : std::as_const(l->subscribers))
                    c->deliverSharedAudio(a);
            };
            const QByteArray key = s->aesKey();
            s->route(l->id, [l, key](const BcMessage &m) {
                if (m.cmd != 3)
                    return;
                if (m.body.isEmpty()) {
                    if (m.status != 0 && m.status != 200)
                        l->refused = true;
                    return;
                }
                l->lastMedia = QDateTime::currentMSecsSinceEpoch();
                l->parser.append(mediaBytes(m.body, m.payloadOffset, key));
            });
            s->send(3, quint8(m_p.channel), l->id, {},
                    bc::aesCfb(previewXml(m_p.channel, m_p.mainStream), key, false));
            qCInfo(lcProto) << "live opened: channel" << m_p.channel
                            << (m_p.mainStream ? "main" : "sub") << "on" << s->linkDescription();
        }
        live = slot;
        QMutexLocker subs(&live->mutex);
        live->subscribers.append(this);
    }

    // Until stopped, the session ends, the device refuses, or no media came
    // for 20 s (the pane then starts over).
    while (!m_abort.load() && s->isAlive() && !live->refused
           && QDateTime::currentMSecsSinceEpoch() - live->lastMedia < 20000)
        QThread::msleep(100);

    bool last = false;
    {
        std::lock_guard lock(g_liveMutex);
        {
            QMutexLocker subs(&live->mutex);
            live->subscribers.removeOne(this);
            last = live->subscribers.isEmpty();
        }
        if (last)
            g_live.erase({s.get(), live->channel, live->main});
    }
    if (last) {
        s->unroute(live->id);
        if (s->isAlive())
            s->send(4, quint8(live->channel), live->id, {},
                    bc::aesCfb(previewStopXml(live->channel, live->main), s->aesKey(), false));
    }
}


// Recorded playback inside the device's session, the way the official client
// plays recordings (BaichuanReplayer): one FILE at a time — cmd 5 with the
// file's path, cmd 123 to seek in it, cmd 7 to stop — then the next file.
//
// A device sends a file as fast as it can (about ten times real time on a
// LAN; <playSpeed> in the request did not slow an RLC-811A down), while this
// thread plays it at the chosen speed. What waits is queued here, and the
// queue is bounded the same way: past kQueueHighSecs of video the file is
// stopped (cmd 7), and below kQueueLowSecs it is asked again from where the
// queue ends (cmd 5 + 123), the frames already queued being skipped.
void BaichuanClient::runSessionReplay()
{
    constexpr double kQueueHighSecs = 20;
    constexpr double kQueueLowSecs = 5;
    constexpr qint64 kQueueMaxBytes = 96LL * 1024 * 1024;

    const QVector<BcReplayFile> &files = m_p.replayFiles;
    if (files.isEmpty())
        return;
    BcSession::Params sp;
    sp.target = {m_p.host, m_p.port, m_p.linkUid, false};
    sp.username = m_p.username;
    sp.password = m_p.password;
    const std::shared_ptr<BcSession> s = BcSession::acquire(sp);
    if (!s->waitReady(30000, &m_abort))
        return;
    const QByteArray key = s->aesKey();
    const QByteArray uid = m_p.uid.toUtf8();
    const QByteArray ch = QByteArray::number(m_p.channel);
    const QByteArray streamName = m_p.mainStream ? "mainStream" : "subStream";
    const QByteArray ext = BcSession::channelExtension(m_p.channel);
    const quint8 hdrCh = quint8(m_p.channel + 1);

    // The file playing `epoch`, else the first one starting after it.
    auto fileAt = [&](qint64 epoch) {
        for (int i = 0; i < files.size(); ++i)
            if (epoch < files[i].end && (epoch >= files[i].start || i == 0 || epoch >= files[i - 1].end))
                return i;
        return -1;
    };
    auto tm = [](const QDateTime &t) {
        return "<year>" + QByteArray::number(t.date().year()) + "</year>\n<month>"
            + QByteArray::number(t.date().month()) + "</month>\n<day>" + QByteArray::number(t.date().day())
            + "</day>\n<hour>" + QByteArray::number(t.time().hour()) + "</hour>\n<minute>"
            + QByteArray::number(t.time().minute()) + "</minute>\n<second>"
            + QByteArray::number(t.time().second()) + "</second>\n";
    };

    // Frames from the session worker, in stream order, waiting for their time.
    struct Item {
        bool audio = false;
        BcMediaParser::VideoFrame video;
        BcMediaParser::AudioFrame sound;
    };
    struct Queue {
        QMutex mutex;
        QQueue<Item> items;
        qint64 bytes = 0;
        quint32 headUs = 0, tailUs = 0; // video timestamps at both ends
        bool haveVideo = false;
        quint32 skipUntilUs = 0;        // after a resume: already queued up to here
        bool skipping = false;
        bool markFirst = false;         // the next video frame starts a file
        quint32 fileUs0 = 0;            // its timestamp
        bool haveFileUs0 = false;
        std::atomic<qint64> lastMedia{0};
        std::atomic<bool> resetParser{false};
        std::atomic<bool> refused{false};
        BcMediaParser parser; // session worker only
        double queuedSecs()
        {
            return haveVideo ? (qint64(tailUs) - qint64(headUs)) / 1e6 : 0;
        }
        void clear()
        {
            items.clear();
            bytes = 0;
            haveVideo = false;
            skipping = false;
        }
    };
    Queue q;
    q.parser.onVideo = [&q](const BcMediaParser::VideoFrame &f) {
        QMutexLocker lock(&q.mutex);
        if (q.skipping) {
            // Queued before the stream was stopped; a timestamp far behind
            // means the device restarted its clock, so nothing is skipped.
            const qint64 us = f.microseconds, until = q.skipUntilUs;
            if (us <= until && us + 30000000 > until)
                return;
            q.skipping = false;
        }
        if (q.markFirst) {
            q.markFirst = false;
            q.fileUs0 = f.microseconds;
            q.haveFileUs0 = true;
        }
        if (!q.haveVideo) {
            q.headUs = f.microseconds;
            q.haveVideo = true;
        }
        q.tailUs = f.microseconds;
        q.bytes += f.annexB.size();
        Item it;
        it.video = f;
        q.items.enqueue(it);
    };
    q.parser.onAudio = [&q](const BcMediaParser::AudioFrame &a) {
        QMutexLocker lock(&q.mutex);
        if (q.skipping)
            return;
        Item it;
        it.audio = true;
        it.sound = a;
        q.items.enqueue(it);
    };

    // What the device is doing with the current file.
    enum class Feed { Sending, Stopped, Done };
    Feed feed = Feed::Done;
    int current = -1;
    quint32 id = 0;
    int seq = 0;
    qint64 fileStartedAt = 0;
    auto stopFile = [&] {
        if (current < 0 || feed != Feed::Sending)
            return;
        s->unroute(id);
        const BcReplayFile &f = files[current];
        const QByteArray stop = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<FileInfoList version=\"1.1\">\n<FileInfo>\n<uid>"
            + uid + "</uid>\n<channelId>" + ch + "</channelId>\n<name>" + f.name.toUtf8()
            + "</name>\n<streamType>" + streamName + "</streamType>\n<Id>" + f.id.toUtf8()
            + "</Id>\n</FileInfo>\n</FileInfoList>\n</body>\n";
        if (s->isAlive())
            s->send(7, 0, id, {}, bc::aesCfb(stop, key, false));
        feed = Feed::Stopped;
    };
    auto seekInFile = [&](qint64 epoch) {
        const QByteArray seek = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<ReplaySeek version=\"1.1\">\n<channelId>"
            + ch + "</channelId>\n<seq>" + QByteArray::number(++seq) + "</seq>\n<seekTime>\n"
            + tm(QDateTime::fromSecsSinceEpoch(epoch)) + "</seekTime>\n</ReplaySeek>\n</body>\n";
        s->send(123, hdrCh, id, bc::aesCfb(ext, key, false), bc::aesCfb(seek, key, false));
    };
    // `fresh`: a new file or position (else the rest of a file stopped by the
    // queue bound, whose timestamps carry on).
    qint64 fileEpoch0 = 0; // where the current file was asked from
    auto startFile = [&](int i, qint64 epoch, bool fresh) {
        if (fresh) {
            QMutexLocker lock(&q.mutex);
            q.markFirst = true;
            q.haveFileUs0 = false;
            fileEpoch0 = qMax(epoch, files[i].start);
        }
        current = i;
        feed = Feed::Sending;
        id = s->newMessId();
        q.resetParser = true;
        fileStartedAt = QDateTime::currentMSecsSinceEpoch();
        q.lastMedia = 0;
        s->route(id, [&q, key](const BcMessage &m) {
            if (m.cmd != 5)
                return;
            if (m.payloadOffset == 0) {
                if (m.status != 0 && m.status != 200)
                    q.refused = true;
                return; // the XML reply, not media
            }
            if (q.resetParser.exchange(false))
                q.parser.reset();
            q.lastMedia = QDateTime::currentMSecsSinceEpoch();
            q.parser.append(mediaBytes(m.body, m.payloadOffset, key));
        });
        const BcReplayFile &f = files[i];
        const QByteArray play = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<FileInfoList version=\"1.1\">\n<FileInfo>\n<channelId>"
            + ch + "</channelId>\n<Id>" + f.id.toUtf8() + "</Id>\n<uid>" + uid + "</uid>\n<supportSub>"
            + QByteArray(m_p.mainStream ? "0" : "1") + "</supportSub>\n<streamType>" + streamName
            + "</streamType>\n</FileInfo>\n</FileInfoList>\n</body>\n";
        s->send(5, hdrCh, id, bc::aesCfb(ext, key, false), bc::aesCfb(play, key, false));
        if (epoch > f.start + 1)
            seekInFile(epoch);
    };

    const int first = fileAt(m_p.startEpoch);
    if (first < 0)
        return;
    startFile(first, m_p.startEpoch, true);
    qCInfo(lcProto) << "replay opened:" << streamName << "from"
                    << QDateTime::fromSecsSinceEpoch(m_p.startEpoch).toString(Qt::ISODate) << "on"
                    << s->linkDescription();

    bool started = false; // a key frame was played since the last (re)start
    QElapsedTimer clock;
    quint32 firstMicros = 0;
    double pacedRate = m_rate.load();
    while (!m_abort.load() && s->isAlive() && !q.refused) {
        qint64 seekTo = 0;
        {
            QMutexLocker lock(&m_mutex);
            if (m_seekEpoch > 0) {
                seekTo = m_seekEpoch;
                m_seekEpoch = 0;
            }
        }
        if (seekTo > 0) {
            const int target = fileAt(seekTo);
            {
                QMutexLocker lock(&q.mutex);
                q.clear();
            }
            {
                QMutexLocker lock(&m_mutex);
                m_ring.clear(); // drop queued old-position frames so playback jumps
                m_cond.wakeAll();
            }
            started = false;
            clock.invalidate();
            if (target < 0) {
                stopFile();
                break;
            }
            if (target == current && feed == Feed::Sending) {
                {
                    QMutexLocker lock(&q.mutex);
                    q.markFirst = true;
                    q.haveFileUs0 = false;
                }
                fileEpoch0 = seekTo;
                q.resetParser = true;
                seekInFile(seekTo);
            } else {
                stopFile();
                startFile(target, seekTo, true);
            }
        }

        // Keep the queue bounded: stop the file when far ahead, ask for the
        // rest when it runs low; the next file once this one is all queued if
        // it follows on (after a gap, playback ends where the page's playhead
        // expects it to, and the page starts the next one when it gets there).
        double queued = 0;
        qint64 queuedBytes = 0;
        quint32 tailUs = 0, fileUs0 = 0;
        bool haveFileUs0 = false;
        {
            QMutexLocker lock(&q.mutex);
            queued = q.queuedSecs();
            queuedBytes = q.bytes;
            tailUs = q.tailUs;
            fileUs0 = q.fileUs0;
            haveFileUs0 = q.haveFileUs0;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (feed == Feed::Sending) {
            if (queued > kQueueHighSecs || queuedBytes > kQueueMaxBytes) {
                qCDebug(lcProto) << "replay: queue full (" << queued << "s," << queuedBytes
                                 << "bytes), file stopped";
                stopFile();
                QMutexLocker lock(&q.mutex);
                q.skipUntilUs = tailUs;
            } else if (q.lastMedia ? now - q.lastMedia > 4000 : now - fileStartedAt > 15000) {
                stopFile(); // the whole file is here
                feed = Feed::Done;
                qCDebug(lcProto) << "replay: file" << files[current].name << "received";
            }
        } else if (queued < kQueueLowSecs && current >= 0) {
            if (feed == Feed::Stopped) {
                // Again from where the queue ends; what was queued is skipped.
                const qint64 resumeAt = fileEpoch0
                    + (haveFileUs0 ? (qint64(tailUs) - qint64(fileUs0)) / 1000000 : 0);
                {
                    QMutexLocker lock(&q.mutex);
                    q.skipping = haveFileUs0;
                }
                qCDebug(lcProto) << "replay: resumed at"
                                 << QDateTime::fromSecsSinceEpoch(resumeAt).time().toString();
                startFile(current, qMax(resumeAt, files[current].start), false);
            } else if (current + 1 < files.size()
                       && files[current + 1].start <= files[current].end + 2) {
                // Footage goes on in the next file: no gap, so no pause either.
                qCDebug(lcProto) << "replay: next file" << files[current + 1].name;
                startFile(current + 1, files[current + 1].start, true);
            } else if (queued <= 0) {
                QMutexLocker lock(&q.mutex);
                if (q.items.isEmpty())
                    break; // the day's last file has played
            }
        }

        Item it;
        bool have = false;
        {
            QMutexLocker lock(&q.mutex);
            if (!q.items.isEmpty()) {
                it = q.items.dequeue();
                have = true;
                if (!it.audio) {
                    q.bytes -= it.video.annexB.size();
                    if (q.items.isEmpty())
                        q.haveVideo = false;
                    else
                        q.headUs = it.video.microseconds;
                }
            }
        }
        if (!have) {
            QThread::msleep(20);
            continue;
        }
        if (it.audio) {
            if (started && m_p.onAudio && m_rate.load() == 1.0)
                m_p.onAudio(it.sound);
            continue;
        }
        const BcMediaParser::VideoFrame &f = it.video;
        if (f.codec != BcMediaParser::Codec::Unknown) {
            QMutexLocker lock(&m_mutex);
            if (m_format.isEmpty())
                m_format = (f.codec == BcMediaParser::Codec::H265) ? QStringLiteral("hevc")
                                                                   : QStringLiteral("h264");
        }
        if (!started && !f.keyFrame)
            continue;
        started = true;
        // Pace from the frame timestamps at the chosen speed; a new file, a
        // seek, a speed change or a pause (the reader stopped) rebases.
        const double rate = m_rate.load();
        if (f.microseconds) {
            const qint64 elapsed = clock.isValid() ? clock.elapsed() : 0;
            const qint64 targetMs = clock.isValid()
                ? qint64((qint64(f.microseconds) - qint64(firstMicros)) / 1000 / rate) : 0;
            if (!clock.isValid() || rate != pacedRate || targetMs < 0
                || targetMs - elapsed > 10000 || elapsed - targetMs > 1000) {
                clock.restart();
                firstMicros = f.microseconds;
                pacedRate = rate;
            } else {
                for (qint64 wait = targetMs - clock.elapsed(); wait > 0 && !m_abort.load();
                     wait = targetMs - clock.elapsed()) {
                    QThread::msleep(static_cast<unsigned long>(qMin<qint64>(wait, 50)));
                    QMutexLocker lock(&m_mutex);
                    if (m_seekEpoch > 0)
                        break; // a seek waits for nobody
                }
            }
        }
        pushAnnexB(f.annexB);
    }
    stopFile();
}

} // namespace rl
