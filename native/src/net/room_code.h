#pragma once
// Short human-friendly room codes: "CTS-XXXX".
// Alphabet omits look-alikes (I, O, 0, 1) so codes survive being read aloud.

#include <optional>
#include <string>
#include <string_view>

namespace cts4p::net {

inline constexpr std::string_view kRoomAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
inline constexpr size_t kRoomCodeChars = 4;

std::string generate_room_code();

// Accepts "cts-abcd", "ABCD", " CTS ABCD " ... and returns "CTS-ABCD", or nullopt if invalid.
std::optional<std::string> normalize_room_code(std::string_view input);

}  // namespace cts4p::net
