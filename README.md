# Reolink Native Linux Client

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Latest release](https://img.shields.io/github/v/release/tlamoureux24/reolink-native-linux)](../../releases/latest)

A fully **native Linux desktop client** for Reolink cameras and NVRs — built to work like the official Reolink Client (Windows/macOS/Android): the 1/4/6/9/16 live grid, synced multi-camera playback on one timeline, double-click maximize, fullscreen, PTZ, and a full device-settings surface.

**No Wine, no Electron, no web wrappers, no bundled third-party NVR software.** Compiled C++/Qt6 talking to the devices the way the official apps do: everything over Reolink's native **Baichuan** protocol, one session per device, on the local network or remotely by the device's UID.

> **Status: 1.0.** Live view, playback, clip export, events, two-way talk, PTZ and device settings all go over Baichuan, at home or away. Tested on RLC-811A, RLC-823A, RLC-823A 16X and Duo 2 PoE cameras; NVRs speak the same protocol but have not been tested with this version yet.

This is a fork of [TodesengelX/reolink-native-linux](https://github.com/TodesengelX/reolink-native-linux), rebuilt on Baichuan alone. Grab the [latest release](../../releases/latest), and see the [changelog](CHANGELOG.md) for what's new. Found a bug? Open an [issue](../../issues).

## Install

### AppImage — download and run (no dependencies)

Grab the latest `.AppImage` from the [**Releases**](../../releases) page, then:

```sh
chmod +x reolink-native-linux-x86_64.AppImage
./reolink-native-linux-x86_64.AppImage
```

### Flatpak

Download the `.flatpak` bundle from [Releases](../../releases) and:

```sh
flatpak install --user ./reolink-native-linux.flatpak
flatpak run io.github.todesengelx.ReolinkLinux
```

*(A Flathub listing — `flatpak install flathub io.github.todesengelx.ReolinkLinux` — is planned.)*

### Arch Linux

Not on the AUR yet — build from the repo's PKGBUILD:

```sh
git clone https://github.com/tlamoureux24/reolink-native-linux.git
cd reolink-native-linux/packaging/aur && makepkg -si
```

### Build from source

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/reolink-client
```

Requires Qt 6.5+ (base / declarative / multimedia / shadertools / tools), FFmpeg dev headers, libsecret, and sqlite. Run the tests with `ctest --test-dir build`.

## Features

- **Live view** — 1/4/6/9/16 grid; drag cameras in from the sidebar or between cells to arrange them (the layout persists); hardware-accelerated H.264/H.265 decode (VAAPI/NVDEC) with software fallback; double-click a camera (or single-click it in the sidebar) to maximize; F11 fullscreen; per-pane floating toolbar (SD/HD, snapshot, record, digital zoom with edge-clamped pan, PTZ, talk, siren, floodlight, pop-out); auto-reconnect.
- **Playback** — month calendar, two-tone timeline (timer/alarm) with red ticks marking detections, recording search, in-place seek and a live playhead, pan/zoom the timeline, clip export, HD playback over Baichuan. **Synced multi-camera playback**: a 4-pane grid driven by one timeline showing every camera's coverage as per-camera lanes, with one playhead driving all panes to the same moment — drag cameras in, swap panes without interrupting their streams, double-click to maximize with an SD/HD toggle, digital zoom in every pane.
- **Honest failure handling** — unreachable devices show why and retry themselves with backoff; sign-in failures surface the device's attempts-left lockout warning and are never auto-retried; right-click a device to Reconnect or fix its credentials in place; adding a device validates it first, so a typo'd address or wrong password is an inline message instead of a stuck row. Per-camera **Rotate view** override for devices whose firmware misreports orientation.
- **Events** — notification inbox with thumbnails, AI-type filters (person/vehicle/pet/visitor/motion) and per-camera filtering, detections pushed by each device, jump-to-playback, unread badge.
- **Background monitoring** — lives in the system tray with the window closed, so alerts keep arriving; optional start-on-login. Desktop notifications fire for detections (gated on each camera's Push setting) and for cameras or the NVR going offline; clicking one opens the app and plays that event back.
- **Device tree** — NVR shown as an expandable parent with its cameras nested; right-click for Properties / Settings / Reboot / Remove.
- **Settings** — Image (brightness/contrast/saturation/sharpness, mirror/flip, day/night), Detection (motion + per-AI-type sensitivity, alerts, and a paint-on-image detection-zone editor), Recording (including a weekly 7×24 schedule grid per recording type), Encoding, Display/OSD, Network, Storage, Users, Time, System.
- **Remote access** — when a device's address can't be reached, the app finds it by its UID through Reolink's P2P service (direct, mapped or relayed), like the official apps; nothing to open on your router.
- **Light and dark themes** — chosen in the client settings, or following the desktop.
- **Extras** — battery/solar status, doorbell answer surface, multi-monitor pop-out, fisheye dewarp shader, manual MP4 recording (stream-copy), and a built-in update checker with one-click self-update for the AppImage.

## How it talks to your devices

Like the official client, the app opens one **Baichuan** session per device and does everything inside it: sign-in, live view, playback and clip export, pushed detections, two-way talk, PTZ and every setting. Only the transport under that session changes:

| Transport | Used when |
|---|---|
| TCP, port 9000 | The device's address answers (usually on your local network). |
| UDP, by UID | It doesn't: the device is reached through Reolink's P2P service, directly, through a mapped port, or relayed. |

## Documentation

- **[CHANGELOG.md](CHANGELOG.md)** — what changed in each release.
- **[docs/DESIGN.md](docs/DESIGN.md)** — the definitive design document (goals, stack, architecture, protocols, media pipeline, UI inventory, data model, packaging, roadmap, risks).
- [docs/research/](docs/research/) — the research dossiers behind the design (official-client UI, protocols, video pipeline, GUI stack, prior art, fact-checks).

## Licensing

MIT — see [LICENSE](LICENSE). All runtime dependencies are LGPL/MIT/Apache and dynamically linked. The protocol layer is written from scratch from interoperability facts: the official client's behaviour, the MIT-licensed `reolink_aio` reference and reads on real devices — **never** from AGPL-licensed code.

## Disclaimer

Independent, unofficial project — not affiliated with, endorsed by, or supported by Reolink. "Reolink" is a trademark of its respective owner. Use with your own devices at your own risk.
