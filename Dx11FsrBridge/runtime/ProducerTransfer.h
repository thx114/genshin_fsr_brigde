#pragma once

#include "../interop/BeforeSrV1.h"
#include "../ComVtableMethods.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <span>

namespace neo::producer
{
void set_enabled(bool enabled) noexcept;
bool enabled() noexcept;
// 2026-11-16：声明来自当前writer的程序和CPU更新源；与诊断异步快照、jitter旧表独立。
void remember_shader(ID3D11PixelShader &shader, const void *bytes, std::size_t size) noexcept;
void remember_vertex(ID3D11VertexShader &shader, const void *bytes, std::size_t size) noexcept;
void remember_buffer(ID3D11Buffer &buffer, const D3D11_BUFFER_DESC &desc,
    const D3D11_SUBRESOURCE_DATA *initial) noexcept;
void attach_context(ID3D11DeviceContext &context) noexcept;
void refresh_targets(ID3D11DeviceContext &context, bool compute = false) noexcept;
class TargetRefresh
{
public:
    explicit TargetRefresh(ID3D11DeviceContext &context, bool compute = false) noexcept
        : context_(context), compute_(compute) {}
    ~TargetRefresh() { refresh_targets(context_, compute_); }
private:
    ID3D11DeviceContext &context_;
    bool compute_;
};
void invalidate(ID3D11Resource *resource) noexcept;
void update_buffer(ID3D11DeviceContext &context, ID3D11Resource *resource, UINT subresource,
    const D3D11_BOX *box, const void *data, bool context1 = false) noexcept;
void mapped(ID3D11DeviceContext &context, ID3D11Resource *resource, UINT subresource,
    D3D11_MAP type, HRESULT result, const D3D11_MAPPED_SUBRESOURCE *mapping) noexcept;
void unmapping(ID3D11DeviceContext &context, ID3D11Resource *resource, UINT subresource) noexcept;
void unordered_write(ID3D11DeviceContext &context) noexcept;

class DrawWrite
{
public:
    void returned() noexcept;

private:
    friend DrawWrite begin_draw(ID3D11DeviceContext &, UINT) noexcept;
    friend DrawWrite begin_draw(ID3D11DeviceContext &, UINT, bool, UINT, INT) noexcept;
    Microsoft::WRL::ComPtr<IUnknown> record_;
    Microsoft::WRL::ComPtr<IUnknown> constants_;
    Microsoft::WRL::ComPtr<IUnknown> geometry_[2];
    std::uint64_t geometry_versions_[2] {};
    std::uint64_t serial_ = 0, owner_ = 0, epoch_ = 0, constants_version_ = 0;
};

DrawWrite begin_draw(ID3D11DeviceContext &context, UINT elements) noexcept;
DrawWrite begin_draw(ID3D11DeviceContext &context, UINT elements, bool indexed, UINT start, INT base) noexcept;
interop::ColorTransfer take_transfer(ID3D11DeviceContext &context, ID3D11Texture2D &source) noexcept;

// 新观察入口仍经现有COM路由；扩展接口单独按实际公开槽数安装，旧代理不因此失去基础Draw钩子。
std::span<const com_vtable::MethodPatch> extra_methods(void (*repair)(ID3D11DeviceContext *)) noexcept;
bool bind_extended(ID3D11DeviceContext &context) noexcept;
bool bind_device(ID3D11Device &device) noexcept;
}
