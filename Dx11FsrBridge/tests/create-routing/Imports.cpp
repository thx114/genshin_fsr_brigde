#include <windows.h>
#include <d3d11.h>

extern "C" __declspec(dllexport) HRESULT WINAPI CallDevice()
{
    return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, nullptr, nullptr, nullptr);
}

extern "C" __declspec(dllexport) HRESULT WINAPI CallCombined()
{
    return D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, nullptr, nullptr, nullptr, nullptr, nullptr);
}
