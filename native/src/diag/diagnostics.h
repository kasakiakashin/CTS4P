#pragma once
// Local environment facts shown in the "Diagnóstico" tab and sent in the
// Phase 3 handshake (packs + mods comparison).

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cts4p::diag {

struct ModFile {
    std::string relative_path;  // relative to Mods/, '/' separators, lower-case
    unsigned long long size = 0;
    unsigned long long fingerprint = 0;  // FNV-1a 64 over size + first/last 64 KiB (fast, content-sensitive)
    bool is_script = false;              // .ts4script
};

// Fingerprint used to tell whether two players have the same version of a mod file.
unsigned long long fingerprint_file(const std::filesystem::path& file, unsigned long long size);

struct Environment {
    std::string game_version;   // Default.ini [Version] gameversion
    std::string game_root;
    std::string user_dir;
    std::string mod_dir;
    std::vector<std::string> packs;  // EP01, GP05, SP12, FP01 ...
    std::vector<ModFile> mods;
    bool script_mods_enabled = false;
    bool mods_enabled = false;
};

// Scans the disk; takes a few ms to a few hundred ms depending on Mods size.
Environment scan();

// Immutable snapshot from the last refresh_cache() (never null; empty before the first scan).
std::shared_ptr<const Environment> cached();
void refresh_cache();

}  // namespace cts4p::diag
