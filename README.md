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

## Using it

1. **Virtual display driver (for Extend mode).** Install the signed, open-source
   [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver/releases)
   (MIT). Without it DisplayMaster falls back to mirroring the main screen.
2. Start **DisplayMaster** on the PC (it starts the engine). For pen/touch input into
   admin windows, use *Restart as administrator* on the Connection page.
3. **USB:** enable *USB debugging* on the device and plug it in. The PC sets up `adb reverse`
   and launches the app, which connects by itself. (`adb` from Android platform-tools must be
   installed or bundled next to the engine.)
4. **Wi-Fi:** turn on *Wi-Fi connections* in the Connection page. PCs show up automatically in
   the app under *On this network* (mDNS), or type the address shown on the PC. The first time,
   the PC asks you to **allow** the device; allowed devices are remembered (and can be forgotten).

On the device: swipe in from the right edge (or press Back) for the quick panel — display mode,
finger input mode (Touch / Mouse / Trackpad), keyboard, performance overlay, disconnect.

### Engine command line (diagnostics)
```
DisplayMasterHost.exe --list-monitors        # all monitors incl. virtual ones
DisplayMasterHost.exe --probe-encoders       # which GPU encodes which codec, with timings
DisplayMasterHost.exe --selftest 5 --out x.hevc   # capture+encode the main screen, no device needed
DisplayMasterHost.exe -v --no-input          # verbose; log device input instead of injecting it
```
Other flags: `--mode extend|mirror`, `--codec h264|hevc|av1`, `--encoder nvenc|amf`,
`--bitrate <kbps>`, `--fps <n>`, `--scale <f>`, `--port <n>`, `--wifi`, `--no-adb`, `--no-launch`, `--log <file>`.

## Feature status (vs. SuperDisplay)

| Feature | Status |
|---|---|
| Extend (virtual monitor per device) | ✅ implemented via VDD; needs the driver installed |
| Mirror (letterboxed, no GPU clone support needed) | ✅ verified end-to-end |
| USB via ADB: auto `adb reverse`, auto-install + auto-launch of the app | ✅ verified with the packaged build |
| Wi-Fi (TCP) + mDNS discovery + device approval (remembered, forgettable) | ✅ verified |
| Hardware encode: NVENC / AMF (H.264, HEVC, AV1*) | ✅ verified (*AV1 on RTX 40 / RDNA3 like the G14) |
| Mouse cursor composited into the stream | ✅ verified (GPU, incl. inverting cursors) |
| 120 Hz, start-to-start frame pacing | ✅ |
| Several devices at once (one shared capture per monitor) | ✅ verified with 5 simulated devices; extend-mode multi-monitor needs VDD |
| Pen: pressure, tilt, hover, barrel, eraser (Windows Ink) | ✅ implemented; message flow verified, injection needs a real pen device |
| Pressure curve (dead zone, full-pressure point, feel) with live preview | ✅ |
| Multi-touch, mouse-emulation and trackpad modes, palm rejection | ✅ |
| Hardware keyboard (scancodes: correct on AZERTY/QWERTZ) + soft keyboard | ✅ |
| Rotation / Fold posture → virtual monitor resize | ✅ implemented |
| Installer, elevated engine at logon, tray autostart, firewall rule | ✅ scripts (`tools/package.ps1`, `install.ps1`) |
| Zero-setup USB (AOA, no USB debugging) | ⏳ planned (M5) |
| UDP video with FEC for Wi-Fi, TLS | ⏳ planned (M4 / backlog) |
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
powershell -ExecutionPolicy Bypass -File tools\package.ps1     # -> dist\DisplayMaster
powershell -ExecutionPolicy Bypass -File dist\DisplayMaster\install.ps1
```

## Hardware notes
* **Zephyrus G14 (780M iGPU + RTX 4070):** the engine creates its D3D device on the adapter that
  owns the captured monitor and prefers that GPU's encoder, so iGPU mode stays on AMF and never
  wakes the dGPU. `--probe-encoders` shows what each GPU can do.
* **Galaxy Tab S7+:** 2800×1752 @ 120 Hz, S Pen with pressure/tilt/hover/button.
* **Galaxy Z Fold 7:** no S Pen support (no digitizer); use Trackpad or Mouse mode. Folding,
  unfolding and rotating resize the virtual monitor automatically.

## Security
USB connections are trusted (they require a USB-debugging-authorized cable). Wi-Fi is off by
default; when on, unknown devices must be approved on the PC because a connected device can see
the screen and control input. Traffic is not encrypted yet — only use Wi-Fi on trusted networks.

## License
Project code: MIT. Third-party headers keep their licenses (`third_party/*`).
