// winmm.dll proxy entry point.
//
// The game (and Simulation_x64.dll) load winmm.dll from Game\Bin first, so this
// DLL is picked up automatically. Every real winmm export is forwarded to
// System32 (see proxy/winmm_exports.inc), so the game behaves exactly as
// without the mod. The cts4p runtime only starts inside TS4_x64.exe, or in a
// test process that sets CTS4P_FORCE=1 / calls cts4p_debug_start().

#include <windows.h>

#include <cwchar>
#include <string_view>

#include "core/app.h"
#include "proxy/winmm_exports.inc"

namespace {

bool is_game_process() {
    wchar_t path[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"TS4_x64.exe") == 0;
}

bool forced_by_env() {
    wchar_t buf[8]{};
    return GetEnvironmentVariableW(L"CTS4P_FORCE", buf, static_cast<DWORD>(std::size(buf))) > 0 && buf[0] == L'1';
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        // app::start() only spawns a thread; real work runs after the loader lock is released.
        if (is_game_process() || forced_by_env()) cts4p::app::start();
    }
    // No teardown on DLL_PROCESS_DETACH: the game never unloads winmm.dll, and joining
    // threads under the loader lock would deadlock. The OS reclaims everything at exit.
    return TRUE;
}
