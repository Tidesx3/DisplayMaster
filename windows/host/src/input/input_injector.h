// Injects client input into Windows.
//  - Pen:   synthetic PT_PEN pointer device (Windows Ink: pressure, tilt, hover,
//           barrel button, eraser). No driver needed (Win10 1809+).
//  - Touch: synthetic PT_TOUCH device, up to 10 contacts.
//  - Mouse/keyboard: SendInput (keys as scancodes so the PC layout applies).
// One injector per session; coordinates arrive normalized to the video surface
// and are mapped onto that session's monitor.
#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

#include "dm/input_math.h"
#include "dm/protocol.h"

namespace dm {

class InputInjector {
public:
    InputInjector();
    ~InputInjector();

    // Target monitor (desktop pixels) and the content area of the video.
    void set_target(const RectI& monitor, const RectF& content);
    void set_pressure_curve(const PressureCurve& c) { curve_ = c; }

    void pen(const proto::Pen& p);
    void touch(const proto::Touch& t);
    void mouse(const proto::Mouse& m);
    void key(const proto::Key& k);
    // Lift everything (session ended / focus lost).
    void release_all();

private:
    static constexpr int kMaxContacts = 10;
    struct Contact {
        bool active = false;
        uint32_t client_id = 0;
        POINT pt{};
        uint32_t pressure = 0;
        LONG radius = 4;
    };

    std::optional<PointI> map(float x, float y, bool clamp) const;
    void inject_touch_frame(int changed, uint32_t changed_flags);
    void keepalive_loop();

    HSYNTHETICPOINTERDEVICE pen_dev_ = nullptr;
    HSYNTHETICPOINTERDEVICE touch_dev_ = nullptr;
    std::mutex mu_;
    RectI monitor_;
    RectF content_;
    PressureCurve curve_;

    bool pen_in_range_ = false;
    bool pen_contact_ = false;
    POINT pen_pt_{};

    std::array<Contact, kMaxContacts> contacts_{};
    uint64_t last_touch_inject_us_ = 0;
    std::thread keepalive_;
    std::atomic<bool> running_{true};
};

}  // namespace dm
