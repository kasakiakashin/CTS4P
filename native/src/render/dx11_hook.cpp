#include "render/dx11_hook.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include "core/log.h"
#include "hook/hooks.h"

namespace cts4p::dx11 {
namespace {

// vtable slots (IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7, IDXGISwapChain 8-17, IDXGISwapChain1 18-28)
constexpr size_t kPresentSlot = 8;
constexpr size_t kResizeBuffersSlot = 13;
constexpr size_t kPresent1Slot = 22;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
ResizeBuffersFn g_resize = nullptr;
Callbacks g_cb;

// The DXGI runtime may implement Present on top of Present1 (or vice versa);
// this guard makes sure the overlay is drawn once per frame.
thread_local bool t_in_present = false;

void draw(IDXGISwapChain* sc) {
    if (t_in_present || g_cb.on_present == nullptr) return;
    t_in_present = true;
    g_cb.on_present(sc);
}

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    stats().present_calls.fetch_add(1, std::memory_order_relaxed);
    const bool outer = !t_in_present;
    if (!(flags & DXGI_PRESENT_TEST)) draw(sc);
    const HRESULT hr = g_present(sc, sync, flags);
    if (outer) t_in_present = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    stats().present1_calls.fetch_add(1, std::memory_order_relaxed);
    const bool outer = !t_in_present;
    if (!(flags & DXGI_PRESENT_TEST)) draw(sc);
    const HRESULT hr = g_present1(sc, sync, flags, params);
    if (outer) t_in_present = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    stats().resize_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_cb.on_before_resize) g_cb.on_before_resize();
    const HRESULT hr = g_resize(sc, count, w, h, fmt, flags);
    if (SUCCEEDED(hr) && g_cb.on_after_resize) g_cb.on_after_resize(sc);
    return hr;
}

LRESULT CALLBACK dummy_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

struct VTable {
    void* present = nullptr;
    void* present1 = nullptr;
    void* resize = nullptr;
};

bool read_vtable(VTable& out) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = dummy_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"cts4p_dummy_dx11";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        log::error("dx11: dummy window creation failed ({})", GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &desc, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) {  // e.g. no GPU in a VM: WARP still exposes the same dxgi vtable
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc,
                                           &sc, &dev, nullptr, &ctx);
    }
    bool ok = false;
    if (SUCCEEDED(hr)) {
        void** vt = *reinterpret_cast<void***>(sc);
        out.present = vt[kPresentSlot];
        out.resize = vt[kResizeBuffersSlot];
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1)))) {
            out.present1 = (*reinterpret_cast<void***>(sc1))[kPresent1Slot];
            sc1->Release();
        }
        ok = true;
    } else {
        log::error("dx11: D3D11CreateDeviceAndSwapChain failed (0x{:08X})", static_cast<unsigned>(hr));
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (sc) sc->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

}  // namespace

Stats& stats() {
    static Stats s;
    return s;
}

bool install(const Callbacks& callbacks) {
    g_cb = callbacks;
    VTable vt;
    if (!read_vtable(vt)) return false;
    bool ok = hooks::create(vt.present, &hk_present, &g_present, "IDXGISwapChain::Present");
    ok = hooks::create(vt.resize, &hk_resize, &g_resize, "IDXGISwapChain::ResizeBuffers") && ok;
    if (vt.present1 && vt.present1 != vt.present) {
        // Optional: only some games use Present1. Failure here is not fatal.
        hooks::create(vt.present1, &hk_present1, &g_present1, "IDXGISwapChain1::Present1");
    }
    if (!ok) return false;
    if (!hooks::enable_all()) return false;
    stats().installed = true;
    log::info("dx11: Present hooks active");
    return true;
}

}  // namespace cts4p::dx11
