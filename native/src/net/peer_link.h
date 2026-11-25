#pragma once
// One WebRTC connection to a remote player.
//
// Owns the rtc::PeerConnection and two DataChannels:
//   "reliable" - ordered + reliable: handshake, chat, game state
//   "fast"     - unordered, no retransmits: ping and (later) high-rate cursor/transform updates
//
// Lifecycle: New -> Connecting -> Open (channels up) -> Ready (Hello exchanged)
//            -> Closed | Failed | Incompatible
//
// Signaling payloads are opaque JSON produced/consumed here and relayed by
// whatever transport the owner uses (WebSocket server, or in-memory for the
// loopback self-test). All callbacks fire on libdatachannel worker threads.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "net/protocol.h"

namespace rtc {
class PeerConnection;
class DataChannel;
struct Configuration;
}  // namespace rtc

namespace cts4p::net {

enum class LinkState { New, Connecting, Open, Ready, Incompatible, Failed, Closed };
const char* state_name(LinkState s);

struct LocalIdentity {
    std::string name;
    std::string role;  // "host" | "guest"
    std::string game_version;
};

struct PeerInfo {
    std::string id;
    std::string name;
    std::string role;
    std::string mod_version;
    std::string game_version;
    std::string error;
    LinkState state = LinkState::New;
    bool game_version_matches = true;
    double rtt_ms = -1.0;  // smoothed; -1 until the first pong
    double rtt_min_ms = -1.0;
    double rtt_max_ms = -1.0;
    uint64_t pings_sent = 0;
    uint64_t pongs_received = 0;
    std::chrono::steady_clock::time_point created_at{};
};

struct LinkCallbacks {
    std::function<void(const nlohmann::json& signal)> send_signal;
    std::function<void(const std::string& id, LinkState state)> on_state;
    // Application messages (Chat, Roster, gameplay...) after the link is Ready.
    std::function<void(const std::string& id, const Message& msg)> on_message;
};

class PeerLink : public std::enable_shared_from_this<PeerLink> {
public:
    // `offerer` creates the DataChannels (and therefore the SDP offer); the host is always the offerer.
    static std::shared_ptr<PeerLink> create(std::string remote_id, bool offerer, const rtc::Configuration& config,
                                            LocalIdentity me, LinkCallbacks callbacks);
    ~PeerLink();
    PeerLink(const PeerLink&) = delete;
    PeerLink& operator=(const PeerLink&) = delete;

    void handle_signal(const nlohmann::json& signal);
    bool send(MsgType type, const std::string& payload, bool reliable = true);
    void ping();
    void close(const std::string& reason);

    PeerInfo info() const;
    LinkState state() const { return state_.load(); }
    // Bytes queued in SCTP on the reliable channel (used for flow control of bulk transfers).
    size_t buffered_amount() const;
    const std::string& id() const { return id_; }

private:
    struct Private {};

public:
    PeerLink(Private, std::string remote_id, LocalIdentity me, LinkCallbacks callbacks);

private:
    void setup(bool offerer, const rtc::Configuration& config);
    void attach(const std::shared_ptr<rtc::DataChannel>& dc);
    void on_channel_open();
    void on_raw(const std::shared_ptr<rtc::DataChannel>& from, std::span<const std::byte> data);
    void on_hello(const Message& m);
    void on_pong(const Message& m);
    void set_state(LinkState s, std::string error = {});
    void maybe_ready();
    bool send_on(const std::shared_ptr<rtc::DataChannel>& dc, MsgType type, std::span<const std::byte> payload);

    const std::string id_;
    const LocalIdentity me_;
    const LinkCallbacks cb_;

    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::DataChannel> reliable_;
    std::shared_ptr<rtc::DataChannel> fast_;

    std::atomic<LinkState> state_{LinkState::New};
    std::atomic<uint32_t> seq_{0};
    std::atomic<bool> hello_sent_{false};
    std::atomic<bool> hello_received_{false};
    std::atomic<bool> ack_received_{false};

    mutable std::mutex mutex_;  // guards info_ and channel pointers
    PeerInfo info_;
};

}  // namespace cts4p::net
