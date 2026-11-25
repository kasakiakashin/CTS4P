// Verifies the winmm.dll proxy: every probed export resolves into the genuine
// System32 winmm.dll, real calls work, and the cts4p runtime stays passive in
// a process that is not the game.

#include <cstdint>
#include <string>

#include "test_common.h"

int main() {
    test::Checker c;
    std::printf("test_proxy: loading %ls\n", test::dll_path().c_str());

    HMODULE ours = LoadLibraryW(test::dll_path().c_str());
    c.check(ours != nullptr, "LoadLibrary(build winmm.dll)");
    if (!ours) return c.finish("test_proxy");
    c.check(_wcsicmp(test::module_path(ours).c_str(), test::dll_path().c_str()) == 0, "loaded module is the proxy, not System32");

    const char* probes[] = {"timeGetTime", "timeBeginPeriod", "timeEndPeriod", "PlaySoundW", "waveOutGetNumDevs",
                            "mciSendStringW", "joyGetPosEx", "midiOutGetNumDevs", "mmioOpenW", "waveOutWrite"};
    for (const char* name : probes) {
        FARPROC fn = GetProcAddress(ours, name);
        HMODULE owner = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(fn), &owner);
        const std::wstring owner_path = owner ? test::module_path(owner) : L"";
        const std::string what = std::string(name) + " forwards to System32\\winmm.dll";
        c.check(fn != nullptr && test::iends_with(owner_path, L"\\System32\\winmm.dll"), what.c_str());
    }
    // Ordinal-only export #2 must exist too.
    c.check(GetProcAddress(ours, MAKEINTRESOURCEA(2)) != nullptr, "ordinal #2 (NONAME) forwarded");

    using TimeGetTime = DWORD(WINAPI*)();
    auto tgt = test::get<TimeGetTime>(ours, "timeGetTime");
    const DWORD a = tgt ? tgt() : 0;
    Sleep(20);
    const DWORD b = tgt ? tgt() : 0;
    c.check(tgt && b >= a + 15 && b - a < 2000, "timeGetTime() advances correctly through the proxy");

    using WaveOutGetNumDevs = UINT(WINAPI*)();
    auto wnd = test::get<WaveOutGetNumDevs>(ours, "waveOutGetNumDevs");
    c.check(wnd != nullptr, "waveOutGetNumDevs callable");
    if (wnd) std::printf("    waveOutGetNumDevs() = %u\n", wnd());

    using ApiVersion = uint32_t (*)();
    using StatusJson = uint32_t (*)(char*, uint32_t);
    auto api = test::get<ApiVersion>(ours, "cts4p_api_version");
    auto status = test::get<StatusJson>(ours, "cts4p_status_json");
    c.check(api && api() == 1, "cts4p_api_version() == 1");
    char buf[4096]{};
    if (status) status(buf, sizeof(buf));
    c.check(std::string(buf).find("\"started\":false") != std::string::npos, "runtime stays passive outside TS4_x64.exe");

    return c.finish("test_proxy");
}
