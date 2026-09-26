# Engine control API

`DisplayMasterHost.exe` exposes a local control channel for the DisplayMaster app:

* Named pipe `\\.\pipe\DisplayMaster.Control`, **message mode**, one JSON request → one JSON response.
* ACL: SYSTEM and Administrators full access, interactive users read/write — the engine may run
  elevated while the app doesn't.
* Implementation: `windows/host/src/ipc/control_server.cpp`, handlers in `windows/host/src/session/host.cpp`.

## Requests

| Request | Response |
|---|---|
| `{"cmd":"status"}` | Full status (below) |
| `{"cmd":"disconnect","id":N}` | `{"ok":true}` — ends session N, the device is told why |
| `{"cmd":"set_wifi","enabled":true}` | `{"ok":true}` — rebinds the listener (0.0.0.0 vs 127.0.0.1), toggles mDNS, persists the choice |
| `{"cmd":"approve","id":N,"allow":true,"remember":true}` | `{"ok":true}` or `{"ok":false,"error":"no such request"}` |
| `{"cmd":"forget_device","device_id":"…"}` | `{"ok":true}` |
| `{"cmd":"shutdown"}` | `{"ok":true}`, then the engine exits |

Unknown commands return `{"ok":false,"error":"unknown command"}`.

## Status

```json
{
  "ok": true, "version": "0.1.0",
  "host": {
    "name": "MICHI-PC", "port": 47800, "wifi": false, "elevated": true,
    "vdd": true, "adb": true,
    "addresses": ["192.168.0.101"],
    "adb_ready": ["R52R30ABCDE"], "adb_unauthorized": []
  },
  "pending": [ {"id": 3, "name": "Galaxy Z Fold7", "model": "samsung SM-F966B", "address": "192.168.0.23:50122", "code": "482 913"} ],
  "trusted": [ {"device_id": "3f9c…e1a0 (the device's public key, hex)", "name": "Galaxy Z Fold7"} ],
  "sessions": [ {
    "id": 1, "name": "Galaxy Tab S7+", "model": "samsung SM-T970", "transport": "usb",
    "streaming": true, "mode": "extend", "codec": "HEVC",
    "width": 2800, "height": 1752, "fps": 120, "bitrate_kbps": 70000,
    "encoder": "AMF", "gpu": "AMD Radeon 780M Graphics", "monitor": "\\\\.\\DISPLAY3", "pen": true,
    "sent_fps": 119.6, "mbps": 41.2, "encode_ms": 3.1, "decode_ms": 4.4, "dropped": 0,
    "rect": {"x": 2880, "y": 0, "w": 2800, "h": 1752}
  } ]
}
```

Sessions appear only after the handshake (and, for new Wi-Fi devices, approval) completed.
Live numbers (`sent_fps`, `mbps`, `encode_ms`, `decode_ms`) update once per second.
