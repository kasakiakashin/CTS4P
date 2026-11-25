#include "core/config.h"

#include <fstream>

#include <nlohmann/json.hpp>

#include "core/log.h"
#include "core/paths.h"

namespace cts4p {
namespace {

using nlohmann::json;

template <class T>
void read_key(const json& j, const char* key, T& out) {
    if (auto it = j.find(key); it != j.end()) {
        try {
            out = it->get<T>();
        } catch (const json::exception& e) {
            log::warn("config.json: ignoring invalid '{}' ({})", key, e.what());
        }
    }
}

json to_json(const Config& c) {
    return json{{"signaling_url", c.signaling_url},
                {"ice_servers", c.ice_servers},
                {"nickname", c.nickname},
                {"overlay_key", c.overlay_key},
                {"max_peers", c.max_peers},
                {"log_level", c.log_level},
                {"show_welcome_toast", c.show_welcome_toast},
                {"auto_sync_save", c.auto_sync_save},
                {"debug_control", c.debug_control}};
}

}  // namespace

Config Config::load_or_create(const std::filesystem::path& file) {
    Config c;
    std::ifstream in(file);
    if (!in) {
        log::info("config.json not found, writing defaults to {}", paths::to_utf8(file.wstring()));
        c.save(file);
        return c;
    }
    json j = json::parse(in, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    if (j.is_discarded() || !j.is_object()) {
        log::error("config.json is not valid JSON; using defaults (file left untouched)");
        return c;
    }
    read_key(j, "signaling_url", c.signaling_url);
    read_key(j, "ice_servers", c.ice_servers);
    read_key(j, "nickname", c.nickname);
    read_key(j, "overlay_key", c.overlay_key);
    read_key(j, "max_peers", c.max_peers);
    read_key(j, "log_level", c.log_level);
    read_key(j, "show_welcome_toast", c.show_welcome_toast);
    read_key(j, "auto_sync_save", c.auto_sync_save);
    read_key(j, "debug_control", c.debug_control);
    if (c.max_peers < 1 || c.max_peers > 15) c.max_peers = 7;
    if (c.overlay_key <= 0 || c.overlay_key > 0xFE) c.overlay_key = 0x2D;
    return c;
}

void Config::save(const std::filesystem::path& file) const {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // Preserve keys we don't know about (e.g. written by a newer version).
    json merged = json::object();
    if (std::ifstream in(file); in) {
        json existing = json::parse(in, nullptr, false, true);
        if (existing.is_object()) merged = std::move(existing);
    }
    merged.update(to_json(*this));
    std::ofstream out(file, std::ios::trunc);
    out << merged.dump(2) << '\n';
}

bool Config::signaling_is_placeholder() const {
    return signaling_url.empty() || signaling_url.find("SEU-SUBDOMINIO") != std::string::npos;
}

Config& config() {
    static Config instance;
    return instance;
}

std::filesystem::path config_path() { return paths::mod_dir() / L"config.json"; }

}  // namespace cts4p
