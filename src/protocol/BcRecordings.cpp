#include "BcRecordings.h"

#include "protocol/BcSession.h"
#include "protocol/ReolinkApi.h"

#include <QRegularExpression>

#include <algorithm>

namespace rl::bcrec {

namespace {

QByteArray timeXml(const char *tag, const QDate &d, const QTime &t)
{
    return "<" + QByteArray(tag) + "><year>" + QByteArray::number(d.year()) + "</year><month>"
           + QByteArray::number(d.month()) + "</month><day>" + QByteArray::number(d.day())
           + "</day><hour>" + QByteArray::number(t.hour()) + "</hour><minute>"
           + QByteArray::number(t.minute()) + "</minute><second>" + QByteArray::number(t.second())
           + "</second></" + QByteArray(tag) + ">\n";
}

QString leaf(const QString &chunk, const char *tag)
{
    const QRegularExpression re(QStringLiteral("<%1>([^<]*)</%1>").arg(QLatin1String(tag)));
    const auto m = re.match(chunk);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

QString handleOf(const QByteArray &xml)
{
    return leaf(QString::fromUtf8(xml), "handle");
}

} // namespace

QVector<int> parseRecordDays(const QByteArray &xml)
{
    QVector<int> days;
    static const QRegularExpression re(QStringLiteral("<dayType>\\s*<index>(\\d+)</index>"));
    for (auto it = re.globalMatch(QString::fromUtf8(xml)); it.hasNext();) {
        const int d = it.next().captured(1).toInt() + 1;
        if (!days.contains(d))
            days.append(d);
    }
    std::sort(days.begin(), days.end());
    return days;
}

int recordTriggers(const QString &recordType)
{
    using namespace api;
    int t = 0;
    for (const QString &w : recordType.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString k = w.trimmed().toLower();
        if (k == QLatin1String("md") || k == QLatin1String("pir") || k == QLatin1String("io")
            || k == QLatin1String("other"))
            t |= trigger::Motion;
        else if (k == QLatin1String("people") || k == QLatin1String("face"))
            t |= trigger::Person;
        else if (k == QLatin1String("vehicle") || k == QLatin1String("nonmotorveh"))
            t |= trigger::Vehicle;
        else if (k == QLatin1String("dog_cat"))
            t |= trigger::Animal;
        else if (k == QLatin1String("visitor") || k == QLatin1String("package"))
            t |= trigger::Doorbell;
        else if (k == QLatin1String("sched") || k == QLatin1String("manual")
                 || k == QLatin1String("timer"))
            t |= trigger::Timer;
    }
    return t;
}

QVector<Recording> parseFileList(const QByteArray &xml)
{
    QVector<Recording> out;
    const QString s = QString::fromUtf8(xml);
    auto when = [](const QString &chunk, const char *tag) {
        const int a = chunk.indexOf(QStringLiteral("<%1>").arg(QLatin1String(tag)));
        const int z = chunk.indexOf(QStringLiteral("</%1>").arg(QLatin1String(tag)), a);
        if (a < 0 || z < 0)
            return QDateTime();
        const QString t = chunk.mid(a, z - a);
        const QDate d(leaf(t, "year").toInt(), leaf(t, "month").toInt(), leaf(t, "day").toInt());
        const QTime h(leaf(t, "hour").toInt(), leaf(t, "minute").toInt(), leaf(t, "second").toInt());
        return d.isValid() && h.isValid() ? QDateTime(d, h) : QDateTime(); // device local time
    };
    for (int at = 0;;) {
        const int a = s.indexOf(QLatin1String("<FileInfo>"), at);
        if (a < 0)
            break;
        const int z = s.indexOf(QLatin1String("</FileInfo>"), a);
        if (z < 0)
            break;
        const QString chunk = s.mid(a, z - a);
        at = z;
        Recording r;
        r.name = leaf(chunk, "name");
        r.id = leaf(chunk, "Id");
        r.start = when(chunk, "startTime");
        r.end = when(chunk, "endTime");
        r.triggers = recordTriggers(leaf(chunk, "recordType"));
        if (!r.id.isEmpty() && r.start.isValid())
            out.append(r);
    }
    return out;
}

QVector<int> monthDays(BcSession &s, int channel, const QString &uid, int year, int month)
{
    const QDate first(year, month, 1);
    const QByteArray body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<DayRecords version=\"1.1\">\n"
        + timeXml("startTime", first, QTime(0, 0, 0))
        + timeXml("endTime", first.addDays(first.daysInMonth() - 1), QTime(23, 59, 59))
        + "<DayRecordList>\n<DayRecord>\n<index>0</index>\n<channelId>" + QByteArray::number(channel)
        + "</channelId>\n<uid>" + uid.toUtf8() + "</uid>\n</DayRecord>\n</DayRecordList>\n"
          "</DayRecords>\n</body>\n";
    QByteArray xml;
    quint16 status = 0;
    if (!s.requestXml(142, quint8(channel + 1), BcSession::channelExtension(channel), body, &xml,
                      &status)
        || status != 200)
        return {};
    return parseRecordDays(xml);
}

bool searchDay(BcSession &s, int channel, const QString &uid, const QDate &date, bool mainStream,
               QVector<Recording> *out)
{
    const QByteArray ch = QByteArray::number(channel);
    const QByteArray u = uid.toUtf8();
    const quint8 hdr = quint8(channel + 1);
    const QByteArray ext = BcSession::channelExtension(channel);
    const QByteArray open =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<FileInfoList version=\"1.1\">\n<FileInfo>\n<uid>" + u
        + "</uid>\n<searchAITrack>1</searchAITrack>\n<channelId>" + ch
        + "</channelId>\n<logicChnBitmap>255</logicChnBitmap>\n<streamType>"
        + (mainStream ? "mainStream" : "subStream")
        + "</streamType>\n<recordType>manual, sched, io, md, people, face, vehicle, dog_cat, visitor, other, package</recordType>\n"
        + timeXml("startTime", date, QTime(0, 0, 0)) + timeXml("endTime", date, QTime(23, 59, 59))
        + "</FileInfo>\n</FileInfoList>\n</body>\n";
    QByteArray xml;
    quint16 status = 0;
    if (!s.requestXml(14, hdr, ext, open, &xml, &status))
        return false;
    QVector<Recording> files;
    const QString handle = handleOf(xml);
    for (int page = 0; page < 200 && !handle.isEmpty(); ++page) {
        const QByteArray next =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<FileInfoList version=\"1.1\">\n<FileInfo>\n<channelId>"
            + ch + "</channelId>\n<uid>" + u + "</uid>\n<searchAITrack>1</searchAITrack>\n<handle>"
            + handle.toUtf8() + "</handle>\n</FileInfo>\n</FileInfoList>\n</body>\n";
        if (!s.requestXml(15, hdr, ext, next, &xml, &status))
            return false;
        const QVector<Recording> got = parseFileList(xml);
        if (got.isEmpty() || status != 200)
            break;
        files += got;
    }
    if (!handle.isEmpty()) {
        const QByteArray close =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<FileInfoList version=\"1.1\">\n<FileInfo>\n<channelId>"
            + ch + "</channelId>\n<uid>" + u + "</uid>\n<handle>" + handle.toUtf8()
            + "</handle>\n</FileInfo>\n</FileInfoList>\n</body>\n";
        s.requestXml(16, hdr, ext, close, nullptr, nullptr, 3000);
    }
    std::sort(files.begin(), files.end(),
              [](const Recording &a, const Recording &b) { return a.start < b.start; });
    *out = files;
    return true;
}

} // namespace rl::bcrec
