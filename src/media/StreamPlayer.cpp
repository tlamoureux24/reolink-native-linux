#include "StreamPlayer.h"

#include "core/AppSettings.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "media/ImaAdpcm.h"

#include <QAudioFormat>
#include <QDateTime>
#include <QMediaDevices>
#include <QDeadlineTimer>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace rl {

namespace {

constexpr int kOpenTimeoutMs = 10000;
constexpr int kReadStallTimeoutMs = 10000; // §5.8 watchdog: no packet in 10s → reconnect
constexpr int kBackoffStartMs = 1000;
constexpr int kBackoffCapMs = 30000;

using Session = StreamPlayer::Session;
using State = StreamPlayer::State;

bool isLiveUrl(const QString &source)
{
    return source.startsWith(QLatin1String("rtsp://")) ||
           source.startsWith(QLatin1String("rtmp://")) ||
           source.startsWith(QLatin1String("tcp://")) ||
           source.startsWith(QLatin1String("udp://"));
}

// Never log credentials — they appear both as userinfo (rtsp://user:pass@host)
// and as query params (the http-flv playback URL: ...&user=X&password=Y).
QString redacted(const QString &source)
{
    QUrl u(source);
    if (!u.isValid())
        return source;
    bool changed = !u.userInfo().isEmpty();
    QUrlQuery q(u);
    for (const QString &key : {QStringLiteral("user"), QStringLiteral("password"),
                               QStringLiteral("token")}) {
        if (q.hasQueryItem(key)) {
            q.removeAllQueryItems(key);
            changed = true;
        }
    }
    if (!changed)
        return source;
    u.setQuery(q);
    return u.toDisplayString(QUrl::RemoveUserInfo);
}

// Determine how the sink should rotate a stream's frames for upright display.
//
// Priority order:
//  1. A display-matrix in the stream side-data (standard rotation signalling;
//     Reolink sends none, but other sources may).
//  2. Declared-vs-decoded swap test [primary]. Some Reolink cameras (Duo 3-class)
//     transmit the main stream transposed — the true image is landscape (GetEnc
//     declares e.g. 7680x2160) but it decodes as 2160x7680 portrait, so the coded
//     width stays within the HEVC level cap. No orientation metadata rides along
//     on RTSP/FLV, so we reconcile the two: if the decoded frame is exactly the
//     declared size transposed, the stream is rotated and we correct it. This is
//     self-calibrating — it leaves already-landscape sub streams and genuine
//     corridor-mode portrait feeds (declared == decoded) untouched, unlike a
//     resolution threshold. `expected` is the GetEnc size for THIS stream.
//  3. Extreme-portrait heuristic [fallback], for raw-URL playback with no NVR
//     session to supply a declared size.
//
// Direction is Clockwise270 (verified on an RLN8-410). Nothing on the wire
// encodes winding direction, so this is a per-model constant, not inferred.
QtVideo::Rotation streamRotation(const AVStream *stream, QSize expected)
{
    // The DISPLAYMATRIX side data moved to codecpar->coded_side_data in FFmpeg 6.1
    // (libavcodec 60.15); older builds (e.g. the Flatpak runtime's FFmpeg) expose
    // it via the now-deprecated av_stream_get_side_data. Support both.
    const int32_t *dm = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 15, 100)
    const AVPacketSideData *sd =
        stream->codecpar->coded_side_data
            ? av_packet_side_data_get(stream->codecpar->coded_side_data,
                                      stream->codecpar->nb_coded_side_data,
                                      AV_PKT_DATA_DISPLAYMATRIX)
            : nullptr;
    if (sd && sd->size >= 9 * static_cast<int>(sizeof(int32_t)))
        dm = reinterpret_cast<const int32_t *>(sd->data);
#else
    size_t dmSize = 0;
    const uint8_t *raw = av_stream_get_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, &dmSize);
    if (raw && dmSize >= 9 * sizeof(int32_t))
        dm = reinterpret_cast<const int32_t *>(raw);
#endif
    if (dm) {
        // av_display_rotation_get returns the CCW angle; the clockwise display
        // rotation is its negation, normalized to [0,360).
        int cw = static_cast<int>(std::llround(-av_display_rotation_get(dm)));
        cw = ((cw % 360) + 360) % 360;
        switch (cw) {
        case 90:
            return QtVideo::Rotation::Clockwise90;
        case 180:
            return QtVideo::Rotation::Clockwise180;
        case 270:
            return QtVideo::Rotation::Clockwise270;
        default:
            return QtVideo::Rotation::None;
        }
    }

    const int w = stream->codecpar->width;
    const int h = stream->codecpar->height;

    // When the NVR told us the declared size, rotate iff the decoded frame is
    // its exact transpose AND the decoded frame is portrait. Cameras transmit
    // transposed as portrait (a landscape sensor turned sideways — Duo 3), so
    // an upright LANDSCAPE stream whose declared size merely looks transposed
    // is metadata being wrong, not a rotated picture: hub-attached doorbells
    // report exactly that, and rotating them broke their view (issue #3).
    if (expected.isValid()) {
        if (w > 0 && h > w && expected.width() == h && expected.height() == w)
            return QtVideo::Rotation::Clockwise270;
        return QtVideo::Rotation::None;
    }

    // No declared size: fall back to the extreme-portrait signature. The 2x
    // threshold sits above a deliberate 9:16 portrait mount (1.78x).
    if (w > 0 && h > w * 2)
        return QtVideo::Rotation::Clockwise270;
    return QtVideo::Rotation::None;
}

QVideoFrameFormat::PixelFormat mapPixelFormat(int avFormat)
{
    switch (avFormat) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        return QVideoFrameFormat::Format_YUV420P;
    case AV_PIX_FMT_NV12:
        return QVideoFrameFormat::Format_NV12;
    default:
        return QVideoFrameFormat::Format_Invalid;
    }
}

// Post a state change to the StreamPlayer on the GUI thread. backMutex guarantees
// the player is alive across the invokeMethod call; invokeMethod(context, fn)
// then cancels itself if the player is destroyed before the event is delivered.
void postState(const std::shared_ptr<Session> &s, State st, const QString &err)
{
    QMutexLocker lock(&s->backMutex);
    if (!s->player)
        return;
    StreamPlayer *p = s->player;
    QMetaObject::invokeMethod(
        p, [p, st, err] { p->applyStateFromWorker(st, err); }, Qt::QueuedConnection);
}

void postAudio(const std::shared_ptr<Session> &s, const QByteArray &pcm)
{
    QMutexLocker lock(&s->backMutex);
    if (!s->player)
        return;
    StreamPlayer *p = s->player;
    QMetaObject::invokeMethod(p, [p, pcm] { p->pushAudio(pcm); }, Qt::QueuedConnection);
}

void postHasAudio(const std::shared_ptr<Session> &s, bool has)
{
    QMutexLocker lock(&s->backMutex);
    if (!s->player)
        return;
    StreamPlayer *p = s->player;
    QMetaObject::invokeMethod(p, [p, has] { p->setHasAudioFromWorker(has); },
                              Qt::QueuedConnection);
}

void postFramesTick(const std::shared_ptr<Session> &s)
{
    QMutexLocker lock(&s->backMutex);
    if (!s->player)
        return;
    StreamPlayer *p = s->player;
    QMetaObject::invokeMethod(p, [p] { emit p->framesDecodedChanged(); }, Qt::QueuedConnection);
}

// Deliver a decoded frame to the QML-owned sink, on the GUI thread where the sink
// lives and is destroyed (finding: cross-thread sink teardown race).
void deliverFrame(const std::shared_ptr<Session> &s, const QVideoFrame &frame)
{
    QMutexLocker lock(&s->backMutex);
    if (!s->player)
        return;
    StreamPlayer *p = s->player;
    QMetaObject::invokeMethod(
        p,
        [s, frame] {
            QMutexLocker sinkLock(&s->sinkMutex);
            if (s->sink)
                s->sink->setVideoFrame(frame);
        },
        Qt::QueuedConnection);
}

void postRecording(const std::shared_ptr<Session> &s, bool recording, const QString &path,
                   const QString &error)
{
    QMutexLocker lock(&s->backMutex);
    if (!s->player)
        return;
    StreamPlayer *p = s->player;
    QMetaObject::invokeMethod(
        p, [p, recording, path, error] { p->applyRecordingState(recording, path, error); },
        Qt::QueuedConnection);
}

// Stream-copy recorder: taps the live demux, muxing video packets into an MP4
// without re-encoding (DESIGN §5.5). Starts on the next keyframe and rebases
// timestamps so the file begins at t=0.
struct Recorder {
    AVFormatContext *oc = nullptr;
    int outIndex = -1;
    int inIndex = -1;
    AVRational inTb{};
    bool waitingKey = true;
    qint64 startTs = AV_NOPTS_VALUE;
    QString path;

    bool active() const { return oc != nullptr; }

    bool open(const QString &outPath, const AVStream *in)
    {
        path = outPath;
        inIndex = in->index;
        inTb = in->time_base;
        const QByteArray p = outPath.toUtf8();
        if (avformat_alloc_output_context2(&oc, nullptr, "mp4", p.constData()) < 0 || !oc)
            return false;
        AVStream *out = avformat_new_stream(oc, nullptr);
        if (!out || avcodec_parameters_copy(out->codecpar, in->codecpar) < 0)
            return false;
        out->codecpar->codec_tag = 0; // let the muxer choose (avc1/hvc1)
        outIndex = out->index;
        if (!(oc->oformat->flags & AVFMT_NOFILE) &&
            avio_open(&oc->pb, p.constData(), AVIO_FLAG_WRITE) < 0)
            return false;
        AVDictionary *opts = nullptr;
        av_dict_set(&opts, "movflags", "+faststart", 0);
        const int rc = avformat_write_header(oc, &opts);
        av_dict_free(&opts);
        if (rc < 0)
            return false;
        waitingKey = true;
        startTs = AV_NOPTS_VALUE;
        return true;
    }

    void write(const AVPacket *src)
    {
        if (src->stream_index != inIndex)
            return;
        if (waitingKey) {
            if (!(src->flags & AV_PKT_FLAG_KEY))
                return; // start on a keyframe so the file is decodable
            waitingKey = false;
            startTs = src->pts != AV_NOPTS_VALUE ? src->pts : src->dts;
        }
        AVPacket *pkt = av_packet_clone(src); // original continues to the decoder
        if (!pkt)
            return;
        pkt->stream_index = outIndex;
        const AVRational outTb = oc->streams[outIndex]->time_base;
        const qint64 off = startTs == AV_NOPTS_VALUE ? 0 : startTs;
        const auto rnd = static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
        if (pkt->pts != AV_NOPTS_VALUE)
            pkt->pts = av_rescale_q_rnd(pkt->pts - off, inTb, outTb, rnd);
        if (pkt->dts != AV_NOPTS_VALUE)
            pkt->dts = av_rescale_q_rnd(pkt->dts - off, inTb, outTb, rnd);
        pkt->duration = av_rescale_q(pkt->duration, inTb, outTb);
        pkt->pos = -1;
        av_interleaved_write_frame(oc, pkt); // takes ownership of pkt's contents
        av_packet_free(&pkt);
    }

    // Writes the trailer and closes; returns the path (empty on nothing recorded).
    QString finalize()
    {
        if (!oc)
            return {};
        QString result;
        if (!waitingKey) { // at least one frame was written
            av_write_trailer(oc);
            result = path;
        }
        if (!(oc->oformat->flags & AVFMT_NOFILE) && oc->pb)
            avio_closep(&oc->pb);
        avformat_free_context(oc);
        oc = nullptr;
        return result;
    }
};

struct InterruptContext {
    Session *session = nullptr;
    QDeadlineTimer deadline;

    static int callback(void *opaque)
    {
        auto *ctx = static_cast<InterruptContext *>(opaque);
        if (ctx->session->abort.load(std::memory_order_relaxed))
            return 1;
        return ctx->deadline.hasExpired() ? 1 : 0;
    }
};

void abortableSleepUs(Session *s, qint64 waitUs)
{
    for (qint64 remaining = waitUs;
         remaining > 0 && !s->abort.load(std::memory_order_relaxed); remaining -= 10000)
        av_usleep(static_cast<unsigned>(qMin<qint64>(remaining, 10000)));
}

// Convert one AVFrame to a QVideoFrame and hand it to the sink. Returns false on
// an unrecoverable mapping failure (frame dropped).
bool emitFrame(const std::shared_ptr<Session> &s, AVFrame *out,
               QVideoFrameFormat::PixelFormat outFormat, QtVideo::Rotation rotation)
{
    QVideoFrameFormat format(QSize(out->width, out->height), outFormat);
    // Full-range (JPEG) YUV must be tagged or the sink renders it as limited range.
    if (out->color_range == AVCOL_RANGE_JPEG || out->format == AV_PIX_FMT_YUVJ420P)
        format.setColorRange(QVideoFrameFormat::ColorRange_Full);

    QVideoFrame videoFrame(format);
    // Rotation is honored at the frame level by the sink/renderer (the format's
    // rotation is not); set it here so cameras that transmit the main stream
    // rotated (portrait) are presented upright.
    if (rotation != QtVideo::Rotation::None)
        videoFrame.setRotation(rotation);
    if (!videoFrame.map(QVideoFrame::WriteOnly))
        return false;
    for (int plane = 0; plane < videoFrame.planeCount(); ++plane) {
        const int planeHeight = plane == 0 ? out->height : (out->height + 1) / 2;
        // linesize may be negative for bottom-up frames; qAbs stops the memcpy
        // size from wrapping to ~2^64.
        const int rowBytes = qMin(videoFrame.bytesPerLine(plane), qAbs(out->linesize[plane]));
        for (int y = 0; y < planeHeight; ++y)
            memcpy(videoFrame.bits(plane) + y * videoFrame.bytesPerLine(plane),
                   out->data[plane] + y * out->linesize[plane], static_cast<size_t>(rowBytes));
    }
    videoFrame.unmap();
    deliverFrame(s, videoFrame);
    return true;
}

// ---- Hardware decode ------------------------------------------------------
// Offload H.264/H.265 decode to the GPU (VAAPI/CUDA/VDPAU), then transfer the
// surface back to system memory for the sink-upload path. This is not yet the
// zero-copy path (DMA-BUF→GL lands with the GPU render spike, DESIGN §5.2) but
// it already moves decode off the CPU, which is what lets a 16-tile grid scale.
// Any failure falls back to software decode, per-stream and silently.
bool hwDecodeEnabled()
{
    return AppSettings::hardwareDecodingEnabled(); // default on; RL_HWDECODE=0 forces off
}

AVPixelFormat getHwFormat(AVCodecContext *ctx, const AVPixelFormat *fmts)
{
    const auto want = static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(ctx->opaque));
    for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == want)
            return *p;
    // Wanted GPU surface not offered (or hwaccel init failed and ffmpeg is
    // re-asking): pick the first SOFTWARE format, not fmts[0] — the list leads
    // with hw formats, and returning a refused one loops the failure (e.g. VAAPI
    // rejecting 2160x7680, which exceeds its 4352 max height).
    for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL))
            return *p;
    }
    return fmts[0];
}

// Configures dec for hardware decode; returns the hw pixel format (or NONE).
AVPixelFormat setupHwDecode(AVCodecContext *dec, const AVCodec *codec, AVBufferRef **hwCtx)
{
    static const AVHWDeviceType order[] = {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA,
                                           AV_HWDEVICE_TYPE_VDPAU};
    for (AVHWDeviceType type : order) {
        AVPixelFormat pixfmt = AV_PIX_FMT_NONE;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
            if (!cfg)
                break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                cfg->device_type == type) {
                pixfmt = cfg->pix_fmt;
                break;
            }
        }
        if (pixfmt == AV_PIX_FMT_NONE)
            continue;
        AVBufferRef *ctx = nullptr;
        if (av_hwdevice_ctx_create(&ctx, type, nullptr, nullptr, 0) < 0)
            continue;
        *hwCtx = ctx;
        dec->hw_device_ctx = av_buffer_ref(ctx);
        dec->opaque = reinterpret_cast<void *>(static_cast<intptr_t>(pixfmt));
        dec->get_format = getHwFormat;
        qCInfo(lcMedia) << "hw decode via" << av_hwdevice_get_type_name(type);
        return pixfmt;
    }
    return AV_PIX_FMT_NONE;
}

// AVIOContext read callback for a custom packet source (native Baichuan): pull
// elementary-stream bytes from the session's blocking reader.
int avioReadThunk(void *opaque, uint8_t *buf, int size)
{
    auto *s = static_cast<Session *>(opaque);
    if (s->abort.load())
        return AVERROR_EOF;
    return s->readPacket(buf, size);
}

// One open→decode session. Returns true when the caller should retry (reconnect).
bool runSession(const std::shared_ptr<Session> &s, bool *streamingOut)
{
    // A W3C media fragment (#t=N) asks for a start offset into a file source
    // (playbackFileUrl). It is ours, not the server's: strip it before opening.
    // Only on http(s): a user-entered stream URL or local path is left intact.
    QString source = s->source;
    double startOffsetSecs = 0;
    const int hash = source.startsWith(QLatin1String("http"))
                         ? source.lastIndexOf(QLatin1String("#t=")) : -1;
    if (hash >= 0) {
        startOffsetSecs = qMax(0.0, source.mid(hash + 3).toDouble());
        source.truncate(hash);
    }
    const QByteArray url = source.toUtf8();
    // A custom packet source (native Baichuan) is treated as live: real-time paced
    // by the device, no URL to open.
    const bool packetSource = static_cast<bool>(s->readPacket);
    const bool live = packetSource || isLiveUrl(source);

    InterruptContext interrupt{s.get(), QDeadlineTimer(kOpenTimeoutMs)};

    AVFormatContext *fmt = avformat_alloc_context();
    fmt->interrupt_callback.callback = &InterruptContext::callback;
    fmt->interrupt_callback.opaque = &interrupt;

    // Custom-IO cleanup must outlive fmt close (avformat_close_input leaves a
    // caller-supplied AVIOContext alone), so this guard is declared first.
    AVIOContext *avioCtx = nullptr;
    struct AvioGuard {
        AVIOContext **pb;
        ~AvioGuard()
        {
            if (*pb) {
                av_freep(&(*pb)->buffer);
                avio_context_free(pb);
            }
        }
    } avioGuard{&avioCtx};

    AVDictionary *opts = nullptr;
    const AVInputFormat *ifmt = nullptr;
    if (packetSource) {
        constexpr int kAvioBuf = 1 << 16;
        avioCtx = avio_alloc_context(static_cast<unsigned char *>(av_malloc(kAvioBuf)), kAvioBuf,
                                     0, s.get(), &avioReadThunk, nullptr, nullptr);
        fmt->pb = avioCtx;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        ifmt = av_find_input_format(s->forcedFormat.toUtf8().constData());
        // Raw elementary stream over non-seekable IO: a big probesize lets the
        // parser find the SPS/VPS in the first I-frame (can be ~600 KB) so the size
        // isn't "unspecified". Keep analyzeduration SHORT — the codec params are all
        // in the first IDR, and waiting to estimate the framerate over many frames
        // adds seconds of startup latency on a realtime live stream (playback is
        // unaffected since its frames arrive faster than realtime).
        av_dict_set(&opts, "probesize", "4000000", 0);
        av_dict_set(&opts, "analyzeduration", "300000", 0);
    } else if (live) {
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        // Don't spend seconds estimating the framerate — the codec params arrive in
        // the SDP/first IDR, so cap the probe to show live video sooner. Do NOT set
        // fflags=nobuffer here: it discards the keyframe find_stream_info already
        // read, forcing the decode loop to wait a whole extra GOP (~4s on the sub
        // stream) for the next IDR before the first frame appears.
        av_dict_set(&opts, "analyzeduration", "500000", 0);
        av_dict_set(&opts, "probesize", "500000", 0);
    }
    // Reolink devices serve HTTPS (playback FLV, snapshots) with a SELF-SIGNED
    // certificate — there is no CA to validate against on a LAN appliance, and
    // the HTTP-CGI transport has always accepted it (CURLOPT_SSL_VERIFYPEER 0).
    // FFmpeg 8.0 flipped tls_verify's default from 0 to 1, so the media path
    // started rejecting the same certificate the rest of the app accepts:
    // playback died with "Peer certificate failed verification" and retried
    // into a black screen. State the project's existing posture explicitly
    // rather than depending on a library default that moved under us.
    if (!packetSource && (url.startsWith("https://") || url.startsWith("rtsps://")))
        av_dict_set(&opts, "tls_verify", "0", 0);
    int rc = avformat_open_input(&fmt, packetSource ? nullptr : url.constData(), ifmt, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        char buf[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(rc, buf, sizeof(buf));
        postState(s, State::Error, QString::fromUtf8(buf));
        return !packetSource && (live || s->retryOnError.load());
    }

    struct FmtGuard {
        AVFormatContext *ctx;
        ~FmtGuard() { avformat_close_input(&ctx); }
    } fmtGuard{fmt};

    interrupt.deadline = QDeadlineTimer(kOpenTimeoutMs);
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        postState(s, State::Error, QStringLiteral("could not read stream info"));
        return !packetSource && (live || s->retryOnError.load());
    }

    if (startOffsetSecs > 0 && !live) {
        const int64_t ts = static_cast<int64_t>(startOffsetSecs * AV_TIME_BASE);
        if (avformat_seek_file(fmt, -1, INT64_MIN, ts, ts, 0) < 0)
            qCWarning(lcMedia) << redacted(source) << "seek to" << startOffsetSecs << "s failed";
    }

    const int videoIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        postState(s, State::Error, QStringLiteral("no video stream"));
        return false;
    }
    AVStream *stream = fmt->streams[videoIndex];
    const QtVideo::Rotation rotation = streamRotation(stream, s->expectedSize);

    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        postState(s, State::Error, QStringLiteral("unsupported codec"));
        return false;
    }
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    struct DecGuard {
        AVCodecContext *ctx;
        ~DecGuard() { avcodec_free_context(&ctx); }
    } decGuard{dec};

    avcodec_parameters_to_context(dec, stream->codecpar);
    dec->thread_count = 0; // auto

    // ---- Audio (optional): decode + resample to 48 kHz stereo S16 ----------
    // Reolink sends AAC; anything libavcodec decodes works. A stream without
    // audio, or one we can't open, simply plays silent.
    const int audioIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, videoIndex, nullptr, 0);
    AVCodecContext *adec = nullptr;
    SwrContext *swr = nullptr;
    if (audioIndex >= 0) {
        const AVCodecParameters *apar = fmt->streams[audioIndex]->codecpar;
        if (const AVCodec *acodec = avcodec_find_decoder(apar->codec_id)) {
            adec = avcodec_alloc_context3(acodec);
            if (avcodec_parameters_to_context(adec, apar) < 0 || avcodec_open2(adec, acodec, nullptr) < 0)
                avcodec_free_context(&adec);
        }
    }
    struct AudioGuard {
        AVCodecContext **dec;
        SwrContext **swr;
        ~AudioGuard()
        {
            avcodec_free_context(dec);
            swr_free(swr);
        }
    } audioGuard{&adec, &swr};
    // A packet source's audio arrives beside the demuxed video (packetAudioInput).
    if (!packetSource)
        postHasAudio(s, adec != nullptr);
    AVFrame *aframe = adec ? av_frame_alloc() : nullptr;
    struct AFrameGuard {
        AVFrame **f;
        ~AFrameGuard() { av_frame_free(f); }
    } aframeGuard{&aframe};

    auto decodeAudio = [&](AVPacket *pkt) {
        if (avcodec_send_packet(adec, pkt) < 0)
            return;
        while (avcodec_receive_frame(adec, aframe) == 0) {
            if (!swr) {
                const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 48000, &aframe->ch_layout,
                                        static_cast<AVSampleFormat>(aframe->format),
                                        aframe->sample_rate, 0, nullptr) < 0
                    || swr_init(swr) < 0) {
                    swr_free(&swr);
                    av_frame_unref(aframe);
                    return;
                }
            }
            const int outSamples = swr_get_out_samples(swr, aframe->nb_samples);
            QByteArray pcm(outSamples * 2 * 2, Qt::Uninitialized);
            uint8_t *out = reinterpret_cast<uint8_t *>(pcm.data());
            const int n = swr_convert(swr, &out, outSamples,
                                      const_cast<const uint8_t **>(aframe->extended_data),
                                      aframe->nb_samples);
            av_frame_unref(aframe);
            if (n > 0) {
                pcm.truncate(n * 2 * 2);
                postAudio(s, pcm);
            }
        }
    };

    AVBufferRef *hwCtx = nullptr;
    AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;
    if (hwDecodeEnabled())
        hwPixFmt = setupHwDecode(dec, codec, &hwCtx);
    struct HwGuard {
        AVBufferRef **ctx;
        ~HwGuard() { av_buffer_unref(ctx); }
    } hwGuard{&hwCtx};

    if (avcodec_open2(dec, codec, nullptr) < 0) {
        // Hardware path can fail at open on some driver/profile combos; retry
        // software before giving up.
        if (hwPixFmt != AV_PIX_FMT_NONE) {
            qCInfo(lcMedia) << redacted(source) << "hw decoder open failed — software fallback";
            av_buffer_unref(&dec->hw_device_ctx);
            av_buffer_unref(&hwCtx);
            dec->get_format = nullptr;
            dec->opaque = nullptr;
            hwPixFmt = AV_PIX_FMT_NONE;
            if (avcodec_open2(dec, codec, nullptr) < 0) {
                postState(s, State::Error, QStringLiteral("could not open decoder"));
                return false;
            }
        } else {
            postState(s, State::Error, QStringLiteral("could not open decoder"));
            return false;
        }
    }

    qCInfo(lcMedia) << redacted(source) << "opened:" << codec->name << stream->codecpar->width
                    << "x" << stream->codecpar->height
                    << (hwPixFmt != AV_PIX_FMT_NONE ? "[hw]" : "[sw]");

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *hwTransfer = av_frame_alloc();
    AVFrame *converted = av_frame_alloc();
    SwsContext *sws = nullptr;
    struct LoopGuard {
        AVPacket *p;
        AVFrame *f1, *f2, *f3;
        SwsContext **s;
        ~LoopGuard()
        {
            av_packet_free(&p);
            av_frame_free(&f1);
            av_frame_free(&f2);
            av_frame_free(&f3);
            sws_freeContext(*s);
        }
    } loopGuard{packet, frame, hwTransfer, converted, &sws};

    bool streaming = *streamingOut;
    qint64 playbackStartUs = 0;
    qint64 firstPtsUs = AV_NOPTS_VALUE;
    double pacedRate = 1.0;

    // Drain one decoded frame at a time; shared by the normal and flush paths.
    auto receiveFrames = [&]() {
        while (avcodec_receive_frame(dec, frame) == 0 && !s->abort.load()) {
            // Don't render frames the decoder knows are damaged (missing slices or
            // references — e.g. Reolink's flawed RTSP output for 8K duo mains).
            // A brief freeze beats a band of macroblock garbage.
            if ((frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->decode_error_flags) {
                av_frame_unref(frame);
                continue;
            }
            // Hardware frames live on the GPU; pull them into system memory.
            AVFrame *decoded = frame;
            if (hwPixFmt != AV_PIX_FMT_NONE && frame->format == hwPixFmt) {
                if (av_hwframe_transfer_data(hwTransfer, frame, 0) < 0) {
                    av_frame_unref(frame);
                    continue;
                }
                hwTransfer->pts = frame->pts; // transfer drops timing metadata
                decoded = hwTransfer;
            }

            const QVideoFrameFormat::PixelFormat qtFormat = mapPixelFormat(decoded->format);
            AVFrame *out = decoded;
            QVideoFrameFormat::PixelFormat outFormat = qtFormat;
            if (qtFormat == QVideoFrameFormat::Format_Invalid) {
                sws = sws_getCachedContext(sws, decoded->width, decoded->height,
                                           static_cast<AVPixelFormat>(decoded->format),
                                           decoded->width, decoded->height, AV_PIX_FMT_YUV420P,
                                           SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws) {
                    if (decoded == hwTransfer)
                        av_frame_unref(hwTransfer);
                    av_frame_unref(frame);
                    continue; // swscale can't ingest this pixel format; drop
                }
                converted->width = decoded->width;
                converted->height = decoded->height;
                converted->format = AV_PIX_FMT_YUV420P;
                if (av_frame_get_buffer(converted, 0) < 0) {
                    if (decoded == hwTransfer)
                        av_frame_unref(hwTransfer);
                    av_frame_unref(frame);
                    continue;
                }
                sws_scale(sws, decoded->data, decoded->linesize, 0, decoded->height,
                          converted->data, converted->linesize);
                out = converted;
                outFormat = QVideoFrameFormat::Format_YUV420P;
            }

            // Pause gate (file sources): hold this frame until resumed, or until
            // stepFrame() releases exactly one. The pacing clock restarts after.
            bool rebase = false;
            if (!live || s->packetPausable) {
                while (s->paused.load(std::memory_order_relaxed)
                       && !s->abort.load(std::memory_order_relaxed)) {
                    rebase = true;
                    int pending = s->steps.load();
                    if (pending > 0 && s->steps.compare_exchange_weak(pending, pending - 1))
                        break;
                    av_usleep(10000);
                }
            }

            // File pacing: map the stream clock onto the wall clock, scaled by the
            // playback rate (live is paced by the network).
            if (!live && decoded->pts != AV_NOPTS_VALUE) {
                const qint64 ptsUs =
                    av_rescale_q(decoded->pts, stream->time_base, AVRational{1, 1000000});
                const double rate = qBound(0.125, s->rate.load(std::memory_order_relaxed), 16.0);
                if (rebase || rate != pacedRate
                    || firstPtsUs == static_cast<qint64>(AV_NOPTS_VALUE) || ptsUs < firstPtsUs) {
                    firstPtsUs = ptsUs;
                    playbackStartUs = av_gettime_relative();
                    pacedRate = rate;
                }
                const qint64 waitUs = playbackStartUs
                                      + static_cast<qint64>((ptsUs - firstPtsUs) / rate)
                                      - av_gettime_relative();
                if (waitUs > 0 && waitUs < 2000000)
                    abortableSleepUs(s.get(), waitUs);
            }

            if (emitFrame(s, out, outFormat, rotation)) {
                const qint64 n = s->framesDecoded.fetch_add(1) + 1;
                if (!streaming) {
                    streaming = true;
                    *streamingOut = true;
                    postState(s, State::Streaming, QString());
                    qCInfo(lcMedia) << redacted(source) << "first frame delivered";
                }
                if (n % 100 == 0)
                    postFramesTick(s);
            }
            if (out == converted)
                av_frame_unref(converted);
            if (decoded == hwTransfer)
                av_frame_unref(hwTransfer);
            av_frame_unref(frame);
        }
    };

    Recorder recorder;
    // Opens/closes the recorder to match the requested state. Called each packet.
    auto syncRecording = [&]() {
        if (s->recordRequested.load() && !recorder.active()) {
            QString path;
            {
                QMutexLocker lock(&s->recMutex);
                path = s->recPath;
            }
            if (recorder.open(path, stream)) {
                s->recording.store(true);
                postRecording(s, true, path, QString());
                qCInfo(lcMedia) << redacted(source) << "recording to" << path;
            } else {
                recorder.finalize();
                s->recordRequested.store(false);
                postRecording(s, false, QString(), QStringLiteral("could not open recording file"));
            }
        } else if (!s->recordRequested.load() && recorder.active()) {
            const QString saved = recorder.finalize();
            s->recording.store(false);
            postRecording(s, false, saved, saved.isEmpty()
                                               ? QStringLiteral("no frames captured")
                                               : QString());
        }
    };
    // Finalize on any session exit (EOF, reconnect, stop). Recording does not
    // survive a reconnect — it saves what it has and disarms.
    auto finalizeRecording = [&]() {
        if (!recorder.active())
            return;
        const QString saved = recorder.finalize();
        s->recording.store(false);
        s->recordRequested.store(false);
        postRecording(s, false, saved,
                      saved.isEmpty() ? QStringLiteral("no frames captured") : QString());
    };

    while (!s->abort.load()) {
        interrupt.deadline = QDeadlineTimer(kReadStallTimeoutMs);
        rc = av_read_frame(fmt, packet);
        if (rc == AVERROR_EOF) {
            avcodec_send_packet(dec, nullptr); // flush: drain buffered frames
            receiveFrames();
            if (!live && s->loop.load()) {
                if (av_seek_frame(fmt, videoIndex, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
                    avcodec_flush_buffers(dec);
                    firstPtsUs = AV_NOPTS_VALUE;
                    continue;
                }
                // Non-seekable input can't loop; fall through to a reconnect.
            }
            finalizeRecording();
            return live && !packetSource; // a custom source is one-shot: don't retry
        }
        if (rc < 0) {
            postState(s, State::Error, QStringLiteral("read error / stalled"));
            finalizeRecording();
            return !packetSource && (live || s->retryOnError.load());
        }
        if (packet->stream_index != videoIndex) {
            // Audio plays only at 1x and unpaused; otherwise it is skipped.
            if (adec && packet->stream_index == audioIndex
                && s->audioEnabled.load(std::memory_order_relaxed)
                && !s->paused.load(std::memory_order_relaxed)
                && s->rate.load(std::memory_order_relaxed) == 1.0)
                decodeAudio(packet);
            av_packet_unref(packet);
            continue;
        }
        syncRecording();
        if (recorder.active())
            recorder.write(packet); // stream-copy before the packet is consumed
        rc = avcodec_send_packet(dec, packet);
        av_packet_unref(packet);
        if (rc < 0 && rc != AVERROR(EAGAIN))
            continue; // tolerate bitstream hiccups on live sources
        receiveFrames();
    }
    finalizeRecording(); // clean stop
    return false;
}

void runWorker(std::shared_ptr<Session> s)
{
    int backoffMs = kBackoffStartMs;
    bool streaming = false;
    // Live sources reconnect indefinitely; a non-live retry (playback FLV on a
    // busy NVR) is capped so it can't loop forever.
    const bool live = isLiveUrl(s->source);
    int retriesLeft = 6;
    while (!s->abort.load()) {
        const qint64 framesBefore = s->framesDecoded.load();
        const bool retry = runSession(s, &streaming);
        if (s->abort.load() || !retry)
            break;
        if (s->framesDecoded.load() - framesBefore > 100) {
            backoffMs = kBackoffStartMs;
            retriesLeft = 6; // made progress; reset the budget
        }
        if (!live && --retriesLeft <= 0)
            break;
        postState(s, State::Connecting, QStringLiteral("reconnecting"));
        QDeadlineTimer wait(backoffMs);
        while (!wait.hasExpired() && !s->abort.load())
            QThread::msleep(50);
        backoffMs = qMin(backoffMs * 2, kBackoffCapMs);
    }
    if (!s->abort.load()) // terminal non-retryable end (EOF/no-video)
        postState(s, State::Stopped, QString());
}

} // namespace

// Audio of a packet source (native Baichuan), which arrives beside the video
// instead of inside the demuxed stream: decoded on the caller's thread into the
// same 48 kHz stereo S16 PCM as a URL stream's audio, then posted to the player.
struct PacketAudio {
    QMutex backMutex; // guards `player`, nulled when replaced or the player goes
    QPointer<StreamPlayer> player;
    std::atomic<bool> enabled{false};

    QMutex decodeMutex;
    bool announced = false;
    AVCodecContext *aac = nullptr;
    AVFrame *frame = nullptr;
    SwrContext *swr = nullptr;
    int swrRate = 0;
    int swrFormat = -1;
    int swrChannels = 0;

    ~PacketAudio()
    {
        avcodec_free_context(&aac);
        av_frame_free(&frame);
        swr_free(&swr);
    }

    template <typename Fn> void toPlayer(Fn fn)
    {
        QMutexLocker lock(&backMutex);
        if (StreamPlayer *p = player)
            QMetaObject::invokeMethod(p, [p, fn] { fn(p); }, Qt::QueuedConnection);
    }

    void play(const uint8_t **in, int samples, const AVChannelLayout &layout, int format, int rate)
    {
        if (!swr || swrRate != rate || swrFormat != format || swrChannels != layout.nb_channels) {
            swr_free(&swr);
            const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 48000, &layout,
                                    static_cast<AVSampleFormat>(format), rate, 0, nullptr) < 0
                || swr_init(swr) < 0) {
                swr_free(&swr);
                return;
            }
            swrRate = rate;
            swrFormat = format;
            swrChannels = layout.nb_channels;
        }
        const int outSamples = swr_get_out_samples(swr, samples);
        QByteArray pcm(outSamples * 2 * 2, Qt::Uninitialized);
        uint8_t *out = reinterpret_cast<uint8_t *>(pcm.data());
        const int n = swr_convert(swr, &out, outSamples, in, samples);
        if (n <= 0)
            return;
        pcm.truncate(n * 2 * 2);
        toPlayer([pcm](StreamPlayer *p) { p->pushAudio(pcm); });
    }

    void input(const QByteArray &data, bool isAac)
    {
        QMutexLocker lock(&decodeMutex);
        if (!announced) {
            announced = true;
            toPlayer([](StreamPlayer *p) { p->setHasAudioFromWorker(true); });
        }
        if (!enabled.load() || data.isEmpty())
            return;
        if (isAac) {
            if (!aac) {
                const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
                aac = codec ? avcodec_alloc_context3(codec) : nullptr;
                if (!aac || avcodec_open2(aac, codec, nullptr) < 0) {
                    avcodec_free_context(&aac);
                    return;
                }
                frame = av_frame_alloc();
            }
            AVPacket *pkt = av_packet_alloc();
            if (av_new_packet(pkt, static_cast<int>(data.size())) == 0) {
                memcpy(pkt->data, data.constData(), data.size());
                if (avcodec_send_packet(aac, pkt) == 0) {
                    while (avcodec_receive_frame(aac, frame) == 0) {
                        play(const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples,
                             frame->ch_layout, frame->format, frame->sample_rate);
                        av_frame_unref(frame);
                    }
                }
            }
            av_packet_free(&pkt);
            return;
        }
        // A Reolink IMA-ADPCM block, as the talk path sends them: 00 01, the
        // block length in words, 00, then the predictor (LE16) and step index
        // (LE16) the block starts from, then the nibbles. Mono, 8 kHz.
        if (data.size() <= 8 || data.at(0) != 0 || data.at(1) != 1)
            return;
        const auto *d = reinterpret_cast<const uchar *>(data.constData());
        const qint16 predictor = static_cast<qint16>(d[4] | (d[5] << 8));
        const int index = qBound(0, int(d[6]), 88);
        const QVector<qint16> samples = ImaAdpcm::decode(data.mid(8), predictor, index);
        const uint8_t *in = reinterpret_cast<const uint8_t *>(samples.constData());
        const AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
        play(&in, static_cast<int>(samples.size()), mono, AV_SAMPLE_FMT_S16, 8000);
    }
};

// applyStateFromWorker runs on the GUI thread (posted via invokeMethod).
void StreamPlayer::applyStateFromWorker(State state, const QString &error)
{
    applyState(state, error);
}

void StreamPlayer::applyRecordingState(bool recording, const QString &path, const QString &error)
{
    if (m_recording != recording) {
        m_recording = recording;
        emit recordingChanged();
    }
    if (!recording) {
        if (!path.isEmpty())
            emit recordingSaved(path);
        else if (!error.isEmpty())
            emit recordingFailed(error);
    }
}

bool StreamPlayer::recording() const
{
    return m_recording;
}

bool StreamPlayer::startRecording(const QString &path)
{
    if (!m_session || m_state != State::Streaming)
        return false;
    QString out = path;
    if (out.isEmpty()) {
        const QString dir = Paths::recordingsDir();
        const QString stamp =
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss"));
        out = dir + QStringLiteral("/clip_") + stamp + QStringLiteral(".mp4");
    }
    {
        QMutexLocker lock(&m_session->recMutex);
        m_session->recPath = out;
    }
    m_session->recordRequested.store(true);
    return true;
}

void StreamPlayer::stopRecording()
{
    if (m_session)
        m_session->recordRequested.store(false);
}

StreamPlayer::StreamPlayer(QObject *parent) : QObject(parent) {}

StreamPlayer::~StreamPlayer()
{
    detachPacketAudio();
    if (m_activeStop)
        m_activeStop();
    if (m_pendingStop)
        m_pendingStop();
    if (m_session) {
        // Null the back-pointer under the lock so no queued callback touches us
        // after this returns, then signal abort. The detached worker drops the
        // last Session reference on its own; we never join.
        {
            QMutexLocker lock(&m_session->backMutex);
            m_session->player = nullptr;
        }
        m_session->abort.store(true);
    }
}

void StreamPlayer::setSource(const QString &source)
{
    if (m_source == source && !m_pendingReader)
        return;
    stop();
    m_pendingReader = nullptr; // URL source supersedes any packet source
    detachPacketAudio();
    m_pendingFormat.clear();
    m_pendingPausable = false;
    m_pendingStop = nullptr;
    m_source = source;
    emit sourceChanged();
}

void StreamPlayer::setExpectedSize(const QSize &size)
{
    if (m_expectedSize == size)
        return;
    m_expectedSize = size;
    emit expectedSizeChanged();
}

void StreamPlayer::setPacketSource(std::function<int(unsigned char *, int)> reader,
                                   const QString &format, std::function<void()> onStop,
                                   bool pausable)
{
    m_pendingPausable = pausable;
    stop();
    m_pendingReader = std::move(reader);
    m_pendingFormat = format;
    m_pendingStop = std::move(onStop);
    m_source.clear();
    emit sourceChanged();
}

std::function<void(const QByteArray &, bool)> StreamPlayer::packetAudioInput()
{
    detachPacketAudio();
    setHasAudioFromWorker(false); // until this source's first audio frame
    auto audio = std::make_shared<PacketAudio>();
    audio->player = this;
    audio->enabled.store(m_audioEnabled);
    m_packetAudio = audio;
    return [audio](const QByteArray &frame, bool aac) { audio->input(frame, aac); };
}

void StreamPlayer::detachPacketAudio()
{
    if (!m_packetAudio)
        return;
    QMutexLocker lock(&m_packetAudio->backMutex);
    m_packetAudio->player = nullptr; // its late frames become no-ops
    lock.unlock();
    m_packetAudio.reset();
}

QVideoSink *StreamPlayer::videoSink() const
{
    return m_session ? m_session->sink.data() : m_pendingSink.data();
}

void StreamPlayer::setVideoSink(QVideoSink *sink)
{
    if (m_pendingSink == sink)
        return;
    m_pendingSink = sink;
    if (m_session) {
        QMutexLocker lock(&m_session->sinkMutex);
        m_session->sink = sink;
    }
    emit videoSinkChanged();
}

qint64 StreamPlayer::framesDecoded() const
{
    return m_session ? m_session->framesDecoded.load() : 0;
}

void StreamPlayer::setPlaybackRate(double rate)
{
    rate = qBound(0.125, rate, 16.0);
    if (qFuzzyCompare(m_rate, rate))
        return;
    m_rate = rate;
    if (m_session)
        m_session->rate.store(rate);
    emit playbackRateChanged();
}

void StreamPlayer::setPaused(bool paused)
{
    if (m_paused == paused)
        return;
    m_paused = paused;
    if (m_session) {
        m_session->steps.store(0);
        m_session->paused.store(paused);
    }
    if (paused)
        closeAudio(); // don't keep playing what was buffered
    emit pausedChanged();
}

void StreamPlayer::setAudioEnabled(bool enabled)
{
    if (m_audioEnabled == enabled)
        return;
    m_audioEnabled = enabled;
    if (m_session)
        m_session->audioEnabled.store(enabled);
    if (m_packetAudio)
        m_packetAudio->enabled.store(enabled);
    if (!enabled)
        closeAudio();
    emit audioEnabledChanged();
}

void StreamPlayer::setVolume(double volume)
{
    volume = qBound(0.0, volume, 1.0);
    if (qFuzzyCompare(m_volume, volume))
        return;
    m_volume = volume;
    if (m_audioSink)
        m_audioSink->setVolume(volume);
    emit volumeChanged();
}

void StreamPlayer::setHasAudioFromWorker(bool has)
{
    if (m_hasAudio == has)
        return;
    m_hasAudio = has;
    emit hasAudioChanged();
}

void StreamPlayer::pushAudio(const QByteArray &pcm)
{
    if (!m_audioEnabled || m_paused)
        return;
    if (!m_audioSink) {
        QAudioFormat format;
        format.setSampleRate(48000);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);
        m_audioSink = new QAudioSink(QMediaDevices::defaultAudioOutput(), format, this);
        m_audioSink->setBufferSize(48000 * 2 * 2 / 4); // ~250 ms: low live latency
        m_audioSink->setVolume(m_volume);
        m_audioIo = m_audioSink->start();
        int peak = 0;
        const auto *v = reinterpret_cast<const qint16 *>(pcm.constData());
        for (qsizetype i = 0; i < pcm.size() / 2; ++i)
            peak = qMax(peak, qAbs(int(v[i])));
        qCInfo(lcMedia) << "audio output started:" << pcm.size() << "bytes, peak" << peak;
    }
    if (m_audioIo)
        m_audioIo->write(pcm); // a full buffer drops the excess (stays live)
}

void StreamPlayer::closeAudio()
{
    if (!m_audioSink)
        return;
    m_audioSink->stop();
    m_audioSink->deleteLater();
    m_audioSink = nullptr;
    m_audioIo = nullptr;
}

void StreamPlayer::stepFrame()
{
    if (m_paused && m_session)
        m_session->steps.fetch_add(1);
}

void StreamPlayer::setLoop(bool loop)
{
    if (m_loop == loop)
        return;
    m_loop = loop;
    if (m_session)
        m_session->loop.store(loop);
    emit loopChanged();
}

void StreamPlayer::setRetryOnError(bool v)
{
    if (m_retryOnError == v)
        return;
    m_retryOnError = v;
    if (m_session)
        m_session->retryOnError.store(v);
    emit retryOnErrorChanged();
}

void StreamPlayer::start()
{
    if (m_source.isEmpty() && !m_pendingReader)
        return;
    stop();
    setPaused(false); // a fresh session always starts playing

    auto s = std::make_shared<Session>();
    s->player = this;
    s->source = m_source;
    s->readPacket = m_pendingReader;
    s->forcedFormat = m_pendingFormat;
    s->packetPausable = m_pendingPausable;
    s->expectedSize = m_expectedSize;
    s->loop.store(m_loop);
    s->rate.store(m_rate);
    s->audioEnabled.store(m_audioEnabled);
    s->retryOnError.store(m_retryOnError);
    {
        QMutexLocker lock(&s->sinkMutex);
        s->sink = m_pendingSink;
    }
    m_session = s;
    // Adopt the pending source's teardown as the running session's teardown, so the
    // stop() above (which tears down the previous session) doesn't kill this source.
    m_activeStop = std::move(m_pendingStop);
    m_pendingStop = nullptr;
    applyState(State::Connecting, QString());
    std::thread(runWorker, s).detach();
}

void StreamPlayer::stop()
{
    // Unblock and tear down the running session's custom packet source (native
    // Baichuan). Its read() may be blocking the decode worker; this wakes it so
    // avformat unwinds. The pending (not-yet-started) source is left untouched.
    if (m_activeStop) {
        m_activeStop();
        m_activeStop = nullptr;
    }
    closeAudio(); // drop buffered sound with the stream
    if (!m_session)
        return;
    {
        QMutexLocker lock(&m_session->backMutex);
        m_session->player = nullptr; // stale callbacks become no-ops
    }
    m_session->abort.store(true);
    m_session.reset();
    if (m_recording) {
        m_recording = false;
        emit recordingChanged();
    }
    if (m_state != State::Idle)
        applyState(State::Stopped, QString());
}

void StreamPlayer::applyState(State state, const QString &error)
{
    if (m_errorString != error) {
        m_errorString = error;
        emit errorStringChanged();
    }
    if (m_state != state) {
        m_state = state;
        emit stateChanged();
    }
}

} // namespace rl
