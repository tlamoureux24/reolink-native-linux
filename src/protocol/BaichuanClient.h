#pragma once

#include "media/BcMediaParser.h"

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QVector>
#include <QWaitCondition>

#include <atomic>
#include <functional>
#include <thread>

// One live or recorded stream of a Reolink device, inside the device's
// Baichuan session (BcSession), the way the official client streams: live
// with cmd 3 / cmd 4 (BaichuanPreviewer), recordings file by file with cmd 5,
// cmd 123 (seek) and cmd 7 (stop) (BaichuanReplayer).
//
// Runs on its own thread and exposes a blocking read() that yields a raw
// H.264/H.265 Annex-B elementary stream, designed to back a libavformat
// AVIOContext so StreamPlayer's decode path is reused as-is.
//
// Clean-room: wire format from the official SDK's behaviour and MIT
// reolink_aio; no AGPL neolink code.
namespace rl {

// A recording file, as the device's file search (cmd 14/15) lists it.
struct BcReplayFile {
    qint64 start = 0; // epoch
    qint64 end = 0;
    QString name;
    QString id; // the file's path on the device
};

class BaichuanClient
{
public:
    struct Params {
        QString host;
        int port = 9000;
        QString username;
        QString password;
        int channel = 0;
        QString uid;                    // camera UID (recordings are searched by it)
        QString linkUid;                // the device's UID: reached by it when away
        bool mainStream = true;
        qint64 startEpoch = 0;          // 0 => live preview; else recorded playback from here
        // Recorded playback: the day's files of this stream, played one by one.
        QVector<BcReplayFile> replayFiles;
        // The stream's audio frames, called on the worker thread (optional).
        std::function<void(const BcMediaParser::AudioFrame &)> onAudio;
    };

    explicit BaichuanClient(Params params);
    ~BaichuanClient();

    // Spawn the worker (connect + login + start the stream). Non-blocking.
    void start();
    // Signal the worker to stop and unblock any pending read(). Never joins on the
    // caller thread.
    void stop();

    // Seek an active recorded playback to a new instant, in place (no new
    // stream). Returns false if it has already ended (start a fresh one).
    bool seek(qint64 startEpoch);
    // Recorded playback speed (0.125–16): the device sends a file faster than
    // real time, and this client paces it.
    void setRate(double rate);

    // Blocking read of decoded Annex-B bytes (for an AVIOContext read callback).
    // Returns the number of bytes written, 0 on clean end, or <0 on abort/error.
    int read(unsigned char *buf, int size);

    // "h264" / "hevc", read off the first key frame itself (its header label
    // can be wrong); empty until then.
    QString ffmpegFormat();

private:
    void run();                                    // worker entry
    // Live view inside the device's session (cmd 3 / cmd 4), shared with every
    // other client watching the same channel and stream.
    void runSessionLive();
    // Recorded playback inside the device's session, file by file.
    void runSessionReplay();
    // A frame of that shared stream (session worker).
    void deliverShared(const BcMediaParser::VideoFrame &f);
    void deliverSharedAudio(const BcMediaParser::AudioFrame &a);
    // `block`: wait for the reader when far behind (playback, its own thread);
    // a shared session's worker must not wait, so it drops instead.
    void pushAnnexB(const QByteArray &bytes, bool block = true);

    Params m_p;
    std::thread m_thread;
    std::atomic<bool> m_abort{false};

    QMutex m_mutex;
    QWaitCondition m_cond;
    QByteArray m_ring;        // decoded Annex-B awaiting the reader
    bool m_finished = false;  // worker done producing
    qint64 m_seekEpoch = 0;   // pending in-place seek target (0 = none)
    QString m_format;         // "h264"/"hevc", guarded by m_mutex
    bool m_started = false;
    bool m_sharedKeyFrame = false; // a key frame reached us on the shared stream
    std::atomic<double> m_rate{1.0}; // recorded playback speed
};

} // namespace rl
