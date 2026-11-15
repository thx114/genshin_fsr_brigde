#pragma once

#include "../ComVtableMethods.h"
#include <d3d11_4.h>

namespace neo::graphics
{
// 同一执行 context 被运行时原位更换槽时，重新装配同一套配置要求的入口。
// 无保护接口的代理仍使用普通 COM patch，不读取运行时内部状态。
bool bind_execution_dispatch(ID3D11DeviceContext *context, std::span<const com_vtable::MethodPatch> methods,
    com_vtable::MethodPatch protection_setter);
}
