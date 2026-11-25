#pragma once
// Runtime lifecycle of the native core inside the game process.
//
// DllMain only spawns a thread that calls start(); everything that could block
// or allocate heavily (config, logs, network, D3D hooks) happens there, never
// under the loader lock.

#include <string>

namespace cts4p::app {

// Idempotent. Loads config, opens the log, starts networking and waits for the
// game window before hooking DX11. Returns immediately after spawning work.
void start();

// Best-effort shutdown when the process exits normally.
void stop();

bool started();

// JSON snapshot used by cts4p_status_json() and the Diagnostics tab.
std::string status_json();

}  // namespace cts4p::app
