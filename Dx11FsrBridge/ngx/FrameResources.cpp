#include "FrameResources.h"
#include "../Ffx12Backend.h"

#include <utility>

namespace neo::ngx
{
namespace
{
bool full_texture(const D3D11_TEXTURE2D_DESC &desc, UINT width, UINT height) noexcept
{
    return width != 0 && height != 0 && desc.Width == width && desc.Height == height &&
        desc.MipLevels == 1 && desc.ArraySize == 1 && desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0;
}

DXGI_FORMAT depth_srv_format(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return DXGI_FORMAT_R32_FLOAT;
    }
}
}

std::optional<FrameShape> copy_shape(const ffx12::FrameInput &input,
    const D3D11_TEXTURE2D_DESC &color, const D3D11_TEXTURE2D_DESC &output) noexcept
{
    // CopyResource 不缩放，也没有成功返回值。非法布局不能走到输出已交付的分支。
    if (!full_texture(color, input.render_w, input.render_h) ||
        !full_texture(output, input.display_w, input.display_h) ||
        color.Format == DXGI_FORMAT_UNKNOWN || output.Format == DXGI_FORMAT_UNKNOWN ||
        output.Usage == D3D11_USAGE_IMMUTABLE)
        return std::nullopt;
    return FrameShape { input.render_w, input.render_h, input.display_w, input.display_h,
        color.Format, output.Format };
}

bool GuideView::bind(ID3D11Device &device, ID3D11Texture2D *source, Kind kind, const FrameShape &shape) noexcept
{
    if (!source) return false;
    if (source_ == source && view_) return true;
    D3D11_TEXTURE2D_DESC desc {};
    source->GetDesc(&desc);
    if (desc.Width < shape.render_width || desc.Height < shape.render_height ||
        desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
        return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = kind == Kind::depth ? depth_srv_format(desc.Format) : DXGI_FORMAT_R10G10B10A2_UNORM;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> next;
    if (FAILED(device.CreateShaderResourceView(source, &srv, next.GetAddressOf()))) return false;
    view_ = std::move(next);
    source_ = source;
    return true;
}

FrameResources::BuildResult FrameResources::rebuild(ID3D11Device &device, const FrameShape &next) noexcept
{
    FrameResources pool;
    struct Spec
    {
        const char *name;
        DXGI_FORMAT format;
        ID3D11Texture2D **texture;
        ID3D11UnorderedAccessView **uav;
        bool output;
    };
    const Spec specs[] {
        { "color", next.color_format, pool.color.GetAddressOf(), nullptr, false },
        { "depth", DXGI_FORMAT_R32_FLOAT, pool.depth.GetAddressOf(), pool.depth_uav.GetAddressOf(), false },
        { "motion", DXGI_FORMAT_R16G16_FLOAT, pool.motion.GetAddressOf(), pool.motion_uav.GetAddressOf(), false },
        { "reactive", DXGI_FORMAT_R8_UNORM, next.reactive_mask ? pool.reactive.GetAddressOf() : nullptr,
            pool.reactive_uav.GetAddressOf(), false },
        { "output", next.output_format, pool.output.GetAddressOf(), nullptr, true },
    };
    for (const auto &spec : specs)
    {
        // 2026-11-16：关闭 mask 时 provider 收到 nullptr，不为未消费的 u1 分配资源。
        if (!spec.texture) continue;
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = spec.output ? next.output_width : next.render_width;
        desc.Height = spec.output ? next.output_height : next.render_height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = spec.format;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (spec.uav || spec.output) desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        if (const HRESULT hr = device.CreateTexture2D(&desc, nullptr, spec.texture); FAILED(hr))
            return { hr, spec.name };
        if (spec.uav)
        {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav {};
            uav.Format = spec.format;
            uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (const HRESULT hr = device.CreateUnorderedAccessView(*spec.texture, &uav, spec.uav); FAILED(hr))
                return { hr, spec.name };
        }
    }

    // NGX 现有深度 scale 和运动方向都是 1；两个 shader 在 b0.x 共用这个不变值。
    const float identity[4] { 1.0f, 0.0f, 0.0f, 0.0f };
    D3D11_SUBRESOURCE_DATA initial { identity, 0, 0 };
    D3D11_BUFFER_DESC cb {};
    cb.ByteWidth = sizeof(identity);
    cb.Usage = D3D11_USAGE_IMMUTABLE;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (const HRESULT hr = device.CreateBuffer(&cb, &initial, pool.constants.GetAddressOf()); FAILED(hr))
        return { hr, "constants" };
    pool.shape = next;
    *this = std::move(pool);
    return { S_OK, nullptr };
}

bool FrameResources::bind_guides(ID3D11Device &device, const ffx12::FrameInput &input) noexcept
{
    return depth_src.bind(device, input.depth, GuideView::Kind::depth, shape) &&
        motion_src.bind(device, input.motion, GuideView::Kind::motion, shape);
}

void FrameResources::record_inputs(ID3D11DeviceContext &context, const ffx12::FrameInput &input,
    ID3D11ComputeShader *depth_cs, ID3D11ComputeShader *motion_cs) const noexcept
{
    context.CopyResource(color.Get(), input.color);
    const UINT groups_x = (shape.render_width + 7u) / 8u;
    const UINT groups_y = (shape.render_height + 7u) / 8u;
    ID3D11Buffer *cb[] { constants.Get() };
    context.CSSetConstantBuffers(0, 1, cb);
    ID3D11ShaderResourceView *srvs[] { depth_src.get() };
    ID3D11UnorderedAccessView *uavs[] { depth_uav.Get(), nullptr };
    context.CSSetShader(depth_cs, nullptr, 0);
    context.CSSetShaderResources(0, 1, srvs);
    context.CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    context.Dispatch(groups_x, groups_y, 1);

    srvs[0] = motion_src.get();
    uavs[0] = motion_uav.Get();
    uavs[1] = reactive_uav.Get();
    context.CSSetShader(motion_cs, nullptr, 0);
    context.CSSetShaderResources(0, 1, srvs);
    context.CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    context.Dispatch(groups_x, groups_y, 1);

    // 清除桥的输入输出绑定后才调用外部 provider；防止它自己的读绑定因 UAV 重叠被驱动置空。
    cb[0] = nullptr;
    srvs[0] = nullptr;
    uavs[0] = uavs[1] = nullptr;
    context.CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    context.CSSetShaderResources(0, 1, srvs);
    context.CSSetConstantBuffers(0, 1, cb);
    context.CSSetShader(nullptr, nullptr, 0);
}
}
