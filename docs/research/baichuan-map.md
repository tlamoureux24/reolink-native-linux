# Baichuan map: replacing api.cgi, RTSP and HTTP-FLV

Desk study (2026-11-03), branch `feat/p2p-v2`. Goal: move every feature the
client reaches over HTTP-CGI (`api.cgi?cmd=…`), RTSP and HTTP-FLV onto
Baichuan (TCP 9000), as the official client does. For each HTTP item this
records where the app uses it, the fields it reads/writes, and the Baichuan
command that replaces it. No device was contacted for this study.

## Sources and confidence

- **VERIFIED-SDK**: seen in the official SDK `libBCSDKWrapper.dll` (client
  8.21.30), in one of three ways:
  1. *dispatch table*: the `BaichuanConfigurator` constructor registers 192
     (internal code → wire cmd id) pairs, e.g. `0x842 → 93`. Internal codes are
     tied to `BCSDK_*` exports (the export loads the code). Cited as
     `BCSDK_Name 0xNNN`.
  2. *name table*: an array of 733 strings `NET_<NAME>_V20`. For wire ids
     0–332 the index **is** the wire id: every dispatch-table anchor in that
     range (40+, e.g. 70 `GET_FTPTASK`, 102 `GET_HDD_CFG`, 208
     `GET_LED_STATE`, 299 `GET_AI_CFG`, 332 `GET_GUARD`) and every reolink_aio
     id agree. Above 332 the table is sparse: wire = index + 3 for 336–352 and
     index + 8 for 363–543 (both bracketed by dispatch anchors); outside those
     ranges names are not used here. Cited as `NAME`.
  3. *builder*: a decompiled XML builder showing the element names and, when
     inline, the header cmd id. Only element/field names are recorded.
- **REOLINK_AIO**: implemented in reolink_aio (MIT), `baichuan/baichuan.py`,
  `xmls.py`.
- **UNRESOLVED**: no wire evidence; what was tried is said.

Earlier project findings reused: `docs/research/p2p.md` on
`feat/p2p-remote-access` (live, recordings, keep-alive; verified on devices)
and its stash `b5523ee` (PTZ, snapshot, alarm push; verified on devices).

Conventions (from reolink_aio `send()` and the app's `BaichuanControl`):
message class 0x6414 (24-byte header) for everything after the nonce; header
channel byte = channel + 1 for channel-scoped commands, 250 for host-level,
251 for the event subscription; channel-scoped commands also carry
`<Extension version="1.1"><channelId>N</channelId></Extension>` (plus
`<subChannel>` for multi-lens). "Get-modify-set" means: send the Get, edit
the returned XML, send it back with the Set id (reolink_aio does this for
almost every setter).

## 1. Session, device info, abilities, channels

| HTTP item → app use | Fields used | Baichuan | Body / reply | Conf. |
|---|---|---|---|---|
| `Login` / `Logout` — `ReolinkHttpClient.cpp:259,318` | token | cmd 1 `LOGIN` (nonce request, then `<LoginUser>` + `<LoginNet>`), cmd 2 `LOGOUT` | Login reply carries `<DeviceInfo>` (`type`, `typeInfo` = NVR/WIFI_NVR/HOMEHUB…, `channelNum`, `analogChnNum`, `sleep`) and `<StreamInfoList>` | VERIFIED-SDK (name; builder `LoginUser`/`LoginNet`) + REOLINK_AIO; app already does it |
| keep-alive (HTTP polls today) | — | cmd 93 `GET_LINKTYPE` used as heartbeat (dispatch `0x842 → 93`); UDP uses 234 `HEARTBEAT_PC` | empty body | VERIFIED-SDK; app sends 93 (`BaichuanClient.cpp:404`) |
| `GetDevInfo` — probe `DeviceManager.cpp:725`, test `:395`, System page `DeviceSettingsPage.qml:1070-1076` | name, model, channelNum, firmVer, hardVer | host: cmd 80 `GET_VERSION` (ch 250); per NVR channel: cmd 318 `GET_CHN_VERSION` (`BCSDK_RemoteGetChannelVersionInfo 0x8d5`) | reply keys `name`, `type` (=model), `firmwareVersion`, `hardwareVersion`, `itemNo`, `serialNumber`; channelNum comes from the login `DeviceInfo` | VERIFIED-SDK (names; strings `itemNo`, `serialNumber`, `firmwareVersion`) + REOLINK_AIO |
| NVR or camera (`rec.kind`) | — | login `DeviceInfo/type`, `typeInfo`; SDK `BCSDK_GetIsNVR` (export loads no command code) | — | REOLINK_AIO |
| `GetAbility` — `ReolinkApi.cpp:242` (parseAbility) | per-channel: ptzType, ptzCtrl, ptzPreset, ptzPatrol, supportPtzSpeed/Check, aiTrack, disableAutoFocus, supportAi*, talk/ipcAudioTalk, supportAudioAlarm, floodLight/whiteLed, battery, supportVisitor, supportBalanced; host: p2p, permit bits | cmd 199 `GET_SUPPORT` (ch 250, no body) | reply root `<Support>`: host-level tags plus `<item>` per channel (`chnID`, optional `<subItem>`). Tags used by reolink_aio: `ptzType` (1 zoom only; 2/3/5/6/7 pan-tilt), `ptzControl` bits (1 no diagonal, 2 guard, 3 calibrate, 4 digital zoom, 6 speed), `ptzPreset`, `autoPt`, `supportPtz3DLocation`, `aitype` bits (1 people, 2 vehicle, 3 face, 4 dog_cat, 6 PIR/other, 7/22 AiCfg, 8 delay, 9 sensitivity, 13 track limit, 17 package, 25 tamper), `smartAI`, `motion`, `talk`, `ipcAudioTalk`, `audioTalk`, `audioVersion` (bit 2 siren), `ledCtrl` (bit 0 status LED, bits 1+2 floodlight, 12 IR brightness), `battery`, `batteryMode`, `doorbellVersion`, `recordCfg`, `reboot`, `upgrade`, `timeFormat`, `netPort`, `rtsp`, `rtmp`, `onvif`, `wifi`, `encCtrl`, `osdCfg`, `IOInputPortNum`, `IOOutputPortNum` | VERIFIED-SDK (name; root `Support`, `BCSDK_GetDeviceSupportXML`) + REOLINK_AIO. Tags for `supportBalanced`, `disableAutoFocus` and patrol: UNRESOLVED (not read by reolink_aio) |
| `GetAbility` permit bits → `isAdmin` | userManage/reboot/restore permit | cmd 151 `GET_DEVABILITY`, extension `<Extension><userName>U</userName><token>image, video</token></Extension>`; or cmd 58 user list `userLevel` | reply `<AbilityInfo>` → per token `<subModule>` with `channelId`, `abilityValue` (comma list such as `ledState_rw`, `shelter_rw`); SDK strings also have `reboot_rw`, `upgrade_rw`, `motion_rw`… | VERIFIED-SDK (name; builder with `chnType`, `userName`, `token`) + REOLINK_AIO. Which token carries user/reboot rights: UNRESOLVED |
| `GetChannelstatus` — probe `DeviceManager.cpp:731`, NVR poll `:231` | channel, name, online, uid | cmd 145 `CHANNEL_INFO_REPORT`: **pushed by the NVR** after login, not requestable (reolink_aio comment) | `<ChannelInfoList><ChannelInfo>` `channelId`, `loginState` (reolink_aio tests `standby`); SDK strings beside it: `onlineState`, `fakeUid`, `streamSupport`, `abilityChanged`, `pwdError`, `connectFailed`, `connect`, `disconnect` | VERIFIED-SDK (name, root) + REOLINK_AIO (standby only). Full `loginState` value set: UNRESOLVED. Fallback seen in reolink_aio: cmd 318 fails ⇒ channel offline |
| channel names (NVR) | name | cmd 44 `GET_OSD_CFG` per channel | `OsdChannelName/name` | REOLINK_AIO + VERIFIED-SDK (name) |
| per-channel UID (P2P, search) | uid | host UID cmd 114 `GET_UID_CFG` (`BCSDK_RemoteGetUidInfo 0x84a`) → `uid`; channel UIDs: in cmd 145 report (reolink_aio `get_channel_uids`) | — | VERIFIED-SDK + REOLINK_AIO; per-channel uid tag name: UNRESOLVED |
| `GetEnc` (codec, sizes) — probe `DeviceManager.cpp:727,814`; Encoding page `DeviceSettingsPage.qml:961` | mainStream/subStream vType, size, frameRate, bitRate; audio | cmd 56 `GET_COMPRESSIONCFG` per channel; ranges: cmd 146 `GET_ENC_CAPABILITY` (also in login reply) | 56 reply: `mainStream`/`subStream` with `videoEncType` (0 h264, 1 h265), `width`, `height`, `frame`, `bitRate`, `audio`. 146: `StreamInfoList/StreamInfo` `channelBits`, `encodeTable` (`type`, `width`, `height`, `framerateTable`, `bitrateTable`) | VERIFIED-SDK (names; strings `videoEncType`, `encodeTable`, `channelBits`) + REOLINK_AIO |
| `SetEnc` audio — `DeviceSettingsPage.qml:982` | Enc.audio | cmd 57 `SET_COMPRESSIONCFG`, get-modify-set on 56 (`audio` in each stream) | — | VERIFIED-SDK (name) + REOLINK_AIO |
| `GetBatteryInfo` — `DeviceManager.cpp:730` | batteryPercent, chargeStatus | cmd 253 `GET_BATTERY_INFO` (`BCSDK_RemoteGetBatteryInfo 0x899`); push 252 `REPORT_BATTERY_INFO_LIST` | `BatteryInfo`: `batteryPercent`, `chargeStatus` (none/charging/chargeComplete…), `adapterStatus`, `powerSupplyStatus`, `lowPower`, `temperature`, `voltage` | VERIFIED-SDK + REOLINK_AIO |

## 2. Live

| HTTP item → app use | Baichuan | Body | Conf. |
|---|---|---|---|
| RTSP `liveUrl` — `DeviceManager.cpp:2000`, `ReolinkApi.cpp:187`; used by `LivePane.qml:131`, `PoppedWindow.qml:94` | cmd 3 `PREVIEW` open, cmd 4 `PREVIEW_STOP`; `BCSDK_LiveOpen 0x829`, `BCSDK_LiveClose 0x82a` | `<Preview>` `channelId`, `handle` (streamType<<8 \| channel), `streamType` mainStream/subStream/mobileStream, `policy` newestIFrame; cmd 4: `channelId`, `handle` | VERIFIED-SDK (p2p.md); app already has it (`BaichuanClient.cpp:257`, `startBaichuanLive`) |
| codec for the decoder | 56 `videoEncType` (above), or the media header (`BcMediaParser`) | — | VERIFIED-SDK + REOLINK_AIO |

## 3. Playback, search, download

| HTTP item → app use | Fields used | Baichuan | Body / reply | Conf. |
|---|---|---|---|---|
| `Search` onlyStatus (calendar dots) — `DeviceManager.cpp:1131` | Status day table | cmd 142 `GET_RECFILE_DETAIL_INFO` (`BCSDK_RemoteGetRecFileDaysByChannel 0x836`), one request per month | `<DayRecords>` `fileType`, `startTime`, `endTime`, `<DayRecordList><DayRecord>` `index`, `channelId`, `uid`; reply `dayType/index` per day with recordings | VERIFIED-SDK (builder `DayRecords`) + REOLINK_AIO + device-verified (p2p.md) |
| `Search` day list — `DeviceManager.cpp:1055` (timeline), `:1306` (camera export) | StartTime, EndTime, name, PlaybackTime, triggers | cmd 14 `FILE_FIND` open → `handle`; cmd 15 `FILE_FIND_NEXT` pages; cmd 16 `FILE_FIND_CLOSE` | 14: `FileInfoList/FileInfo` `uid`, `channelId`, `logicChnBitmap`, `streamType`, `recordType` list (manual, sched, md, people, vehicle, dog_cat, visitor, other, package, crossline, intrusion, loitering, legacy…), `searchAITrack`, `startTime`, `endTime`; 15/16: `channelId`, `uid`, `handle`; replies list files with `name`, `Id`, `recordType`, start/end | VERIFIED-SDK (builder, inline id 14) + REOLINK_AIO + device-verified (p2p.md) |
| alarm/AI triggers per clip | triggers bitmask | cmd 272 `START_FIND_ALARM_VIDEO` → `fileHandle`; 273 `DO_FIND_ALARM_VIDEO`; 274 `STOP_FIND_ALARM_VIDEO` | `<findAlarmVideo>` `channelId`, `uid`, `logicChnBitmap`, `streamType` (0 main, 1 sub), `notSearchVideo`, time range, `alarmType` list; reply `alarmVideoInfo` `bFinished`, `alarmVideoList/alarmVideo` `fileName`, `alarmType`, `startTime`, `endTime` | VERIFIED-SDK (names, builder) + REOLINK_AIO |
| HTTP-FLV playback (NVR) `ReolinkApi.cpp:475`; `Playback` MP4 by name (camera) `:499`; used by `PlaybackPage.qml:397,500`, `PlaybackPane.qml:136` | file start, seek | by file: cmd 5 `REPLAY` (header ch N+1 + channel extension), cmd 123 `REPLAY_SEEK`, cmd 7 `REPLAY_STOP`; `BCSDK_PlaybackOpen 0x82c`, `PlaybackClose 0x82d`, `PlaybackSeek 0x818` | 5: `FileInfoList/FileInfo` `channelId`, `Id` (path from cmd 15), `uid`, `supportSub`, `streamType`, `playSpeed`; 123: `<ReplaySeek>` `channelId`, `seq`, `seekTime`; 7: `uid`, `channelId`, `name`, `streamType`, `Id` | VERIFIED-SDK (builders, inline id 5) + device-verified (p2p.md) |
| by-time playback the app uses today — `BaichuanClient.cpp:280` (cmd 143) | — | the SDK names **143 `DOWNLOAD_CUT`** and 144 `DOWNLOAD_CUT_STOP` (download by time, see below), not a replay command. By-time replay in the SDK is 381 `REPLAY_BY_TIME`, 382 `…_STOP`, 383 `…_SEEK` (name table, offset-8 range) and a `<ReplayByTimeV2>` builder (`startTime`, `endTime`, `sOffset`, `eOffset`, `playSpeed`, `streamType`, `durationList/duration`, `channelId`, `logicChnBitmap`) | — | ids VERIFIED-SDK (name); which id carries `ReplayByTimeV2`: UNRESOLVED (builder has no inline id) |
| `NvrDownload` + `Download` (HD clip, export) — `DeviceManager.cpp:1205,1279`, `ReolinkApi.cpp:523,549` | clip names covering [start,end], size | by file: cmd 8 `DOWNLOAD`, stop cmd 9 (`BCSDK_StartDownloadFile 0x830`, `StopDownload 0x831`); by time: cmd 143 `DOWNLOAD_CUT`, stop 144 (`BCSDK_StartDownloadByTime 0x861`, stop `0x862`) | 8: `FileInfoList/FileInfo` `uid` and `Id` (both optional), `name`, `channelId`, `supportSub`, `streamType`, `startTime` (builder, inline id 8). By time: builder `FileInfoList/FileInfo` `uid`, `logicChnBitmap`, `channelId`, `supportSub`, `streamType`, `startTime`, `endTime` (same body the app sends with 143). 143 delivers BcMedia frames (it plays in the app's player); cmd 8's payload format was not checked | VERIFIED-SDK. No "list the clips covering a range" command: search (14/15) then download; the downloader's container/output handling: UNRESOLVED |
| past-frame thumbnail (not used yet) | — | cmd 298 `COVER_PREVIEW` | `<CoverPreview>` `channelId`, `streamType`, time range, `frameList/frameNo` | VERIFIED-SDK + REOLINK_AIO |

```xml
<!-- cmd 142, header ch N+1 -->
<body><DayRecords version="1.1">
  <startTime><year>2026</year><month>11</month><day>1</day>…</startTime>
  <endTime>…</endTime>
  <DayRecordList><DayRecord><index>0</index><channelId>N</channelId><uid>U</uid></DayRecord></DayRecordList>
</DayRecords></body>
```

## 4. Events (motion, AI, visitor)

| HTTP item → app use | Baichuan | Body | Conf. |
|---|---|---|---|
| `GetMdState` + `GetAiState` polled every cycle — `DeviceManager.cpp:220-224`, consumed by `EventManager` | subscribe: cmd 31 `SET_ALARM` on header channel **251**, no body; unsubscribe 32 `STOP_ALARM`; push: cmd 33 `ALARM` | push body `<AlarmEventList><AlarmEvent>` `channelId`, `status` (contains `MD`, `visitor`, `tamper`), `AItype` (comma list: people, vehicle, dog_cat, face, package, other…), optional `smartAiTypeList`; also `<DayNightEvent>` `mode` | VERIFIED-SDK (names, roots `AlarmEventList`/`AlarmEvent`, string `AItype`) + REOLINK_AIO + device-verified (stash b5523ee) |
| keep the subscription alive | resend 31 until events flow, then 93 periodically | — | REOLINK_AIO |
| polling alternative | cmd 131 `GET_ALARMSTATE` exists | — | name VERIFIED-SDK; body/reply UNRESOLVED |
| other pushes worth handling | 145 channel report, 252 battery, 291 floodlight state, 438 floodlight task, 547 siren state, 580 "config modified" (`cmdId` of the changed config), 588 manual record, 623/574 privacy/sleep | — | REOLINK_AIO (ids 291/438 also VERIFIED-SDK names) |

## 5. PTZ, zoom, focus

| HTTP item → app use | Fields | Baichuan | Body / reply | Conf. |
|---|---|---|---|---|
| `PtzCtrl` move/stop — `DeviceManager.cpp:866,876` | op, speed | cmd 18 `PTZ_CONTROL` (`BCSDK_PTZControl 0x83c`) | `<PtzControl>` `channelId`, `speed`, `command` = stop left right up down leftUp leftDown rightUp rightDown irisDec irisInc zoomDec zoomInc focusDec focusInc auto startPatrol stopPatrol | VERIFIED-SDK (builder) + REOLINK_AIO + device-verified (stash) |
| `PtzCtrl` Start/StopPatrol — `DeviceManager.cpp:963` | patrolId | cmd 18, `command` startPatrol/stopPatrol + `patrolId` | — | VERIFIED-SDK (builder) |
| `PtzCtrl` ToPos — `DeviceManager.cpp:886` | presetId | cmd 19 `PTZ_PRESET` (`BCSDK_RemotePresetInvoke 0x870`) | `<PtzPreset>` `channelId`, `presetList/preset` `id`, `command` toPos | VERIFIED-SDK + REOLINK_AIO |
| `GetPtzPreset` — `PtzPanel.qml:64` | id, name, enable | cmd 190 `GET_PTZ_PRESET` (`BCSDK_RemoteGetPresets 0x84e`) | reply `preset` `id`, `name` | VERIFIED-SDK + REOLINK_AIO |
| `SetPtzPreset` add/rename/delete — `PtzPanel.qml:268,274,295` | id, name, enable | cmd 19 with `command` setPos / delPos (`BCSDK_RemoteSetPresets 0x84f`) | `preset` `id`, `name`, `command`, optional `imageName` | VERIFIED-SDK (builder) |
| `GetPtzPatrol` — `PtzPanel.qml:65` | patrol list, stops | cmd 64 `GET_PTZCRUISE` (`BCSDK_RemoteGetCruises 0x850`) | reolink_aio reads only `cruising`; list layout UNRESOLVED (SDK strings `PtzCruise`, `cruisePointPatrol`) | VERIFIED-SDK (id) / fields UNRESOLVED |
| `SetPtzPatrol` — `PtzPanel.qml:384` | id, name, preset stops | cmd 20 `PTZ_CRUISE` (`BCSDK_RemoteSetCruise 0x851`) | UNRESOLVED (builder not traced) | VERIFIED-SDK (id only) |
| `GetPtzGuard` / `SetPtzGuard` — `PtzPanel.qml:66,410-436` | benable, timeout, set/go to pos | 332 `GET_GUARD` / 331 `SET_GUARD` (`BCSDK_RemoteGetGuard 0x8ee`, set `0x8ef`) | `<PtzGuard>` `channelId`, `benable`, `command` setGrd/toGrd, `timeout`, `needSetPos`, `imageName`; reply `timeout`, `benable`, `bexistPos` | VERIFIED-SDK + REOLINK_AIO |
| `GetZoomFocus` / `StartZoomFocus` — `PtzPanel.qml:62,203,211` | zoom/focus pos + range | 294 `GET_ZOOM_FOCUS_INFO` (`0x8bd`) / 295 `START_ZOOM_FOCUS` (`BCSDK_RemoteStartZoomFocus 0x8be`) | 294 reply `zoom`/`focus` each `curPos`, `minPos`, `maxPos`; 295 `<StartZoomFocus>` `channelId`, `command`, `movePos` | VERIFIED-SDK + REOLINK_AIO; `command` values (reolink_aio passes HTTP's ZoomPos/FocusPos): UNRESOLVED |
| `GetAutoFocus` / `SetAutoFocus` — `PtzPanel.qml:63,223` | disable | 224 / 225 (`0x881` / `0x882`) | `<AutoFocus>` `channelId`, `disable` | VERIFIED-SDK + REOLINK_AIO |
| `GetAiCfg`/`SetAiCfg` auto-track — `PtzPanel.qml:67,456` | aiTrack, bSmartTrack | 299 `GET_AI_CFG` / 300 `SET_AI_CFG` (`0x8c2` / `0x8c3`) | `<AiCfg>` `smartTrack`, `smartTrackMode`, `smartTrackObjectDisappearDelay`, `smartTrackObjectStopDelay`, `cryDetectLevel` | VERIFIED-SDK + REOLINK_AIO |
| PtzCheck (cap only) | — | cmd 341 (name `START_PT_SELF_TEST`, offset-3 range) | no body | VERIFIED-SDK + REOLINK_AIO |
| current position (not used) | — | 433 `GET_PTZ_CUR_POS` | `pPos`, `tPos` | VERIFIED-SDK + REOLINK_AIO |

## 6. Snapshot

| HTTP item → app use | Baichuan | Body / reply | Conf. |
|---|---|---|---|
| `Snap` — `ReolinkHttpClient.cpp:194`; `DeviceManager.cpp:984` (save), `:1022` (event thumbnail) | cmd 109 `SNAP`, header ch N+1 (`BCSDK_RemoteSnap 0x86f`) | `<Snap>` `channelId`, `logicChannel` 0, `time` 0, `fullFrame` 0, `streamType` main/sub; reply XML `pictureSize` (+ `encryptLen`), JPEG in payload chunks under the same message id until an empty one | VERIFIED-SDK (name; strings `Snap`, `fullFrame`) + REOLINK_AIO + device-verified (stash) |

## 7. Talk

| App use | Baichuan | Conf. |
|---|---|---|
| `TalkSession` — `DeviceManager.cpp:908`, `BaichuanControl.cpp:436-517` | 10 `GET_TALK_ABILITY` (`<TalkAbility>`), 201 `TALK_OPEN` (`<TalkConfig>`), 202 `TALK_FDX_STREAM` (binaryData extension), 11 `TALK_CLOSE`; SDK `BCSDK_AudioTalkOpen/SendStream/Close` | VERIFIED-SDK (names, `BCSDK_RemoteGetTalkAbility 0x86d → 10`); already over Baichuan |

## 8. Image, OSD, mask, encoding

| HTTP item → app use | Fields | Baichuan | Body | Conf. |
|---|---|---|---|---|
| `GetIsp`/`SetIsp` — `DeviceSettingsPage.qml:640-703` | exposure, antiFlicker, backLight (+BLC/DRC strength), whiteBalance, redGain, blueGain, nr3d | 26 `GETVEDIOPARAM` / 25 `SETVEDIOPARAM`, get-modify-set | `VideoInput` (`bright`, `contrast`, `saturation`, `hue`, `sharpen`); `InputAdvanceCfg` (`DayNight/mode`, `Exposure/mode`, `PowerLineFrequency/mode`+`enable`); `hdrSwitch`, `binning_mode` | VERIFIED-SDK (names; roots `VideoInput`, `InputAdvanceCfg`) + REOLINK_AIO. backLight, whiteBalance, redGain, blueGain, nr3d: strings exist in the SDK but were not tied to cmd 26 → UNRESOLVED (a cmd 26 dump through `BaichuanControl::getConfigFlat` settles it) |
| brightness/contrast/saturation/sharpen/mirror/flip/day-night — already BC (`DeviceSettingsPage.qml:579-618`) | — | 26/25; threshold 296 `GET_DAY_NIGHT_THRESHOLD` / 297 (`cur`) | — | VERIFIED-SDK + REOLINK_AIO |
| `GetOsd`/`SetOsd` — `DeviceSettingsPage.qml:1002-1025` | osdChannel enable/name, osdTime enable | 44 `GET_OSD_CFG` / 45 `SET_OSD_CFG` | roots `OsdChannelName` (`name`), `OsdDatetime`; enable/position tag names UNRESOLVED | ids VERIFIED-SDK; name REOLINK_AIO |
| `GetMask`/`SetMask` — `DeviceSettingsPage.qml:1042`, `PrivacyMaskDialog.qml:52` | enable, area[] | 52 `GET_SHELTER_CFG` / 53 `SET_SHELTER_CFG` | `Shelter/enable`, `Shelter/shelterList/Shelter` (one per area); area coordinate tags UNRESOLVED | VERIFIED-SDK (names) + REOLINK_AIO (enable) |
| `GetEnc`/`SetEnc` | see §1 | 56 / 57 | — | — |

## 9. Lights and siren

| HTTP item → app use | Fields | Baichuan | Body | Conf. |
|---|---|---|---|---|
| `GetIrLights`/`SetIrLights`, `GetPowerLed`/`SetPowerLed` — `DeviceSettingsPage.qml:710,717` | state | 208 `GET_LED_STATE` / 209 `SET_LED_STATE` (`BCSDK_RemoteGetLedState 0x867`) | `<LedState>` `state` (IR mode, lower-case; HTTP Off is `close`), `lightState` (status LED open/close), `IRLedBrightness`, `doorbellLightState` | VERIFIED-SDK + REOLINK_AIO |
| `GetWhiteLed`/`SetWhiteLed` — `DeviceManager.cpp:1666` (toggle), `DeviceSettingsPage.qml:846` | state, mode, bright, LightingSchedule | on/off: 288 `FLOODLIGHT_MANUAL` (`0x8b7`); config: 289 `GET_FLOODLIGHT_TASK` / 290 `SET_FLOODLIGHT_TASK` (`0x8b8` / `0x8b9`) | 288 `<FloodlightManual>` `channelId`, `status`, `duration`; 289 `FloodlightTask` `brightness_cur`, `alarmMode`, `enable`, `newColorTemperature`, `alarmLightModeSL`…; mode numbers differ from HTTP (reolink_aio maps them) | VERIFIED-SDK + REOLINK_AIO; schedule (`FloodlightScheduleListV2` in SDK): UNRESOLVED |
| `GetAudioAlarmV20`/`SetAudioAlarmV20` — `DeviceSettingsPage.qml:928` | enable | 232 `GET_AUDIOTASK` / 231 `SET_AUDIOTASK` (`0x887` / `0x888`) | `AudioTask` `enable` | VERIFIED-SDK + REOLINK_AIO |
| `GetAudioCfg`/`SetAudioCfg` volume — `DeviceSettingsPage.qml:946` | volume | 264 `GET_RINGTONE_CFG` / 265 (`0x8ab`) | `audioCfg` `volume`, `talkAndReplyVolume`, `visitorVolume`, `visitorLoudspeaker`, `preAlarm` | VERIFIED-SDK + REOLINK_AIO |
| `AudioAlarmPlay` — `LivePane.qml:554` | alarm_mode times, times | 263 `MANUAL_RING_DOWN` (`BCSDK_RemoteManualRingDown 0x8aa`) | `<audioPlayInfo>` `channelId`, `playMode` (0 times, 2 manual, 4 pre-alarm), `playDuration`, `playTimes`, `onOff` | VERIFIED-SDK (name, root) + REOLINK_AIO |
| `QuickReplyPlay` — `DoorbellOverlay.qml:86` | channel, id | 349 `PLAY_ALARM_ALERTOR` (offset-3 range) | `<audioFileInfo>` `channelId`, `id`, `timeout`; list: 347 | VERIFIED-SDK (name) + REOLINK_AIO |

## 10. Detection, alerts, AI

| HTTP item → app use | Fields | Baichuan | Conf. |
|---|---|---|---|
| motion zones/sensitivity — already BC 46/47 (`DetectionZoneDialog.qml:142`, `DeviceSettingsPage.qml:734`) | — | 46 `GET_MD_CFG` / 47 `SET_MD_CFG` (`sensInfoNew`, `sensitivityDefault`…) | VERIFIED-SDK + REOLINK_AIO |
| AI sensitivity/stay — already BC 342/343 (`DeviceSettingsPage.qml:121,765`) | — | 342 `GET_AI_DETECT_CFG` / 343 `SET_AI_DETECT_CFG` (offset-3 range; `BCSDK_RemoteGetAIDetectCfg 0x8ea`); `<AiDetectCfg>` `chn`, `type` → `sensitivity`, `stayTime` | VERIFIED-SDK + REOLINK_AIO |
| `GetAiCfg`/`SetAiCfg` `AiDetectType` people/vehicle/dog_cat on/off — `DeviceSettingsPage.qml:757-785` | AiDetectType.* | no Baichuan field found: 299 `AiCfg` (tracking/cry) and 342 `AiDetectCfg` (sensitivity/stay) as read by reolink_aio carry no per-type enable | UNRESOLVED (dump 342 per type and 299 to look for an enable) |
| push/email/FTP enable — already BC (`DeviceManager.cpp:1711`) | enable | push 219/218 `GET/SET_PUSHTASK`; email 217/216 `…_EMAILTASK`; FTP 70/71 `…_FTPTASK`; separate master switches 314/315 push, 310/311 email, 312/313 FTP `…_ENABLE` | VERIFIED-SDK + REOLINK_AIO (task ids) |
| `GetPush(V20)`/`GetEmail(V20)`/`GetFtp(V20)` (fallback list `DeviceManager.cpp:1488`) | schedule | same ids; server settings: email 42/43 `GET/SET_EMAILCFG`, FTP 68/69 `GET/SET_FTPCFG`, push 363 `GET_PUSH_CFG` | ids VERIFIED-SDK; bodies UNRESOLVED beyond `enable` |

## 11. Recording schedule

| App use | Baichuan | Conf. |
|---|---|---|
| already BC: 54/55 (`RecordCfg`: `recordDelayTime`, `packageTime`, `preRecordTime`, `cycle`), 81/82 (`Record`: `enable`, schedule `valueTable` per `type`) — `DeviceManager.cpp:1838,1881`, `DeviceSettingsPage.qml:1167-1192` | 54 `GET_RECORD`, 55 `SET_RECORD`, 81 `GET_RECSCHEDULE`, 82 `SET_RECSCHEDULE`; master switch 308/309 `…_RECORD_ENABLE` | VERIFIED-SDK + REOLINK_AIO |

## 12. Network, ports, NTP, time

| HTTP item → app use | Fields | Baichuan | Body | Conf. |
|---|---|---|---|---|
| `GetNetPort` — `DeviceSettingsPage.qml:54` | https/rtsp/onvif enable + port | 37 `GET_NETPORT_CFG` / 36 `SET_NETPORT_CFG` (host) | one element per protocol: `ServerPort` (Baichuan), `HttpPort`, `HttpsPort`, `RtspPort`, `RtmpPort`, `OnvifPort`, each with a port and an enable child (reolink_aio strips the protocol prefix from child tags); set sends `<XxxPort><enable>` | VERIFIED-SDK (names) + REOLINK_AIO |
| `GetLocalLink` — `DeviceSettingsPage.qml:1208-1215` | activeLink, mac, type DHCP/static, static ip/mask/gateway | 76 `GET_LOCAL_LINK` (`ip`, `mac`) + 93 `GET_LINKTYPE` (`type` → activeLink) | DHCP/static and mask/gateway tags: UNRESOLVED (SDK strings `Dhcp`, `mask`, `gateway`, `dns` exist, not tied) | ids VERIFIED-SDK; ip/mac/type REOLINK_AIO |
| `GetTime` — `DeviceSettingsPage.qml:1351` | Time.*, timeZone | 104 `GET_SYSGENERAL_CFG` / 105 `SET_SYSGENERAL_CFG`; DST 106/107 (`Dst`); `BCSDK_RemoteSyncUtcTime 0x8b5` and 287 `SYNC_UTC_TIME` (tied by name only) | `<SystemGeneral>` `timeZone`, `osdFormat`, `timeFormat`, `year`…`second` | VERIFIED-SDK + REOLINK_AIO |
| `GetNtp`/`SetNtp` — `DeviceSettingsPage.qml:1369,1385` | enable, server, port, interval | 38 `GET_NTPCFG` / 39 `SET_NTPCFG`; root `Ntp` | child tags UNRESOLVED (the root is reached through a pointer table, no builder found by string) | ids VERIFIED-SDK |

## 13. Storage

| HTTP item → app use | Fields | Baichuan | Body | Conf. |
|---|---|---|---|---|
| `GetHddInfo` — `DeviceSettingsPage.qml:1238` | storageType, capacity, size(free), mount, format, id | 102 `GET_HDD_CFG` (`BCSDK_RemoteGetHDDCfg 0x80c`) | `HddInfoList/HddInfo` `number`, `capacity`, `capacityM`, `remainSize`, `remainSizeM`, `format`, `mount`, `storageType` (GB×1000 + MB, per reolink_aio) | VERIFIED-SDK + REOLINK_AIO |
| `Format` — `DeviceSettingsPage.qml:361` | HddInfo.id[] | 103 `INIT_HDD` (`BCSDK_RemoteInitHdd 0x80b`); 223 `FMT_HDD_NOREBOOT` also named | `<HddInitList>` with one `<HddInit>` `initId` per disk, optional `formatExpandCfg` | VERIFIED-SDK (builder, name); which of 103/223 the client picks: UNRESOLVED |

## 14. Users

| HTTP item → app use | Fields | Baichuan | Body | Conf. |
|---|---|---|---|---|
| `GetUser` — `DeviceSettingsPage.qml:1293` | userName, level | 58 `GET_USERCFG`, extension `<userName>` of the logged-in user (`BCSDK_RemoteGetUserCfg 0x814`) | reply `UserList/User` `userName`, `userLevel` (1 = admin) | VERIFIED-SDK (builder, inline 58) + REOLINK_AIO |
| `AddUser`/`DelUser`/`ModifyUser` — `UserEditDialog.qml:82,88`, `DeviceSettingsPage.qml:1318` | userName, password, level; old/new password | 59 `SET_USERCFG` (`BCSDK_RemoteSetUserCfg 0x815`) | `<UserList><User>` `userName`, `nickname`, `password`, `userLevel`, `userSetState` = add / delete / modify / none, optional `validHours`, `channelAbility`; plus `<Extension><userName>`. The SDK's password-change builder sends `userName`, `password`, `nickname`, `userSetState` modify — **no old password** | VERIFIED-SDK (two builders, inline 59) |
| online users (not used) | — | 120 `GET_USER_ONLINE` | VERIFIED-SDK |

## 15. System, firmware, reboot

| HTTP item → app use | Fields | Baichuan | Body | Conf. |
|---|---|---|---|---|
| `Reboot` — `DeviceManager.cpp:1705` | — | 23 `REBOOT` (`BCSDK_RemoteReboot 0x835`); NVR channel reboot with channel header | no body | VERIFIED-SDK + REOLINK_AIO |
| `CheckFirmware` — `DeviceSettingsPage.qml:384` | newFirmware | 197 `GET_ONLINE_NEW_FW` (`0x875`) | reply `OnlineNewFirmwareInfo`, `hasNewFirmware` | VERIFIED-SDK + REOLINK_AIO |
| `UpgradeOnline` — `:396` | — | 191 `ONLINE_UPDATE` (`BCSDK_RemoteOnlineUpate 0x84b`) | `<OnlineUpdate>` `needUpdate` 1 | VERIFIED-SDK + REOLINK_AIO |
| `UpgradeStatus` — `:405` | Status.Persent | 227 `GET_ONLINE_UPGRADE_STATE` (`0x880`) | `upgradeState` `state` (SDK strings: downloading, updating, stopped, finish, updateFailed, imgerror, waitDownload), `downloadSize`, `packetSize` (percent = download/packet) | VERIFIED-SDK + REOLINK_AIO |
| `GetAutoUpgrade`/`SetAutoUpgrade` — `DeviceSettingsPage.qml:1089` | enable | 195 `GET_AUTO_UPDATE_STATE` (`0x873`) / 196 (`0x874`) | `<AutoUpdate>` `enable` | VERIFIED-SDK (builder, names) |
| firmware file upload (not used) | — | 67 `UPDATE_DEV`: `<ConfigFileInfo>` `fileName`, `fileSize`, `updateParameter`, `channelBits`, then chunks with binaryData extension, same message id | VERIFIED-SDK + REOLINK_AIO |

## Unresolved items

1. Per-channel online state values in cmd 145 `loginState` (only `standby` is
   known; SDK strings suggest connect/disconnect/pwdError/connectFailed) and
   the per-channel UID tag. Tried: reolink_aio parser, SDK strings; no
   builder (it is a device push).
2. `GET_ALARMSTATE` (131) body/reply, for polling instead of push. Tried:
   name table only.
3. Which command carries the SDK's `<ReplayByTimeV2>` body (381 named
   `REPLAY_BY_TIME`); builder has no inline id.
4. Downloader output handling (how 8/143 media is turned into a file) and how
   clips are chosen for a range (no `NvrDownload` equivalent).
5. ISP fields backLight/BLC/DRC strength, whiteBalance, redGain/blueGain,
   nr3d inside cmd 26.
6. OSD enable/position tags (44/45), mask area coordinates (52/53).
7. Per-type AI detection enable (`AiDetectType` people/vehicle/dog_cat).
8. Patrol list layout (64) and patrol set body (20); `StartZoomFocus`
   `command` values.
9. NTP tags (38/39); LocalLink DHCP/static, mask, gateway tags (76).
10. Email/FTP/push server settings bodies (42/43, 68/69, 363) beyond `enable`.
11. Floodlight schedule (`LightingSchedule`) in 289/290.
12. Ability tags for `supportBalanced`, `disableAutoFocus`, patrol, and which
    151 token encodes user-management/reboot rights.
13. Choice between 103 `INIT_HDD` and 223 `FMT_HDD_NOREBOOT`.

Most of 5, 6, 7, 9, 11 can be read from a single Get dump on a device
(the app's `BaichuanControl::getConfigFlat` already flattens any reply).

## NVR notes

- **Channel byte**: channel-scoped commands put channel + 1 in the header
  channel byte and add the `<Extension><channelId>` block; host commands use
  250 and no extension; the event subscription uses 251 (reolink_aio
  `send()`, `BaichuanControl::transact`). `BaichuanClient` (live 3 and 143)
  writes the 0-based channel and the stream type into those bytes instead;
  p2p.md found cmd 5 needs N+1 + extension. Check cmd 3 on an NVR channel > 0.
- **Host-level** (ch 250): 1, 2, 23 (host), 31/33 (251), 37/36, 38/39, 58/59,
  76 (host), 80, 93, 102, 103, 104/105, 114, 145 (push), 146, 151, 191, 195/196,
  197, 199, 227.
- **Per-channel**: 3/4, 5/7/123, 8/9, 14/15/16, 142, 143/144, 272–274, 109,
  298, 18/19/20, 64, 190, 224/225, 294/295, 331/332, 25/26, 296/297, 44/45,
  46/47, 52/53, 54/55, 56/57, 81/82, 70/71, 217/216, 219/218, 208/209,
  231/232, 263, 264/265, 288–290, 299/300, 342/343, 253, 318, 10/201/202/11.
- Per-channel abilities come from cmd 199 `<item><chnID>`; there is no
  separate per-channel GetAbility. Channel list and online state come from
  login `DeviceInfo/channelNum` and the cmd 145 push, not from a request.
- Recording search and playback on NVRs also need the camera `uid`
  (`DayRecords`, `FileInfo`); reolink_aio refuses to search without it.
- reolink_aio: a cmd 318 failure for a channel marks that camera offline.
