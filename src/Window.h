#pragma once

#include <Windows.h>
#include <functional>

class Window
{
public:
    using MessageCallback = std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>;

    Window();
    ~Window();

    bool Create(const wchar_t* title, int width, int height);
    void Destroy();

    // Pump all pending messages. Returns false when WM_QUIT received.
    bool PumpMessages();

    void ToggleFullscreen();

    // Switch between a standard titled window (default) and a fully
    // borderless window. Borderless is what you want when streaming or
    // capturing since the title bar isn't part of the captured output.
    void SetBorderless(bool borderless);
    bool IsBorderless() const { return borderless_; }

    // Consumes & clears the resized flag. Returns true if a resize happened
    // since the previous call.
    bool ConsumeResized();

    HWND Hwnd() const { return hwnd_; }
    int  Width() const { return width_; }
    int  Height() const { return height_; }

    // Allow the renderer to hook in ImGui's Win32 handler.
    MessageCallback ExternalWndProc;

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND  hwnd_ = nullptr;
    int   width_ = 0;
    int   height_ = 0;
    bool  resized_ = false;
    bool  fullscreen_ = false;
    bool  borderless_ = false;
    WINDOWPLACEMENT savedPlacement_{};
    DWORD savedStyle_ = 0;
    DWORD savedExStyle_ = 0;
};
