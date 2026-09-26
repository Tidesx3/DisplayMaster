// Hands the bundled Android app to phones over plain HTTP on the engine's own port, so a
// device without a cable (or USB debugging) can install it: the DisplayMaster app shows a
// QR code for http://<pc>:<port>/DisplayMaster.apk. Only reachable when Wi-Fi is enabled.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace dm {

class Connection;

// android\DisplayMaster.apk next to the engine (installed builds).
std::filesystem::path bundled_apk_path();

constexpr const char* kApkUrlPath = "/DisplayMaster.apk";

// The protocol starts with a binary header; a browser starts with "GET ".
bool looks_like_http(const uint8_t* data, size_t n);

// Answers one HTTP request (`data` holds its start) and closes the connection.
void serve_http(Connection& conn, const uint8_t* data, size_t n);

}  // namespace dm
