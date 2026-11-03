#include "BcSettings.h"

#include "protocol/BcSession.h"
#include "protocol/BcXml.h"

#include <QVariantList>

namespace rl::bcset {

namespace {

using bcxml::Node;

constexpr auto kXmlHead = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n";
constexpr auto kXmlTail = "</body>\n";

QByteArray num(int v)
{
    return QByteArray::number(v);
}

// A channel-scoped request: header channel byte = channel + 1, plus the
// <Extension> with the 0-based channel, as the SDK sends them.
bool request(BcSession &s, int channel, quint32 cmd, const QByteArray &body, QByteArray *reply,
             QString *error)
{
    quint16 status = 0;
    QByteArray xml;
    if (!s.requestXml(cmd, quint8(channel + 1), BcSession::channelExtension(channel), body, &xml,
                      &status)) {
        if (error)
            *error = QStringLiteral("no reply");
        return false;
    }
    if (status != 200) {
        if (error)
            *error = QStringLiteral("refused (status %1)").arg(status);
        return false;
    }
    if (reply)
        *reply = xml;
    return true;
}

QStringList words(const QString &list)
{
    QStringList out;
    for (const QString &w : list.split(QLatin1Char(','), Qt::SkipEmptyParts))
        out << w.trimmed();
    return out;
}

// "people,vehicle" → {"people":1,"vehicle":1,"dog_cat":0,"face":0}
QVariantMap typeFlags(const QString &list)
{
    const QStringList w = words(list);
    QVariantMap m;
    for (const char *k : {"people", "vehicle", "dog_cat", "face"})
        m.insert(QString::fromLatin1(k), w.contains(QLatin1String(k)) ? 1 : 0);
    return m;
}

QString typeList(const QVariantMap &flags, const QString &current)
{
    QStringList w = words(current);
    for (auto it = flags.constBegin(); it != flags.constEnd(); ++it) {
        w.removeAll(it.key());
        if (it.value().toInt())
            w << it.key();
    }
    return w.join(QLatin1Char(','));
}

QVariantMap range(int min, int max)
{
    return {{QStringLiteral("min"), min}, {QStringLiteral("max"), max}};
}

// ---- Get ------------------------------------------------------------------

bool getZoomFocus(BcSession &s, int ch, QVariant *value, QVariant *rng, QString *error)
{
    QByteArray xml;
    if (!request(s, ch, 294, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *zf = root.find(u"PtzZoomFocus");
    if (!zf)
        return false;
    QVariantMap v{{QStringLiteral("channel"), ch}};
    QVariantMap r;
    for (const char *k : {"zoom", "focus"}) {
        const Node *n = zf->child(QString::fromLatin1(k));
        if (!n)
            continue;
        v.insert(QString::fromLatin1(k), QVariantMap{{QStringLiteral("pos"), n->intValue(u"curPos")}});
        r.insert(QString::fromLatin1(k),
                 QVariantMap{{QStringLiteral("pos"), range(n->intValue(u"minPos"), n->intValue(u"maxPos"))}});
    }
    *value = QVariantMap{{QStringLiteral("ZoomFocus"), v}};
    *rng = QVariantMap{{QStringLiteral("ZoomFocus"), r}};
    return true;
}

bool getAutoFocus(BcSession &s, int ch, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!request(s, ch, 224, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *af = root.find(u"AutoFocus");
    if (!af)
        return false;
    *value = QVariantMap{{QStringLiteral("AutoFocus"),
                          QVariantMap{{QStringLiteral("channel"), ch},
                                      {QStringLiteral("disable"), af->intValue(u"disable")}}}};
    return true;
}

bool getPtzPreset(BcSession &s, int ch, QVariant *value, QVariant *rng, QString *error)
{
    QByteArray xml;
    if (!request(s, ch, 190, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *pp = root.find(u"PtzPreset");
    if (!pp)
        return false;
    // The device lists its saved presets; the UI wants every slot, a free one
    // being disabled (as the HTTP API lists them).
    const int max = qMax(1, pp->intValue(u"maxPresetNum", 64));
    QHash<int, QString> saved;
    if (const Node *list = pp->child(u"presetList"))
        for (const Node *p : list->children(u"preset"))
            saved.insert(p->intValue(u"id", -1), p->value(u"name"));
    QVariantList all;
    for (int id = 0; id < max; ++id) {
        const bool used = saved.contains(id);
        all.append(QVariantMap{{QStringLiteral("channel"), ch},
                               {QStringLiteral("id"), id},
                               {QStringLiteral("enable"), used ? 1 : 0},
                               {QStringLiteral("name"), used ? saved.value(id)
                                                             : QStringLiteral("pos%1").arg(id + 1)}});
    }
    *value = QVariantMap{{QStringLiteral("PtzPreset"), all}};
    // Slot 0 is refused for a new preset by the HTTP API of an RLC-823A.
    *rng = QVariantMap{{QStringLiteral("PtzPreset"), QVariantMap{{QStringLiteral("id"), range(1, max - 1)}}}};
    return true;
}

bool getPtzPatrol(BcSession &s, int ch, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!request(s, ch, 64, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *pc = root.find(u"PtzCruise");
    if (!pc)
        return false;
    // <cruiseList><cruise> patrolId, enable, name, keyPosList/keyPos (presetId,
    // speed, dwellTime), as the SDK builds and reads them.
    QVariantList list;
    if (const Node *cl = pc->child(u"cruiseList")) {
        for (const Node *c : cl->children(u"cruise")) {
            QVariantList stops;
            if (const Node *kl = c->child(u"keyPosList"))
                for (const Node *k : kl->children(u"keyPos")) {
                    const int preset = k->intValue(u"presetId", -1);
                    if (preset < 0)
                        continue;
                    stops.append(QVariantMap{{QStringLiteral("id"), preset},
                                             {QStringLiteral("speed"), k->intValue(u"speed", 32)},
                                             {QStringLiteral("dwellTime"), k->intValue(u"dwellTime", 10)}});
                }
            const int id = c->intValue(u"patrolId");
            const QString name = c->value(u"name");
            list.append(QVariantMap{{QStringLiteral("channel"), ch},
                                    {QStringLiteral("id"), id},
                                    {QStringLiteral("enable"), c->intValue(u"enable")},
                                    {QStringLiteral("name"), name.isEmpty() ? QStringLiteral("cruise%1").arg(id + 1) : name},
                                    {QStringLiteral("preset"), stops.isEmpty() ? QVariant() : QVariant(stops)},
                                    {QStringLiteral("running"), c->intValue(u"cruising")}});
        }
    }
    if (list.isEmpty()) // no route yet: the first one, empty
        list.append(QVariantMap{{QStringLiteral("channel"), ch},
                                {QStringLiteral("id"), 0},
                                {QStringLiteral("enable"), 0},
                                {QStringLiteral("name"), QStringLiteral("cruise1")},
                                {QStringLiteral("preset"), QVariant()},
                                {QStringLiteral("running"), pc->intValue(u"cruising")}});
    *value = QVariantMap{{QStringLiteral("PtzPatrol"), list}};
    return true;
}

bool getPtzGuard(BcSession &s, int ch, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!request(s, ch, 332, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *g = root.find(u"PtzGuard");
    if (!g)
        return false;
    *value = QVariantMap{{QStringLiteral("PtzGuard"),
                          QVariantMap{{QStringLiteral("channel"), ch},
                                      {QStringLiteral("benable"), g->intValue(u"benable")},
                                      {QStringLiteral("bexistPos"), g->intValue(u"bvalid")},
                                      {QStringLiteral("timeout"), g->intValue(u"timeout", 60)},
                                      {QStringLiteral("imgName"), g->value(u"imageName")}}}};
    return true;
}

bool getAiCfg(BcSession &s, int ch, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!request(s, ch, 299, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *ai = root.find(u"AiCfg");
    if (!ai)
        return false;
    // HTTP's GetAiCfg fields from <AiCfg>: detectType / smartTrackType lists,
    // smartTrack, and the two delays (identical values on an RLC-823A).
    QVariantMap v{{QStringLiteral("channel"), ch},
                  {QStringLiteral("AiDetectType"), typeFlags(ai->value(u"detectType"))},
                  {QStringLiteral("trackType"), typeFlags(ai->value(u"smartTrackType"))},
                  {QStringLiteral("aiTrack"), ai->child(u"smartTrackModeAbility") ? 1 : 0}};
    if (ai->child(u"smartTrack"))
        v.insert(QStringLiteral("bSmartTrack"), ai->intValue(u"smartTrack"));
    if (ai->child(u"smartTrackObjectStopDelay"))
        v.insert(QStringLiteral("aiStopBackTime"), ai->intValue(u"smartTrackObjectStopDelay"));
    if (ai->child(u"smartTrackObjectDisappearDelay"))
        v.insert(QStringLiteral("aiDisappearBackTime"), ai->intValue(u"smartTrackObjectDisappearDelay"));
    *value = v;
    return true;
}

// ---- Set ------------------------------------------------------------------

bool setZoomFocus(BcSession &s, int ch, const QVariantMap &p, QString *error)
{
    const QVariantMap zf = p.value(QStringLiteral("ZoomFocus")).toMap();
    // <command> carries the HTTP op (ZoomPos / FocusPos), as reolink_aio sends it.
    const QByteArray body = kXmlHead + QByteArray("<StartZoomFocus version=\"1.1\">\n<channelId>") + num(ch)
        + "</channelId>\n<command>" + bcxml::escaped(zf.value(QStringLiteral("op")).toString())
        + "</command>\n<movePos>" + num(zf.value(QStringLiteral("pos")).toInt())
        + "</movePos>\n</StartZoomFocus>\n" + kXmlTail;
    return request(s, ch, 295, body, nullptr, error);
}

bool setAutoFocus(BcSession &s, int ch, const QVariantMap &p, QString *error)
{
    const QVariantMap af = p.value(QStringLiteral("AutoFocus")).toMap();
    const QByteArray body = kXmlHead + QByteArray("<AutoFocus version=\"1.1\">\n<channelId>") + num(ch)
        + "</channelId>\n<disable>" + num(af.value(QStringLiteral("disable")).toInt())
        + "</disable>\n</AutoFocus>\n" + kXmlTail;
    return request(s, ch, 225, body, nullptr, error);
}

bool setPtzPreset(BcSession &s, int ch, const QVariantMap &p, QString *error)
{
    const QVariantMap pp = p.value(QStringLiteral("PtzPreset")).toMap();
    // enable 1: save the current position under this id and name (setPos);
    // enable 0: delete the preset (delPos).
    const bool save = pp.value(QStringLiteral("enable")).toInt() == 1;
    const QByteArray body = kXmlHead + QByteArray("<PtzPreset version=\"1.1\">\n<channelId>") + num(ch)
        + "</channelId>\n<presetList>\n<preset>\n<id>" + num(pp.value(QStringLiteral("id")).toInt())
        + "</id>\n<name>" + bcxml::escaped(pp.value(QStringLiteral("name")).toString())
        + "</name>\n<command>" + (save ? "setPos" : "delPos")
        + "</command>\n</preset>\n</presetList>\n</PtzPreset>\n" + kXmlTail;
    return request(s, ch, 19, body, nullptr, error);
}

bool setPtzGuard(BcSession &s, int ch, const QVariantMap &p, QString *error)
{
    const QVariantMap g = p.value(QStringLiteral("PtzGuard")).toMap();
    QVariant cur;
    if (!getPtzGuard(s, ch, &cur, error))
        return false;
    const QVariantMap now = cur.toMap().value(QStringLiteral("PtzGuard")).toMap();
    const int enable = g.value(QStringLiteral("benable"), now.value(QStringLiteral("benable"), 1)).toInt();
    const int timeout = g.value(QStringLiteral("timeout"), now.value(QStringLiteral("timeout"), 60)).toInt();
    const bool toPos = g.value(QStringLiteral("cmdStr")).toString() == QLatin1String("toPos");
    const QByteArray body = kXmlHead + QByteArray("<PtzGuard version=\"1.1\">\n<channelId>") + num(ch)
        + "</channelId>\n<benable>" + num(enable) + "</benable>\n<command>"
        + (toPos ? "toGrd" : "setGrd") + "</command>\n<timeout>" + num(timeout)
        + "</timeout>\n<needSetPos>" + num(g.value(QStringLiteral("bSaveCurrentPos")).toInt())
        + "</needSetPos>\n<imageName></imageName>\n</PtzGuard>\n" + kXmlTail;
    return request(s, ch, 331, body, nullptr, error);
}

bool setPtzPatrol(BcSession &s, int ch, const QVariantMap &p, QString *error)
{
    const QVariantMap pt = p.value(QStringLiteral("PtzPatrol")).toMap();
    const QVariantList stops = pt.value(QStringLiteral("preset")).toList();
    // The SDK's <PtzCruise> builder (cmd 20): one <cruise> with its stops;
    // valueTable is its 7 × 24 hours schedule, here every hour.
    QByteArray keys;
    int k = 0;
    for (const QVariant &st : stops) {
        const QVariantMap m = st.toMap();
        keys += "<keyPos>\n<keyPosId>" + num(k++) + "</keyPosId>\n<presetId>"
            + num(m.value(QStringLiteral("id")).toInt()) + "</presetId>\n<speed>"
            + num(m.value(QStringLiteral("speed"), 32).toInt()) + "</speed>\n<dwellTime>"
            + num(m.value(QStringLiteral("dwellTime"), 10).toInt())
            + "</dwellTime>\n<trackTime>0</trackTime>\n<trackEnable>0</trackEnable>\n</keyPos>\n";
    }
    const bool enable = !stops.isEmpty();
    QByteArray cruise = "<cruise>\n<patrolId>" + num(pt.value(QStringLiteral("id")).toInt())
        + "</patrolId>\n<enable>" + num(enable ? 1 : 0) + "</enable>\n";
    if (enable)
        cruise += "<name>" + bcxml::escaped(pt.value(QStringLiteral("name")).toString())
            + "</name>\n<valueTable>" + QByteArray(168, '1') + "</valueTable>\n<keyPosList>\n" + keys
            + "</keyPosList>\n";
    cruise += "</cruise>\n";
    const QByteArray body = kXmlHead + QByteArray("<PtzCruise version=\"1.1\">\n<channelId>") + num(ch)
        + "</channelId>\n<cruiseList>\n" + cruise + "</cruiseList>\n</PtzCruise>\n" + kXmlTail;
    return request(s, ch, 20, body, nullptr, error);
}

bool setAiCfg(BcSession &s, int ch, const QVariantMap &p, QString *error)
{
    // Get, change, set (cmd 299 → 300).
    QByteArray xml;
    if (!request(s, ch, 299, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *ai = root.find(u"AiCfg");
    if (!ai)
        return false;
    auto put = [&](const char *httpKey, const char *tag) {
        if (p.contains(QString::fromLatin1(httpKey)))
            bcxml::setTag(xml, tag, p.value(QString::fromLatin1(httpKey)).toString());
    };
    put("bSmartTrack", "smartTrack");
    put("aiTrack", "smartTrack");
    put("aiStopBackTime", "smartTrackObjectStopDelay");
    put("aiDisappearBackTime", "smartTrackObjectDisappearDelay");
    if (p.contains(QStringLiteral("trackType")))
        bcxml::setTag(xml, "smartTrackType",
                      typeList(p.value(QStringLiteral("trackType")).toMap(), ai->value(u"smartTrackType")));
    if (p.contains(QStringLiteral("AiDetectType")))
        bcxml::setTag(xml, "detectType",
                      typeList(p.value(QStringLiteral("AiDetectType")).toMap(), ai->value(u"detectType")));
    return request(s, ch, 300, xml, nullptr, error);
}

} // namespace

bool handles(const QString &command)
{
    static const QSet<QString> mapped = {
        QStringLiteral("GetZoomFocus"), QStringLiteral("StartZoomFocus"),
        QStringLiteral("GetAutoFocus"), QStringLiteral("SetAutoFocus"),
        QStringLiteral("GetPtzPreset"), QStringLiteral("SetPtzPreset"),
        QStringLiteral("GetPtzPatrol"), QStringLiteral("SetPtzPatrol"),
        QStringLiteral("GetPtzGuard"),  QStringLiteral("SetPtzGuard"),
        QStringLiteral("GetAiCfg"),     QStringLiteral("SetAiCfg"),
    };
    return mapped.contains(command);
}

bool get(BcSession &s, int channel, const QString &command, QVariant *value, QVariant *rng,
         QString *error)
{
    *value = QVariant();
    *rng = QVariant();
    if (command == QLatin1String("GetZoomFocus"))
        return getZoomFocus(s, channel, value, rng, error);
    if (command == QLatin1String("GetAutoFocus"))
        return getAutoFocus(s, channel, value, error);
    if (command == QLatin1String("GetPtzPreset"))
        return getPtzPreset(s, channel, value, rng, error);
    if (command == QLatin1String("GetPtzPatrol"))
        return getPtzPatrol(s, channel, value, error);
    if (command == QLatin1String("GetPtzGuard"))
        return getPtzGuard(s, channel, value, error);
    if (command == QLatin1String("GetAiCfg"))
        return getAiCfg(s, channel, value, error);
    if (error)
        *error = QStringLiteral("not mapped");
    return false;
}

bool set(BcSession &s, int channel, const QString &command, const QVariantMap &param,
         QString *error)
{
    if (command == QLatin1String("StartZoomFocus"))
        return setZoomFocus(s, channel, param, error);
    if (command == QLatin1String("SetAutoFocus"))
        return setAutoFocus(s, channel, param, error);
    if (command == QLatin1String("SetPtzPreset"))
        return setPtzPreset(s, channel, param, error);
    if (command == QLatin1String("SetPtzPatrol"))
        return setPtzPatrol(s, channel, param, error);
    if (command == QLatin1String("SetPtzGuard"))
        return setPtzGuard(s, channel, param, error);
    if (command == QLatin1String("SetAiCfg"))
        return setAiCfg(s, channel, param, error);
    if (error)
        *error = QStringLiteral("not mapped");
    return false;
}

bool ptzControl(BcSession &s, int channel, const QString &op, int speed, int patrolId)
{
    const QString command = op.isEmpty() ? op : op.left(1).toLower() + op.mid(1);
    QByteArray body = kXmlHead + QByteArray("<PtzControl version=\"1.1\">\n<channelId>") + num(channel)
        + "</channelId>\n<speed>" + num(qBound(1, speed, 64)) + "</speed>\n<command>"
        + command.toUtf8() + "</command>\n";
    if (patrolId >= 0)
        body += "<patrolId>" + num(patrolId) + "</patrolId>\n";
    body += "</PtzControl>\n" + QByteArray(kXmlTail);
    return request(s, channel, 18, body, nullptr, nullptr);
}

bool ptzToPreset(BcSession &s, int channel, int presetId)
{
    const QByteArray body = kXmlHead + QByteArray("<PtzPreset version=\"1.1\">\n<channelId>") + num(channel)
        + "</channelId>\n<presetList>\n<preset>\n<id>" + num(presetId)
        + "</id>\n<command>toPos</command>\n</preset>\n</presetList>\n</PtzPreset>\n" + kXmlTail;
    return request(s, channel, 19, body, nullptr, nullptr);
}

} // namespace rl::bcset
