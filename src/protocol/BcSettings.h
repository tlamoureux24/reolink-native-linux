#pragma once

#include <QString>
#include <QVariant>
#include <QVariantMap>

// Device settings over Baichuan, offered to the UI in the shape it already
// knows: the HTTP API's command names and JSON (GetPtzPreset → {"PtzPreset":
// [...]}, SetPtzGuard {"PtzGuard": {...}} …), so the panels need not change
// while each command moves to the device's session. Writes follow the
// official SDK and reolink_aio: read the setting, change the fields asked
// for, send it back with the Set command.
//
// Formats are those the devices answered with (see docs/research/
// baichuan-map.md); a command not listed by handles() is not mapped yet.
namespace rl {

class BcSession;

namespace bcset {

bool handles(const QString &command);

// A Get command's value and, when the HTTP API gave one, its range.
bool get(BcSession &s, int channel, const QString &command, QVariant *value, QVariant *range,
         QString *error);
bool set(BcSession &s, int channel, const QString &command, const QVariantMap &param,
         QString *error);

// PTZ moves (cmd 18): the HTTP PtzCtrl operation ("Left", "ZoomInc", "Stop",
// "StartPatrol"…) as the SDK words it ("left", "zoomInc"…).
bool ptzControl(BcSession &s, int channel, const QString &op, int speed, int patrolId = -1);
// Go to a preset (cmd 19, toPos).
bool ptzToPreset(BcSession &s, int channel, int presetId);

} // namespace bcset
} // namespace rl
