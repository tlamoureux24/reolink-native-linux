#pragma once

#include "protocol/BcLink.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QWaitCondition>

#include <atomic>
#include <memory>
#include <thread>

// Baichuan over UDP to a camera found by its UID: on the LAN (C2D_C to the
// camera or broadcast), or through Reolink's P2P servers (lookup, register,
// then a direct "local"/"map" path or the Reolink relay). Once connected it is
// a reliable, ordered byte stream carrying the same Baichuan bytes as TCP 9000.
// Specification: docs/research/p2p.md.
//
// Threading: one worker thread owns the socket for the whole life of the link
// (discovery, ACKs every few ms, resends, heartbeats), so the camera keeps
// getting answers even while the consumer is busy. The consumer side (BcLink)
// is a mutex-guarded byte queue in each direction.
//
// Clean-room: from MIT reolink_aio and the official client's SDK strings. No
// AGPL neolink code.
namespace rl {

class BcUdpLink : public BcLink
{
    Q_DECLARE_TR_FUNCTIONS(BcUdpLink)

public:
    struct Server {
        QString host;
        quint16 port = 9999;
    };
    struct Options {
        QString uid;
        QString lanHost;           // camera address when known: C2D_C straight to it
        quint16 lanPort = 2015;
        bool lan = true;           // C2D_C to lanHost (and broadcast, below)
        bool broadcast = true;     // also C2D_C to the LAN broadcast addresses
        bool remote = true;        // Reolink servers, direct paths (local / map)
        bool relay = true;         // Reolink relay as the last resort
        int timeoutMs = 20000;
        QList<Server> servers = defaultServers();
    };
    static QList<Server> defaultServers();

    // Blocking connect; nullptr on failure (`error` says why). `abort` cancels.
    static std::unique_ptr<BcUdpLink> connect(const Options &options,
                                              const std::atomic<bool> *abort = nullptr,
                                              QString *error = nullptr);
    ~BcUdpLink() override;

    bool write(const QByteArray &bytes) override;
    bool waitForReadyRead(int ms) override;
    QByteArray readAll() override;
    qint64 bytesAvailable() const override;
    bool isConnected() const override;
    QString description() const override;

    // How the camera was reached: "lan", "local", "map" or "relay".
    QString mode() const override;
    QString lanHost() const override;

private:
    enum class State { Connecting, Connected, Closed };

    explicit BcUdpLink(const Options &options);
    void run(const std::atomic<bool> *abort);
    void setState(State s, const QString &error = {});

    Options m_opt;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};

    mutable QMutex m_mutex;
    QWaitCondition m_cond;
    State m_state = State::Connecting;
    QString m_error;
    QString m_mode;
    QString m_peerHost; // the camera's address on this link
    QByteArray m_rx; // in-order bytes from the camera, not yet read
    QByteArray m_tx; // bytes to send, not yet cut into packets
};

} // namespace rl
