#include <gtest/gtest.h>

#include "core/json.h"

using namespace dm;

TEST(JsonWriter, NestedObjectsAndArrays) {
    json::Writer w;
    w.begin_object()
        .field("name", "Tab \"S7+\"")
        .field("fps", 120)
        .field("usb", true)
        .key("sessions")
        .begin_array()
        .begin_object()
        .field("id", 1)
        .end_object()
        .begin_object()
        .field("id", 2)
        .end_object()
        .end_array()
        .field("mbps", 42.5)
        .end_object();
    const std::string expected =
        "{\"name\":\"Tab \\\"S7+\\\"\",\"fps\":120,\"usb\":true,"
        "\"sessions\":[{\"id\":1},{\"id\":2}],\"mbps\":42.50}";
    EXPECT_EQ(w.str(), expected);
}

TEST(JsonWriter, EmptyContainersAndEscapes) {
    json::Writer w;
    w.begin_object().key("a").begin_array().end_array().field("s", "line\nnext\\").end_object();
    EXPECT_EQ(w.str(), R"({"a":[],"s":"line\nnext\\"})");
}

TEST(JsonRead, FlatFields) {
    const std::string doc = R"({ "cmd" : "disconnect", "id": 42, "enabled":false, "neg":-3, "q":"a\"b" })";
    EXPECT_EQ(json::get_string(doc, "cmd").value_or(""), "disconnect");
    EXPECT_EQ(json::get_int(doc, "id").value_or(0), 42);
    EXPECT_EQ(json::get_int(doc, "neg").value_or(0), -3);
    EXPECT_EQ(json::get_bool(doc, "enabled"), std::optional<bool>(false));
    EXPECT_EQ(json::get_string(doc, "q").value_or(""), "a\"b");
    EXPECT_FALSE(json::get_string(doc, "missing").has_value());
    EXPECT_FALSE(json::get_int(doc, "cmd").has_value());  // wrong type
}

TEST(JsonRead, KeyTextInsideValueIsNotAKey) {
    const std::string doc = R"({"note":"\"id\" is here","id":7})";
    EXPECT_EQ(json::get_int(doc, "id").value_or(0), 7);
}

TEST(JsonRead, Numbers) {
    const std::string doc = R"({"min":0.05,"max":1,"gamma":1.8e0,"neg":-0.5,"bad":"x"})";
    EXPECT_DOUBLE_EQ(json::get_number(doc, "min").value_or(-1), 0.05);
    EXPECT_DOUBLE_EQ(json::get_number(doc, "max").value_or(-1), 1.0);
    EXPECT_DOUBLE_EQ(json::get_number(doc, "gamma").value_or(-1), 1.8);
    EXPECT_DOUBLE_EQ(json::get_number(doc, "neg").value_or(0), -0.5);
    EXPECT_FALSE(json::get_number(doc, "bad").has_value());
}
