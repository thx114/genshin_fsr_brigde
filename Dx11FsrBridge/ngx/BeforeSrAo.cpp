#include "BeforeSrAo.h"
#include "BeforeSrAoBytecode.h"

#include <array>
#include <cmath>
#include <utility>

namespace neo::ngx
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT k_group = 8;

// 2026-11-15：整套 state swap 在实测中让运行时替换了 Draw/Map 槽；只恢复 AO 自己使用的 CS 状态。
class CsBindings
{
public:
    explicit CsBindings(ID3D11DeviceContext1 &context) noexcept : context_(context)
    {
        context_.CSGetShader(shader_.GetAddressOf(), classes_.data(), &class_count_);
        context_.CSGetConstantBuffers1(0, 1, cb_.GetAddressOf(), &first_constant_, &constant_count_);
        context_.CSGetShaderResources(0, static_cast<UINT>(srvs_.size()), srvs_.data());
        context_.CSGetUnorderedAccessViews(0, 1, uav_.GetAddressOf());
    }

    ~CsBindings()
    {
        auto *uav = uav_.Get();
        auto *cb = cb_.Get();
        // nullptr 保留原 UAV 的隐藏计数；常量缓冲可能只绑定了原缓冲的一个子范围。
        context_.CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context_.CSSetShaderResources(0, static_cast<UINT>(srvs_.size()), srvs_.data());
        context_.CSSetConstantBuffers1(0, 1, &cb, &first_constant_, &constant_count_);
        context_.CSSetShader(shader_.Get(), classes_.data(), class_count_);
        for (auto *srv : srvs_)
            if (srv) srv->Release();
        for (UINT i = 0; i < class_count_; ++i)
            if (classes_[i]) classes_[i]->Release();
    }

    CsBindings(const CsBindings &) = delete;
    CsBindings &operator=(const CsBindings &) = delete;

private:
    ID3D11DeviceContext1 &context_;
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11UnorderedAccessView> uav_;
    std::array<ID3D11ClassInstance *, D3D11_SHADER_MAX_INTERFACES> classes_;
    std::array<ID3D11ShaderResourceView *, 2> srvs_ {};
    UINT class_count_ = static_cast<UINT>(classes_.size());
    UINT first_constant_ = 0, constant_count_ = 0;
};

bool plain_texture(const D3D11_TEXTURE2D_DESC &desc) noexcept
{
    return desc.Width && desc.Height && desc.MipLevels == 1 && desc.ArraySize == 1 &&
        desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0;
}
}

bool BeforeSrAo::prepare(ID3D11Device &device, ID3D11DeviceContext &context, ID3D11Texture2D &color,
    ID3D11Texture2D &depth) noexcept
{
    if (color_.Get() == &color && depth_.Get() == &depth && attempted_context_.Get() == &context)
        return result_ != nullptr;

    // 2026-11-15：只在池/执行域变化时准备一次。失败只否决这一组输入，不在每帧反复创建。
    BeforeSrAo next;
    next.color_ = &color;
    next.depth_ = &depth;
    next.attempted_context_ = &context;
    const bool ready = next.initialize(device, context, color, depth);
    if (!ready)
    {
        // 只缓存失败键，不保留初始化到一半的 GPU 对象。
        *this = {};
        color_ = std::move(next.color_);
        depth_ = std::move(next.depth_);
        attempted_context_ = std::move(next.attempted_context_);
        return false;
    }
    *this = std::move(next);
    return true;
}

bool BeforeSrAo::initialize(ID3D11Device &device, ID3D11DeviceContext &context, ID3D11Texture2D &color,
    ID3D11Texture2D &depth) noexcept
{
    D3D11_TEXTURE2D_DESC color_desc {}, depth_desc {};
    color.GetDesc(&color_desc);
    depth.GetDesc(&depth_desc);
    constexpr auto rgba = DXGI_FORMAT_R8G8B8A8_UNORM;
    if ((color_desc.Format != rgba && color_desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS) ||
        depth_desc.Format != DXGI_FORMAT_R32_FLOAT || !plain_texture(color_desc) || !plain_texture(depth_desc) ||
        color_desc.Width != depth_desc.Width || color_desc.Height != depth_desc.Height ||
        context.GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return false;

    if (device.GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0 ||
        FAILED(context.QueryInterface(IID_PPV_ARGS(execution_.GetAddressOf()))))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    srv.Format = rgba;
    if (FAILED(device.CreateShaderResourceView(&color, &srv, srvs_[0].GetAddressOf()))) return false;
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    if (FAILED(device.CreateShaderResourceView(&depth, &srv, srvs_[1].GetAddressOf()))) return false;

    D3D11_BUFFER_DESC cb {};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device.CreateBuffer(&cb, nullptr, constants_.GetAddressOf())) ||
        FAILED(device.CreateComputeShader(k_neo_before_sr_ao, sizeof(k_neo_before_sr_ao), nullptr,
            shader_.GetAddressOf())))
        return false;

    // 与原色保持同一存储格式，只有私有视图按 UNORM 访问；不在 NGX 的原输入上原位写。
    D3D11_TEXTURE2D_DESC output {};
    output.Width = width_ = color_desc.Width;
    output.Height = height_ = color_desc.Height;
    output.Format = color_desc.Format;
    output.MipLevels = output.ArraySize = output.SampleDesc.Count = 1;
    output.Usage = D3D11_USAGE_DEFAULT;
    output.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device.CreateTexture2D(&output, nullptr, result_.GetAddressOf()))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.Format = rgba;
    uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    return SUCCEEDED(device.CreateUnorderedAccessView(result_.Get(), &uav, result_uav_.GetAddressOf()));
}

bool BeforeSrAo::record(ID3D11Device &device, ID3D11DeviceContext &context, ID3D11Texture2D &color,
    ID3D11Texture2D &depth, const AoProjection &p) noexcept
{
    if (!(p.near_plane > 0.f && p.far_plane > p.near_plane && std::isfinite(p.far_plane) &&
        p.vertical_fov > 0.f && p.vertical_fov < 3.14159f) || !prepare(device, context, color, depth))
        return false;

    const Constants next { p.near_plane, p.near_plane / p.far_plane,
        2.f * std::tan(p.vertical_fov * 0.5f) / static_cast<float>(height_), p.reversed_depth ? 1u : 0u };
    if (!(next.near_over_far > 0.f && next.ray_step > 0.f)) return false;
    if (next != last_constants_)
    {
        context.UpdateSubresource(constants_.Get(), 0, nullptr, &next, 0, 0);
        last_constants_ = next;
    }

    {
        const CsBindings restore(*execution_.Get());
        ID3D11Buffer *cb = constants_.Get();
        ID3D11ShaderResourceView *srvs[] { srvs_[0].Get(), srvs_[1].Get() };
        ID3D11UnorderedAccessView *uav = result_uav_.Get();
        context.CSSetShader(shader_.Get(), nullptr, 0);
        context.CSSetConstantBuffers(0, 1, &cb);
        context.CSSetShaderResources(0, 2, srvs);
        context.CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context.Dispatch((width_ + k_group - 1) / k_group, (height_ + k_group - 1) / k_group, 1);
    }
    // 私有 UAV 不外借给供应方；不会因上一帧的 provider 绑定而被运行时从其他槽隐式解绑。
    context.CopyResource(&color, result_.Get());
    return true;
}
}
