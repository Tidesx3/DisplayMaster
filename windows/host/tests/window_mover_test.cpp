#include <gtest/gtest.h>

#include <algorithm>

#include "window/window_mover.h"

using namespace dm;

namespace {

bool eq(const RectI& a, const RectI& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

}  // namespace

TEST(WindowMover, PlaceCentersInTheWorkArea) {
    const RectI monitor{2560, 0, 2560, 1600}, work{2560, 0, 2560, 1552};  // taskbar at the bottom
    EXPECT_TRUE(eq(place_window({1200, 800}, false, monitor, work), RectI{2560 + 680, 376, 1200, 800}));
}

TEST(WindowMover, PlaceShrinksToFit) {
    const RectI monitor{-1920, 0, 1920, 1080}, work{-1920, 0, 1920, 1032};
    EXPECT_TRUE(eq(place_window({2560, 1400}, false, monitor, work), work));
    EXPECT_TRUE(eq(place_window({800, 1400}, false, monitor, work), RectI{-1920 + 560, 0, 800, 1032}));
}

TEST(WindowMover, FullScreenWindowsCoverTheNewScreen) {
    const RectI monitor{1920, 0, 2176, 1812}, work{1920, 0, 2176, 1764};
    EXPECT_TRUE(eq(place_window({1920, 1080}, true, monitor, work), monitor));
}

TEST(WindowMover, ShortcutCyclesThroughDevicesThenBack) {
    const RectI pc{0, 0, 2560, 1440}, tab{2560, 0, 2560, 1600}, fold{-2176, 0, 2176, 1812};
    const std::vector<RectI> devices{tab, fold};
    EXPECT_EQ(next_screen(pc, devices), 0);
    EXPECT_EQ(next_screen(tab, devices), 1);
    EXPECT_EQ(next_screen(fold, devices), -1);
    EXPECT_EQ(next_screen(pc, {}), -1);
    EXPECT_EQ(next_screen(tab, {tab}), -1);
}

namespace {

RectI window_rect(HWND w) {
    RECT r{};
    GetWindowRect(w, &r);
    return {r.left, r.top, r.right - r.left, r.bottom - r.top};
}

bool inside(const RectI& a, const RectI& outer) {
    return a.x >= outer.x && a.y >= outer.y && a.x + a.w <= outer.x + outer.w && a.y + a.h <= outer.y + outer.h;
}

}  // namespace

// Moves a window of its own between two real screens (the second one stands in for a
// device). Needs two monitors and briefly shows (and focuses) a blank window, so it only
// runs when asked for: --gtest_also_run_disabled_tests --gtest_filter=*MovesAWindow*
TEST(WindowMover, DISABLED_MovesAWindowToAnotherScreenAndBack) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::vector<ScreenArea> screens;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(m, &mi);
            const auto r = [](const RECT& x) { return RectI{x.left, x.top, x.right - x.left, x.bottom - x.top}; };
            auto& v = *reinterpret_cast<std::vector<ScreenArea>*>(p);
            // The primary screen first.
            v.insert((mi.dwFlags & MONITORINFOF_PRIMARY) ? v.begin() : v.end(), {r(mi.rcMonitor), r(mi.rcWork)});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&screens));
    if (screens.size() < 2) GTEST_SKIP() << "needs two screens";
    const ScreenArea pc = screens[0], device = screens[1];

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DmMoveProbe";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"DisplayMaster move test", WS_OVERLAPPEDWINDOW,
                             pc.work.x + 100, pc.work.y + 100, 640, 400, nullptr, nullptr, wc.hInstance, nullptr);
    ASSERT_TRUE(w);
    ShowWindow(w, SW_SHOWNOACTIVATE);
    const RectI start = window_rect(w);
    const uint64_t id = reinterpret_cast<uintptr_t>(w);

    WindowMover mover([&] { return std::vector<RectI>{device.monitor}; });
    auto find = [&](const std::vector<proto::WindowInfo>& l) {
        return std::find_if(l.begin(), l.end(), [&](const proto::WindowInfo& x) { return x.id == id; });
    };
    auto list = mover.list(device.monitor);
    auto it = find(list);
    ASSERT_NE(it, list.end());
    EXPECT_EQ(it->title, "DisplayMaster move test");
    EXPECT_FALSE(it->flags & proto::kWindowHere);

    // Here: centered on the device.
    auto r = mover.move({id, proto::WindowTarget::Here}, device.monitor);
    EXPECT_EQ(r.result, proto::MoveResult::Moved);
    EXPECT_EQ(r.title, "DisplayMaster move test");
    const RectI there = window_rect(w);
    EXPECT_TRUE(inside(there, device.work));
    EXPECT_NEAR(there.x + there.w / 2, device.work.x + device.work.w / 2, 1);
    list = mover.list(device.monitor);
    it = find(list);
    ASSERT_NE(it, list.end());
    EXPECT_TRUE(it->flags & proto::kWindowHere);

    // Back: exactly where it was.
    r = mover.move({id, proto::WindowTarget::Back}, device.monitor);
    EXPECT_EQ(r.result, proto::MoveResult::Moved);
    EXPECT_TRUE(eq(window_rect(w), start));

    // Maximized stays maximized, on the other screen and back.
    ShowWindow(w, SW_MAXIMIZE);
    EXPECT_EQ(mover.move({id, proto::WindowTarget::Here}, device.monitor).result, proto::MoveResult::Moved);
    EXPECT_TRUE(IsZoomed(w));
    EXPECT_EQ(MonitorFromWindow(w, MONITOR_DEFAULTTONULL), MonitorFromPoint({device.monitor.x + 1, device.monitor.y + 1}, MONITOR_DEFAULTTONULL));
    EXPECT_EQ(mover.move({id, proto::WindowTarget::Back}, device.monitor).result, proto::MoveResult::Moved);
    EXPECT_TRUE(IsZoomed(w));
    EXPECT_EQ(MonitorFromWindow(w, MONITOR_DEFAULTTONULL), MonitorFromPoint({pc.monitor.x + 1, pc.monitor.y + 1}, MONITOR_DEFAULTTONULL));

    // Minimized: comes back restored on the device.
    ShowWindow(w, SW_RESTORE);
    ShowWindow(w, SW_MINIMIZE);
    EXPECT_EQ(mover.move({id, proto::WindowTarget::Here}, device.monitor).result, proto::MoveResult::Moved);
    EXPECT_FALSE(IsIconic(w));
    EXPECT_TRUE(inside(window_rect(w), device.work));

    DestroyWindow(w);
    EXPECT_EQ(mover.move({id, proto::WindowTarget::Back}, device.monitor).result, proto::MoveResult::Gone);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}
