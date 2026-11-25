#pragma once
// Locates the game install and the user's "The Sims 4" documents folder.

#include <filesystem>
#include <optional>
#include <string>

namespace cts4p::paths {

// Folder containing the host executable (…\The Sims 4\Game\Bin).
std::filesystem::path game_bin_dir();

// Game install root (…\The Sims 4), derived from game_bin_dir().
std::filesystem::path game_root_dir();

// …\Documents\Electronic Arts\The Sims 4 (folder holding Options.ini), if found.
// CTS4P_USER_DIR overrides it (test harnesses simulate two different players).
std::optional<std::filesystem::path> user_dir();

// <user_dir>\saves (Slot_XXXXXXXX.save files).
std::optional<std::filesystem::path> saves_dir();

// <user_dir>\Tray (the in-game Library: households, lots, rooms).
std::optional<std::filesystem::path> tray_dir();

// Folder for our config/logs: <user_dir>\Mods\cts4p, or CTS4P_MOD_DIR when set
// (used by the test harnesses so they never touch the real Mods folder).
std::filesystem::path mod_dir();

// UTF-8 helpers for Win32 wide strings.
std::string to_utf8(const std::wstring& wide);
std::wstring from_utf8(const std::string& utf8);

// Value of an environment variable, if set and non-empty.
std::optional<std::wstring> env(const wchar_t* name);

}  // namespace cts4p::paths
