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

// ---- Image, display, encoding, siren and speaker -----------------------------

// The SDK writes these modes as words; the HTTP API (and so the UI) as labels.
// Exposure: SDK E_BC_EXPOSURE_TYPE AUTO 0 / LOWNOISE 1 / FRAMERATE ("Anti-
// Smearing") 2 / MANUAL 3 → auto / shutterFirst / gainFist / manual.
const QList<QPair<QString, QString>> kExposure = {
    {QStringLiteral("auto"), QStringLiteral("Auto")},
    {QStringLiteral("shutterFirst"), QStringLiteral("LowNoise")},
    {QStringLiteral("gainFist"), QStringLiteral("Anti-Smearing")},
    {QStringLiteral("manual"), QStringLiteral("Manual")}};
// Anti-flicker: E_BC_ANTIFLICK UNKNOWN 0 (the SDK writes "outdoor") / 50HZ /
// 60HZ; off is <enable>0.
const QList<QPair<QString, QString>> kAntiFlicker = {
    {QStringLiteral("outdoor"), QStringLiteral("Other")},
    {QStringLiteral("50hz"), QStringLiteral("50HZ")},
    {QStringLiteral("60hz"), QStringLiteral("60HZ")}};
// Backlight: E_BC_BLC_MODE CLOSE (<enable>0) / BLC / DRC.
const QList<QPair<QString, QString>> kBacklight = {
    {QStringLiteral("backLight"), QStringLiteral("BackLightControl")},
    {QStringLiteral("dynamicRange"), QStringLiteral("DynamicRangeControl")}};

QString toLabel(const QList<QPair<QString, QString>> &map, const QString &word)
{
    for (const auto &p : map)
        if (p.first.compare(word, Qt::CaseInsensitive) == 0)
            return p.second;
    return word;
}
QString toWord(const QList<QPair<QString, QString>> &map, const QString &label)
{
    for (const auto &p : map)
        if (p.second == label)
            return p.first;
    return {};
}
QString capitalized(const QString &w)
{
    return w.isEmpty() ? w : w.left(1).toUpper() + w.mid(1);
}
QVariant list(const QStringList &l)
{
    return QVariant(l).toList(); // a JSON array for the UI
}
QVariantMap minMax(const Node *n)
{
    return n ? range(n->intValue(u"min"), n->intValue(u"max")) : QVariantMap();
}

bool getIsp(BcSession &s, int ch, QVariant *value, QVariant *rng, QString *error)
{
    QByteArray xml;
    if (!request(s, ch, 26, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *in = root.find(u"InputAdvanceCfg");
    if (!in)
        return false;
    QVariantMap v{{QStringLiteral("channel"), ch}};
    QVariantMap r;
    if (const Node *pl = in->child(u"PowerLineFrequency")) {
        v.insert(QStringLiteral("antiFlicker"), pl->intValue(u"enable", 1) == 0
                                                    ? QStringLiteral("Off")
                                                    : toLabel(kAntiFlicker, pl->value(u"mode")));
        r.insert(QStringLiteral("antiFlicker"), list(QStringList{QStringLiteral("Other"), QStringLiteral("50HZ"),
                                                            QStringLiteral("60HZ"), QStringLiteral("Off")}));
    }
    if (const Node *ex = in->child(u"Exposure")) {
        v.insert(QStringLiteral("exposure"), toLabel(kExposure, ex->value(u"mode")));
        QStringList labels;
        for (const auto &p : kExposure)
            labels << p.second;
        r.insert(QStringLiteral("exposure"), list(labels));
    }
    if (const Node *sc = in->child(u"Scene")) {
        v.insert(QStringLiteral("whiteBalance"), capitalized(sc->value(u"mode")));
        QStringList modes;
        for (const QString &m : words(sc->value(u"modeList")))
            modes << capitalized(m);
        if (!modes.isEmpty())
            r.insert(QStringLiteral("whiteBalance"), list(modes));
        if (const Node *g = sc->child(u"Redgain")) {
            v.insert(QStringLiteral("redGain"), g->intValue(u"cur"));
            r.insert(QStringLiteral("redGain"), minMax(g));
        }
        if (const Node *g = sc->child(u"Bluegain")) {
            v.insert(QStringLiteral("blueGain"), g->intValue(u"cur"));
            r.insert(QStringLiteral("blueGain"), minMax(g));
        }
    }
    if (const Node *blc = in->child(u"BLC")) {
        v.insert(QStringLiteral("backLight"), blc->intValue(u"enable") == 0
                                                  ? QStringLiteral("Off")
                                                  : toLabel(kBacklight, blc->value(u"mode")));
        r.insert(QStringLiteral("backLight"), list(QStringList{QStringLiteral("Off"), QStringLiteral("BackLightControl"),
                                                          QStringLiteral("DynamicRangeControl")}));
        if (const Node *b = blc->child(u"backlight")) {
            v.insert(QStringLiteral("blc"), b->intValue(u"cur"));
            r.insert(QStringLiteral("blc"), minMax(b));
        }
        if (const Node *d = blc->child(u"dynamicrange")) {
            v.insert(QStringLiteral("drc"), d->intValue(u"cur"));
            r.insert(QStringLiteral("drc"), minMax(d));
        }
    }
    if (const Node *nr = in->child(u"nr3d"))
        v.insert(QStringLiteral("nr3d"), nr->intValue(u"enable"));
    *value = QVariantMap{{QStringLiteral("Isp"), v}};
    *rng = QVariantMap{{QStringLiteral("Isp"), r}};
    return true;
}

bool setIsp(BcSession &s, int ch, const QVariantMap &p, QString *error)
{
    const QVariantMap isp = p.value(QStringLiteral("Isp")).toMap();
    QByteArray xml;
    if (!request(s, ch, 26, {}, &xml, error))
        return false;
    bool ok = true;
    auto has = [&](const char *k) { return isp.contains(QString::fromLatin1(k)); };
    auto str = [&](const char *k) { return isp.value(QString::fromLatin1(k)).toString(); };
    if (has("antiFlicker")) {
        const QString v = str("antiFlicker");
        ok &= bcxml::setTagIn(xml, "PowerLineFrequency", "enable", v == QLatin1String("Off") ? QStringLiteral("0")
                                                                                              : QStringLiteral("1"));
        if (v != QLatin1String("Off"))
            ok &= bcxml::setTagIn(xml, "PowerLineFrequency", "mode", toWord(kAntiFlicker, v));
    }
    if (has("exposure"))
        ok &= bcxml::setTagIn(xml, "Exposure", "mode", toWord(kExposure, str("exposure")));
    if (has("whiteBalance"))
        ok &= bcxml::setTagIn(xml, "Scene", "mode", str("whiteBalance").toLower());
    if (has("redGain"))
        ok &= bcxml::setTagIn(xml, "Redgain", "cur", str("redGain"));
    if (has("blueGain"))
        ok &= bcxml::setTagIn(xml, "Bluegain", "cur", str("blueGain"));
    if (has("backLight")) {
        const QString v = str("backLight");
        ok &= bcxml::setTagIn(xml, "BLC", "enable", v == QLatin1String("Off") ? QStringLiteral("0")
                                                                              : QStringLiteral("1"));
        if (v != QLatin1String("Off"))
            ok &= bcxml::setTagIn(xml, "BLC", "mode", toWord(kBacklight, v));
    }
    if (has("blc"))
        ok &= bcxml::setTagIn(xml, "backlight", "cur", str("blc"));
    if (has("drc"))
        ok &= bcxml::setTagIn(xml, "dynamicrange", "cur", str("drc"));
    if (has("nr3d"))
        ok &= bcxml::setTagIn(xml, "nr3d", "enable", str("nr3d"));
    if (!ok) {
        if (error)
            *error = QStringLiteral("this camera has no such setting");
        return false;
    }
    return request(s, ch, 25, xml, nullptr, error);
}

// IR lights and the status LED (cmd 208/209 <LedState>): state auto / close /
// open (IR), lightState open / close (status LED), as the SDK writes them.
bool getLed(BcSession &s, int ch, bool power, QVariant *value, QVariant *rng, QString *error)
{
    QByteArray xml;
    if (!request(s, ch, 208, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *led = root.find(u"LedState");
    if (!led)
        return false;
    if (power) {
        if (!led->child(u"lightState"))
            return false;
        const bool on = led->value(u"lightState") == QLatin1String("open");
        *value = QVariantMap{{QStringLiteral("PowerLed"),
                              QVariantMap{{QStringLiteral("channel"), ch},
                                          {QStringLiteral("state"), on ? QStringLiteral("On") : QStringLiteral("Off")}}}};
        *rng = QVariantMap{{QStringLiteral("PowerLed"),
                            QVariantMap{{QStringLiteral("state"), list(QStringList{QStringLiteral("On"), QStringLiteral("Off")})}}}};
        return true;
    }
    if (!led->child(u"state"))
        return false;
    const QString st = led->value(u"state");
    *value = QVariantMap{{QStringLiteral("IrLights"),
                          QVariantMap{{QStringLiteral("channel"), ch},
                                      {QStringLiteral("state"), st == QLatin1String("auto") ? QStringLiteral("Auto")
                                                               : st == QLatin1String("open") ? QStringLiteral("On")
                                                                                             : QStringLiteral("Off")}}}};
    *rng = QVariantMap{{QStringLiteral("IrLights"),
                        QVariantMap{{QStringLiteral("state"), list(QStringList{QStringLiteral("Auto"), QStringLiteral("Off")})}}}};
    return true;
}

bool setLed(BcSession &s, int ch, bool power, const QVariantMap &p, QString *error)
{
    const QString state = p.value(power ? QStringLiteral("PowerLed") : QStringLiteral("IrLights"))
                              .toMap().value(QStringLiteral("state")).toString();
    QByteArray xml;
    if (!request(s, ch, 208, {}, &xml, error))
        return false;
    const bool ok = power ? bcxml::setTag(xml, "lightState", state == QLatin1String("On") ? QStringLiteral("open")
                                                                                           : QStringLiteral("close"))
                          : bcxml::setTag(xml, "state", state == QLatin1String("Auto") ? QStringLiteral("auto")
                                                        : state == QLatin1String("On") ? QStringLiteral("open")
                                                                                       : QStringLiteral("close"));
    if (!ok)
        return false;
    return request(s, ch, 209, xml, nullptr, error);
}

// On-screen display (cmd 44/45): the camera name and the date/time.
bool getOsd(BcSession &s, int ch, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!request(s, ch, 44, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *name = root.find(u"OsdChannelName");
    const Node *time = root.find(u"OsdDatetime");
    if (!name && !time)
        return false;
    QVariantMap v{{QStringLiteral("channel"), ch}};
    if (name) {
        v.insert(QStringLiteral("osdChannel"), QVariantMap{{QStringLiteral("enable"), name->intValue(u"enable")},
                                                           {QStringLiteral("name"), name->value(u"name")}});
        v.insert(QStringLiteral("watermark"), name->intValue(u"enWatermark"));
        v.insert(QStringLiteral("bgcolor"), name->intValue(u"enBgcolor"));
    }
    if (time)
        v.insert(QStringLiteral("osdTime"), QVariantMap{{QStringLiteral("enable"), time->intValue(u"enable")}});
    *value = QVariantMap{{QStringLiteral("Osd"), v}};
    return true;
}

bool setOsd(BcSession &s, int ch, const QVariantMap &p, QString *error)
{
    const QVariantMap osd = p.value(QStringLiteral("Osd")).toMap();
    QByteArray xml;
    if (!request(s, ch, 44, {}, &xml, error))
        return false;
    bool ok = true;
    const QVariantMap name = osd.value(QStringLiteral("osdChannel")).toMap();
    if (name.contains(QStringLiteral("enable")))
        ok &= bcxml::setTagIn(xml, "OsdChannelName", "enable", name.value(QStringLiteral("enable")).toString());
    if (name.contains(QStringLiteral("name")))
        ok &= bcxml::setTagIn(xml, "OsdChannelName", "name", name.value(QStringLiteral("name")).toString());
    const QVariantMap time = osd.value(QStringLiteral("osdTime")).toMap();
    if (time.contains(QStringLiteral("enable")))
        ok &= bcxml::setTagIn(xml, "OsdDatetime", "enable", time.value(QStringLiteral("enable")).toString());
    if (!ok)
        return false;
    return request(s, ch, 45, xml, nullptr, error);
}

// Encoding (cmd 56/57 <Compression>). The codec is not in the reply: it is
// in each frame's header, so vType is left out.
bool getEnc(BcSession &s, int ch, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!request(s, ch, 56, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *c = root.find(u"Compression");
    if (!c)
        return false;
    QVariantMap v{{QStringLiteral("channel"), ch}};
    for (const char *k : {"mainStream", "subStream"}) {
        const Node *st = c->child(QString::fromLatin1(k));
        if (!st)
            continue;
        if (!v.contains(QStringLiteral("audio")))
            v.insert(QStringLiteral("audio"), st->intValue(u"audio"));
        v.insert(QString::fromLatin1(k),
                 QVariantMap{{QStringLiteral("bitRate"), st->intValue(u"bitRate")},
                             {QStringLiteral("frameRate"), st->intValue(u"frame")},
                             {QStringLiteral("size"), st->value(u"resolutionName")},
                             {QStringLiteral("width"), st->intValue(u"width")},
                             {QStringLiteral("height"), st->intValue(u"height")}});
    }
    *value = QVariantMap{{QStringLiteral("Enc"), v}};
    return true;
}

bool setEnc(BcSession &s, int ch, const QVariantMap &p, QString *error)
{
    const QVariantMap enc = p.value(QStringLiteral("Enc")).toMap();
    QByteArray xml;
    if (!request(s, ch, 56, {}, &xml, error))
        return false;
    if (enc.contains(QStringLiteral("audio"))) {
        const QString a = enc.value(QStringLiteral("audio")).toString();
        bcxml::setTagIn(xml, "mainStream", "audio", a); // one switch for both streams
        bcxml::setTagIn(xml, "subStream", "audio", a);
    }
    return request(s, ch, 57, xml, nullptr, error);
}

// Siren on alarm (cmd 232/231 <AudioTask>): enable, and its per-type schedules.
bool getAudioAlarm(BcSession &s, int ch, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!request(s, ch, 232, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *t = root.find(u"AudioTask");
    if (!t)
        return false;
    QVariantMap table;
    if (const Node *list = t->child(u"typeScheduleList"))
        for (const Node *item : list->children(u"item")) {
            const QString type = item->value(u"type");
            const QString key = type == QLatin1String("MD") ? type : QStringLiteral("AI_") + type.toUpper();
            table.insert(key, item->value(u"valueTable"));
        }
    *value = QVariantMap{{QStringLiteral("Audio"),
                          QVariantMap{{QStringLiteral("enable"), t->intValue(u"enable")},
                                      {QStringLiteral("schedule"),
                                       QVariantMap{{QStringLiteral("channel"), ch}, {QStringLiteral("table"), table}}}}}};
    return true;
}

bool setAudioAlarm(BcSession &s, int ch, const QVariantMap &p, QString *error)
{
    const QVariantMap a = p.value(QStringLiteral("Audio")).toMap();
    QByteArray xml;
    if (!request(s, ch, 232, {}, &xml, error))
        return false;
    if (a.contains(QStringLiteral("enable")))
        bcxml::setTagIn(xml, "AudioTask", "enable", a.value(QStringLiteral("enable")).toString());
    return request(s, ch, 231, xml, nullptr, error);
}

// Speaker volume (cmd 264/265 <audioCfg>).
bool getAudioCfg(BcSession &s, int ch, QVariant *value, QVariant *rng, QString *error)
{
    QByteArray xml;
    if (!request(s, ch, 264, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *a = root.find(u"audioCfg");
    if (!a)
        return false;
    *value = QVariantMap{{QStringLiteral("AudioCfg"),
                          QVariantMap{{QStringLiteral("channel"), ch}, {QStringLiteral("volume"), a->intValue(u"volume")}}}};
    *rng = QVariantMap{{QStringLiteral("AudioCfg"), QVariantMap{{QStringLiteral("volume"), range(0, 100)}}}};
    return true;
}

bool setAudioCfg(BcSession &s, int ch, const QVariantMap &p, QString *error)
{
    const QVariantMap a = p.value(QStringLiteral("AudioCfg")).toMap();
    QByteArray xml;
    if (!request(s, ch, 264, {}, &xml, error))
        return false;
    if (a.contains(QStringLiteral("volume")))
        bcxml::setTag(xml, "volume", a.value(QStringLiteral("volume")).toString());
    return request(s, ch, 265, xml, nullptr, error);
}

// Floodlight (cmd 289/290 <FloodlightTask>; on/off with cmd 288
// <FloodlightManual>; its state is pushed with cmd 291). Modes as reolink_aio
// maps them: the same numbers for 0–3, then Baichuan 5 → HTTP 4, 6 → 5.
int whiteLedModeToHttp(int bc)
{
    return bc == 5 ? 4 : bc == 6 ? 5 : bc;
}
int whiteLedModeToBc(int http)
{
    return http == 4 ? 5 : http == 5 ? 6 : http;
}

bool getWhiteLed(BcSession &s, int ch, QVariant *value, QVariant *rng, QString *error)
{
    QByteArray xml;
    if (!request(s, ch, 289, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *f = root.find(u"FloodlightTask");
    if (!f)
        return false;
    int state = 0;
    BcMessage push;
    if (s.lastPush(291, &push)) {
        const Node pr = bcxml::parse(s.bodyXml(push));
        if (const Node *list = pr.find(u"FloodlightStatusList"))
            for (const Node *st : list->children(u"FloodlightStatus"))
                if (st->intValue(u"channel", -1) == ch)
                    state = st->intValue(u"status");
    }
    QVariantMap v{{QStringLiteral("channel"), ch},
                  {QStringLiteral("bright"), f->intValue(u"brightness_cur")},
                  {QStringLiteral("mode"), whiteLedModeToHttp(f->intValue(u"alarmMode"))},
                  {QStringLiteral("state"), state}};
    if (const Node *sch = f->child(u"schedule"))
        v.insert(QStringLiteral("LightingSchedule"),
                 QVariantMap{{QStringLiteral("StartHour"), sch->intValue(u"startHour")},
                             {QStringLiteral("StartMin"), sch->intValue(u"startMin")},
                             {QStringLiteral("EndHour"), sch->intValue(u"endHour")},
                             {QStringLiteral("EndMin"), sch->intValue(u"endMin")}});
    if (f->child(u"detectType")) {
        QVariantMap t = typeFlags(f->value(u"detectType"));
        t.remove(QStringLiteral("face"));
        v.insert(QStringLiteral("wlAiDetectType"), t);
    }
    *value = QVariantMap{{QStringLiteral("WhiteLed"), v}};
    *rng = QVariantMap{{QStringLiteral("WhiteLed"),
                        QVariantMap{{QStringLiteral("bright"),
                                     range(f->intValue(u"brightness_min", 0), f->intValue(u"brightness_max", 100))}}}};
    return true;
}

bool switchFloodlight(BcSession &s, int ch, bool on, QString *error)
{
    const QByteArray body = kXmlHead + QByteArray("<FloodlightManual version=\"1.1\">\n<channelId>") + num(ch)
        + "</channelId>\n<status>" + num(on ? 1 : 0) + "</status>\n<duration>180</duration>\n</FloodlightManual>\n"
        + kXmlTail;
    return request(s, ch, 288, body, nullptr, error);
}

bool setWhiteLed(BcSession &s, int ch, const QVariantMap &p, QString *error)
{
    const QVariantMap w = p.value(QStringLiteral("WhiteLed")).toMap();
    const bool settings = w.contains(QStringLiteral("mode")) || w.contains(QStringLiteral("bright"))
                          || w.contains(QStringLiteral("LightingSchedule"))
                          || w.contains(QStringLiteral("wlAiDetectType"));
    // The settings page sends the current on/off state with every change: only
    // a write of the state alone switches the light.
    if (!settings)
        return switchFloodlight(s, ch, w.value(QStringLiteral("state")).toInt() == 1, error);
    QByteArray xml;
    if (!request(s, ch, 289, {}, &xml, error))
        return false;
    if (w.contains(QStringLiteral("mode"))) {
        const QString m = QString::number(whiteLedModeToBc(w.value(QStringLiteral("mode")).toInt()));
        bcxml::setTagIn(xml, "FloodlightTask", "alarmMode", m);
        bcxml::setTagIn(xml, "FloodlightTask", "enable", m);
    }
    if (w.contains(QStringLiteral("bright")))
        bcxml::setTag(xml, "brightness_cur", w.value(QStringLiteral("bright")).toString());
    if (w.contains(QStringLiteral("LightingSchedule"))) {
        const QVariantMap sch = w.value(QStringLiteral("LightingSchedule")).toMap();
        bcxml::setTagIn(xml, "schedule", "startHour", sch.value(QStringLiteral("StartHour")).toString());
        bcxml::setTagIn(xml, "schedule", "startMin", sch.value(QStringLiteral("StartMin")).toString());
        bcxml::setTagIn(xml, "schedule", "endHour", sch.value(QStringLiteral("EndHour")).toString());
        bcxml::setTagIn(xml, "schedule", "endMin", sch.value(QStringLiteral("EndMin")).toString());
    }
    if (w.contains(QStringLiteral("wlAiDetectType"))) {
        const Node root = bcxml::parse(xml);
        const Node *f = root.find(u"FloodlightTask");
        bcxml::setTag(xml, "detectType", typeList(w.value(QStringLiteral("wlAiDetectType")).toMap(),
                                                  f ? f->value(u"detectType") : QString()));
    }
    return request(s, ch, 290, xml, nullptr, error);
}

// Siren (cmd 263 <audioPlayInfo>, as reolink_aio): play mode 0 = so many
// times, 2 = on/off by hand.
bool playSiren(BcSession &s, const QVariantMap &p, QString *error)
{
    const int ch = p.value(QStringLiteral("channel")).toInt();
    const bool times = p.value(QStringLiteral("alarm_mode"), QStringLiteral("times")).toString()
                       == QLatin1String("times");
    const QByteArray body = kXmlHead + QByteArray("<audioPlayInfo version=\"1.1\">\n<channelId>") + num(ch)
        + "</channelId>\n<playMode>" + (times ? "0" : "2") + "</playMode>\n<playDuration>10</playDuration>\n"
          "<playTimes>" + num(times ? qMax(1, p.value(QStringLiteral("times"), 1).toInt()) : 1)
        + "</playTimes>\n<onOff>" + num(times ? 1 : p.value(QStringLiteral("manual_switch"), 1).toInt())
        + "</onOff>\n</audioPlayInfo>\n" + kXmlTail;
    return request(s, ch, 263, body, nullptr, error);
}

// Doorbell quick reply (cmd 349 <audioFileInfo>, as reolink_aio).
bool quickReply(BcSession &s, const QVariantMap &p, QString *error)
{
    const int ch = p.value(QStringLiteral("channel")).toInt();
    const QByteArray body = kXmlHead + QByteArray("<audioFileInfo version=\"1.1\">\n<channelId>") + num(ch)
        + "</channelId>\n<id>" + num(p.value(QStringLiteral("id")).toInt())
        + "</id>\n<timeout>0</timeout>\n</audioFileInfo>\n" + kXmlTail;
    return request(s, ch, 349, body, nullptr, error);
}

// Reboot (cmd 23, host level).
bool reboot(BcSession &s, QString *error)
{
    quint16 status = 0;
    if (!s.requestXml(23, 250, {}, {}, nullptr, &status, 12000)) {
        if (error)
            *error = QStringLiteral("no reply");
        return false;
    }
    if (status != 200 && error)
        *error = QStringLiteral("refused (status %1)").arg(status);
    return status == 200;
}
// ---- Network, storage, users, time, system ---------------------------------

// A host-level request: header channel byte 250, no channel extension.
bool requestHost(BcSession &s, quint32 cmd, const QByteArray &ext, const QByteArray &body,
                 QByteArray *reply, QString *error, int timeoutMs = 12000)
{
    quint16 status = 0;
    QByteArray xml;
    if (!s.requestXml(cmd, 250, ext, body, &xml, &status, timeoutMs)) {
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

bool getLocalLink(BcSession &s, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!requestHost(s, 76, {}, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *ip = root.find(u"Ip");
    if (!ip)
        return false;
    const Node *dhcp = root.find(u"Dhcp");
    const Node *autoDns = root.find(u"AutoDns");
    const Node *dns = root.find(u"Dns");
    QString link = QStringLiteral("LAN");
    QByteArray lt;
    if (requestHost(s, 93, {}, {}, &lt, nullptr)) {
        const Node lr = bcxml::parse(lt);
        if (const Node *t = lr.find(u"LinkType"))
            link = t->value(u"type");
    }
    *value = QVariantMap{{QStringLiteral("LocalLink"),
                          QVariantMap{{QStringLiteral("activeLink"), link},
                                      {QStringLiteral("mac"), ip->value(u"mac")},
                                      {QStringLiteral("type"), dhcp && dhcp->intValue(u"enable")
                                                                   ? QStringLiteral("DHCP") : QStringLiteral("Static")},
                                      {QStringLiteral("static"),
                                       QVariantMap{{QStringLiteral("ip"), ip->value(u"ip")},
                                                   {QStringLiteral("mask"), ip->value(u"mask")},
                                                   {QStringLiteral("gateway"), ip->value(u"gateway")}}},
                                      {QStringLiteral("dns"),
                                       QVariantMap{{QStringLiteral("auto"), autoDns ? autoDns->intValue(u"enable") : 0},
                                                   {QStringLiteral("dns1"), dns ? dns->value(u"dns1") : QString()},
                                                   {QStringLiteral("dns2"), dns ? dns->value(u"dns2") : QString()}}}}}};
    return true;
}

bool getNetPort(BcSession &s, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!requestHost(s, 37, {}, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    QVariantMap v;
    // <HttpPort><httpPort>80</httpPort><enable>1</enable></HttpPort> …
    const QList<QPair<const char *, const char *>> blocks = {
        {"HttpPort", "http"}, {"HttpsPort", "https"}, {"RtspPort", "rtsp"},
        {"RtmpPort", "rtmp"}, {"OnvifPort", "onvif"}};
    for (const auto &b : blocks) {
        const Node *n = root.find(QString::fromLatin1(b.first));
        if (!n)
            continue;
        const QString k = QString::fromLatin1(b.second);
        v.insert(k + QStringLiteral("Port"), n->intValue(k + QStringLiteral("Port")));
        v.insert(k + QStringLiteral("Enable"), n->intValue(u"enable"));
    }
    if (const Node *sp = root.find(u"ServerPort"))
        v.insert(QStringLiteral("mediaPort"), sp->intValue(u"serverPort"));
    *value = QVariantMap{{QStringLiteral("NetPort"), v}};
    return true;
}

bool getHddInfo(BcSession &s, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!requestHost(s, 102, {}, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *list = root.find(u"HddInfoList");
    if (!list)
        return false;
    // Sizes come as GB plus MB (capacity / capacityM, remainSize / remainSizeM);
    // the UI wants MB, as the HTTP API gave them.
    QVariantList disks;
    for (const Node *h : list->children(u"HddInfo")) {
        QVariantMap d{{QStringLiteral("number"), h->intValue(u"number")},
                      {QStringLiteral("capacity"), h->intValue(u"capacity") * 1024 + h->intValue(u"capacityM")},
                      {QStringLiteral("size"), h->intValue(u"remainSize") * 1024 + h->intValue(u"remainSizeM")},
                      {QStringLiteral("format"), h->intValue(u"format")},
                      {QStringLiteral("mount"), h->intValue(u"mount")}};
        if (h->child(u"storageType"))
            d.insert(QStringLiteral("storageType"), h->intValue(u"storageType"));
        disks.append(d);
    }
    *value = QVariantMap{{QStringLiteral("HddInfo"), disks}};
    return true;
}

QByteArray userExtension(const QString &user)
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<Extension version=\"1.1\">\n<userName>"
           + bcxml::escaped(user) + "</userName>\n</Extension>\n";
}

bool getUsers(BcSession &s, const QString &me, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!requestHost(s, 58, userExtension(me), {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *list = root.find(u"UserList");
    if (!list)
        return false;
    QVariantList users;
    for (const Node *u : list->children(u"User"))
        users.append(QVariantMap{{QStringLiteral("userName"), u->value(u"userName")},
                                 {QStringLiteral("level"), u->intValue(u"userLevel") == 1
                                                               ? QStringLiteral("admin") : QStringLiteral("guest")}});
    *value = QVariantMap{{QStringLiteral("User"), users}};
    return true;
}

// Users (cmd 59 <UserList><User>, as the SDK's builders write it):
// userSetState add / delete / modify; a password change sends no old one.
bool setUser(BcSession &s, const QString &me, const QString &command, const QVariantMap &p, QString *error)
{
    const QVariantMap u = p.value(QStringLiteral("User")).toMap();
    const QString name = u.value(QStringLiteral("userName")).toString();
    QByteArray user = "<userName>" + bcxml::escaped(name) + "</userName>\n";
    QByteArray state;
    if (command == QLatin1String("AddUser")) {
        user += "<password>" + bcxml::escaped(u.value(QStringLiteral("password")).toString())
              + "</password>\n<userLevel>"
              + (u.value(QStringLiteral("level")).toString() == QLatin1String("admin") ? "1" : "0")
              + "</userLevel>\n";
        state = "add";
    } else if (command == QLatin1String("ModifyUser")) {
        user += "<password>" + bcxml::escaped(u.value(QStringLiteral("newPassword")).toString()) + "</password>\n";
        state = "modify";
    } else {
        state = "delete";
    }
    const QByteArray body = kXmlHead + QByteArray("<UserList version=\"1.1\">\n<User>\n") + user
        + "<userSetState>" + state + "</userSetState>\n</User>\n</UserList>\n" + kXmlTail;
    return requestHost(s, 59, userExtension(me), body, nullptr, error);
}

bool getTime(BcSession &s, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!requestHost(s, 104, {}, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *g = root.find(u"SystemGeneral");
    if (!g)
        return false;
    const QString osd = g->value(u"osdFormat");
    const QString fmt = osd == QLatin1String("DMY") ? QStringLiteral("DD/MM/YYYY")
                      : osd == QLatin1String("YMD") ? QStringLiteral("YYYY/MM/DD")
                                                    : QStringLiteral("MM/DD/YYYY");
    QVariantMap v{{QStringLiteral("Time"),
                   QVariantMap{{QStringLiteral("year"), g->intValue(u"year")},
                               {QStringLiteral("mon"), g->intValue(u"month")},
                               {QStringLiteral("day"), g->intValue(u"day")},
                               {QStringLiteral("hour"), g->intValue(u"hour")},
                               {QStringLiteral("min"), g->intValue(u"minute")},
                               {QStringLiteral("sec"), g->intValue(u"second")},
                               {QStringLiteral("timeZone"), g->intValue(u"timeZone")},
                               {QStringLiteral("timeFmt"), fmt},
                               {QStringLiteral("hourFmt"), g->intValue(u"timeFormat")}}}};
    QByteArray dst;
    if (requestHost(s, 106, {}, {}, &dst, nullptr)) {
        const Node dr = bcxml::parse(dst);
        if (const Node *d = dr.find(u"Dst")) {
            static const QStringList days = {QStringLiteral("Sunday"), QStringLiteral("Monday"),
                                             QStringLiteral("Tuesday"), QStringLiteral("Wednesday"),
                                             QStringLiteral("Thursday"), QStringLiteral("Friday"),
                                             QStringLiteral("Saturday")};
            v.insert(QStringLiteral("Dst"),
                     QVariantMap{{QStringLiteral("enable"), d->intValue(u"enable")},
                                 {QStringLiteral("offset"), d->intValue(u"offset")},
                                 {QStringLiteral("startMon"), d->intValue(u"startMonth")},
                                 {QStringLiteral("startWeek"), d->intValue(u"startWeekIndex")},
                                 {QStringLiteral("startWeekday"), qMax(0, days.indexOf(d->value(u"startWeekday")))},
                                 {QStringLiteral("startHour"), d->intValue(u"startHour")},
                                 {QStringLiteral("startMin"), d->intValue(u"startMinute")},
                                 {QStringLiteral("startSec"), d->intValue(u"startSecond")},
                                 {QStringLiteral("endMon"), d->intValue(u"endMonth")},
                                 {QStringLiteral("endWeek"), d->intValue(u"endWeekIndex")},
                                 {QStringLiteral("endWeekday"), qMax(0, days.indexOf(d->value(u"endWeekday")))},
                                 {QStringLiteral("endHour"), d->intValue(u"endHour")},
                                 {QStringLiteral("endMin"), d->intValue(u"endMinute")},
                                 {QStringLiteral("endSec"), d->intValue(u"endSecond")}});
        }
    }
    *value = v;
    return true;
}

bool getNtp(BcSession &s, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!requestHost(s, 38, {}, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *n = root.find(u"Ntp");
    if (!n)
        return false;
    *value = QVariantMap{{QStringLiteral("Ntp"),
                          QVariantMap{{QStringLiteral("enable"), n->intValue(u"enable")},
                                      {QStringLiteral("server"), n->value(u"server")},
                                      {QStringLiteral("port"), n->intValue(u"port")},
                                      {QStringLiteral("interval"), n->intValue(u"synchronizeInterval")}}}};
    return true;
}

bool setNtp(BcSession &s, const QVariantMap &p, QString *error)
{
    const QVariantMap n = p.value(QStringLiteral("Ntp")).toMap();
    QByteArray xml;
    if (!requestHost(s, 38, {}, {}, &xml, error))
        return false;
    const QList<QPair<const char *, const char *>> fields = {
        {"enable", "enable"}, {"server", "server"}, {"port", "port"}, {"interval", "synchronizeInterval"}};
    for (const auto &f : fields)
        if (n.contains(QString::fromLatin1(f.first)))
            bcxml::setTagIn(xml, "Ntp", f.second, n.value(QString::fromLatin1(f.first)).toString());
    return requestHost(s, 39, {}, xml, nullptr, error);
}

bool getDevInfo(BcSession &s, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!requestHost(s, 80, {}, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *v = root.find(u"VersionInfo");
    if (!v)
        return false;
    const Node login = bcxml::parse(s.loginInfo());
    const Node *di = login.find(u"DeviceInfo");
    *value = QVariantMap{{QStringLiteral("DevInfo"),
                          QVariantMap{{QStringLiteral("name"), v->value(u"name")},
                                      {QStringLiteral("model"), v->value(u"type")},
                                      {QStringLiteral("firmVer"), v->value(u"firmwareVersion")},
                                      {QStringLiteral("hardVer"), v->value(u"hardwareVersion")},
                                      {QStringLiteral("serial"), v->value(u"serialNumber")},
                                      {QStringLiteral("buildDay"), v->value(u"buildDay")},
                                      {QStringLiteral("cfgVer"), v->value(u"cfgVersion")},
                                      {QStringLiteral("detail"), v->value(u"detail")},
                                      {QStringLiteral("channelNum"), di ? di->intValue(u"channelNum", 1) : 1}}}};
    return true;
}

bool getAutoUpgrade(BcSession &s, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!requestHost(s, 195, {}, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *a = root.find(u"AutoUpdate");
    if (!a)
        return false;
    *value = QVariantMap{{QStringLiteral("AutoUpgrade"), QVariantMap{{QStringLiteral("enable"), a->intValue(u"enable")}}}};
    return true;
}

bool setAutoUpgrade(BcSession &s, const QVariantMap &p, QString *error)
{
    const int enable = p.value(QStringLiteral("AutoUpgrade")).toMap().value(QStringLiteral("enable")).toInt();
    const QByteArray body = kXmlHead + QByteArray("<AutoUpdate version=\"1.1\">\n<enable>") + num(enable)
        + "</enable>\n</AutoUpdate>\n" + kXmlTail;
    return requestHost(s, 196, {}, body, nullptr, error);
}

// Firmware: is there a newer one online (197), install it (191), progress (227).
bool checkFirmware(BcSession &s, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!requestHost(s, 197, {}, {}, &xml, error, 30000))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *f = root.find(u"OnlineNewFirmwareInfo");
    if (!f)
        return false;
    *value = QVariantMap{{QStringLiteral("newFirmware"), f->intValue(u"hasNewFirmware")}};
    return true;
}

bool upgradeOnline(BcSession &s, QString *error)
{
    const QByteArray body = kXmlHead + QByteArray("<OnlineUpdate version=\"1.1\">\n<needUpdate>1</needUpdate>\n"
                                                  "</OnlineUpdate>\n") + kXmlTail;
    return requestHost(s, 191, {}, body, nullptr, error);
}

bool upgradeStatus(BcSession &s, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!requestHost(s, 227, {}, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *st = root.find(u"upgradeState");
    if (!st)
        return false;
    const int total = st->intValue(u"packetSize");
    const int done = st->intValue(u"downloadSize");
    *value = QVariantMap{{QStringLiteral("Status"),
                          QVariantMap{{QStringLiteral("Persent"), total > 0 ? done * 100 / total : 0},
                                      {QStringLiteral("state"), st->value(u"state")}}}};
    return true;
}

// Format a disk (cmd 103 INIT_HDD, the SDK's BCSDK_RemoteInitHdd).
bool formatDisk(BcSession &s, const QVariantMap &p, QString *error)
{
    QByteArray disks;
    for (const QVariant &id : p.value(QStringLiteral("HddInfo")).toMap().value(QStringLiteral("id")).toList())
        disks += "<HddInit>\n<initId>" + num(id.toInt()) + "</initId>\n</HddInit>\n";
    const QByteArray body = kXmlHead + QByteArray("<HddInitList version=\"1.1\">\n") + disks + "</HddInitList>\n"
        + kXmlTail;
    return requestHost(s, 103, {}, body, nullptr, error, 60000);
}
// Privacy masks (cmd 52/53 <Shelter>). Each coordinate packs the HTTP value in
// its high 16 bits and the reference screen size in its low 16 bits (width
// for x and width, height for y and height) — read off a mask drawn on an
// RLC-823A: x 1781 on a 3840 wide screen = 1781 × 65536 + 3840.
bool getMask(BcSession &s, int ch, QVariant *value, QString *error)
{
    QByteArray xml;
    if (!request(s, ch, 52, {}, &xml, error))
        return false;
    const Node root = bcxml::parse(xml);
    const Node *top = root.find(u"Shelter");
    if (!top)
        return false;
    QVariantList areas;
    if (const Node *list = top->child(u"shelterList"))
        for (const Node *a : list->children(u"Shelter")) {
            if (a->intValue(u"enable", 1) == 0)
                continue;
            const quint32 x = quint32(a->value(u"topLeftX").toUInt());
            const quint32 y = quint32(a->value(u"topLeftY").toUInt());
            const quint32 w = quint32(a->value(u"width").toUInt());
            const quint32 h = quint32(a->value(u"height").toUInt());
            areas.append(QVariantMap{{QStringLiteral("block"),
                                      QVariantMap{{QStringLiteral("x"), int(x >> 16)},
                                                  {QStringLiteral("y"), int(y >> 16)},
                                                  {QStringLiteral("width"), int(w >> 16)},
                                                  {QStringLiteral("height"), int(h >> 16)}}},
                                     {QStringLiteral("screen"),
                                      QVariantMap{{QStringLiteral("width"), int(x & 0xFFFF)},
                                                  {QStringLiteral("height"), int(y & 0xFFFF)}}}});
        }
    *value = QVariantMap{{QStringLiteral("Mask"),
                          QVariantMap{{QStringLiteral("channel"), ch},
                                      {QStringLiteral("enable"), top->intValue(u"enable")},
                                      {QStringLiteral("area"), areas.isEmpty() ? QVariant() : QVariant(areas)}}}};
    return true;
}

bool setMask(BcSession &s, int ch, const QVariantMap &p, QString *error)
{
    // Written the way the SDK's builder writes it (not the shape of the
    // reply): <Shelter> logicChannel / channelId / enable, then <ShelterList>
    // with one <Shelter> per slot — id, enable 1 and the packed coordinates
    // for an area, enable 0 for a free slot (that is how an area goes away).
    const QVariantMap m = p.value(QStringLiteral("Mask")).toMap();
    QVariant cur;
    if (!getMask(s, ch, &cur, error))
        return false;
    const QVariantMap now = cur.toMap().value(QStringLiteral("Mask")).toMap();
    const int enable = m.value(QStringLiteral("enable"), now.value(QStringLiteral("enable"))).toInt();
    const QVariantList areas = m.contains(QStringLiteral("area"))
                                   ? m.value(QStringLiteral("area")).toList()
                                   : now.value(QStringLiteral("area")).toList();
    constexpr int kSlots = 4; // the HTTP API's maxAreas
    QByteArray list;
    for (int i = 0; i < qMax(kSlots, int(areas.size())); ++i) {
        list += "<Shelter>\n<id>" + num(i) + "</id>\n";
        if (i >= areas.size()) {
            list += "<enable>0</enable>\n</Shelter>\n";
            continue;
        }
        const QVariantMap a = areas.at(i).toMap();
        const QVariantMap b = a.value(QStringLiteral("block")).toMap();
        const QVariantMap sc = a.value(QStringLiteral("screen")).toMap();
        const quint32 sw = quint32(sc.value(QStringLiteral("width")).toUInt()) & 0xFFFF;
        const quint32 sh = quint32(sc.value(QStringLiteral("height")).toUInt()) & 0xFFFF;
        auto pack = [](const QVariant &val, quint32 ref) {
            return QByteArray::number((quint32(val.toUInt()) << 16) | ref);
        };
        list += "<enable>1</enable>\n<topLeftX>" + pack(b.value(QStringLiteral("x")), sw)
                + "</topLeftX>\n<topLeftY>" + pack(b.value(QStringLiteral("y")), sh) + "</topLeftY>\n<width>"
                + pack(b.value(QStringLiteral("width")), sw) + "</width>\n<height>"
                + pack(b.value(QStringLiteral("height")), sh) + "</height>\n</Shelter>\n";
    }
    const QByteArray body = kXmlHead + QByteArray("<Shelter version=\"1.1\">\n<logicChannel>0</logicChannel>\n<channelId>")
        + num(ch) + "</channelId>\n<enable>" + num(enable ? 1 : 0) + "</enable>\n<ShelterList>\n" + list
        + "</ShelterList>\n</Shelter>\n" + kXmlTail;
    return request(s, ch, 53, body, nullptr, error);
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
        QStringLiteral("GetIsp"),       QStringLiteral("SetIsp"),
        QStringLiteral("GetIrLights"),  QStringLiteral("SetIrLights"),
        QStringLiteral("GetPowerLed"),  QStringLiteral("SetPowerLed"),
        QStringLiteral("GetOsd"),       QStringLiteral("SetOsd"),
        QStringLiteral("GetEnc"),       QStringLiteral("SetEnc"),
        QStringLiteral("GetAudioAlarmV20"), QStringLiteral("SetAudioAlarmV20"),
        QStringLiteral("GetAudioCfg"),  QStringLiteral("SetAudioCfg"),
        QStringLiteral("GetWhiteLed"),  QStringLiteral("SetWhiteLed"),
        QStringLiteral("AudioAlarmPlay"), QStringLiteral("QuickReplyPlay"),
        QStringLiteral("Reboot"),
        QStringLiteral("GetLocalLink"), QStringLiteral("GetNetPort"),
        QStringLiteral("GetHddInfo"),   QStringLiteral("Format"),
        QStringLiteral("GetUser"),      QStringLiteral("AddUser"),
        QStringLiteral("ModifyUser"),   QStringLiteral("DelUser"),
        QStringLiteral("GetTime"),      QStringLiteral("GetNtp"),
        QStringLiteral("SetNtp"),       QStringLiteral("GetDevInfo"),
        QStringLiteral("GetAutoUpgrade"), QStringLiteral("SetAutoUpgrade"),
        QStringLiteral("CheckFirmware"),  QStringLiteral("UpgradeOnline"),
        QStringLiteral("UpgradeStatus"),
        QStringLiteral("GetMask"),      QStringLiteral("SetMask"),
    };
    return mapped.contains(command);
}

bool get(BcSession &s, int channel, const QString &command, QVariant *value, QVariant *rng,
         QString *error, const QString &user)
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
    if (command == QLatin1String("GetIsp"))
        return getIsp(s, channel, value, rng, error);
    if (command == QLatin1String("GetIrLights"))
        return getLed(s, channel, false, value, rng, error);
    if (command == QLatin1String("GetPowerLed"))
        return getLed(s, channel, true, value, rng, error);
    if (command == QLatin1String("GetOsd"))
        return getOsd(s, channel, value, error);
    if (command == QLatin1String("GetEnc"))
        return getEnc(s, channel, value, error);
    if (command == QLatin1String("GetAudioAlarmV20"))
        return getAudioAlarm(s, channel, value, error);
    if (command == QLatin1String("GetAudioCfg"))
        return getAudioCfg(s, channel, value, rng, error);
    if (command == QLatin1String("GetWhiteLed"))
        return getWhiteLed(s, channel, value, rng, error);
    if (command == QLatin1String("GetMask"))
        return getMask(s, channel, value, error);
    if (command == QLatin1String("GetLocalLink"))
        return getLocalLink(s, value, error);
    if (command == QLatin1String("GetNetPort"))
        return getNetPort(s, value, error);
    if (command == QLatin1String("GetHddInfo"))
        return getHddInfo(s, value, error);
    if (command == QLatin1String("GetUser"))
        return getUsers(s, user, value, error);
    if (command == QLatin1String("GetTime"))
        return getTime(s, value, error);
    if (command == QLatin1String("GetNtp"))
        return getNtp(s, value, error);
    if (command == QLatin1String("GetDevInfo"))
        return getDevInfo(s, value, error);
    if (command == QLatin1String("GetAutoUpgrade"))
        return getAutoUpgrade(s, value, error);
    if (command == QLatin1String("CheckFirmware"))
        return checkFirmware(s, value, error);
    if (command == QLatin1String("UpgradeStatus"))
        return upgradeStatus(s, value, error);
    if (error)
        *error = QStringLiteral("not mapped");
    return false;
}

bool set(BcSession &s, int channel, const QString &command, const QVariantMap &param,
         QString *error, const QString &user)
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
    if (command == QLatin1String("SetIsp"))
        return setIsp(s, channel, param, error);
    if (command == QLatin1String("SetIrLights"))
        return setLed(s, channel, false, param, error);
    if (command == QLatin1String("SetPowerLed"))
        return setLed(s, channel, true, param, error);
    if (command == QLatin1String("SetOsd"))
        return setOsd(s, channel, param, error);
    if (command == QLatin1String("SetEnc"))
        return setEnc(s, channel, param, error);
    if (command == QLatin1String("SetAudioAlarmV20"))
        return setAudioAlarm(s, channel, param, error);
    if (command == QLatin1String("SetAudioCfg"))
        return setAudioCfg(s, channel, param, error);
    if (command == QLatin1String("SetWhiteLed"))
        return setWhiteLed(s, channel, param, error);
    if (command == QLatin1String("AudioAlarmPlay"))
        return playSiren(s, param, error);
    if (command == QLatin1String("QuickReplyPlay"))
        return quickReply(s, param, error);
    if (command == QLatin1String("Reboot"))
        return reboot(s, error);
    if (command == QLatin1String("SetMask"))
        return setMask(s, channel, param, error);
    if (command == QLatin1String("SetNtp"))
        return setNtp(s, param, error);
    if (command == QLatin1String("SetAutoUpgrade"))
        return setAutoUpgrade(s, param, error);
    if (command == QLatin1String("UpgradeOnline"))
        return upgradeOnline(s, error);
    if (command == QLatin1String("Format"))
        return formatDisk(s, param, error);
    if (command == QLatin1String("AddUser") || command == QLatin1String("ModifyUser")
        || command == QLatin1String("DelUser"))
        return setUser(s, user, command, param, error);
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
