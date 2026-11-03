#include "TalkSession.h"

#include "core/AppSettings.h"
#include "core/Log.h"
#include "media/ImaAdpcm.h"

#include <QAudioDevice>
#include <QAudioSource>
#include <QMediaDevices>
#include <QMetaObject>

#include <cmath>

namespace rl {

TalkSession::TalkSession(std::shared_ptr<BcSession> session, int channel, QObject *parent)
    : QObject(parent), m_session(std::move(session)), m_channel(channel)
{
}

TalkSession::~TalkSession()
{
    stop();
    if (m_worker.joinable())
        m_worker.join(); // owners delete us on finished(), so this is immediate
}

void TalkSession::start()
{
    if (m_worker.joinable())
        return;
    m_abort = false;
    m_worker = std::thread([this] { runWorker(); });
}

void TalkSession::stop()
{
    if (m_source) {
        m_source->stop();
        m_source->deleteLater();
        m_source = nullptr;
        m_io = nullptr;
    }
    // Non-blocking: a camera that stopped answering must not freeze the UI.
    // The worker sends cmd 11, closes, then emits finished().
    qCDebug(lcMedia) << "talk stop requested";
    m_abort = true;
    m_cond.wakeAll();
}

void TalkSession::runWorker()
{
    talk();
    QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
}

void TalkSession::talk()
{
    BaichuanControl bc(m_session);
    auto fail = [this](const QString &msg) {
        QMetaObject::invokeMethod(this, [this, msg] { emit failed(msg); }, Qt::QueuedConnection);
    };
    if (!bc.open()) {
        fail(tr("couldn't reach the device"));
        return;
    }
    bool ok = false;
    const BaichuanControl::TalkParams p = BaichuanControl::parseTalkAbility(bc.get(10, m_channel), &ok);
    if (!ok || p.audioType != QLatin1String("adpcm") || p.samplePrecision != 16
        || p.soundTrack != QLatin1String("mono")) {
        fail(tr("this camera doesn't offer two-way talk in a supported format"));
        return;
    }
    quint16 status = 0;
    if (!bc.talkStart(m_channel, p, &status)) {
        // 422-style refusals mean another client is already talking.
        fail(tr("the camera refused to start talking (status %1)").arg(status));
        return;
    }
    qCInfo(lcMedia) << "talk started:" << p.sampleRate << "Hz," << p.lengthPerEncoder << "samples/frame";
    QMetaObject::invokeMethod(this, [this, p] {
        if (m_abort)
            return; // stopped while negotiating
        m_blockSamples = p.lengthPerEncoder;
        openMicrophone(p.sampleRate);
        emit started();
    }, Qt::QueuedConnection);

    ImaAdpcm codec;
    bool sendOk = true;
    int sent = 0;
    while (!m_abort && sendOk) {
        QByteArray block;
        {
            QMutexLocker lock(&m_mutex);
            while (m_blocks.isEmpty() && !m_abort)
                m_cond.wait(&m_mutex, 200);
            if (m_abort)
                break;
            block = m_blocks.dequeue();
        }
        const qint16 predictor = codec.predictor();
        const quint8 index = codec.index();
        const QByteArray adpcm = codec.encode(reinterpret_cast<const qint16 *>(block.constData()),
                                              static_cast<int>(block.size() / 2));
        sendOk = bc.talkSend(m_channel, BaichuanControl::talkFrame(adpcm, predictor, index));
        ++sent;
    }
    qCInfo(lcMedia) << "talk ended:" << sent << "frames sent";
    bc.talkStop(m_channel);
    bc.close();
    if (!sendOk)
        fail(tr("the connection to the camera was lost"));
}

void TalkSession::openMicrophone(int sampleRate)
{
    m_sampleRate = sampleRate;
    const QAudioDevice dev = QMediaDevices::defaultAudioInput();
    if (dev.isNull()) {
        emit failed(tr("no microphone found"));
        return;
    }
    // Ask for the camera's own format; when the device can't, take its
    // preferred one and convert (mono mix + linear resample) ourselves.
    QAudioFormat want;
    want.setSampleRate(sampleRate);
    want.setChannelCount(1);
    want.setSampleFormat(QAudioFormat::Int16);
    m_micFormat = dev.isFormatSupported(want) ? want : dev.preferredFormat();
    m_source = new QAudioSource(dev, m_micFormat, this);
    m_source->setBufferSize(m_micFormat.bytesForDuration(100000)); // 100 ms
    m_io = m_source->start();
    if (!m_io) {
        emit failed(tr("the microphone couldn't be opened"));
        return;
    }
    connect(m_io, &QIODevice::readyRead, this, &TalkSession::onMicData);
    qCInfo(lcMedia) << "talk microphone:" << dev.description() << m_micFormat.sampleRate() << "Hz"
                    << m_micFormat.channelCount() << "ch, state" << m_source->state();
}

void TalkSession::onMicData()
{
    if (!m_io)
        return;
    const QByteArray raw = m_io->readAll();
    const int channels = qMax(1, m_micFormat.channelCount());
    const int frames = static_cast<int>(raw.size() / m_micFormat.bytesPerFrame());
    if (frames <= 0)
        return;

    // Mono float samples from whatever the device delivers.
    QVector<float> mono(frames);
    for (int f = 0; f < frames; ++f) {
        float acc = 0;
        for (int c = 0; c < channels; ++c) {
            const char *p = raw.constData() + f * m_micFormat.bytesPerFrame() + c * m_micFormat.bytesPerSample();
            switch (m_micFormat.sampleFormat()) {
            case QAudioFormat::Int16: acc += *reinterpret_cast<const qint16 *>(p) / 32768.f; break;
            case QAudioFormat::Int32: acc += *reinterpret_cast<const qint32 *>(p) / 2147483648.f; break;
            case QAudioFormat::Float: acc += *reinterpret_cast<const float *>(p); break;
            case QAudioFormat::UInt8: acc += (*reinterpret_cast<const quint8 *>(p) - 128) / 128.f; break;
            default: break;
            }
        }
        mono[f] = acc / channels;
    }

    // Clean-up before encoding: remove the DC offset some microphones carry
    // (a one-pole high-pass), apply the user's gain, then soft-clip so a
    // saturating microphone sounds loud rather than harsh and broken.
    const float gain = AppSettings::talkGainPercent() / 100.f;
    for (float &v : mono) {
        const float hp = v - m_dcPrevIn + 0.995f * m_dcPrevOut;
        m_dcPrevIn = v;
        m_dcPrevOut = hp;
        v = std::tanh(hp * gain);
    }

    // Resample to the camera rate (linear; voice only, 48k->16k etc.).
    const double ratio = double(m_micFormat.sampleRate()) / m_sampleRate;
    QVector<qint16> out;
    out.reserve(int(frames / ratio) + 2);
    if (std::abs(ratio - 1.0) < 1e-9) {
        for (float v : mono)
            out.append(static_cast<qint16>(qBound(-32768.f, v * 32767.f, 32767.f)));
    } else {
        for (; m_resamplePos < frames - 1; m_resamplePos += ratio) {
            const int i = static_cast<int>(m_resamplePos);
            const float t = static_cast<float>(m_resamplePos - i);
            const float v = mono[i] * (1 - t) + mono[i + 1] * t;
            out.append(static_cast<qint16>(qBound(-32768.f, v * 32767.f, 32767.f)));
        }
        m_resamplePos -= frames;
    }
    // Diagnostic: RL_TALK_TONE=1 replaces the microphone with a 440 Hz tone
    // (same timing), to tell a bad microphone from a talk-path problem.
    static const bool tone = qEnvironmentVariableIsSet("RL_TALK_TONE");
    if (tone)
        for (qint16 &v : out)
            v = static_cast<qint16>(12000 * std::sin(2 * M_PI * 440 * (m_toneT++) / double(m_sampleRate)));
    pushPcm16k(out.constData(), static_cast<int>(out.size()));
}

void TalkSession::pushPcm16k(const qint16 *samples, int count)
{
    m_pending.append(reinterpret_cast<const char *>(samples), count * 2);
    const int blockBytes = m_blockSamples * 2;
    QMutexLocker lock(&m_mutex);
    while (m_pending.size() >= blockBytes) {
        m_blocks.enqueue(m_pending.left(blockBytes));
        m_pending.remove(0, blockBytes);
        while (m_blocks.size() > 8) // ~0.5 s behind: drop the oldest, stay live
            m_blocks.dequeue();
    }
    m_cond.wakeAll();
}

} // namespace rl
