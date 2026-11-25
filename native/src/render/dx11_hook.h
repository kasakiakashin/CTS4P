#pragma once
// Hooks IDXGISwapChain::Present / Present1 / ResizeBuffers.
//
// The vtable is read from a throwaway D3D11 device + swap chain created on a
// hidden window of our own, so the game's real swap chain is never touched
// until it calls Present. The hooked functions live in dxgi.dll, not in the
// DRM-wrapped TS4_x64.exe.

#include <atomic>
#include <cstdint>

struct IDXGISwapChain;

namespace cts4p::dx11 {

struct Callbacks {
    void (*on_present)(IDXGISwapChain* swap_chain) = nullptr;
    void (*on_before_resize)() = nullptr;
    void (*on_after_resize)(IDXGISwapChain* swap_chain) = nullptr;
};

bool install(const Callbacks& callbacks);

struct Stats {
    std::atomic<uint64_t> present_calls{0};
    std::atomic<uint64_t> present1_calls{0};
    std::atomic<uint64_t> resize_calls{0};
    std::atomic<bool> installed{false};
};
Stats& stats();

}  // namespace cts4p::dx11
