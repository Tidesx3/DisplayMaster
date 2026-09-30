<p align="center">
  <img src="windows/ui/DisplayMaster.App/Assets/Logo.png" alt="DisplayMaster logo" width="128">
</p>

<h1 align="center">DisplayMaster</h1>

<p align="center">
  <a href="https://github.com/Tidesx3/DisplayMaster/releases"><b>⬇️ Download the latest version</b></a>
</p>

Turn your Android tablet or phone into a **second screen for your Windows PC**.
Extend your desktop or mirror it, over **USB or Wi-Fi**, with full **pen support**
(pressure, tilt, hover, side button) and **multi-touch**.

Free, open source and built for low latency, an alternative to SuperDisplay.

---

## What you need

- A **Windows 10 (version 1809 or newer) or Windows 11** PC, 64-bit
- An **NVIDIA or AMD graphics card** (used for fast video encoding)
- An **Android phone or tablet** with Android 7 or newer
- A **USB cable**, or both devices on the **same Wi-Fi network**

---

## Installation

1. Go to the **[Releases page](https://github.com/Tidesx3/DisplayMaster/releases)** and download
   `DisplayMaster-Setup-<version>.exe`.
2. Run the installer. It contains everything you need:
   - the PC app
   - the Android app (installed on your device automatically over USB)
   - the virtual display driver (needed to **extend** your desktop)
3. Windows may ask once whether to trust the driver publisher (**SignPath Foundation**). Click **Install**.

During setup you can choose:
- **Virtual display driver**: keep this on if you want to use the device as an extra monitor.
- **Allow private Wi-Fi networks**: keep this on if you want to connect without a cable.

---

## Connecting your device

### Option 1: USB (easiest)

1. On your Android device, turn on **USB debugging**:
   - Open *Settings → About phone* and tap **Build number** 7 times to unlock *Developer options*
     (on Samsung: *Settings → About phone → Software information*).
   - Go to *Settings → Developer options* and turn on **USB debugging**.
2. Open **DisplayMaster** on your PC (Start menu or tray icon).
3. Plug in your device. If it asks *"Allow USB debugging?"*, tap **Allow**.

That's it. The PC installs and opens the app on your device, and it connects by itself.

### Option 2: Wi-Fi

1. On the PC, open DisplayMaster, go to the **Connection** page and turn on **Wi-Fi connections**.
2. Don't have the app on your device yet? Scan the **QR code** on the Connection page with your
   phone's camera. It downloads the Android app directly from your PC (no internet needed).
3. Open the app on your device. Your PC appears under **On this network**.
   If it doesn't, type in the address shown on the PC.
4. **First connection only:** the PC and the device both show a **6-digit code**.
   Check that they match, click **Allow** on the PC and tap **Codes match** on the device.

From then on, the device connects without asking. You can un-pair a device anytime with
**Forget** on the Connection page.

---

## Using DisplayMaster

### The quick panel (on your device)
**Swipe in from the right edge** (or press **Back**) to open the quick panel. From there you can:

- switch between **Extend** and **Mirror**
- change how your finger works: **Touch**, **Mouse** or **Trackpad**
- open the keyboard
- show the performance overlay
- disconnect

### Touch gestures (Touch mode)
| Gesture | Action |
|---|---|
| 2 fingers drag | Scroll |
| 2 fingers pinch | Zoom |
| 3 fingers swipe | Task View, show desktop, switch app |

Palm rejection is built in, so you can rest your hand while drawing.

### Drawing with a pen
- Pressure, tilt, hover, side button and eraser work in Windows Ink apps.
- Adjust the **pressure curve** (dead zone, full-pressure point, feel) in the settings, with a live preview.
- The **shortcut bar** gives you quick buttons for drawing apps: undo/redo, hold or lock
  Ctrl/Shift/Alt/Space, brush size, zoom and Esc.

### Moving windows to your device
- **One tap** on the device pulls over the window you last used on the PC.
- A **window picker** lists all windows, and lets you send them back.
- On the PC, press **Ctrl + Alt + Win + →** to send the active window to your device(s).

### Where the screen sits
In the device's settings on the PC you can choose whether the device is **left, right, above or
below** your main monitor, just like with a normal second monitor.

### Settings on the PC
Bitrate, frame-rate limit, resolution, video format and **Start when I sign in** (off by default).

### Tray icon
Closing or minimizing the window keeps DisplayMaster running in the tray.
To stop it completely, right-click the tray icon and choose **Quit**.

---

## Updates

DisplayMaster checks for new versions twice a day. When one is available, click **Update now**.
It downloads, checks and installs the update for you (one admin prompt). Devices connected over
USB get the matching Android app automatically.

---

## Tips for specific devices

- **Galaxy Tab S7+:** full resolution (2800×1752) at 120 Hz with S Pen pressure, tilt, hover and button.
- **Galaxy Z Fold 7:** has no pen support, so use **Trackpad** or **Mouse** mode. Folding,
  unfolding and rotating resize the screen automatically.
- **Laptops with two graphics chips (e.g. Zephyrus G14):** DisplayMaster uses the graphics chip
  that's already driving your screen, so it won't wake the power-hungry dedicated GPU unnecessarily.

---

## Troubleshooting

**My device isn't detected over USB**
- Make sure **USB debugging** is on and you tapped **Allow** on the device.
- Try a different cable. Some cables only charge and can't transfer data.

**My PC doesn't show up over Wi-Fi**
- Both devices must be on the **same network**.
- In Windows, the network must be set to **Private** (not Public). DisplayMaster only allows
  Wi-Fi connections on private networks for your safety.
- Try typing the address shown on the PC's Connection page.

**Something else is wrong**
Go to **Settings → Export logs**. This saves `DisplayMaster-logs-<date>.txt` to your desktop.
Attach it when you [open an issue](https://github.com/Tidesx3/DisplayMaster/issues).
The file contains device names and IP addresses, but **no keys and nothing from your screen**.
If the app won't open at all, run `DisplayMaster.exe --export-logs` instead.

---

## Privacy & security

- **Wi-Fi is off by default.**
- **Everything sent over Wi-Fi is encrypted.** Nobody else on your network can see your screen
  or send input to your PC.
- The **6-digit code** on first connection makes sure you're really talking to your own device.
- **USB** connections are trusted, because they already require you to approve USB debugging on the device.

---

## Uninstalling

Go to **Windows Settings → Apps** and uninstall DisplayMaster. This removes everything it set up.
The virtual display driver is only removed if DisplayMaster installed it (you'll be asked).

---

## Coming soon

- Connecting over USB **without** USB debugging
- Wintab driver support and pen button remapping

---

<details>
<summary><b>For developers</b> (building, testing, architecture)</summary>

### How it works

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

The engine runs elevated and windowless through an on-demand scheduled task
(`\DisplayMaster\Engine`), so pen/touch input reaches admin apps without UAC prompts.
It starts with the app and stops on *Quit*.

### Repository

| Path | What |
|---|---|
| `protocol/` | Shared C++ wire protocol, input math (pen tilt, mapping, letterboxing), keymap. Built for MSVC *and* the Android NDK. |
| `windows/host/` | C++ engine: virtual display control, capture, encoders, transports, input injection, control API. |
| `windows/ui/DisplayMaster.App/` | WinUI 3 app: tray icon, device cards with live stats, connection settings, Wi-Fi device approval. |
| `android/` | Android app: Kotlin/Compose UI + NDK core (decoder, network client). |
| `tools/` | Icon generator, window screenshot helper, packaging. |
| `third_party/` | Vendored MIT headers: NVIDIA `nvEncodeAPI.h`, AMD AMF. |

### Building

**Windows engine (C++)** requires Visual Studio 2022 with the C++ workload.
```
cmake --preset windows-x64
cmake --build --preset release
```
Output: `build/windows-x64/windows/host/Release/DisplayMasterHost.exe`

**Windows app (C# / WinUI 3)** requires the .NET 10 SDK.
```
cd windows/ui/DisplayMaster.App
dotnet build -c Release -p:Platform=x64
```
In a dev checkout the app finds and starts the engine from the CMake output folder.

**Android app** requires JDK 17+ and the Android SDK (Gradle installs the NDK).
```
cd android
gradlew assembleDebug          # app/build/outputs/apk/debug/app-debug.apk
```
minSdk 24 (Android 7). Native libraries are 16 KB page aligned.

### Testing
```
ctest --preset debug
build\windows-x64\protocol\tests\Debug\dm_protocol_tests.exe    # wire format, pen math, keymap
build\windows-x64\windows\host\Debug\dm_host_tests.exe          # VDD config, cursor rendering (WARP), JSON, approvals
powershell -File tools\integration_test.ps1                     # engine + simulated devices, incl. 3 at once
```
`dm_testclient` connects any number of simulated devices to a running engine and checks
handshake, keyframes, bitstream format and keyframe-on-request.

### Packaging
```
powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Version 0.1.0
# -> dist\DisplayMaster-Setup-0.1.0.exe   (needs Inno Setup 7: winget install JrSoftware.InnoSetup)
powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Version 0.2.0 -Publish
# also creates GitHub release v0.2.0 -> installed apps offer the update
```
`-Version` sets the engine, PC app and Android app versions alike.

Silent install: `DisplayMaster-Setup-<version>.exe /VERYSILENT /TASKS="vdd,firewall"`
Silent uninstall: `unins000.exe /VERYSILENT`

### Engine command line
The engine has no console window; in PowerShell, pipe into `Out-Host`.
```
DisplayMasterHost.exe --list-monitors | Out-Host   # all monitors incl. virtual ones
DisplayMasterHost.exe --probe-encoders             # which GPU encodes which codec, with timings
DisplayMasterHost.exe --selftest 5 --out x.hevc    # capture+encode the main screen, no device needed
DisplayMasterHost.exe -v --no-input                # verbose; log device input instead of injecting it
```
Other flags: `--mode extend|mirror`, `--codec h264|hevc|av1`, `--encoder nvenc|amf`,
`--bitrate <kbps>`, `--fps <n>`, `--scale <f>`, `--port <n>`, `--wifi`, `--no-adb`, `--no-launch`, `--log <file>`.

### Technical details
- **Video:** NVENC / AMF hardware encoding (H.264, HEVC, AV1 on RTX 40 / RDNA3), cursor
  composited on the GPU, 120 Hz with start-to-start frame pacing, one shared capture per monitor
  for multiple devices.
- **Wi-Fi transport:** UDP with adaptive Reed-Solomon loss repair and automatic TCP fallback.
  Lost frames are repaired with recovery frames (NVENC reference invalidation, HEVC/AV1),
  keyframe fallback on AMF / H.264.
- **Encryption:** Noise XX handshake (X25519, ChaCha20-Poly1305, BLAKE2b via
  [Monocypher](https://monocypher.org)), verified against the official test vectors.
  The PC key is stored DPAPI-protected in `%LOCALAPPDATA%\DisplayMaster\identity.key`.
  See `protocol/include/dm/noise.h`.
- **Keyboard:** hardware keys are sent as scancodes, so AZERTY/QWERTZ layouts work correctly.

</details>

---

## License

MIT. Third-party headers keep their own licenses (see `third_party/`).
