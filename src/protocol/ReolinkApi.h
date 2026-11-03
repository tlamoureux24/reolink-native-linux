#pragma once

#include "Json.h"

#include <QVariant>

namespace rl {

// What the client knows of a Reolink device, whichever command told it: the
// capabilities of a channel, its detection state, its battery, what triggered
// a recording. Filled from Baichuan replies (bcdev, bcrec); the names follow
// the Reolink HTTP API's, which the UI was written against.
namespace api {

// JSON <-> QVariant bridge (diagnostics print the UI's values as JSON).
QVariant toVariant(const Json &j);
Json toJson(const QVariant &v);

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
struct DetectionState {
    bool motion = false;
    bool person = false;
    bool vehicle = false;
    bool pet = false;
};

struct BatteryInfo {
    bool present = false;
    int percent = 0;
    bool charging = false; // adapter or solar input active
};

// What triggered a recording (cmd 15 <recordType>, bcrec::recordTriggers).
namespace trigger {
enum : int { Timer = 1, Motion = 2, Person = 4, Vehicle = 8, Animal = 16, Doorbell = 32 };
}

} // namespace api
} // namespace rl
