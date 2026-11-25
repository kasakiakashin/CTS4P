#pragma once
// Thin RAII-free wrapper over MinHook with logging. All hooks are created
// disabled and enabled together so a partial failure never leaves the game
// half-patched.

#include <string_view>

namespace cts4p::hooks {

bool init();
void shutdown();

// Creates (but does not enable) a hook. `original` receives the trampoline.
bool create(void* target, void* detour, void** original, std::string_view name);

template <class Fn>
bool create(void* target, Fn* detour, Fn** original, std::string_view name) {
    return create(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), name);
}

bool enable_all();

}  // namespace cts4p::hooks
