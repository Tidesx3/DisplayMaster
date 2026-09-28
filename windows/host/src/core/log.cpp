#include "core/log.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <mutex>

namespace dm::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
Level g_min = Level::Info;

const char* tag(Level l) {
    switch (l) {
        case Level::Debug: return "D";
        case Level::Info: return "I";
        case Level::Warn: return "W";
        case Level::Error: return "E";
    }
    return "?";
}

}  // namespace

void init(const std::wstring& file_path, Level min_level) {
    std::lock_guard lock(g_mutex);
    g_min = min_level;
    if (g_file) fclose(g_file);
    if (!file_path.empty()) {
        const std::filesystem::path path(file_path);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        // Keep the file small enough to attach to a bug report: the previous 4 MB stay as *.old.log.
        if (std::filesystem::file_size(path, ec) > 4 * 1024 * 1024 && !ec) {
            auto old = path;
            old.replace_extension(L".old.log");
            std::filesystem::rename(path, old, ec);
        }
    }
    g_file = file_path.empty() ? nullptr : _wfsopen(file_path.c_str(), L"a", _SH_DENYWR);
}

void write(Level level, const char* fmt, ...) {
    if (level < g_min) return;
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard lock(g_mutex);
    for (FILE* f : {stderr, g_file}) {
        if (!f) continue;
        fprintf(f, "%02d:%02d:%02d.%03d [%s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, tag(level), msg);
        fflush(f);
    }
}

}  // namespace dm::log
