#include "core/paths.h"

#include <windows.h>
#include <shlobj.h>

#include <system_error>

namespace cts4p::paths {
namespace fs = std::filesystem;

std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring from_utf8(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::optional<std::wstring> env(const wchar_t* name) {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return std::nullopt;
    return std::wstring(buf, n);
}

fs::path game_bin_dir() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
}

fs::path game_root_dir() {
    // Game\Bin -> Game -> root
    return game_bin_dir().parent_path().parent_path();
}

std::optional<fs::path> user_dir() {
    if (auto overridden = env(L"CTS4P_USER_DIR")) return fs::path(*overridden);  // tests only
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return std::nullopt;
    const fs::path ea = fs::path(docs) / L"Electronic Arts";
    CoTaskMemFree(docs);

    std::error_code ec;
    const fs::path standard = ea / L"The Sims 4";
    if (fs::exists(standard / L"Options.ini", ec)) return standard;

    // Localized installs may use a different folder name: pick the one with Options.ini.
    for (const auto& entry : fs::directory_iterator(ea, ec)) {
        if (entry.is_directory(ec) && fs::exists(entry.path() / L"Options.ini", ec)) return entry.path();
    }
    if (fs::exists(standard, ec)) return standard;
    return std::nullopt;
}

std::optional<fs::path> saves_dir() {
    if (auto user = user_dir()) return *user / L"saves";
    return std::nullopt;
}

std::optional<fs::path> tray_dir() {
    if (auto user = user_dir()) return *user / L"Tray";
    return std::nullopt;
}

fs::path mod_dir() {
    if (auto overridden = env(L"CTS4P_MOD_DIR")) return fs::path(*overridden);
    if (auto user = user_dir()) return *user / L"Mods" / L"cts4p";
    return game_bin_dir() / L"cts4p";  // last resort: next to the exe
}

}  // namespace cts4p::paths
