#include "../ngx/BeforeSr.h"

#include <cstdio>

using namespace neo::interop;
using namespace neo::ngx;
using Microsoft::WRL::ComPtr;

extern "C" HRESULT WINAPI Dx11FsrBridge_QueryBeforeSrFrameV1(ID3D11DeviceContext *context,
    ID3D11Resource *color, ID3D11Resource *depth, BeforeSrFrameV1 *out) noexcept;

namespace
{
ComPtr<ID3D11Texture2D> texture(ID3D11Device &device, DXGI_FORMAT format, UINT mips = 1)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 64;
    desc.Height = 32;
    desc.MipLevels = mips;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> result;
    if (FAILED(device.CreateTexture2D(&desc, nullptr, result.GetAddressOf()))) return {};
    return result;
}

ComPtr<ID3D11ShaderResourceView> view(ID3D11Device &device, ID3D11Texture2D &resource, DXGI_FORMAT format,
    UINT base_mip = 0)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
    desc.Format = format;
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MostDetailedMip = base_mip;
    desc.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> result;
    if (FAILED(device.CreateShaderResourceView(&resource, &desc, result.GetAddressOf()))) return {};
    return result;
}
}

int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        device.GetAddressOf(), nullptr, context.GetAddressOf()))) return 2;

    constexpr auto storage = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    constexpr auto srgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    constexpr auto unorm = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr auto unknown = DXGI_FORMAT_UNKNOWN;
    const auto source = texture(*device.Get(), storage);
    const auto color = texture(*device.Get(), storage);
    const auto depth = texture(*device.Get(), DXGI_FORMAT_R32_FLOAT);
    const auto mipped = texture(*device.Get(), storage, 2);
    if (!source || !color || !depth || !mipped) return 2;
    const auto srgb_view = view(*device.Get(), *source.Get(), srgb);
    const auto unorm_view = view(*device.Get(), *source.Get(), unorm);
    const auto copy_view = view(*device.Get(), *color.Get(), srgb);
    const auto mip_view = view(*device.Get(), *mipped.Get(), srgb, 1);
    if (!srgb_view || !unorm_view || !copy_view || !mip_view) return 2;
    context->CopyResource(color.Get(), source.Get());

    BeforeSrInput input { { .frame = 17, .instance = 9, .context = context.Get(), .color = color.Get(),
        .depth = depth.Get(), .render_width = 64, .render_height = 32, .color_storage_format = storage },
        { source.Get() } };
    const auto check = [&](const char *label, ID3D11ShaderResourceView *srv, DXGI_FORMAT expected,
        ColorTransfer transfer = ColorTransfer::unknown)
    {
        input.producer.view = srv;
        BeforeSrQueryScope scope(input);
        BeforeSrFrameV1 out;
        const bool ok = Dx11FsrBridge_QueryBeforeSrFrameV1(context.Get(), color.Get(), depth.Get(), &out) == S_OK &&
            out.frame == 17 && out.instance == 9 && out.color == color.Get() && out.depth == depth.Get() &&
            out.color_storage_format == storage && out.color_view_format == expected &&
            out.color_transfer == transfer;
        std::printf("%s: view=%u expected=%u transfer=%u %s\n", label, static_cast<unsigned>(out.color_view_format),
            static_cast<unsigned>(expected), static_cast<unsigned>(out.color_transfer), ok ? "PASS" : "FAIL");
        return ok;
    };

    // 2026-11-15：游戏局报 storage=27；即使原资源和 NGX 副本形状相同，也不能互相借用视图。
    bool ok = check("source sRGB", srgb_view.Get(), srgb);
    ok &= check("source UNORM", unorm_view.Get(), unorm);
    ok &= check("same-layout copy is not source", copy_view.Get(), unknown);
    ok &= check("no source view", nullptr, unknown);
    input.producer.texture = nullptr;
    ok &= check("no copy source", srgb_view.Get(), unknown);
    // WARP 允许此视图；即使资源匹配，它所读的 mip 也不能代表 mip 0 的色彩。
    input.producer.texture = mipped.Get();
    ok &= check("wrong mip", mip_view.Get(), unknown);
    // 2026-11-16：已认证编码来自测试 producer；格式本身始终不产生认证。
    input.producer = { source.Get(), nullptr, ColorTransfer::srgb };
    ok &= check("declared sRGB via UNORM", unorm_view.Get(), unorm, ColorTransfer::srgb);
    ok &= check("declared sRGB via sRGB view", srgb_view.Get(), srgb, ColorTransfer::srgb);
    ok &= check("declaration cannot borrow copy view", copy_view.Get(), unknown);
    ok &= check("declaration needs view", nullptr, unknown);
    input.producer.texture = nullptr;
    ok &= check("declaration needs texture", srgb_view.Get(), unknown);
    input.producer = { mipped.Get(), nullptr, ColorTransfer::srgb };
    ok &= check("declaration needs mip zero", mip_view.Get(), unknown);
    // AO 已将结果写回 color，其 producer 与 Copy 的原游戏来源是不同资源。
    input.producer = { color.Get() };
    const auto ao_view = view(*device.Get(), *color.Get(), unorm);
    if (!ao_view) return 2;
    ok &= check("AO output UNORM stays unknown transfer", ao_view.Get(), unorm);
    ok &= check("AO cannot borrow original source view", srgb_view.Get(), unknown);
    input.producer.transfer = ColorTransfer::srgb;
    ok &= check("certified AO output", ao_view.Get(), unorm, ColorTransfer::srgb);
    return ok ? 0 : 1;
}
