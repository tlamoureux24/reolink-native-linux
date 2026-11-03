#include "BcSession.h"

#include "core/Log.h"
#include "protocol/BcCrypto.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QtEndian>

#include <chrono>
#include <map>
#include <mutex>

namespace rl {

namespace {

constexpr quint32 kMagic = 0x0ABCDEF0;
constexpr quint32 kMagicRev = 0x0FEDCBA0;
constexpr quint16 kClassModern = 0x6414;
constexpr quint16 kClassLegacy = 0x6514;
// How long an unused session stays open, so the next user (a live pane after
// the validation that found the device, a pane switching streams) needn't set
// up a new P2P connection.
constexpr int kLingerMs = 20000;
// As the official SDK: a heartbeat (cmd 93) after 5 s without traffic, and
// the device is given up after 20 s of silence.
constexpr int kHeartbeatMs = 5000;
constexpr int kSilenceMs = 20000;

void putLE32(QByteArray &b, quint32 v)
{
    char t[4];
    qToLittleEndian(v, t);
    b.append(t, 4);
}
void putLE16(QByteArray &b, quint16 v)
{
    char t[2];
    qToLittleEndian(v, t);
    b.append(t, 2);
}
quint32 getLE32(const char *p)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(p));
}
quint16 getLE16(const char *p)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(p));
}

QByteArray header(quint32 cmd, quint8 ch, quint32 messId, quint16 status, quint16 cls, int len)
{
    QByteArray m;
    putLE32(m, kMagic);
    putLE32(m, cmd);
    putLE32(m, static_cast<quint32>(len));
    m.append(char(ch));
    m.append(char(messId & 0xFF));
    m.append(char((messId >> 8) & 0xFF));
    m.append(char((messId >> 16) & 0xFF));
    putLE16(m, status);
    putLE16(m, cls);
    return m;
}

// One complete message from `buf` (consumed). False if incomplete; a buffer
// that doesn't start with a header is dropped up to the next magic.
bool parse(QByteArray &buf, BcMessage &m)
{
    for (;;) {
        if (buf.size() < 20)
            return false;
        const quint32 magic = getLE32(buf.constData());
        if (magic == kMagic || magic == kMagicRev)
            break;
        const int next = buf.indexOf(QByteArray::fromHex("f0debc0a"), 1);
        buf.remove(0, next < 0 ? buf.size() - 3 : next);
    }
    const quint32 len = getLE32(buf.constData() + 8);
    const quint16 cls = getLE16(buf.constData() + 18);
    const bool modern = cls == kClassModern || cls == 0x0000;
    const int hdr = modern ? 24 : 20;
    if (buf.size() < hdr + static_cast<int>(len))
        return false;
    m.cmd = getLE32(buf.constData() + 4);
    m.channel = static_cast<quint8>(buf[12]);
    m.messId = quint32(uchar(buf[13])) | quint32(uchar(buf[14])) << 8 | quint32(uchar(buf[15])) << 16;
    m.status = getLE16(buf.constData() + 16);
    m.cls = cls;
    m.payloadOffset = modern ? getLE32(buf.constData() + 20) : 0;
    m.body = buf.mid(hdr, static_cast<int>(len));
    buf.remove(0, hdr + static_cast<int>(len));
    return true;
}

QByteArray sessionKey(const BcSession::Params &p)
{
    // The password is part of the key: a changed one must not reuse a session
    // logged in with the old one.
    const QString where = p.target.viaUid
                              ? QStringLiteral("uid:") + p.target.uid
                              : QStringLiteral("tcp:%1:%2").arg(p.target.host).arg(p.target.port);
    return (where + QLatin1Char('|') + p.username + QLatin1Char('|')).toUtf8()
        + QCryptographicHash::hash(p.password.toUtf8(), QCryptographicHash::Sha256).toHex();
}

qint64 nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

// Live sessions by device. An entry holds a reference of its own; once only
// that one is left, the session is idle, and after kLingerMs it is dropped.
struct BcSessionRegistry {
    struct Entry {
        std::shared_ptr<BcSession> session;
        qint64 idleSince = 0;
    };
    std::mutex mutex;
    std::map<QByteArray, Entry> entries;
    std::thread reaper;
    bool reaperStarted = false;

    void reap()
    {
        std::vector<std::shared_ptr<BcSession>> drop; // destroyed outside the lock
        {
            std::lock_guard lock(mutex);
            const qint64 now = nowMs();
            for (auto it = entries.begin(); it != entries.end();) {
                Entry &e = it->second;
                const bool idle = e.session.use_count() == 1;
                if (!idle)
                    e.idleSince = 0;
                else if (!e.idleSince)
                    e.idleSince = now;
                if (!e.session->isAlive() || (idle && now - e.idleSince > kLingerMs)) {
                    drop.push_back(std::move(e.session));
                    it = entries.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    static BcSessionRegistry &instance()
    {
        // Never destroyed: its reaper thread runs until the process ends, and
        // sessions still open then simply end with it.
        static BcSessionRegistry *r = new BcSessionRegistry;
        return *r;
    }
};

std::shared_ptr<BcSession> BcSession::acquire(const Params &params)
{
    BcSessionRegistry &r = BcSessionRegistry::instance();
    r.reap();
    const QByteArray key = sessionKey(params);
    std::lock_guard lock(r.mutex);
    if (!r.reaperStarted) {
        r.reaperStarted = true;
        std::thread([&r] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                r.reap();
            }
        }).detach();
    }
    auto it = r.entries.find(key);
    if (it != r.entries.end() && it->second.session->isAlive()) {
        it->second.idleSince = 0;
        return it->second.session;
    }
    std::shared_ptr<BcSession> s(new BcSession(params));
    BcSession *raw = s.get();
    raw->m_thread = std::thread([raw] { raw->run(); });
    r.entries[key] = {s, 0};
    return s;
}

BcSession::BcSession(const Params &params) : m_p(params) {}

BcSession::~BcSession()
{
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();
}

bool BcSession::waitReady(int timeoutMs, const std::atomic<bool> *abort)
{
    QElapsedTimer t;
    t.start();
    QMutexLocker lock(&m_mutex);
    while (m_state == State::Connecting && t.elapsed() < timeoutMs && !(abort && *abort))
        m_cond.wait(&m_mutex, 100);
    return m_state == State::Ready;
}

bool BcSession::isAlive() const
{
    QMutexLocker lock(&m_mutex);
    return m_state != State::Closed;
}

BcSession::Failure BcSession::failure() const
{
    QMutexLocker lock(&m_mutex);
    return m_failure;
}

QString BcSession::error() const
{
    QMutexLocker lock(&m_mutex);
    return m_error;
}

QString BcSession::linkMode() const
{
    QMutexLocker lock(&m_mutex);
    return m_link && m_state == State::Ready ? m_link->mode() : QString();
}

QString BcSession::linkLanHost() const
{
    QMutexLocker lock(&m_mutex);
    return m_link && m_state == State::Ready ? m_link->lanHost() : QString();
}

QString BcSession::linkDescription() const
{
    QMutexLocker lock(&m_mutex);
    return m_link && m_state == State::Ready ? m_link->description() : QString();
}

quint32 BcSession::newMessId(quint8 low)
{
    QMutexLocker lock(&m_mutex);
    for (;;) {
        const quint32 id = (m_nextId << 8) | low;
        m_nextId = m_nextId >= 0xFFFF ? 1 : m_nextId + 1;
        if (!m_routes.contains(id))
            return id;
    }
}

bool BcSession::send(quint32 cmd, quint8 channel, quint32 messId, const QByteArray &ext,
                     const QByteArray &body)
{
    QByteArray m = header(cmd, channel, messId, 0, kClassModern, int(ext.size() + body.size()));
    putLE32(m, static_cast<quint32>(ext.size()));
    m += ext;
    m += body;
    QMutexLocker lock(&m_writeMutex);
    return m_link && m_link->write(m);
}

void BcSession::route(quint32 messId, std::function<void(const BcMessage &)> fn)
{
    QMutexLocker lock(&m_mutex);
    m_routes.insert(messId, std::move(fn));
}

int BcSession::addPushHandler(std::function<void(const BcMessage &)> fn)
{
    QMutexLocker lock(&m_mutex);
    const int token = m_nextPushToken++;
    m_pushHandlers.insert(token, std::move(fn));
    return token;
}

void BcSession::removePushHandler(int token)
{
    QMutexLocker lock(&m_mutex);
    m_pushHandlers.remove(token);
    if (std::this_thread::get_id() != m_workerId)
        while (m_inPush)
            m_cond.wait(&m_mutex);
}

void BcSession::unroute(quint32 messId)
{
    QMutexLocker lock(&m_mutex);
    m_routes.remove(messId);
    if (std::this_thread::get_id() != m_workerId)
        while (m_inRoute == messId)
            m_cond.wait(&m_mutex);
}

bool BcSession::request(quint32 cmd, quint8 channel, const QByteArray &ext, const QByteArray &body,
                        BcMessage *reply, int timeoutMs, quint32 *messIdOut)
{
    Pending p;
    p.cmd = cmd;
    p.messId = newMessId();
    if (messIdOut)
        *messIdOut = p.messId;
    {
        QMutexLocker lock(&m_mutex);
        if (m_state != State::Ready)
            return false;
        m_pending.append(&p);
    }
    bool ok = send(cmd, channel, p.messId, ext, body);
    QElapsedTimer t;
    t.start();
    QMutexLocker lock(&m_mutex);
    while (ok && !p.done && m_state == State::Ready && t.elapsed() < timeoutMs)
        m_cond.wait(&m_mutex, 100);
    m_pending.removeOne(&p);
    ok = ok && p.done;
    if (ok && reply)
        *reply = p.reply;
    return ok;
}

bool BcSession::requestXml(quint32 cmd, quint8 channel, const QByteArray &extXml,
                           const QByteArray &bodyXml, QByteArray *replyXml, quint16 *status,
                           int timeoutMs, quint32 *messIdOut)
{
    const QByteArray key = aesKey();
    BcMessage reply;
    if (!request(cmd, channel, extXml.isEmpty() ? QByteArray() : bc::aesCfb(extXml, key, false),
                 bodyXml.isEmpty() ? QByteArray() : bc::aesCfb(bodyXml, key, false), &reply,
                 timeoutMs, messIdOut))
        return false;
    if (status)
        *status = reply.status;
    if (replyXml)
        *replyXml = this->bodyXml(reply);
    return true;
}

QByteArray BcSession::channelExtension(int channel)
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<Extension version=\"1.1\">\n<channelId>"
           + QByteArray::number(channel) + "</channelId>\n</Extension>\n";
}

QByteArray BcSession::bodyXml(const BcMessage &m) const
{
    const QByteArray body = m.payloadOffset > 0 ? m.body.mid(int(m.payloadOffset)) : m.body;
    return body.isEmpty() ? body : bc::aesCfb(body, aesKey(), true);
}

bool BcSession::lastPush(quint32 cmd, BcMessage *out) const
{
    QMutexLocker lock(&m_mutex);
    auto it = m_lastPush.constFind(cmd);
    if (it == m_lastPush.constEnd())
        return false;
    if (out)
        *out = *it;
    return true;
}

void BcSession::finish(Failure f, const QString &error)
{
    QMutexLocker lock(&m_mutex);
    m_state = State::Closed;
    if (m_failure == Failure::None)
        m_failure = f;
    if (m_error.isEmpty())
        m_error = error;
    m_cond.wakeAll();
}

bool BcSession::login()
{
    QByteArray buf;
    auto recv = [&](quint32 cmd, BcMessage &out) {
        for (int i = 0; i < 40 && !m_stop; ++i) {
            while (parse(buf, out))
                if (out.cmd == cmd)
                    return true;
            if (m_link->waitForReadyRead(300))
                buf += m_link->readAll();
        }
        return false;
    };
    // Legacy hello (class 0x6514, no payload offset) asks for the AES nonce.
    m_link->write(header(1, 0, 0, 0xDC12, kClassLegacy, 0));
    BcMessage nonceMsg;
    if (!recv(1, nonceMsg)) {
        finish(Failure::Unreachable, QStringLiteral("no reply to the login"));
        return false;
    }
    const QByteArray nonceXml = bc::xorCrypt(nonceMsg.body, nonceMsg.channel);
    const int a = nonceXml.indexOf("<nonce>");
    const int z = nonceXml.indexOf("</nonce>");
    if (a < 0 || z < 0) {
        finish(Failure::Unreachable, QStringLiteral("no nonce in the login reply"));
        return false;
    }
    const QString nonce = QString::fromUtf8(nonceXml.mid(a + 7, z - a - 7));
    const QByteArray key = bc::aesKey(nonce, m_p.password);
    const QByteArray login =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<LoginUser version=\"1.1\">\n<userName>"
        + bc::modernHash(m_p.username, nonce) + "</userName>\n<password>"
        + bc::modernHash(m_p.password, nonce)
        + "</password>\n<userVer>1</userVer>\n</LoginUser>\n"
          "<LoginNet version=\"1.1\"><type>LAN</type><udpPort>0</udpPort></LoginNet>\n</body>\n";
    QByteArray msg = header(1, 0, 0, 0, kClassModern, int(login.size()));
    putLE32(msg, 0);
    msg += bc::xorCrypt(login, 0);
    m_link->write(msg);
    BcMessage reply;
    if (!recv(1, reply)) {
        finish(Failure::Unreachable, QStringLiteral("no reply to the login"));
        return false;
    }
    if (reply.status != 200) {
        finish(Failure::Rejected, QStringLiteral("login rejected (status %1)").arg(reply.status));
        return false;
    }
    QByteArray info = bc::xorCrypt(reply.body, reply.channel);
    if (!info.contains("<?xml") && !info.contains("<body"))
        info = bc::aesCfb(reply.body, key, true);
    {
        QMutexLocker lock(&m_mutex);
        m_aesKey = key;
        m_loginInfo = info;
        m_state = State::Ready;
        m_cond.wakeAll();
    }
    // Whatever arrived past the login reply belongs to the session.
    BcMessage m;
    while (parse(buf, m))
        dispatch(m);
    m_pendingBytes = buf;
    return true;
}

void BcSession::dispatch(const BcMessage &m)
{
    std::function<void(const BcMessage &)> fn;
    {
        QMutexLocker lock(&m_mutex);
        fn = m_routes.value(m.messId);
        if (fn)
            m_inRoute = m.messId;
        if (!fn) {
            // A reply: to the request with this id, else the oldest one waiting
            // for this command.
            Pending *target = nullptr;
            for (Pending *p : std::as_const(m_pending))
                if (!p->done && p->messId == m.messId) {
                    target = p;
                    break;
                }
            if (!target)
                for (Pending *p : std::as_const(m_pending))
                    if (!p->done && p->cmd == m.cmd) {
                        target = p;
                        break;
                    }
            if (target) {
                target->reply = m;
                target->done = true;
                m_cond.wakeAll();
                return;
            }
            m_lastPush.insert(m.cmd, m);
            if (m_pushHandlers.isEmpty())
                return;
            const QList<std::function<void(const BcMessage &)>> handlers = m_pushHandlers.values();
            m_inPush = true;
            lock.unlock();
            for (const auto &h : handlers)
                h(m);
            lock.relock();
            m_inPush = false;
            m_cond.wakeAll();
            return;
        }
    }
    fn(m);
    QMutexLocker lock(&m_mutex);
    m_inRoute = 0;
    m_cond.wakeAll();
}

void BcSession::run()
{
    {
        QMutexLocker lock(&m_mutex);
        m_workerId = std::this_thread::get_id();
    }
    QString error;
    std::unique_ptr<BcLink> link = openBcLink(m_p.target, &m_stop, &error);
    if (!link) {
        finish(Failure::Unreachable, error);
        return;
    }
    {
        QMutexLocker lock(&m_writeMutex);
        m_link = std::move(link);
    }
    if (!login())
        return;
    qCInfo(lcProto) << "session ready on" << m_link->description();
    QByteArray buf = m_pendingBytes;
    QElapsedTimer quiet;     // since the device last sent anything
    QElapsedTimer heartbeat; // since our last heartbeat
    quiet.start();
    heartbeat.start();
    while (!m_stop && m_link->isConnected()) {
        if (!m_link->waitForReadyRead(100)) {
            if (quiet.elapsed() >= kSilenceMs) {
                qCInfo(lcProto) << "session" << m_link->description() << "silent for"
                                << kSilenceMs / 1000 << "s";
                break;
            }
            if (quiet.elapsed() >= kHeartbeatMs && heartbeat.elapsed() >= kHeartbeatMs) {
                send(93, 0, newMessId(), {}, {});
                heartbeat.restart();
            }
            continue;
        }
        quiet.restart();
        buf += m_link->readAll();
        BcMessage m;
        while (parse(buf, m))
            dispatch(m);
    }
    qCInfo(lcProto) << "session closed on" << m_link->description();
    finish(Failure::Unreachable, QStringLiteral("the connection to the device was lost"));
}

} // namespace rl
