#include "ImaAdpcm.h"

#include <QVector>

namespace rl {

namespace {
constexpr int kStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kIndexShift[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

int clampIndex(int i) { return i < 0 ? 0 : (i > 88 ? 88 : i); }
int clampSample(int s) { return s < -32768 ? -32768 : (s > 32767 ? 32767 : s); }

// One sample through the standard IMA quantiser; updates predictor/index.
int encodeSample(int sample, int &predictor, int &index)
{
    const int step = kStep[index];
    int diff = sample - predictor;
    int code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    int delta = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; delta += step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; delta += step >> 1; }
    if (diff >= step >> 2) { code |= 1; delta += step >> 2; }
    predictor = clampSample(code & 8 ? predictor - delta : predictor + delta);
    index = clampIndex(index + kIndexShift[code]);
    return code;
}

int decodeNibble(int code, int &predictor, int &index)
{
    const int step = kStep[index];
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    predictor = clampSample(code & 8 ? predictor - delta : predictor + delta);
    index = clampIndex(index + kIndexShift[code]);
    return predictor;
}
} // namespace

QByteArray ImaAdpcm::encode(const qint16 *samples, int count)
{
    QByteArray out;
    out.reserve((count + 1) / 2);
    for (int i = 0; i < count; i += 2) {
        const int hi = encodeSample(samples[i], m_predictor, m_index);
        const int lo = i + 1 < count ? encodeSample(samples[i + 1], m_predictor, m_index) : 0;
        out.append(char((hi << 4) | lo));
    }
    return out;
}

QVector<qint16> ImaAdpcm::decode(const QByteArray &data, qint16 predictor, int index)
{
    QVector<qint16> out;
    out.reserve(data.size() * 2);
    int p = predictor;
    int i = clampIndex(index);
    for (const char c : data) {
        const auto b = static_cast<quint8>(c);
        out.append(static_cast<qint16>(decodeNibble(b >> 4, p, i)));
        out.append(static_cast<qint16>(decodeNibble(b & 0x0F, p, i)));
    }
    return out;
}

} // namespace rl
