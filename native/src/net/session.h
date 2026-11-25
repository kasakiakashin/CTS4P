#pragma once
// Room session: hosting or joining a "CTS-XXXX" room.
//
// Topology is a star: every guest has one PeerLink to the host; the host
// relays chat (and, in later phases, game state) between guests.
// Public methods are thread-safe and non-blocking; the overlay calls them
// from the render thread and reads state through snapshot().

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "net/peer_link.h"
#include "net/signaling.h"
#include "sync/manifest.h"

namespace rtc {
struct Configuration;
}

namespace cts4p::net {

enum class SessionMode { Idle, Hosting, Joining };

struct ChatLine {
    std::string from;
    std::string text;
    bool system = false;  // join/leave notices
    std::chrono::system_clock::time_point at;
};

struct RosterEntry {
    std::string name;
    std::string role;
    double rtt_ms = -1;
};

struct PeerCompat {
    std::string peer_id;
    sync::CompatReport report;
};

struct SessionSnapshot {
    SessionMode mode = SessionMode::Idle;
    std::string room;
    std::string status;  // human readable, pt-BR
    std::string error;
    bool signaling_open = false;
    std::string local_name;
    std::vector<PeerInfo> peers;      // direct links (host: all guests, guest: the host)
    std::vector<RosterEntry> roster;  // full room as last reported by the host
    std::vector<ChatLine> chat;
    std::vector<PeerCompat> compat;  // one report per direct link, once its manifest arrived
};

class Session {
public:
    static Session& instance();

    // Must be called once before anything else (bootstrap does it).
    void init(std::string local_name, std::string game_version);

    void host();
    void join(const std::string& room_code);
    void leave(const std::string& reason = "saiu da sala");
    void send_chat(const std::string& text);

    SessionSnapshot snapshot() const;
    // Short notices for overlay toasts (consumed once).
    std::vector<std::string> take_notices();

    void set_local_name(std::string name);
    void shutdown();

    // Toast shown in the overlay (thread-safe).
    void notice(std::string text);

    // Gameplay payload from Python. target "" = everyone reachable (host: all guests, guest: host).
    bool send_game(const std::string& target, uint8_t msg_type, const std::string& payload, bool reliable);
    std::vector<std::shared_ptr<PeerLink>> ready_links() const;
    // Build op frame to every ready link except `except_peer` (reliable, ordered).
    bool send_build_op(const std::vector<uint8_t>& frame, const std::string& except_peer);
    SessionMode mode() const;

private:
    Session() = default;
    void connect_signaling(const std::string& room, const std::string& role);
    void on_signal_message(uint64_t generation, const nlohmann::json& msg);
    void on_signal_closed(uint64_t generation, const std::string& reason);
    void on_link_state(const std::string& id, LinkState s);
    void on_link_message(const std::string& id, const Message& m);
    std::shared_ptr<PeerLink> make_link(const std::string& remote_id, bool offerer);
    void tick_loop(std::stop_token stop);
    void add_chat(ChatLine line);
    void reset_locked(SessionMode mode, std::string room);
    void send_manifest(const std::shared_ptr<PeerLink>& link);
    void on_manifest(const std::shared_ptr<PeerLink>& link, const Message& m);
    void post_session_event();

    mutable std::mutex mutex_;
    SessionMode mode_ = SessionMode::Idle;
    std::string room_;
    std::string status_ = "Desconectado";
    std::string error_;
    std::string my_signal_id_;
    std::string host_signal_id_;
    std::string local_name_;
    std::string game_version_;
    int host_retries_ = 0;
    uint64_t generation_ = 0;  // invalidates callbacks from a previous connection
    std::unordered_map<std::string, std::shared_ptr<PeerLink>> links_;
    std::vector<RosterEntry> roster_;
    std::deque<ChatLine> chat_;
    std::deque<std::string> notices_;
    std::unordered_map<std::string, sync::CompatReport> compat_;

    std::shared_ptr<SignalingClient> signaling_;
    std::jthread ticker_;
    bool initialized_ = false;
};

// Builds the libdatachannel configuration from config.json (ICE servers).
rtc::Configuration make_rtc_config();

// Initializes libdatachannel logging once.
void init_rtc_logging();

}  // namespace cts4p::net
