#include "BcLink.h"

#include "protocol/BcUdpLink.h"

#include <QCoreApplication>
#include <QMutex>
#include <QElapsedTimer>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rl {

namespace {

// A plain socket rather than a QTcpSocket: a device's session writes from any
// thread (every request, every stream's commands) while its worker reads, and
// a QTcpSocket may only be used from the thread that owns it.
class BcTcpLink : public BcLink
{
public:
    explicit BcTcpLink(const BcTarget &t)
        : m_desc(QStringLiteral("tcp %1:%2").arg(t.host).arg(t.port)), m_host(t.host)
    {
    }
    ~BcTcpLink() override
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }

    // Blocking connect, `ms` at most, cancelled by `abort`.
    bool connectTo(const BcTarget &t, int ms, const std::atomic<bool> *abort)
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        if (::getaddrinfo(t.host.toUtf8().constData(), QByteArray::number(t.port).constData(),
                          &hints, &res) != 0)
            return false;
        QElapsedTimer clock;
        clock.start();
        for (addrinfo *ai = res; ai && m_fd < 0; ai = ai->ai_next) {
            const int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
            if (fd < 0)
                continue;
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            bool ok = ::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0;
            if (!ok && errno == EINPROGRESS) {
                pollfd p{fd, POLLOUT, 0};
                while (!ok && clock.elapsed() < ms && !(abort && *abort)) {
                    if (::poll(&p, 1, 100) > 0) {
                        int err = 0;
                        socklen_t len = sizeof err;
                        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                        if (err != 0)
                            break;
                        ok = true;
                    }
                }
            }
            if (ok)
                m_fd = fd;
            else
                ::close(fd);
        }
        ::freeaddrinfo(res);
        m_connected = m_fd >= 0;
        return m_connected;
    }

    bool write(const QByteArray &bytes) override
    {
        QMutexLocker lock(&m_writeMutex);
        const char *p = bytes.constData();
        qsizetype left = bytes.size();
        while (left > 0 && m_connected) {
            const ssize_t n = ::send(m_fd, p, size_t(left), MSG_NOSIGNAL);
            if (n > 0) {
                p += n;
                left -= n;
            } else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                pollfd w{m_fd, POLLOUT, 0};
                if (::poll(&w, 1, 5000) <= 0) {
                    m_connected = false; // the device stopped taking data
                    return false;
                }
            } else {
                m_connected = false;
                return false;
            }
        }
        return left == 0;
    }
    // Reads happen on the session's worker only.
    bool waitForReadyRead(int ms) override
    {
        if (!m_buf.isEmpty())
            return true;
        if (!m_connected)
            return false;
        pollfd r{m_fd, POLLIN, 0};
        if (::poll(&r, 1, ms) <= 0)
            return false;
        char chunk[65536];
        const ssize_t n = ::recv(m_fd, chunk, sizeof chunk, 0);
        if (n > 0) {
            m_buf.append(chunk, n);
            return true;
        }
        if (n == 0 || (errno != EAGAIN && errno != EINTR))
            m_connected = false; // closed by the device, or broken
        return false;
    }
    QByteArray readAll() override
    {
        QByteArray out;
        out.swap(m_buf);
        return out;
    }
    qint64 bytesAvailable() const override { return m_buf.size(); }
    bool isConnected() const override { return m_connected || !m_buf.isEmpty(); }
    QString description() const override { return m_desc; }
    QString mode() const override { return QStringLiteral("tcp"); }
    QString lanHost() const override { return m_host; }

private:
    int m_fd = -1;
    std::atomic<bool> m_connected{false};
    QMutex m_writeMutex;
    QByteArray m_buf; // worker only
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
    if (!link->connectTo(target, 5000, abort)) {
        if (error)
            *error = QCoreApplication::translate("BcLink", "couldn't reach the device");
        return nullptr;
    }
    return link;
}

} // namespace rl
