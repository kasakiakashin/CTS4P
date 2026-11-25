#include "net/protocol.h"

#include <cstring>

#include "core/version.h"

namespace cts4p::net {

std::string Message::payload_string() const {
    return {reinterpret_cast<const char*>(payload.data()), payload.size()};
}

std::vector<std::byte> encode(MsgType type, uint32_t seq, std::span<const std::byte> payload) {
    std::vector<std::byte> out(kHeaderSize + payload.size());
    out[0] = std::byte{'T'};
    out[1] = std::byte{'4'};
    out[2] = static_cast<std::byte>(kProtocolVersion);
    out[3] = static_cast<std::byte>(type);
    for (int i = 0; i < 4; ++i) out[4 + static_cast<size_t>(i)] = static_cast<std::byte>((seq >> (8 * i)) & 0xFF);
    if (!payload.empty()) std::memcpy(out.data() + kHeaderSize, payload.data(), payload.size());
    return out;
}

std::vector<std::byte> encode(MsgType type, uint32_t seq, const std::string& payload) {
    return encode(type, seq, std::as_bytes(std::span(payload.data(), payload.size())));
}

std::optional<Message> decode(std::span<const std::byte> data) {
    if (data.size() < kHeaderSize || data.size() > kMaxMessageSize) return std::nullopt;
    if (data[0] != std::byte{'T'} || data[1] != std::byte{'4'}) return std::nullopt;
    if (static_cast<unsigned>(data[2]) != kProtocolVersion) return std::nullopt;
    Message m;
    m.type = static_cast<MsgType>(data[3]);
    for (int i = 0; i < 4; ++i) m.seq |= static_cast<uint32_t>(data[4 + static_cast<size_t>(i)]) << (8 * i);
    m.payload.assign(data.begin() + kHeaderSize, data.end());
    return m;
}

const char* type_name(MsgType type) {
    switch (type) {
        case MsgType::Hello: return "Hello";
        case MsgType::HelloAck: return "HelloAck";
        case MsgType::Ping: return "Ping";
        case MsgType::Pong: return "Pong";
        case MsgType::Chat: return "Chat";
        case MsgType::Roster: return "Roster";
        case MsgType::Bye: return "Bye";
    }
    return "Unknown";
}

}  // namespace cts4p::net
