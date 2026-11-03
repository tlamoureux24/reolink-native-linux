#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>

// Datagram layer of Baichuan over UDP — the transport the official apps use to
// reach a camera by UID (on the LAN or through Reolink's P2P servers).
// Specification: docs/research/p2p.md.
//
// Clean-room: packet layouts, XOR key and CRC from MIT reolink_aio; message
// names from the official client's SDK. No AGPL neolink code.
namespace rl::bcudp {

constexpr quint32 kMagicDiscovery = 0x2a87cf3a;
constexpr quint32 kMagicData = 0x2a87cf10;
constexpr quint32 kMagicAck = 0x2a87cf20;
constexpr int kMtu = 1350;
constexpr int kDataHeader = 20;
constexpr int kMaxChunk = kMtu - kDataHeader;

// Discovery XML cipher (symmetric): XOR with the fixed key words, each + tid.
QByteArray xorXml(const QByteArray &in, quint32 tid);

// Reflected CRC-32 (poly 0xEDB88320), initial value 0, no final XOR.
quint32 crc32(const QByteArray &data);

struct Packet {
    enum class Kind { Discovery, Data, Ack } kind = Kind::Data;
    // Discovery
    quint32 tid = 0;
    QByteArray xml; // plaintext <P2P>…</P2P>
    // Data / Ack
    qint32 connId = 0;
    quint32 packetId = 0; // Data: this packet; Ack: last contiguous id received
    quint32 group = 0;    // Ack only
    quint32 latency = 0;  // Ack only
    QByteArray payload;   // Data bytes, or the Ack's received-bitmap
};

QByteArray encodeDiscovery(quint32 tid, const QByteArray &xml);
QByteArray encodeData(qint32 connId, quint32 packetId, const QByteArray &payload);
QByteArray encodeAck(qint32 connId, quint32 lastContiguous, const QByteArray &bitmap,
                     quint32 latency = 0, quint32 group = 0);

// Parse every packet in one datagram (normally one). Malformed or short input,
// or a discovery packet with a bad CRC, stops parsing at that point.
QList<Packet> decode(const QByteArray &datagram);

// ---- Discovery XML ------------------------------------------------------

// Builder for <P2P><type>…</type></P2P>. Values are inserted as-is (they are
// numbers, UIDs, IPs and fixed words, never free text).
class Xml
{
public:
    explicit Xml(const char *type) : m_type(type) {}
    Xml &add(const char *tag, const QByteArray &value);
    Xml &add(const char *tag, qint64 value) { return add(tag, QByteArray::number(value)); }
    Xml &addAddr(const char *tag, const QString &ip, int port); // <tag><ip/><port/></tag>
    QByteArray build() const;

private:
    QByteArray m_type;
    QByteArray m_body;
};

// Parsed discovery message: its type (C2D_C, D2C_CFM, …) and its leaf values
// keyed by path below the type ("cid", "reg/ip", "relay/port").
struct Message {
    QString type;
    QMap<QString, QString> values;
    QString value(const QString &path) const { return values.value(path); }
    int intValue(const QString &path, int def = 0) const;
};
Message parseXml(const QByteArray &xml);

} // namespace rl::bcudp
