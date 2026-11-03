#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
#include <memory>

// A connected Baichuan byte pipe. Over TCP port 9000 on the LAN, or over
// Baichuan-UDP when the camera is reached by UID (LAN broadcast or Reolink's
// P2P servers, see BcUdpLink). The Baichuan framing, login and AES above it are
// identical, so BaichuanControl / BaichuanClient / TalkSession only see this.
//
// Blocking, single-consumer: used from one worker thread at a time.
namespace rl {

class BcLink
{
public:
    virtual ~BcLink() = default;

    virtual bool write(const QByteArray &bytes) = 0;
    // Wait up to `ms` for incoming bytes; true once some are available.
    virtual bool waitForReadyRead(int ms) = 0;
    virtual QByteArray readAll() = 0;
    virtual qint64 bytesAvailable() const = 0;
    virtual bool isConnected() const = 0;
    // For logs and the UI: "tcp 192.168.1.20:9000", "uid lan", "uid relay"…
    virtual QString description() const = 0;
    // How the device was reached: "tcp", or for a UID link "lan", "local",
    // "map" or "relay".
    virtual QString mode() const = 0;
    // The device's address on this link when it is a LAN address (TCP, or a
    // UID link in "lan"/"local" mode); empty when reached through the internet.
    virtual QString lanHost() const = 0;
};

// Where a device's Baichuan service is.
struct BcTarget {
    QString host;   // LAN address (TCP, or a hint for a UID connection)
    int port = 9000;
    QString uid;    // camera UID, for a connection by UID
    bool viaUid = false;
};

// Open a link: by UID only when viaUid; else TCP to the address, and when that
// fails and a UID is known, by UID (P2P) — one device, whichever way reaches
// it. Blocking (up to ~5 s for TCP, ~20 s by UID); `abort` cancels it early.
std::unique_ptr<BcLink> openBcLink(const BcTarget &target, const std::atomic<bool> *abort = nullptr,
                                   QString *error = nullptr);

} // namespace rl
