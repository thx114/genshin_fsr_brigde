#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace neo::graphics
{
enum class CreationPath
{
    device,
    combined,
    factory,
    factory_hwnd,
};

struct Extent
{
    UINT width = 0;
    UINT height = 0;
};

// 所有结果对象均借用；只有创建成功后才读取输出。工厂的 IUnknown 入参不是结果设备。
struct CreatedGraphics
{
    CreationPath path;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    IDXGISwapChain *chain = nullptr;
    Extent requested_size {};
};

struct IngressSink
{
    void (*bind_device)(ID3D11Device *device, ID3D11DeviceContext *execution);
    void (*bind_chain)(IDXGISwapChain *chain, ID3D11DeviceContext *execution);
    void (*set_extent)(UINT width, UINT height, const char *source);
};

void publish_creation(HRESULT result, const CreatedGraphics &created, const IngressSink &sink);
}
