#pragma once

#include <QAudioSink>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QVideoSink>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace rl {

// One live/playback stream: FFmpeg demux + decode on a dedicated worker thread,
// frames delivered to a QML VideoOutput's QVideoSink (NV12/YUV420P upload path —
// DESIGN.md §5: the universal fallback; zero-copy VAAPI arrives with the GPU spike).
//
// Lifetime: the worker shares a heap-allocated Session (below). stop()/destruction
// signal abort and detach — they NEVER join on the GUI thread, so a stalled network
// read or DNS lookup can never freeze the UI. The Session outlives the StreamPlayer
// until the worker drops its reference; a mutex-guarded back-pointer makes stale
// frame/state callbacks safe no-ops after the StreamPlayer is gone.
//
// Sources: rtsp:// (live, TCP, low-latency flags), or any libavformat-openable
// URL/file (used by tests and the "direct stream" device kind).
class StreamPlayer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    // GetEnc-declared display size of the stream being opened (optional). When the
    // decoded frame is this size transposed, the stream was transmitted rotated and
    // is corrected for display. Set alongside `source`; unset ⇒ heuristic fallback.
    Q_PROPERTY(QSize expectedSize READ expectedSize WRITE setExpectedSize NOTIFY expectedSizeChanged)
    Q_PROPERTY(QVideoSink *videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(qint64 framesDecoded READ framesDecoded NOTIFY framesDecodedChanged)
    Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY loopChanged)
    Q_PROPERTY(bool retryOnError READ retryOnError WRITE setRetryOnError NOTIFY retryOnErrorChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    // File/playback sources only (live sources are paced by the network): speed
    // factor applied to the pacing clock, and a pause that keeps the connection
    // open so stepFrame() can advance one frame at a time.
    Q_PROPERTY(double playbackRate READ playbackRate WRITE setPlaybackRate NOTIFY playbackRateChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    // Audio is decoded only while enabled (one pane at a time is the intent),
    // and is silent while paused or at any speed other than 1x.
    Q_PROPERTY(bool audioEnabled READ audioEnabled WRITE setAudioEnabled NOTIFY audioEnabledChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    // True once the current stream turned out to carry decodable audio.
    Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY hasAudioChanged)

public:
    enum class State { Idle, Connecting, Streaming, Error, Stopped };
    Q_ENUM(State)

    // Shared between the StreamPlayer and its detached worker thread.
    struct Session {
        std::atomic<bool> abort{false};
        std::atomic<bool> loop{false};
        std::atomic<qint64> framesDecoded{0};
        std::atomic<double> rate{1.0};
        std::atomic<bool> paused{false};
        std::atomic<int> steps{0}; // frames to release while paused
        std::atomic<bool> audioEnabled{false};
        QString source;
        QSize expectedSize; // declared size for rotation detection (see property)

        // Custom packet source (native Baichuan): when set, the demuxer reads the
        // raw elementary stream via this blocking callback instead of opening a URL.
        // forcedFormat is the libavformat demuxer name ("h264"/"hevc").
        std::function<int(unsigned char *, int)> readPacket;
        QString forcedFormat;

        // Retry on connection error even for non-live sources (playback FLV on a
        // connection-limited NVR often needs a couple of attempts).
        std::atomic<bool> retryOnError{false};

        // Recording taps this same demux session (DESIGN §5.5): no second stream.
        std::atomic<bool> recordRequested{false};
        std::atomic<bool> recording{false};
        QMutex recMutex;
        QString recPath;

        // Frame/state callbacks marshal to the GUI thread only while the player
        // is alive. backMutex guards `player`; the StreamPlayer destructor nulls
        // it under the lock before detaching, closing the lifetime race.
        QMutex backMutex;
        QPointer<StreamPlayer> player;

        // The sink is owned by QML; delivery hops to the GUI thread where it lives.
        QMutex sinkMutex;
        QPointer<QVideoSink> sink;
    };

    explicit StreamPlayer(QObject *parent = nullptr);
    ~StreamPlayer() override;

    QString source() const { return m_source; }
    void setSource(const QString &source);

    QSize expectedSize() const { return m_expectedSize; }
    void setExpectedSize(const QSize &size);

    // Drive playback from a blocking packet callback (native Baichuan) instead of a
    // URL. `reader` fills a buffer with elementary-stream bytes (returns count, 0/EOF
    // to end); `format` is the demuxer ("h264"/"hevc"); `onStop` is invoked on
    // stop()/destruction to unblock and tear down the source. Set before start();
    // cleared by setSource(). Not a QML API.
    void setPacketSource(std::function<int(unsigned char *, int)> reader, const QString &format,
                         std::function<void()> onStop);

    QVideoSink *videoSink() const;
    void setVideoSink(QVideoSink *sink);

    State state() const { return m_state; }
    QString errorString() const { return m_errorString; }
    qint64 framesDecoded() const;

    bool loop() const { return m_loop; }
    void setLoop(bool loop);

    bool retryOnError() const { return m_retryOnError; }
    void setRetryOnError(bool v);

    double playbackRate() const { return m_rate; }
    void setPlaybackRate(double rate);
    bool paused() const { return m_paused; }
    void setPaused(bool paused);
    // While paused, decode and show exactly one more frame.
    Q_INVOKABLE void stepFrame();

    bool audioEnabled() const { return m_audioEnabled; }
    void setAudioEnabled(bool enabled);
    double volume() const { return m_volume; }
    void setVolume(double volume);
    bool hasAudio() const { return m_hasAudio; }
    // GUI thread, from the worker: 48 kHz stereo S16 PCM to play, or the
    // stream's audio availability. Not part of the QML API.
    void pushAudio(const QByteArray &pcm);
    void setHasAudioFromWorker(bool has);

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();

    bool recording() const;
    // Begin/stop stream-copy recording of the live session to an MP4. An empty
    // path auto-names a file in the recordings dir. Returns false if not streaming.
    Q_INVOKABLE bool startRecording(const QString &path = QString());
    Q_INVOKABLE void stopRecording();

    // Called on the GUI thread by the worker (via QMetaObject::invokeMethod).
    // Public so the worker helpers can reach it; not part of the QML API.
    void applyStateFromWorker(State state, const QString &error);
    void applyRecordingState(bool recording, const QString &path, const QString &error);

signals:
    void sourceChanged();
    void expectedSizeChanged();
    void videoSinkChanged();
    void stateChanged();
    void errorStringChanged();
    void framesDecodedChanged();
    void loopChanged();
    void retryOnErrorChanged();
    void playbackRateChanged();
    void pausedChanged();
    void audioEnabledChanged();
    void volumeChanged();
    void hasAudioChanged();
    void recordingChanged();
    void recordingSaved(const QString &path);
    void recordingFailed(const QString &error);

private:
    void applyState(State state, const QString &error);

    QString m_source;
    QSize m_expectedSize;
    std::function<int(unsigned char *, int)> m_pendingReader;
    QString m_pendingFormat;
    std::function<void()> m_pendingStop; // teardown for the not-yet-started source
    std::function<void()> m_activeStop;  // teardown for the running session's source
    bool m_loop = false;
    double m_rate = 1.0;
    bool m_paused = false;
    bool m_audioEnabled = false;
    double m_volume = 1.0;
    bool m_hasAudio = false;
    QAudioSink *m_audioSink = nullptr;
    QIODevice *m_audioIo = nullptr;
    void closeAudio();

    State m_state = State::Idle;
    QString m_errorString;
    bool m_recording = false;
    bool m_retryOnError = false;

    // Set via QML before start(); copied into each new Session. Survives stop().
    QPointer<QVideoSink> m_pendingSink;

    // The current session; replaced on each start(). Held so we can signal abort
    // and read framesDecoded. The worker holds its own shared_ptr copy.
    std::shared_ptr<Session> m_session;
};

} // namespace rl
