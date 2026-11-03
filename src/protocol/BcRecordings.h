#pragma once

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QString>
#include <QVector>

#include "media/BcMediaParser.h"

#include <atomic>
#include <functional>

// Recordings over Baichuan, as the official client finds them (BaichuanReplayer):
//
//   cmd 142 <DayRecords>     the days of a month that have recordings
//   cmd 14  <FileInfoList>   opens a search for a day and stream → <handle>
//   cmd 15  <FileInfoList>   one page of files per call, until an empty one
//   cmd 16                   closes the search
//
// Each file carries its path on the device (<Id>, what cmd 5 plays), its times
// (device local time) and what triggered it (<recordType>).
namespace rl {

class BcSession;

namespace bcrec {

struct Recording {
    QString name; // e.g. 0120261102072209
    QString id;   // the file's path on the device
    QDateTime start;
    QDateTime end;
    int triggers = 0; // api::trigger flags from <recordType>
};

// Parsers (exposed for tests).
QVector<int> parseRecordDays(const QByteArray &xml);   // cmd 142: 1-based days
QVector<Recording> parseFileList(const QByteArray &xml); // one cmd 15 page
int recordTriggers(const QString &recordType);         // "md, people…" → flags

// `uid`: the camera's UID (an NVR's camera has its own); may be empty.
QVector<int> monthDays(BcSession &s, int channel, const QString &uid, int year, int month);
// A day's files of one stream, sorted by start; false if the device stopped
// answering.
bool searchDay(BcSession &s, int channel, const QString &uid, const QDate &date, bool mainStream,
               QVector<Recording> *out);

// A recording cut from `from` to `to` (cmd 143 DOWNLOAD_CUT, as the official
// client exports a clip): the device sends that span's media, faster than real
// time, then stops. Frames go to the callbacks (on the session's worker).
bool downloadClip(BcSession &s, int channel, const QString &uid, const QDateTime &from,
                  const QDateTime &to, bool mainStream,
                  const std::function<void(const BcMediaParser::VideoFrame &, int w, int h)> &onVideo,
                  const std::function<void(const BcMediaParser::AudioFrame &)> &onAudio,
                  const std::atomic<bool> *abort, QString *error);

} // namespace bcrec
} // namespace rl
