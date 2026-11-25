#include "net/room_code.h"

#include <random>

namespace cts4p::net {

std::string generate_room_code() {
    std::random_device rd;
    std::uniform_int_distribution<size_t> pick(0, kRoomAlphabet.size() - 1);
    std::string code = "CTS-";
    for (size_t i = 0; i < kRoomCodeChars; ++i) code += kRoomAlphabet[pick(rd)];
    return code;
}

std::optional<std::string> normalize_room_code(std::string_view input) {
    std::string compact;
    for (char c : input) {
        if (c == ' ' || c == '-' || c == '_') continue;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        compact += c;
    }
    // "CTS-ABCD", "cts abcd" or just "ABCD" (also "TS4-ABCD", the pre-rename format).
    if ((compact.starts_with("CTS") || compact.starts_with("TS4")) && compact.size() == 3 + kRoomCodeChars) compact.erase(0, 3);
    if (compact.size() != kRoomCodeChars) return std::nullopt;
    for (char c : compact) {
        if (kRoomAlphabet.find(c) == std::string_view::npos) return std::nullopt;
    }
    return "CTS-" + compact;
}

}  // namespace cts4p::net
