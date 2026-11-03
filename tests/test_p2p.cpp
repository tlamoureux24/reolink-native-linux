#include "protocol/BcUdp.h"
#include "protocol/BcUdpLink.h"

#include <QRandomGenerator>
#include <QUdpSocket>
#include <QtTest>

#include <atomic>
#include <map>
#include <thread>

using namespace rl;
using namespace rl::bcudp;

namespace {

// A stand-in camera on 127.0.0.1: answers C2D_C like a camera on the LAN, then
// echoes back every byte it receives over the reliable data layer. It drops
// every 5th data packet the first time it sends it, so the client's ACK
// bitmap and the resend path both get exercised.
class FakeCamera
{
public:
    static constexpr qint32 kDid = 77;

    FakeCamera() { m_thread = std::thread([this] { run(); }); }
    ~FakeCamera()
    {
        m_stop = true;
        m_thread.join();
    }
    quint16 port()
    {
        while (!m_port)
            std::this_thread::yield();
        return m_port;
    }
    bool sawDisconnect() const { return m_disc; }

private:
    void run()
    {
        QUdpSocket s;
        s.bind(QHostAddress::LocalHost, 0);
        m_port = s.localPort();
        qint32 cid = 0;
        QHostAddress peer;
        quint16 peerPort = 0;
        quint32 rxWant = 0;
        std::map<quint32, QByteArray> early;
        quint32 nextTx = 0;
        std::map<quint32, QByteArray> unacked; // id -> datagram
        QElapsedTimer resend;
        resend.start();
        while (!m_stop) {
            if (cid && resend.elapsed() > 200) {
                for (auto &[id, d] : unacked)
                    s.writeDatagram(d, peer, peerPort);
                resend.restart();
            }
            if (!s.waitForReadyRead(5) && !s.hasPendingDatagrams())
                continue;
            while (s.hasPendingDatagrams()) {
                QByteArray d(int(s.pendingDatagramSize()), Qt::Uninitialized);
                d.resize(int(s.readDatagram(d.data(), d.size(), &peer, &peerPort)));
                QByteArray echo;
                for (const Packet &p : decode(d)) {
                    if (p.kind == Packet::Kind::Discovery) {
                        const Message m = parseXml(p.xml);
                        if (m.type == QLatin1String("C2D_C") && m.value("uid") == QLatin1String("TESTUID")) {
                            cid = m.intValue("cid");
                            const QByteArray r = Xml("D2C_C_R").add("rsp", 0).add("cid", cid).add("did", kDid).build();
                            s.writeDatagram(encodeDiscovery(p.tid, r), peer, peerPort);
                        } else if (m.type == QLatin1String("C2D_DISC")) {
                            m_disc = true;
                        }
                    } else if (p.kind == Packet::Kind::Data && p.connId == kDid) {
                        if (p.packetId >= rxWant)
                            early.emplace(p.packetId, p.payload);
                        for (auto it = early.find(rxWant); it != early.end(); it = early.find(rxWant)) {
                            echo += it->second;
                            early.erase(it);
                            ++rxWant;
                        }
                        s.writeDatagram(encodeAck(cid, rxWant - 1, {}), peer, peerPort);
                    } else if (p.kind == Packet::Kind::Ack && p.connId == kDid) {
                        if (p.packetId != 0xffffffffu)
                            unacked.erase(unacked.begin(), unacked.upper_bound(p.packetId));
                        for (int i = 0; i < p.payload.size(); ++i)
                            if (p.payload[i])
                                unacked.erase(p.packetId + 1 + quint32(i));
                    }
                }
                for (int at = 0; at < echo.size(); at += kMaxChunk) {
                    const quint32 id = nextTx++;
                    const QByteArray dg = encodeData(cid, id, echo.mid(at, kMaxChunk));
                    unacked.emplace(id, dg);
                    if (id % 5 != 4)
                        s.writeDatagram(dg, peer, peerPort);
                }
            }
        }
    }

    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<quint16> m_port{0};
    std::atomic<bool> m_disc{false};
};

} // namespace

class TestP2p : public QObject
{
    Q_OBJECT
private slots:
    void crcMatchesReference();
    void discoveryCipherMatchesReference();
    void dataAndAckRoundTrip();
    void corruptDiscoveryIsRejected();
    void xmlBuildAndParse();
    void lanConnectAndReliableEcho();
    void lanConnectTimesOutWithoutCamera();
};

// Reference values computed with reolink_aio's (MIT) calc_crc / encrypt_udp_baichuan.
void TestP2p::crcMatchesReference()
{
    QCOMPARE(crc32("123456789"), 0x2dfd2d88u);
    QCOMPARE(crc32({}), 0u);
}

void TestP2p::discoveryCipherMatchesReference()
{
    const QByteArray xml = "<P2P>\n<C2D_DISC>\n<cid>82000</cid>\n<did>80</did>\n</C2D_DISC>\n</P2P>\n";
    const QByteArray enc = QByteArray::fromHex(
        "976c1f4fd3755019996a487cf33032bc81265cef5cc6fe9724d241bf2ab79b739536117b841b52629b12385c"
        "d3074f88b7357cb47ca782ec47a14f8939fba225fb0227");
    QCOMPARE(xorXml(xml, 96), enc);
    QCOMPARE(xorXml(enc, 96), xml);
    QCOMPARE(crc32(enc), 0x42824554u);
    // The key words wrap around 2^32 when tid is added.
    QCOMPARE(xorXml(QByteArray(40, 0), 0xffffffffu),
             QByteArray::fromHex("4a3c2d1f8c7f6c5a4a2e1738596371822a1a3f86d7f7c6a5b3e17183a4d3f2174a3c2d1f8c7f6c5a"));

    const QByteArray d = encodeDiscovery(96, xml);
    QCOMPARE(d.left(20), QByteArray::fromHex("3acf872a") + QByteArray::fromHex("43000000")
                             + QByteArray::fromHex("01000000") + QByteArray::fromHex("60000000")
                             + QByteArray::fromHex("54458242"));
    const QList<Packet> ps = decode(d);
    QCOMPARE(ps.size(), 1);
    QCOMPARE(ps[0].kind, Packet::Kind::Discovery);
    QCOMPARE(ps[0].tid, 96u);
    QCOMPARE(ps[0].xml, xml);
}

void TestP2p::dataAndAckRoundTrip()
{
    const QByteArray data = encodeData(82000, 2439, "hello");
    QCOMPARE(data.toHex(), QByteArray("10cf872a" "50400100" "00000000" "87090000" "05000000" "68656c6c6f"));
    const QByteArray ack = encodeAck(80, 2439, QByteArray::fromHex("000101"), 54785);
    QCOMPARE(ack.toHex(), QByteArray("20cf872a" "50000000" "00000000" "00000000" "87090000" "01d60000"
                                     "03000000" "000101"));
    const QList<Packet> ps = decode(data + ack); // two packets in one datagram
    QCOMPARE(ps.size(), 2);
    QCOMPARE(ps[0].kind, Packet::Kind::Data);
    QCOMPARE(ps[0].connId, 82000);
    QCOMPARE(ps[0].packetId, 2439u);
    QCOMPARE(ps[0].payload, QByteArray("hello"));
    QCOMPARE(ps[1].kind, Packet::Kind::Ack);
    QCOMPARE(ps[1].connId, 80);
    QCOMPARE(ps[1].packetId, 2439u);
    QCOMPARE(ps[1].latency, 54785u);
    QCOMPARE(ps[1].payload, QByteArray::fromHex("000101"));
    // "Nothing received yet" acknowledgement.
    const QList<Packet> none = decode(encodeAck(5, 0xffffffffu, {}, 0, 0xffffffffu));
    QCOMPARE(none.size(), 1);
    QCOMPARE(none[0].group, 0xffffffffu);
}

void TestP2p::corruptDiscoveryIsRejected()
{
    QByteArray d = encodeDiscovery(7, "<P2P><C2M_Q><uid>X</uid></C2M_Q></P2P>");
    d[25] = char(d[25] ^ 0x01);
    QVERIFY(decode(d).isEmpty());
    QVERIFY(decode(d.left(30)).isEmpty());      // truncated
    QVERIFY(decode("garbage-garbage-garbage").isEmpty());
}

void TestP2p::xmlBuildAndParse()
{
    const QByteArray x = Xml("C2R_C")
                             .add("uid", "95270000ABCDEFGH")
                             .addAddr("cli", "192.168.1.10", 53600)
                             .addAddr("relay", "18.162.200.47", 58100)
                             .add("cid", -1234)
                             .build();
    const Message m = parseXml(x);
    QCOMPARE(m.type, QStringLiteral("C2R_C"));
    QCOMPARE(m.value("uid"), QStringLiteral("95270000ABCDEFGH"));
    QCOMPARE(m.value("cli/ip"), QStringLiteral("192.168.1.10"));
    QCOMPARE(m.intValue("relay/port"), 58100);
    QCOMPARE(m.intValue("cid"), -1234);
    QCOMPARE(m.intValue("missing", 42), 42);

    // A server reply with nested blocks and empty elements.
    const Message r = parseXml("<P2P><M2C_Q_R><reg><ip>1.2.3.4</ip><port>58200</port></reg>"
                               "<relay><ip>1.2.3.4</ip><port>58100</port></relay><timer/>"
                               "<rsp>0</rsp></M2C_Q_R></P2P>");
    QCOMPARE(r.type, QStringLiteral("M2C_Q_R"));
    QCOMPARE(r.value("reg/ip"), QStringLiteral("1.2.3.4"));
    QCOMPARE(r.intValue("reg/port"), 58200);
    QCOMPARE(r.intValue("rsp", -9), 0);
}

void TestP2p::lanConnectAndReliableEcho()
{
    FakeCamera cam;
    BcUdpLink::Options o;
    o.uid = QStringLiteral("TESTUID");
    o.lanHost = QStringLiteral("127.0.0.1");
    o.lanPort = cam.port();
    o.broadcast = false;
    o.remote = false;
    o.relay = false;
    o.timeoutMs = 5000;
    QString error;
    std::unique_ptr<BcUdpLink> link = BcUdpLink::connect(o, nullptr, &error);
    QVERIFY2(link, qPrintable(error));
    QCOMPARE(link->mode(), QStringLiteral("lan"));
    QVERIFY(link->isConnected());

    // Larger than a key frame, in uneven writes, so packets span write boundaries.
    QByteArray sent(300 * 1000, Qt::Uninitialized);
    for (char &c : sent)
        c = char(QRandomGenerator::global()->generate());
    for (int at = 0; at < sent.size(); at += 7777)
        QVERIFY(link->write(sent.mid(at, 7777)));

    QByteArray got;
    QElapsedTimer t;
    t.start();
    while (got.size() < sent.size() && t.elapsed() < 20000)
        if (link->waitForReadyRead(200))
            got += link->readAll();
    QCOMPARE(got.size(), sent.size());
    QVERIFY(got == sent);

    link.reset(); // sends C2D_DISC
    QTRY_VERIFY(cam.sawDisconnect());
}

void TestP2p::lanConnectTimesOutWithoutCamera()
{
    QUdpSocket silent; // a port where nobody answers
    silent.bind(QHostAddress::LocalHost, 0);
    BcUdpLink::Options o;
    o.uid = QStringLiteral("TESTUID");
    o.lanHost = QStringLiteral("127.0.0.1");
    o.lanPort = silent.localPort();
    o.broadcast = false;
    o.remote = false;
    o.relay = false;
    o.timeoutMs = 800;
    QString error;
    QElapsedTimer t;
    t.start();
    QVERIFY(!BcUdpLink::connect(o, nullptr, &error));
    QVERIFY(t.elapsed() < 3000);
    QVERIFY(!error.isEmpty());

    // Cancelling stops the attempt early.
    std::atomic<bool> abort{true};
    o.timeoutMs = 10000;
    t.restart();
    QVERIFY(!BcUdpLink::connect(o, &abort, &error));
    QVERIFY(t.elapsed() < 1000);
}

QTEST_GUILESS_MAIN(TestP2p)
#include "test_p2p.moc"
