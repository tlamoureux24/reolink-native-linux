#include "BaichuanControl.h"

#include "core/Log.h"

#include "protocol/BcCrypto.h"
#include "protocol/BcSession.h"

#include <QXmlStreamReader>

namespace rl {


BaichuanControl::BaichuanControl(std::shared_ptr<BcSession> session) : m_session(std::move(session)) {}

BaichuanControl::~BaichuanControl() = default;

bool BaichuanControl::isOpen() const
{
    return m_session && m_session->isAlive();
}

bool BaichuanControl::open()
{
    if (!m_session || !m_session->waitReady(15000))
        return false;
    m_aesKey = m_session->aesKey();
    return true;
}

void BaichuanControl::close()
{
    m_aesKey.clear();
}

QByteArray BaichuanControl::transact(quint32 cmdId, int channel, const QByteArray &bodyXml,
                                     quint16 *statusOut)
{
    if (!isOpen())
        return {};
    // Channel-scoped: header channel byte channel + 1 and the <Extension>;
    // host level: 250.
    const quint8 chId = channel < 0 ? 250 : static_cast<quint8>(channel + 1);
    const QByteArray ext = channel < 0 ? QByteArray() : BcSession::channelExtension(channel);
    QByteArray xml;
    quint16 status = 0;
    if (!m_session->requestXml(cmdId, chId, ext, bodyXml, &xml, &status, 12000, &m_messId))
        return {};
    if (statusOut)
        *statusOut = status;
    return xml;
}

QByteArray BaichuanControl::get(quint32 cmdId, int channel, quint16 *statusOut)
{
    return transact(cmdId, channel, QByteArray(), statusOut);
}

int BaichuanControl::readEnable(quint32 getCmdId, int channel)
{
    const QByteArray xml = get(getCmdId, channel);
    if (xml.isEmpty())
        return -1;
    const int a = xml.indexOf("<enable>");
    if (a < 0)
        return -1;
    const int b = xml.indexOf("</enable>", a);
    if (b < 0)
        return -1;
    return xml.mid(a + 8, b - a - 8).trimmed().toInt();
}

QVariantMap BaichuanControl::getConfigFlat(quint32 cmdId, int channel, const QByteArray &reqBody)
{
    QVariantMap out;
    const QByteArray xml = transact(cmdId, channel, reqBody);
    if (xml.isEmpty())
        return out;
    QXmlStreamReader r(xml);
    QStringList stack;
    while (!r.atEnd()) {
        const auto tok = r.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            stack.append(r.name().toString());
        } else if (tok == QXmlStreamReader::EndElement) {
            if (!stack.isEmpty())
                stack.removeLast();
        } else if (tok == QXmlStreamReader::Characters && !r.isWhitespace() && !stack.isEmpty()) {
            const QString tag = stack.last();
            const QString text = r.text().toString();
            // Flat leaf tag (first occurrence wins) AND a "parent/tag" path key, so
            // ambiguous tags like DayNight/mode (vs anti-flicker mode) are reachable.
            if (!out.contains(tag))
                out.insert(tag, text);
            if (stack.size() >= 2) {
                const QString path = stack.at(stack.size() - 2) + '/' + tag;
                if (!out.contains(path))
                    out.insert(path, text);
            }
        }
    }
    return out;
}

bool BaichuanControl::writeFields(quint32 getCmdId, quint32 setCmdId, int channel,
                                  const QVariantMap &changes, const QByteArray &getBody)
{
    QByteArray xml = transact(getCmdId, channel, getBody);
    if (xml.isEmpty())
        return false;
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        const QByteArray key = it.key().toUtf8();
        const QByteArray val = it.value().toString().toUtf8();

        // "parent//child": set EVERY <child> inside the first <parent> element —
        // for list configs like motion's <sensitivityInfoList> where each time
        // block carries its own <sensitivity>, without touching a sibling range.
        const int dslash = key.indexOf("//");
        if (dslash >= 0) {
            const QByteArray parent = key.left(dslash);
            const QByteArray child = key.mid(dslash + 2);
            int pStart = xml.indexOf("<" + parent + ">");
            if (pStart < 0)
                pStart = xml.indexOf("<" + parent + " ");
            if (pStart < 0)
                continue;
            int pEnd = xml.indexOf("</" + parent + ">", pStart);
            if (pEnd < 0)
                pEnd = xml.size();
            const QByteArray inner = xml.mid(pStart, pEnd - pStart);
            const QByteArray co = "<" + child + ">";
            const QByteArray cc = "</" + child + ">";
            QByteArray rebuilt;
            int pos = 0;
            while (true) {
                const int o = inner.indexOf(co, pos);
                if (o < 0) {
                    rebuilt += inner.mid(pos);
                    break;
                }
                const int c = inner.indexOf(cc, o + co.size());
                if (c < 0) {
                    rebuilt += inner.mid(pos);
                    break;
                }
                rebuilt += inner.mid(pos, o + co.size() - pos) + val;
                pos = c;
            }
            xml = xml.left(pStart) + rebuilt + xml.mid(pEnd);
            continue;
        }

        const int slash = key.indexOf('/');
        int searchFrom = 0;
        QByteArray child = key;
        if (slash >= 0) {
            // "parent/child": scope the search to inside the parent element so an
            // ambiguous child tag (e.g. DayNight/mode) hits the right one.
            const QByteArray parent = key.left(slash);
            child = key.mid(slash + 1);
            int p = xml.indexOf("<" + parent + ">");
            if (p < 0)
                p = xml.indexOf("<" + parent + " "); // element with attributes
            if (p < 0)
                continue;
            searchFrom = p;
        }
        const QByteArray open = "<" + child + ">";
        const QByteArray close = "</" + child + ">";
        const int a = xml.indexOf(open, searchFrom);
        if (a < 0)
            continue;
        const int b = xml.indexOf(close, a + open.size());
        if (b < 0)
            continue;
        xml = xml.left(a + open.size()) + val + xml.mid(b);
    }
    quint16 status = 0;
    transact(setCmdId, channel, xml, &status);
    return status == 200 || status == 201 || status == 300;
}

bool BaichuanControl::writeEnable(quint32 getCmdId, quint32 setCmdId, int channel, bool enable)
{
    // Read-modify-write: GET the config, flip only the first <enable>, SET it back.
    QByteArray xml = get(getCmdId, channel);
    if (xml.isEmpty())
        return false;
    const int a = xml.indexOf("<enable>");
    if (a < 0)
        return false;
    const int b = xml.indexOf("</enable>", a);
    if (b < 0)
        return false;
    const QByteArray mutated =
        xml.left(a + 8) + (enable ? "1" : "0") + xml.mid(b);
    quint16 status = 0;
    transact(setCmdId, channel, mutated, &status);
    return status == 200 || status == 201 || status == 300;
}

} // namespace rl

// ---- Two-way talk --------------------------------------------------------

namespace rl {

namespace {
QString xmlText(const QByteArray &xml, const char *tag)
{
    const QByteArray open = QByteArray("<") + tag + ">";
    const int a = xml.indexOf(open);
    if (a < 0)
        return {};
    const int b = xml.indexOf(QByteArray("</") + tag + ">", a);
    return b < 0 ? QString() : QString::fromUtf8(xml.mid(a + open.size(), b - a - open.size())).trimmed();
}
} // namespace

BaichuanControl::TalkParams BaichuanControl::parseTalkAbility(const QByteArray &xml, bool *ok)
{
    TalkParams p;
    const bool found = xml.contains("<TalkAbility");
    if (ok)
        *ok = found;
    if (!found)
        return p;
    // First entry of each list wins (the camera orders them by priority).
    if (const QString v = xmlText(xml, "duplex"); !v.isEmpty()) p.duplex = v;
    if (const QString v = xmlText(xml, "audioStreamMode"); !v.isEmpty()) p.audioStreamMode = v;
    if (const QString v = xmlText(xml, "audioType"); !v.isEmpty()) p.audioType = v;
    if (const int v = xmlText(xml, "sampleRate").toInt(); v > 0) p.sampleRate = v;
    if (const int v = xmlText(xml, "samplePrecision").toInt(); v > 0) p.samplePrecision = v;
    if (const int v = xmlText(xml, "lengthPerEncoder").toInt(); v > 0) p.lengthPerEncoder = v;
    if (const QString v = xmlText(xml, "soundTrack"); !v.isEmpty()) p.soundTrack = v;
    return p;
}

QByteArray BaichuanControl::talkConfigXml(int channel, const TalkParams &p)
{
    return QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n"
                          "<TalkConfig version=\"1.1\">\n"
                          "<channelId>%1</channelId>\n<duplex>%2</duplex>\n"
                          "<audioStreamMode>%3</audioStreamMode>\n<audioConfig>\n"
                          "<audioType>%4</audioType>\n<sampleRate>%5</sampleRate>\n"
                          "<samplePrecision>%6</samplePrecision>\n"
                          "<lengthPerEncoder>%7</lengthPerEncoder>\n<soundTrack>%8</soundTrack>\n"
                          "</audioConfig>\n</TalkConfig>\n</body>\n")
        .arg(channel)
        .arg(p.duplex, p.audioStreamMode, p.audioType)
        .arg(p.sampleRate)
        .arg(p.samplePrecision)
        .arg(p.lengthPerEncoder)
        .arg(p.soundTrack)
        .toUtf8();
}

static void putLE16(QByteArray &b, quint16 v)
{
    b.append(char(v & 0xFF));
    b.append(char(v >> 8));
}

QByteArray BaichuanControl::talkFrame(const QByteArray &adpcm, qint16 predictor, quint8 index)
{
    const int n = static_cast<int>(adpcm.size());
    const quint16 size = static_cast<quint16>(n + 8); // block header + state + data
    QByteArray f;
    f.reserve(16 + n);
    f.append("01wb", 4);
    putLE16(f, size);
    putLE16(f, size);
    f.append(char(0x00));
    f.append(char(0x01));
    f.append(char((n / 2 + 2) & 0xFF)); // block length in 16-bit words, truncated
    f.append(char(0x00));
    putLE16(f, static_cast<quint16>(predictor));
    putLE16(f, index);
    f.append(adpcm);
    return f;
}

bool BaichuanControl::talkStart(int channel, const TalkParams &p, quint16 *statusOut)
{
    quint16 status = 0;
    transact(201, channel, talkConfigXml(channel, p), &status);
    if (statusOut)
        *statusOut = status;
    m_talkMessId = m_messId & 0xFFFFFF;
    return status == 200 || status == 201 || status == 300;
}

bool BaichuanControl::talkSend(int channel, const QByteArray &frame)
{
    if (!isOpen())
        return false;
    const QByteArray ext = QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
                                             "<Extension version=\"1.1\">\n<binaryData>1</binaryData>\n"
                                             "<channelId>")
        + QByteArray::number(channel) + QByteArrayLiteral("</channelId>\n</Extension>\n");
    // Same channel and mess_id as the cmd 201 that opened the session, AES
    // extension (as every other modern message). RL_TALK_VARIANT=zerohdr sends
    // channel/mess_id 0 like the SDK's own header template; =plainext leaves the
    // extension unencrypted — diagnostics for firmware that disagrees.
    static const QByteArray variant = qgetenv("RL_TALK_VARIANT");
    const bool zeroHdr = variant.contains("zerohdr");
    const bool plainExt = variant.contains("plainext");
    return m_session->send(202, zeroHdr ? 0 : static_cast<quint8>(channel + 1),
                           zeroHdr ? 0 : m_talkMessId,
                           plainExt ? ext : bc::aesCfb(ext, m_aesKey, false), frame);
}

void BaichuanControl::talkStop(int channel)
{
    if (isOpen())
        transact(11, channel, QByteArray());
}

} // namespace rl
