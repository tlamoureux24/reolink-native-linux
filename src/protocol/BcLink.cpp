#include "BcLink.h"

#include "protocol/BcUdpLink.h"

#include <QCoreApplication>
#include <QTcpSocket>

namespace rl {

namespace {

class BcTcpLink : public BcLink
{
public:
    explicit BcTcpLink(const BcTarget &t)
        : m_desc(QStringLiteral("tcp %1:%2").arg(t.host).arg(t.port)), m_host(t.host)
    {
        m_sock.connectToHost(t.host, static_cast<quint16>(t.port));
    }
    ~BcTcpLink() override { m_sock.disconnectFromHost(); }

    bool waitForConnected(int ms) { return m_sock.waitForConnected(ms); }

    bool write(const QByteArray &bytes) override
    {
        if (m_sock.write(bytes) != bytes.size())
            return false;
        m_sock.flush();
        return true;
    }
    bool waitForReadyRead(int ms) override
    {
        return m_sock.bytesAvailable() > 0 || m_sock.waitForReadyRead(ms);
    }
    QByteArray readAll() override { return m_sock.readAll(); }
    qint64 bytesAvailable() const override { return m_sock.bytesAvailable(); }
    bool isConnected() const override { return m_sock.state() == QAbstractSocket::ConnectedState; }
    QString description() const override { return m_desc; }
    QString mode() const override { return QStringLiteral("tcp"); }
    QString lanHost() const override { return m_host; }

private:
    QTcpSocket m_sock;
    QString m_desc;
    QString m_host;
};

} // namespace

std::unique_ptr<BcLink> openBcLink(const BcTarget &target, const std::atomic<bool> *abort,
                                   QString *error)
{
    if (target.viaUid) {
        BcUdpLink::Options o;
        o.uid = target.uid;
        o.lanHost = target.host;
        // Diagnostic: RL_P2P_MODES=lan,remote,relay limits the paths tried.
        const QByteArray modes = qgetenv("RL_P2P_MODES");
        if (!modes.isEmpty()) {
            o.lan = modes.contains("lan");
            o.remote = modes.contains("remote");
            o.relay = modes.contains("relay");
        }
        return BcUdpLink::connect(o, abort, error);
    }
    auto link = std::make_unique<BcTcpLink>(target);
    if (!link->waitForConnected(5000)) {
        if (error)
            *error = QCoreApplication::translate("BcLink", "couldn't reach the device");
        return nullptr;
    }
    return link;
}

} // namespace rl
