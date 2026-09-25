// Pure math shared by host and client: pen tilt conversion, pressure curves,
// coordinate mapping and letterboxing. Header-only and unit tested.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace dm {

constexpr float kPi = 3.14159265358979323846f;

struct TiltXY {
    float x_deg = 0;
    float y_deg = 0;
};

// Android reports stylus tilt as AXIS_TILT (angle from the screen normal, 0..pi/2)
// plus AXIS_ORIENTATION (direction in the screen plane, 0 = up, +pi/2 = right).
// Windows / W3C want tiltX/tiltY: the pen's angle in the X-Z and Y-Z planes.
// Same conversion Chromium uses for Android pointer events.
inline TiltXY tilt_orientation_to_xy(float tilt_rad, float orientation_rad) {
    tilt_rad = std::clamp(tilt_rad, 0.0f, kPi / 2);
    const float r = std::sin(tilt_rad);
    // cos(pi/2) is slightly negative in float; a negative z would flip atan2 past 90.
    const float z = std::max(std::cos(tilt_rad), 0.0f);
    auto deg = [](float rad) { return std::clamp(rad * 180.0f / kPi, -90.0f, 90.0f); };
    TiltXY t;
    t.x_deg = deg(std::atan2(std::sin(-orientation_rad) * r, z));
    t.y_deg = deg(std::atan2(std::cos(-orientation_rad) * r, z));
    return t;
}

// Pressure shaping: ignore below `min_in`, saturate above `max_in`, then apply a
// gamma (<1 = softer / more ink early, >1 = firmer).
struct PressureCurve {
    float min_in = 0.0f;
    float max_in = 1.0f;
    float gamma = 1.0f;

    float apply(float p) const {
        if (max_in <= min_in) return p > min_in ? 1.0f : 0.0f;
        const float t = std::clamp((p - min_in) / (max_in - min_in), 0.0f, 1.0f);
        return gamma == 1.0f ? t : std::pow(t, gamma);
    }
};

struct RectF {
    float x = 0, y = 0, w = 1, h = 1;
};

struct RectI {
    int32_t x = 0, y = 0, w = 0, h = 0;
};

struct PointI {
    int32_t x = 0, y = 0;
};

// Map a point normalized to the video surface into desktop pixels of `target`.
// `content` is the normalized sub-rect of the video that holds the desktop
// (letterboxing). Points in the letterbox bars return nullopt unless `clamp_edges`.
inline std::optional<PointI> map_to_desktop(float nx, float ny, const RectF& content, const RectI& target,
                                            bool clamp_edges = false) {
    if (content.w <= 0 || content.h <= 0 || target.w <= 0 || target.h <= 0) return std::nullopt;
    float u = (nx - content.x) / content.w;
    float v = (ny - content.y) / content.h;
    if (u < 0 || u > 1 || v < 0 || v > 1) {
        if (!clamp_edges) return std::nullopt;
        u = std::clamp(u, 0.0f, 1.0f);
        v = std::clamp(v, 0.0f, 1.0f);
    }
    // Last pixel is w-1: map [0,1] onto [0, w-1] so the far edge stays on-monitor.
    const auto px = static_cast<int32_t>(std::lround(u * static_cast<float>(target.w - 1)));
    const auto py = static_cast<int32_t>(std::lround(v * static_cast<float>(target.h - 1)));
    return PointI{target.x + px, target.y + py};
}

// Fit a source of size sw x sh into a destination dw x dh preserving aspect ratio,
// centered. Returns the occupied area normalized to the destination.
inline RectF letterbox(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh) {
    if (!sw || !sh || !dw || !dh) return {};
    const double src_aspect = static_cast<double>(sw) / sh;
    const double dst_aspect = static_cast<double>(dw) / dh;
    RectF r;
    if (src_aspect > dst_aspect) {  // wider: bars top/bottom
        r.w = 1;
        r.h = static_cast<float>(dst_aspect / src_aspect);
        r.x = 0;
        r.y = (1 - r.h) / 2;
    } else {  // taller: bars left/right
        r.h = 1;
        r.w = static_cast<float>(src_aspect / dst_aspect);
        r.y = 0;
        r.x = (1 - r.w) / 2;
    }
    return r;
}

struct Size {
    uint32_t w = 0, h = 0;
    bool operator==(const Size&) const = default;
};

// Virtual-monitor resolution for a device panel: scale the panel's pixels,
// round to even (encoders need 4:2:0 chroma alignment) and clamp to encoder limits.
inline Size virtual_mode_for_panel(uint32_t panel_w, uint32_t panel_h, float scale = 1.0f,
                                   uint32_t max_dim = 4096) {
    if (!panel_w || !panel_h) return {1920, 1080};
    double w = panel_w * static_cast<double>(scale);
    double h = panel_h * static_cast<double>(scale);
    const double over = std::max(w, h) / max_dim;
    if (over > 1.0) {
        w /= over;
        h /= over;
    }
    auto even = [](double v) { return std::max<uint32_t>(2u, static_cast<uint32_t>(v) & ~1u); };
    return {even(w), even(h)};
}

}  // namespace dm
