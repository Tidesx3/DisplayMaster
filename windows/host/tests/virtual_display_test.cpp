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

TEST(MttVddXml, UnknownFormatIsLeftAlone) {
    bool changed = true;
    const std::string weird = "<vdd_settings></vdd_settings>";
    EXPECT_EQ(MttVddProvider::add_resolutions(weird, {{800, 600, 60}}, &changed), weird);
    EXPECT_FALSE(changed);
}
