#include "BcUdpLink.h"

#include "core/Log.h"
#include "protocol/BcUdp.h"

#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QSet>
#include <QUdpSocket>
#include <QVariant>

#include <chrono>
#include <netdb.h>
#include <netinet/in.h>
#include <map>

namespace rl {

using namespace bcudp;

namespace {

qint64 nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

quint32 newTid()
{
    return QRandomGenerator::global()->bounded(1000u, 1000000u);
}

struct Endpoint {
    QHostAddress addr;
    quint16 port = 0;
    bool valid() const { return !addr.isNull() && addr != QHostAddress::AnyIPv4 && port != 0; }
    bool operator==(const Endpoint &o) const { return addr.isEqual(o.addr) && port == o.port; }
    QString toString() const { return addr.toString() + QLatin1Char(':') + QString::number(port); }
};

Endpoint endpointAt(const Message &m, const QString &tag)
{
    return {QHostAddress(m.value(tag + QStringLiteral("/ip"))),
            static_cast<quint16>(m.intValue(tag + QStringLiteral("/port")))};
}

// A discovery message resent every 500 ms until answered (or dropped).
struct Retry {
    QString key; // groups retries that one reply settles ("lan", "lookup", "reg", …)
    QByteArray datagram;
    Endpoint to;
    qint64 next = 0;
};

// Where each UID was last found: the register/relay servers a lookup named.
// Reusing them spares asking every regional server again — which, with several
// connections at once (validation, live panes, talk), gets answers dropped.
struct KnownUid {
    Endpoint reg;
    Endpoint relay;
    qint64 at = 0;
};
QMutex g_knownMutex;
QHash<QString, KnownUid> g_known;
constexpr qint64 kKnownMs = 30 * 60 * 1000;
constexpr qint64 kKnownGraceMs = 3000; // then ask every server anyway

constexpr qint64 kResendMs = 500;
constexpr qint64 kRelayDelayMs = 1500; // let a direct path win when there is one
constexpr int kWindow = 10;            // data packets in flight (camera drops bursts beyond)
constexpr qint64 kDataResendMs = 500;
constexpr qint64 kMissingResendMs = 100;
constexpr qint64 kAckMinMs = 0;
constexpr qint64 kAckIdleMs = 1000;
constexpr qint64 kHeartbeatMs = 1000;
constexpr qint64 kSilenceMs = 15000;

// Everything that touches the socket; lives on the link's worker thread.
class Engine
{
public:
    explicit Engine(const BcUdpLink::Options &o) : m_o(o) {}

    bool discover(const std::atomic<bool> *abort, const std::atomic<bool> &stop, QString *error);

    QUdpSocket sock;
    qint32 cid = 0;
    qint32 did = 0;
    quint32 sid = 0;
    Endpoint peer;
    Endpoint reg; // register server that confirmed sid (for C2R_CFM)
    QString mode;

    void send(const QByteArray &datagram, const Endpoint &to)
    {
        sock.writeDatagram(datagram, to.addr, to.port);
    }

private:
    void sendDue(qint64 now);
    void drop(const QString &key);
    void add(const QString &key, const QByteArray &xml, const Endpoint &to, quint32 tid = 0);
    void registerWith(const Endpoint &reg, const Endpoint &relay);
    bool handle(const Message &m, quint32 tid, const Endpoint &from, qint64 now);
    QHostAddress localAddressTowards(const Endpoint &to);

    BcUdpLink::Options m_o;
    qint64 m_t0 = nowMs();
    QList<Retry> m_retries;
    QSet<QString> m_registered; // register servers already asked
    bool m_lookupAnswered = false;
    int m_lastRegisterRsp = 1;  // 1 = no answer yet
    Endpoint m_relay;
    Endpoint m_dmap;            // the camera's public address, per the register server
    qint64 m_relayAt = 0;       // when to start the relay attempt (0 = not scheduled)
};

void Engine::add(const QString &key, const QByteArray &xml, const Endpoint &to, quint32 tid)
{
    if (!to.valid())
        return;
    qCDebug(lcProto) << "uid" << m_o.uid << "+" << nowMs() - m_t0 << "ms ->" << to.toString() << parseXml(xml).type << "(" << key << ")";
    m_retries.append({key, encodeDiscovery(tid ? tid : newTid(), xml), to, 0});
}

void Engine::drop(const QString &key)
{
    m_retries.removeIf([&](const Retry &r) { return r.key == key; });
}

void Engine::sendDue(qint64 now)
{
    for (Retry &r : m_retries) {
        if (r.next > now)
            continue;
        send(r.datagram, r.to);
        r.next = now + kResendMs;
    }
}

void Engine::registerWith(const Endpoint &r, const Endpoint &relay)
{
    if (m_registered.contains(r.toString()))
        return;
    m_registered.insert(r.toString());
    const QHostAddress local = localAddressTowards(r);
    add(QStringLiteral("reg"),
        Xml("C2R_C")
            .add("uid", m_o.uid.toLatin1())
            .addAddr("cli", local.toString(), sock.localPort())
            .addAddr("relay", relay.addr.toString(), relay.port)
            .add("cid", cid)
            .add("debug", 0)
            .add("family", 4)
            .add("p", "WIN")
            .add("r", 3)
            .build(),
        r);
}

QHostAddress Engine::localAddressTowards(const Endpoint &to)
{
    // The kernel's route choice tells which local address the server would see
    // as ours on the LAN side; nothing is sent.
    QUdpSocket probe;
    probe.connectToHost(to.addr, to.port);
    probe.waitForConnected(200);
    const QHostAddress a = probe.localAddress();
    return a.isNull() ? QHostAddress(QHostAddress::LocalHost) : a;
}

bool Engine::discover(const std::atomic<bool> *abort, const std::atomic<bool> &stop, QString *error)
{
    if (!sock.bind(QHostAddress::AnyIPv4, 0)) {
        *error = BcUdpLink::tr("couldn't open a UDP socket");
        return false;
    }
    // Key frames arrive as bursts of ~150 datagrams.
    sock.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, QVariant(4 * 1024 * 1024));
    cid = static_cast<qint32>(QRandomGenerator::global()->bounded(10000, 100000));
    const QByteArray uid = m_o.uid.toLatin1();

    if (m_o.lan) {
        const QByteArray c2dc = Xml("C2D_C")
                                    .add("uid", uid)
                                    .add("cli", "<port>" + QByteArray::number(sock.localPort()) + "</port>")
                                    .add("cid", cid)
                                    .add("mtu", kMtu)
                                    .add("debug", 0)
                                    .add("p", "WIN")
                                    .build();
        const quint32 tid = newTid();
        if (!m_o.lanHost.isEmpty())
            add(QStringLiteral("lan"), c2dc, {QHostAddress(m_o.lanHost), m_o.lanPort}, tid);
        QList<QHostAddress> bcast;
        if (m_o.broadcast)
            bcast.append(QHostAddress(QHostAddress::Broadcast));
        for (const QNetworkInterface &i : m_o.broadcast ? QNetworkInterface::allInterfaces()
                                                         : QList<QNetworkInterface>()) {
            if (!(i.flags() & QNetworkInterface::IsUp) || (i.flags() & QNetworkInterface::IsLoopBack))
                continue;
            for (const QNetworkAddressEntry &e : i.addressEntries())
                if (e.ip().protocol() == QAbstractSocket::IPv4Protocol && !e.broadcast().isNull())
                    bcast.append(e.broadcast());
        }
        for (const QHostAddress &a : bcast)
            for (quint16 port : {quint16(2015), quint16(2018)})
                add(QStringLiteral("lan"), c2dc, {a, port}, tid);
    }
    sendDue(nowMs());

    // Resolve the servers in parallel, each used as soon as it resolves, and
    // IPv4 only (the socket is IPv4): an unanswered AAAA query costs 5 s.
    // The threads are detached so a hung lookup never delays the connection;
    // they only touch the shared box.
    struct Resolved {
        QMutex mutex;
        QList<Endpoint> endpoints;
    };
    const auto resolved = std::make_shared<Resolved>();
    if (m_o.remote || m_o.relay)
        for (const BcUdpLink::Server &s : m_o.servers)
            std::thread([resolved, s] {
                addrinfo hints{};
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_DGRAM;
                addrinfo *res = nullptr;
                if (getaddrinfo(s.host.toUtf8().constData(), nullptr, &hints, &res) != 0 || !res)
                    return;
                const auto *in = reinterpret_cast<const sockaddr_in *>(res->ai_addr);
                const QHostAddress a(ntohl(in->sin_addr.s_addr));
                freeaddrinfo(res);
                // A DNS filter answering 127.0.0.1 / 0.0.0.0 for a blocked name.
                if (a.isLoopback() || a == QHostAddress::AnyIPv4)
                    return;
                QMutexLocker lock(&resolved->mutex);
                resolved->endpoints.append({a, s.port});
            }).detach();
    const QByteArray c2mq = Xml("C2M_Q").add("uid", uid).add("p", "WIN").build();

    // Found recently: register straight away, and only ask the lookup servers
    // if that doesn't answer soon.
    qint64 lookupFrom = 0;
    if (m_o.remote || m_o.relay) {
        KnownUid known;
        {
            QMutexLocker lock(&g_knownMutex);
            known = g_known.value(m_o.uid);
        }
        if (known.at && nowMs() - known.at < kKnownMs) {
            registerWith(known.reg, known.relay);
            lookupFrom = nowMs() + kKnownGraceMs;
        }
    }

    const qint64 deadline = nowMs() + m_o.timeoutMs;
    while (!(abort && *abort) && !stop) {
        const qint64 now = nowMs();
        if (now >= deadline)
            break;
        if (!m_lookupAnswered && sid == 0 && now >= lookupFrom) {
            QMutexLocker lock(&resolved->mutex);
            for (const Endpoint &ep : std::as_const(resolved->endpoints))
                add(QStringLiteral("lookup"), c2mq, ep);
            resolved->endpoints.clear();
        }
        if (m_relayAt && now >= m_relayAt) {
            m_relayAt = 0;
            add(QStringLiteral("relay"),
                Xml("C2D_T").add("sid", sid).add("conn", "relay").add("cid", cid).add("mtu", kMtu).build(),
                m_relay);
            qCDebug(lcProto) << "uid" << m_o.uid << ": trying the relay" << m_relay.toString();
        }
        sendDue(now);
        if (!sock.waitForReadyRead(50) && !sock.hasPendingDatagrams())
            continue;
        while (sock.hasPendingDatagrams()) {
            QHostAddress addr;
            quint16 port = 0;
            QByteArray d(int(sock.pendingDatagramSize()), Qt::Uninitialized);
            d.resize(int(sock.readDatagram(d.data(), d.size(), &addr, &port)));
            const Endpoint from{addr, port};
            for (const Packet &p : decode(d)) {
                if (p.kind != Packet::Kind::Discovery)
                    continue;
                const Message m = parseXml(p.xml);
                qCDebug(lcProto) << "uid" << m_o.uid << "+" << nowMs() - m_t0 << "ms <-" << from.toString() << m.type << m.values;
                if (handle(m, p.tid, from, nowMs()))
                    return true;
            }
        }
    }
    if ((abort && *abort) || stop)
        return false; // cancelled: no error text
    if (m_lastRegisterRsp < 0)
        *error = BcUdpLink::tr("the camera isn't reachable through Reolink's servers (offline, or UID access disabled)");
    else if (!m_lookupAnswered && !(m_o.remote || m_o.relay))
        *error = BcUdpLink::tr("the camera didn't answer on the local network");
    else if (!m_lookupAnswered && m_registered.isEmpty())
        *error = BcUdpLink::tr("Reolink's servers didn't answer");
    else
        *error = BcUdpLink::tr("no network path to the camera was found");
    return false;
}

bool Engine::handle(const Message &m, quint32 tid, const Endpoint &from, qint64 now)
{
    const QString &t = m.type;
    if (t == QLatin1String("D2C_C_R")) { // LAN: the camera answered C2D_C
        if (m.intValue(QStringLiteral("cid")) != cid || m.intValue(QStringLiteral("rsp")) != 0)
            return false;
        did = m.intValue(QStringLiteral("did"));
        peer = from;
        mode = QStringLiteral("lan");
        return true;
    }
    if (t == QLatin1String("M2C_Q_R")) { // lookup: where to register
        const Endpoint r = endpointAt(m, QStringLiteral("reg"));
        const Endpoint relay = endpointAt(m, QStringLiteral("relay"));
        if (!r.valid() || !relay.valid())
            return false;
        m_lookupAnswered = true;
        drop(QStringLiteral("lookup"));
        {
            QMutexLocker lock(&g_knownMutex);
            g_known.insert(m_o.uid, {r, relay, now});
        }
        registerWith(r, relay);
        return false;
    }
    if (t == QLatin1String("R2C_C_R") || t == QLatin1String("R2C_T")) { // register: where the camera is
        const int rsp = m.intValue(QStringLiteral("rsp"), 0);
        if (rsp < 0) {
            m_lastRegisterRsp = rsp; // not available yet; the C2R_C keeps being resent
            return false;
        }
        const quint32 s = m.value(QStringLiteral("sid")).toUInt();
        if (s == 0 || (sid && s != sid))
            return false;
        m_lastRegisterRsp = rsp;
        const bool first = sid == 0;
        sid = s;
        reg = from;
        if (!m_dmap.valid())
            m_dmap = endpointAt(m, QStringLiteral("dmap"));
        if (first && m_o.remote) {
            const Endpoint dev = endpointAt(m, QStringLiteral("dev"));
            add(QStringLiteral("local"),
                Xml("C2D_T").add("sid", sid).add("conn", "local").add("cid", cid).add("mtu", kMtu).build(),
                dev);
        }
        const Endpoint relay = endpointAt(m, QStringLiteral("relay"));
        if (t == QLatin1String("R2C_C_R") && relay.valid() && !m_relay.valid() && m_o.relay) {
            m_relay = relay;
            m_relayAt = now + (m_o.remote ? kRelayDelayMs : 0);
        }
        if (t == QLatin1String("R2C_C_R"))
            drop(QStringLiteral("reg"));
        return false;
    }
    if (t == QLatin1String("D2C_T")) { // the camera opens a path towards us
        if (!m_o.remote || m.intValue(QStringLiteral("cid")) != cid)
            return false;
        const quint32 s = m.value(QStringLiteral("sid")).toUInt();
        if (sid && s != sid)
            return false;
        add(QStringLiteral("accept"),
            Xml("C2D_A")
                .add("sid", s)
                .add("conn", m.value(QStringLiteral("conn")).toLatin1())
                .add("cid", cid)
                .add("did", m.intValue(QStringLiteral("did")))
                .add("mtu", kMtu)
                .build(),
            from, tid);
        return false;
    }
    if (t == QLatin1String("D2C_CFM")) { // a path is confirmed
        if (m.intValue(QStringLiteral("cid")) != cid || m.intValue(QStringLiteral("rsp")) != 0)
            return false;
        const quint32 s = m.value(QStringLiteral("sid")).toUInt();
        if (sid && s != sid)
            return false;
        // A "map" path is bound to the camera's public address: confirmed from
        // anywhere else (the camera's LAN address when we share its network),
        // the camera expects our data from our public address and drops it.
        if (m.value(QStringLiteral("conn")) == QLatin1String("map") && !(from == m_dmap))
            return false;
        sid = s;
        did = m.intValue(QStringLiteral("did"));
        peer = from;
        mode = m.value(QStringLiteral("conn"));
        return true;
    }
    return false;
}

} // namespace

QList<BcUdpLink::Server> BcUdpLink::defaultServers()
{
    // The official client knows p2p1–p2p9 by region (France = p2p7); asking all
    // of them finds the camera whichever region it registered in.
    QList<Server> s{{QStringLiteral("p2p.reolink.com")}};
    for (int i = 1; i <= 11; ++i)
        s.append({QStringLiteral("p2p%1.reolink.com").arg(i)});
    return s;
}

BcUdpLink::BcUdpLink(const Options &options) : m_opt(options) {}

std::unique_ptr<BcUdpLink> BcUdpLink::connect(const Options &options, const std::atomic<bool> *abort,
                                              QString *error)
{
    std::unique_ptr<BcUdpLink> link(new BcUdpLink(options));
    BcUdpLink *l = link.get();
    l->m_thread = std::thread([l, abort] { l->run(abort); });
    QMutexLocker lock(&l->m_mutex);
    while (l->m_state == State::Connecting)
        l->m_cond.wait(&l->m_mutex);
    if (l->m_state != State::Connected) {
        if (error)
            *error = l->m_error;
        lock.unlock();
        return nullptr; // the destructor joins the finished worker
    }
    return link;
}

BcUdpLink::~BcUdpLink()
{
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();
}

void BcUdpLink::setState(State s, const QString &error)
{
    QMutexLocker lock(&m_mutex);
    m_state = s;
    if (!error.isEmpty())
        m_error = error;
    m_cond.wakeAll();
}

void BcUdpLink::run(const std::atomic<bool> *abort)
{
    Engine e(m_opt);
    QString error;
    if (!e.discover(abort, m_stop, &error)) {
        qCInfo(lcProto) << "uid" << m_opt.uid << ": no connection:" << error;
        setState(State::Closed, error.isEmpty() ? tr("cancelled") : error);
        return;
    }
    qCInfo(lcProto) << "uid" << m_opt.uid << ": connected," << e.mode << "via" << e.peer.toString();
    {
        QMutexLocker lock(&m_mutex);
        m_mode = e.mode;
        m_peerHost = e.peer.addr.toString();
    }
    setState(State::Connected);

    struct Inflight {
        QByteArray datagram;
        qint64 sentAt = 0;
    };
    std::map<quint32, QByteArray> queued;    // cut into packets, waiting for window room
    std::map<quint32, Inflight> inflight;    // sent, not acknowledged yet
    std::map<quint32, QByteArray> early;     // received ahead of a missing packet
    quint32 nextTx = 0;
    quint32 rxWant = 0;
    bool ackDue = false;
    qint64 lastAck = 0;
    qint64 lastHb = 0;
    qint64 lastHeard = nowMs();
    int confirmsLeft = e.sid ? 5 : 0; // C2R_CFM to the register server
    qint64 nextConfirm = 0;
    QString closeReason;
    quint64 nSent = 0, nResent = 0, nRecv = 0, nDup = 0;

    const QByteArray cfm = Xml("C2R_CFM")
                               .add("sid", e.sid)
                               .add("conn", e.mode.toLatin1())
                               .add("rsp", 0)
                               .add("cid", e.cid)
                               .add("did", e.did)
                               .build();
    const QByteArray hb = Xml("C2D_HB").add("cid", e.cid).add("did", e.did).build();

    while (!m_stop) {
        const qint64 now = nowMs();
        QByteArray tx;
        {
            QMutexLocker lock(&m_mutex);
            tx.swap(m_tx);
        }
        for (int at = 0; at < tx.size(); at += kMaxChunk)
            queued.emplace(nextTx++, tx.mid(at, kMaxChunk));
        while (!queued.empty() && int(inflight.size()) < kWindow) {
            auto it = queued.begin();
            const QByteArray d = encodeData(e.did, it->first, it->second);
            e.send(d, e.peer);
            ++nSent;
            inflight.emplace(it->first, Inflight{d, now});
            queued.erase(it);
        }
        for (auto &[id, f] : inflight)
            if (now - f.sentAt >= kDataResendMs) {
                e.send(f.datagram, e.peer);
                ++nResent;
                f.sentAt = now;
            }
        const bool haveRx = rxWant > 0 || !early.empty();
        if (haveRx && ((ackDue && now - lastAck >= kAckMinMs) || now - lastAck >= kAckIdleMs)) {
            QByteArray bitmap;
            if (!early.empty())
                for (quint32 id = rxWant; id <= early.rbegin()->first; ++id)
                    bitmap.append(char(early.count(id) ? 1 : 0));
            const quint32 last = rxWant ? rxWant - 1 : 0xffffffffu;
            e.send(encodeAck(e.did, last, bitmap, 0, rxWant ? 0 : 0xffffffffu), e.peer);
            lastAck = now;
            ackDue = false;
        }
        if (now - lastHb >= kHeartbeatMs) {
            e.send(encodeDiscovery(newTid(), hb), e.peer);
            lastHb = now;
        }
        if (confirmsLeft > 0 && now >= nextConfirm) {
            e.send(encodeDiscovery(newTid(), cfm), e.reg);
            --confirmsLeft;
            nextConfirm = now + kResendMs;
        }
        if (now - lastHeard > kSilenceMs) {
            closeReason = tr("the connection to the camera was lost");
            break;
        }

        if (!e.sock.waitForReadyRead(queued.empty() && !ackDue ? 10 : 2) && !e.sock.hasPendingDatagrams())
            continue;
        QByteArray delivered;
        while (e.sock.hasPendingDatagrams()) {
            QHostAddress addr;
            quint16 port = 0;
            QByteArray d(int(e.sock.pendingDatagramSize()), Qt::Uninitialized);
            d.resize(int(e.sock.readDatagram(d.data(), d.size(), &addr, &port)));
            for (const Packet &p : decode(d)) {
                const qint64 t = nowMs();
                if (p.kind == Packet::Kind::Data) {
                    if (p.connId != e.cid)
                        continue;
                    lastHeard = t;
                    ackDue = true; // also re-acknowledges duplicates whose ACK was lost
                    ++nRecv;
                    if (p.packetId < rxWant || early.count(p.packetId)) {
                        ++nDup;
                        continue;
                    }
                    early.emplace(p.packetId, p.payload);
                    for (auto it = early.find(rxWant); it != early.end(); it = early.find(rxWant)) {
                        delivered += it->second;
                        early.erase(it);
                        ++rxWant;
                    }
                } else if (p.kind == Packet::Kind::Ack) {
                    if (p.connId != e.cid)
                        continue;
                    lastHeard = t;
                    if (p.packetId != 0xffffffffu)
                        inflight.erase(inflight.begin(), inflight.upper_bound(p.packetId));
                    for (int i = 0; i < p.payload.size(); ++i) {
                        const quint32 id = p.packetId + 1 + quint32(i);
                        auto it = inflight.find(id);
                        if (it == inflight.end())
                            continue;
                        if (p.payload[i]) {
                            inflight.erase(it);
                        } else if (t - it->second.sentAt >= kMissingResendMs) {
                            e.send(it->second.datagram, e.peer);
                            it->second.sentAt = t;
                            ++nResent;
                        }
                    }
                } else {
                    const Message m = parseXml(p.xml);
                    if (m.type == QLatin1String("D2C_HB")) {
                        lastHeard = t;
                    } else if (m.type == QLatin1String("D2C_DISC") || m.type == QLatin1String("R2C_DISC")) {
                        qCInfo(lcProto) << "uid" << m_opt.uid << ":" << m.type << "from" << addr.toString();
                        closeReason = tr("the camera closed the connection");
                    }
                }
            }
        }
        if (!delivered.isEmpty()) {
            QMutexLocker lock(&m_mutex);
            m_rx += delivered;
            m_cond.wakeAll();
        }
        if (!closeReason.isEmpty())
            break;
    }

    qCDebug(lcProto) << "uid" << m_opt.uid << ": data packets sent" << nSent << "resent" << nResent
                     << "received" << nRecv << "duplicates" << nDup;
    if (closeReason.isEmpty()) { // we are closing: tell the camera
        const QByteArray disc = encodeDiscovery(
            newTid(), Xml("C2D_DISC").add("cid", e.cid).add("did", e.did).build());
        for (int i = 0; i < 3; ++i)
            e.send(disc, e.peer);
        e.sock.waitForBytesWritten(100);
    } else {
        qCInfo(lcProto) << "uid" << m_opt.uid << ":" << closeReason;
    }
    setState(State::Closed, closeReason);
}

bool BcUdpLink::write(const QByteArray &bytes)
{
    QMutexLocker lock(&m_mutex);
    if (m_state != State::Connected)
        return false;
    m_tx += bytes;
    return true;
}

bool BcUdpLink::waitForReadyRead(int ms)
{
    QMutexLocker lock(&m_mutex);
    if (m_rx.isEmpty() && m_state == State::Connected)
        m_cond.wait(&m_mutex, static_cast<unsigned long>(qMax(0, ms)));
    return !m_rx.isEmpty();
}

QByteArray BcUdpLink::readAll()
{
    QMutexLocker lock(&m_mutex);
    QByteArray out;
    out.swap(m_rx);
    return out;
}

qint64 BcUdpLink::bytesAvailable() const
{
    QMutexLocker lock(&m_mutex);
    return m_rx.size();
}

bool BcUdpLink::isConnected() const
{
    QMutexLocker lock(&m_mutex);
    return m_state == State::Connected;
}

QString BcUdpLink::description() const
{
    return QStringLiteral("uid %1").arg(mode());
}

QString BcUdpLink::lanHost() const
{
    QMutexLocker lock(&m_mutex);
    return m_mode == QLatin1String("lan") || m_mode == QLatin1String("local") ? m_peerHost : QString();
}

QString BcUdpLink::mode() const
{
    QMutexLocker lock(&m_mutex);
    return m_mode;
}

} // namespace rl
