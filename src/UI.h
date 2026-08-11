#pragma once

#include <Windows.h>
#include <d3d11.h>

class Application;

class UI
{
public:
    UI();
    ~UI();

    bool Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx);
    void Shutdown();

    void NewFrame();
    void Draw(Application& app);
    // ImGui's Win32 backend message handler. Returns non-zero if handled.
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM, bool& handled);

private:
    bool initialized_ = false;
};
