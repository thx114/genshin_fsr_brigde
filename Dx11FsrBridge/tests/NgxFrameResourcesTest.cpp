#include "../Ffx12Backend.h"
#include "../ngx/FeatureHandle.h"
#include "../ngx/FrameResources.h"
#include "../Ffx12ShaderSource.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>

#include <cstdio>

struct NVSDK_NGX_Handle { unsigned id; }; // 独立测试只用不透明指针身份。

namespace
{
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::puts("Failed: " #__VA_ARGS__); return 1; } } while (false)

unsigned release_count = 0;
unsigned refuse_count = 0;
unsigned crash_count = 0;
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

NVSDK_NGX_Result NVSDK_CONV crash(NVSDK_NGX_Handle *)
{
    ++crash_count;
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    return NVSDK_NGX_Result_Success;
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
    {
        FeatureHandle feature;
        CHECK(feature.adopt(NVSDK_NGX_Result_Success, &owned, crash));
        CHECK(feature.retire() == NVSDK_NGX_Result_Fail && feature.get() == &owned);
        CHECK(feature.retire_exception() == EXCEPTION_ACCESS_VIOLATION);
        CHECK(feature.retire() == NVSDK_NGX_Result_Fail && crash_count == 1);
        CHECK(!feature.adopt(NVSDK_NGX_Result_Success, &unrelated, release));
    }
    CHECK(crash_count == 1);

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
    CHECK(!pool.reactive && !pool.reactive_uav);
    for (bool reactive : { true, false })
    {
        auto variant = *shape;
        variant.reactive_mask = reactive;
        CHECK(SUCCEEDED(pool.rebuild(*device.Get(), variant).hr));
        CHECK((pool.reactive != nullptr) == reactive && (pool.reactive_uav != nullptr) == reactive);
        const D3D_SHADER_MACRO no_reactive[] { { "FFX12_NO_REACTIVE", "1" }, { nullptr, nullptr } };
        ComPtr<ID3DBlob> code;
        CHECK(SUCCEEDED(D3DCompile(ffx12::g_motion_decode_d11_hlsl, sizeof(ffx12::g_motion_decode_d11_hlsl) - 1,
            nullptr, reactive ? nullptr : no_reactive, nullptr, "main", "cs_5_0", 0, 0, code.GetAddressOf(), nullptr)));
        ComPtr<ID3D11ShaderReflection> reflect;
        CHECK(SUCCEEDED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
            __uuidof(ID3D11ShaderReflection), reinterpret_cast<void **>(reflect.GetAddressOf()))));
        D3D11_SHADER_INPUT_BIND_DESC binding {};
        CHECK(SUCCEEDED(reflect->GetResourceBindingDescByName("out_mv", &binding)) && binding.BindPoint == 0);
        CHECK(SUCCEEDED(reflect->GetResourceBindingDescByName("out_reactive", &binding)) == reactive);
        CHECK(!reactive || binding.BindPoint == 1);
    }
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
    const auto *motion_srv = pool.guides.motion.get();
    frame.motion = wrong_mv.Get();
    CHECK(!pool.bind_guides(*device.Get(), frame));
    frame.motion = mv.Get();
    CHECK(pool.bind_guides(*device.Get(), frame) && pool.guides.motion.get() == motion_srv);
    frame.depth = small_depth.Get();
    CHECK(!pool.bind_guides(*device.Get(), frame));

    // 2026-11-16（PID50264）：先复刻日志中的资源描述，单独定位失败的 guide，避免猜槽位。
    auto logged_depth_desc = desc(frame.render_w, frame.render_h, DXGI_FORMAT_R32G8X24_TYPELESS);
    logged_depth_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
    auto logged_motion_desc = desc(frame.render_w, frame.render_h, DXGI_FORMAT_R8G8_TYPELESS);
    logged_motion_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET |
        D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> logged_depth, logged_motion;
    CHECK(SUCCEEDED(device->CreateTexture2D(&logged_depth_desc, nullptr, &logged_depth)));
    CHECK(SUCCEEDED(device->CreateTexture2D(&logged_motion_desc, nullptr, &logged_motion)));
    GuideView logged_depth_view, logged_motion_view;
    const bool logged_depth_bound = logged_depth_view.bind(*device.Get(), logged_depth.Get(),
        GuideView::Kind::depth, pool.shape);
    const bool logged_motion_bound = logged_motion_view.bind(*device.Get(), logged_motion.Get(),
        GuideView::Kind::motion, pool.shape);
    D3D11_SHADER_RESOURCE_VIEW_DESC logged_view_desc {};
    logged_view_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    logged_view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    logged_view_desc.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> incompatible_view, logged_motion_srv;
    const HRESULT incompatible_hr = device->CreateShaderResourceView(logged_motion.Get(),
        &logged_view_desc, &incompatible_view);
    // 受控 UNORM 视图只证明 API 兼容性；现场实际 SRV 与运动编码仍须独立取证。
    logged_view_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    const HRESULT observed_view_hr = device->CreateShaderResourceView(logged_motion.Get(),
        &logged_view_desc, &logged_motion_srv);
    std::printf("PID50264 resource repro: depth19/bind0x48=%d motion48/bind0xA8=%d "
        "forced24_hr=0x%08X supplied49_hr=0x%08X\n", logged_depth_bound, logged_motion_bound,
        static_cast<unsigned>(incompatible_hr), static_cast<unsigned>(observed_view_hr));
    CHECK(logged_depth_bound && !logged_motion_bound && FAILED(incompatible_hr) &&
        SUCCEEDED(observed_view_hr) && logged_motion_srv);
    ComPtr<ID3D11ShaderResourceView> logged_depth_srv, signed_motion_srv;
    logged_view_desc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    CHECK(SUCCEEDED(device->CreateShaderResourceView(logged_depth.Get(), &logged_view_desc, &logged_depth_srv)));
    logged_view_desc.Format = DXGI_FORMAT_R8G8_SNORM;
    CHECK(SUCCEEDED(device->CreateShaderResourceView(logged_motion.Get(), &logged_view_desc, &signed_motion_srv)));
    auto observed_frame = frame;
    observed_frame.depth = logged_depth.Get();
    observed_frame.motion = logged_motion.Get();
    observed_frame.depth_source_view = logged_depth_srv.Get();
    observed_frame.motion_source_view = logged_motion_srv.Get();
    CHECK(pool.bind_guides(*device.Get(), observed_frame));
    CHECK(!pool.guides.known_motion_encoding());
    CHECK(pool.guides.depth.get() == logged_depth_srv.Get() && pool.guides.motion.get() == logged_motion_srv.Get());
    observed_frame.motion_source_view = signed_motion_srv.Get();
    CHECK(pool.bind_guides(*device.Get(), observed_frame) && pool.guides.motion.get() == signed_motion_srv.Get());
    observed_frame.motion = mv.Get();
    CHECK(!pool.bind_guides(*device.Get(), observed_frame));
    observed_frame.motion = logged_motion.Get();
    observed_frame.motion_source_view = nullptr;
    CHECK(!pool.bind_guides(*device.Get(), observed_frame));
    observed_frame.motion_source_view = logged_motion_srv.Get();
    CHECK(pool.bind_guides(*device.Get(), observed_frame) && pool.guides.motion.get() == logged_motion_srv.Get());
    std::puts("guide views: supplied format49/51 kept; foreign resource and missing evidence rejected");

    // 2026-11-16：解除 DSV 写绑定不能吞掉原有 OM UAV；先核验前置状态，再执行被测路径。
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.GetAddressOf());
    auto scene_desc = desc(frame.render_w, frame.render_h, DXGI_FORMAT_R32_TYPELESS);
    scene_desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> scene_depth, scene_color;
    ComPtr<ID3D11DepthStencilView> scene_dsv;
    ComPtr<ID3D11UnorderedAccessView> scene_uav;
    CHECK(SUCCEEDED(device->CreateTexture2D(&scene_desc, nullptr, &scene_depth)));
    CHECK(SUCCEEDED(device->CreateTexture2D(&color, nullptr, &scene_color)));
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc {};
    dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    CHECK(SUCCEEDED(device->CreateDepthStencilView(scene_depth.Get(), &dsv_desc, &scene_dsv)));
    CHECK(SUCCEEDED(device->CreateUnorderedAccessView(pool.output.Get(), nullptr, &scene_uav)));
    context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, scene_dsv.Get(), 3, 1,
        scene_uav.GetAddressOf(), nullptr);
    ComPtr<ID3D11DepthStencilView> bound_depth;
    ComPtr<ID3D11UnorderedAccessView> bound_uav;
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, &bound_depth, 3, 1, &bound_uav);
    const bool initial_bindings = bound_depth == scene_dsv && bound_uav == scene_uav;
    CHECK(initial_bindings);
    constexpr char noop[] = "[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {}";
    ComPtr<ID3DBlob> noop_code;
    ComPtr<ID3D11ComputeShader> noop_shader;
    CHECK(SUCCEEDED(D3DCompile(noop, sizeof(noop), nullptr, nullptr, nullptr,
        "main", "cs_5_0", 0, 0, &noop_code, nullptr)));
    CHECK(SUCCEEDED(device->CreateComputeShader(noop_code->GetBufferPointer(),
        noop_code->GetBufferSize(), nullptr, &noop_shader)));
    frame.color = scene_color.Get();
    frame.depth = scene_depth.Get();
    frame.depth_from_dsv = true;
    CHECK(pool.bind_guides(*device.Get(), frame));
    pool.record_inputs(*context.Get(), frame, noop_shader.Get(), noop_shader.Get());
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr,
        bound_depth.ReleaseAndGetAddressOf(), 3, 1, bound_uav.ReleaseAndGetAddressOf());
    std::printf("scene OM restore: initial=%d dsv=%d uav=%d\n",
        initial_bindings, bound_depth == scene_dsv, bound_uav == scene_uav);
    const bool restored_bindings = bound_depth == scene_dsv && bound_uav == scene_uav;
    CHECK(restored_bindings);
    context->ClearState();

    auto resized = *shape;
    resized.output_width *= 2;
    CHECK(SUCCEEDED(pool.rebuild(*device.Get(), resized).hr) && pool.shape == resized && pool.output != before);
    CHECK(pool.guides.depth.get() == nullptr && pool.guides.motion.get() == nullptr);
    std::puts("NGX: feature ownership, invalid copy layouts, WARP pool rollback and guide rebinding passed");
    return 0;
}
