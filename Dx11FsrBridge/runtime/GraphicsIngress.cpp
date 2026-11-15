#include "GraphicsIngress.h"
#include "../InterfaceVtableLayout.h"

#include <wrl/client.h>

namespace neo::graphics
{
namespace
{
using Microsoft::WRL::ComPtr;

const char *source_name(CreationPath path)
{
    switch (path)
    {
    case CreationPath::device: return "CreateDevice";
    case CreationPath::combined: return "CreateDeviceAndSwapChain";
    case CreationPath::factory: return "CreateSwapChain";
    case CreationPath::factory_hwnd: return "CreateSwapChainForHwnd";
    }
    return "unknown";
}
}

void publish_creation(HRESULT result, const CreatedGraphics &created, const IngressSink &sink)
{
    if (FAILED(result))
        return;

    auto *device = created.device;
    auto *context = created.context;
    ComPtr<ID3D11Device> recovered_device;
    ComPtr<ID3D11DeviceContext> recovered_context;
    if (!device && context)
    {
        context->GetDevice(recovered_device.GetAddressOf());
        device = recovered_device.Get();
    }
    if (!device && !context && created.chain)
    {
        // 2026-11-15：设备可能早于桥创建，F6 的 ForHwnd 返回链接才是事实源。
        // DX12 返回链只能提供队列关联，不能作为 DX11 设备参与后端选路。
        if (created.chain->GetDevice(IID_PPV_ARGS(recovered_device.GetAddressOf())) == S_OK)
            device = recovered_device.Get();
    }
    if (!context && device)
    {
        device->GetImmediateContext(recovered_context.GetAddressOf());
        context = recovered_context.Get();
    }

    ComPtr<ID3D11DeviceContext> execution;
    if (device || context)
    {
        if (context)
            execution.Attach(interface_layout::execution_context(context));
        // 保留包装器的 BeforeDraw/AfterDraw，只在执行端接管；不递归追逐代理 QI。
        sink.bind_device(device, execution ? execution.Get() : context);
    }

    if (!created.chain)
        return;
    Extent size = created.requested_size;
    if (created.path == CreationPath::combined || size.width == 0 || size.height == 0)
    {
        DXGI_SWAP_CHAIN_DESC actual {};
        if (created.chain->GetDesc(&actual) == S_OK)
            size = { actual.BufferDesc.Width, actual.BufferDesc.Height };
    }
    if (size.width != 0 && size.height != 0)
        sink.set_extent(size.width, size.height, source_name(created.path));
    sink.bind_chain(created.chain, execution ? execution.Get() : context);
}
}
