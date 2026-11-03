#pragma once

#include "protocol/ReolinkApi.h"

#include <QByteArray>
#include <QSize>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>

// What the client learns about a device when it opens its session, over
// Baichuan as the official client does (no HTTP):
//
//   login reply  <DeviceInfo>   type (ipc / nvr / wifi_nvr …), channelNum
//   cmd 80       <VersionInfo>  name, model, firmware, hardware, serial
//   cmd 114      <Uid>          the device's UID
//   cmd 199      <Support>      abilities, host-level and one <item> per channel
//   cmd 151      <AbilityInfo>  the logged-in user's rights (…_rw / …_ro)
//   cmd 56       <Compression>  per channel: main/sub (and third) stream sizes
//   cmd 252/253  <BatteryInfo>  battery state (battery cameras)
//   cmd 145      <ChannelInfoList> pushed by an NVR: its channels' states
//   cmd 44       <OsdChannelName> per channel: the camera's name
//
// Capability bits follow reolink_aio (MIT), which reads the same <Support>
// reply: see docs/research/baichuan-map.md.
namespace rl {

class BcSession;

namespace bcdev {

struct Channel {
    int channel = 0;
    QString name;
    bool online = true;
    QSize mainSize;
    QSize subSize;
    bool thirdStream = false; // a "Balanced" stream exists
    api::ChannelCaps caps;
};

struct Device {
    QString type;      // login DeviceInfo/type: ipc, nvr, wifi_nvr, homebase…
    bool isNvr = false;
    int channelNum = 1;
    QString name;      // cmd 80
    QString model;
    QString firmware;
    QString hardware;
    QString serial;
    QString uid;       // cmd 114
    bool isAdmin = false;
    bool talk = false; // any channel talks
    api::BatteryInfo battery; // channel 0
    QVector<Channel> channels; // standalone camera: just channel 0
};

// Parsers, one per reply (exposed for tests).
void parseLoginInfo(const QByteArray &xml, Device &d);
void parseVersionInfo(const QByteArray &xml, Device &d);
QString parseUid(const QByteArray &xml);
// <Support> → per-channel capabilities (channelNum entries, by chnID).
QVector<api::ChannelCaps> parseSupport(const QByteArray &xml, int channelNum, bool isNvr);
// <AbilityInfo> → may the user reboot / restore / manage the device.
bool parseIsAdmin(const QByteArray &xml);
void parseCompression(const QByteArray &xml, Channel &c);
api::BatteryInfo parseBattery(const QByteArray &xml);
// <ChannelInfoList> → channel → online.
QVector<QPair<int, bool>> parseChannelStates(const QByteArray &xml);
QString parseChannelName(const QByteArray &xml);

// Read everything above through an open session. Blocking; false when the
// device stopped answering (the session's error() then says why).
bool readDevice(BcSession &s, const QString &username, Device *out);

} // namespace bcdev
} // namespace rl
