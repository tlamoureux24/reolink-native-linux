#pragma once

#include "core/CredentialStore.h"
#include "core/Database.h"
#include "protocol/BaichuanClient.h"
#include "protocol/ReolinkApi.h"
// Not forward-declared on purpose — see the note by the other forward
// declarations below.
#include "media/StreamPlayer.h"

#include <QAbstractListModel>
#include <QPointer>
#include <QDateTime>
#include <QFutureSynchronizer>
#include <QHash>
#include <QSet>
#include <QSize>
#include <QTimer>

#include <memory>

namespace rl {
// These two are only ever held behind pointers in private members and never
// appear in a signal/slot/Q_INVOKABLE signature, so moc needs nothing from
// them and a forward declaration is right.
//
// StreamPlayer is different and is INCLUDED above: it appears as a pointer
// parameter of Q_INVOKABLE methods, so moc instantiates Qt's metatype traits
// for rl::StreamPlayer* in its own translation unit. Qt decides
// IsPointerToTypeDerivedFromQObject by overload resolution against QObject*,
// which silently answers "false" for an incomplete type and "true" once the
// class is visible — so the same template specialisation had different values
// in the moc TU than in the TUs that include StreamPlayer.h. That is an ODR
// violation (GCC 16 reports it as -Wsfinae-incomplete) and it left the
// registered metatype without its PointerToQObject flag.
class BaichuanClient;
class BcSession;
}

namespace rl {

// The device tree behind the sidebar: hosts from the database, exposed to QML.
// Every camera/NVR has one Baichuan session (BcSession), opened on a worker
// thread when it is added or the app starts; its identity, abilities and live
// streams go through it. Credentials live in the keyring; the in-memory copy
// (loaded on a worker thread) spares the GUI thread a DBus round trip.
class DeviceManager : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        AddrRole,
        KindRole,
        ModelRole,
        OnlineRole,
        StatusRole,
        HostIdRole,
        HasPtzRole,
        HasPtzPresetRole,
        HasZoomRole,
        HasAudioRole,
        HasSirenRole,
        HasFloodlightRole,
        HasBatteryRole,
        HasTalkRole,
        IsAdminRole,
        BatteryPercentRole,
        BatteryChargingRole,
        ChannelRole,
        ProblemRole,
        RotationRole,
    };

    DeviceManager(Database *db, CredentialStore *credentials, QObject *parent = nullptr);
    ~DeviceManager() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Camera/NVR by IP or hostname. Validates asynchronously; the row appears
    // immediately with status "connecting…".
    // Probe a device WITHOUT persisting anything: the Add Device dialog calls
    // this first so a typo'd address or rejected password becomes an inline
    // error instead of a phantom "connecting…" row. Result via testDeviceResult.
    Q_INVOKABLE void testDevice(const QString &addr, const QString &username,
                                const QString &password, bool https, int port);
    // User-driven revalidation of a host (sidebar "Reconnect").
    Q_INVOKABLE void reconnect(int row);
    // Status words are stored translated; re-translate them after a language
    // switch (problem statuses keep their detail until the next poll).
    void retranslateStatuses();
    // Fix a host's credentials in place (sidebar "Update credentials…"): stores
    // the new username/password and revalidates. Empty password keeps the stored
    // one (username-only fix).
    Q_INVOKABLE void updateCredentials(int row, const QString &username,
                                       const QString &password);
    // Manual view-rotation correction for a camera (degrees, multiples of 90),
    // persisted per hostId:channel. For streams whose rotation our heuristics
    // can't know (hub-attached doorbells and the like) — issue #3.
    Q_INVOKABLE void setRotationOverride(int row, int degrees);

    Q_INVOKABLE void addDevice(const QString &addr, const QString &username,
                               const QString &password, bool https = true, int port = 0);
    // Direct stream URL (rtsp://…, file …) — testing and generic-RTSP escape hatch.
    // Any embedded credentials are stripped to the keyring, not persisted in the DB.
    Q_INVOKABLE void addStreamUrl(const QString &name, const QString &url);
    Q_INVOKABLE void removeDevice(int row);

    // Playable URL of a plain stream device; empty for Reolink devices (their
    // live view is startBaichuanLive) and until credentials are loaded.
    Q_INVOKABLE QString liveUrl(int row, bool mainStream = true);

    // GetEnc-declared display size for a channel's main/sub stream (e.g. main
    // 7680x2160). Lets StreamPlayer detect a transmitted-rotated stream by
    // comparing this against the decoded frame size. Invalid if not yet primed.
    Q_INVOKABLE QSize declaredSize(int row, bool mainStream) const;
    Q_INVOKABLE QString nameAt(int row) const;
    Q_INVOKABLE int rowOfHost(qint64 hostId) const { return rowForHostId(hostId); }
    // The exact camera row for a host+channel (an NVR shares a host across cameras).
    Q_INVOKABLE int rowOfHostChannel(qint64 hostId, int channel) const
    {
        for (int i = 0; i < m_entries.size(); ++i)
            if (m_entries.at(i).rec.id == hostId && m_entries.at(i).channel == channel)
                return i;
        return rowForHostId(hostId);
    }
    // A Reolink camera or NVR (live over Baichuan), not a plain stream URL.
    Q_INVOKABLE bool usesBaichuanAt(int row) const
    {
        return row >= 0 && row < m_entries.size()
               && m_entries.at(row).rec.kind != QLatin1String("stream");
    }
    Q_INVOKABLE bool isOnlineAt(int row) const
    {
        return row >= 0 && row < m_entries.size() && m_entries.at(row).online;
    }
    Q_INVOKABLE bool isAdminAt(int row) const
    {
        return row >= 0 && row < m_entries.size() && m_entries.at(row).isAdmin;
    }
    // This camera's channel index on its host (0 for a standalone camera; the NVR
    // channel for a fanned-out one). Lets QML target a live action (siren, etc.) at
    // the correct camera instead of a hardcoded channel 0.
    Q_INVOKABLE int channelOf(int row) const
    {
        return (row >= 0 && row < m_entries.size()) ? m_entries.at(row).channel : 0;
    }

    // Host-level summary (an NVR or a standalone camera), keyed by hostId — feeds
    // the sidebar's NVR header and the NVR properties dialog. Keys: name, kind,
    // model, addr, port, https, username, online, channelCount, onlineCount,
    // isAdmin, firstRow.
    Q_INVOKABLE QVariantMap hostInfo(qint64 hostId) const;
    // Per-camera summary for the camera properties dialog. Keys: name, hostName,
    // hostId, channel, kind, model, codec, mainSize, subSize, uid, online, isAdmin,
    // and cap* booleans (ptz/zoom/audio/siren/floodlight/battery/talk).
    Q_INVOKABLE QVariantMap cameraInfo(int row) const;
    // Distinct host ids in list order (NVRs and standalone cameras), for grouping.
    Q_INVOKABLE QVariantList hostIds() const;

    // Live controls (channel 0). Fire-and-forget on the worker pool.
    Q_INVOKABLE void ptzMove(int row, const QString &op, int speed = 32);
    Q_INVOKABLE void ptzStop(int row);
    Q_INVOKABLE void ptzPreset(int row, int presetId);
    // Two-way talk: stream the microphone to this camera until stopTalk().
    // One talk session per camera; talkStateChanged reports start/stop/errors.
    Q_INVOKABLE void startTalk(int row);
    Q_INVOKABLE void stopTalk(int row);
    Q_INVOKABLE bool isTalking(int row) const;
    // Start/stop a patrol (cruise) by its GetPtzPatrol id. Emits ptzPatrolDone.
    Q_INVOKABLE void ptzPatrol(int row, int patrolId, bool start);
    // Capture a JPEG snapshot; emits snapshotSaved/snapshotFailed.
    Q_INVOKABLE void snapshot(int row);
    // Quiet snapshot for an event thumbnail: fetches a Snap JPEG, downscales it,
    // writes it under Paths::thumbnailsDir(), and emits eventThumbnailReady with
    // the caller's cookie (no snapshotSaved toast). Silent no-op on failure.
    void captureEventThumbnail(qint64 hostId, int channel, qint64 eventId);

    // Playback, over the device's Baichuan session as the official client does:
    // search a day's recordings (cmd 14/15; emits recordingsFound with a list of
    // {start,end,type,triggers} where start/end are seconds into the day, then
    // recordingDaysFound), and the month's days with recordings (cmd 142).
    Q_INVOKABLE void searchRecordings(int row, int year, int month, int day);
    // Calendar dots for a whole month (emits recordingDaysFound only).
    Q_INVOKABLE void searchRecordingDays(int row, int year, int month);
    // Save a main-stream clip covering [startEpoch, +durationSecs] into
    // ~/Videos/Reolink with a friendly name. Emits clipExported/clipExportFailed.
    Q_INVOKABLE void exportClip(int row, qint64 startEpoch, int durationSecs);

    // Native (Baichuan/TCP 9000) recorded playback: stream the recording from
    // startEpoch straight into `player` — realtime, frame-accurate, full HEVC main.
    Q_INVOKABLE void startBaichuanPlayback(int row, qint64 startEpoch, rl::StreamPlayer *player,
                                           bool mainStream = true);
    // Live view (cmd 3 Preview) inside the device's Baichuan session, main or
    // sub stream, as the official client does; panes watching the same channel
    // and stream share it.
    Q_INVOKABLE void startBaichuanLive(int row, rl::StreamPlayer *player,
                                       bool mainStream = true);
    // Seek the active Baichuan playback (same row) to a new instant in place — no
    // reconnect. Returns false if there's no live session for `row` (caller should
    // then startBaichuanPlayback instead).
    Q_INVOKABLE bool seekBaichuanPlayback(int row, qint64 startEpoch);

    // Settings: fetch a batch of Get* commands (emits settingsLoaded with a map
    // of cmd -> value) and apply one Set* command (emits settingApplied).
    // A non-empty tag is echoed back as values["_tag"] (failures included, as
    // "_error"), so a panel other than Device Settings — e.g. the PTZ panel —
    // can fetch without the Settings page mistaking the reply for its own.
    Q_INVOKABLE void fetchSettings(int row, const QStringList &getCommands,
                                   const QString &tag = QString());
    Q_INVOKABLE void applySetting(int row, const QString &setCommand, const QVariantMap &param);
    // Toggle the camera's white-LED / floodlight on<->off. Reads GetWhiteLed first
    // so only the on/off state flips, leaving brightness and the auto/schedule mode
    // untouched. Emits settingApplied("SetWhiteLed", ...) for UI feedback.
    Q_INVOKABLE void toggleFloodlight(int row);
    Q_INVOKABLE void reboot(int row);

    // Alert-action config (push / email / ftp enable) over the native Baichuan
    // protocol (TCP 9000). The NVR's HTTP-CGI 502s on these commands under load,
    // but the app-native BC transport the official clients use is reliable.
    // fetchAlerts reads the current enables (emits alertsLoaded with keys
    // ok/push/email/ftp, each 0/1 or -1); setAlertEnable toggles one over BC and
    // emits settingApplied(kind, ...). kind = "push" | "email" | "ftp".
    Q_INVOKABLE void fetchAlerts(int row);
    Q_INVOKABLE void setAlertEnable(int row, const QString &kind, bool enable);

    // Whether this camera's Push Notifications action is enabled, from a cache
    // warmed shortly after startup and updated whenever alerts are read/toggled.
    // Gates desktop notifications for new detections; unknown => treated enabled.
    bool pushEnabledFor(qint64 hostId, int channel) const;

    // Motion-detection zone (grid mask) helpers. The device stores the zone as a
    // base64 <valueTable> in the MdAlarm config: one bit per cell (1 = detect),
    // row-major over columns x rows. mdZoneBits decodes it to a '0'/'1' string of
    // `cells` chars for the editor; mdZoneTable re-encodes a bit string to the
    // base64 the device expects (write it back via writeBcConfig(row, 46, 47, ...)).
    Q_INVOKABLE QString mdZoneBits(const QString &valueTable, int cells) const;
    Q_INVOKABLE QString mdZoneTable(const QString &bits) const;

    // Generic settings over Baichuan: read a command's config into a flat
    // { tag: value } map (emits bcConfigLoaded), and read-modify-write leaf tags
    // (emits settingApplied("Set<setCmd>", ...)). reqBody is the optional GET
    // request XML some commands need (e.g. AI detection selects an ai_type).
    // These bypass the 502-prone HTTP-CGI, like the official apps.
    Q_INVOKABLE void fetchBcConfig(int row, int cmdId, const QString &reqBody = QString());
    // Weekly recording schedule (BC cmd 81/82): per-type 168-char '0'/'1'
    // tables (7 days x 24 hours). fetchRecSchedule emits recScheduleLoaded with
    // { enable, <type>: table } (types e.g. Normal/MD/people/vehicle/dog_cat);
    // writeRecSchedule RMWs one type's table (emits settingApplied).
    Q_INVOKABLE void fetchRecSchedule(int row);
    Q_INVOKABLE void writeRecSchedule(int row, const QString &type, const QString &table);
    Q_INVOKABLE void writeBcConfig(int row, int getCmd, int setCmd, const QVariantMap &changes,
                                   const QString &reqBody = QString());

signals:
    void countChanged();
    void deviceError(const QString &addr, const QString &message);
    // Outcome of testDevice. problem is "", "transport", "auth", "locked" or
    // "protocol" so the dialog can phrase the failure usefully.
    void testDeviceResult(bool ok, const QString &message, const QString &name,
                          const QString &model, const QString &problem);
    void snapshotSaved(int row, const QString &path);
    void snapshotFailed(int row, const QString &error);
    void recordingsFound(int row, const QVariantList &segments);
    void recordingDaysFound(int row, int year, int month, const QVariantList &days);
    void recordingsFailed(int row, const QString &error);
    void clipExported(int row, const QString &path);
    void clipExportFailed(int row, const QString &error);
    void settingsLoaded(int row, const QVariantMap &values);
    void settingsFailed(int row, const QString &error);
    void settingApplied(int row, const QString &command, bool ok, const QString &error);
    void ptzPatrolDone(int row, bool ok, const QString &error);
    void talkStateChanged(int row, bool active, const QString &error);
    void alertsLoaded(int row, const QVariantMap &values);
    void bcConfigLoaded(int row, int cmdId, const QVariantMap &values);
    void recScheduleLoaded(int row, const QVariantMap &values);
    // Emitted on each detection 0->1 transition (feeds the event inbox).
    void detectionEvent(qint64 hostId, int channel, const QString &type, const QString &camera);
    // A quiet event-thumbnail capture finished (path is a local JPEG).
    void eventThumbnailReady(qint64 eventId, const QString &path);
    // A camera (channel >= 0) or a whole host (channel == -1) changed
    // connectivity. name is the camera/host display name.
    void connectivityChanged(qint64 hostId, int channel, const QString &name, bool online);

private slots:
    void pollDetections();

private:
    // One model row = one camera. A standalone camera is a host with a single
    // channel (0); an NVR fans out into one Entry per online channel, all sharing
    // the same host session (hostId, addr, credentials, Baichuan session).
    // Why a host isn't online. Drives both the sidebar visuals and the retry
    // policy: Unreachable is auto-retried with backoff (device off, wrong IP,
    // network down — retrying is free); Auth/Locked are NEVER auto-retried,
    // because each rejected login burns the firmware's lockout counter.
    enum class Problem { None, Connecting, Unreachable, Auth, Locked };

    struct Entry {
        HostRecord rec;    // host connection (shared across an NVR's channels)
        int channel = 0;   // channel index on the host
        QString chanName;  // camera name for this channel
        bool online = false;
        QString status;
        Problem problem = Problem::Connecting; // until first validation lands
        int rotationOverride = 0;              // user view-rotation fix (degrees CW)
        QString mainCodec = QStringLiteral("h264"); // sub stream is always h264
        QSize mainSize;                             // GetEnc-declared main resolution
        QSize subSize;                              // GetEnc-declared sub resolution
        QString uid;                                // per-camera UID (Baichuan playback)
        QString password;                           // in-memory only (keyring at rest)
        bool primed = false;                        // password loaded?
        api::ChannelCaps caps;                      // this channel's capabilities
        bool talk = false;                          // channel supports two-way audio
        bool isAdmin = false;                       // logged-in user may edit settings
        api::BatteryInfo battery;                   // battery/solar state (if any)
        QString hostUid; // the host's UID (searches fall back to it)
        // The last searched day's files, per stream (sub draws the timeline).
        QVector<BcReplayFile> filesSub;
        QVector<BcReplayFile> filesMain;
    };

    // One camera's validated state (channel within a host).
    struct ChannelResult {
        int channel = 0;
        QString name;
        bool online = false;
        QString codec; // empty = unknown; the stream is probed instead of assumed
        QSize mainSize; // GetEnc-declared main/sub resolution (for rotation detection)
        QSize subSize;
        QString uid;
        api::ChannelCaps caps;
    };

    // Outcome of a worker-thread validation, applied back on the GUI thread.
    struct Validation {
        bool online = false;
        QString status;
        Problem problem = Problem::Unreachable;
        QString hostName; // device/NVR name from GetDevInfo
        QString model;
        int channelNum = 0;
        bool isAdmin = false;
        bool talk = false;
        QString password;
        QString hostUid;
        api::BatteryInfo battery;
        std::shared_ptr<BcSession> session;
        QVector<ChannelResult> channels; // one per camera to show
    };

    // Runs on a worker: loads the password from the keyring (or stores newPassword
    // first), logs in, fetches device info + encoding + abilities, updates the row
    // on the GUI thread. storeNew persists newPassword to the keyring first.
    void validateAsync(qint64 hostId, const QString &newPassword = QString(),
                       bool storeNew = false);
    int rowForHostId(qint64 hostId) const;
    void warmPushCache();   // fetch each camera's push state once, staggered
    // A channel's detection state from the device's alarm pushes (cmd 33):
    // emits detectionEvent on each rising edge.
    void applyDetection(qint64 hostId, int channel, const api::DetectionState &st);
    // An NVR's channels that dropped or returned (its cmd 145 pushes).
    void applyChannelStates(qint64 hostId, const QHash<int, bool> &chanOnline);
    // Every second: a host whose session ended goes offline and is revalidated.
    void watchSessions();
    // Shared body of startBaichuanPlayback/startBaichuanLive (startEpoch<=0 = live).
    void startBaichuan(int row, qint64 startEpoch, rl::StreamPlayer *player, bool mainStream);
    void applyValidation(qint64 hostId, const Validation &v);
    void postValidation(qint64 hostId, const Validation &v); // marshals to GUI thread

    Database *m_db;
    CredentialStore *m_credentials;
    QVector<Entry> m_entries;
    QFutureSynchronizer<void> m_pending; // drains in-flight validations at teardown

    QTimer m_pollTimer;
    // Keyed by "hostId:channel" so each NVR camera is tracked independently.
    QHash<QString, api::DetectionState> m_lastDetection; // for 0->1 edge detection
    QHash<int, int> m_pushEnabled;  // row -> push enable (1/0/-1) for notif gating
    // Each host's Baichuan session, as in the official client: everything about
    // the device goes through it, and its end is how the client notices a lost
    // device. pushToken: the handler for its pushes (NVR channel states).
    struct HostSession {
        std::shared_ptr<BcSession> session;
        int pushToken = 0;
        quint32 alarmId = 0; // the alarm subscription (cmd 31)
    };
    QHash<qint64, HostSession> m_sessions;
    void dropSession(qint64 hostId);
    QSet<qint64> m_lostHosts;       // hosts whose session ended, announced offline
    QTimer m_sessionTimer;
    bool m_pushWarmed = false;      // one-time warm of the push cache after priming

    // Auto-retry of Unreachable hosts, driven by the poll tick. Backoff doubles
    // 15s -> 300s cap; cleared on success or when the failure is Auth/Locked
    // (those wait for the user — see Problem).
    QHash<qint64, QDateTime> m_nextRetryAt;
    QHash<qint64, int> m_retryDelaySecs;
    QSet<qint64> m_validating;      // hosts with a validation in flight

    // Each row's running recorded playback, so a scrub can seek it in place.
    // Weak: the StreamPlayer owns the client's lifetime.
    QHash<int, std::weak_ptr<BaichuanClient>> m_playbackClients;
    // Running talk sessions, keyed "hostId:channel" (rows can shift).
    QHash<QString, QPointer<class TalkSession>> m_talks;
    QString talkKey(int row) const;
    std::shared_ptr<BcSession> sessionFor(int row) const; // alive, or null
};

} // namespace rl
