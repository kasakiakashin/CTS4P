// End-to-end overlay test in a real D3D11 window:
//   1. creates a 1280x720 window + swap chain (like the game would)
//   2. loads winmm.dll with CTS4P_FORCE=1 -> DLL waits for the window, hooks Present
//   3. presses INSERT (WM_KEYDOWN) -> overlay must open
//   4. runs the loopback self-test while rendering, so the overlay shows its result
//   5. saves the final frame to overlay_test.png and checks the runtime status JSON

#include <d3d11.h>
#include <wincodec.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "test_common.h"

namespace {

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

bool save_png(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc, const std::wstring& path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &staging));
    if (ok) {
        ctx->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE map{};
        ok = SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map));
        if (ok) {
            std::vector<uint8_t> bgra(static_cast<size_t>(d.Width) * d.Height * 4);
            for (UINT y = 0; y < d.Height; ++y) {
                const auto* src = static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch;
                uint8_t* dst = bgra.data() + static_cast<size_t>(y) * d.Width * 4;
                for (UINT x = 0; x < d.Width; ++x) {  // RGBA -> BGRA, opaque
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 255;
                }
            }
            ctx->Unmap(staging, 0);

            IWICImagingFactory* wic = nullptr;
            IWICStream* stream = nullptr;
            IWICBitmapEncoder* enc = nullptr;
            IWICBitmapFrameEncode* frame = nullptr;
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                 SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
                 SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                 SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
                 SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(d.Width, d.Height)) &&
                 SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
                 SUCCEEDED(frame->WritePixels(d.Height, d.Width * 4, static_cast<UINT>(bgra.size()), bgra.data())) &&
                 SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
            if (frame) frame->Release();
            if (enc) enc->Release();
            if (stream) stream->Release();
            if (wic) wic->Release();
        }
        staging->Release();
    }
    back->Release();
    return ok;
}

std::string status(HMODULE dll) {
    using StatusJson = uint32_t (*)(char*, uint32_t);
    static auto fn = test::get<StatusJson>(dll, "cts4p_status_json");
    std::vector<char> buf(16 * 1024);
    if (fn) fn(buf.data(), static_cast<uint32_t>(buf.size()));
    return buf.data();
}

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

}  // namespace

int main() {
    test::Checker c;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring mod_dir = test::exe_dir() + L"\\test_mod_dir";
    SetEnvironmentVariableW(L"CTS4P_FORCE", L"1");
    SetEnvironmentVariableW(L"CTS4P_MOD_DIR", mod_dir.c_str());

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"cts4p_overlay_test";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    RECT r{0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"cts4p overlay test (fake TS4)", WS_OVERLAPPEDWINDOW, 40, 40,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 1280;
    sd.BufferDesc.Height = 720;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;  // keeps back buffer contents after Present -> screenshot
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                               &sc, &dev, nullptr, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev,
                                           nullptr, &ctx);
    c.check(SUCCEEDED(hr), "D3D11 device + swap chain");
    if (FAILED(hr)) return c.finish("test_overlay");
    ID3D11Texture2D* back = nullptr;
    sc->GetBuffer(0, IID_PPV_ARGS(&back));
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();

    HMODULE dll = LoadLibraryW(test::dll_path().c_str());
    c.check(dll != nullptr, "LoadLibrary(winmm.dll) with CTS4P_FORCE=1");
    if (!dll) return c.finish("test_overlay");

    using SelfTest = int32_t (*)(int32_t, const char*, char*, uint32_t);
    auto selftest = test::get<SelfTest>(dll, "cts4p_selftest");
    std::atomic<int> loopback_result{-1};
    std::thread loopback;

    // Screenshot mode (UI review): CTS4P_UI_SHOT=<png>, with CTS4P_UI_DEMO / CTS4P_UI_TAB set by the caller.
    // CTS4P_UI_CLOSED=1 keeps the overlay closed (HUD pill + toasts only).
    wchar_t shot[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"CTS4P_UI_SHOT", shot, MAX_PATH) > 0) {
        const bool closed = GetEnvironmentVariableW(L"CTS4P_UI_CLOSED", nullptr, 0) > 0;
        const float bg[4] = {0.18f, 0.42f, 0.62f, 1.0f};
        const auto s0 = std::chrono::steady_clock::now();
        bool sent = false;
        int after = -1;
        MSG m{};
        while (std::chrono::steady_clock::now() - s0 < std::chrono::seconds(20)) {
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->ClearRenderTargetView(rtv, bg);
            sc->Present(1, 0);
            const std::string st = status(dll);
            if (!sent && has(st, "\"initialized\":true")) {
                if (!closed) {
                    PostMessageW(hwnd, WM_KEYDOWN, VK_INSERT, 0x00520001);
                    PostMessageW(hwnd, WM_KEYUP, VK_INSERT, 0xC0520001);
                }
                sent = true;
            }
            if (sent && after < 0 && (closed || has(st, "\"visible\":true"))) after = 0;
            if (after >= 0 && ++after > 40) break;
        }
        const bool ok = after > 40 && save_png(dev, ctx, sc, shot);
        std::printf("shot %ls: %s\n", shot, ok ? "ok" : "FAILED");
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), ok ? 0u : 1u);
    }

    const auto t0 = std::chrono::steady_clock::now();
    bool pressed = false;
    bool started_loopback = false;
    const float clear[4] = {0.18f, 0.42f, 0.62f, 1.0f};  // "sky" blue stand-in for the game scene
    MSG msg{};
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(25)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, clear);
        sc->Present(1, 0);

        const std::string st = status(dll);
        if (!pressed && has(st, "\"initialized\":true")) {
            PostMessageW(hwnd, WM_KEYDOWN, VK_INSERT, 0x00520001);  // INSERT, first press
            PostMessageW(hwnd, WM_KEYUP, VK_INSERT, 0xC0520001);
            pressed = true;
        }
        if (pressed && !started_loopback && has(st, "\"visible\":true")) {
            started_loopback = true;
            loopback = std::thread([&] {
                std::vector<char> out(64 * 1024);
                loopback_result = selftest(0, nullptr, out.data(), static_cast<uint32_t>(out.size()));
            });
        }
        if (loopback_result >= 0 && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(2)) {
            // Render a few more frames so the overlay shows the final report, then stop.
            for (int i = 0; i < 10; ++i) {
                ctx->OMSetRenderTargets(1, &rtv, nullptr);
                ctx->ClearRenderTargetView(rtv, clear);
                sc->Present(1, 0);
            }
            break;
        }
    }

    const std::string st = status(dll);
    std::printf("status: %s\n", st.c_str());
    c.check(has(st, "\"started\":true"), "runtime started (forced)");
    c.check(has(st, "\"installed\":true"), "Present hook installed");
    c.check(has(st, "\"initialized\":true"), "ImGui overlay initialized on the test swap chain");
    c.check(pressed && has(st, "\"visible\":true"), "INSERT opened the overlay");
    c.check(!has(st, "\"frames_drawn\":0"), "overlay frames drawn");
    c.check(loopback_result == 1, "loopback self-test passed while overlay rendered");

    const std::wstring png = test::exe_dir() + L"\\overlay_test.png";
    const bool saved = save_png(dev, ctx, sc, png);
    c.check(saved, "screenshot saved");
    if (saved) std::printf("screenshot: %ls\n", png.c_str());

    if (loopback.joinable()) loopback.join();
    const int rc = c.finish("test_overlay");
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(rc));
    return rc;
}
