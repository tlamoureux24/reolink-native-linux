#include "BcClipWriter.h"

#include <QList>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

namespace rl {

namespace {

constexpr AVRational kMicros{1, 1000000};

// The parameter sets at the head of a key frame (Annex-B), which the MP4
// muxer turns into the avcC / hvcC box.
QByteArray parameterSets(const QByteArray &annexB, bool hevc)
{
    QByteArray out;
    int i = 0;
    const int n = int(annexB.size());
    const auto *d = reinterpret_cast<const uchar *>(annexB.constData());
    auto nextStart = [&](int from) {
        for (int k = from; k + 3 < n; ++k)
            if (d[k] == 0 && d[k + 1] == 0 && (d[k + 2] == 1 || (d[k + 2] == 0 && d[k + 3] == 1)))
                return k;
        return -1;
    };
    i = nextStart(0);
    while (i >= 0) {
        const int sc = d[i + 2] == 1 ? 3 : 4;
        const int nal = i + sc;
        int next = nextStart(nal);
        const int end = next < 0 ? n : next;
        if (nal < n) {
            const int type = hevc ? (d[nal] >> 1) & 0x3F : d[nal] & 0x1F;
            const bool ps = hevc ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
            if (ps)
                out += QByteArray("\x00\x00\x00\x01", 4) + annexB.mid(nal, end - nal);
            else if (!out.isEmpty())
                break; // parameter sets come first; the slice data follows
        }
        i = next;
    }
    return out;
}

// ADTS header → (header length, sample rate, AudioSpecificConfig); false if
// this is not an ADTS frame.
bool parseAdts(const QByteArray &f, int *hdr, int *rate, QByteArray *asc, int *channels)
{
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                22050, 16000, 12000, 11025, 8000, 7350};
    if (f.size() < 7)
        return false;
    const auto *d = reinterpret_cast<const uchar *>(f.constData());
    if (d[0] != 0xFF || (d[1] & 0xF0) != 0xF0)
        return false;
    const bool noCrc = d[1] & 1;
    const int profile = (d[2] >> 6) & 3;          // AAC object type - 1
    const int sfi = (d[2] >> 2) & 0xF;
    const int ch = ((d[2] & 1) << 2) | (d[3] >> 6);
    if (sfi >= 13)
        return false;
    *hdr = noCrc ? 7 : 9;
    *rate = rates[sfi];
    *channels = ch;
    const int objectType = profile + 1;
    const quint16 cfg = quint16((objectType << 11) | (sfi << 7) | (ch << 3));
    *asc = QByteArray(1, char(cfg >> 8)) + QByteArray(1, char(cfg & 0xFF));
    return true;
}

// H.265 or H.264, from the stream itself: recorded frames can be labelled
// H264 in their BCMedia header while carrying H.265 (an RLC-811A's main
// recordings). The first NAL of a key frame tells: H.265 VPS/SPS/PPS/IDR
// headers, else H.264.
bool looksHevc(const QByteArray &annexB)
{
    const auto *d = reinterpret_cast<const uchar *>(annexB.constData());
    const int n = int(annexB.size());
    for (int k = 0; k + 4 < n; ++k) {
        if (d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1) {
            const uchar h = d[k + 3];
            const int hevcType = (h >> 1) & 0x3F;
            const int avcType = h & 0x1F;
            if (avcType == 7 || avcType == 8 || avcType == 5 || avcType == 9 || avcType == 6)
                return (hevcType >= 32 && hevcType <= 40) && (h & 0x81) == 0 && d[k + 4] == 1;
            return hevcType >= 16 && hevcType <= 40;
        }
    }
    return false;
}

} // namespace

BcClipWriter::BcClipWriter(QString path) : m_path(std::move(path)) {}

BcClipWriter::~BcClipWriter()
{
    if (m_oc) {
        if (m_oc->pb)
            avio_closep(&m_oc->pb);
        avformat_free_context(m_oc);
    }
}

bool BcClipWriter::open(const BcMediaParser::VideoFrame &key, int width, int height)
{
    const QByteArray p = m_path.toUtf8();
    if (avformat_alloc_output_context2(&m_oc, nullptr, "mp4", p.constData()) < 0 || !m_oc) {
        m_error = QStringLiteral("cannot create the MP4");
        return false;
    }
    const bool hevc = key.codec == BcMediaParser::Codec::H265 || looksHevc(key.annexB);
    AVStream *v = avformat_new_stream(m_oc, nullptr);
    if (!v)
        return false;
    v->time_base = kMicros;
    v->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    v->codecpar->codec_id = hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    v->codecpar->width = width;
    v->codecpar->height = height;
    const QByteArray ps = parameterSets(key.annexB, hevc);
    v->codecpar->extradata = static_cast<uint8_t *>(av_mallocz(ps.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    memcpy(v->codecpar->extradata, ps.constData(), ps.size());
    v->codecpar->extradata_size = int(ps.size());
    m_video = v->index;
    if (avio_open(&m_oc->pb, p.constData(), AVIO_FLAG_WRITE) < 0) {
        m_error = QStringLiteral("cannot write %1").arg(m_path);
        return false;
    }
    return true;
}

bool BcClipWriter::openAudio(const QByteArray &adts)
{
    int hdr = 0, rate = 0, channels = 0;
    QByteArray asc;
    if (!parseAdts(adts, &hdr, &rate, &asc, &channels))
        return false;
    AVStream *a = avformat_new_stream(m_oc, nullptr);
    if (!a)
        return false;
    a->time_base = AVRational{1, rate};
    a->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    a->codecpar->codec_id = AV_CODEC_ID_AAC;
    a->codecpar->sample_rate = rate;
    av_channel_layout_default(&a->codecpar->ch_layout, qMax(1, channels));
    a->codecpar->frame_size = 1024;
    a->codecpar->extradata = static_cast<uint8_t *>(av_mallocz(asc.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    memcpy(a->codecpar->extradata, asc.constData(), asc.size());
    a->codecpar->extradata_size = int(asc.size());
    m_audio = a->index;
    m_audioRate = rate;
    return true;
}

void BcClipWriter::addVideo(const BcMediaParser::VideoFrame &f, int width, int height)
{
    if (m_failed)
        return;
    if (!m_oc) {
        if (!f.keyFrame || f.codec == BcMediaParser::Codec::Unknown)
            return; // start on a key frame so the file is decodable
        if (!open(f, width, height)) {
            m_failed = true;
            return;
        }
        m_firstUs = f.microseconds;
    }
    if (!m_headerWritten) {
        // The audio stream, if any, must exist before the header: hold the
        // video until the first audio frame, or for a second without one.
        m_pending.append(f);
        if (m_audio < 0 && qint64(f.microseconds) - qint64(m_firstUs) < 1000000)
            return;
        writeHeader();
        return;
    }
    writeVideo(f);
}

void BcClipWriter::writeHeader()
{
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "movflags", "+faststart", 0);
    const int rc = avformat_write_header(m_oc, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        m_failed = true;
        m_error = QStringLiteral("cannot write the MP4 header");
        return;
    }
    m_headerWritten = true;
    const QList<BcMediaParser::VideoFrame> pending = m_pending;
    m_pending.clear();
    for (const auto &v : pending)
        writeVideo(v);
}

void BcClipWriter::writeVideo(const BcMediaParser::VideoFrame &f)
{
    qint64 pts = qint64(f.microseconds) - qint64(m_firstUs);
    if (pts <= m_lastPts)
        pts = m_lastPts + 1; // a timestamp hiccup must not break the file
    m_lastPts = pts;
    AVPacket *pkt = av_packet_alloc();
    if (av_new_packet(pkt, int(f.annexB.size())) == 0) {
        memcpy(pkt->data, f.annexB.constData(), f.annexB.size());
        pkt->stream_index = m_video;
        pkt->pts = pkt->dts = av_rescale_q(pts, kMicros, m_oc->streams[m_video]->time_base);
        if (f.keyFrame)
            pkt->flags |= AV_PKT_FLAG_KEY;
        if (av_interleaved_write_frame(m_oc, pkt) == 0)
            ++m_videoFrames;
    }
    av_packet_free(&pkt);
}

void BcClipWriter::addAudio(const BcMediaParser::AudioFrame &a)
{
    if (m_failed || !a.aac || !m_oc)
        return; // before the first key frame, or ADPCM (not stored in MP4)
    if (m_audio < 0) {
        if (m_headerWritten || !openAudio(a.data))
            return;
        writeHeader(); // the video held for it goes out now
        if (m_failed)
            return;
    }
    if (!m_headerWritten)
        return;
    int hdr = 0, rate = 0, channels = 0;
    QByteArray asc;
    if (!parseAdts(a.data, &hdr, &rate, &asc, &channels))
        return;
    const QByteArray raw = a.data.mid(hdr);
    AVPacket *pkt = av_packet_alloc();
    if (av_new_packet(pkt, int(raw.size())) == 0) {
        memcpy(pkt->data, raw.constData(), raw.size());
        pkt->stream_index = m_audio;
        pkt->pts = pkt->dts = av_rescale_q(m_audioSamples, AVRational{1, m_audioRate},
                                           m_oc->streams[m_audio]->time_base);
        pkt->duration = av_rescale_q(1024, AVRational{1, m_audioRate}, m_oc->streams[m_audio]->time_base);
        m_audioSamples += 1024;
        av_interleaved_write_frame(m_oc, pkt);
    }
    av_packet_free(&pkt);
}

bool BcClipWriter::finish(QString *error)
{
    if (m_oc && !m_headerWritten && !m_failed && !m_pending.isEmpty())
        writeHeader(); // a clip shorter than a second, without sound
    bool ok = !m_failed && m_headerWritten && m_videoFrames > 0;
    if (ok && av_write_trailer(m_oc) < 0) {
        ok = false;
        m_error = QStringLiteral("cannot finish the MP4");
    }
    if (m_oc) {
        if (m_oc->pb)
            avio_closep(&m_oc->pb);
        avformat_free_context(m_oc);
        m_oc = nullptr;
    }
    if (!ok && error)
        *error = m_error.isEmpty() ? QStringLiteral("no video") : m_error;
    return ok;
}

} // namespace rl
