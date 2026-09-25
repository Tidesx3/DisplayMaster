#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "session/approval.h"

using namespace dm;
namespace fs = std::filesystem;

namespace {
fs::path temp_store() {
    auto p = fs::temp_directory_path() / "dm_approval_test" / "trusted.txt";
    fs::remove_all(p.parent_path());
    return p;
}
const auto kAlwaysConnected = [] { return true; };
}  // namespace

TEST(Approval, AllowAndRememberPersists) {
    const auto store = temp_store();
    {
        ApprovalBroker b(store);
        EXPECT_FALSE(b.is_trusted("dev-1"));
        std::thread ui([&] {
            while (b.pending().empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            EXPECT_EQ(b.pending()[0].name, "Fold7");
            EXPECT_TRUE(b.decide(b.pending()[0].session_id, true, true));
        });
        EXPECT_TRUE(b.request({7, "dev-1", "Fold7", "SM-F966", "10.0.0.5:5000"}, 5000, kAlwaysConnected));
        ui.join();
        EXPECT_TRUE(b.is_trusted("dev-1"));
        EXPECT_TRUE(b.pending().empty());
    }
    ApprovalBroker reloaded(store);  // survives restarts
    EXPECT_TRUE(reloaded.is_trusted("dev-1"));
    reloaded.forget("dev-1");
    EXPECT_FALSE(ApprovalBroker(store).is_trusted("dev-1"));
}

TEST(Approval, DenyAndAllowOnceAreNotRemembered) {
    ApprovalBroker b(temp_store());
    std::thread deny([&] {
        while (b.pending().empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        b.decide(1, false, true);
    });
    EXPECT_FALSE(b.request({1, "dev-2", "Tab", "", ""}, 5000, kAlwaysConnected));
    deny.join();
    EXPECT_FALSE(b.is_trusted("dev-2"));

    std::thread once([&] {
        while (b.pending().empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        b.decide(2, true, false);
    });
    EXPECT_TRUE(b.request({2, "dev-2", "Tab", "", ""}, 5000, kAlwaysConnected));
    once.join();
    EXPECT_FALSE(b.is_trusted("dev-2"));
}

TEST(Approval, TimesOutAndStopsWhenDeviceLeaves) {
    ApprovalBroker b(temp_store());
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(b.request({3, "dev-3", "X", "", ""}, 300, kAlwaysConnected));
    EXPECT_GE(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(290));

    // Device disconnects: the wait ends well before the 60 s timeout.
    const auto t1 = std::chrono::steady_clock::now();
    EXPECT_FALSE(b.request({4, "dev-4", "Y", "", ""}, 60000, [] { return false; }));
    EXPECT_LT(std::chrono::steady_clock::now() - t1, std::chrono::seconds(2));
    EXPECT_TRUE(b.pending().empty());
    EXPECT_FALSE(b.decide(4, true, true));  // request is gone
}
