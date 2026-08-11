#include <Windows.h>
#include "Application.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    Application app;
    if (!app.Initialize(L"Desktop Cam App", 1280, 720))
        return 1;
    const int rc = app.Run();
    app.Shutdown();
    return rc;
}
