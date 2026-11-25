#include "ui/overlay.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "ui/panels.h"
#include "ui/theme.h"

// Declared (commented out) in imgui_impl_win32.h on purpose; we must declare it ourselves.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace cts4p::overlay {
namespace {

struct QueuedMsg {
    UINT msg;
    WPARAM wparam;
    LPARAM lparam;
};

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
IDXGISwapChain* g_swap = nullptr;  // not owned; used only for identity
HWND g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;
std::string g_ini_path;

std::mutex g_input_mutex;
std::vector<QueuedMsg> g_input;

bool is_input_message(UINT m) {
    return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || (m >= WM_KEYFIRST && m <= WM_KEYLAST) || m == WM_INPUT ||
           m == WM_MOUSEHOVER || m == WM_MOUSELEAVE || m == WM_NCMOUSEMOVE;
}

LRESULT CALLBACK hooked_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    const bool key_down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const bool first_press = (lparam & (1 << 30)) == 0;
    if (key_down && static_cast<int>(wparam) == config().overlay_key) {
        if (first_press) set_visible(!stats().visible);
        return 0;
    }
    if (stats().visible) {
        if (is_input_message(msg) || msg == WM_KILLFOCUS || msg == WM_SETFOCUS) {
            std::scoped_lock lock(g_input_mutex);
            if (g_input.size() < 4096) g_input.push_back({msg, wparam, lparam});
        }
        if (is_input_message(msg)) return 0;  // swallow: the game must not react while the overlay is open
    }
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wparam, lparam);
}

void release_rtv() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool create_rtv(IDXGISwapChain* sc) {
    release_rtv();
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    if (FAILED(hr)) {  // typeless back buffers need an explicit view format
        D3D11_TEXTURE2D_DESC td{};
        back->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = g_device->CreateRenderTargetView(back, &rd, &g_rtv);
    }
    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    stats().width = td.Width;
    stats().height = td.Height;
    back->Release();
    if (FAILED(hr)) log::error("overlay: CreateRenderTargetView failed (0x{:08X})", static_cast<unsigned>(hr));
    return SUCCEEDED(hr);
}

bool init(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_device)))) {
        log::error("overlay: swap chain is not D3D11; overlay disabled");
        stats().failed = true;
        return false;
    }
    g_device->GetImmediateContext(&g_context);
    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;
    g_swap = sc;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    g_ini_path = paths::to_utf8((paths::mod_dir() / L"overlay_layout.ini").wstring());
    io.IniFilename = g_ini_path.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    theme::apply();

    if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX11_Init(g_device, g_context)) {
        log::error("overlay: ImGui backend init failed");
        stats().failed = true;
        return false;
    }
    if (!create_rtv(sc)) {
        stats().failed = true;
        return false;
    }
    g_orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hooked_wndproc)));
    if (!g_orig_wndproc) log::warn("overlay: could not subclass game window ({}); toggle key won't work", GetLastError());

    stats().initialized = true;
    log::info("overlay: initialized ({}x{}, hwnd {})", stats().width.load(), stats().height.load(), static_cast<void*>(g_hwnd));
    panels::on_overlay_ready();
    return true;
}

void replay_input() {
    std::vector<QueuedMsg> batch;
    {
        std::scoped_lock lock(g_input_mutex);
        batch.swap(g_input);
    }
    for (const auto& m : batch) ImGui_ImplWin32_WndProcHandler(g_hwnd, m.msg, m.wparam, m.lparam);
}

}  // namespace

Stats& stats() {
    static Stats s;
    return s;
}

void set_visible(bool visible) {
    stats().visible = visible;
    if (!visible) {
        std::scoped_lock lock(g_input_mutex);
        g_input.clear();
    }
}

void on_present(IDXGISwapChain* sc) {
    if (stats().failed) return;
    if (!stats().initialized) {
        if (!init(sc)) return;
    } else if (sc != g_swap) {
        // Game recreated its swap chain (fullscreen toggle, device reset): rebind our render target.
        ID3D11Device* dev = nullptr;
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) return;
        const bool same_device = dev == g_device;
        dev->Release();
        if (!same_device) {
            log::warn("overlay: game switched D3D device; overlay disabled until restart");
            release_rtv();
            stats().failed = true;
            return;
        }
        g_swap = sc;
        if (!create_rtv(sc)) return;
    }
    if (!g_rtv) {
        if (!create_rtv(sc)) return;
    }

    const bool visible = stats().visible;
    if (!visible && !panels::toasts_active()) {
        std::scoped_lock lock(g_input_mutex);
        g_input.clear();
        return;  // nothing to draw: zero overhead while closed
    }

    replay_input();
    ImGui::GetIO().MouseDrawCursor = visible;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (visible) {
        bool open = true;
        panels::draw_main_window(&open);
        if (!open) set_visible(false);
    }
    panels::draw_toasts();
    ImGui::Render();

    // Preserve the game's render targets around our draw.
    ID3D11RenderTargetView* old_rtv = nullptr;
    ID3D11DepthStencilView* old_dsv = nullptr;
    g_context->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_context->OMSetRenderTargets(1, &old_rtv, old_dsv);
    if (old_rtv) old_rtv->Release();
    if (old_dsv) old_dsv->Release();
    stats().frames_drawn.fetch_add(1, std::memory_order_relaxed);
}

void on_before_resize() { release_rtv(); }

void on_after_resize(IDXGISwapChain* sc) {
    if (stats().initialized && !stats().failed && sc == g_swap) create_rtv(sc);
}

}  // namespace cts4p::overlay
