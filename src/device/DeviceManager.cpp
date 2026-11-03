#include "DeviceManager.h"

#include "media/TalkSession.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "media/StreamPlayer.h"
#include "protocol/BaichuanClient.h"
#include "protocol/BaichuanControl.h"
#include "protocol/BcDevice.h"
#include "protocol/BcRecordings.h"
#include "protocol/BcSession.h"
#include "protocol/ReolinkHttpClient.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QUrl>
#include <QVariant>
#include <QThread>
#include <QtConcurrent/QtConcurrent>

namespace rl {

namespace {
// Per-camera manual view-rotation fix (issue #3), keyed by identity rather
// than row because rows shift when devices come and go.
QString rotationKey(qint64 hostId, int channel)
{
    return QStringLiteral("rotation/%1:%2").arg(hostId).arg(channel);
}
int loadRotation(qint64 hostId, int channel)
{
    return QSettings().value(rotationKey(hostId, channel), 0).toInt();
}
} // namespace


namespace {
} // namespace

DeviceManager::DeviceManager(Database *db, CredentialStore *credentials, QObject *parent)
    : QAbstractListModel(parent), m_db(db), m_credentials(credentials)
{
    const QVector<HostRecord> stored = m_db->hosts();
    m_entries.reserve(stored.size());
    for (const HostRecord &rec : stored) {
        Entry e;
        e.rec = rec;
        e.chanName = rec.name;
        e.rotationOverride = loadRotation(rec.id, 0);
        e.online = rec.kind == QLatin1String("stream");
        e.status = e.online ? tr("ready") : tr("connecting…");
        m_entries.append(e);
    }
    // Prime credentials + refresh status for stored camera/NVR devices on startup
    // (also enumerates NVR channels and loads passwords into memory).
    QSet<qint64> validated;
    for (const Entry &e : m_entries)
        if (!validated.contains(e.rec.id)) {
            validated.insert(e.rec.id);
            validateAsync(e.rec.id);
        }

    // 10s interval also delays the first poll to ~10s, so it doesn't compete with
    // the startup validation burst for the NVR's limited connection slots.
    m_pollTimer.setInterval(10000);
    connect(&m_pollTimer, &QTimer::timeout, this, &DeviceManager::pollDetections);
    m_pollTimer.start();

    m_sessionTimer.setInterval(1000);
    connect(&m_sessionTimer, &QTimer::timeout, this, &DeviceManager::watchSessions);
    m_sessionTimer.start();
}

DeviceManager::~DeviceManager()
{
    m_pending.waitForFinished();
    for (const qint64 hostId : m_sessions.keys())
        dropSession(hostId);
}

static QString detKey(qint64 hostId, int channel)
{
    return QString::number(hostId) + u':' + QString::number(channel);
}

void DeviceManager::applyChannelStates(qint64 hostId, const QHash<int, bool> &chanOnline)
{
    // Channel-level (NVRs): flip individual cameras that dropped or returned.
    for (auto it = chanOnline.constBegin(); it != chanOnline.constEnd(); ++it) {
        for (int i = 0; i < m_entries.size(); ++i) {
            Entry &e = m_entries[i];
            if (e.rec.id != hostId || e.channel != it.key())
                continue;
            if (e.online != it.value()) {
                e.online = it.value();
                e.status = it.value() ? tr("online") : tr("offline");
                emit dataChanged(index(i), index(i));
                emit connectivityChanged(hostId, e.channel, e.chanName, it.value());
            }
            break;
        }
    }
}

void DeviceManager::dropSession(qint64 hostId)
{
    const HostSession hs = m_sessions.take(hostId);
    if (hs.session && hs.pushToken)
        hs.session->removePushHandler(hs.pushToken);
}

void DeviceManager::watchSessions()
{
    // A session ends when the device stops answering (20 s of silence despite
    // heartbeats, as the official SDK) or closes the connection: the host goes
    // offline at once and is revalidated with the usual backoff.
    const QList<qint64> hosts = m_sessions.keys();
    for (const qint64 hostId : hosts) {
        const HostSession hs = m_sessions.value(hostId);
        if (hs.session->isAlive())
            continue;
        qCInfo(lcCore) << "host" << hostId << "lost:" << hs.session->error();
        dropSession(hostId);
        QString hostName;
        for (int i = 0; i < m_entries.size(); ++i) {
            Entry &e = m_entries[i];
            if (e.rec.id != hostId)
                continue;
            hostName = e.rec.name;
            if (!e.online)
                continue;
            e.online = false;
            e.status = tr("unreachable");
            e.problem = Problem::Unreachable;
            emit dataChanged(index(i), index(i));
        }
        if (!m_lostHosts.contains(hostId)) {
            m_lostHosts.insert(hostId);
            emit connectivityChanged(hostId, -1, hostName, false);
        }
        m_retryDelaySecs[hostId] = 15;
        validateAsync(hostId);
    }
}

void DeviceManager::pollDetections()
{
    if (!m_pushWarmed)
        warmPushCache();

    // Recovery probes ride the poll tick: hosts that failed with a TRANSPORT
    // problem revalidate when their backoff expires, so an NVR that was off at
    // app launch appears by itself once it's back — previously a host with no
    // client was skipped by everything and stayed "connecting…" until restart.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto it = m_nextRetryAt.begin(); it != m_nextRetryAt.end();) {
        if (it.value() <= now) {
            const qint64 hostId = it.key();
            it = m_nextRetryAt.erase(it);
            validateAsync(hostId);
        } else {
            ++it;
        }
    }

    // Poll each HOST with a SINGLE batched request covering all its online
    // channels — Reolink NVRs are connection-limited, so opening one connection
    // per channel (5+ at once) starves live/playback streams. One connection per
    // host per cycle keeps capacity free.
    struct HostPoll {
        std::shared_ptr<ReolinkHttpClient> client;
        QVector<int> channels;
        QVector<bool> wantAi;
        QVector<QString> names;
    };
    QHash<qint64, HostPoll> byHost;
    QVector<qint64> order;
    for (const Entry &e : m_entries) {
        // Offline entries stay in the poll: that's how we notice recovery.
        if (e.rec.kind == QLatin1String("stream") || !e.client)
            continue;
        if (!byHost.contains(e.rec.id)) {
            byHost[e.rec.id] = HostPoll{e.client, {}, {}, {}};
            order.append(e.rec.id);
        }
        HostPoll &hp = byHost[e.rec.id];
        hp.channels.append(e.channel);
        hp.wantAi.append(e.caps.ai);
        hp.names.append(e.chanName);
    }

    for (qint64 hostId : order) {
        const QString key = QString::number(hostId);
        if (m_pollInFlight.value(key, false))
            continue;
        m_pollInFlight[key] = true;
        const HostPoll hp = byHost.value(hostId);

        m_pending.addFuture(QtConcurrent::run([this, hostId, key, hp] {
            // Build one ordered batch; remember what each entry maps to.
            struct Item { int channel; bool isAi; };
            QVector<Item> items;
            Json cmds = Json::array();
            for (int i = 0; i < hp.channels.size(); ++i) {
                const int ch = hp.channels[i];
                items.append({ch, false});
                cmds.push_back(api::command(QStringLiteral("GetMdState"), Json{{"channel", ch}}));
                if (hp.wantAi[i]) {
                    items.append({ch, true});
                    cmds.push_back(
                        api::command(QStringLiteral("GetAiState"), Json{{"channel", ch}}));
                }
            }
            // Online state comes from the host's Baichuan session, not from here.
            const api::BatchResult batch = hp.client->call(cmds);

            // Combine per-channel md/ai (results preserve request order).
            QHash<int, api::DetectionState> states;
            if (batch.transportOk) {
                for (int i = 0; i < batch.results.size() && i < items.size(); ++i) {
                    const api::CommandResult &r = batch.results[i];
                    if (!r.ok)
                        continue;
                    api::DetectionState &st = states[items[i].channel];
                    if (items[i].isAi) {
                        const api::DetectionState ai = api::parseAiState(r.value);
                        st.person = ai.person;
                        st.vehicle = ai.vehicle;
                        st.pet = ai.pet;
                    } else {
                        st.motion = api::parseMdState(r.value);
                    }
                }
            }

            const bool transportOk = batch.transportOk;
            QMetaObject::invokeMethod(
                this,
                [this, hostId, key, hp, states, transportOk] {
                    m_pollInFlight[key] = false;
                    if (!transportOk)
                        return;
                    for (int i = 0; i < hp.channels.size(); ++i) {
                        const int channel = hp.channels[i];
                        const QString ckey = detKey(hostId, channel);
                        const api::DetectionState st = states.value(channel);
                        const api::DetectionState prev = m_lastDetection.value(ckey);
                        const QString camera = hp.names[i];
                        auto edge = [&](bool now, bool was, const char *type) {
                            if (now && !was)
                                emit detectionEvent(hostId, channel, QString::fromUtf8(type),
                                                    camera);
                        };
                        edge(st.person, prev.person, "person");
                        edge(st.vehicle, prev.vehicle, "vehicle");
                        edge(st.pet, prev.pet, "pet");
                        const bool aiActive = st.person || st.vehicle || st.pet;
                        const bool prevAi = prev.person || prev.vehicle || prev.pet;
                        if (!aiActive)
                            edge(st.motion, prev.motion || prevAi, "motion");
                        m_lastDetection[ckey] = st;
                    }
                },
                Qt::QueuedConnection);
        }));
    }
}

int DeviceManager::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant DeviceManager::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case NameRole:
        return e.chanName.isEmpty() ? e.rec.name : e.chanName;
    case AddrRole:
        return e.rec.addr;
    case KindRole:
        return e.rec.kind;
    case ModelRole:
        return e.rec.model;
    case OnlineRole:
        return e.online;
    case StatusRole:
        return e.status;
    case ProblemRole:
        switch (e.problem) {
        case Problem::Connecting: return QStringLiteral("connecting");
        case Problem::Unreachable: return QStringLiteral("unreachable");
        case Problem::Auth: return QStringLiteral("auth");
        case Problem::Locked: return QStringLiteral("locked");
        case Problem::None: break;
        }
        return QString();
    case HostIdRole:
        return e.rec.id;
    case ChannelRole:
        return e.channel;
    case RotationRole:
        return e.rotationOverride;
    case HasPtzRole:
        return e.caps.ptz;
    case HasPtzPresetRole:
        return e.caps.ptzPreset;
    case HasZoomRole:
        return e.caps.zoom;
    case HasAudioRole:
        return e.caps.audio;
    case HasSirenRole:
        return e.caps.siren;
    case HasFloodlightRole:
        return e.caps.floodlight;
    case HasBatteryRole:
        return e.caps.battery;
    case HasTalkRole:
        return e.talk;
    case IsAdminRole:
        return e.isAdmin;
    case BatteryPercentRole:
        return e.battery.present ? e.battery.percent : -1;
    case BatteryChargingRole:
        return e.battery.charging;
    }
    return {};
}

QHash<int, QByteArray> DeviceManager::roleNames() const
{
    return {
        {NameRole, "name"},
        {AddrRole, "addr"},
        {KindRole, "kind"},
        {ModelRole, "model"},
        {OnlineRole, "online"},
        {StatusRole, "status"},
        {HostIdRole, "hostId"},
        {ChannelRole, "channel"},
        {HasPtzRole, "hasPtz"},
        {HasPtzPresetRole, "hasPtzPreset"},
        {HasZoomRole, "hasZoom"},
        {HasAudioRole, "hasAudio"},
        {HasSirenRole, "hasSiren"},
        {HasFloodlightRole, "hasFloodlight"},
        {HasBatteryRole, "hasBattery"},
        {HasTalkRole, "hasTalk"},
        {IsAdminRole, "isAdmin"},
        {BatteryPercentRole, "batteryPercent"},
        {BatteryChargingRole, "batteryCharging"},
        {ProblemRole, "problem"},
        {RotationRole, "rotationOverride"},
    };
}

void DeviceManager::testDevice(const QString &addr, const QString &username,
                               const QString &password, bool https, int port)
{
    Q_UNUSED(https)
    Q_UNUSED(port)
    m_pending.addFuture(QtConcurrent::run([this, addr, username, password] {
        // Log in over Baichuan, as the device will be used, and read its name.
        BcSession::Params sp;
        sp.target = {addr, 9000, QString(), false};
        sp.username = username;
        sp.password = password;
        const std::shared_ptr<BcSession> session = BcSession::acquire(sp);
        bool ok = session->waitReady(15000);
        QString name, model, message, problem;
        if (ok) {
            bcdev::Device d;
            QByteArray xml;
            quint16 status = 0;
            ok = session->requestXml(80, 250, {}, {}, &xml, &status) && status == 200;
            if (ok) {
                bcdev::parseVersionInfo(xml, d);
                name = d.name;
                model = d.model;
            } else {
                message = tr("the device did not answer like a Reolink camera or NVR");
                problem = QStringLiteral("protocol");
            }
        } else if (session->failure() == BcSession::Failure::Rejected) {
            message = tr("login rejected — check the user name and password");
            problem = QStringLiteral("auth");
        } else {
            message = tr("no answer on port 9000 — is it a Reolink camera or NVR?");
            problem = QStringLiteral("transport");
        }
        QMetaObject::invokeMethod(
            this,
            [this, ok, message, name, model, problem] {
                emit testDeviceResult(ok, message, name, model, problem);
            },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::reconnect(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const qint64 hostId = m_entries.at(row).rec.id;
    // Reset the backoff — this is the user saying "try NOW".
    m_nextRetryAt.remove(hostId);
    m_retryDelaySecs.remove(hostId);
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].rec.id != hostId)
            continue;
        m_entries[i].status = tr("connecting…");
        m_entries[i].problem = Problem::Connecting;
        emit dataChanged(index(i), index(i));
    }
    validateAsync(hostId);
}

void DeviceManager::updateCredentials(int row, const QString &username, const QString &password)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const qint64 hostId = m_entries.at(row).rec.id;
    if (!username.trimmed().isEmpty()) {
        for (int i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].rec.id == hostId)
                m_entries[i].rec.username = username.trimmed();
        }
        const int first = rowForHostId(hostId);
        if (first >= 0)
            m_db->updateHost(m_entries.at(first).rec);
    }
    m_nextRetryAt.remove(hostId);
    m_retryDelaySecs.remove(hostId);
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].rec.id != hostId)
            continue;
        m_entries[i].status = tr("connecting…");
        m_entries[i].problem = Problem::Connecting;
        emit dataChanged(index(i), index(i));
    }
    // A non-empty password is stored to the keyring and used; empty keeps the
    // stored one (username-only fix).
    validateAsync(hostId, password, /*storeNew=*/!password.isEmpty());
}

void DeviceManager::addDevice(const QString &addr, const QString &username,
                              const QString &password, bool https, int port)
{
    HostRecord rec;
    rec.kind = QStringLiteral("camera"); // refined to "nvr" after GetDevInfo
    rec.name = addr;
    rec.addr = addr;
    rec.https = https;
    rec.port = port > 0 ? port : (https ? 443 : 80);
    rec.username = username;
    rec.id = m_db->addHost(rec);
    if (rec.id < 0) {
        emit deviceError(addr, m_db->lastError());
        return;
    }

    beginInsertRows({}, m_entries.size(), m_entries.size());
    Entry e;
    e.rec = rec;
    e.chanName = addr;
    e.status = tr("connecting…");
    e.rotationOverride = loadRotation(rec.id, 0);
    m_entries.append(e);
    endInsertRows();
    emit countChanged();

    validateAsync(rec.id, password, /*storeNew=*/true);
}

void DeviceManager::addStreamUrl(const QString &name, const QString &url)
{
    QUrl u(url);
    const QString user = u.userName();
    const QString pass = u.password();
    if (!user.isEmpty() || !pass.isEmpty()) {
        u.setUserName({});
        u.setPassword({});
    }
    const QString cleanUrl =
        u.isValid() && !u.scheme().isEmpty() ? u.toString(QUrl::FullyEncoded) : url;

    HostRecord rec;
    rec.kind = QStringLiteral("stream");
    rec.name = name.isEmpty() ? cleanUrl : name;
    rec.addr = cleanUrl;
    rec.https = false;
    rec.port = 0;
    rec.username = user;
    rec.id = m_db->addHost(rec);
    if (rec.id < 0) {
        emit deviceError(url, m_db->lastError());
        return;
    }
    if (!pass.isEmpty() && !m_credentials->store(rec.id, pass))
        qCWarning(lcCore) << "Keyring unavailable; stream credentials not saved";

    beginInsertRows({}, m_entries.size(), m_entries.size());
    Entry e;
    e.rec = rec;
    e.chanName = rec.name;
    e.online = true;
    e.status = tr("ready");
    e.password = pass;
    e.primed = true;
    m_entries.append(e);
    endInsertRows();
    emit countChanged();
}

void DeviceManager::removeDevice(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const qint64 id = m_entries.at(row).rec.id;
    // Remove every channel entry belonging to this host.
    for (int i = m_entries.size() - 1; i >= 0; --i) {
        if (m_entries.at(i).rec.id == id) {
            beginRemoveRows({}, i, i);
            m_entries.removeAt(i);
            endRemoveRows();
        }
    }
    dropSession(id);
    m_lostHosts.remove(id);
    m_db->removeHost(id);
    m_credentials->remove(id);
    emit countChanged();
}

int DeviceManager::rowForHostId(qint64 hostId) const
{
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries.at(i).rec.id == hostId)
            return i;
    return -1;
}

void DeviceManager::applyValidation(qint64 hostId, const Validation &v)
{
    // Locate the contiguous block of rows for this host.
    int first = rowForHostId(hostId);
    if (first < 0)
        return;
    int oldCount = 0;
    for (int i = first; i < m_entries.size() && m_entries.at(i).rec.id == hostId; ++i)
        ++oldCount;

    HostRecord rec = m_entries.at(first).rec;

    m_validating.remove(hostId);

    if (!v.online || v.channels.isEmpty()) {
        // Only announce a CHANGE — the retry loop re-lands the same failure
        // every backoff interval, and repeating it would spam any listener.
        const bool changed = m_entries.at(first).status != v.status
                             || m_entries.at(first).problem != v.problem;
        // Failed/offline: keep the existing rows, just mark them.
        for (int i = first; i < first + oldCount; ++i) {
            m_entries[i].online = false;
            m_entries[i].status = v.status;
            m_entries[i].problem = v.problem;
            m_entries[i].primed = true;
            m_entries[i].password = v.password;
        }
        emit dataChanged(index(first), index(first + oldCount - 1));
        if (changed)
            emit deviceError(rec.addr, v.status);
        // Schedule the recovery probe. Unreachable retries with doubling
        // backoff (15s..300s); Auth/Locked never auto-retry — every rejected
        // login burns the firmware's 10-attempt lockout counter, so recovery
        // there is Reconnect / Update credentials in the sidebar.
        if (v.problem == Problem::Unreachable) {
            const int delay = m_retryDelaySecs.value(hostId, 15);
            m_nextRetryAt[hostId] = QDateTime::currentDateTimeUtc().addSecs(delay);
            m_retryDelaySecs[hostId] = qMin(delay * 2, 300);
        } else {
            m_nextRetryAt.remove(hostId);
            m_retryDelaySecs.remove(hostId);
        }
        return;
    }
    m_nextRetryAt.remove(hostId);
    m_retryDelaySecs.remove(hostId);

    // The host's session from now on; an NVR pushes its channels' states on it.
    dropSession(hostId);
    HostSession hs;
    hs.session = v.session;
    if (v.session && v.channelNum > 1) {
        const std::weak_ptr<BcSession> weak = v.session;
        hs.pushToken = v.session->addPushHandler([this, hostId, weak](const BcMessage &m) {
            if (m.cmd != 145)
                return;
            const auto s = weak.lock();
            if (!s)
                return;
            QHash<int, bool> states;
            for (const auto &st : bcdev::parseChannelStates(s->bodyXml(m)))
                states.insert(st.first, st.second);
            QMetaObject::invokeMethod(
                this, [this, hostId, states] { applyChannelStates(hostId, states); },
                Qt::QueuedConnection);
        });
    }
    if (hs.session)
        m_sessions.insert(hostId, hs);
    const bool recovered = m_lostHosts.remove(hostId);

    // Update the host record (name/model/kind), persist once.
    if (!v.hostName.isEmpty())
        rec.name = v.hostName;
    rec.model = v.model;
    if (v.channelNum > 1)
        rec.kind = QStringLiteral("nvr");
    m_db->updateHost(rec);

    // Build the new per-channel entries.
    QVector<Entry> fresh;
    fresh.reserve(v.channels.size());
    for (const ChannelResult &ch : v.channels) {
        Entry e;
        e.rec = rec;
        e.channel = ch.channel;
        e.chanName = ch.name.isEmpty() ? rec.name : ch.name;
        e.online = ch.online;
        e.status = ch.online ? tr("online") : tr("offline");
        e.mainCodec = ch.codec; // may be empty = unknown -> probe
        e.mainSize = ch.mainSize;
        e.subSize = ch.subSize;
        e.uid = ch.uid;
        e.hostUid = v.hostUid;
        e.caps = ch.caps;
        e.talk = ch.caps.talk;
        e.isAdmin = v.isAdmin;
        e.password = v.password;
        e.primed = true;
        e.problem = Problem::None;
        e.rotationOverride = loadRotation(rec.id, ch.channel);
        e.client = v.client;
        if (ch.channel == 0)
            e.battery = v.battery;
        fresh.append(e);
    }

    // Same channels as before (a camera revalidated, an NVR with the same
    // cameras): update the rows in place, so the panes showing them keep their
    // camera and stream instead of being rebuilt.
    bool sameChannels = fresh.size() == oldCount;
    for (int j = 0; sameChannels && j < oldCount; ++j)
        sameChannels = m_entries.at(first + j).channel == fresh.at(j).channel;
    if (sameChannels) {
        for (int j = 0; j < oldCount; ++j) {
            Entry e = fresh.at(j);
            const Entry &old = m_entries.at(first + j);
            e.filesSub = old.filesSub; // learned by the last search
            e.filesMain = old.filesMain;
            m_entries[first + j] = e;
        }
        emit dataChanged(index(first), index(first + oldCount - 1));
        if (recovered)
            emit connectivityChanged(hostId, -1, rec.name, true);
        return;
    }

    // Replace the old block with the new channel entries.
    beginRemoveRows({}, first, first + oldCount - 1);
    m_entries.remove(first, oldCount);
    endRemoveRows();
    beginInsertRows({}, first, first + fresh.size() - 1);
    for (int j = 0; j < fresh.size(); ++j)
        m_entries.insert(first + j, fresh.at(j));
    endInsertRows();
    emit countChanged();
    if (recovered)
        emit connectivityChanged(hostId, -1, rec.name, true);
}

void DeviceManager::postValidation(qint64 hostId, const Validation &v)
{
    QMetaObject::invokeMethod(
        this, [this, hostId, v] { applyValidation(hostId, v); }, Qt::QueuedConnection);
}

void DeviceManager::validateAsync(qint64 hostId, const QString &newPassword, bool storeNew)
{
    const int row = rowForHostId(hostId);
    if (row < 0)
        return;
    // One validation per host at a time — the retry tick and a user-driven
    // Reconnect must not stack logins on a struggling device.
    if (m_validating.contains(hostId))
        return;
    m_validating.insert(hostId);
    const HostRecord rec = m_entries.at(row).rec;
    const bool isStream = rec.kind == QLatin1String("stream");

    m_pending.addFuture(QtConcurrent::run([this, rec, newPassword, storeNew, isStream, hostId] {
        if (storeNew && !newPassword.isEmpty())
            m_credentials->store(hostId, newPassword);

        bool havePassword = false;
        QString password = newPassword;
        if (password.isEmpty())
            password = m_credentials->lookup(hostId, &havePassword);
        else
            havePassword = true;

        if (isStream) {
            const QString pw = password;
            QMetaObject::invokeMethod(
                this,
                [this, hostId, pw] {
                    const int r = rowForHostId(hostId);
                    if (r >= 0) {
                        m_entries[r].password = pw;
                        m_entries[r].primed = true;
                    }
                    m_validating.remove(hostId);
                },
                Qt::QueuedConnection);
            return;
        }

        if (!havePassword && storeNew) {
            Validation v;
            v.status = tr("keyring unavailable — password not saved");
            v.problem = Problem::Auth; // needs the user, not a retry loop
            postValidation(hostId, v);
            return;
        }

        // The device's session, as the official client opens it (BCSDK_Open):
        // log in over Baichuan, then read what it is and can do.
        BcSession::Params sp;
        sp.target = {rec.addr, 9000, QString(), false};
        sp.username = rec.username;
        sp.password = password;
        const std::shared_ptr<BcSession> session = BcSession::acquire(sp);
        bcdev::Device d;
        const bool ok = session->waitReady(15000) && bcdev::readDevice(*session, rec.username, &d);

        Validation v;
        v.password = password;
        if (!ok) {
            // Unreachable is retried with backoff; a rejected login waits for
            // the user (each try burns the firmware's lockout counter).
            if (session->failure() == BcSession::Failure::Rejected) {
                v.status = tr("login rejected — check the user name and password");
                v.problem = Problem::Auth;
            } else {
                v.status = tr("unreachable");
                v.problem = Problem::Unreachable;
            }
            qCInfo(lcCore) << "host" << hostId << "not validated:" << session->error();
            postValidation(hostId, v);
            return;
        }
        v.online = true;
        v.status = tr("online");
        v.problem = Problem::None;
        v.session = session;
        // Features not moved to Baichuan yet still use the HTTP API.
        v.client = std::make_shared<ReolinkHttpClient>(rec.addr, rec.port, rec.https,
                                                       rec.username, password);
        v.hostName = d.name;
        v.model = d.model;
        v.channelNum = d.channelNum;
        v.isAdmin = d.isAdmin;
        v.hostUid = d.uid;
        v.battery = d.battery;
        // A camera is its channel 0; an NVR shows its online channels.
        for (const bcdev::Channel &c : d.channels) {
            if (d.isNvr && !c.online)
                continue;
            ChannelResult cr;
            cr.channel = c.channel;
            cr.name = c.name.isEmpty() ? d.name : c.name;
            cr.online = true;
            cr.mainSize = c.mainSize;
            cr.subSize = c.subSize;
            cr.caps = c.caps;
            cr.uid = d.isNvr ? QString() : d.uid; // an NVR camera's own: not known yet
            // The codec is in every frame's header; the stream is probed.
            cr.codec.clear();
            v.talk = v.talk || cr.caps.talk;
            v.channels.append(cr);
        }
        if (v.channels.isEmpty()) {
            // NVR reachable but no online cameras yet — keep a placeholder.
            ChannelResult cr;
            cr.channel = 0;
            cr.name = d.name;
            cr.online = false;
            v.channels.append(cr);
        }
        postValidation(hostId, v);
    }));
}

std::shared_ptr<ReolinkHttpClient> DeviceManager::clientFor(int row)
{
    if (row < 0 || row >= m_entries.size())
        return {};
    Entry &e = m_entries[row];
    if (e.rec.kind == QLatin1String("stream") || !e.primed)
        return {};
    if (!e.client)
        e.client = std::make_shared<ReolinkHttpClient>(e.rec.addr, e.rec.port, e.rec.https,
                                                       e.rec.username, e.password);
    return e.client;
}

void DeviceManager::ptzMove(int row, const QString &op, int speed)
{
    auto client = clientFor(row);
    if (!client)
        return;
    const int ch = m_entries.at(row).channel;
    m_pending.addFuture(QtConcurrent::run(
        [client, ch, op, speed] { client->call(Json::array({api::ptzCtrl(ch, op, speed)})); }));
}

void DeviceManager::ptzStop(int row)
{
    auto client = clientFor(row);
    if (!client)
        return;
    const int ch = m_entries.at(row).channel;
    m_pending.addFuture(QtConcurrent::run(
        [client, ch] { client->call(Json::array({api::ptzCtrl(ch, QStringLiteral("Stop"))})); }));
}

void DeviceManager::ptzPreset(int row, int presetId)
{
    auto client = clientFor(row);
    if (!client)
        return;
    const int ch = m_entries.at(row).channel;
    m_pending.addFuture(QtConcurrent::run([client, ch, presetId] {
        client->call(Json::array({api::ptzCtrl(ch, QStringLiteral("ToPos"), 32, presetId)}));
    }));
}

QString DeviceManager::talkKey(int row) const
{
    const Entry &e = m_entries.at(row);
    return QString::number(e.rec.id) + u':' + QString::number(e.channel);
}

bool DeviceManager::isTalking(int row) const
{
    return row >= 0 && row < m_entries.size() && !m_talks.value(talkKey(row)).isNull();
}

void DeviceManager::startTalk(int row)
{
    if (row < 0 || row >= m_entries.size() || isTalking(row))
        return;
    const Entry &e = m_entries.at(row);
    if (!e.primed) {
        emit talkStateChanged(row, false, tr("device not ready"));
        return;
    }
    const QString key = talkKey(row);
    auto *talk = new TalkSession({e.rec.addr, 9000, e.rec.username, e.password}, e.channel, this);
    m_talks.insert(key, talk);
    // Rows can move while talking: resolve the current row when reporting.
    auto rowOf = [this, key] {
        for (int i = 0; i < m_entries.size(); ++i)
            if (talkKey(i) == key)
                return i;
        return -1;
    };
    connect(talk, &TalkSession::started, this, [this, rowOf] { emit talkStateChanged(rowOf(), true, {}); });
    connect(talk, &TalkSession::failed, this, [this, rowOf, talk](const QString &err) {
        talk->stop();
        emit talkStateChanged(rowOf(), false, err);
    });
    connect(talk, &TalkSession::finished, this, [this, rowOf, key, talk] {
        if (m_talks.value(key) == talk)
            m_talks.remove(key);
        talk->deleteLater();
        emit talkStateChanged(rowOf(), false, {});
    });
    talk->start();
}

void DeviceManager::stopTalk(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    if (TalkSession *talk = m_talks.value(talkKey(row)))
        talk->stop(); // finished() reports the stop and frees it
}

void DeviceManager::retranslateStatuses()
{
    for (int i = 0; i < m_entries.size(); ++i) {
        Entry &e = m_entries[i];
        if (e.problem != Problem::None)
            continue;
        if (e.primed)
            e.status = e.online ? tr("online") : tr("offline");
        else
            e.status = e.online ? tr("ready") : tr("connecting…");
        emit dataChanged(index(i), index(i));
    }
}

void DeviceManager::ptzPatrol(int row, int patrolId, bool start)
{
    auto client = clientFor(row);
    if (!client) {
        emit ptzPatrolDone(row, false, tr("device not ready"));
        return;
    }
    const int ch = m_entries.at(row).channel;
    const QString op = start ? QStringLiteral("StartPatrol") : QStringLiteral("StopPatrol");
    m_pending.addFuture(QtConcurrent::run([this, client, row, ch, op, patrolId] {
        const api::BatchResult b = client->call(Json::array({api::ptzCtrl(ch, op, 32, patrolId)}));
        const bool ok = b.transportOk && !b.results.isEmpty() && b.results.first().ok;
        const QString error = ok ? QString()
                            : (!b.results.isEmpty() && !b.results.first().detail.isEmpty()
                                   ? b.results.first().detail : tr("failed"));
        QMetaObject::invokeMethod(
            this, [this, row, ok, error] { emit ptzPatrolDone(row, ok, error); },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::snapshot(int row)
{
    auto client = clientFor(row);
    if (!client) {
        emit snapshotFailed(row, tr("device not ready"));
        return;
    }
    const int ch = m_entries.at(row).channel;
    const QString name = m_entries.at(row).chanName;
    m_pending.addFuture(QtConcurrent::run([this, client, ch, row, name] {
        QString error;
        const QByteArray jpeg = client->fetchSnapshot(ch, &error);
        QString path;
        if (!jpeg.isEmpty()) {
            const QString safe =
                QString(name).replace(QRegularExpression(QStringLiteral("[^\\w-]")),
                                      QStringLiteral("_"));
            const QString stamp =
                QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss"));
            path = Paths::recordingsDir() + u'/' + safe + u'_' + stamp + QStringLiteral(".jpg");
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly) || f.write(jpeg) != jpeg.size()) {
                error = f.errorString();
                path.clear();
            }
        }
        QMetaObject::invokeMethod(
            this,
            [this, row, path, error] {
                if (!path.isEmpty())
                    emit snapshotSaved(row, path);
                else
                    emit snapshotFailed(row, error.isEmpty() ? tr("snapshot failed") : error);
            },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::captureEventThumbnail(qint64 hostId, int channel, qint64 eventId)
{
    const int row = rowOfHostChannel(hostId, channel);
    auto client = clientFor(row);
    if (!client)
        return; // silent: an event without a thumbnail still shows its placeholder
    const int ch = channel;
    m_pending.addFuture(QtConcurrent::run([this, client, ch, eventId] {
        QString error;
        const QByteArray jpeg = client->fetchSnapshot(ch, &error);
        if (jpeg.isEmpty())
            return;
        // Downscale so 100 cached thumbnails stay small (a Snap off a 4K/8K
        // camera can be several MB). Fall back to the raw bytes if decoding
        // isn't possible.
        QString path = Paths::thumbnailsDir() + QStringLiteral("/event_%1.jpg").arg(eventId);
        const QImage img = QImage::fromData(jpeg);
        bool ok = false;
        if (!img.isNull())
            ok = img.scaledToWidth(qMin(640, img.width()), Qt::SmoothTransformation)
                     .save(path, "JPG", 85);
        if (!ok) {
            QFile f(path);
            ok = f.open(QIODevice::WriteOnly) && f.write(jpeg) == jpeg.size();
        }
        if (!ok)
            return;
        QMetaObject::invokeMethod(
            this, [this, eventId, path] { emit eventThumbnailReady(eventId, path); },
            Qt::QueuedConnection);
    }));
}

std::shared_ptr<BcSession> DeviceManager::sessionFor(int row) const
{
    if (row < 0 || row >= m_entries.size())
        return {};
    const auto s = m_sessions.value(m_entries.at(row).rec.id).session;
    return s && s->isAlive() ? s : nullptr;
}

void DeviceManager::searchRecordings(int row, int year, int month, int day)
{
    const auto session = sessionFor(row);
    const QDate date(year, month, day);
    if (!session || !date.isValid()) {
        emit recordingsFailed(row, tr("device not ready"));
        return;
    }
    const Entry &e = m_entries.at(row);
    const int ch = e.channel;
    const QString uid = e.uid.isEmpty() ? e.hostUid : e.uid;
    m_pending.addFuture(QtConcurrent::run([this, session, ch, uid, row, date, year, month] {
        auto toFiles = [](const QVector<bcrec::Recording> &recs) {
            QVector<BcReplayFile> out;
            for (const bcrec::Recording &r : recs)
                out.append({r.start.toSecsSinceEpoch(),
                            r.end.isValid() ? r.end.toSecsSinceEpoch() : r.start.toSecsSinceEpoch() + 60,
                            r.name, r.id});
            return out;
        };
        // Sub files first: they draw the timeline. Then the month's days, then
        // the main files for HD.
        QVector<bcrec::Recording> sub;
        if (!bcrec::searchDay(*session, ch, uid, date, false, &sub)) {
            QMetaObject::invokeMethod(
                this, [this, row] { emit recordingsFailed(row, tr("search failed")); },
                Qt::QueuedConnection);
            return;
        }
        QVariantList segments;
        for (const bcrec::Recording &f : sub) {
            const qint64 s = qBound(qint64(0), date.startOfDay().secsTo(f.start), qint64(86400));
            const qint64 e = qBound(s, date.startOfDay().secsTo(f.end.isValid() ? f.end : f.start),
                                    qint64(86400));
            QVariantMap seg;
            seg["start"] = static_cast<double>(s);
            seg["end"] = static_cast<double>(e);
            // Anything beyond the schedule is an alarm (blue on the timeline).
            seg["type"] = (f.triggers & ~api::trigger::Timer) ? QStringLiteral("alarm")
                                                              : QStringLiteral("timer");
            seg["triggers"] = f.triggers;
            seg["startEpoch"] = static_cast<double>(f.start.toSecsSinceEpoch());
            segments.append(seg);
        }
        QVariantList days;
        for (int d : bcrec::monthDays(*session, ch, uid, year, month))
            days.append(d);
        const QVector<BcReplayFile> subFiles = toFiles(sub);
        QMetaObject::invokeMethod(
            this,
            [this, row, segments, days, year, month, subFiles] {
                // Store the files BEFORE emitting, so a handler that plays at
                // once finds them.
                if (row >= 0 && row < m_entries.size())
                    m_entries[row].filesSub = subFiles;
                emit recordingsFound(row, segments);
                emit recordingDaysFound(row, year, month, days);
            },
            Qt::QueuedConnection);
        QVector<bcrec::Recording> main;
        if (bcrec::searchDay(*session, ch, uid, date, true, &main)) {
            const QVector<BcReplayFile> mainFiles = toFiles(main);
            QMetaObject::invokeMethod(
                this,
                [this, row, mainFiles] {
                    if (row >= 0 && row < m_entries.size())
                        m_entries[row].filesMain = mainFiles;
                },
                Qt::QueuedConnection);
        }
    }));
}

void DeviceManager::searchRecordingDays(int row, int year, int month)
{
    const auto session = sessionFor(row);
    if (!session || !QDate(year, month, 1).isValid())
        return;
    const Entry &e = m_entries.at(row);
    const int ch = e.channel;
    const QString uid = e.uid.isEmpty() ? e.hostUid : e.uid;
    m_pending.addFuture(QtConcurrent::run([this, session, ch, uid, row, year, month] {
        QVariantList days;
        for (int d : bcrec::monthDays(*session, ch, uid, year, month))
            days.append(d);
        QMetaObject::invokeMethod(
            this, [this, row, year, month, days] { emit recordingDaysFound(row, year, month, days); },
            Qt::QueuedConnection);
    }));
}

QSize DeviceManager::declaredSize(int row, bool mainStream) const
{
    if (row < 0 || row >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(row);
    return mainStream ? e.mainSize : e.subSize;
}


void DeviceManager::requestHdClip(int row, qint64 startEpoch, int durationSecs)
{
    auto client = clientFor(row);
    if (!client || row < 0 || row >= m_entries.size() || startEpoch <= 0) {
        emit hdClipFailed(row, tr("device not ready"));
        return;
    }
    const Entry &e = m_entries.at(row);
    if (e.rec.kind == QLatin1String("stream") || !e.primed) {
        emit hdClipFailed(row, tr("no recordings on this device"));
        return;
    }
    const int ch = e.channel;
    const QString host = e.rec.addr;
    const int port = e.rec.port;
    const bool https = e.rec.https;
    // NvrDownload takes wall-clock local times (like Search), not the FLV
    // playback clock — no offset applied here.
    const QDateTime start = QDateTime::fromSecsSinceEpoch(startEpoch);
    const QDateTime end = start.addSecs(qBound(2, durationSecs, 120));
    const QString startStamp = start.toString(QStringLiteral("yyyyMMddHHmmss"));

    m_pending.addFuture(QtConcurrent::run([this, client, ch, row, host, port, https, start, end,
                                           startStamp, startEpoch] {
        QString error;
        QString localPath;
        const api::BatchResult b =
            client->call(Json::array({api::nvrDownloadBody(ch, start, end, QStringLiteral("main"))}));
        if (b.transportOk && !b.results.isEmpty() && b.results.first().ok) {
            const QVector<api::DownloadFile> files = api::parseNvrDownload(b.results.first().value);
            // Prefer the clip named for the requested start; otherwise the largest.
            api::DownloadFile pick;
            for (const api::DownloadFile &f : files)
                if (f.fileName.contains(startStamp)) {
                    pick = f;
                    break;
                }
            if (pick.fileName.isEmpty())
                for (const api::DownloadFile &f : files)
                    if (f.size > pick.size)
                        pick = f;

            if (pick.fileName.isEmpty()) {
                error = tr("no clip available for this moment");
            } else {
                const QString url =
                    api::downloadUrl(host, port, https, pick.fileName, client->token());
                const QString dir = Paths::dataDir() + QStringLiteral("/hdcache");
                QDir().mkpath(dir);
                // Keep only the current clip — these files are large.
                for (const QFileInfo &fi : QDir(dir).entryInfoList(QDir::Files))
                    QFile::remove(fi.absoluteFilePath());
                const QString path = dir + QStringLiteral("/clip_") + startStamp + QStringLiteral(".mp4");
                QString dlErr;
                if (client->downloadToFile(url, path, 120, &dlErr)) // slow NVR download
                    localPath = path;
                else
                    error = dlErr.isEmpty() ? tr("clip download failed") : dlErr;
            }
        } else {
            error = b.error.isEmpty() ? tr("clip request failed") : b.error;
        }
        QMetaObject::invokeMethod(
            this,
            [this, row, localPath, startEpoch, error] {
                if (!localPath.isEmpty())
                    emit hdClipReady(row, localPath, startEpoch);
                else
                    emit hdClipFailed(row, error);
            },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::exportClip(int row, qint64 startEpoch, int durationSecs)
{
    auto client = clientFor(row);
    if (!client || row < 0 || row >= m_entries.size() || startEpoch <= 0) {
        emit clipExportFailed(row, tr("device not ready"));
        return;
    }
    const Entry &e = m_entries.at(row);
    const int ch = e.channel;
    const QString host = e.rec.addr;
    const int port = e.rec.port;
    const bool https = e.rec.https;
    const QString safe = QString(e.chanName.isEmpty() ? e.rec.name : e.chanName)
                             .replace(QRegularExpression(QStringLiteral("[^\\w-]")),
                                      QStringLiteral("_"));
    const QDateTime start = QDateTime::fromSecsSinceEpoch(startEpoch);
    const QDateTime end = start.addSecs(qBound(2, durationSecs, 300));
    const QString startStamp = start.toString(QStringLiteral("yyyyMMddHHmmss"));

    // Standalone cameras have no NvrDownload ("not support", rspCode -9 on a
    // Duo 2 PoE). They download recordings by name: find the main-stream files
    // overlapping [start,end] and fetch each whole (they are short event clips).
    if (e.rec.kind != QLatin1String("nvr")) {
        m_pending.addFuture(QtConcurrent::run([this, client, ch, row, host, port, https, start,
                                               end, safe] {
            QStringList saved;
            QString error;
            const api::BatchResult b = client->call(Json::array(
                {api::searchBody(ch, start.addSecs(-3600), end, QStringLiteral("main"))}));
            if (!b.transportOk || b.results.isEmpty() || !b.results.first().ok) {
                error = b.error.isEmpty() ? tr("clip request failed") : b.error;
            } else {
                for (const api::RecordingFile &f : api::parseSearch(b.results.first().value).files) {
                    if (f.name.isEmpty() || !f.start.isValid() || f.start > end
                        || (f.end.isValid() && f.end < start))
                        continue;
                    const QString url = api::downloadUrl(host, port, https, f.name,
                                                         client->token(), f.name.section(u'/', -1));
                    const QString path = Paths::recordingsDir() + u'/' + safe + u'_'
                                         + f.start.toString(QStringLiteral("yyyyMMdd_hhmmss"))
                                         + QStringLiteral(".mp4");
                    QString dlErr;
                    if (client->downloadToFile(url, path, 300, &dlErr))
                        saved.append(path);
                    else
                        error = dlErr.isEmpty() ? tr("download failed") : dlErr;
                }
                if (saved.isEmpty() && error.isEmpty())
                    error = tr("no recording covers this moment");
            }
            QMetaObject::invokeMethod(
                this,
                [this, row, saved, error] {
                    if (!saved.isEmpty())
                        emit clipExported(row, saved.join(QStringLiteral(", ")));
                    else
                        emit clipExportFailed(row, error);
                },
                Qt::QueuedConnection);
        }));
        return;
    }

    m_pending.addFuture(QtConcurrent::run([this, client, ch, row, host, port, https, start, end,
                                           startStamp, safe] {
        QString error;
        QString saved;
        const api::BatchResult b =
            client->call(Json::array({api::nvrDownloadBody(ch, start, end, QStringLiteral("main"))}));
        if (b.transportOk && !b.results.isEmpty() && b.results.first().ok) {
            const QVector<api::DownloadFile> files = api::parseNvrDownload(b.results.first().value);
            api::DownloadFile pick;
            for (const api::DownloadFile &f : files)
                if (f.fileName.contains(startStamp)) { pick = f; break; }
            if (pick.fileName.isEmpty())
                for (const api::DownloadFile &f : files)
                    if (f.size > pick.size)
                        pick = f;
            if (pick.fileName.isEmpty()) {
                error = tr("no recording covers this moment");
            } else {
                const QString url =
                    api::downloadUrl(host, port, https, pick.fileName, client->token());
                const QString path = Paths::recordingsDir() + u'/' + safe + u'_'
                                     + start.toString(QStringLiteral("yyyyMMdd_hhmmss"))
                                     + QStringLiteral(".mp4");
                QString dlErr;
                if (client->downloadToFile(url, path, 300, &dlErr)) // NVR downloads are slow
                    saved = path;
                else
                    error = dlErr.isEmpty() ? tr("download failed") : dlErr;
            }
        } else {
            error = b.error.isEmpty() ? tr("clip request failed") : b.error;
        }
        QMetaObject::invokeMethod(
            this,
            [this, row, saved, error] {
                if (!saved.isEmpty())
                    emit clipExported(row, saved);
                else
                    emit clipExportFailed(row, error);
            },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::startBaichuan(int row, qint64 startEpoch, StreamPlayer *player,
                                  bool mainStream)
{
    if (row < 0 || row >= m_entries.size() || !player)
        return;
    const Entry &e = m_entries.at(row);
    if (e.rec.kind == QLatin1String("stream") || !e.primed)
        return;

    BaichuanClient::Params p;
    p.host = e.rec.addr;
    p.port = 9000; // Baichuan is a separate port from the HTTP API
    p.username = e.rec.username;
    p.password = e.password;
    p.channel = e.channel;
    p.uid = e.uid.isEmpty() ? e.hostUid : e.uid;
    p.mainStream = mainStream;
    p.startEpoch = startEpoch; // <= 0 = live (cmd 3 Preview), else recorded playback
    if (startEpoch > 0) {
        // The searched day's files of this stream (the other one if missing).
        const bool useMain = mainStream ? !e.filesMain.isEmpty() : e.filesSub.isEmpty();
        p.replayFiles = useMain ? e.filesMain : e.filesSub;
        p.mainStream = mainStream = useMain;
    }
    // The stream's sound plays through the player.
    auto audio = player->packetAudioInput();
    p.onAudio = [audio](const BcMediaParser::AudioFrame &a) { audio(a.data, a.aac); };

    auto client = std::make_shared<BaichuanClient>(p);
    if (startEpoch > 0) {
        m_playbackClients.insert(row, client); // weak — for in-place seek
        // The client paces recordings, at the player's speed.
        client->setRate(player->playbackRate());
        delete player->findChild<QObject *>(QStringLiteral("rl_bcRate"));
        auto *ctx = new QObject(player);
        ctx->setObjectName(QStringLiteral("rl_bcRate"));
        const std::weak_ptr<BaichuanClient> weak = client;
        connect(player, &StreamPlayer::playbackRateChanged, ctx, [player, weak] {
            if (const auto c = weak.lock())
                c->setRate(player->playbackRate());
        });
    }
    client->start();
    // The sub stream is always H.264; the main stream's codec is per-channel.
    // An UNKNOWN main codec (firmware omitted/mangled GetEnc vType — issue #4)
    // passes an empty format so avformat probes the stream instead: forcing
    // h264 onto an HEVC bitstream yields "Could not find codec parameters".
    QString format = QStringLiteral("h264");
    if (mainStream)
        format = e.mainCodec.isEmpty()
                     ? QString()
                     : (e.mainCodec == QLatin1String("h265") ? QStringLiteral("hevc")
                                                             : QStringLiteral("h264"));
    player->setExpectedSize(mainStream ? e.mainSize : e.subSize);
    player->setPacketSource(
        [client](unsigned char *buf, int size) { return client->read(buf, size); },
        format,
        [client] { client->stop(); },
        /*pausable=*/startEpoch > 0);
    player->start();
}

void DeviceManager::startBaichuanPlayback(int row, qint64 startEpoch, StreamPlayer *player,
                                          bool mainStream)
{
    if (startEpoch <= 0)
        return;
    startBaichuan(row, startEpoch, player, mainStream);
}

void DeviceManager::startBaichuanLive(int row, StreamPlayer *player, bool mainStream)
{
    startBaichuan(row, /*startEpoch=*/0, player, mainStream);
}

bool DeviceManager::seekBaichuanPlayback(int row, qint64 startEpoch)
{
    const auto client = m_playbackClients.value(row).lock();
    return client && client->seek(startEpoch);
}

void DeviceManager::fetchSettings(int row, const QStringList &getCommands, const QString &tag)
{
    auto client = clientFor(row);
    if (!client || getCommands.isEmpty()) {
        if (tag.isEmpty())
            emit settingsFailed(row, tr("device not ready"));
        else
            emit settingsLoaded(row, {{QStringLiteral("_tag"), tag},
                                      {QStringLiteral("_error"), tr("device not ready")}});
        return;
    }
    const int ch = m_entries.at(row).channel;
    m_pending.addFuture(QtConcurrent::run([this, client, row, ch, getCommands, tag] {
        QVariantMap values;
        QStringList failed;
        QString lastError;
        // Fetch one command, retrying only transient transport failures: NVRs (e.g.
        // RLN8-410) intermittently return HTTP 502 when proxying a camera command
        // under load. Retry a few times with a short backoff (the lighter action=0
        // on the 2nd try); a DEFINITIVE device error (rspCode set — e.g. -9 "not
        // supported") stops immediately so we don't hammer the NVR for nothing.
        auto fetchOne = [&](const QString &cmd) {
            api::CommandResult r;
            for (int attempt = 0; attempt < 3; ++attempt) {
                r = client->callOne(cmd, Json{{"channel", ch}}, attempt == 1 ? 0 : 1);
                if (r.ok || r.rspCode != 0)
                    break;
                QThread::msleep(400);
            }
            return r;
        };
        // Newer firmware renamed several commands with a "V20" suffix (a different
        // schedule shape). If the device says the legacy name isn't supported, try
        // the V20 variant and store it under the legacy key so panels bind uniformly.
        static const QHash<QString, QString> v20Alt = {
            {QStringLiteral("GetPush"), QStringLiteral("GetPushV20")},
            {QStringLiteral("GetEmail"), QStringLiteral("GetEmailV20")},
            {QStringLiteral("GetFtp"), QStringLiteral("GetFtpV20")},
            {QStringLiteral("GetRec"), QStringLiteral("GetRecV20")}};
        for (const QString &c : getCommands) {
            api::CommandResult r = fetchOne(c);
            if (!r.ok && r.rspCode != 0 && v20Alt.contains(c))
                r = fetchOne(v20Alt.value(c));
            if (r.ok) {
                values.insert(c, api::toVariant(r.value));
                // Bounds differ per model (zoom 0..34 vs 0..41, focus 0..230 vs
                // 0..1104 on two RLC-823A): expose them as "<Cmd>Range".
                if (!r.range.empty())
                    values.insert(c + QStringLiteral("Range"), api::toVariant(r.range));
            } else {
                failed.append(c);
                if (!r.detail.isEmpty())
                    lastError = r.detail;
            }
        }
        QMetaObject::invokeMethod(
            this,
            [this, row, values, failed, lastError, tag] {
                const QString error = lastError.isEmpty() ? tr("settings fetch failed") : lastError;
                if (values.isEmpty() && tag.isEmpty()) {
                    emit settingsFailed(row, error);
                    return;
                }
                // Report which commands the device refused so panels can show an
                // honest "unavailable" state instead of misleading defaults.
                QVariantMap v = values;
                if (!failed.isEmpty()) {
                    QVariantList f;
                    for (const QString &c : failed)
                        f.append(c);
                    v.insert(QStringLiteral("_failed"), f);
                }
                if (!tag.isEmpty()) {
                    v.insert(QStringLiteral("_tag"), tag);
                    if (values.isEmpty())
                        v.insert(QStringLiteral("_error"), error);
                }
                emit settingsLoaded(row, v);
            },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::applySetting(int row, const QString &setCommand, const QVariantMap &param)
{
    auto client = clientFor(row);
    if (!client) {
        emit settingApplied(row, setCommand, false, tr("device not ready"));
        return;
    }
    if (row < m_entries.size() && !m_entries.at(row).isAdmin) {
        emit settingApplied(row, setCommand, false, tr("requires an administrator account"));
        return;
    }
    const Json p = api::toJson(param);
    m_pending.addFuture(QtConcurrent::run([this, client, row, setCommand, p] {
        const api::CommandResult r = client->callOne(setCommand, p, /*action=*/0);
        const bool ok = r.ok;
        const QString error = ok ? QString() : (r.detail.isEmpty() ? tr("failed") : r.detail);
        QMetaObject::invokeMethod(
            this,
            [this, row, setCommand, ok, error] {
                emit settingApplied(row, setCommand, ok, error);
            },
            Qt::QueuedConnection);
    }));
}

QVariantMap DeviceManager::hostInfo(qint64 hostId) const
{
    QVariantMap m;
    int channelCount = 0, onlineCount = 0, firstRow = -1;
    const Entry *host = nullptr;
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &e = m_entries.at(i);
        if (e.rec.id != hostId)
            continue;
        if (!host) { host = &e; firstRow = i; }
        ++channelCount;
        if (e.online)
            ++onlineCount;
    }
    if (!host)
        return m;
    m["name"] = host->rec.name;
    m["kind"] = host->rec.kind;
    m["model"] = host->rec.model;
    m["addr"] = host->rec.addr;
    m["port"] = host->rec.port;
    m["https"] = host->rec.https;
    m["username"] = host->rec.username;
    m["online"] = onlineCount > 0;
    m["channelCount"] = channelCount;
    m["onlineCount"] = onlineCount;
    m["isAdmin"] = host->isAdmin;
    m["firstRow"] = firstRow;
    m["status"] = host->status;
    switch (host->problem) {
    case Problem::Connecting: m["problem"] = QStringLiteral("connecting"); break;
    case Problem::Unreachable: m["problem"] = QStringLiteral("unreachable"); break;
    case Problem::Auth: m["problem"] = QStringLiteral("auth"); break;
    case Problem::Locked: m["problem"] = QStringLiteral("locked"); break;
    case Problem::None: m["problem"] = QString(); break;
    }
    return m;
}

QVariantMap DeviceManager::cameraInfo(int row) const
{
    QVariantMap m;
    if (row < 0 || row >= m_entries.size())
        return m;
    const Entry &e = m_entries.at(row);
    const auto dim = [](const QSize &s) {
        return s.isValid() ? QStringLiteral("%1×%2").arg(s.width()).arg(s.height()) : QString();
    };
    m["name"] = e.chanName.isEmpty() ? e.rec.name : e.chanName;
    m["hostName"] = e.rec.name;
    m["hostId"] = e.rec.id;
    m["username"] = e.rec.username;
    m["addr"] = e.rec.addr;
    m["channel"] = e.channel;
    m["kind"] = e.rec.kind;
    m["model"] = e.rec.model;
    m["codec"] = e.mainCodec;
    m["mainSize"] = dim(e.mainSize);
    m["subSize"] = dim(e.subSize);
    m["uid"] = e.uid;
    m["online"] = e.online;
    m["isAdmin"] = e.isAdmin;
    m["capPtz"] = e.caps.ptz;
    m["capZoom"] = e.caps.zoom;
    m["capFocus"] = e.caps.focus;
    m["capPanTilt"] = e.caps.panTilt;
    m["capPtzPreset"] = e.caps.ptzPreset;
    m["capPtzGuard"] = e.caps.ptzGuard;
    m["capPtzPatrol"] = e.caps.ptzPatrol;
    m["capPtzSpeed"] = e.caps.ptzSpeed;
    m["capPtzCheck"] = e.caps.ptzCheck;
    m["capAutoTrack"] = e.caps.autoTrack;
    m["capAutoFocus"] = e.caps.autoFocus;
    m["capZoomFocusPos"] = e.caps.zoomFocusPos;
    m["capAudio"] = e.caps.audio;
    m["capSiren"] = e.caps.siren;
    m["capFloodlight"] = e.caps.floodlight;
    m["capBattery"] = e.caps.battery;
    m["capTalk"] = e.talk;
    m["rotationOverride"] = e.rotationOverride;
    return m;
}

void DeviceManager::setRotationOverride(int row, int degrees)
{
    if (row < 0 || row >= m_entries.size())
        return;
    Entry &e = m_entries[row];
    const int deg = ((degrees % 360) + 360) % 360;
    if (e.rotationOverride == deg)
        return;
    e.rotationOverride = deg;
    QSettings().setValue(rotationKey(e.rec.id, e.channel), deg);
    emit dataChanged(index(row), index(row), {RotationRole});
}

QVariantList DeviceManager::hostIds() const
{
    QVariantList ids;
    for (const Entry &e : m_entries)
        if (!ids.contains(e.rec.id))
            ids.append(e.rec.id);
    return ids;
}

void DeviceManager::toggleFloodlight(int row)
{
    const QString cmd = QStringLiteral("SetWhiteLed");
    auto client = clientFor(row);
    if (!client) {
        emit settingApplied(row, cmd, false, tr("device not ready"));
        return;
    }
    if (row < m_entries.size() && !m_entries.at(row).isAdmin) {
        emit settingApplied(row, cmd, false, tr("requires an administrator account"));
        return;
    }
    const int ch = m_entries.at(row).channel;
    m_pending.addFuture(QtConcurrent::run([this, client, row, ch, cmd] {
        // Read the current config so the toggle flips only on/off and leaves the
        // user's brightness + auto/schedule mode intact.
        const api::CommandResult got =
            client->callOne(QStringLiteral("GetWhiteLed"), Json{{"channel", ch}}, /*action=*/1);
        bool ok = false;
        QString error;
        if (got.ok) {
            const Json wl = jsonObj(got.value, "WhiteLed");
            Json set = Json{{"channel", ch}, {"state", jsonInt(wl, "state", 0) ? 0 : 1}};
            for (const char *k : {"mode", "bright", "LightingSchedule"})
                if (wl.contains(k))
                    set[k] = jsonRef(wl, k); // preserve settable fields verbatim
            const api::CommandResult put =
                client->callOne(cmd, Json{{"WhiteLed", set}}, /*action=*/0);
            ok = put.ok;
            error = ok ? QString() : (put.detail.isEmpty() ? tr("failed") : put.detail);
        } else {
            error = got.detail.isEmpty() ? tr("floodlight not supported") : got.detail;
        }
        QMetaObject::invokeMethod(
            this, [this, row, cmd, ok, error] { emit settingApplied(row, cmd, ok, error); },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::reboot(int row)
{
    applySetting(row, QStringLiteral("Reboot"), {});
}

// Baichuan cmd_id pairs for the alert toggles (verified on the RLN8-410).
static bool alertCmds(const QString &kind, quint32 &getCmd, quint32 &setCmd)
{
    if (kind == QLatin1String("push")) { getCmd = 219; setCmd = 218; return true; }
    if (kind == QLatin1String("email")) { getCmd = 217; setCmd = 216; return true; }
    if (kind == QLatin1String("ftp")) { getCmd = 70; setCmd = 71; return true; }
    return false;
}

void DeviceManager::fetchAlerts(int row)
{
    if (row < 0 || row >= m_entries.size()) {
        emit alertsLoaded(row, {});
        return;
    }
    const Entry &e = m_entries.at(row);
    if (e.rec.kind == QLatin1String("stream") || !e.primed) {
        emit alertsLoaded(row, {});
        return;
    }
    BaichuanControl::Params p;
    p.host = e.rec.addr;
    p.username = e.rec.username;
    p.password = e.password;
    const int ch = e.channel;
    m_pending.addFuture(QtConcurrent::run([this, row, p, ch] {
        QVariantMap m;
        BaichuanControl bc(p);
        if (bc.open()) {
            const int push = bc.readEnable(219, ch);
            const int email = bc.readEnable(217, ch);
            const int ftp = bc.readEnable(70, ch);
            bc.close();
            m[QStringLiteral("ok")] = (push >= 0 || email >= 0 || ftp >= 0);
            m[QStringLiteral("push")] = push;
            m[QStringLiteral("email")] = email;
            m[QStringLiteral("ftp")] = ftp;
        } else {
            m[QStringLiteral("ok")] = false;
        }
        QMetaObject::invokeMethod(
            this, [this, row, m] {
                if (m.contains(QStringLiteral("push")))
                    m_pushEnabled[row] = m.value(QStringLiteral("push")).toInt();
                emit alertsLoaded(row, m);
            }, Qt::QueuedConnection);
    }));
}

bool DeviceManager::pushEnabledFor(qint64 hostId, int channel) const
{
    const int row = rowOfHostChannel(hostId, channel);
    // Unknown (cache cold, or push not reported by the device) => allow the
    // notification; only an explicit disabled (0) suppresses it.
    return m_pushEnabled.value(row, 1) != 0;
}

QString DeviceManager::mdZoneBits(const QString &valueTable, int cells) const
{
    const QByteArray raw = QByteArray::fromBase64(valueTable.toLatin1());
    QString out;
    out.reserve(cells);
    for (int i = 0; i < cells; ++i) {
        const int byte = i / 8;
        const bool on = byte < raw.size()
                        && ((static_cast<quint8>(raw[byte]) >> (7 - (i % 8))) & 1);
        out += on ? QLatin1Char('1') : QLatin1Char('0');
    }
    return out;
}

QString DeviceManager::mdZoneTable(const QString &bits) const
{
    const int cells = bits.size();
    QByteArray raw((cells + 7) / 8, '\0');
    for (int i = 0; i < cells; ++i)
        if (bits.at(i) == QLatin1Char('1'))
            raw[i / 8] = static_cast<char>(static_cast<quint8>(raw[i / 8])
                                           | (1 << (7 - (i % 8))));
    return QString::fromLatin1(raw.toBase64());
}

void DeviceManager::warmPushCache()
{
    int i = 0;
    for (int row = 0; row < m_entries.size(); ++row) {
        const Entry &e = m_entries.at(row);
        if (e.rec.kind == QLatin1String("stream") || !e.primed)
            continue;
        m_pushWarmed = true;   // a camera is ready; don't re-warm every poll cycle
        // Stagger so we never open several Baichuan settings sessions at once.
        QTimer::singleShot(i++ * 1200, this, [this, row] { fetchAlerts(row); });
    }
}

void DeviceManager::setAlertEnable(int row, const QString &kind, bool enable)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const Entry &e = m_entries.at(row);
    if (!e.isAdmin) {
        emit settingApplied(row, kind, false, tr("requires an administrator account"));
        return;
    }
    quint32 getCmd = 0, setCmd = 0;
    if (!alertCmds(kind, getCmd, setCmd))
        return;
    BaichuanControl::Params p;
    p.host = e.rec.addr;
    p.username = e.rec.username;
    p.password = e.password;
    const int ch = e.channel;
    m_pending.addFuture(QtConcurrent::run([this, row, p, ch, kind, getCmd, setCmd, enable] {
        BaichuanControl bc(p);
        const bool ok = bc.open() && bc.writeEnable(getCmd, setCmd, ch, enable);
        bc.close();
        QMetaObject::invokeMethod(
            this,
            [this, row, kind, ok, enable] {
                if (ok && kind == QLatin1String("push"))
                    m_pushEnabled[row] = enable ? 1 : 0;
                emit settingApplied(row, kind, ok,
                                    ok ? QString() : tr("couldn't reach the device"));
            },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::fetchRecSchedule(int row)
{
    if (row < 0 || row >= m_entries.size()) {
        emit recScheduleLoaded(row, {});
        return;
    }
    const Entry &e = m_entries.at(row);
    if (e.rec.kind == QLatin1String("stream") || !e.primed) {
        emit recScheduleLoaded(row, {});
        return;
    }
    BaichuanControl::Params p{e.rec.addr, 9000, e.rec.username, e.password};
    const int ch = e.channel;
    m_pending.addFuture(QtConcurrent::run([this, row, p, ch] {
        QVariantMap m;
        BaichuanControl bc(p);
        if (bc.open()) {
            const QByteArray xml = bc.get(81, ch);
            bc.close();
            // Parse <item><type>T</type><valueTable>V</valueTable></item> pairs.
            int pos = 0;
            while (true) {
                const int t0 = xml.indexOf("<type>", pos);
                if (t0 < 0)
                    break;
                const int t1 = xml.indexOf("</type>", t0);
                const int v0 = xml.indexOf("<valueTable>", t1);
                const int v1 = xml.indexOf("</valueTable>", v0);
                if (t1 < 0 || v0 < 0 || v1 < 0)
                    break;
                m.insert(QString::fromLatin1(xml.mid(t0 + 6, t1 - t0 - 6)),
                         QString::fromLatin1(xml.mid(v0 + 12, v1 - v0 - 12)));
                pos = v1;
            }
            const int e0 = xml.indexOf("<enable>");
            if (e0 >= 0)
                m.insert(QStringLiteral("enable"), xml.mid(e0 + 8, 1) == "1");
        }
        QMetaObject::invokeMethod(
            this, [this, row, m] { emit recScheduleLoaded(row, m); }, Qt::QueuedConnection);
    }));
}

void DeviceManager::writeRecSchedule(int row, const QString &type, const QString &table)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const Entry &e = m_entries.at(row);
    if (!e.isAdmin) {
        emit settingApplied(row, QStringLiteral("SetRecSchedule"), false,
                            tr("requires an administrator account"));
        return;
    }
    BaichuanControl::Params p{e.rec.addr, 9000, e.rec.username, e.password};
    const int ch = e.channel;
    const QByteArray typeTag = "<type>" + type.toLatin1() + "</type>";
    const QByteArray newTable = table.toLatin1();
    m_pending.addFuture(QtConcurrent::run([this, row, p, ch, typeTag, newTable] {
        bool ok = false;
        QString err;
        BaichuanControl bc(p);
        if (bc.open()) {
            QByteArray xml = bc.get(81, ch);
            const int t = xml.indexOf(typeTag);
            const int v0 = t >= 0 ? xml.indexOf("<valueTable>", t) : -1;
            const int v1 = v0 >= 0 ? xml.indexOf("</valueTable>", v0) : -1;
            if (v1 > 0) {
                const QByteArray mutated =
                    xml.left(v0 + 12) + newTable + xml.mid(v1);
                quint16 st = 0;
                bc.transact(82, ch, mutated, &st);
                ok = st == 200 || st == 201 || st == 300;
                if (!ok)
                    err = tr("device rejected the schedule (status %1)").arg(st);
            } else {
                err = tr("schedule type not found on this device");
            }
            bc.close();
        } else {
            err = tr("couldn't reach the device");
        }
        QMetaObject::invokeMethod(
            this,
            [this, row, ok, err] {
                emit settingApplied(row, QStringLiteral("SetRecSchedule"), ok, err);
            },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::fetchBcConfig(int row, int cmdId, const QString &reqBody)
{
    if (row < 0 || row >= m_entries.size()) {
        emit bcConfigLoaded(row, cmdId, {});
        return;
    }
    const Entry &e = m_entries.at(row);
    if (e.rec.kind == QLatin1String("stream") || !e.primed) {
        emit bcConfigLoaded(row, cmdId, {});
        return;
    }
    BaichuanControl::Params p;
    p.host = e.rec.addr;
    p.username = e.rec.username;
    p.password = e.password;
    const int ch = e.channel;
    const QByteArray body = reqBody.toUtf8();
    m_pending.addFuture(QtConcurrent::run([this, row, cmdId, p, ch, body] {
        QVariantMap m;
        BaichuanControl bc(p);
        if (bc.open()) {
            m = bc.getConfigFlat(static_cast<quint32>(cmdId), ch, body);
            bc.close();
        }
        QMetaObject::invokeMethod(
            this, [this, row, cmdId, m] { emit bcConfigLoaded(row, cmdId, m); },
            Qt::QueuedConnection);
    }));
}

void DeviceManager::writeBcConfig(int row, int getCmd, int setCmd, const QVariantMap &changes,
                                  const QString &reqBody)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const Entry &e = m_entries.at(row);
    const QString label = QStringLiteral("Set%1").arg(setCmd);
    if (!e.isAdmin) {
        emit settingApplied(row, label, false, tr("requires an administrator account"));
        return;
    }
    BaichuanControl::Params p;
    p.host = e.rec.addr;
    p.username = e.rec.username;
    p.password = e.password;
    const int ch = e.channel;
    const QByteArray body = reqBody.toUtf8();
    m_pending.addFuture(QtConcurrent::run(
        [this, row, getCmd, setCmd, p, ch, changes, body, label] {
            BaichuanControl bc(p);
            const bool ok = bc.open()
                && bc.writeFields(static_cast<quint32>(getCmd), static_cast<quint32>(setCmd), ch,
                                  changes, body);
            bc.close();
            QMetaObject::invokeMethod(
                this,
                [this, row, label, ok] {
                    emit settingApplied(row, label, ok,
                                        ok ? QString() : tr("couldn't reach the device"));
                },
                Qt::QueuedConnection);
        }));
}

QString DeviceManager::nameAt(int row) const
{
    if (row < 0 || row >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(row);
    return e.chanName.isEmpty() ? e.rec.name : e.chanName;
}

QString DeviceManager::liveUrl(int row, bool mainStream)
{
    Q_UNUSED(mainStream)
    if (row < 0 || row >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(row);
    // Only a plain stream device has a URL: Reolink cameras and NVRs stream
    // live inside their Baichuan session (startBaichuanLive).
    if (e.rec.kind != QLatin1String("stream"))
        return {};
    if (e.rec.username.isEmpty() && e.password.isEmpty())
        return e.rec.addr;
    QUrl u(e.rec.addr);
    u.setUserName(e.rec.username);
    u.setPassword(e.password);
    return u.toString(QUrl::FullyEncoded);
}

} // namespace rl
