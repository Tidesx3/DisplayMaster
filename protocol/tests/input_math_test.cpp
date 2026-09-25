#include <gtest/gtest.h>

#include "dm/input_math.h"
#include "dm/keymap.h"

using namespace dm;

TEST(Tilt, PerpendicularPenHasNoTilt) {
    const auto t = tilt_orientation_to_xy(0.0f, 1.234f);
    EXPECT_NEAR(t.x_deg, 0.0f, 1e-4);
    EXPECT_NEAR(t.y_deg, 0.0f, 1e-4);
}

TEST(Tilt, OrientationRightGivesPureTiltX) {
    // Orientation +pi/2 ("pointing right"), tilted 30 degrees.
    const auto t = tilt_orientation_to_xy(30 * kPi / 180, kPi / 2);
    EXPECT_NEAR(std::abs(t.x_deg), 30.0f, 1e-3);
    EXPECT_NEAR(t.y_deg, 0.0f, 1e-3);
}

TEST(Tilt, OrientationUpGivesPureTiltY) {
    const auto t = tilt_orientation_to_xy(45 * kPi / 180, 0.0f);
    EXPECT_NEAR(t.x_deg, 0.0f, 1e-3);
    EXPECT_NEAR(std::abs(t.y_deg), 45.0f, 1e-3);
}

TEST(Tilt, OppositeOrientationsMirror) {
    const auto a = tilt_orientation_to_xy(0.5f, 0.7f);
    const auto b = tilt_orientation_to_xy(0.5f, 0.7f - kPi);
    EXPECT_NEAR(a.x_deg, -b.x_deg, 1e-3);
    EXPECT_NEAR(a.y_deg, -b.y_deg, 1e-3);
}

TEST(Tilt, ResultStaysInRange) {
    for (float tilt = 0; tilt <= kPi / 2 + 0.2f; tilt += 0.1f) {
        for (float o = -kPi; o <= kPi; o += 0.3f) {
            const auto t = tilt_orientation_to_xy(tilt, o);
            EXPECT_LE(std::abs(t.x_deg), 90.0f);
            EXPECT_LE(std::abs(t.y_deg), 90.0f);
        }
    }
}

TEST(PressureCurve, LinearIdentity) {
    PressureCurve c;
    EXPECT_FLOAT_EQ(c.apply(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(c.apply(0.3f), 0.3f);
    EXPECT_FLOAT_EQ(c.apply(1.0f), 1.0f);
}

TEST(PressureCurve, DeadzoneSaturationAndGamma) {
    PressureCurve c{0.1f, 0.9f, 2.0f};
    EXPECT_FLOAT_EQ(c.apply(0.05f), 0.0f);
    EXPECT_FLOAT_EQ(c.apply(0.95f), 1.0f);
    EXPECT_NEAR(c.apply(0.5f), 0.25f, 1e-5);  // (0.4/0.8)^2
}

TEST(MapToDesktop, FullContentCorners) {
    const RectI mon{1920, 0, 2800, 1752};
    auto tl = map_to_desktop(0, 0, {}, mon);
    auto br = map_to_desktop(1, 1, {}, mon);
    ASSERT_TRUE(tl && br);
    EXPECT_EQ(tl->x, 1920);
    EXPECT_EQ(tl->y, 0);
    EXPECT_EQ(br->x, 1920 + 2799);
    EXPECT_EQ(br->y, 1751);
}

TEST(MapToDesktop, LetterboxBarsRejectedUnlessClamped) {
    const RectF content{0, 0.1f, 1, 0.8f};
    const RectI mon{0, 0, 1000, 500};
    EXPECT_FALSE(map_to_desktop(0.5f, 0.05f, content, mon).has_value());
    auto c = map_to_desktop(0.5f, 0.05f, content, mon, true);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->y, 0);
    auto mid = map_to_desktop(0.5f, 0.5f, content, mon);
    ASSERT_TRUE(mid);
    EXPECT_EQ(mid->y, 250);  // lround(0.5 * 499) = 250
}

TEST(MapToDesktop, NegativeMonitorOrigin) {
    auto p = map_to_desktop(0, 0, {}, RectI{-2800, -200, 2800, 1752});
    ASSERT_TRUE(p);
    EXPECT_EQ(p->x, -2800);
    EXPECT_EQ(p->y, -200);
}

TEST(Letterbox, WiderSourceGetsHorizontalBars) {
    // 16:9 desktop onto a 2184x1968 (near-square) Fold inner screen.
    const auto r = letterbox(1920, 1080, 2184, 1968);
    EXPECT_FLOAT_EQ(r.w, 1.0f);
    EXPECT_NEAR(r.h, (2184.0 / 1968.0) / (16.0 / 9.0), 1e-5);
    EXPECT_NEAR(r.y, (1 - r.h) / 2, 1e-6);
}

TEST(Letterbox, TallerSourceGetsVerticalBars) {
    const auto r = letterbox(1000, 2000, 2000, 1000);
    EXPECT_FLOAT_EQ(r.h, 1.0f);
    EXPECT_NEAR(r.w, 0.25f, 1e-6);
    EXPECT_NEAR(r.x, 0.375f, 1e-6);
}

TEST(VirtualMode, EvenAndClamped) {
    EXPECT_EQ(virtual_mode_for_panel(2800, 1752), (Size{2800, 1752}));
    EXPECT_EQ(virtual_mode_for_panel(2184, 1968), (Size{2184, 1968}));
    EXPECT_EQ(virtual_mode_for_panel(1081, 2401), (Size{1080, 2400}));
    const auto big = virtual_mode_for_panel(6000, 3000);
    EXPECT_EQ(big.w, 4096u);
    EXPECT_EQ(big.h, 2048u);
    EXPECT_EQ(virtual_mode_for_panel(2800, 1752, 0.5f), (Size{1400, 876}));
}

TEST(Keymap, MainBlockIsIdentity) {
    EXPECT_EQ(evdev_to_set1(1).code, 0x01);   // Esc
    EXPECT_EQ(evdev_to_set1(30).code, 0x1E);  // A (position, not letter)
    EXPECT_EQ(evdev_to_set1(44).code, 0x2C);  // Z key position
    EXPECT_FALSE(evdev_to_set1(29).extended); // Left Ctrl
    EXPECT_EQ(evdev_to_set1(88).code, 0x58);  // F12
}

TEST(Keymap, NavigationBlockIsExtended) {
    const auto up = evdev_to_set1(103);
    EXPECT_EQ(up.code, 0x48);
    EXPECT_TRUE(up.extended);
    EXPECT_EQ(evdev_to_set1(111).code, 0x53);  // Delete
    EXPECT_TRUE(evdev_to_set1(100).extended);  // AltGr
    EXPECT_EQ(evdev_to_set1(125).code, 0x5B);  // Win
}

TEST(Keymap, UnknownIsZero) {
    EXPECT_EQ(evdev_to_set1(0).code, 0);
    EXPECT_EQ(evdev_to_set1(500).code, 0);
}
