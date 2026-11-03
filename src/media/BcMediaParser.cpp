#include "BcMediaParser.h"

#include <QtEndian>

namespace rl {

namespace {

quint32 le32(const char *p)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(p));
}
quint16 le16(const char *p)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(p));
}

// Padding after an I/P/audio payload rounds it up to an 8-byte boundary.
int padFor(quint32 payloadSize)
{
    return static_cast<int>((8 - payloadSize % 8) % 8);
}

bool isInfo(const char *b) // "1001" / "1002"
{
    return b[0] == '1' && b[1] == '0' && b[2] == '0' && (b[3] == '1' || b[3] == '2');
}
bool isVideo(const char *b) // "N0dc" (I) / "N1dc" (P)
{
    return b[2] == 'd' && b[3] == 'c' && b[0] >= '0' && b[0] <= '9' &&
           (b[1] == '0' || b[1] == '1');
}
bool isAudio(const char *b) // "05wb" (AAC) / "01wb" (ADPCM)
{
    return b[2] == 'w' && b[3] == 'b' && b[0] == '0' && (b[1] == '5' || b[1] == '1');
}
bool anyMagic(const char *b)
{
    return isInfo(b) || isVideo(b) || isAudio(b);
}

} // namespace

int BcMediaParser::append(const QByteArray &mediaBytes)
{
    m_buf.append(mediaBytes);
    int emitted = 0;

    for (;;) {
        if (m_buf.size() < 4)
            break;
        const char *b = m_buf.constData();

        if (isInfo(b)) {
            if (m_buf.size() < 32)
                break; // Info V1/V2 are a fixed 32 bytes
            m_width = static_cast<int>(le32(b + 8));
            m_height = static_cast<int>(le32(b + 12));
            m_buf.remove(0, 32);
            continue;
        }

        if (isVideo(b)) {
            if (m_buf.size() < 24)
                break; // need the base header
            const bool keyFrame = (b[1] == '0');
            const quint32 payloadSize = le32(b + 8);
            const quint32 addHdr = le32(b + 12);
            const quint32 micros = le32(b + 16);
            // POSIX wall-clock time rides at offset 24 when the extra header is present.
            const quint32 time = (addHdr >= 4) ? le32(b + 24) : 0;
            const int dataStart = 24 + static_cast<int>(addHdr);
            const int total = dataStart + static_cast<int>(payloadSize) + padFor(payloadSize);
            if (m_buf.size() < total)
                break; // frame spans more messages — wait for more
            VideoFrame f;
            f.keyFrame = keyFrame;
            f.microseconds = micros;
            f.time = time;
            if (b[4] == 'H' && b[5] == '2' && b[6] == '6')
                f.codec = (b[7] == '5') ? Codec::H265 : Codec::H264;
            f.annexB = m_buf.mid(dataStart, static_cast<int>(payloadSize));
            m_buf.remove(0, total);
            if (onVideo)
                onVideo(f);
            ++emitted;
            continue;
        }

        if (isAudio(b)) {
            if (m_buf.size() < 8)
                break;
            const quint32 size = le16(b + 4);
            const int total = 8 + static_cast<int>(size) + padFor(size);
            if (m_buf.size() < total)
                break;
            if (onAudio) {
                AudioFrame a;
                a.aac = (b[1] == '5');
                a.data = m_buf.mid(8, static_cast<int>(size));
                onAudio(a);
            }
            m_buf.remove(0, total);
            continue;
        }

        // Unrecognized magic: resync by dropping bytes to the next valid magic.
        int next = -1;
        for (int i = 1; i + 4 <= m_buf.size(); ++i) {
            if (anyMagic(m_buf.constData() + i)) {
                next = i;
                break;
            }
        }
        if (next < 0) {
            // No magic found; keep the last 3 bytes (a magic may straddle the next
            // append) and drop the rest.
            if (m_buf.size() > 3)
                m_buf.remove(0, m_buf.size() - 3);
            break;
        }
        m_buf.remove(0, next);
    }
    return emitted;
}

// The first NAL of a key frame tells: H.265 VPS/SPS/PPS/IDR headers, else
// H.264.
bool BcMediaParser::isHevc(const VideoFrame &key)
{
    if (key.codec == Codec::H265)
        return true;
    const auto *d = reinterpret_cast<const uchar *>(key.annexB.constData());
    const int n = int(key.annexB.size());
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

} // namespace rl
