// Minimal thread-safe logger: stderr + optional file, printf-style.
#pragma once

#include <cstdarg>
#include <string>

namespace dm::log {

enum class Level { Debug, Info, Warn, Error };

void init(const std::wstring& file_path, Level min_level);
void write(Level level, const char* fmt, ...);

}  // namespace dm::log

#define DM_LOGD(...) ::dm::log::write(::dm::log::Level::Debug, __VA_ARGS__)
#define DM_LOGI(...) ::dm::log::write(::dm::log::Level::Info, __VA_ARGS__)
#define DM_LOGW(...) ::dm::log::write(::dm::log::Level::Warn, __VA_ARGS__)
#define DM_LOGE(...) ::dm::log::write(::dm::log::Level::Error, __VA_ARGS__)
