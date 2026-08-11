#include "Window.h"
#include "resource.h"

#include <windowsx.h>

static constexpr const wchar_t* kClassName = L"DesktopCamAppWindowClass";

// Height of the invisible drag strip along the top of the window (no title
// bar). Below this strip is HTCLIENT so ImGui receives clicks normally.
static constexpr int kDragStripPx   = 28;
// Width of the invisible resize handles around the edges.
static constexpr int kResizeEdgePx  = 6;

Window::Window() = default;

Window::~Window()
{
    Destroy();
}

bool Window::Create(const wchar_t* title, int width, int height)
{
    // Load the best-matching icon sizes for the taskbar, alt-tab, and
    // window title / tray. The .ico embeds 16 / 32 / 48 / 64 / 128 / 256
    // so both small and large requests get a crisp result.
    HMODULE hInst = GetModuleHandleW(nullptr);
    HICON   hBig  = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON),
                                      GetSystemMetrics(SM_CYICON),
                                      LR_DEFAULTCOLOR);
    HICON   hSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON),
                                       GetSystemMetrics(SM_CYSMICON),
                                       LR_DEFAULTCOLOR);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = &Window::StaticWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wc.lpszClassName = kClassName;
    wc.hIcon         = hBig   ? hBig   : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = hSmall ? hSmall : wc.hIcon;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    // Default: standard titled window. Borderless is opt-in via the UI
    // (and persisted), so users get normal close/minimize/maximize buttons
    // out of the box.
    const DWORD style   = WS_OVERLAPPEDWINDOW;
    const DWORD exStyle = 0;

    RECT rc{ 0, 0, width, height };
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);

    hwnd_ = CreateWindowExW(
        exStyle,
        kClassName,
        title,
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr,
        wc.hInstance,
        this);

    if (!hwnd_)
        return false;

    width_ = width;
    height_ = height;

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void Window::Destroy()
{
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool Window::PumpMessages()
{
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
            return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

bool Window::ConsumeResized()
{
    bool r = resized_;
    resized_ = false;
    return r;
}

void Window::SetBorderless(bool borderless)
{
    if (!hwnd_ || fullscreen_) return;
    if (borderless_ == borderless) return;

    borderless_ = borderless;

    RECT windowRect{};
    GetWindowRect(hwnd_, &windowRect);

    if (borderless_) {
        const LONG_PTR style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_VISIBLE;
        SetWindowLongPtrW(hwnd_, GWL_STYLE,   style);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, WS_EX_APPWINDOW);
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE,   WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, 0);
    }

    // SWP_FRAMECHANGED re-runs WM_NCCALCSIZE. Keep the outer window rect
    // exactly where it was so the user doesn't see the window jump.
    SetWindowPos(hwnd_, nullptr,
        windowRect.left, windowRect.top,
        windowRect.right  - windowRect.left,
        windowRect.bottom - windowRect.top,
        SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}

void Window::ToggleFullscreen()
{
    if (!hwnd_) return;

    if (!fullscreen_)
    {
        savedStyle_   = (DWORD)GetWindowLongPtrW(hwnd_, GWL_STYLE);
        savedExStyle_ = (DWORD)GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        savedPlacement_.length = sizeof(savedPlacement_);
        GetWindowPlacement(hwnd_, &savedPlacement_);

        MONITORINFO mi{ sizeof(mi) };
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(mon, &mi))
            return;

        // Borderless window style (NOT WS_EX_LAYERED / transparent — required
        // for OBS Game Capture to hook us as a regular window).
        SetWindowLongPtrW(hwnd_, GWL_STYLE,   WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, savedExStyle_ & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(hwnd_, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

        fullscreen_ = true;
    }
    else
    {
        SetWindowLongPtrW(hwnd_, GWL_STYLE,   savedStyle_);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, savedExStyle_);
        SetWindowPlacement(hwnd_, &savedPlacement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = false;
    }
}

LRESULT CALLBACK Window::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Window* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    }
    else
    {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self)
        return self->WndProc(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Forward to ImGui first.
    if (ExternalWndProc)
    {
        bool handled = false;
        LRESULT r = ExternalWndProc(hwnd, msg, wParam, lParam, handled);
        if (handled)
            return r;
    }

    switch (msg)
    {
    case WM_NCCALCSIZE:
        // In borderless mode, collapse the non-client area to nothing so the
        // whole window is the D3D surface (no title bar / sizing frame gets
        // captured by Discord or Window Capture).
        if (borderless_ && wParam == TRUE) return 0;
        break;

    case WM_NCHITTEST: {
        if (!borderless_) break; // default chrome handles hit-testing
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT  rc; GetWindowRect(hwnd, &rc);
        const int x = pt.x - rc.left;
        const int y = pt.y - rc.top;
        const int w = rc.right  - rc.left;
        const int h = rc.bottom - rc.top;
        const int b = kResizeEdgePx;
        const bool left   = x <  b;
        const bool right  = x >= w - b;
        const bool top    = y <  b;
        const bool bottom = y >= h - b;
        if (top    && left)  return HTTOPLEFT;
        if (top    && right) return HTTOPRIGHT;
        if (bottom && left)  return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (top)    return HTTOP;
        if (bottom) return HTBOTTOM;
        if (left)   return HTLEFT;
        if (right)  return HTRIGHT;
        if (y < kDragStripPx) return HTCAPTION;
        return HTCLIENT;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            width_ = LOWORD(lParam);
            height_ = HIWORD(lParam);
            resized_ = true;
        }
        return 0;

    case WM_SYSKEYDOWN:
        // Alt+Enter -> fullscreen toggle.
        if (wParam == VK_RETURN && (HIWORD(lParam) & KF_ALTDOWN))
        {
            ToggleFullscreen();
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
