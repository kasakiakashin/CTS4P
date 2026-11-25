#pragma once
// The game's Library ("Biblioteca") lives in <user_dir>\Tray. A saved household
// is a small group of files sharing one instance id:
//
//   0x00000001!0x<id>.trayitem          metadata (protobuf after an 8-byte header; field 4 = name)
//   0x00000000!0x<id>.householdbinary   the household and its Sims
//   0x<grp>!0x<id>.hhi                  household thumbnails
//   0x<grp>!0x<id+n>.sgi                one portrait per Sim (instance = id + 1, id + 2, ...)
//
// "Criar juntos": each player creates their Sim in their own Create a Sim, saves it
// to the Library, and cts4p copies those files into the other player's Library.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cts4p::sync::tray {

struct Household {
    uint64_t id = 0;                              // instance id of the .trayitem
    std::string name;                             // family name shown in the Library ("" if unreadable)
    int64_t modified_unix = 0;                    // last write of the .trayitem (seconds since 1970)
    std::vector<std::filesystem::path> files;     // every file of the group (full paths)
    uint64_t total_bytes = 0;
};

constexpr size_t kMaxFiles = 64;
constexpr uint64_t kMaxBytes = 16ull << 20;  // 16 MiB per household is far above real ones (~100 KB)

// Households saved in `dir`, newest first.
std::vector<Household> list_households(const std::filesystem::path& dir);

// File name accepted inside the Tray folder: "0x<8 hex>!0x<16 hex>.<trayitem|householdbinary|hhi|sgi>".
bool valid_file_name(const std::string& name);

// Reads the family name out of a .trayitem's bytes (protobuf field 4).
std::string read_name(std::span<const std::byte> trayitem);

// Packs files into one blob: "T4TR" | u32 count | {u16 name_len, name, u64 size, bytes}...
std::optional<std::vector<std::byte>> pack(const std::vector<std::filesystem::path>& files, std::string& error);

struct Unpacked {
    std::vector<std::filesystem::path> written;
    std::string name;  // family name, from the packed .trayitem
};
// Validates the blob completely, then writes its files into `dir` (atomic per file).
std::optional<Unpacked> unpack_into(std::span<const std::byte> blob, const std::filesystem::path& dir, std::string& error);

}  // namespace cts4p::sync::tray
