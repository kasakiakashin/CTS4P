#pragma once
// Single source of truth for versions exchanged in the network handshake.

namespace cts4p {

inline constexpr const char* kModVersion = "0.5.0";

// Bump whenever the wire format of net/protocol.h changes incompatibly.
inline constexpr unsigned kProtocolVersion = 1;

// Bump whenever the exported C API (ipc/ipc_api.h) changes incompatibly.
inline constexpr unsigned kApiVersion = 1;

}  // namespace cts4p
