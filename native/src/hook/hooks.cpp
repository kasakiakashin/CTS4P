#include "hook/hooks.h"

#include <MinHook.h>

#include <atomic>

#include "core/log.h"

namespace cts4p::hooks {
namespace {
std::atomic<bool> g_initialized{false};
}

bool init() {
    if (g_initialized.exchange(true)) return true;
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        log::error("MinHook init failed: {}", MH_StatusToString(st));
        g_initialized = false;
        return false;
    }
    return true;
}

void shutdown() {
    if (!g_initialized.exchange(false)) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

bool create(void* target, void* detour, void** original, std::string_view name) {
    if (target == nullptr) {
        log::error("hook '{}': target is null", name);
        return false;
    }
    const MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK) {
        log::error("hook '{}' at {}: {}", name, target, MH_StatusToString(st));
        return false;
    }
    log::info("hook '{}' created at {}", name, target);
    return true;
}

bool enable_all() {
    const MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        log::error("enabling hooks failed: {}", MH_StatusToString(st));
        return false;
    }
    return true;
}

}  // namespace cts4p::hooks
