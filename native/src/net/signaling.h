#pragma once
// WebSocket client for the room signaling server (signaling/cloudflare/worker.js
// or signaling/python/server.py). The server only relays small JSON messages
// between the host and guests of a room until the P2P link is up.
//
// Protocol (JSON text frames), connect to  <url>?room=CTS-XXXX&role=host|guest&name=..&proto=1
//   server -> client  {"t":"welcome","id":"<your id>","room":"CTS-XXXX","host":"<host id>|null"}
//   server -> host    {"t":"peer_joined","id":"<guest id>","name":"..."}
//   server -> all     {"t":"peer_left","id":"..."}
//   client -> server  {"t":"signal","to":"<id>","data":{...}}
//   server -> client  {"t":"signal","from":"<id>","data":{...}}
//   server -> guests  {"t":"room_closed"}
//   server -> client  {"t":"error","code":"room_taken|no_host|room_full|bad_request","message":"..."}

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace rtc {
class WebSocket;
}

namespace cts4p::net {

class SignalingClient {
public:
    struct Callbacks {
        std::function<void(const nlohmann::json& msg)> on_message;  // every server message, including welcome/error
        std::function<void(const std::string& reason)> on_closed;
    };

    SignalingClient() = default;
    ~SignalingClient();
    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    // Starts connecting asynchronously. Returns false if the URL is unusable.
    bool connect(const std::string& base_url, const std::string& room, const std::string& role, const std::string& name,
                 Callbacks callbacks);
    bool send(const nlohmann::json& msg);
    void close();
    bool is_open() const;

    static std::string url_encode(const std::string& s);

private:
    std::shared_ptr<rtc::WebSocket> ws_;
    mutable std::mutex mutex_;
};

}  // namespace cts4p::net
