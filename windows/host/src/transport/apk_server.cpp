#include "transport/apk_server.h"

#include <windows.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "core/log.h"
#include "transport/connection.h"

namespace dm {
namespace fs = std::filesystem;

namespace {

bool send_text(Connection& conn, const std::string& s) {
    return conn.send(std::vector<uint8_t>(s.begin(), s.end()));
}

void respond(Connection& conn, const char* status, const std::string& body) {
    send_text(conn, std::string("HTTP/1.1 ") + status +
                        "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
}

}  // namespace

fs::path bundled_apk_path() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path() / L"android" / L"DisplayMaster.apk";
}

bool looks_like_http(const uint8_t* data, size_t n) {
    return (n >= 4 && std::memcmp(data, "GET ", 4) == 0) || (n >= 5 && std::memcmp(data, "HEAD ", 5) == 0);
}

void serve_http(Connection& conn, const uint8_t* data, size_t n) {
    // Request line: METHOD SP PATH SP VERSION. Nothing else of the request matters.
    const std::string head(reinterpret_cast<const char*>(data), n);
    const bool is_head = head.rfind("HEAD ", 0) == 0;
    const size_t start = head.find(' ') + 1;
    const size_t end = head.find(' ', start);
    const std::string path = end == std::string::npos ? "" : head.substr(start, end - start);

    if (path != kApkUrlPath) {
        respond(conn, "404 Not Found", "DisplayMaster: scan the QR code in the DisplayMaster app on the PC.\n");
        conn.close_gracefully();
        return;
    }
    const fs::path apk = bundled_apk_path();
    std::ifstream f(apk, std::ios::binary);
    std::error_code ec;
    const auto size = fs::file_size(apk, ec);
    if (!f || ec) {
        respond(conn, "404 Not Found", "The Android app isn't bundled with this installation.\n");
        conn.close_gracefully();
        return;
    }
    DM_LOGI("Sending the Android app to %s", conn.peer().c_str());
    send_text(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.android.package-archive\r\n"
                    "Content-Disposition: attachment; filename=\"DisplayMaster.apk\"\r\n"
                    "Content-Length: " + std::to_string(size) + "\r\nConnection: close\r\n\r\n");
    if (!is_head) {
        std::vector<uint8_t> buf(256 * 1024);
        while (f) {
            f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
            const auto got = static_cast<size_t>(f.gcount());
            if (!got) break;
            buf.resize(got);
            if (!conn.send(buf)) break;
            buf.resize(256 * 1024);
        }
    }
    conn.close_gracefully();
}

}  // namespace dm
