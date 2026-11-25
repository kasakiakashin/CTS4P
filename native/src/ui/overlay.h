#pragma once
// Dear ImGui overlay rendered from the Present hook.
//
// Input: the game window's WndProc is subclassed. Messages are queued and
// replayed into ImGui on the render thread (ImGui is not thread-safe and the
// game's window thread may differ from its render thread). While the overlay
// is open, mouse/keyboard messages are swallowed so they don't reach the game.

#include <atomic>
#include <cstdint>

struct IDXGISwapChain;

namespace cts4p::overlay {

void on_present(IDXGISwapChain* swap_chain);
void on_before_resize();
void on_after_resize(IDXGISwapChain* swap_chain);

struct Stats {
    std::atomic<bool> initialized{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> visible{false};
    std::atomic<uint64_t> frames_drawn{0};
    std::atomic<uint32_t> width{0};
    std::atomic<uint32_t> height{0};
};
Stats& stats();

void set_visible(bool visible);

}  // namespace cts4p::overlay
