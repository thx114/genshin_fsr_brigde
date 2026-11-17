#include "FrameResources.h"
#include "../Ffx12Backend.h"

#include <utility>
#include <array>

namespace neo::ngx
{
namespace
{
class SceneDepthRead
{
public:
    SceneDepthRead(ID3D11DeviceContext &context, bool needed) noexcept : context_(needed ? &context : nullptr)
    {
        // 2026-11-16（四路 guide 实测）：仍绑定为 DSV 的深度被 CS 读成 0，必须先解除写绑定。
        // 只在调用方确实选择了当前 DSV 时读取 OM，普通 FSR guide 路径不付这次查询成本。
        if (!context_) return;
        context_->OMGetRenderTargets(static_cast<UINT>(targets_.size()), targets_.data(), &depth_);
        for (UINT i = static_cast<UINT>(targets_.size()); i != 0; --i)
            if (targets_[i - 1])
            {
                target_count_ = i;
                break;
            }
        bind_depth(nullptr);
    }
    ~SceneDepthRead()
    {
        if (!context_) return;
        bind_depth(depth_);
        for (auto *target : targets_) if (target) target->Release();
        if (depth_) depth_->Release();
    }
    SceneDepthRead(const SceneDepthRead &) = delete;
    SceneDepthRead &operator=(const SceneDepthRead &) = delete;
private:
    void bind_depth(ID3D11DepthStencilView *depth) noexcept
    {
        // 2026-11-16（OM 恢复实测）：OMSetRenderTargets(8) 清掉 slot 3 UAV；只换 DSV 并保留 UAV。
        context_->OMSetRenderTargetsAndUnorderedAccessViews(target_count_, targets_.data(), depth,
            0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
    }
    ID3D11DeviceContext *context_;
    UINT target_count_ = 0;
    std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets_ {};
    ID3D11DepthStencilView *depth_ = nullptr;
};

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

bool float_guide_view(DXGI_FORMAT format, GuideView::Kind kind) noexcept
{
    // 2026-11-16：两个既有 CS 声明 Texture2D<float4>；整数视图不能冒充浮点 guide。
    if (kind == GuideView::Kind::depth)
        return format == DXGI_FORMAT_R32_FLOAT || format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
            format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    switch (format)
    {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R32G32_FLOAT:
        return true;
    default:
        return false;
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

bool GuideView::bind(ID3D11Device &device, ID3D11Texture2D *source, Kind kind, const FrameShape &shape,
    ID3D11ShaderResourceView *source_view) noexcept
{
    if (!source) return false;
    if (source_ == source && view_ && render_width_ == shape.render_width &&
        render_height_ == shape.render_height &&
        (source_view ? view_.Get() == source_view : !supplied_view_)) return true;
    D3D11_TEXTURE2D_DESC desc {};
    source->GetDesc(&desc);
    if (desc.Width < shape.render_width || desc.Height < shape.render_height ||
        desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
        return false;
    if (source_view)
    {
        // 2026-11-16（PID50264 资源复现）：保留当前 Draw 的 typed view，不能用资源格式猜 UNORM/SNORM。
        // 缓存同时认资源和视图，换视图或验证失败时不能借用上一次成功的 guide。
        Microsoft::WRL::ComPtr<ID3D11Resource> viewed;
        source_view->GetResource(viewed.GetAddressOf());
        Microsoft::WRL::ComPtr<ID3D11Device> owner;
        source->GetDevice(owner.GetAddressOf());
        D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
        source_view->GetDesc(&srv);
        if (owner.Get() != &device || viewed.Get() != source ||
            srv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
            srv.Texture2D.MostDetailedMip != 0 || !float_guide_view(srv.Format, kind))
            return false;
        view_ = source_view;
        source_ = source;
        supplied_view_ = true;
        format_ = srv.Format;
        render_width_ = shape.render_width;
        render_height_ = shape.render_height;
        return true;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = kind == Kind::depth ? depth_srv_format(desc.Format) : DXGI_FORMAT_R10G10B10A2_UNORM;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> next;
    if (FAILED(device.CreateShaderResourceView(source, &srv, next.GetAddressOf()))) return false;
    view_ = std::move(next);
    source_ = source;
    supplied_view_ = false;
    format_ = srv.Format;
    render_width_ = shape.render_width;
    render_height_ = shape.render_height;
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

    // 2026-11-17（上游深度缩放）：两个 shader 都读 b0.x，motion 必须保持 1，不能跟随 depth。
    // 默认值复用 immutable CB；非默认只在建池时额外分配，不逐帧 Map。
    const float identity[4] { 1.0f, 0.0f, 0.0f, 0.0f };
    D3D11_SUBRESOURCE_DATA initial { identity, 0, 0 };
    D3D11_BUFFER_DESC cb {};
    cb.ByteWidth = sizeof(identity);
    cb.Usage = D3D11_USAGE_IMMUTABLE;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (const HRESULT hr = device.CreateBuffer(&cb, &initial, pool.constants.GetAddressOf()); FAILED(hr))
        return { hr, "constants" };
    pool.depth_constants = pool.constants;
    if (next.depth_scale != 1.0f)
    {
        const float depth_values[4] { next.depth_scale, 0.0f, 0.0f, 0.0f };
        initial.pSysMem = depth_values;
        pool.depth_constants.Reset();
        if (const HRESULT hr = device.CreateBuffer(&cb, &initial, pool.depth_constants.GetAddressOf()); FAILED(hr))
            return { hr, "depth_constants" };
    }
    pool.shape = next;
    *this = std::move(pool);
    return { S_OK, nullptr };
}

bool FrameGuides::bind(ID3D11Device &device, const ffx12::FrameInput &input, const FrameShape &shape) noexcept
{
    return depth.bind(device, input.depth, GuideView::Kind::depth, shape, input.depth_source_view) &&
        motion.bind(device, input.motion, GuideView::Kind::motion, shape, input.motion_source_view);
}

bool FrameResources::bind_guides(ID3D11Device &device, const ffx12::FrameInput &input) noexcept
{
    return guides.bind(device, input, shape);
}

void FrameResources::record_inputs(ID3D11DeviceContext &context, const ffx12::FrameInput &input,
    ID3D11ComputeShader *depth_cs, ID3D11ComputeShader *motion_cs) const noexcept
{
    const SceneDepthRead depth_read(context, input.depth_from_dsv);
    context.CopyResource(color.Get(), input.color);
    const UINT groups_x = (shape.render_width + 7u) / 8u;
    const UINT groups_y = (shape.render_height + 7u) / 8u;
    ID3D11Buffer *cb[] { depth_constants.Get() };
    context.CSSetConstantBuffers(0, 1, cb);
    ID3D11ShaderResourceView *srvs[] { guides.depth.get() };
    ID3D11UnorderedAccessView *uavs[] { depth_uav.Get(), nullptr };
    context.CSSetShader(depth_cs, nullptr, 0);
    context.CSSetShaderResources(0, 1, srvs);
    context.CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    context.Dispatch(groups_x, groups_y, 1);

    if (depth_constants != constants)
    {
        cb[0] = constants.Get();
        context.CSSetConstantBuffers(0, 1, cb);
    }
    srvs[0] = guides.motion.get();
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
