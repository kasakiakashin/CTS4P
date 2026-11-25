# CTS4P native core (`winmm.dll`)

Proxy DLL in C++20 that The Sims 4 loads automatically from `Game\Bin`. It:

- **forwards all 181 exports** of the real `System32\winmm.dll` (same ordinals), so the game behaves exactly as without the mod;
- starts the CTS4P runtime **only inside `TS4_x64.exe`** (any other process that loads it stays passive);
- hooks `IDXGISwapChain::Present/Present1/ResizeBuffers` (in `dxgi.dll`, not in the DRM-protected exe) and draws a **Dear ImGui overlay** (`INSERT`);
- opens P2P rooms `CTS-XXXX` over **WebRTC DataChannels** (libdatachannel, ICE/STUN), with signaling through `signaling/`;
- exposes a **C API** (`cts4p_*`) and lock-free IPC rings for the game's Python scripts (Phase 3).

## Layout

```
src/dllmain.cpp            entry point; includes proxy/winmm_exports.inc (generated forwarders)
src/core/                  app lifecycle, config.json, log, paths, versions
src/hook/                  MinHook wrapper
src/render/dx11_hook.*     Present/ResizeBuffers hooks via a dummy swap chain's vtable
src/ui/                    ImGui overlay (input queued from WndProc, replayed on render thread) + tabs
src/net/protocol.*         8-byte header wire format (no pickle, no eval)
src/net/peer_link.*        one WebRTC peer: "reliable" + "fast" channels, Hello handshake, RTT
src/net/session.*          host/join a room, star topology, chat relay, roster, timeouts
src/net/signaling.*        WebSocket client for the room server
src/net/selftest.*         loopback 127.0.0.1 test + signaling probe (overlay buttons)
src/ipc/                   SPSC frame rings + exported C API
src/diag/                  game version, installed packs, Mods listing
tools/gen_winmm_exports.py regenerates the forwarders from `dumpbin /exports System32\winmm.dll`
tests/                     test_proxy, test_net, test_overlay
```

## Build (Release x64)

Requires Visual Studio 2022 (MSVC v143, Desktop C++), CMake ≥ 3.25. Dependencies come from vcpkg (bundled with VS),
statically linked: MinHook, Dear ImGui (dx11/win32), libdatachannel (+OpenSSL), nlohmann-json.

```powershell
$env:VCPKG_ROOT = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg"
cmake --preset x64-release
cmake --build --preset x64-release
```

First configure builds the dependencies (~10 min). Output: `build\Release\winmm.dll`.
The build uses `/W4 /WX` on our code: any warning fails the build.

## Tests

```powershell
cd build\Release
.\test_proxy.exe                                   # forwarding + passive outside the game
.\test_net.exe loopback                            # WebRTC on 127.0.0.1, in-process
python ..\..\..\signaling\python\server.py         # (other terminal) local signaling server
.\test_net.exe signaling ws://127.0.0.1:8787/ws    # signaling protocol probe
.\test_net.exe host ws://127.0.0.1:8787/ws code.txt   # run these two at the same time:
.\test_net.exe join ws://127.0.0.1:8787/ws code.txt   # two processes = host + guest in a real room
.\test_overlay.exe                                 # D3D11 window: hook, INSERT, overlay, screenshot
```

## Manual install (until Phase 5's installer)

1. Copy `build\Release\winmm.dll` to `…\The Sims 4\Game\Bin\` (next to `TS4_x64.exe`).
2. Start the game. `Documents\Electronic Arts\The Sims 4\Mods\cts4p\` gets `config.json` and `cts4p_native.log`.
3. Press `INSERT` in game. Uninstall = delete that `winmm.dll`.
