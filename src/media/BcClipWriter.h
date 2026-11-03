#pragma once

#include "media/BcMediaParser.h"

#include <QList>
#include <QString>

struct AVFormatContext;

// Writes a Baichuan media stream (BcMediaParser frames) into an MP4, without
// re-encoding: H.264/H.265 video timed by the frames' own timestamps, and AAC
// audio (its ADTS headers become the MP4's AudioSpecificConfig). Starts at the
// first key frame. Used to export recordings the device sends over Baichuan.
namespace rl {

class BcClipWriter
{
public:
    explicit BcClipWriter(QString path);
    ~BcClipWriter();

    void addVideo(const BcMediaParser::VideoFrame &f, int width, int height);
    void addAudio(const BcMediaParser::AudioFrame &a);
    // Writes the trailer; false when no video was written or writing failed.
    bool finish(QString *error = nullptr);

    int videoFrames() const { return m_videoFrames; }

private:
    bool open(const BcMediaParser::VideoFrame &key, int width, int height);
    bool openAudio(const QByteArray &adts);
    void writeHeader();
    void writeVideo(const BcMediaParser::VideoFrame &f);

    QString m_path;
    AVFormatContext *m_oc = nullptr;
    int m_video = -1;
    int m_audio = -1;
    bool m_headerWritten = false;
    bool m_failed = false;
    QString m_error;
    quint32 m_firstUs = 0;
    qint64 m_lastPts = -1;
    qint64 m_audioSamples = 0;
    int m_audioRate = 0;
    int m_videoFrames = 0;
    QList<BcMediaParser::VideoFrame> m_pending; // held until the header is out
};

} // namespace rl
