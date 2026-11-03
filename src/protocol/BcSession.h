#pragma once

#include "protocol/BcLink.h"

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QWaitCondition>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

// One logged-in Baichuan session per device, shared by everything that talks to
// it — the way the official client works: its SDK opens a device once
// (BCSDK_AddDevice / BCSDK_Open) and opens live view, talk and requests inside
// that session (BCSDK_LiveOpen posts a command to the device's session queue;
// BaichuanPreviewer gives each stream an id of its own, opens it with cmd 3 and
// closes it with cmd 4 under the same id).
//
// Every device has one, over TCP 9000 or by UID (P2P): the client talks to
// devices only through it, as the official client does.
//
// Like the SDK, it sends a heartbeat (cmd 93) after 5 s without traffic and
// gives the device up after 20 s of silence.
//
// Messages are told apart by their 24-bit message id (header bytes 13–15),
// which the device echoes on replies and on a stream's media.
//
// Threading: a worker thread connects, logs in, then reads and dispatches every
// message: to the route registered for its id, else to a request waiting for
// that command. Routes run on the worker and must not block for long.
namespace rl {

struct BcMessage {
    quint32 cmd = 0;
    quint8 channel = 0;
    quint32 messId = 0; // 24 bits
    quint16 status = 0;
    quint16 cls = 0;
    quint32 payloadOffset = 0;
    QByteArray body;
};

class BcSession
{
public:
    struct Params {
        BcTarget target;
        QString username;
        QString password;
    };
    enum class Failure { None, Unreachable, Rejected };

    // The device's session: the live one if any (also one closed less than a
    // few seconds ago is kept for reuse), else a new one, connecting now.
    static std::shared_ptr<BcSession> acquire(const Params &params);
    ~BcSession();

    // Wait for connect + login; false on failure (see failure()/error()).
    bool waitReady(int timeoutMs, const std::atomic<bool> *abort = nullptr);
    bool isAlive() const;
    // End the session now (its streams and requests end with it), e.g. to
    // reopen it over a better link; `reason` becomes its error().
    void close(const QString &reason);
    Failure failure() const;
    QString error() const;

    QByteArray aesKey() const { return m_aesKey; }
    QByteArray loginInfo() const { return m_loginInfo; } // decrypted login reply
    QString linkMode() const;                              // see BcLink::mode()
    QString linkLanHost() const;
    QString linkDescription() const;

    // A message id unused in this session; `low` fills header byte 13 (the
    // live stream type for cmd 3, as the client always sent it).
    quint32 newMessId(quint8 low = 0);

    // Send one modern (class 0x6414) message; ext/body already ciphered.
    bool send(quint32 cmd, quint8 channel, quint32 messId, const QByteArray &ext,
              const QByteArray &body);

    // Every message carrying `messId` goes to `fn` until unroute(), which
    // returns once `fn` is no longer running (so its captures may go away).
    void route(quint32 messId, std::function<void(const BcMessage &)> fn);
    void unroute(quint32 messId);

    // Messages nobody asked for (no route, no waiting request): the device's
    // own pushes, such as alarm events (cmd 33) after a subscription (cmd 31).
    // Handlers run on the worker. Returns a token for removePushHandler(),
    // which returns once the handler is no longer running.
    int addPushHandler(std::function<void(const BcMessage &)> fn);
    void removePushHandler(int token);

    // Send and wait for the reply (same id, or else the same command).
    bool request(quint32 cmd, quint8 channel, const QByteArray &ext, const QByteArray &body,
                 BcMessage *reply, int timeoutMs = 12000, quint32 *messIdOut = nullptr);

    // request() with plain XML: ciphers the extension and body, deciphers the
    // reply's body into `replyXml`. False if no reply came; `status` is the
    // reply's (200 = OK).
    bool requestXml(quint32 cmd, quint8 channel, const QByteArray &extXml, const QByteArray &bodyXml,
                    QByteArray *replyXml, quint16 *status = nullptr, int timeoutMs = 12000,
                    quint32 *messIdOut = nullptr);
    // The extension a channel-scoped command carries (0-based channel id).
    static QByteArray channelExtension(int channel);
    // Decipher a message's body (without its extension).
    QByteArray bodyXml(const BcMessage &m) const;

    // The latest push of this command the device sent (true if any), such as
    // an NVR's channel states (cmd 145), which it sends once after login.
    bool lastPush(quint32 cmd, BcMessage *out) const;

private:
    explicit BcSession(const Params &params);
    void run();
    bool login();
    void dispatch(const BcMessage &m);
    void finish(Failure f, const QString &error);

    struct Pending {
        quint32 cmd = 0;
        quint32 messId = 0;
        bool done = false;
        BcMessage reply;
    };

    Params m_p;
    std::unique_ptr<BcLink> m_link;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};

    mutable QMutex m_mutex; // state, routes, pending requests
    QWaitCondition m_cond;
    enum class State { Connecting, Ready, Closed } m_state = State::Connecting;
    Failure m_failure = Failure::None;
    QString m_error;
    QByteArray m_aesKey;
    QByteArray m_loginInfo;
    quint32 m_nextId = 1;
    QHash<quint32, std::function<void(const BcMessage &)>> m_routes;
    QHash<int, std::function<void(const BcMessage &)>> m_pushHandlers;
    int m_nextPushToken = 1;
    bool m_inPush = false;
    quint32 m_inRoute = 0; // id whose route is running now (0 = none)
    std::thread::id m_workerId;
    QList<Pending *> m_pending;
    QHash<quint32, BcMessage> m_lastPush;
    QMutex m_writeMutex;
    QByteArray m_pendingBytes; // read past the login reply, handed to the loop

    friend struct BcSessionRegistry;
};

} // namespace rl
