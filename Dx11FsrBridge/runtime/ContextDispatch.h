#pragma once

#include "../ComVtableMethods.h"
#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace neo::graphics
{
// 同一执行 context 被运行时原位更换槽时，重新装配同一套配置要求的入口。
// 无保护接口的代理仍使用普通 COM patch，不读取运行时内部状态。
bool bind_execution_dispatch(ID3D11DeviceContext *context, std::span<const com_vtable::MethodPatch> methods,
    com_vtable::MethodPatch protection_setter);

// 2026-11-16：真实 DXGI/F6 的首次 Present 会恢复 Draw 槽；执行 context 随交换链持有，
// 避免裸指针缓存跨越交换链销毁或地址复用。GetPrivateData 返回的引用由 ComPtr 释放。
HRESULT bind_present_execution(IDXGISwapChain *chain, ID3D11DeviceContext *context);
Microsoft::WRL::ComPtr<ID3D11DeviceContext> present_execution(IDXGISwapChain *chain);
}
