#pragma once
// Install manifest exchanged right after the handshake: game version, installed
// packs (EP/GP/SP/FP) and every .package/.ts4script under Mods with a content
// fingerprint. Both sides compare and show what the other player is missing.
//
// Sent as zstd-compressed JSON (thousands of CC files compress ~10x).

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace cts4p::diag {
struct Environment;
}

namespace cts4p::sync {

struct ManifestMod {
    std::string path;  // relative to Mods, lower-case, '/' separators
    unsigned long long size = 0;
    unsigned long long fingerprint = 0;
};

struct Manifest {
    std::string game_version;
    std::vector<std::string> packs;
    std::vector<ManifestMod> mods;  // sorted by path
};

struct CompatReport {
    bool received = false;
    std::string remote_name;
    std::string local_game_version;
    std::string remote_game_version;
    std::vector<std::string> packs_missing_here;   // remote has, we don't
    std::vector<std::string> packs_missing_there;  // we have, remote doesn't
    std::vector<std::string> mods_missing_here;
    std::vector<std::string> mods_missing_there;
    std::vector<std::string> mods_different;       // same path, different content

    bool game_version_matches() const { return local_game_version == remote_game_version; }
    bool packs_match() const { return packs_missing_here.empty() && packs_missing_there.empty(); }
    bool mods_match() const { return mods_missing_here.empty() && mods_missing_there.empty() && mods_different.empty(); }
    bool fully_compatible() const { return game_version_matches() && packs_match() && mods_match(); }
    std::string summary() const;  // one line, pt-BR
};

Manifest from_environment(const diag::Environment& env);

// Wire encoding: zstd(JSON). decode() returns nullopt for corrupt/oversized input.
std::vector<std::byte> encode(const Manifest& m);
std::optional<Manifest> decode(const std::byte* data, size_t size);

CompatReport compare(const Manifest& local, const Manifest& remote, std::string remote_name);

}  // namespace cts4p::sync
