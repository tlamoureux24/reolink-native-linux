# Everything over Baichuan: migration plan

Branch `feat/p2p-v2` (from `e2ebb72`, the last commit before P2P v1).

## Why

The official Reolink client has one mode. Every feature — live, playback,
events, PTZ, settings, firmware — goes through its SDK (`libBCSDKWrapper.dll`)
over Baichuan, for cameras and NVRs alike; only the transport under it changes
(TCP 9000 for a device added by address, UDP P2P for one added by UID). It
never calls `api.cgi` (the one occurrence is the device web UI's firmware
upload, skipped when `IS_CLIENT`), never opens RTSP, never plays HTTP-FLV. An
NVR is one SDK device type (`E_BC_DEVICE_TYPE_NVR`, `BCSDK_GetIsNVR`) on the
same session.

This client was built on the HTTP-CGI API + RTSP + FLV (the interfaces Reolink
offers to third-party software), with Baichuan added piece by piece. Remote
access then needed a second mode, and mixing the two is what went wrong in
P2P v1. Moving everything to Baichuan removes the second mode: P2P becomes one
more transport under the same session.

The command-by-command map is [baichuan-map.md](baichuan-map.md).

## Model (as in the SDK)

- **One session per device** (`BCSDK_AddDevice` / `BCSDK_Open`): connect,
  log in (cmd 1; the reply carries `DeviceInfo`: type, NVR or not,
  `channelNum`), then everything runs inside it. Requests and streams are told
  apart by their message id; live and playback streams each take an id of
  their own (cmd 3 / cmd 4, cmd 5 / cmd 7).
- **Heartbeat** cmd 93 after 5 s of silence; reconnect after 20 s.
- **Device pushes** arrive on the same session: channel states (cmd 145, NVR),
  alarms after a subscription (cmd 31 on channel byte 251 → cmd 33), battery
  (cmd 252).
- **Transport** under the session: TCP 9000 by address; UDP P2P by UID (local
  search, then Reolink's servers, direct or relay) — last step.

## Steps

Each step moves one whole area to Baichuan and deletes its HTTP/RTSP/FLV code
— no fallback between the two, no area served both ways. Each is tested on the
user's six cameras (isolated copy of their database, read-only on the cameras
unless they agree otherwise) and validated by the user before the next one.

1. **Session and identity.** A session per device replaces
   `ReolinkHttpClient`: login, version (cmd 80), abilities (cmd 199), rights
   (cmd 151 / user level), encoders (cmd 56: codec, sizes), UID (cmd 114),
   battery (cmd 253/252), NVR channel states (cmd 145 push) and names (cmd 44).
   Add-device test and sidebar status come from it. Reuses the session and
   TCP/UDP link written for P2P v1 (our own code, field-tested over 4G).
2. **Live.** Grid (sub) and single view (main) as streams inside the session
   (cmd 3 / 4), sound included. RTSP is removed. Check the channel byte for NVR
   channels > 0 (cmd 5 needs channel + 1; cmd 3 is sent 0-based today).
3. **Playback.** Calendar (cmd 142), file search (cmd 14/15), play by file
   (cmd 5), seek (cmd 123), stop (cmd 7), as the official replayer.
   HTTP-FLV is removed, and so is the use of cmd 143 for playback: 143 is
   `DOWNLOAD_CUT`, the by-time clip download.
4. **Events.** Alarm subscription (cmd 31 → cmd 33 pushes) replaces
   `GetMdState`/`GetAiState` polling; thumbnails by snapshot (cmd 109).
5. **PTZ, snapshots, clips.** PTZ (cmd 18 control, 19 presets, 20/64
   patrols), snapshot (cmd 109), clip export by time (cmd 143) or by file
   (cmd 8). Talk is already Baichuan.
6. **Settings,** page by page (image, OSD, encoding, recording, detection,
   lights/siren, network, storage, users, time, system/firmware). Fields the
   map leaves unresolved are settled by reading the setting from a camera
   (Get only); writing is tested only with the user's go-ahead, page by page.
7. **Remote access.** The UDP P2P transport under the same session, for
   devices added or reached by UID. Nothing above changes.
8. **Clean-up.** Remove `ReolinkHttpClient`, the HTTP parts of `ReolinkApi`,
   RTSP/FLV helpers, and the HTTP settings (port, HTTPS) from the add-device
   flow.

## Progress

**Step 1 — done, validated by the user (2026-11-03).** `BcSession` (one per
device, heartbeat cmd 93 after 5 s, lost after 20 s of silence) and
`bcdev::readDevice` (login `DeviceInfo`, cmd 80, 114, 199, 151, 56, 44, 253,
145 pushes) replace the HTTP sign-in, `GetDevInfo`, `GetAbility`, `GetEnc`,
`GetBatteryInfo` and `GetChannelstatus` (validation, add-device test, online
state). Compared field by field with the HTTP answers on the six cameras
(`bc-probe --compare`): identical except
- *floodlight*: `<Support>` `ledCtrl` bits 1+2 report the spotlight that the
  RLC-811A, RLC-823A and Duo 2 have; the HTTP `GetAbility` reading missed it;
- *auto tracking*: `aitype` bit 7 (as reolink_aio);
- *Balanced stream*: cmd 56 lists a `thirdStream` on all six (unused by the UI).
The main-stream codec is not in any config reply — it is in each frame's
header — so the live stream is probed instead of being told the codec.
Detection polling and the other features still use HTTP until their steps.

**Step 2 — done, validated by the user (2026-11-03).** Grid (sub) and single
view (main) stream inside the device's session: cmd 3 with the SDK's
`Preview` (handle `(streamType << 8) | channel`, policy `newestIFrame`), one
stream per (channel, stream type) shared by every pane or window watching it,
closed with cmd 4 by its last viewer; the header channel byte stays 0-based
for cmd 3 (as the client sent it to NVRs before). A pane streams while its
device is online and starts again when it comes back. RTSP is gone for
Reolink devices (only plain stream devices keep a URL). Rows of a revalidated
device are now updated in place: they used to be removed and re-inserted,
which shifted every pane to another camera for a moment and briefly opened
main streams at start-up.

**Step 3 — done, validated by the user (2026-11-03).** Calendar days (cmd 142)
and a day's files per stream (cmd 14 → `handle`, cmd 15 pages, cmd 16 close)
come from the device's session; playback goes file by file in it, as the
official replayer: cmd 5 with the file's `Id`, cmd 123 `ReplaySeek` to seek
inside it, cmd 7 to stop, then the next file when it follows on without a gap.
A camera sends a file as fast as it can — about 10× real time on the LAN, up to
~53 Mbit/s for a main file — and the SDK's `<playSpeed>` (cmd 5 FileInfo,
seen in `BaichuanReplayer`) did not slow an RLC-811A down, nor is cmd 185
`PAUSE_REPLAY` sent by the SDK. So the client queues what arrives and plays it
at the chosen speed (0.5×–8×, pause, frame step), and bounds the queue with
the official commands only: past 20 s of video the file is stopped (cmd 7);
below 5 s it is asked again from where the queue ends (cmd 5 + 123) and the
frames already queued are skipped by timestamp. Measured: sub at 1× ≈ 0.22
Mbit/s (the 256 kbit/s stream), main at 1× ≈ 6.9 Mbit/s. HTTP-FLV, the
camera MP4 URL and the old cmd 143 playback on its own connection are gone
(143 is `DOWNLOAD_CUT`, the clip download).

**Step 4 — done, validated by the user (2026-11-03).** Detections are pushed by
each device on its session: right after validation the client subscribes
(cmd 31 on header channel byte 251, as the SDK and reolink_aio), and every
`<AlarmEventList>` (cmd 33: `status` MD = motion, `AItype` people / vehicle /
dog_cat) goes through the same rising-edge logic as the HTTP polling did. The
first push, the current state, arrives within a second. Snapshots and event
thumbnails come from cmd 109 `<Snap>` (an RLC-811A returns the full 3840×2160
JPEG even for the sub stream, in about 0.7 s; thumbnails are scaled down
here). The 10 s `GetMdState` / `GetAiState` polling and the HTTP `Snap` are
gone: in 90 s of running the client made no HTTP request at all.

## Risks

- **NVR:** the user has none at the moment, so NVR paths are written from the
  SDK and reolink_aio but not tested on hardware.
- **Unresolved fields** (see the map): a few settings sub-fields, cmd 145
  state values, download-to-file flow. Each is resolved from the SDK or a Get
  on a real device before its step, never guessed.
- **Battery cameras** stop needing the go2rtc bridge (DESIGN §4.6) once live
  is Baichuan; untested here (no battery camera).
