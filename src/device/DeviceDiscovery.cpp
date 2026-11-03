#include "DeviceDiscovery.h"

#include "core/Log.h"

#include <QNetworkInterface>
#include <QTcpSocket>
#include <QtConcurrent/QtConcurrent>


namespace rl {

namespace {

constexpr int kPortConnectMs = 500; // per-host TCP connect timeout to :9000
constexpr int kReolinkPort = 9000;  // Baichuan — a strong Reolink signature

// Confirm the host really speaks Baichuan, as the official client finds a
// device: the legacy hello (cmd 1, class 0x6514) asks for a login nonce, and a
// Reolink device answers with a message carrying the Baichuan magic.
bool confirmReolink(const QString &ip)
{
    QTcpSocket sock;
    sock.connectToHost(ip, static_cast<quint16>(kReolinkPort));
    if (!sock.waitForConnected(kPortConnectMs))
        return false;
    QByteArray hello;
    const quint32 magic = 0x0ABCDEF0;
    const quint32 cmd = 1;
    hello.append(reinterpret_cast<const char *>(&magic), 4); // little-endian hosts
    hello.append(reinterpret_cast<const char *>(&cmd), 4);
    hello.append(QByteArray(4, '\0'));                     // no payload
    hello.append(QByteArray(4, '\0'));                     // channel 0, message id 0
    hello.append("\x12\xdc\x14\x65", 4);                   // status 0xdc12, class 0x6514
    sock.write(hello);
    sock.flush();
    QByteArray reply;
    while (reply.size() < 4 && sock.waitForReadyRead(1500))
        reply += sock.readAll();
    return reply.startsWith(QByteArray("\xf0\xde\xbc\x0a", 4));
}

// All host IPs on the machine's IPv4 /24 subnets (private ranges only).
QStringList localSubnetHosts()
{
    QStringList ips;
    QSet<QString> prefixes;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            (iface.flags() & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol || addr.isLoopback())
                continue;
            const quint32 v4 = addr.toIPv4Address();
            // Private ranges only (10/8, 172.16/12, 192.168/16).
            const bool priv = (v4 >> 24) == 10 || (v4 >> 20) == 0xAC1 || (v4 >> 16) == 0xC0A8;
            if (!priv)
                continue;
            const QString prefix = addr.toString().section(u'.', 0, 2); // a.b.c
            prefixes.insert(prefix);
        }
    }
    for (const QString &prefix : prefixes)
        for (int host = 1; host <= 254; ++host)
            ips.append(prefix + u'.' + QString::number(host));
    return ips;
}

} // namespace

DeviceDiscovery::DeviceDiscovery(QObject *parent) : QObject(parent) {}

DeviceDiscovery::~DeviceDiscovery() = default;

void DeviceDiscovery::setScanning(bool s)
{
    if (m_scanning == s)
        return;
    m_scanning = s;
    emit scanningChanged();
    if (!s)
        emit scanFinished();
}

void DeviceDiscovery::reportFound(const QString &ip, const QString &info)
{
    if (!m_found.contains(ip)) {
        m_found.append(ip);
        emit deviceFound(ip, info);
    }
}

void DeviceDiscovery::scan()
{
    if (m_scanning)
        return;
    m_found.clear();
    setScanning(true);

    const QStringList hosts = localSubnetHosts();
    m_pending = hosts.size();
    if (m_pending == 0) {
        setScanning(false);
        return;
    }
    qCInfo(lcCore) << "discovery: scanning" << m_pending << "hosts for Reolink devices";

    // A dedicated pool so 254 network probes run wide (I/O-bound, not CPU-bound)
    // without starving the app's other worker tasks.
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(48);
        return p;
    }();

    for (const QString &ip : hosts) {
        auto *watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
            const QString ip = watcher->result();
            watcher->deleteLater();
            if (!ip.isEmpty())
                reportFound(ip, QStringLiteral("baichuan"));
            if (--m_pending <= 0)
                setScanning(false);
        });
        watcher->setFuture(QtConcurrent::run(pool, [ip]() -> QString {
            if (confirmReolink(ip))
                return ip;
            return {};
        }));
    }
}

} // namespace rl
