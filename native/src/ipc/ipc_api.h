#pragma once
// C ABI exported by winmm.dll for the Python side (ctypes) and test tools.
// All functions are safe to call from any thread unless noted.
//
// Python usage (Phase 3):
//   lib = ctypes.WinDLL(path_to_game_bin + "\\winmm.dll")   # same module the game loaded
//   lib.cts4p_api_version() -> 1

#include <cstdint>

#define CTS4P_API extern "C" __declspec(dllexport)

// Version of this C API (see core/version.h kApiVersion).
CTS4P_API uint32_t cts4p_api_version();

// Writes a NUL-terminated UTF-8 string into `out`. Returns bytes needed (excluding NUL).
CTS4P_API uint32_t cts4p_mod_version(char* out, uint32_t cap);

// Python -> native. Single producer (the game's Python thread). Returns 1 on success, 0 if full/too big.
CTS4P_API int32_t cts4p_ipc_send(const void* data, uint32_t len);

// Native -> Python. Single consumer (the game's Python thread).
// Returns frame length, 0 if empty, or -(needed size) if `cap` is too small.
CTS4P_API int64_t cts4p_ipc_recv(void* out, uint32_t cap);

// Snapshot of internal state as JSON (hooks, overlay, session). Returns bytes needed (excluding NUL).
CTS4P_API uint32_t cts4p_status_json(char* out, uint32_t cap);

// Runs a self-test synchronously and writes its JSON report. Returns 1 if passed.
// kind: 0 = loopback 127.0.0.1, 1 = signaling probe against `url` (UTF-8, NULL = config value).
CTS4P_API int32_t cts4p_selftest(int32_t kind, const char* url, char* out, uint32_t cap);

// Session control (used by the Python UI in Phase 3 and by test_net). Non-blocking; poll
// cts4p_status_json() for progress. `code`/`text` are UTF-8. Return 1 if the request was accepted.
CTS4P_API int32_t cts4p_session_host();
CTS4P_API int32_t cts4p_session_join(const char* code);
CTS4P_API int32_t cts4p_session_leave();
CTS4P_API int32_t cts4p_session_chat(const char* text);

// Runs queued remote build operations. Only does work when called on the game's build
// dispatch thread (otherwise returns 0 and the replays run inside the next dispatch).
CTS4P_API int32_t cts4p_build_pump();

// Test-only: sends a frame as if the local game had produced it. Returns 1 if sent to a peer.
CTS4P_API int32_t cts4p_debug_build_emit(const void* frame, uint32_t len);

// Test-only: locates/validates the build dispatcher in a loaded module without hooking. JSON report.
CTS4P_API uint32_t cts4p_debug_build_probe(const wchar_t* module_name, char* out, uint32_t cap);

// Test-only: starts the full runtime (config, overlay hooks) in a non-game process.
// The game itself never needs this; the DLL starts automatically inside TS4_x64.exe.
CTS4P_API int32_t cts4p_debug_start();

namespace cts4p::ipc {
// Native side of the bridge (used by Phase 3 sync code).
bool post_to_python(const void* data, uint32_t len);
int64_t take_from_python(void* out, uint32_t cap);
}  // namespace cts4p::ipc
