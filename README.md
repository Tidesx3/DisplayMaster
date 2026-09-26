# DisplayMaster

Use an Android tablet or phone as a second screen for a Windows PC — extend or mirror,
over USB or Wi-Fi, with full pen (pressure, tilt, hover, side button) and multi-touch input.
An open-source take on SuperDisplay, tuned for low latency.

```
 Windows PC                                              Android device
 ┌───────────────────────────────────────────┐          ┌──────────────────────────────┐
 │ DisplayMaster (WinUI 3 tray + settings)    │          │ DisplayMaster app (Compose)  │
 │        │ named pipe (JSON)                 │          │   pen/touch/keys ─┐          │
 │ DisplayMasterHost.exe (C++20 engine)       │  USB     │                   ▼          │
 │  virtual monitor (VDD) → DXGI capture      │ (adb) or │  C++ client ─► MediaCodec    │
 │  → cursor + scale/convert (D3D11) → NVENC/ │◄──Wi-Fi─►│  (low-latency) ─► Surface    │
 │    AMF encode ─► framed protocol ─────────►│          │                              │
 │  ◄─ Synthetic Pointer / SendInput injection│          │                              │
 └───────────────────────────────────────────┘          └──────────────────────────────┘
```

## Repository

| Path | What |
|---|---|
| `protocol/` | Shared C++ wire protocol, input math (pen tilt, mapping, letterboxing), keymap. Built for MSVC *and* the Android NDK. |
| `windows/host/` | C++ engine: virtual display control, capture, encoders, transports, input injection, control API. |
| `windows/ui/DisplayMaster.App/` | WinUI 3 app: tray icon, device cards with live stats, connection settings, Wi-Fi device approval. |
| `android/` | Android app: Kotlin/Compose UI + NDK core (decoder, network client). |
| `tools/` | Icon generator, window screenshot helper. |
| `third_party/` | Vendored MIT headers: NVIDIA `nvEncodeAPI.h`, AMD AMF. |

## Building

### Windows host (C++)
Requires Visual Studio 2022 with the C++ workload (bundled CMake is fine).
```
cmake --preset windows-x64
cmake --build --preset release
ctest --preset debug            # or run build/windows-x64/**/dm_*_tests.exe
```
Output: `build/windows-x64/windows/host/Release/DisplayMasterHost.exe`.

### Windows app (C# / WinUI 3)
Requires the .NET 10 SDK.
```
cd windows/ui/DisplayMaster.App
dotnet build -c Release -p:Platform=x64
```
The app is self-contained. In a dev checkout it finds and starts the engine from the CMake
output folder; installed builds keep `DisplayMasterHost.exe` next to `DisplayMaster.exe`.

### Android app
Requires Android Studio's JDK (or any JDK 17+) and the Android SDK; Gradle installs the NDK.
```
cd android
gradlew assembleDebug          # app/build/outputs/apk/debug/app-debug.apk
```
minSdk 24 (Android 7). Native libraries are 16 KB page aligned.

## Installing (Windows)

Download or build **`DisplayMaster-Setup-<version>.exe`** and run it. One installer contains
everything: the app, the engine, `adb`, the Android app (installed automatically on plugged-in
devices) and the signed [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
for Extend mode. Options: *virtual display driver*, *allow private Wi-Fi networks*. Starting at sign-in
is a switch in the app (Settings, off by default).
Windows may ask once to trust the driver publisher (SignPath Foundation).

* Unattended, e.g. on other PCs: `DisplayMaster-Setup-<version>.exe /VERYSILENT /TASKS="vdd,firewall"`
* Uninstall from *Settings → Apps* (or `unins000.exe /VERYSILENT`). It removes the engine task,
  firewall rule and sign-in autostart; the driver is removed only if the installer added it (you're asked).
* Requires Windows 10 1809+ x64.

What it sets up: the engine starts **with the app** and runs elevated and windowless through an
on-demand scheduled task (`\DisplayMaster\Engine`), so pen/touch reach admin apps and there's no
UAC prompt later. Nothing starts at sign-in unless you turn on *Start when I sign in* (Settings). Closing or
minimizing the window keeps DisplayMaster in the tray; *Quit* in the tray menu stops the engine
too. The firewall rule allows the engine on *Private* networks only.

## Using it

1. Open **DisplayMaster** (Start menu or tray).
2. **USB:** enable *USB debugging* on the device (Developer options) and plug it in. The PC installs
   and opens the app on the device, which connects by itself.
3. **Wi-Fi:** turn on *Wi-Fi connections* in the Connection page. PCs show up automatically in
   the app under *On this network* (mDNS), or type the address shown on the PC. The first time,
   the PC asks you to **allow** the device; allowed devices are remembered (and can be forgotten).
   No app on the phone yet and no cable? Scan the **QR code** on the Connection page with the
   phone's camera: it downloads the Android app straight from the PC (no internet needed).

**Updates:** the app checks GitHub releases twice a day and offers *Update now*, which downloads
the new installer, verifies its SHA-256 against the digest GitHub publishes, and installs it
silently (one admin prompt). Devices plugged in over USB then get the matching Android app.

On the device: swipe in from the right edge (or press Back) for the quick panel — display mode,
finger input mode (Touch / Mouse / Trackpad), keyboard, performance overlay, disconnect.

### Engine command line (diagnostics)
The engine has no console window of its own; from PowerShell, pipe into `Out-Host` so the prompt
waits for the output.
```
DisplayMasterHost.exe --list-monitors | Out-Host   # all monitors incl. virtual ones
DisplayMasterHost.exe --probe-encoders       # which GPU encodes which codec, with timings
DisplayMasterHost.exe --selftest 5 --out x.hevc   # capture+encode the main screen, no device needed
DisplayMasterHost.exe -v --no-input          # verbose; log device input instead of injecting it
```
Other flags: `--mode extend|mirror`, `--codec h264|hevc|av1`, `--encoder nvenc|amf`,
`--bitrate <kbps>`, `--fps <n>`, `--scale <f>`, `--port <n>`, `--wifi`, `--no-adb`, `--no-launch`, `--log <file>`.

## Feature status (vs. SuperDisplay)

| Feature | Status |
|---|---|
| Extend (virtual monitor per device) | ✅ verified, incl. two devices at once |
| Mirror (letterboxed, no GPU clone support needed) | ✅ verified end-to-end |
| USB via ADB: auto `adb reverse`, auto-install + auto-launch of the app | ✅ verified with the packaged build |
| Wi-Fi (TCP) + mDNS discovery + device approval (remembered, forgettable) | ✅ verified |
| Hardware encode: NVENC / AMF (H.264, HEVC, AV1*) | ✅ verified (*AV1 on RTX 40 / RDNA3 like the G14) |
| Mouse cursor composited into the stream | ✅ verified (GPU, incl. inverting cursors) |
| 120 Hz, start-to-start frame pacing | ✅ |
| Several devices at once (one shared capture per monitor) | ✅ verified (5 mirroring, 2 extending) |
| Pen: pressure, tilt, hover, barrel, eraser (Windows Ink) | ✅ injection verified (synthetic pen reaches apps, also with monitors left of / above the primary) |
| Pressure curve (dead zone, full-pressure point, feel) with live preview | ✅ |
| Multi-touch, mouse-emulation and trackpad modes, palm rejection | ✅ touch injection verified on a multi-monitor desktop |
| Hardware keyboard (scancodes: correct on AZERTY/QWERTZ) + soft keyboard | ✅ |
| Rotation / Fold posture → virtual monitor resize | ✅ implemented |
| Installer (setup.exe incl. driver), elevated windowless engine, optional autostart, firewall, clean uninstall | ✅ verified install / upgrade / uninstall |
| Zero-setup USB (AOA, no USB debugging) | ⏳ planned (M5) |
| Encrypted Wi-Fi with pairing codes (Noise XX, verified against the official test vectors) | ✅ verified (emulator + simulated devices) |
| Wi-Fi video over UDP with Reed-Solomon loss repair (adaptive parity), automatic TCP fallback | ✅ verified with simulated loss up to 40 % (test client), fallback on the emulator |
| Lost frames repaired by recovery frames (NVENC reference invalidation, HEVC/AV1) instead of keyframes | ✅ verified with NVENC at 15 % simulated loss; keyframe fallback on AMF / H.264 |
| Auto-reconnect after drops / engine restarts; optional auto-connect to known PCs | ✅ verified on the emulator (engine restart, kick from the PC) |
| Touch mode gestures: 2-finger scroll / pinch-zoom, 3-finger swipes (Task View, desktop, switch app) | ✅ unit-tested |
| Screen position per device (left / right / above / below) | ✅ placement unit-tested, API verified |
| Tray menu with devices, connect/disconnect notifications, rotation lock, German translation (both apps) | ✅ |
| Shortcut bar for drawing apps (undo/redo, Ctrl/Shift/Alt/Space hold or latch, brush size, zoom, Esc) | ✅ keys verified in the engine's input log |
| Settings page: bitrate, frame-rate cap, resolution, video format, start at sign-in | ✅ |
| Wintab driver, pen button remapping | ⏳ planned (M7) |

## Testing
```
build\windows-x64\protocol\tests\Debug\dm_protocol_tests.exe    # wire format, pen math, keymap
build\windows-x64\windows\host\Debug\dm_host_tests.exe          # VDD config, cursor rendering (WARP), JSON, approvals
powershell -File tools\integration_test.ps1                        # engine + simulated devices, incl. 3 at once
```
`dm_testclient` (built with the engine) connects any number of simulated devices to a running
engine and checks handshake, keyframes, bitstream format and keyframe-on-request.

### Packaging
```
powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Version 0.1.0
# -> dist\DisplayMaster-Setup-0.1.0.exe   (needs Inno Setup 7: winget install JrSoftware.InnoSetup)
powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Version 0.2.0 -Publish
# also creates GitHub release v0.2.0 with the installer -> installed apps offer the update
```
`-Version` sets the engine, the PC app and the Android app (`versionName`, `versionCode`) alike.

## Hardware notes
* **Zephyrus G14 (780M iGPU + RTX 4070):** the engine creates its D3D device on the adapter that
  owns the captured monitor and prefers that GPU's encoder, so iGPU mode stays on AMF and never
  wakes the dGPU. `--probe-encoders` shows what each GPU can do.
* **Galaxy Tab S7+:** 2800×1752 @ 120 Hz, S Pen with pressure/tilt/hover/button.
* **Galaxy Z Fold 7:** no S Pen support (no digitizer); use Trackpad or Mouse mode. Folding,
  unfolding and rotating resize the virtual monitor automatically.

## Security
USB connections are trusted (they require a USB-debugging-authorized cable). Wi-Fi is off by
default. Over Wi-Fi everything is **encrypted and authenticated**: a Noise XX handshake
(X25519, ChaCha20-Poly1305, BLAKE2b; [Monocypher](https://monocypher.org)) followed by sealed
frames, so nobody on the network can watch the screen or inject input. The first time a device
connects, the PC and the device show the same **6-digit pairing code**; you compare them, click
*Allow* on the PC and *Codes match* on the device (a man in the middle would produce different
codes). After that both sides recognize each other by their keys and connect silently. The PC's
key is stored DPAPI-protected in `%LOCALAPPDATA%\DisplayMaster\identity.key`; *Forget* on the
Connection page un-pairs a device. Details: `protocol/include/dm/noise.h`.

## License
Project code: MIT. Third-party headers keep their licenses (`third_party/*`).
