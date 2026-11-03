#pragma once

#include <memory>

#include "protocol/BaichuanControl.h"

#include <QAudioFormat>
#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QWaitCondition>

#include <atomic>
#include <thread>

class QAudioSource;
class QIODevice;

namespace rl {

class BcSession;

// Two-way talk to one camera: captures the default microphone, cuts it into
// the camera's frames (TalkAbility: typically 1024 samples of 16 kHz mono),
// IMA-ADPCM encodes them and streams them over a dedicated Baichuan session
// (cmd 10 / 201 / 202 / 11 — see BaichuanControl).
//
// Threading: the QAudioSource lives on the GUI thread (it needs an event loop);
// a worker thread owns the Baichuan socket, so the UI never blocks on the
// network. Blocks cross through a small queue; if the camera falls behind, the
// oldest blocks are dropped to keep the voice live.
class TalkSession : public QObject
{
    Q_OBJECT
public:
    // Talks inside the device's Baichuan session, as the official client.
    TalkSession(std::shared_ptr<BcSession> session, int channel, QObject *parent = nullptr);
    ~TalkSession() override;

    void start(); // connect + negotiate on the worker, then open the microphone
    void stop();  // non-blocking; finished() follows once cmd 11 went out

signals:
    void started();
    void failed(const QString &error);
    void finished();

private:
    void runWorker();
    void talk();
    void openMicrophone(int sampleRate);
    void onMicData();
    void pushPcm16k(const qint16 *samples, int count);

    std::shared_ptr<BcSession> m_session;
    int m_channel = 0;
    std::thread m_worker;
    std::atomic<bool> m_abort{false};

    QMutex m_mutex;
    QWaitCondition m_cond;
    QQueue<QByteArray> m_blocks; // PCM blocks awaiting encode + send
    int m_blockSamples = 1024;
    int m_sampleRate = 16000;

    QAudioSource *m_source = nullptr;
    QIODevice *m_io = nullptr;
    QAudioFormat m_micFormat;
    QByteArray m_pending;        // mono PCM at m_sampleRate, not yet a full block
    double m_resamplePos = 0;    // fractional read position for the resampler
    qint64 m_toneT = 0;          // RL_TALK_TONE diagnostic phase
    float m_dcPrevIn = 0;        // DC-blocking high-pass state
    float m_dcPrevOut = 0;
};

} // namespace rl
