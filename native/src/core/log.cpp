#include "core/log.h"

#include <windows.h>

#include <deque>
#include <fstream>
#include <mutex>

namespace cts4p::log {
namespace {

constexpr size_t kRingSize = 500;

struct State {
    std::mutex mutex;
    std::ofstream file;
    std::deque<std::string> ring;
    Level min_level = Level::Info;
};

State& state() {
    static State s;  // intentionally function-local: usable from DllMain-spawned threads
    return s;
}

const char* tag(Level level) {
    switch (level) {
        case Level::Debug: return "DBG";
        case Level::Info: return "INF";
        case Level::Warn: return "WRN";
        case Level::Error: return "ERR";
    }
    return "???";
}

}  // namespace

void init(const std::filesystem::path& file, Level min_level) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    s.min_level = min_level;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    s.file.close();
    s.file.open(file, std::ios::out | std::ios::trunc);
    for (const auto& line : s.ring) {  // flush lines captured before init
        s.file << line << '\n';
    }
    s.file.flush();
}

void write(Level level, std::string_view message) {
    auto& s = state();
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::string line = std::format("{:02}:{:02}:{:02}.{:03} [{}] {}", t.wHour, t.wMinute, t.wSecond,
                                   t.wMilliseconds, tag(level), message);

    std::scoped_lock lock(s.mutex);
    if (level < s.min_level) {
        return;
    }
    if (s.file.is_open()) {
        s.file << line << '\n';
        s.file.flush();
    }
    s.ring.push_back(std::move(line));
    if (s.ring.size() > kRingSize) {
        s.ring.pop_front();
    }
}

std::vector<std::string> recent(size_t max_lines) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const size_t n = std::min(max_lines, s.ring.size());
    return {s.ring.end() - static_cast<std::ptrdiff_t>(n), s.ring.end()};
}

Level parse_level(std::string_view name, Level fallback) {
    if (name == "debug") return Level::Debug;
    if (name == "info") return Level::Info;
    if (name == "warn") return Level::Warn;
    if (name == "error") return Level::Error;
    return fallback;
}

}  // namespace cts4p::log
