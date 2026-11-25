#pragma once
// Test-only control channel: a local named pipe (\\.\pipe\cts4p-debug) that accepts
// one-line commands and answers with JSON. Started only when config.json has
// "debug_control": true (default false), so normal players never expose it.
//
//   status          -> runtime status JSON (same as cts4p_status_json)
//   host            -> create a room (like the overlay's "Criar sala")
//   join CTS-XXXX   -> join a room (like the overlay's "Entrar")
//   leave           -> leave the room
//
// It exists so automated tests can drive the mod inside the real game without
// touching the game's own UI.

namespace cts4p::debug_pipe {

void start();

}  // namespace cts4p::debug_pipe
