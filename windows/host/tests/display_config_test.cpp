#include <gtest/gtest.h>

#include "display/display_config.h"

using namespace dm;

namespace {
constexpr LUID kGpu{1, 0};

DISPLAYCONFIG_PATH_INFO path(UINT32 source, UINT32 source_mode, UINT32 target, UINT32 target_mode) {
    DISPLAYCONFIG_PATH_INFO p{};
    p.sourceInfo.adapterId = kGpu;
    p.sourceInfo.id = source;
    p.sourceInfo.modeInfoIdx = source_mode;
    p.targetInfo.adapterId = kGpu;
    p.targetInfo.id = target;
    p.targetInfo.modeInfoIdx = target_mode;
    p.flags = DISPLAYCONFIG_PATH_ACTIVE;
    return p;
}

DISPLAYCONFIG_MODE_INFO source_mode(UINT32 w, UINT32 h) {
    DISPLAYCONFIG_MODE_INFO m{};
    m.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    m.sourceMode.width = w;
    m.sourceMode.height = h;
    return m;
}

DISPLAYCONFIG_MODE_INFO target_mode(UINT32 w, UINT32 h, UINT32 hz) {
    DISPLAYCONFIG_MODE_INFO m{};
    m.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
    m.targetMode.targetVideoSignalInfo.activeSize = {w, h};
    m.targetMode.targetVideoSignalInfo.vSyncFreq = {hz, 1};
    return m;
}

// Built-in 2880x1800 panel (target 1) whose preferred timing is 60 Hz.
std::optional<DISPLAYCONFIG_TARGET_PREFERRED_MODE> laptop_panel(const DISPLAYCONFIG_PATH_TARGET_INFO& t) {
    if (t.id != 1) return std::nullopt;
    DISPLAYCONFIG_TARGET_PREFERRED_MODE pref{};
    pref.width = 2880;
    pref.height = 1800;
    pref.targetMode = target_mode(2880, 1800, 60).targetMode;
    return pref;
}

bool is_vdd(const DISPLAYCONFIG_PATH_INFO& p) { return p.targetInfo.id == 2; }
}  // namespace

TEST(RemovePaths, DuplicatedLaptopScreenGetsNativeResolutionBack) {
    // Windows duplicated the virtual monitor (target 2) onto the panel at a shared 1920x1080;
    // the panel kept its native 120 Hz signal and scales.
    std::vector paths{path(0, 0, 1, 1), path(0, 0, 2, 2)};
    std::vector modes{source_mode(1920, 1080), target_mode(2880, 1800, 120), target_mode(1920, 1080, 60)};
    remove_paths(paths, modes, is_vdd, laptop_panel);
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0].targetInfo.id, 1u);
    EXPECT_EQ(modes[0].sourceMode.width, 2880u);
    EXPECT_EQ(modes[0].sourceMode.height, 1800u);
    EXPECT_EQ(modes[1].targetMode.targetVideoSignalInfo.vSyncFreq.Numerator, 120u);  // refresh kept
}

TEST(RemovePaths, ReplacesASignalSwitchedToTheSharedSize) {
    std::vector paths{path(0, 0, 1, 1), path(0, 0, 2, 2)};
    std::vector modes{source_mode(1920, 1080), target_mode(1920, 1080, 60), target_mode(1920, 1080, 60)};
    remove_paths(paths, modes, is_vdd, laptop_panel);
    EXPECT_EQ(modes[1].targetMode.targetVideoSignalInfo.activeSize.cx, 2880u);
    EXPECT_EQ(modes[1].targetMode.targetVideoSignalInfo.activeSize.cy, 1800u);
}

TEST(RemovePaths, LeavesScreensAloneThatWereNotDuplicated) {
    // User runs the panel below native; the virtual monitor has its own desktop.
    std::vector paths{path(0, 0, 1, 1), path(1, 3, 2, 2)};
    std::vector modes{source_mode(1920, 1200), target_mode(2880, 1800, 120), target_mode(2184, 1968, 60),
                      source_mode(2184, 1968)};
    remove_paths(paths, modes, is_vdd, laptop_panel);
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(modes[0].sourceMode.width, 1920u);
    EXPECT_EQ(modes[0].sourceMode.height, 1200u);
}

TEST(RemovePaths, KeepsAStillDuplicatedDesktop) {
    // Panel duplicated onto a projector (target 3) as well: that pair must keep its shared size.
    std::vector paths{path(0, 0, 1, 1), path(0, 0, 2, 2), path(0, 0, 3, 3)};
    std::vector modes{source_mode(1920, 1080), target_mode(2880, 1800, 120), target_mode(1920, 1080, 60),
                      target_mode(1920, 1080, 60)};
    remove_paths(paths, modes, is_vdd, laptop_panel);
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(modes[0].sourceMode.width, 1920u);
}
