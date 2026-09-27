#include "window/window_mover.h"

#include <dwmapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <future>

#include "core/log.h"
#include "core/win.h"

namespace dm {

namespace {

constexpr int kIconSize = 48;   // px on the wire; sharp at the list's 40 dp on most tablets
constexpr size_t kMaxTitle = 200;
constexpr int kHotkeyId = 1;

RectI to_rect(const RECT& r) {
    return {r.left, r.top, r.right - r.left, r.bottom - r.top};
}

RECT to_win(const RectI& r) {
    return {r.x, r.y, r.x + r.w, r.y + r.h};
}

bool same(const RectI& a, const RectI& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

std::wstring class_name(HWND w) {
    wchar_t buf[128] = {};
    GetClassNameW(w, buf, ARRAYSIZE(buf));
    return buf;
}

std::wstring title_of(HWND w) {
    std::wstring t(static_cast<size_t>(GetWindowTextLengthW(w)) + 1, L'\0');
    t.resize(static_cast<size_t>(GetWindowTextW(w, t.data(), static_cast<int>(t.size()))));
    if (t.size() > kMaxTitle) t.resize(kMaxTitle);
    return t;
}

bool cloaked(HWND w) {
    DWORD c = 0;
    return SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &c, sizeof c)) && c;
}

// The windows Alt+Tab offers: visible app windows on this virtual desktop (cloaked ones
// are on another desktop or suspended Store apps), not tool windows, owned popups or
// the shell's own.
bool switchable(HWND w) {
    if (!IsWindowVisible(w) || cloaked(w)) return false;
    const auto ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    if (!(ex & WS_EX_APPWINDOW) && (GetWindow(w, GW_OWNER) || (ex & WS_EX_NOACTIVATE))) return false;
    if (GetWindowTextLengthW(w) == 0) return false;
    const auto cls = class_name(w);
    return cls != L"Progman" && cls != L"WorkerW" && cls != L"Shell_TrayWnd" && cls != L"Shell_SecondaryTrayWnd";
}

// Front to back (EnumWindows follows the z-order, which is also the order of last use).
std::vector<HWND> switchable_windows() {
    std::vector<HWND> out;
    EnumWindows(
        [](HWND w, LPARAM p) -> BOOL {
            if (switchable(w)) reinterpret_cast<std::vector<HWND>*>(p)->push_back(w);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

std::optional<ScreenArea> screen_of(HMONITOR m) {
    MONITORINFO mi{sizeof mi};
    if (!m || !GetMonitorInfoW(m, &mi)) return std::nullopt;
    return ScreenArea{to_rect(mi.rcMonitor), to_rect(mi.rcWork)};
}

ScreenArea screen_of_window(HWND w) {
    // For a minimized window this is the screen it will come back on.
    return screen_of(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST)).value_or(ScreenArea{});
}

// The screen with exactly this desktop rectangle, if it still exists.
std::optional<ScreenArea> screen_at(const RectI& monitor) {
    if (monitor.w <= 0 || monitor.h <= 0) return std::nullopt;
    const RECT r = to_win(monitor);
    auto s = screen_of(MonitorFromRect(&r, MONITOR_DEFAULTTONULL));
    if (s && same(s->monitor, monitor)) return s;
    return std::nullopt;
}

bool is_device(const RectI& monitor, const std::vector<RectI>& devices) {
    return std::any_of(devices.begin(), devices.end(), [&](const RectI& d) { return same(d, monitor); });
}

// The PC's main screen: the primary one unless a device took that role, else any other
// screen that isn't a device.
std::optional<ScreenArea> main_screen(const std::vector<RectI>& devices) {
    std::vector<std::pair<bool, ScreenArea>> screens;  // (primary, screen)
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            MONITORINFO mi{sizeof mi};
            if (GetMonitorInfoW(m, &mi))
                reinterpret_cast<decltype(screens)*>(p)->push_back(
                    {(mi.dwFlags & MONITORINFOF_PRIMARY) != 0, {to_rect(mi.rcMonitor), to_rect(mi.rcWork)}});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&screens));
    std::stable_partition(screens.begin(), screens.end(), [](const auto& s) { return s.first; });
    for (const auto& [primary, s] : screens)
        if (!is_device(s.monitor, devices)) return s;
    return std::nullopt;
}

// Store apps live in an ApplicationFrameHost frame; the app is the child from another process.
DWORD process_of(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (class_name(w) == L"ApplicationFrameWindow") {
        struct Find {
            DWORD frame, app = 0;
        } f{pid};
        EnumChildWindows(
            w,
            [](HWND c, LPARAM p) -> BOOL {
                auto& f = *reinterpret_cast<Find*>(p);
                DWORD cp = 0;
                GetWindowThreadProcessId(c, &cp);
                if (cp == f.frame) return TRUE;
                f.app = cp;
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&f));
        if (f.app) pid = f.app;
    }
    return pid;
}

std::wstring image_path(DWORD pid) {
    UniqueHandle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!p.valid()) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD n = ARRAYSIZE(buf);
    return QueryFullProcessImageNameW(p.get(), 0, buf, &n) ? std::wstring(buf, n) : std::wstring();
}

// "Firefox" rather than "firefox.exe": the program's FileDescription, else its file name.
std::string display_name(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    std::wstring name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    if (const auto dot = name.find_last_of(L'.'); dot != std::wstring::npos) name.resize(dot);

    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return to_utf8(name);
    std::vector<uint8_t> info(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, info.data())) return to_utf8(name);
    struct Translation {
        WORD language, codepage;
    }* tr = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &len) ||
        len < sizeof *tr)
        return to_utf8(name);
    wchar_t key[64];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->language, tr->codepage);
    wchar_t* desc = nullptr;
    if (VerQueryValueW(info.data(), key, reinterpret_cast<void**>(&desc), &len) && desc && len > 1 && *desc)
        return to_utf8(desc);
    return to_utf8(name);
}

// kIconSize^2 RGBA with straight alpha.
std::vector<uint8_t> icon_pixels(HICON icon) {
    std::vector<uint8_t> out;
    if (!icon) return out;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kIconSize;
    bi.bmiHeader.biHeight = -kIconSize;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(dc);
        return out;
    }
    const HGDIOBJ old = SelectObject(dc, bmp);
    constexpr size_t n = size_t{kIconSize} * kIconSize;
    auto* px = static_cast<uint8_t*>(bits);  // BGRA, premultiplied
    std::memset(px, 0, n * 4);
    DrawIconEx(dc, 0, 0, icon, kIconSize, kIconSize, 0, nullptr, DI_NORMAL);
    GdiFlush();
    std::vector<uint8_t> color(px, px + n * 4);
    bool has_alpha = false;
    for (size_t i = 0; i < n && !has_alpha; ++i) has_alpha = color[i * 4 + 3] != 0;
    if (!has_alpha) {
        // An old icon without alpha channel: its mask says what's transparent (white).
        std::memset(px, 0xff, n * 4);
        DrawIconEx(dc, 0, 0, icon, kIconSize, kIconSize, 0, nullptr, DI_MASK);
        GdiFlush();
        for (size_t i = 0; i < n; ++i) color[i * 4 + 3] = px[i * 4] ? 0 : 255;
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);

    out.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = color[i * 4], g = color[i * 4 + 1], r = color[i * 4 + 2], a = color[i * 4 + 3];
        const auto straight = [a, has_alpha](uint8_t c) {
            if (!a) return uint8_t{0};
            return has_alpha ? static_cast<uint8_t>(std::min(255, c * 255 / a)) : c;
        };
        out[i * 4] = straight(r);
        out[i * 4 + 1] = straight(g);
        out[i * 4 + 2] = straight(b);
        out[i * 4 + 3] = a;
    }
    return out;
}

// The icon the window itself shows (apps without one in their program file). Not owned.
HICON window_icon(HWND w) {
    DWORD_PTR icon = 0;
    if (SendMessageTimeoutW(w, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &icon) && icon)
        return reinterpret_cast<HICON>(icon);
    return reinterpret_cast<HICON>(GetClassLongPtrW(w, GCLP_HICON));
}

// Windows only lets the foreground app hand over focus; borrow its input queue briefly.
void activate(HWND w) {
    if (SetForegroundWindow(w)) return;
    const DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), me = GetCurrentThreadId();
    if (fg && fg != me && AttachThreadInput(me, fg, TRUE)) {
        SetForegroundWindow(w);
        AttachThreadInput(me, fg, FALSE);
    }
}

bool elevated() {
    static const bool value = [] {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        TOKEN_ELEVATION e{};
        DWORD len = 0;
        const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &len) && e.TokenIsElevated;
        CloseHandle(token);
        return ok;
    }();
    return value;
}

// Where a window was before a move.
struct WasAt {
    RectI monitor, rect;  // its screen, and its normal (not maximized) rectangle
    bool maximized = false;
};

// Moves `w` onto `to`: at `exact` if given, else centered (see place_window). `maximize`
// unset keeps the window's own state. Fills `from` with where it was.
proto::MoveResult move_window(HWND w, const ScreenArea& to, const std::optional<RectI>& exact,
                              std::optional<bool> maximize, WasAt* from = nullptr) {
    using proto::MoveResult;
    if (IsHungAppWindow(w)) return MoveResult::NotResponding;
    const auto src = screen_of_window(w);
    WINDOWPLACEMENT wp{sizeof wp};
    GetWindowPlacement(w, &wp);
    const bool maximized = IsZoomed(w) || (IsIconic(w) && (wp.flags & WPF_RESTORETOMAXIMIZED));
    RECT r{};
    GetWindowRect(w, &r);
    const bool fullscreen = !IsIconic(w) && !IsZoomed(w) && same(to_rect(r), src.monitor);

    // A minimized or maximized window only moves in its normal state (a minimized one that
    // was maximized comes back maximized first, hence twice).
    for (int i = 0; i < 2 && (IsIconic(w) || IsZoomed(w)); ++i) ShowWindow(w, SW_RESTORE);
    if (IsIconic(w) || IsZoomed(w)) return elevated() ? MoveResult::Failed : MoveResult::Denied;
    GetWindowRect(w, &r);
    if (from) *from = {src.monitor, to_rect(r), maximized};

    // Move first, size second: on a screen with another scale the app resizes itself
    // (WM_DPICHANGED) as it arrives, and the final size is based on that.
    const RectI cur = to_rect(r);
    const RectI& work = to.work;
    if (!SetWindowPos(w, nullptr, work.x + (work.w - cur.w) / 2, work.y + (work.h - cur.h) / 2, 0, 0,
                      SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE))
        return GetLastError() == ERROR_ACCESS_DENIED ? MoveResult::Denied : MoveResult::Failed;
    GetWindowRect(w, &r);
    const RectI dst = exact ? *exact : place_window({r.right - r.left, r.bottom - r.top}, fullscreen, to.monitor, work);
    SetWindowPos(w, HWND_TOP, dst.x, dst.y, dst.w, dst.h, SWP_NOACTIVATE);
    if (maximize.value_or(maximized)) ShowWindow(w, SW_MAXIMIZE);
    activate(w);
    return MoveResult::Moved;
}

const char* result_name(proto::MoveResult r) {
    switch (r) {
        case proto::MoveResult::Moved: return "moved";
        case proto::MoveResult::NothingToMove: return "nothing to move";
        case proto::MoveResult::Gone: return "window gone";
        case proto::MoveResult::Denied: return "access denied (administrator window)";
        case proto::MoveResult::NotResponding: return "app not responding";
        case proto::MoveResult::NotExtended: return "not in extend mode";
        case proto::MoveResult::Failed: return "failed";
    }
    return "?";
}

}  // namespace

RectI place_window(PointI size, bool fullscreen, const RectI& monitor, const RectI& work) {
    if (fullscreen) return monitor;
    const int w = std::clamp(size.x, 1, std::max(1, work.w));
    const int h = std::clamp(size.y, 1, std::max(1, work.h));
    return {work.x + (work.w - w) / 2, work.y + (work.h - h) / 2, w, h};
}

int next_screen(const RectI& current, const std::vector<RectI>& devices) {
    if (devices.empty()) return -1;
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const RectI& d) { return same(d, current); });
    if (it == devices.end()) return 0;
    const auto next = static_cast<int>(it - devices.begin()) + 1;
    return next < static_cast<int>(devices.size()) ? next : -1;
}

WindowMover::App WindowMover::app_of(HWND w) {
    const auto path = image_path(process_of(w));
    if (path.empty()) return {};  // e.g. another user's process
    if (auto it = apps_.find(path); it != apps_.end()) return it->second;
    App app;
    app.name = display_name(path);
    HICON icon = nullptr;
    if (SHDefExtractIconW(path.c_str(), 0, 0, &icon, nullptr, MAKELONG(kIconSize, 16)) == S_OK && icon) {
        app.icon = icon_pixels(icon);
        DestroyIcon(icon);
    }
    return apps_.emplace(path, std::move(app)).first->second;
}

std::vector<proto::WindowInfo> WindowMover::list(const RectI& device) {
    std::lock_guard lock(mu_);
    std::vector<proto::WindowInfo> out;
    for (HWND w : switchable_windows()) {
        if (out.size() == proto::WindowList::kMaxWindows) break;
        proto::WindowInfo x;
        x.id = reinterpret_cast<uintptr_t>(w);
        x.title = to_utf8(title_of(w));
        if (IsIconic(w)) x.flags |= proto::kWindowMinimized;
        else if (IsZoomed(w)) x.flags |= proto::kWindowMaximized;
        if (device.w > 0 && same(screen_of_window(w).monitor, device)) x.flags |= proto::kWindowHere;
        App app = app_of(w);
        x.app = std::move(app.name);
        x.icon = app.icon.empty() ? icon_pixels(window_icon(w)) : std::move(app.icon);
        if (!x.icon.empty()) x.icon_size = kIconSize;
        out.push_back(std::move(x));
    }
    return out;
}

proto::MoveResult WindowMover::move_here(HWND w, const ScreenArea& to, const std::vector<RectI>& devices) {
    WasAt from;
    const auto r = move_window(w, to, std::nullopt, std::nullopt, &from);
    // Remember where it came from, unless that was another device (then keep the PC screen).
    if (r == proto::MoveResult::Moved && from.monitor.w > 0 && !is_device(from.monitor, devices)) {
        std::erase_if(origins_, [](const auto& kv) { return !IsWindow(kv.first); });
        origins_[w] = {from.monitor, from.rect, from.maximized};
    }
    return r;
}

proto::MoveResult WindowMover::move_back(HWND w, const std::vector<RectI>& devices) {
    proto::MoveResult r = proto::MoveResult::NothingToMove;
    const auto it = origins_.find(w);
    if (it != origins_.end()) {
        if (auto s = screen_at(it->second.monitor); s && !is_device(s->monitor, devices))
            r = move_window(w, *s, it->second.rect, it->second.maximized);
    }
    if (r == proto::MoveResult::NothingToMove) {
        // Not moved by us, or its screen is gone: the PC's main screen, centered.
        if (auto s = main_screen(devices)) r = move_window(w, *s, std::nullopt, std::nullopt);
    }
    if (r == proto::MoveResult::Moved) origins_.erase(w);
    return r;
}

proto::MoveWindowResult WindowMover::move(const proto::MoveWindow& m, const RectI& device, bool dry_run) {
    auto devices = devices_ ? devices_() : std::vector<RectI>{};
    if (!is_device(device, devices)) devices.push_back(device);

    std::lock_guard lock(mu_);
    proto::MoveWindowResult out;
    out.target = m.target;
    const bool here = m.target == proto::WindowTarget::Here;
    HWND w = reinterpret_cast<HWND>(static_cast<uintptr_t>(m.id));
    if (!m.id) {
        // The window used last on the other screens (Here) or on this one (Back). Minimized
        // windows were put away on purpose; always-on-top ones only if nothing else is there.
        HWND topmost = nullptr;
        for (HWND c : switchable_windows()) {
            if (IsIconic(c) || same(screen_of_window(c).monitor, device) == here) continue;
            if (!(GetWindowLongPtrW(c, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
                w = c;
                break;
            }
            if (!topmost) topmost = c;
        }
        if (!w) w = topmost;
        if (!w) {
            out.result = proto::MoveResult::NothingToMove;
            return out;
        }
    }
    if (!IsWindow(w) || !switchable(w)) {
        out.result = proto::MoveResult::Gone;
        return out;
    }
    out.title = to_utf8(title_of(w));
    if (dry_run) {
        DM_LOGI("Windows: would move a window %s (--no-input)", here ? "to the device" : "back");
        out.result = proto::MoveResult::Moved;
        return out;
    }
    if (here) {
        const auto s = screen_at(device);
        out.result = s ? move_here(w, *s, devices) : proto::MoveResult::Failed;
    } else {
        out.result = move_back(w, devices);
    }
    DM_LOGI("Windows: %s %s", here ? "to the device:" : "back:", result_name(out.result));
    return out;
}

void WindowMover::on_hotkey() {
    HWND w = GetForegroundWindow();
    if (w && !switchable(w)) w = GetAncestor(w, GA_ROOTOWNER);  // a dialog: its main window
    if (!w || !switchable(w)) return;
    const auto devices = devices_ ? devices_() : std::vector<RectI>{};
    if (devices.empty()) return;

    std::lock_guard lock(mu_);
    const int next = next_screen(screen_of_window(w).monitor, devices);
    proto::MoveResult r = proto::MoveResult::Failed;
    if (next < 0) {
        r = move_back(w, devices);
    } else if (auto s = screen_at(devices[static_cast<size_t>(next)])) {
        r = move_here(w, *s, devices);
    }
    DM_LOGI("Windows: shortcut moved the active window to %s: %s", next < 0 ? "the PC" : "a device", result_name(r));
}

bool WindowMover::start_hotkey() {
    stop_hotkey();
    std::promise<bool> registered;
    auto result = registered.get_future();
    hotkey_thread_ = std::thread([this, &registered] {
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // creates the message queue
        hotkey_tid_ = GetCurrentThreadId();
        const bool ok = RegisterHotKey(nullptr, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_WIN | MOD_NOREPEAT, VK_RIGHT);
        registered.set_value(ok);
        if (!ok) return;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
            if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyId) on_hotkey();
        UnregisterHotKey(nullptr, kHotkeyId);
    });
    const bool ok = result.get();
    if (!ok) {
        hotkey_thread_.join();
        DM_LOGW("Windows: Ctrl+Alt+Win+Right is taken by another app - the send-to-device shortcut is off");
    }
    hotkey_active_ = ok;
    return ok;
}

void WindowMover::stop_hotkey() {
    if (hotkey_thread_.joinable()) {
        PostThreadMessageW(hotkey_tid_, WM_QUIT, 0, 0);
        hotkey_thread_.join();
    }
    hotkey_active_ = false;
}

}  // namespace dm
