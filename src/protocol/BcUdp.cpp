#include "BcUdp.h"

#include <QXmlStreamReader>
#include <QtEndian>

namespace rl::bcudp {

namespace {

constexpr quint32 kXmlKey[8] = {0x1f2d3c4b, 0x5a6c7f8d, 0x38172e4b, 0x8271635a,
                                0x863f1a2b, 0xa5c6f7d8, 0x8371e1b4, 0x17f2d3a5};

void putLE32(QByteArray &b, quint32 v)
{
    char t[4];
    qToLittleEndian(v, t);
    b.append(t, 4);
}

quint32 getLE32(const QByteArray &b, int at)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + at));
}

} // namespace

QByteArray xorXml(const QByteArray &in, quint32 tid)
{
    uchar key[32];
    for (int i = 0; i < 8; ++i)
        qToLittleEndian<quint32>(kXmlKey[i] + tid, key + 4 * i);
    QByteArray out(in.size(), Qt::Uninitialized);
    for (int i = 0; i < in.size(); ++i)
        out[i] = char(uchar(in[i]) ^ key[i % 32]);
    return out;
}

quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0;
    for (const char c : data) {
        crc ^= uchar(c);
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return crc;
}

QByteArray encodeDiscovery(quint32 tid, const QByteArray &xml)
{
    const QByteArray enc = xorXml(xml, tid);
    QByteArray b;
    putLE32(b, kMagicDiscovery);
    putLE32(b, static_cast<quint32>(enc.size()));
    putLE32(b, 1);
    putLE32(b, tid);
    putLE32(b, crc32(enc));
    return b + enc;
}

QByteArray encodeData(qint32 connId, quint32 packetId, const QByteArray &payload)
{
    QByteArray b;
    putLE32(b, kMagicData);
    putLE32(b, static_cast<quint32>(connId));
    putLE32(b, 0);
    putLE32(b, packetId);
    putLE32(b, static_cast<quint32>(payload.size()));
    return b + payload;
}

QByteArray encodeAck(qint32 connId, quint32 lastContiguous, const QByteArray &bitmap,
                     quint32 latency, quint32 group)
{
    QByteArray b;
    putLE32(b, kMagicAck);
    putLE32(b, static_cast<quint32>(connId));
    putLE32(b, 0);
    putLE32(b, group);
    putLE32(b, lastContiguous);
    putLE32(b, latency);
    putLE32(b, static_cast<quint32>(bitmap.size()));
    return b + bitmap;
}

QList<Packet> decode(const QByteArray &d)
{
    QList<Packet> out;
    int at = 0;
    while (d.size() - at >= 20) {
        const quint32 magic = getLE32(d, at);
        Packet p;
        if (magic == kMagicDiscovery) {
            const quint32 len = getLE32(d, at + 4);
            if (getLE32(d, at + 8) != 1 || len > quint32(d.size() - at - 20))
                break;
            p.kind = Packet::Kind::Discovery;
            p.tid = getLE32(d, at + 12);
            const QByteArray enc = d.mid(at + 20, int(len));
            if (crc32(enc) != getLE32(d, at + 16))
                break;
            p.xml = xorXml(enc, p.tid);
            at += 20 + int(len);
        } else if (magic == kMagicData) {
            const quint32 len = getLE32(d, at + 16);
            if (len > quint32(d.size() - at - 20))
                break;
            p.kind = Packet::Kind::Data;
            p.connId = static_cast<qint32>(getLE32(d, at + 4));
            p.packetId = getLE32(d, at + 12);
            p.payload = d.mid(at + 20, int(len));
            at += 20 + int(len);
        } else if (magic == kMagicAck) {
            if (d.size() - at < 28)
                break;
            const quint32 len = getLE32(d, at + 24);
            if (len > quint32(d.size() - at - 28))
                break;
            p.kind = Packet::Kind::Ack;
            p.connId = static_cast<qint32>(getLE32(d, at + 4));
            p.group = getLE32(d, at + 12);
            p.packetId = getLE32(d, at + 16);
            p.latency = getLE32(d, at + 20);
            p.payload = d.mid(at + 28, int(len));
            at += 28 + int(len);
        } else {
            break;
        }
        out.append(p);
    }
    return out;
}

Xml &Xml::add(const char *tag, const QByteArray &value)
{
    m_body += '<' + QByteArray(tag) + '>' + value + "</" + QByteArray(tag) + ">\n";
    return *this;
}

Xml &Xml::addAddr(const char *tag, const QString &ip, int port)
{
    m_body += '<' + QByteArray(tag) + ">\n<ip>" + ip.toLatin1() + "</ip>\n<port>"
        + QByteArray::number(port) + "</port>\n</" + QByteArray(tag) + ">\n";
    return *this;
}

QByteArray Xml::build() const
{
    return "<P2P>\n<" + m_type + ">\n" + m_body + "</" + m_type + ">\n</P2P>\n";
}

int Message::intValue(const QString &path, int def) const
{
    bool ok = false;
    const int v = values.value(path).toInt(&ok);
    return ok ? v : def;
}

Message parseXml(const QByteArray &xml)
{
    Message m;
    QXmlStreamReader r(xml);
    QStringList path; // element names below <P2P>
    QString text;
    while (!r.atEnd()) {
        switch (r.readNext()) {
        case QXmlStreamReader::StartElement:
            path.append(r.name().toString());
            text.clear();
            if (path.size() == 2)
                m.type = path[1];
            break;
        case QXmlStreamReader::Characters:
            text += r.text();
            break;
        case QXmlStreamReader::EndElement:
            if (path.size() > 2)
                m.values.insert(path.mid(2).join(QLatin1Char('/')), text.trimmed());
            text.clear();
            path.removeLast();
            break;
        default:
            break;
        }
    }
    return m;
}

} // namespace rl::bcudp
