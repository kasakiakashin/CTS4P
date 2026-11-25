/*
 * py37host - runs a Python script on The Sims 4's own embedded Python 3.7
 * (Game\Bin\python37_x64.dll + the game's standard library zip), outside the game.
 *
 * Used to byte-compile the cts4p scripts with the exact interpreter the game
 * uses (same .pyc magic/format) and to unit-test the ctypes bridge for real.
 *
 * usage: py37host <Game\Bin dir> <stdlib zip> <extra sys.path entries ;-sep> <script.py> [args...]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef void(__cdecl* SetPathFn)(const wchar_t*);
typedef void(__cdecl* InitFn)(int);
typedef int(__cdecl* RunFn)(const char*);
typedef int(__cdecl* FinalizeFn)(void);

static char* utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char* s = (char*)malloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* Appends `s` as a Python string literal (escaping backslashes and quotes). */
static void append_literal(char* out, size_t cap, const char* s) {
    size_t len = strlen(out);
    if (len + 2 >= cap) return;
    out[len++] = '\'';
    for (; *s && len + 3 < cap; ++s) {
        if (*s == '\\' || *s == '\'') out[len++] = '\\';
        out[len++] = *s;
    }
    out[len++] = '\'';
    out[len] = 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 5) {
        fwprintf(stderr, L"usage: py37host <Game\\Bin> <stdlib.zip> <extra;paths> <script.py> [args...]\n");
        return 2;
    }
    const wchar_t* bin = argv[1];
    SetDllDirectoryW(bin); /* python37_x64.dll's own dependencies + extension modules */
    wchar_t dll[MAX_PATH * 2];
    swprintf(dll, MAX_PATH * 2, L"%s\\python37_x64.dll", bin);
    HMODULE py = LoadLibraryW(dll);
    if (!py) {
        fwprintf(stderr, L"cannot load %s (%lu)\n", dll, GetLastError());
        return 2;
    }
    SetPathFn set_path = (SetPathFn)(void*)GetProcAddress(py, "Py_SetPath");
    InitFn init = (InitFn)(void*)GetProcAddress(py, "Py_InitializeEx");
    RunFn run = (RunFn)(void*)GetProcAddress(py, "PyRun_SimpleString");
    FinalizeFn fin = (FinalizeFn)(void*)GetProcAddress(py, "Py_FinalizeEx");
    int* no_site = (int*)(void*)GetProcAddress(py, "Py_NoSiteFlag");
    int* no_user_site = (int*)(void*)GetProcAddress(py, "Py_NoUserSiteDirectory");
    int* dont_write = (int*)(void*)GetProcAddress(py, "Py_DontWriteBytecodeFlag");
    if (!set_path || !init || !run || !fin) {
        fwprintf(stderr, L"python37_x64.dll is missing the embedding API\n");
        return 2;
    }
    if (no_site) *no_site = 1;
    if (no_user_site) *no_user_site = 1;
    if (dont_write) *dont_write = 1;

    /* sys.path: stdlib zip (its pyc live under lib/), the DLLs folder, then extra entries. */
    wchar_t path[8192];
    swprintf(path, 8192, L"%s\\lib;%s\\Python\\DLLs;%s", argv[2], bin, argv[3]);
    set_path(path);
    init(0);

    static char code[65536];
    strcpy_s(code, sizeof(code), "import sys\nsys.argv = [");
    for (int i = 4; i < argc; ++i) {
        char* a = utf8(argv[i]);
        append_literal(code, sizeof(code), a);
        strcat_s(code, sizeof(code), ",");
        free(a);
    }
    strcat_s(code, sizeof(code), "]\n__file__ = sys.argv[0]\n"
                                 "with open(sys.argv[0], 'rb') as _f:\n"
                                 "    _src = _f.read()\n"
                                 "try:\n"
                                 "    exec(compile(_src, sys.argv[0], 'exec'), {'__name__': '__main__', '__file__': sys.argv[0]})\n"
                                 "except SystemExit as _e:\n"
                                 "    sys.stdout.flush()\n"
                                 "    import os\n"
                                 "    os._exit(_e.code if isinstance(_e.code, int) else (0 if _e.code is None else 1))\n");
    int rc = run(code);
    fflush(stdout);
    fin();
    return rc == 0 ? 0 : 1;
}
