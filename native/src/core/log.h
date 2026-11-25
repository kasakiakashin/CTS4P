#pragma once
// Thread-safe logger: appends to <mod dir>/cts4p_native.log and keeps the most
// recent lines in memory so the overlay's Diagnostics tab can show them.

#include <format>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cts4p::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Opens (or re-opens) the log file. Safe to call before any other function;
// lines logged before init are kept in memory only.
void init(const std::filesystem::path& file, Level min_level);

void write(Level level, std::string_view message);

// Most recent lines (oldest first), at most `max_lines`.
std::vector<std::string> recent(size_t max_lines = 200);

Level parse_level(std::string_view name, Level fallback);

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace cts4p::log
