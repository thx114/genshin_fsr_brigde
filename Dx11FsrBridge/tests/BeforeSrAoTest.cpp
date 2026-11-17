#include "../ngx/BeforeSrAo.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using neo::ngx::AoProjection;
using neo::ngx::BeforeSrAo;

namespace
{
constexpr UINT k_width = 79, k_height = 53;
constexpr std::uint32_t k_scene = 0x71D1A87Bu;
constexpr AoProjection k_normal {0.5f, 100.f, 0.7853981634f, false};
constexpr AoProjection k_reversed {0.5f, 100.f, 0.7853981634f, true};

ComPtr<ID3D11Texture2D> texture(ID3D11Device &device, DXGI_FORMAT format, const void *data, UINT bind)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = k_width;
    desc.Height = k_height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA initial { data, k_width * 4u, 0 };
    ComPtr<ID3D11Texture2D> result;
    if (FAILED(device.CreateTexture2D(&desc, data ? &initial : nullptr, result.GetAddressOf()))) return {};
    return result;
}

std::vector<float> depth_scene(const AoProjection &projection, bool contact)
{
    std::vector<float> depths(k_width * k_height);
    for (UINT y = 0; y < k_height; ++y)
        for (UINT x = 0; x < k_width; ++x)
        {
            // 直接投影两个平行平面；不是从 shader 输出导出预期。底行是清屏/天空对照。
            const float z = contact && x < k_width / 2 ? 3.0f : 3.45f;
            const float normal = projection.far_plane * (z - projection.near_plane) /
                (z * (projection.far_plane - projection.near_plane));
            depths[y * k_width + x] = y == k_height - 1 ? (projection.reversed_depth ? 0.0f : 1.0f) :
                (projection.reversed_depth ? 1.0f - normal : normal);
        }
    return depths;
}

std::array<std::uint32_t, 3> pixels(ID3D11Device &device, ID3D11DeviceContext &context, ID3D11Texture2D &input)
{
    D3D11_TEXTURE2D_DESC desc {};
    input.GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    if (FAILED(device.CreateTexture2D(&desc, nullptr, readback.GetAddressOf()))) return {};
    context.CopyResource(readback.Get(), &input);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context.Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    const auto *data = static_cast<const std::uint8_t *>(mapped.pData);
    std::array<std::uint32_t, 3> values {};
    const auto at = [&](UINT x, UINT y, std::uint32_t &out)
    {
        std::memcpy(&out, data + y * mapped.RowPitch + x * 4, sizeof(out));
    };
    at(k_width / 2 + 2, k_height / 2, values[0]);
    at(k_width - 2, k_height / 2, values[1]);
    at(k_width - 1, k_height - 1, values[2]);
    context.Unmap(readback.Get(), 0);
    return values;
}

bool darker(std::uint32_t source, std::uint32_t filtered)
{
    if ((source >> 24) != (filtered >> 24)) return false;
    for (unsigned shift : {0u, 8u, 16u})
    {
        const unsigned a = (source >> shift) & 255u, b = (filtered >> shift) & 255u;
        if (b < a * 0.55f || b + 2u >= a) return false;
    }
    return true;
}

bool run(ID3D11Device &device, ID3D11DeviceContext &context, DXGI_FORMAT format)
{
    const std::vector<std::uint32_t> scene(k_width * k_height, k_scene);
    const auto color = texture(device, format, scene.data(), D3D11_BIND_SHADER_RESOURCE);
    const auto flat = depth_scene(k_normal, false);
    const auto wall = depth_scene(k_normal, true);
    const auto reversed = depth_scene(k_reversed, true);
    const auto depth = texture(device, DXGI_FORMAT_R32_FLOAT, flat.data(), D3D11_BIND_SHADER_RESOURCE);
    if (!color || !depth) return false;

    ComPtr<ID3D11ShaderResourceView> sentinel;
    if (FAILED(device.CreateShaderResourceView(depth.Get(), nullptr, sentinel.GetAddressOf()))) return false;
    // 覆盖 AO 真正使用的槽；测试不能只盯着从未接触的 slot。
    auto *bound = sentinel.Get();
    context.CSSetShaderResources(1, 1, &bound);
    BeforeSrAo ao;
    const bool flat_out = ao.record(device, context, *color.Get(), *depth.Get(), k_normal);
    const auto producer = ao.color_output();
    D3D11_SHADER_RESOURCE_VIEW_DESC output_view {};
    ComPtr<ID3D11Resource> viewed_output;
    if (producer.view)
    {
        producer.view->GetDesc(&output_view);
        producer.view->GetResource(viewed_output.GetAddressOf());
    }
    const bool producer_ok = producer.texture == color.Get() && viewed_output.Get() == color.Get() &&
        output_view.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
        output_view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
        output_view.Texture2D.MostDetailedMip == 0 && output_view.Texture2D.MipLevels == 1 &&
        producer.transfer == neo::interop::ColorTransfer::unknown;
    ComPtr<ID3D11ShaderResourceView> after;
    context.CSGetShaderResources(1, 1, after.GetAddressOf());
    if (!flat_out || !producer_ok || after.Get() != sentinel.Get()) return false;
    const auto plane = pixels(device, context, *color.Get());
    context.UpdateSubresource(depth.Get(), 0, nullptr, wall.data(), k_width * 4u, 0);
    if (!ao.record(device, context, *color.Get(), *depth.Get(), k_normal)) return false;
    const auto contact = pixels(device, context, *color.Get());
    context.UpdateSubresource(color.Get(), 0, nullptr, scene.data(), k_width * 4u, 0);
    context.UpdateSubresource(depth.Get(), 0, nullptr, reversed.data(), k_width * 4u, 0);
    if (!ao.record(device, context, *color.Get(), *depth.Get(), k_reversed)) return false;
    const auto reverse_contact = pixels(device, context, *color.Get());
    const auto same = std::array { k_scene, k_scene, k_scene };
    const bool input_ok = !ao.record(device, context, *color.Get(), *depth.Get(), {0.5f, 0.0f, 0.0f, false}) &&
        pixels(device, context, *color.Get()) == reverse_contact;
    const bool contact_ok = darker(k_scene, contact[0]) && contact[1] == k_scene && contact[2] == k_scene &&
        reverse_contact == contact;
    std::printf("format=%u plane=%08X wall=%08X reverse=%08X isolated=%d contact=%d\n", format,
        plane[0], contact[0], reverse_contact[0], input_ok, contact_ok);
    return input_ok && plane == same && contact_ok;
}
}

int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf()))) return 2;

    const bool typeless = run(*device.Get(), *context.Get(), DXGI_FORMAT_R8G8B8A8_TYPELESS);
    const bool unorm = run(*device.Get(), *context.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    const auto unsupported = texture(*device.Get(), DXGI_FORMAT_R16G16_FLOAT, nullptr, D3D11_BIND_SHADER_RESOURCE);
    const auto depths = texture(*device.Get(), DXGI_FORMAT_R32_FLOAT, nullptr, D3D11_BIND_SHADER_RESOURCE);
    BeforeSrAo ao;
    const bool bypass = unsupported && depths &&
        !ao.record(*device.Get(), *context.Get(), *unsupported.Get(), *depths.Get(), k_normal);
    return typeless && unorm && bypass ? 0 : 1;
}
