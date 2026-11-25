#include "diag/diagnostics.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <regex>

#include "core/paths.h"

namespace cts4p::diag {
namespace fs = std::filesystem;
namespace {

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    const auto e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
}

// Returns the value of `key` from an INI-ish file ("key = value"), case-insensitive key.
std::string ini_value(const fs::path& file, const std::string& key) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (k == key) {
            std::string v = line.substr(eq + 1);
            if (auto hash = v.find('#'); hash != std::string::npos) v.resize(hash);
            return trim(v);
        }
    }
    return {};
}

std::mutex g_mutex;
std::shared_ptr<const Environment> g_cache = std::make_shared<const Environment>();

constexpr unsigned long long kFnvOffset = 1469598103934665603ULL;
constexpr unsigned long long kFnvPrime = 1099511628211ULL;

unsigned long long fnv1a(unsigned long long h, const char* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= kFnvPrime;
    }
    return h;
}

}  // namespace

unsigned long long fingerprint_file(const fs::path& file, unsigned long long size) {
    constexpr size_t kWindow = 64 * 1024;
    unsigned long long h = fnv1a(kFnvOffset, reinterpret_cast<const char*>(&size), sizeof(size));
    std::ifstream in(file, std::ios::binary);
    if (!in) return h;
    std::string buf(kWindow, '\0');
    in.read(buf.data(), static_cast<std::streamsize>(kWindow));
    h = fnv1a(h, buf.data(), static_cast<size_t>(in.gcount()));
    if (size > kWindow * 2) {
        in.clear();
        in.seekg(-static_cast<std::streamoff>(kWindow), std::ios::end);
        in.read(buf.data(), static_cast<std::streamsize>(kWindow));
        h = fnv1a(h, buf.data(), static_cast<size_t>(in.gcount()));
    }
    return h;
}

Environment scan() {
    Environment env;
    std::error_code ec;
    const fs::path root = paths::game_root_dir();
    env.game_root = paths::to_utf8(root.wstring());
    env.game_version = ini_value(root / L"Game" / L"Bin" / L"Default.ini", "gameversion");
    if (env.game_version.empty()) env.game_version = "desconhecida";

    const std::regex pack_re("^(EP|GP|SP|FP)\\d{2}$");
    for (const auto& e : fs::directory_iterator(root, ec)) {
        const std::string name = paths::to_utf8(e.path().filename().wstring());
        if (e.is_directory(ec) && std::regex_match(name, pack_re)) env.packs.push_back(name);
    }
    std::sort(env.packs.begin(), env.packs.end());

    env.mod_dir = paths::to_utf8(paths::mod_dir().wstring());
    if (auto user = paths::user_dir()) {
        env.user_dir = paths::to_utf8(user->wstring());
        const fs::path options = *user / L"Options.ini";
        env.script_mods_enabled = ini_value(options, "scriptmodsenabled") == "1";
        env.mods_enabled = ini_value(options, "modsdisabled") == "0";

        // The game reads packages up to 5 folders deep and scripts 1 folder deep;
        // we list both up to depth 5 so mismatches are visible.
        const fs::path mods = *user / L"Mods";
        fs::recursive_directory_iterator it(mods, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            if (it.depth() > 5) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            std::wstring ext = it->path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext != L".package" && ext != L".ts4script") continue;
            std::wstring rel = fs::relative(it->path(), mods, ec).wstring();
            std::replace(rel.begin(), rel.end(), L'\\', L'/');
            std::transform(rel.begin(), rel.end(), rel.begin(), ::towlower);
            const auto size = it->file_size(ec);
            env.mods.push_back({paths::to_utf8(rel), size, fingerprint_file(it->path(), size), ext == L".ts4script"});
        }
        std::sort(env.mods.begin(), env.mods.end(), [](const ModFile& a, const ModFile& b) { return a.relative_path < b.relative_path; });
    }
    return env;
}

std::shared_ptr<const Environment> cached() {
    std::scoped_lock lock(g_mutex);
    return g_cache;
}

void refresh_cache() {
    auto env = std::make_shared<const Environment>(scan());
    std::scoped_lock lock(g_mutex);
    g_cache = std::move(env);
}

}  // namespace cts4p::diag
