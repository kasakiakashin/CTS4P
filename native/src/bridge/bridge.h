#pragma once
// Native end of the Python <-> native bridge (in-process, lock-free rings in
// ipc/ring_buffer.h, exposed to Python through ctypes in ipc/ipc_api.h).
//
// Frame layout (first byte = kind):
//   Python -> native
//     0x01 NET_SEND  u8 flags(bit0 = reliable) | u8 msg_type(32..127) | u8 target_len | target (utf-8) | payload
//                    target "" = everyone (host: all guests, guest: the host)
//     0x02 REQUEST   UTF-8 JSON {"op": ...}
//   native -> Python
//     0x81 NET_RECV  u8 msg_type | u8 from_len | from (utf-8) | payload
//     0x82 EVENT     UTF-8 JSON {"ev": ...}
//
// A dedicated thread drains Python's frames (woken by cts4p_ipc_send through
// an event object, so there is no polling latency on the native side).

#include <cstdint>
#include <span>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace cts4p::bridge {

enum Kind : uint8_t { NetSend = 0x01, Request = 0x02, NetRecv = 0x81, Event = 0x82 };

void start();
void notify_python_frame();  // called by cts4p_ipc_send

// native -> Python (thread-safe; drops and counts frames if Python stopped reading).
void post_event(const nlohmann::json& event);
void post_net_recv(const std::string& from, uint8_t msg_type, std::span<const std::byte> payload);

struct Status {
    bool python_attached = false;
    std::string python_version;
    std::string python_mod_version;
    uint64_t frames_from_python = 0;
    uint64_t frames_to_python = 0;
    uint64_t dropped_to_python = 0;
};
Status status();

}  // namespace cts4p::bridge
