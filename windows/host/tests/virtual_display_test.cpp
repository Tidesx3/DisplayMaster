#include <gtest/gtest.h>

#include "display/virtual_display.h"

using namespace dm;

namespace {
const char* kXml = R"(<vdd_settings>
    <monitors>
        <count>1</count>
    </monitors>
    <resolutions>
        <resolution>
            <width>1920</width>
            <height>1080</height>
            <refresh_rate>60</refresh_rate>
        </resolution>
    </resolutions>
</vdd_settings>)";
}

TEST(MttVddXml, ParseCount) {
    EXPECT_EQ(MttVddProvider::parse_count(kXml), 1u);
    EXPECT_EQ(MttVddProvider::parse_count("<monitors><count> 4 </count></monitors>"), 4u);
    EXPECT_EQ(MttVddProvider::parse_count("<vdd_settings/>"), 1u);  // driver default
}

TEST(MttVddXml, SetCount) {
    const auto out = MttVddProvider::set_count(kXml, 12);
    EXPECT_EQ(MttVddProvider::parse_count(out), 12u);
    EXPECT_NE(out.find("<resolutions>"), std::string::npos);
    EXPECT_EQ(MttVddProvider::set_count("<vdd_settings/>", 3), "<vdd_settings/>");
}

TEST(MttVddXml, AddsOnlyMissingResolutions) {
    bool changed = false;
    auto out = MttVddProvider::add_resolutions(kXml, {{1920, 1080, 60}}, &changed);
    EXPECT_FALSE(changed);
    EXPECT_EQ(out, kXml);

    out = MttVddProvider::add_resolutions(kXml, {{2800, 1752, 120}, {1752, 2800, 120}}, &changed);
    EXPECT_TRUE(changed);
    EXPECT_NE(out.find("<width>2800</width>"), std::string::npos);
    EXPECT_NE(out.find("<width>1752</width>"), std::string::npos);
    EXPECT_NE(out.find("<refresh_rate>120</refresh_rate>"), std::string::npos);
    // Idempotent.
    bool again = true;
    const auto out2 = MttVddProvider::add_resolutions(out, {{2800, 1752, 120}}, &again);
    EXPECT_FALSE(again);
    EXPECT_EQ(out2, out);
    // Still well-formed: new entries land inside <resolutions>.
    EXPECT_LT(out.find("<width>2800</width>"), out.find("</resolutions>"));
}

TEST(MttVddXml, SetResolutionsDropsOldModesKeepsStock) {
    bool changed = false;
    // Grow the list the way 0.5.0 did, then trim it to what's needed now.
    auto grown = MttVddProvider::add_resolutions(kXml, {{800, 600, 30}, {1600, 1000, 60}, {1000, 1600, 60}}, &changed);
    grown = MttVddProvider::add_resolutions(grown, {{2560, 1600, 120}, {1600, 2560, 120}}, &changed);
    EXPECT_EQ(MttVddProvider::count_resolutions(grown), 6u);

    const auto out = MttVddProvider::set_resolutions(grown, {{2560, 1600, 120}, {1600, 2560, 120}}, &changed);
    EXPECT_TRUE(changed);
    EXPECT_EQ(MttVddProvider::count_resolutions(out), 3u);  // 800x600 stock + the two wanted
    EXPECT_NE(out.find("<width>800</width>"), std::string::npos);
    EXPECT_NE(out.find("<width>2560</width>"), std::string::npos);
    EXPECT_EQ(out.find("<width>1600</width>\n            <height>1000"), std::string::npos);
    EXPECT_EQ(out.find("1920"), std::string::npos);  // 1920x1080@60 isn't stock (stock is @30)
    EXPECT_LT(out.find("<width>2560</width>"), out.find("</resolutions>"));
    EXPECT_EQ(MttVddProvider::parse_count(out), 1u);

    // Nothing to do the second time.
    bool again = true;
    EXPECT_EQ(MttVddProvider::set_resolutions(out, {{1600, 2560, 120}, {2560, 1600, 120}}, &again), out);
    EXPECT_FALSE(again);

    // New mode: added, the old ones dropped.
    const auto next = MttVddProvider::set_resolutions(out, {{1080, 2400, 60}}, &changed);
    EXPECT_TRUE(changed);
    EXPECT_EQ(MttVddProvider::count_resolutions(next), 2u);
    EXPECT_NE(next.find("<width>1080</width>"), std::string::npos);
}

TEST(MttVddXml, UnknownFormatIsLeftAlone) {
    bool changed = true;
    const std::string weird = "<vdd_settings></vdd_settings>";
    EXPECT_EQ(MttVddProvider::add_resolutions(weird, {{800, 600, 60}}, &changed), weird);
    EXPECT_FALSE(changed);
}

namespace {
MonitorInfo mon(const wchar_t* gdi, int32_t x, int32_t y, int32_t w, int32_t h, bool primary = false) {
    MonitorInfo m;
    m.gdi_name = gdi;
    m.rect = {x, y, w, h};
    m.active = true;
    m.primary = primary;
    return m;
}
}  // namespace

TEST(Placement, NextToTheDesktop) {
    // The dev PC: a 1920x1080 monitor left of the 1920x1080 primary, plus the device's screen.
    const std::vector<MonitorInfo> mons = {mon(L"DISPLAY1", -1920, 0, 1920, 1080),
                                           mon(L"DISPLAY2", 0, 0, 1920, 1080, true),
                                           mon(L"DISPLAY9", 5000, 0, 800, 1280)};
    const auto self = L"DISPLAY9";  // the monitor being placed doesn't count
    auto at = [&](Placement p) { return position_for(p, mons, self, 800, 1280); };
    EXPECT_EQ(at(Placement::Right).x, 1920);
    EXPECT_EQ(at(Placement::Right).y, 0);
    EXPECT_EQ(at(Placement::Left).x, -1920 - 800);
    EXPECT_EQ(at(Placement::Left).y, 0);
    EXPECT_EQ(at(Placement::Above).x, (1920 - 800) / 2);  // centred on the primary
    EXPECT_EQ(at(Placement::Above).y, -1280);
    EXPECT_EQ(at(Placement::Below).x, (1920 - 800) / 2);
    EXPECT_EQ(at(Placement::Below).y, 1080);
}

TEST(Placement, Names) {
    for (auto p : {Placement::Right, Placement::Left, Placement::Above, Placement::Below})
        EXPECT_EQ(placement_from_name(placement_name(p)), p);
    EXPECT_FALSE(placement_from_name("sideways"));
}
