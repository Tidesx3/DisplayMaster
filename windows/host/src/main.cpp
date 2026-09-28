// DisplayMaster host: turns Android devices into extra monitors for this PC.
// Built for the GUI subsystem so it never opens a console window of its own; run from a
// terminal it prints into that terminal (PowerShell returns to the prompt right away:
// pipe into Out-Host, e.g. `DisplayMasterHost.exe --list-monitors | Out-Host`, to wait).
//
//   DisplayMasterHost.exe [options]
//     --mode extend|mirror      force a display mode (default: what the device asks for)
//     --codec h264|hevc|av1     force a codec (default: best the device decodes)
//     --encoder auto|nvenc|amf  force an encoder backend
//     --bitrate <kbps>          fixed bitrate (default: automatic)
//     --fps <n>                 frame-rate cap (default 120)
//     --scale <f>               virtual monitor resolution relative to the device (1.0)
//     --port <n>                TCP port (47800)
//     --wifi                    accept Wi-Fi connections (default: USB only)
//     --no-adb                  don't manage adb reverse
//     --no-launch               don't auto-start the app on USB connect
//     --no-input                log input from devices instead of injecting it
//     --test-mode               automated tests: separate data folder and control pipe,
//                               local connections count as Wi-Fi, pairings auto-approved
//     --test-frames             tests: keep sending frames, a blank one if the screen is off
//     --log <file>|auto         also log to a file (auto: %LOCALAPPDATA%\DisplayMaster\host.log)
//     -v                        verbose logging
//   Diagnostics:
//     --list-monitors           print monitors (incl. virtual) and exit
//     --set-monitor <target> on|off   attach/detach a monitor
//   Setup (elevated, used by the installer):
//     --install-vdd <dir>       install the virtual display driver from <dir>\MttVDD.inf
//     --uninstall-vdd           remove the virtual display driver
//     --probe-encoders          encode a test frame on every GPU with every backend/codec
//     --selftest <seconds> [--out file]
//                               capture+encode the primary monitor without a client,
//                               print throughput, optionally write the raw stream
#include <windows.h>

#include <cstdio>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/win.h"
#include "diagnostics.h"
#include "display/display_config.h"
#include "setup/vdd_setup.h"
#include "session/host.h"
#include "transport/connection.h"

using namespace dm;

namespace {

HANDLE g_quit = nullptr;

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD len = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &len) && e.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

// No console of our own: use the terminal we were started from, if any. Redirected
// handles (pipes, files) are already wired up by the CRT and are left alone.
void attach_parent_console() {
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
}

BOOL WINAPI on_ctrl(DWORD) {
    SetEvent(g_quit);
    return TRUE;
}

std::optional<proto::Codec> parse_codec(const std::string& s) {
    if (s == "h264" || s == "avc") return proto::Codec::H264;
    if (s == "hevc" || s == "h265") return proto::Codec::HEVC;
    if (s == "av1") return proto::Codec::AV1;
    return std::nullopt;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Per-monitor DPI awareness: required by DuplicateOutput1 and for physical-pixel coordinates.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    attach_parent_console();

    HostOptions opts;
    std::wstring log_file;
    auto level = log::Level::Info;
    int selftest_seconds = 0;
    std::string selftest_out;
    bool list = false;
    std::wstring install_vdd_dir;
    bool uninstall_vdd = false;
    bool probe = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = to_utf8(argv[i]);
        auto next = [&]() -> std::string { return i + 1 < argc ? to_utf8(argv[++i]) : std::string(); };
        if (a == "--mode") {
            const auto v = next();
            opts.force_mode = v == "mirror" ? proto::DisplayMode::Mirror : proto::DisplayMode::Extend;
        } else if (a == "--codec") {
            opts.codec = parse_codec(next());
            opts.stream_from_cli = true;
        } else if (a == "--encoder") {
            const auto v = next();
            opts.backend = v == "nvenc" ? EncoderBackend::Nvenc : v == "amf" ? EncoderBackend::Amf : EncoderBackend::Auto;
        } else if (a == "--bitrate") {
            opts.bitrate_kbps = static_cast<uint32_t>(std::stoul(next()));
            opts.stream_from_cli = true;
        } else if (a == "--fps") {
            opts.max_fps = static_cast<uint32_t>(std::stoul(next()));
            opts.stream_from_cli = true;
        } else if (a == "--scale") {
            opts.resolution_scale = std::stof(next());
            opts.stream_from_cli = true;
        } else if (a == "--port") {
            opts.port = static_cast<uint16_t>(std::stoul(next()));
        } else if (a == "--wifi") {
            opts.allow_wifi = true;
        } else if (a == "--no-adb") {
            opts.adb = false;
        } else if (a == "--no-input") {
            opts.inject_input = false;
        } else if (a == "--test-mode") {
            opts.test_mode = true;
        } else if (a == "--test-frames") {
            opts.test_frames = true;
        } else if (a == "--no-launch") {
            opts.adb_auto_launch = false;
        } else if (a == "--log") {
            // "auto": %LOCALAPPDATA%\DisplayMaster\host.log of whoever runs the engine.
            const auto v = next();
            log_file = v == "auto" ? (Config::default_path().parent_path() / L"host.log").wstring() : to_wide(v);
        } else if (a == "-v") {
            level = log::Level::Debug;
        } else if (a == "--install-vdd") {
            install_vdd_dir = to_wide(next());
        } else if (a == "--uninstall-vdd") {
            uninstall_vdd = true;
        } else if (a == "--set-monitor") {
            // Attach/detach a monitor by target id (see --list-monitors).
            const auto target = static_cast<uint32_t>(std::stoul(next()));
            const bool on = next() == "on";
            for (const auto& m : enumerate_monitors(true))
                if (m.target_id == target) return set_target_active(m.adapter_luid, target, on) ? 0 : 1;
            fprintf(stderr, "No monitor with target id %u\n", target);
            return 2;
        } else if (a == "--list-monitors") {
            list = true;
        } else if (a == "--probe-encoders") {
            probe = true;
        } else if (a == "--selftest") {
            selftest_seconds = std::stoi(next());
        } else if (a == "--out") {
            selftest_out = next();
        } else {
            fprintf(stderr, "Unknown option: %s\n", a.c_str());
            return 2;
        }
    }
    log::init(log_file, level);

    if (!install_vdd_dir.empty()) return setup::install_vdd(install_vdd_dir);
    if (uninstall_vdd) return setup::uninstall_vdd();
    if (list) return list_monitors();
    if (probe) return probe_encoders();
    if (selftest_seconds > 0) return selftest(selftest_seconds, selftest_out, opts);

    if (!winsock_init()) return 1;
    g_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    SYSTEMTIME today;
    GetLocalTime(&today);  // log lines only carry the time of day
    DM_LOGI("DisplayMaster %s host starting on %04d-%02d-%02d (Ctrl+C to quit)", DM_VERSION, today.wYear, today.wMonth,
            today.wDay);
    if (!is_elevated())
        DM_LOGW("Not elevated: input won't reach admin windows and virtual monitor settings may be read-only");
    Host host(opts);
    host.on_shutdown_requested = [] { SetEvent(g_quit); };
    if (!host.start()) return 1;
    while (WaitForSingleObject(g_quit, 1000) == WAIT_TIMEOUT) host.reap();
    DM_LOGI("Shutting down");
    host.stop();
    return 0;
}
