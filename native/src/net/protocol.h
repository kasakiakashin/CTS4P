#pragma once
// Wire format for messages exchanged over the WebRTC DataChannels.
//
//   offset size field
//   0      2    magic   'T','4'
//   2      1    version kProtocolVersion
//   3      1    type    MsgType
//   4      4    seq     per-link sequence number (little endian)
//   8      n    payload type-specific (JSON for control messages)
//
// SCTP preserves message boundaries, so no length field is needed.
// No pickle/eval anywhere: payloads are parsed as plain JSON or raw bytes.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cts4p::net {

enum class MsgType : uint8_t {
    Hello = 1,     // JSON: {proto, mod, game, name, role}
    HelloAck = 2,  // JSON: {ok, reason}
    Ping = 3,      // 8 bytes: sender timestamp in microseconds
    Pong = 4,      // echo of the Ping payload
    Chat = 5,      // JSON: {from, text}
    Roster = 6,    // JSON: [{name, rtt_ms}] host -> guests
    Bye = 7,       // JSON: {reason}

    // 16..31: session services handled natively (Phase 3)
    Manifest = 16,    // zstd(JSON): game version, packs, mod fingerprints
    SaveBegin = 17,   // JSON: {xfer, slot_id, label, raw_size, comp_size, hash, chunks}
    SaveChunk = 18,   // u32 xfer, u32 index, bytes
    SaveEnd = 19,     // JSON: {xfer}
    SaveResult = 20,  // JSON: {xfer, ok, error}

    // 32..127: gameplay sync - opaque payloads produced/consumed by the Python scripts.
    GameplayFirst = 32,
    GameplayLast = 127,
    // 128..255: build sync (Phase 4), handled natively.
    BuildOp = 128,  // raw RequestBuildBuyOp frame: u32 id | u32 channel | u32 len | payload
};

constexpr bool is_gameplay(MsgType t) {
    return static_cast<uint8_t>(t) >= static_cast<uint8_t>(MsgType::GameplayFirst) &&
           static_cast<uint8_t>(t) <= static_cast<uint8_t>(MsgType::GameplayLast);
}

constexpr size_t kHeaderSize = 8;
constexpr size_t kMaxMessageSize = 256 * 1024;

struct Message {
    MsgType type{};
    uint32_t seq = 0;
    std::vector<std::byte> payload;

    std::string payload_string() const;
};

std::vector<std::byte> encode(MsgType type, uint32_t seq, std::span<const std::byte> payload);
std::vector<std::byte> encode(MsgType type, uint32_t seq, const std::string& payload);

// Returns nullopt for anything malformed (wrong magic/version, oversized, truncated).
std::optional<Message> decode(std::span<const std::byte> data);

const char* type_name(MsgType type);

}  // namespace cts4p::net
