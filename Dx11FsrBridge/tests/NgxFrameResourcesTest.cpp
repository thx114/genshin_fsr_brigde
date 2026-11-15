#include "../Ffx12Backend.h"
#include "../ngx/FeatureHandle.h"
#include "../ngx/FrameResources.h"

#include <cstdio>

struct NVSDK_NGX_Handle { unsigned id; }; // 独立测试只用不透明指针身份。

namespace
{
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::puts("Failed: " #__VA_ARGS__); return 1; } } while (false)

unsigned release_count = 0;
unsigned refuse_count = 0;
NVSDK_NGX_Handle *released = nullptr;
NVSDK_NGX_Result NVSDK_CONV release(NVSDK_NGX_Handle *handle)
{
    ++release_count;
    released = handle;
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV refuse(NVSDK_NGX_Handle *)
{
    ++refuse_count;
    return NVSDK_NGX_Result_Fail;
}

D3D11_TEXTURE2D_DESC desc(UINT width, UINT height, DXGI_FORMAT format)
{
    D3D11_TEXTURE2D_DESC d {};
    d.Width = width;
    d.Height = height;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = format;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return d;
}
}

int main()
{
    using namespace neo::ngx;
    NVSDK_NGX_Handle owned { 17 }, unrelated { 99 };
    {
        FeatureHandle feature;
        CHECK(!feature.adopt(NVSDK_NGX_Result_FAIL_FeatureAlreadyExists, &unrelated, release));
        CHECK(!feature.adopt(NVSDK_NGX_Result_Success, nullptr, release));
        CHECK(release_count == 0 && !feature);
        CHECK(feature.adopt(NVSDK_NGX_Result_Success, &owned, release));
        CHECK(feature.get() == &owned);
        CHECK(feature.retire() == NVSDK_NGX_Result_Success);
        CHECK(feature.retire() == NVSDK_NGX_Result_Success);
        CHECK(release_count == 1 && released == &owned);
        CHECK(feature.adopt(NVSDK_NGX_Result_Success, &unrelated, release));
    }
    CHECK(release_count == 2 && released == &unrelated);
    {
        FeatureHandle feature;
        CHECK(feature.adopt(NVSDK_NGX_Result_Success, &owned, refuse));
        CHECK(feature.retire() == NVSDK_NGX_Result_Fail && feature.get() == &owned);
        CHECK(feature.retire() == NVSDK_NGX_Result_Fail && refuse_count == 1);
        CHECK(!feature.adopt(NVSDK_NGX_Result_Success, &unrelated, release));
    }
    CHECK(refuse_count == 1);

    ffx12::FrameInput frame;
    frame.render_w = 64;
    frame.render_h = 32;
    frame.display_w = 128;
    frame.display_h = 64;
    const auto color = desc(frame.render_w, frame.render_h, DXGI_FORMAT_R10G10B10A2_UNORM);
    auto output = desc(frame.display_w, frame.display_h, DXGI_FORMAT_R8G8B8A8_UNORM);
    const auto shape = copy_shape(frame, color, output);
    CHECK(shape && shape->color_format == color.Format && shape->output_format == output.Format);
    output.Width++;
    CHECK(!copy_shape(frame, color, output));
    output.Width--;
    output.MipLevels++;
    CHECK(!copy_shape(frame, color, output));
    output.MipLevels--;
    output.Usage = D3D11_USAGE_IMMUTABLE;
    CHECK(!copy_shape(frame, color, output));

    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr)));
    FrameResources pool;
    auto unsupported = *shape;
    unsupported.output_format = DXGI_FORMAT_UNKNOWN;
    CHECK(FAILED(pool.rebuild(*device.Get(), unsupported).hr) && !pool.ready());
    CHECK(SUCCEEDED(pool.rebuild(*device.Get(), *shape).hr) && pool.ready() && pool.shape == *shape);
    const auto before = pool.output;
    CHECK(FAILED(pool.rebuild(*device.Get(), unsupported).hr) && pool.output == before && pool.shape == *shape);

    ComPtr<ID3D11Texture2D> depth, mv, wrong_mv, small_depth;
    const auto depth_desc = desc(frame.render_w, frame.render_h, DXGI_FORMAT_R32_FLOAT);
    const auto mv_desc = desc(frame.render_w, frame.render_h, DXGI_FORMAT_R10G10B10A2_UNORM);
    const auto wrong_mv_desc = desc(frame.render_w, frame.render_h, DXGI_FORMAT_R16G16_FLOAT);
    const auto small_desc = desc(frame.render_w - 1, frame.render_h, DXGI_FORMAT_R32_FLOAT);
    CHECK(SUCCEEDED(device->CreateTexture2D(&depth_desc, nullptr, depth.GetAddressOf())));
    CHECK(SUCCEEDED(device->CreateTexture2D(&mv_desc, nullptr, mv.GetAddressOf())));
    CHECK(SUCCEEDED(device->CreateTexture2D(&wrong_mv_desc, nullptr, wrong_mv.GetAddressOf())));
    CHECK(SUCCEEDED(device->CreateTexture2D(&small_desc, nullptr, small_depth.GetAddressOf())));
    frame.depth = depth.Get();
    frame.motion = mv.Get();
    CHECK(pool.bind_guides(*device.Get(), frame));
    const auto *motion_srv = pool.motion_src.get();
    frame.motion = wrong_mv.Get();
    CHECK(!pool.bind_guides(*device.Get(), frame));
    frame.motion = mv.Get();
    CHECK(pool.bind_guides(*device.Get(), frame) && pool.motion_src.get() == motion_srv);
    frame.depth = small_depth.Get();
    CHECK(!pool.bind_guides(*device.Get(), frame));

    auto resized = *shape;
    resized.output_width *= 2;
    CHECK(SUCCEEDED(pool.rebuild(*device.Get(), resized).hr) && pool.shape == resized && pool.output != before);
    CHECK(pool.depth_src.get() == nullptr && pool.motion_src.get() == nullptr);
    std::puts("NGX: feature ownership, invalid copy layouts, WARP pool rollback and guide rebinding passed");
    return 0;
}
