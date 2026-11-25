#pragma once
// Runtime configuration, persisted as <mod dir>/config.json.
// Missing keys fall back to defaults; unknown keys are preserved on save.

#include <filesystem>
#include <string>
#include <vector>

namespace cts4p {

struct Config {
    // WebSocket URL of the signaling server (Cloudflare Worker or signaling/python/server.py).
    std::string signaling_url = "ws://127.0.0.1:8787/ws";
    // ICE servers in libdatachannel URL form: "stun:host:port" or "turn:user:pass@host:port".
    std::vector<std::string> ice_servers = {"stun:stun.l.google.com:19302", "stun:stun1.l.google.com:19302"};
    std::string nickname;          // empty -> Windows user name
    int overlay_key = 0x2D;        // VK_INSERT
    int max_peers = 7;             // guests per room (host excluded)
    std::string log_level = "info";
    bool show_welcome_toast = true;
    bool auto_sync_save = true;  // host: refresh the guests' copy of the save every time the game is saved
    // Test-only local control pipe (named pipe "cts4p-debug"): status/host/leave. Off by default.
    bool debug_control = false;

    // Loads `file`; writes it with defaults when missing or unreadable.
    static Config load_or_create(const std::filesystem::path& file);
    void save(const std::filesystem::path& file) const;

    // True while signaling_url still points to the placeholder Worker host.
    bool signaling_is_placeholder() const;
};

// Process-wide config, loaded once at startup by the bootstrap.
Config& config();
std::filesystem::path config_path();

}  // namespace cts4p
