#pragma once
// Shared helpers for the cts4p native test executables.

#include <windows.h>

#include <cstdio>
#include <string>

namespace test {

inline std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L'\\'));
}

inline std::wstring dll_path() { return exe_dir() + L"\\winmm.dll"; }

inline std::wstring module_path(HMODULE m) {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(m, buf, static_cast<DWORD>(std::size(buf)));
    return std::wstring(buf, n);
}

inline bool iends_with(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() && _wcsicmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

struct Checker {
    int failures = 0;
    void check(bool ok, const char* what) {
        std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
        if (!ok) ++failures;
    }
    int finish(const char* suite) const {
        std::printf("%s: %s (%d failure%s)\n", suite, failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
        return failures ? 1 : 0;
    }
};

template <class Fn>
Fn get(HMODULE m, const char* name) {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

}  // namespace test
