#include "ContextDispatch.h"
#include "../InterfaceVtableLayout.h"
#include <wrl/client.h>

namespace neo::graphics
{
namespace
{
constexpr GUID k_present_execution { 0xe8bf63c2, 0x2ea5, 0x4196, { 0xb8, 0x27, 0x53, 0xa9, 0xdd, 0x14, 0x70, 0xc1 } };
}

HRESULT bind_present_execution(IDXGISwapChain *chain, ID3D11DeviceContext *context)
{
    if (!chain || !context) return E_INVALIDARG;
    return chain->SetPrivateDataInterface(k_present_execution, context);
}

Microsoft::WRL::ComPtr<ID3D11DeviceContext> present_execution(IDXGISwapChain *chain)
{
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    if (!chain) return context;
    UINT size = sizeof(ID3D11DeviceContext *);
    const HRESULT result = chain->GetPrivateData(k_present_execution, &size, context.GetAddressOf());
    if (FAILED(result)) context.Reset();
    return context;
}

bool bind_execution_dispatch(ID3D11DeviceContext *context, std::span<const com_vtable::MethodPatch> methods,
    com_vtable::MethodPatch protection_setter)
{
    if (!context) return false;

    Microsoft::WRL::ComPtr<ID3D11Multithread> protection;
    context->QueryInterface(IID_PPV_ARGS(protection.GetAddressOf()));
    struct ProtectionScope
    {
        ID3D11Multithread *api;
        explicit ProtectionScope(ID3D11Multithread *p) : api(p)
        {
            if (api) api->Enter();
        }
        ~ProtectionScope()
        {
            if (api) api->Leave();
        }
    } scope(protection.Get());
    if (protection)
    {
        // 2026-11-15：硬件和 WARP 首次读状态才会完成保护开关后的换表；不改变 IA 或提交 GPU 命令。
        D3D11_PRIMITIVE_TOPOLOGY ignored {};
        context->IAGetPrimitiveTopology(&ignored);
    }
    if (!com_vtable::patch(context, interface_layout::context(context), methods))
        return false;
    // IUnknown (3) + Enter, Leave, Set, Get；不得按该长度拷贝运行时私有表。
    constexpr std::size_t k_multithread_methods = 7;
    return !protection || com_vtable::patch(protection.Get(), k_multithread_methods, { protection_setter });
}
}
