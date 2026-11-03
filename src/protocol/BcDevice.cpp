#include "BcDevice.h"

#include "core/Log.h"
#include "protocol/BcCrypto.h"
#include "protocol/BcSession.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QThread>
#include <QXmlStreamReader>

namespace rl::bcdev {

namespace {

// A reply as a small tree: enough to read the device's flat XML.
struct Node {
    QString name;
    QString text;
    QList<Node> kids;

    const Node *child(QStringView n) const
    {
        for (const Node &k : kids)
            if (k.name == n)
                return &k;
        return nullptr;
    }
    // Depth-first: the first element called `n` under this one.
    const Node *find(QStringView n) const
    {
        for (const Node &k : kids) {
            if (k.name == n)
                return &k;
            if (const Node *f = k.find(n))
                return f;
        }
        return nullptr;
    }
    QString value(QStringView n) const
    {
        const Node *c = child(n);
        return c ? c->text.trimmed() : QString();
    }
    int intValue(QStringView n, int dflt = 0) const
    {
        bool ok = false;
        const int v = value(n).toInt(&ok);
        return ok ? v : dflt;
    }
};

Node parse(const QByteArray &xml)
{
    Node root;
    QList<Node *> stack{&root};
    QXmlStreamReader r(xml);
    while (!r.atEnd()) {
        switch (r.readNext()) {
        case QXmlStreamReader::StartElement: {
            Node *parent = stack.last();
            parent->kids.append(Node{r.name().toString(), {}, {}});
            stack.append(&parent->kids.last());
            break;
        }
        case QXmlStreamReader::Characters:
            stack.last()->text += r.text();
            break;
        case QXmlStreamReader::EndElement:
            if (stack.size() > 1)
                stack.removeLast();
            break;
        default:
            break;
        }
    }
    return root;
}

bool bit(int v, int n)
{
    return (v >> n) & 1;
}

QSize streamSize(const Node *stream)
{
    if (!stream)
        return {};
    return {stream->intValue(u"width"), stream->intValue(u"height")};
}

} // namespace

void parseLoginInfo(const QByteArray &xml, Device &d)
{
    const Node root = parse(xml);
    const Node *info = root.find(u"DeviceInfo");
    if (!info)
        return;
    d.type = info->value(u"type").toLower();
    d.channelNum = qMax(1, info->intValue(u"channelNum", 1));
    // An NVR (or hub) shows its cameras as channels; a camera is channel 0.
    d.isNvr = d.type.contains(QLatin1String("nvr")) || d.type == QLatin1String("homebase")
              || d.channelNum > 1;
}

void parseVersionInfo(const QByteArray &xml, Device &d)
{
    const Node root = parse(xml);
    const Node *v = root.find(u"VersionInfo");
    if (!v)
        return;
    d.name = v->value(u"name");
    d.model = v->value(u"type");
    d.firmware = v->value(u"firmwareVersion");
    d.hardware = v->value(u"hardwareVersion");
    d.serial = v->value(u"serialNumber");
}

QString parseUid(const QByteArray &xml)
{
    const Node root = parse(xml);
    const Node *u = root.find(u"Uid");
    return u ? u->value(u"uid") : QString();
}

QVector<api::ChannelCaps> parseSupport(const QByteArray &xml, int channelNum, bool isNvr)
{
    QVector<api::ChannelCaps> out(qMax(1, channelNum));
    const Node root = parse(xml);
    const Node *sup = root.find(u"Support");
    if (!sup)
        return out;
    const bool hostAudio = sup->intValue(u"audioNum") > 0;
    const bool hostTalk = sup->intValue(u"audioTalk") > 0;
    for (const Node &item : sup->kids) {
        if (item.name != u"item")
            continue;
        const int ch = item.intValue(u"chnID", -1);
        if (ch < 0 || ch >= out.size())
            continue;
        api::ChannelCaps &c = out[ch];
        // ptzType: 0 none, 1 zoom/focus only, 2/3/5/6/7 pan-tilt; ptzControl
        // bits: 1 no diagonal, 2 guard, 3 calibration, 4 digital zoom, 6 speed.
        const int ptzType = item.intValue(u"ptzType");
        const int ptzCtl = item.intValue(u"ptzControl");
        c.ptz = ptzType != 0;
        c.panTilt = ptzType == 2 || ptzType == 3 || ptzType == 5 || ptzType == 6 || ptzType == 7;
        c.ptzPreset = c.panTilt && item.intValue(u"ptzPreset") > 0;
        c.ptzPatrol = c.panTilt && item.intValue(u"ptzPatrol") > 0;
        c.ptzGuard = c.panTilt && bit(ptzCtl, 2);
        c.ptzSpeed = bit(ptzCtl, 6);
        c.ptzCheck = c.panTilt && bit(ptzCtl, 3);
        c.zoomFocusPos = ptzType == 1 || ptzType == 2 || ptzType == 5;
        c.zoom = c.zoomFocusPos || bit(ptzCtl, 4);
        c.focus = c.zoomFocusPos;
        c.autoFocus = item.intValue(u"autoFocus") > 0;
        // aitype bits: 1 people, 2 vehicle, 4 dog/cat, 7 auto tracking.
        const int ai = item.intValue(u"aitype");
        c.aiPeople = bit(ai, 1);
        c.aiVehicle = bit(ai, 2);
        c.aiDogCat = bit(ai, 4);
        c.ai = c.aiPeople || c.aiVehicle || c.aiDogCat;
        c.autoTrack = bit(ai, 7);
        c.audio = item.intValue(u"noAudio") == 0 && (isNvr || hostAudio);
        c.talk = item.intValue(u"talk") > 0 || item.intValue(u"ipcAudioTalk") > 0
                 || (!isNvr && hostTalk);
        // audioVersion bit 2: siren; ledCtrl bits 1 and 2: floodlight.
        c.siren = bit(item.intValue(u"audioVersion"), 2);
        const int led = item.intValue(u"ledCtrl");
        c.floodlight = bit(led, 1) && bit(led, 2);
        c.battery = item.intValue(u"battery") > 0;
        c.doorbell = item.intValue(u"doorbellVersion") > 0;
    }
    return out;
}

bool parseIsAdmin(const QByteArray &xml)
{
    const Node root = parse(xml);
    const Node *sys = root.find(u"system");
    if (!sys)
        return false;
    for (const Node &m : sys->kids) {
        const QString v = m.value(u"abilityValue");
        if (v.contains(QLatin1String("reboot_rw")) || v.contains(QLatin1String("restore_rw")))
            return true;
    }
    return false;
}

void parseCompression(const QByteArray &xml, Channel &c)
{
    const Node root = parse(xml);
    const Node *comp = root.find(u"Compression");
    if (!comp)
        return;
    c.mainSize = streamSize(comp->child(u"mainStream"));
    c.subSize = streamSize(comp->child(u"subStream"));
    c.thirdStream = comp->child(u"thirdStream") != nullptr;
}

api::BatteryInfo parseBattery(const QByteArray &xml)
{
    api::BatteryInfo b;
    const Node root = parse(xml);
    const Node *bat = root.find(u"BatteryInfo");
    if (!bat || bat->value(u"batteryPercent").isEmpty())
        return b;
    const int pct = bat->intValue(u"batteryPercent", -1);
    if (pct < 0 || pct > 100)
        return b;
    b.present = true;
    b.percent = pct;
    const QString charge = bat->value(u"chargeStatus").toLower();
    b.charging = charge == QLatin1String("charging") || charge == QLatin1String("chargecomplete")
                 || bat->value(u"adapterStatus").compare(QLatin1String("none"), Qt::CaseInsensitive) != 0
                        && !bat->value(u"adapterStatus").isEmpty();
    return b;
}

QVector<QPair<int, bool>> parseChannelStates(const QByteArray &xml)
{
    QVector<QPair<int, bool>> out;
    const Node root = parse(xml);
    const Node *list = root.find(u"ChannelInfoList");
    if (!list)
        return out;
    for (const Node &ci : list->kids) {
        if (ci.name != u"ChannelInfo")
            continue;
        const int ch = ci.intValue(u"channelId", -1);
        if (ch < 0)
            continue;
        // loginState: only "standby" is known for sure besides the connected
        // state; a camera that is disconnected or rejects the NVR is offline.
        const QString st = ci.value(u"loginState").toLower();
        const bool online = !(st.contains(QLatin1String("disconnect"))
                              || st.contains(QLatin1String("fail"))
                              || st.contains(QLatin1String("pwderror")) || st == QLatin1String("offline"));
        out.append({ch, online});
    }
    return out;
}

QString parseChannelName(const QByteArray &xml)
{
    const Node root = parse(xml);
    const Node *n = root.find(u"OsdChannelName");
    return n ? n->value(u"name") : QString();
}

QVector<QPair<int, api::DetectionState>> parseAlarmEvents(const QByteArray &xml)
{
    QVector<QPair<int, api::DetectionState>> out;
    const Node root = parse(xml);
    const Node *list = root.find(u"AlarmEventList");
    if (!list)
        return out;
    auto words = [](const QString &v) {
        QStringList l;
        for (const QString &w : v.split(QLatin1Char(','), Qt::SkipEmptyParts))
            l << w.trimmed().toLower();
        return l;
    };
    for (const Node &ev : list->kids) {
        if (ev.name != u"AlarmEvent")
            continue;
        bool ok = false;
        const int ch = ev.value(u"channelId").toInt(&ok);
        if (!ok)
            continue;
        const QStringList status = words(ev.value(u"status"));
        const QStringList ai = words(ev.value(u"AItype"));
        api::DetectionState st;
        st.motion = status.contains(QLatin1String("md"));
        st.person = ai.contains(QLatin1String("people"));
        st.vehicle = ai.contains(QLatin1String("vehicle"));
        st.pet = ai.contains(QLatin1String("dog_cat"));
        out.append({ch, st});
    }
    return out;
}

QByteArray snapshot(BcSession &s, int channel, bool mainStream, QString *error)
{
    struct State {
        QMutex mutex;
        QByteArray jpeg;
        int expected = -1;
        bool done = false;
        quint16 status = 0;
    };
    auto st = std::make_shared<State>();
    const QByteArray key = s.aesKey();
    const quint32 id = s.newMessId();
    s.route(id, [st, key](const BcMessage &m) {
        if (m.cmd != 109)
            return;
        const int off = m.payloadOffset ? int(m.payloadOffset) : int(m.body.size());
        const QByteArray xml = off ? bc::aesCfb(m.body.left(off), key, true) : QByteArray();
        QByteArray payload = m.body.mid(off);
        const int p = xml.indexOf("<encryptLen>");
        if (p >= 0 && !payload.isEmpty()) {
            const int n = xml.mid(p + 12, xml.indexOf("</encryptLen>", p) - p - 12).toInt();
            payload = bc::aesCfb(payload.left(n), key, true) + payload.mid(n);
        }
        QMutexLocker lock(&st->mutex);
        st->status = m.status;
        const int a = xml.indexOf("<pictureSize>");
        if (a >= 0)
            st->expected = xml.mid(a + 13, xml.indexOf("</pictureSize>", a) - a - 13).toInt();
        st->jpeg += payload;
        // Ends with an empty message, or once the announced size is in.
        if (m.body.isEmpty() || (st->expected > 0 && st->jpeg.size() >= st->expected)
            || (m.status != 0 && m.status != 200 && st->jpeg.isEmpty()))
            st->done = true;
    });
    const QByteArray body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n<Snap version=\"1.1\">\n<channelId>"
        + QByteArray::number(channel)
        + "</channelId>\n<logicChannel>0</logicChannel>\n<time>0</time>\n<fullFrame>0</fullFrame>\n"
          "<streamType>"
        + (mainStream ? "main" : "sub") + "</streamType>\n</Snap>\n</body>\n";
    s.send(109, quint8(channel + 1), id, {}, bc::aesCfb(body, key, false));
    QElapsedTimer t;
    t.start();
    for (;;) {
        {
            QMutexLocker lock(&st->mutex);
            if (st->done)
                break;
        }
        if (t.elapsed() > 30000 || !s.isAlive())
            break;
        QThread::msleep(50);
    }
    s.unroute(id);
    QMutexLocker lock(&st->mutex);
    if (!st->jpeg.startsWith("\xff\xd8")) {
        if (error)
            *error = st->status && st->status != 200 ? QStringLiteral("status %1").arg(st->status)
                                                     : QStringLiteral("no image");
        return {};
    }
    return st->jpeg;
}

bool readDevice(BcSession &s, const QString &username, Device *out)
{
    Device d;
    parseLoginInfo(s.loginInfo(), d);

    // Host level: header channel byte 250 (as the SDK and reolink_aio).
    constexpr quint8 kHost = 250;
    QByteArray xml;
    quint16 status = 0;
    if (!s.requestXml(80, kHost, {}, {}, &xml, &status))
        return false;
    parseVersionInfo(xml, d);
    if (s.requestXml(114, kHost, {}, {}, &xml, &status) && status == 200)
        d.uid = parseUid(xml);
    QVector<api::ChannelCaps> caps(d.channelNum);
    if (s.requestXml(199, kHost, {}, {}, &xml, &status) && status == 200)
        caps = parseSupport(xml, d.channelNum, d.isNvr);
    const QByteArray rightsExt =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<Extension version=\"1.1\">\n<userName>"
        + username.toHtmlEscaped().toUtf8() + "</userName>\n<token>system</token>\n</Extension>\n";
    if (s.requestXml(151, kHost, rightsExt, {}, &xml, &status) && status == 200)
        d.isAdmin = parseIsAdmin(xml);

    // Channels: a camera is channel 0; an NVR pushes its channels' states
    // (cmd 145) right after login.
    QHash<int, bool> online;
    if (d.isNvr) {
        BcMessage push;
        QElapsedTimer t;
        t.start();
        while (!s.lastPush(145, &push) && t.elapsed() < 3000 && s.isAlive())
            QThread::msleep(50);
        if (s.lastPush(145, &push))
            for (const auto &st : parseChannelStates(s.bodyXml(push)))
                online.insert(st.first, st.second);
    }
    for (int ch = 0; ch < d.channelNum; ++ch) {
        Channel c;
        c.channel = ch;
        c.online = online.value(ch, true);
        c.caps = ch < caps.size() ? caps.at(ch) : api::ChannelCaps{};
        if (c.online) {
            const quint8 hdr = quint8(ch + 1); // channel-scoped: channel + 1
            const QByteArray ext = BcSession::channelExtension(ch);
            if (s.requestXml(56, hdr, ext, {}, &xml, &status) && status == 200)
                parseCompression(xml, c);
            else if (d.isNvr && online.isEmpty())
                c.online = false; // no state pushed: a channel that answers nothing is empty
            if (d.isNvr && s.requestXml(44, hdr, ext, {}, &xml, &status) && status == 200)
                c.name = parseChannelName(xml);
            if (ch == 0 && c.caps.battery
                && s.requestXml(253, hdr, ext, {}, &xml, &status) && status == 200)
                d.battery = parseBattery(xml);
        }
        c.caps.supportsBalanced = c.thirdStream;
        if (!d.isNvr)
            c.name = d.name;
        d.talk = d.talk || c.caps.talk;
        d.channels.append(c);
    }
    if (!s.isAlive())
        return false;
    *out = d;
    return true;
}

} // namespace rl::bcdev
