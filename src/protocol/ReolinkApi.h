#pragma once

#include "Json.h"

#include <QDateTime>
#include <QString>
#include <QVariant>
#include <QVector>

namespace rl {

// Pure request-building and response-parsing for the Reolink HTTP-CGI JSON API.
// No I/O here — everything is unit-testable. Transport lives in ReolinkHttpClient.
//
// Wire shape (from the official API guide and reolink_aio):
//   POST http(s)://host:port/cgi-bin/api.cgi?cmd=<FirstCmd>&token=<token>
//   body: JSON array of {"cmd": ..., "action": 0|1, "param": {...}}
//   response: JSON array of {"cmd": ..., "code": 0, "value": {...}}
//        or   {"cmd": ..., "code": 1, "error": {"rspCode": <neg>, "detail": "..."}}
namespace api {

// rspCode values observed across firmware (behaviors validated against reolink_aio/HA;
// treat as "observed, verify against target firmware" per DESIGN.md §4).
enum RspCode {
    RspLoginRequired = -6,   // "please login first" — token missing/expired
    RspLoginFailed = -7,     // generic login failure (older firmware)
    RspNotSupported = -9,    // command not supported by this device
    RspPasswordWrong = -502, // wrong password (verified on real firmware; carries
                             // error.auth_warning_info.remain_times before lockout)
};

struct CommandResult {
    QString cmd;
    bool ok = false;
    Json value;      // "value" object when ok
    Json range;      // "range" object (action=1 only): slider bounds, enums
    int rspCode = 0; // negative device error when !ok
    QString detail;
};

struct BatchResult {
    bool transportOk = false; // false: malformed/unparseable body
    QString error;
    QVector<CommandResult> results;
    // True when any command failed because the token is missing/expired.
    bool needsRelogin() const;
};

Json command(const QString &cmd, Json param = Json::object(), int action = 0);
Json loginBody(const QString &username, const QString &password);

// JSON <-> QVariant bridge so QML settings panels can bind directly to device
// config without a typed parser per command.
QVariant toVariant(const Json &j);
Json toJson(const QVariant &v);

QString apiUrl(const QString &host, int port, bool https, const QString &firstCmd,
               const QString &token = {});

BatchResult parseBatch(const QByteArray &body);

struct LoginResult {
    bool ok = false;
    QString token;
    int leaseTimeSec = 0;
    QString error;
    bool wrongPassword = false; // rspCode -502
    int remainingAttempts = -1; // error.auth_warning_info.remain_times (-1 = unknown)
};
LoginResult parseLogin(const QByteArray &body);

// ---- PTZ ------------------------------------------------------------------
// Operations accepted by PtzCtrl (Reolink HTTP API). Directional ops run until a
// matching Stop; ToPos/Auto/Patrol take an id.
namespace ptz {
inline constexpr auto Left = "Left";
inline constexpr auto Right = "Right";
inline constexpr auto Up = "Up";
inline constexpr auto Down = "Down";
inline constexpr auto LeftUp = "LeftUp";
inline constexpr auto RightUp = "RightUp";
inline constexpr auto LeftDown = "LeftDown";
inline constexpr auto RightDown = "RightDown";
inline constexpr auto ZoomInc = "ZoomInc";
inline constexpr auto ZoomDec = "ZoomDec";
inline constexpr auto FocusInc = "FocusInc";
inline constexpr auto FocusDec = "FocusDec";
inline constexpr auto Stop = "Stop";
inline constexpr auto ToPos = "ToPos"; // go to preset (needs presetId)
} // namespace ptz

// Build a PtzCtrl command. presetId >= 0 is included (for ToPos); speed is clamped
// by the device to its own range (typically 1..64).
Json ptzCtrl(int channel, const QString &op, int speed = 32, int presetId = -1);


// ---- Capabilities (GetAbility) --------------------------------------------
// Per-channel capability flags parsed from Ability.abilityChn[i]. Field names
// follow reolink_aio; unknown/absent capabilities degrade to false so the UI
// simply hides the control. Verify against target firmware (DESIGN §6.10).
struct ChannelCaps {
    bool ptz = false;
    bool panTilt = false;    // can actually pan/tilt (not just zoom/focus)
    bool ptzPreset = false;
    bool ptzPatrol = false;
    bool ptzGuard = false;   // guard point (GetPtzGuard / SetPtzGuard)
    bool ptzSpeed = false;   // PtzCtrl honours "speed"
    bool ptzCheck = false;   // PtzCheck (calibration)
    bool autoTrack = false;  // AI tracking (GetAiCfg aiTrack/bSmartTrack)
    bool autoFocus = false;  // GetAutoFocus / SetAutoFocus
    bool zoomFocusPos = false; // StartZoomFocus ZoomPos/FocusPos sliders
    bool zoom = false;
    bool focus = false;
    bool ai = false;
    bool aiPeople = false;
    bool aiVehicle = false;
    bool aiDogCat = false;
    bool audio = false;
    bool talk = false; // two-way audio (verified per-channel on real firmware)
    bool siren = false;
    bool floodlight = false;
    bool battery = false;
    bool doorbell = false;
    bool supportsBalanced = false; // exposes a third ("Balanced") stream
};
struct Capabilities {
    bool valid = false;
    bool talk = false;    // any channel supports two-way audio
    bool p2p = false;
    bool isAdmin = false; // logged-in user may change settings / reboot / manage users
    QVector<ChannelCaps> channels;
};
Capabilities parseAbility(const Json &value);

// ---- Channels (GetChannelstatus, NVR fan-out) -----------------------------
// An NVR reports its bound cameras here; each online entry becomes a live pane.
struct ChannelInfo {
    int channel = 0;
    QString name;
    bool online = false;
    QString uid; // per-camera UID (for P2P), may be empty
};
QVector<ChannelInfo> parseChannelStatus(const Json &value);

// ---- Detection state (pushed by the device, cmd 33) -----------------------
struct DetectionState {
    bool motion = false;
    bool person = false;
    bool vehicle = false;
    bool pet = false;
};

// ---- Battery (GetBatteryInfo, battery/solar cameras) ----------------------
struct BatteryInfo {
    bool present = false;
    int percent = 0;
    bool charging = false; // adapter or solar input active
};
BatteryInfo parseBatteryInfo(const Json &value);

// ---- Playback search ------------------------------------------------------
// Search recorded files for a channel in [start,end]. streamType is "main"/"sub".
// onlyStatus=true asks only for the per-month day bitmap (calendar dots).
Json searchBody(int channel, const QDateTime &start, const QDateTime &end,
                const QString &streamType = QStringLiteral("sub"), bool onlyStatus = false);

struct RecordingFile {
    QString name;      // file handle (empty on NVR firmware — identify by start)
    QDateTime start;
    QDateTime end;
    // The NVR's playback reference time for this file. It differs from `start`
    // (wall-clock) by the device's UTC offset, and is the value the HTTP-FLV
    // playback endpoint expects as its `start=` — passing wall-clock `start`
    // makes the NVR silently drop the connection. See playbackFlvUrl.
    QDateTime playbackTime;
    QString streamType; // "main"/"sub" (NOT the trigger type on NVR firmware)
    qint64 size = 0;
    int triggers = 0;   // trigger:: flags decoded from `name` (0 when unnamed)
};
struct SearchResult {
    bool ok = false;
    QVector<RecordingFile> files;
    QVector<int> recordingDays; // days-of-month (1..31) with recordings, from Status
    QString error;
};
SearchResult parseSearch(const Json &value);

// What triggered a recording, decoded from its file name like reolink_aio's
// parse_file_name: RecXVV_<date>_<start>_<end>_[...]_<hexflags>_<size>, where
// the hex flags read as a bit string, left to right. 0 when the name doesn't
// match (e.g. NVR firmware, which returns no name).
namespace trigger {
enum : int { Timer = 1, Motion = 2, Person = 4, Vehicle = 8, Animal = 16, Doorbell = 32 };
}
int recordingTriggers(const QString &fileName);

// ---- Full-resolution clip download (NvrDownload + Download) ----------------
// HTTP-FLV can't carry the HEVC main stream, so full-res playback goes through
// the clip-download path the official client uses: NvrDownload enumerates the
// clip(s) covering [start,end] (local time), then downloadUrl() fetches one.
// The endpoint does NOT support HTTP range, so the whole clip must be fetched to
// disk before a demuxer can open it (its moov box is at the end and unseekable).
Json nvrDownloadBody(int channel, const QDateTime &start, const QDateTime &end,
                     const QString &streamType = QStringLiteral("main"));

struct DownloadFile {
    QString fileName; // opaque handle passed to Download as `source`
    qint64 size = 0;  // bytes (clipped to the requested range)
};
QVector<DownloadFile> parseNvrDownload(const Json &value);

QString downloadUrl(const QString &host, int port, bool https, const QString &fileName,
                    const QString &token, const QString &output = QStringLiteral("clip.mp4"));

} // namespace api
} // namespace rl
