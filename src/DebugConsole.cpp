#include "DebugConsole.h"

#include <Windows.h>
#include <cstdio>
#include <io.h>
#include <fcntl.h>

static bool g_allocated = false;

void ShowDebugConsole(bool show)
{
    if (show && !g_allocated) {
        if (!AllocConsole()) return;
        SetConsoleTitleW(L"Desktop Cam App - Debug Console");

        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONIN$",  "r", stdin);

        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);

        // Also mirror wchar_t writes onto the console.
        _setmode(_fileno(stdout), _O_U8TEXT);
        _setmode(_fileno(stderr), _O_U8TEXT);

        g_allocated = true;
        wprintf(L"[debug] console attached\n");
    } else if (!show && g_allocated) {
        wprintf(L"[debug] detaching console\n");
        FreeConsole();
        g_allocated = false;
    }
}

bool IsDebugConsoleShown() { return g_allocated; }
