#include "input/input_injector.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/log.h"
#include "core/win.h"

namespace dm {
namespace {

// Windows cancels injected touch contacts that go quiet; resend held contacts
// at least this often.
constexpr uint64_t kTouchKeepaliveUs = 60'000;

// Synthetic pointer devices span the whole virtual desktop but read ptPixelLocation
// relative to its top-left corner, not as screen coordinates. With a monitor left of
// or above the primary (negative origin) every point would land shifted by that much.
POINT to_device(POINT desktop) {
    return {desktop.x - GetSystemMetrics(SM_XVIRTUALSCREEN), desktop.y - GetSystemMetrics(SM_YVIRTUALSCREEN)};
}

}  // namespace

InputInjector::InputInjector() {
    pen_dev_ = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
    touch_dev_ = CreateSyntheticPointerDevice(PT_TOUCH, kMaxContacts, POINTER_FEEDBACK_DEFAULT);
    if (!pen_dev_ || !touch_dev_)
        DM_LOGE("CreateSyntheticPointerDevice failed (%lu) - pen/touch input unavailable", GetLastError());
    keepalive_ = std::thread([this] { keepalive_loop(); });
}

InputInjector::~InputInjector() {
    running_ = false;
    if (keepalive_.joinable()) keepalive_.join();
    release_all();
    if (pen_dev_) DestroySyntheticPointerDevice(pen_dev_);
    if (touch_dev_) DestroySyntheticPointerDevice(touch_dev_);
}

void InputInjector::set_target(const RectI& monitor, const RectF& content) {
    std::lock_guard lock(mu_);
    monitor_ = monitor;
    content_ = content;
}

std::optional<PointI> InputInjector::map(float x, float y, bool clamp) const {
    return map_to_desktop(x, y, content_, monitor_, clamp);
}

// ---------------------------------------------------------------- pen

void InputInjector::pen(const proto::Pen& p) {
    std::lock_guard lock(mu_);
    if (!pen_dev_) return;
    const bool in_range = p.flags & proto::kPenInRange;
    const bool contact = in_range && (p.flags & proto::kPenContact);

    // Strokes that started on-screen keep tracking into the letterbox (clamped);
    // hover outside the content area is simply dropped.
    auto pt = map(p.x, p.y, pen_contact_ || contact);
    if (!pt && in_range) return;

    POINTER_TYPE_INFO info{};
    info.type = PT_PEN;
    auto& pen = info.penInfo;
    pen.pointerInfo.pointerType = PT_PEN;
    const POINT desktop_pt = pt ? POINT{pt->x, pt->y} : pen_pt_;
    pen.pointerInfo.ptPixelLocation = to_device(desktop_pt);

    POINTER_FLAGS flags = 0;
    if (in_range) flags |= POINTER_FLAG_INRANGE;
    if (contact) flags |= POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON;
    if (contact && !pen_contact_)
        flags |= POINTER_FLAG_DOWN;
    else if (!contact && pen_contact_)
        flags |= POINTER_FLAG_UP;
    else
        flags |= POINTER_FLAG_UPDATE;
    pen.pointerInfo.pointerFlags = flags;

    pen.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y | PEN_MASK_ROTATION;
    pen.pressure = contact ? static_cast<UINT32>(std::lround(curve_.apply(p.pressure) * 1024.0f)) : 0;
    pen.tiltX = static_cast<INT32>(std::lround(std::clamp(p.tilt_x, -90.0f, 90.0f)));
    pen.tiltY = static_cast<INT32>(std::lround(std::clamp(p.tilt_y, -90.0f, 90.0f)));
    pen.rotation = static_cast<UINT32>(std::lround(p.rotation)) % 360;
    if (p.flags & (proto::kPenBarrel | proto::kPenBarrel2)) pen.penFlags |= PEN_FLAG_BARREL;
    if (p.flags & proto::kPenInverted) pen.penFlags |= PEN_FLAG_INVERTED;
    if (p.flags & proto::kPenEraser) pen.penFlags |= contact ? PEN_FLAG_ERASER : PEN_FLAG_INVERTED;

    if (!InjectSyntheticPointerInput(pen_dev_, &info, 1)) DM_LOGD("pen inject failed %lu", GetLastError());
    pen_in_range_ = in_range;
    pen_contact_ = contact;
    pen_pt_ = desktop_pt;
}

// ---------------------------------------------------------------- touch

void InputInjector::inject_touch_frame(int changed, uint32_t changed_flags) {
    std::vector<POINTER_TYPE_INFO> infos;
    for (int i = 0; i < kMaxContacts; ++i) {
        const auto& c = contacts_[i];
        if (!c.active && i != changed) continue;
        POINTER_TYPE_INFO info{};
        info.type = PT_TOUCH;
        auto& t = info.touchInfo;
        t.pointerInfo.pointerType = PT_TOUCH;
        t.pointerInfo.pointerId = static_cast<UINT32>(i);
        const POINT pt = to_device(c.pt);
        t.pointerInfo.ptPixelLocation = pt;
        t.pointerInfo.pointerFlags =
            i == changed ? changed_flags : (POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
        t.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
        t.rcContact = {pt.x - c.radius, pt.y - c.radius, pt.x + c.radius, pt.y + c.radius};
        t.pressure = c.pressure;
        infos.push_back(info);
    }
    if (infos.empty()) return;
    if (!InjectSyntheticPointerInput(touch_dev_, infos.data(), static_cast<UINT32>(infos.size())))
        DM_LOGD("touch inject failed %lu", GetLastError());
    last_touch_inject_us_ = now_us();
}

void InputInjector::touch(const proto::Touch& t) {
    std::lock_guard lock(mu_);
    if (!touch_dev_) return;

    // Palm rejection: ignore touch while the pen is near the screen.
    if (pen_in_range_ && t.action == proto::PointerAction::Down) return;

    auto slot_of = [&](uint32_t client_id) {
        for (int i = 0; i < kMaxContacts; ++i)
            if (contacts_[i].active && contacts_[i].client_id == client_id) return i;
        return -1;
    };

    if (t.action == proto::PointerAction::Cancel) {
        for (int i = 0; i < kMaxContacts; ++i) {
            if (!contacts_[i].active) continue;
            contacts_[i].active = false;
            inject_touch_frame(i, POINTER_FLAG_UP | POINTER_FLAG_CANCELED);
        }
        return;
    }

    // Update positions of all contacts we already track.
    for (const auto& p : t.points) {
        const int s = slot_of(p.id);
        if (s < 0) continue;
        if (auto pt = map(p.x, p.y, true)) contacts_[s].pt = {pt->x, pt->y};
        contacts_[s].pressure = static_cast<uint32_t>(std::clamp(p.pressure, 0.0f, 1.0f) * 1024.0f);
    }

    int changed = -1;
    uint32_t flags = 0;
    switch (t.action) {
        case proto::PointerAction::Down: {
            if (slot_of(t.action_id) >= 0) break;
            const auto it = std::find_if(t.points.begin(), t.points.end(),
                                         [&](const proto::TouchPoint& p) { return p.id == t.action_id; });
            if (it == t.points.end()) break;
            auto pt = map(it->x, it->y, false);
            if (!pt) break;  // touch-down in the letterbox bars
            for (int i = 0; i < kMaxContacts; ++i) {
                if (contacts_[i].active) continue;
                auto& c = contacts_[i];
                c.active = true;
                c.client_id = t.action_id;
                c.pt = {pt->x, pt->y};
                c.pressure = static_cast<uint32_t>(std::clamp(it->pressure, 0.0f, 1.0f) * 1024.0f);
                c.radius = std::max<LONG>(2, static_cast<LONG>(it->major * monitor_.w / 2));
                changed = i;
                flags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
                break;
            }
            break;
        }
        case proto::PointerAction::Up: {
            changed = slot_of(t.action_id);
            if (changed >= 0) {
                contacts_[changed].active = false;
                flags = POINTER_FLAG_UP;
            }
            break;
        }
        default: break;
    }
    if (changed < 0 && std::none_of(contacts_.begin(), contacts_.end(), [](const Contact& c) { return c.active; }))
        return;
    inject_touch_frame(changed, flags);
}

void InputInjector::keepalive_loop() {
    while (running_) {
        Sleep(20);
        std::lock_guard lock(mu_);
        const bool any = std::any_of(contacts_.begin(), contacts_.end(), [](const Contact& c) { return c.active; });
        if (any && now_us() - last_touch_inject_us_ > kTouchKeepaliveUs) inject_touch_frame(-1, 0);
    }
}

// ---------------------------------------------------------------- mouse / keyboard

void InputInjector::mouse(const proto::Mouse& m) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    auto abs_move = [&](int x, int y) {
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        in.mi.dx = static_cast<LONG>((static_cast<int64_t>(x - vx) * 65535) / std::max(1, vw - 1));
        in.mi.dy = static_cast<LONG>((static_cast<int64_t>(y - vy) * 65535) / std::max(1, vh - 1));
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    };
    switch (m.kind) {
        case proto::MouseKind::MoveAbs: {
            std::optional<PointI> pt;
            {
                std::lock_guard lock(mu_);
                pt = map(m.x, m.y, true);
            }
            if (!pt) return;
            abs_move(pt->x, pt->y);
            break;
        }
        case proto::MouseKind::MoveRel: {
            // Absolute move from the current position: immune to pointer acceleration.
            POINT cur;
            GetCursorPos(&cur);
            abs_move(cur.x + static_cast<int>(std::lround(m.x)), cur.y + static_cast<int>(std::lround(m.y)));
            break;
        }
        case proto::MouseKind::Button: {
            static constexpr DWORD kDown[] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN};
            static constexpr DWORD kUp[] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP};
            const auto b = static_cast<size_t>(m.button);
            if (b >= 3) return;
            in.mi.dwFlags = m.down ? kDown[b] : kUp[b];
            break;
        }
        case proto::MouseKind::Wheel: {
            if (m.y != 0) {
                in.mi.dwFlags = MOUSEEVENTF_WHEEL;
                in.mi.mouseData = static_cast<DWORD>(std::lround(m.y * WHEEL_DELTA));
                SendInput(1, &in, sizeof in);
            }
            if (m.x == 0) return;
            in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
            in.mi.mouseData = static_cast<DWORD>(std::lround(m.x * WHEEL_DELTA));
            break;
        }
    }
    SendInput(1, &in, sizeof in);
}

void InputInjector::key(const proto::Key& k) {
    const bool down = k.flags & proto::kKeyDown;
    if (k.scancode) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = k.scancode;
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP) |
                        ((k.flags & proto::kKeyExtended) ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(1, &in, sizeof in);
        return;
    }
    if (!k.unicode || !down) return;
    // Text without a scancode: type it as UTF-16 (surrogate pairs as two units).
    wchar_t units[2];
    int n = 1;
    if (k.unicode > 0xFFFF) {
        const uint32_t v = k.unicode - 0x10000;
        units[0] = static_cast<wchar_t>(0xD800 + (v >> 10));
        units[1] = static_cast<wchar_t>(0xDC00 + (v & 0x3FF));
        n = 2;
    } else {
        units[0] = static_cast<wchar_t>(k.unicode);
    }
    INPUT in[4]{};
    for (int i = 0; i < n; ++i) {
        in[i * 2].type = in[i * 2 + 1].type = INPUT_KEYBOARD;
        in[i * 2].ki.wScan = in[i * 2 + 1].ki.wScan = units[i];
        in[i * 2].ki.dwFlags = KEYEVENTF_UNICODE;
        in[i * 2 + 1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    }
    SendInput(static_cast<UINT>(n * 2), in, sizeof(INPUT));
}

void InputInjector::release_all() {
    std::lock_guard lock(mu_);
    if (pen_dev_ && (pen_in_range_ || pen_contact_)) {
        POINTER_TYPE_INFO info{};
        info.type = PT_PEN;
        info.penInfo.pointerInfo.pointerType = PT_PEN;
        info.penInfo.pointerInfo.ptPixelLocation = to_device(pen_pt_);
        info.penInfo.pointerInfo.pointerFlags = pen_contact_ ? POINTER_FLAG_UP : POINTER_FLAG_UPDATE;
        InjectSyntheticPointerInput(pen_dev_, &info, 1);
        pen_in_range_ = pen_contact_ = false;
    }
    for (int i = 0; i < kMaxContacts; ++i) {
        if (!contacts_[i].active) continue;
        contacts_[i].active = false;
        inject_touch_frame(i, POINTER_FLAG_UP | POINTER_FLAG_CANCELED);
    }
}

}  // namespace dm
