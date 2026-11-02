#pragma once

#include <QByteArray>
#include <QVector>
#include <QtGlobal>

namespace rl {

// IMA (DVI) ADPCM, the codec Reolink cameras take for two-way talk: 4 bits per
// 16-bit sample, FIRST sample of each byte in the HIGH nibble (DVI order —
// Microsoft IMA WAV uses the low nibble first). Standard step/index tables.
class ImaAdpcm
{
public:
    // Encoder state at the start of the next block: Reolink's talk frames carry
    // it in their header so the camera can decode each block on its own.
    qint16 predictor() const { return m_predictor; }
    quint8 index() const { return static_cast<quint8>(m_index); }

    // Encode `count` samples (count/2 bytes; an odd last sample fills a high
    // nibble on its own) and advance the state.
    QByteArray encode(const qint16 *samples, int count);

    // Decode count*2 nibbles with an explicit start state (tests, diagnostics).
    static QVector<qint16> decode(const QByteArray &data, qint16 predictor, int index);

private:
    int m_predictor = 0;
    int m_index = 0;
};

} // namespace rl
