// Moves desktop windows between screens: "pull window here" from a device, and the PC
// shortcut Ctrl+Alt+Win+Right that sends the active window to the device screens in turn.
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dm/input_math.h"
#include "dm/protocol.h"

namespace dm {

// A screen in desktop coordinates: all of it, and the part without taskbars.
struct ScreenArea {
    RectI monitor, work;
};

// Pure geometry, exposed for tests.
// Where a window of `size` goes on a screen: centered in the work area, shrunk to fit it.
// A window that covered its whole screen (borderless full screen) covers the new one.
RectI place_window(PointI size, bool fullscreen, const RectI& monitor, const RectI& work);
// Shortcut order: from a PC screen to the first device, from each device to the next.
// Returns the index in `devices`, or -1 after the last one (the window goes back).
int next_screen(const RectI& current, const std::vector<RectI>& devices);

class WindowMover {
public:
    // The device screens (extend mode, desktop coordinates) in session order. Called
    // without the mover's lock held.
    using DeviceScreens = std::function<std::vector<RectI>()>;

    explicit WindowMover(DeviceScreens devices) : devices_(std::move(devices)) {}
    ~WindowMover() { stop_hotkey(); }

    // Windows Alt+Tab would offer, most recently used first. `device`: the asking device's
    // screen (empty when it mirrors), for WindowInfo::kWindowHere.
    std::vector<proto::WindowInfo> list(const RectI& device);
    // MoveWindow from the device whose screen is `device`. `dry_run` (--no-input) only logs.
    proto::MoveWindowResult move(const proto::MoveWindow& m, const RectI& device, bool dry_run = false);

    // Registers Ctrl+Alt+Win+Right on a thread of its own. False: another app has it.
    bool start_hotkey();
    void stop_hotkey();
    bool hotkey_active() const { return hotkey_active_; }

private:
    // Where a window we moved onto a device came from, for sending it back.
    struct Origin {
        RectI monitor, rect;
        bool maximized = false;
    };
    struct App {
        std::string name;
        std::vector<uint8_t> icon;  // kIconSize^2 RGBA, empty if the program has none
    };

    void on_hotkey();
    proto::MoveResult move_here(HWND w, const ScreenArea& to, const std::vector<RectI>& devices);
    proto::MoveResult move_back(HWND w, const std::vector<RectI>& devices);
    App app_of(HWND w);

    DeviceScreens devices_;
    std::mutex mu_;  // one move at a time; guards origins_ and apps_
    std::map<HWND, Origin> origins_;
    std::map<std::wstring, App> apps_;  // by program path

    std::thread hotkey_thread_;
    DWORD hotkey_tid_ = 0;
    std::atomic<bool> hotkey_active_{false};
};

}  // namespace dm
