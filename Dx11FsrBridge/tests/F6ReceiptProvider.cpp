#include <d3d11.h>
#include <wrl/client.h>
#include "../ngx/F6EvalReceiptV1.h"
#include "../interop/BeforeSrV1.h"
#include "../third_party/ngx/nvsdk_ngx_params.h"
#include "../Il2CppCallSiteHook.h"
#include "../runtime/FrameTimeline.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <variant>

#if !defined(BEFORE_SR_LEGACY_FIXTURE)
#include "NgxInputMapFixture.h"
#include <DirectXPackedVector.h>
#include <bit>
#include <cmath>
#include <cstring>
#endif

struct NVSDK_NGX_Handle { unsigned id; };

namespace
{
class Parameters final : public NVSDK_NGX_Parameter
{
public:
#define PARAMETER_TYPE(T) \
    void Set(const char *name, T value) override { values_[name] = value; } \
    NVSDK_NGX_Result Get(const char *name, T *value) const override { return get(name, value); }
    PARAMETER_TYPE(unsigned long long)
    PARAMETER_TYPE(float)
    PARAMETER_TYPE(double)
    PARAMETER_TYPE(unsigned int)
    PARAMETER_TYPE(int)
    PARAMETER_TYPE(ID3D11Resource *)
    PARAMETER_TYPE(ID3D12Resource *)
    PARAMETER_TYPE(void *)
#undef PARAMETER_TYPE
    void Reset() override { values_.clear(); }

private:
    using Value = std::variant<unsigned long long, float, double, unsigned int, int,
        ID3D11Resource *, ID3D12Resource *, void *>;
    std::unordered_map<std::string, Value> values_;
    template <typename T>
    NVSDK_NGX_Result get(const char *name, T *value) const
    {
        const auto it = values_.find(name);
        if (it == values_.end() || !value) return NVSDK_NGX_Result_Fail;
        const auto *stored = std::get_if<T>(&it->second);
        if (!stored) return NVSDK_NGX_Result_Fail;
        *value = *stored;
        return NVSDK_NGX_Result_Success;
    }
};

Parameters g_main, g_query;
bool g_hudless_fixture = false;
ID3D11Resource *g_hudless_expected = nullptr;
unsigned g_hudless_flip = 0, g_hudless_result = 0;
Microsoft::WRL::ComPtr<ID3D11Texture2D> g_hudless_held;
struct FixtureFeature
{
    NVSDK_NGX_Handle handle {};
    bool active = false;
};
std::array<FixtureFeature, il2cpp_callsite::k_render_instance_capacity> g_features {};
unsigned g_feature_id = 16;
unsigned g_feature_releases = 0;
unsigned g_fault = 0, g_fault_calls = 0;
extern "C" __declspec(dllexport) void ConfigureProviderFault(unsigned fault) { g_fault = fault; }
extern "C" __declspec(dllexport) unsigned ProviderFaultCalls() { return g_fault_calls; }
void provider_fault(unsigned boundary)
{
    if (g_fault != boundary) return;
    ++g_fault_calls;
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
}
extern "C" __declspec(dllexport) unsigned ReceiptFixtureCreates() { return g_feature_id - 16; }
extern "C" __declspec(dllexport) unsigned ReceiptFixtureReleases() { return g_feature_releases; }
bool active_feature(const NVSDK_NGX_Handle *handle)
{
    for (const auto &feature : g_features)
        if (feature.active && &feature.handle == handle) return true;
    return false;
}
unsigned g_params_issued = 0;
unsigned g_ao_frames = 0;
bool g_ao_fixture = false;
bool g_scene_depth_fixture = false;
float g_scene_depth_expected = 0.5f;
extern "C" __declspec(dllexport) void ConfigureDepthExpected(float value) { g_scene_depth_expected = value; }
#if !defined(BEFORE_SR_LEGACY_FIXTURE)
unsigned g_eval = 0;
unsigned g_receipt_display_width = 1024, g_receipt_display_height = 1024;
struct InputMapState
{
    bool enabled = false, delayed = false;
    std::uint64_t keys[2] {};
    unsigned create_flags = 0, create_count = 0;
    std::array<std::uint64_t, 2> last_evaluate_tick {};
};
InputMapState g_input_map;
#endif
}

#define EXPORT extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV

extern "C" __declspec(dllexport) void ConfigureHudlessFixture(ID3D11Resource *expected,
    unsigned flip, unsigned result) noexcept
{
    g_hudless_fixture = true;
    g_hudless_expected = expected;
    g_hudless_flip = flip;
    g_hudless_result = result;
    g_hudless_held.Reset();
}
extern "C" __declspec(dllexport) ID3D11Texture2D *HudlessFixtureHeld() noexcept { return g_hudless_held.Get(); }
extern "C" __declspec(dllexport) bool HudlessFixtureClosed() noexcept
{
    ID3D11Resource *color = g_hudless_expected;
    unsigned flip = 9;
    const bool closed = g_main.Get("Opti.Hudless", &color) == NVSDK_NGX_Result_Success && !color &&
        g_main.Get("Opti.Hudless.Flip", &flip) == NVSDK_NGX_Result_Success && flip == 0;
    g_hudless_held.Reset();
    return closed;
}

static std::atomic_uint g_init_calls {};
static std::atomic_bool g_refuse_first_init {};
extern "C" __declspec(dllexport) unsigned ReceiptInitCalls() { return g_init_calls.load(); }
extern "C" __declspec(dllexport) void RefuseEarlyInit() { g_refuse_first_init.store(true); }

EXPORT NVSDK_NGX_D3D11_Init_Ext(unsigned long long, const wchar_t *, ID3D11Device *,
    NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *)
{
    ++g_init_calls;
    provider_fault(1);
    if (g_refuse_first_init.exchange(false)) return NVSDK_NGX_Result_Fail;
    return NVSDK_NGX_Result_Success;
}

EXPORT NVSDK_NGX_D3D11_GetCapabilityParameters(NVSDK_NGX_Parameter **p)
{
    auto &params = g_params_issued++ == 0 ? g_main : g_query;
    params.Set(NVSDK_NGX_Parameter_SuperSampling_Available, 1);
    *p = &params;
    return NVSDK_NGX_Result_Success;
}

EXPORT NVSDK_NGX_D3D11_AllocateParameters(NVSDK_NGX_Parameter **p) { return NVSDK_NGX_D3D11_GetCapabilityParameters(p); }
EXPORT NVSDK_NGX_D3D11_DestroyParameters(NVSDK_NGX_Parameter *) { return NVSDK_NGX_Result_Success; }
EXPORT NVSDK_NGX_D3D11_Shutdown1(ID3D11Device *) { return NVSDK_NGX_Result_Success; }
EXPORT NVSDK_NGX_D3D11_GetScratchBufferSize(NVSDK_NGX_Feature, const NVSDK_NGX_Parameter *, size_t *bytes)
{
    *bytes = 0;
    return NVSDK_NGX_Result_Success;
}
EXPORT NVSDK_NGX_D3D11_GetFeatureRequirements(IDXGIAdapter *, const NVSDK_NGX_FeatureDiscoveryInfo *,
    NVSDK_NGX_FeatureRequirement *req)
{
    *req = {};
    return NVSDK_NGX_Result_Success;
}
EXPORT NVSDK_NGX_D3D11_CreateFeature(ID3D11DeviceContext *, NVSDK_NGX_Feature kind,
    [[maybe_unused]] NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out)
{
    if (!out || kind != NVSDK_NGX_Feature_SuperSampling) return NVSDK_NGX_Result_Fail;
    provider_fault(2);
#if !defined(BEFORE_SR_LEGACY_FIXTURE)
    if (g_input_map.enabled)
    {
        if (!p || p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &g_input_map.create_flags) !=
            NVSDK_NGX_Result_Success) return NVSDK_NGX_Result_Fail;
        ++g_input_map.create_count;
    }
#endif
    for (auto &feature : g_features)
        if (!feature.active)
        {
            feature.handle.id = ++g_feature_id;
            feature.active = true;
            *out = &feature.handle;
            return NVSDK_NGX_Result_Success;
        }
    return NVSDK_NGX_Result_Fail;
}
EXPORT NVSDK_NGX_D3D11_ReleaseFeature(NVSDK_NGX_Handle *handle)
{
    provider_fault(4);
    for (auto &feature : g_features)
        if (feature.active && handle == &feature.handle)
        {
            feature.active = false;
            ++g_feature_releases;
            return NVSDK_NGX_Result_Success;
        }
    return NVSDK_NGX_Result_Fail;
}
extern "C" __declspec(dllexport) void ConfigureBeforeSrAoFixture() noexcept { g_ao_fixture = true; }
extern "C" __declspec(dllexport) unsigned BeforeSrAoFixtureFrames() noexcept { return g_ao_frames; }
extern "C" __declspec(dllexport) void ConfigureSceneDepthFixture() noexcept
{
    g_ao_fixture = g_scene_depth_fixture = true;
}
#if !defined(BEFORE_SR_LEGACY_FIXTURE)
extern "C" __declspec(dllexport) void ConfigureReceiptFixtureDisplay(unsigned width, unsigned height) noexcept
{
    // 2026-11-16：由宿主给出独立期望，不能拿桥返回的尺寸给桥自身作证。
    g_receipt_display_width = width;
    g_receipt_display_height = height;
}
#endif

static bool scene_depth_matches(ID3D11DeviceContext *context, const NVSDK_NGX_Parameter *p)
{
    if (!g_scene_depth_fixture) return true;
    // 2026-11-16：只在夹具读回 SDK 实际深度；同尺寸无关 DSV 的 0.25 必须被检测出来。
    using Microsoft::WRL::ComPtr;
    ID3D11Resource *depth = nullptr;
    if (p->Get(NVSDK_NGX_Parameter_Depth, &depth) != NVSDK_NGX_Result_Success || !depth) return false;
    ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> sample;
    if (!device || FAILED(device->CreateTexture2D(&desc, nullptr, sample.GetAddressOf()))) return false;
    const D3D11_BOX box {256, 256, 0, 257, 257, 1};
    context->CopySubresourceRegion(sample.Get(), 0, 0, 0, 0, depth, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(sample.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    float value = 0.f;
    std::memcpy(&value, mapped.pData, sizeof(value));
    context->Unmap(sample.Get(), 0);
    std::printf("scene depth source: actual=%.3f expected_fsr=%.3f unrelated_dsv=0.250\n",
        value, g_scene_depth_expected);
    return value == g_scene_depth_expected;
}

static NVSDK_NGX_Result copy_eval_color(ID3D11DeviceContext *context, const NVSDK_NGX_Handle *handle,
    const NVSDK_NGX_Parameter *p)
{
    ID3D11Resource *color = nullptr, *output = nullptr;
    unsigned render = 0, height = 0;
    if (!context || !p || !active_feature(handle) ||
        p->Get(NVSDK_NGX_Parameter_Color, &color) != NVSDK_NGX_Result_Success || !color ||
        p->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success || !output ||
        p->Get(NVSDK_NGX_Parameter_Width, &render) != NVSDK_NGX_Result_Success || !render ||
        p->Get(NVSDK_NGX_Parameter_Height, &height) != NVSDK_NGX_Result_Success || !height)
        return NVSDK_NGX_Result_Fail;
    if (!scene_depth_matches(context, p)) return NVSDK_NGX_Result_Fail;
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Texture2D> target;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(target.GetAddressOf())))) return NVSDK_NGX_Result_Fail;
    D3D11_TEXTURE2D_DESC desc {};
    target->GetDesc(&desc);
    // 从实际绑定给 Evaluate 的颜色取一个精确像素，不用固定输出掩盖接错或调用顺序错误。
    const D3D11_BOX center { render / 2, height / 2, 0, render / 2 + 1, height / 2 + 1, 1 };
    context->CopySubresourceRegion(output, 0, desc.Width / 2, desc.Height / 2, 0, color, 0, &center);
    if (g_ao_fixture) ++g_ao_frames;
    return NVSDK_NGX_Result_Success;
}

#if defined(BEFORE_SR_LEGACY_FIXTURE)
EXPORT NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext *context, const NVSDK_NGX_Handle *handle,
    const NVSDK_NGX_Parameter *p, PFN_NVSDK_NGX_ProgressCallback)
{
    provider_fault(3);
    return copy_eval_color(context, handle, p);
}
#else
namespace
{
using namespace neo::tests::input_map;
using Microsoft::WRL::ComPtr;
using GuideSamples = std::array<std::uint32_t, k_probes.size()>;

bool matches_texture(ID3D11Resource &resource, UINT width, UINT height, DXGI_FORMAT format)
{
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.QueryInterface(IID_PPV_ARGS(texture.GetAddressOf())))) return false;
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    if (desc.Width == width && desc.Height == height && desc.Format == format && desc.MipLevels == 1 &&
        desc.ArraySize == 1 && desc.SampleDesc.Count == 1) return true;
    std::printf("binding mismatch: texture=%ux%u format=%u, expected=%ux%u format=%u\n",
        desc.Width, desc.Height, static_cast<unsigned>(desc.Format), width, height, static_cast<unsigned>(format));
    return false;
}

bool sample_guide(ID3D11DeviceContext &context, ID3D11Device &device, ID3D11Resource &resource,
    DXGI_FORMAT format, GuideSamples &samples)
{
    if (!matches_texture(resource, k_render_width, k_render_height, format)) return false;
    // 2026-11-15（PR #16）：仅夹具读回 Evaluate 已绑定的 mip 0，避免把游戏源描述误当成最终值。
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = static_cast<UINT>(samples.size());
    desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device.CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return false;
    for (UINT i = 0; i < samples.size(); ++i)
    {
        const auto x = static_cast<UINT>(k_probes[i].x), y = static_cast<UINT>(k_probes[i].y);
        const D3D11_BOX box { x, y, 0, x + 1, y + 1, 1 };
        context.CopySubresourceRegion(staging.Get(), 0, i, 0, 0, &resource, 0, &box);
    }
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context.Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    std::memcpy(samples.data(), mapped.pData, samples.size() * sizeof(samples[0]));
    context.Unmap(staging.Get(), 0);
    return true;
}

template <typename T>
bool get_binding(const NVSDK_NGX_Parameter &p, const char *name, T &value)
{
    if (p.Get(name, &value) == NVSDK_NGX_Result_Success) return true;
    std::printf("binding missing: %s\n", name);
    return false;
}

NVSDK_NGX_Result evaluate_input_map(ID3D11DeviceContext &context, const NVSDK_NGX_Handle *handle,
    const NVSDK_NGX_Parameter &p, std::uint64_t request, neo::ngx::F6EvalReceiptV1 &receipt, unsigned ordinal)
{
    using namespace neo::interop;
    if (ordinal > std::size(k_frames)) return NVSDK_NGX_Result_Fail;
    const auto &test = k_frames[ordinal - 1];
    ID3D11Resource *color = nullptr, *output = nullptr, *depth = nullptr, *motion = nullptr;
    unsigned w = 0, h = 0, ow = 0, oh = 0, sub_w = 0, sub_h = 0;
    float jx = 0.f, jy = 0.f, sx = 0.f, sy = 0.f;
    int reset = 0;
    if (!get_binding(p, NVSDK_NGX_Parameter_Color, color) || !color ||
        !get_binding(p, NVSDK_NGX_Parameter_Output, output) || !output ||
        !get_binding(p, NVSDK_NGX_Parameter_Depth, depth) || !depth ||
        !get_binding(p, NVSDK_NGX_Parameter_MotionVectors, motion) || !motion ||
        !get_binding(p, NVSDK_NGX_Parameter_Width, w) || !get_binding(p, NVSDK_NGX_Parameter_Height, h) ||
        !get_binding(p, NVSDK_NGX_Parameter_OutWidth, ow) || !get_binding(p, NVSDK_NGX_Parameter_OutHeight, oh) ||
        !get_binding(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, sub_w) ||
        !get_binding(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, sub_h) ||
        !get_binding(p, NVSDK_NGX_Parameter_Jitter_Offset_X, jx) || !get_binding(p, NVSDK_NGX_Parameter_Jitter_Offset_Y, jy) ||
        !get_binding(p, NVSDK_NGX_Parameter_MV_Scale_X, sx) || !get_binding(p, NVSDK_NGX_Parameter_MV_Scale_Y, sy) ||
        !get_binding(p, NVSDK_NGX_Parameter_Reset, reset)) return NVSDK_NGX_Result_Fail;

    const auto query = reinterpret_cast<QueryBeforeSrFrameV1>(GetProcAddress(
        GetModuleHandleW(L"Dx11FsrBridge.dll"), k_query_before_sr_frame_v1));
    BeforeSrFrameV1 frame;
    if (!query || query(&context, color, depth, &frame) != S_OK || frame.motion != motion)
        return NVSDK_NGX_Result_Fail;
    const float expected_x = g_input_map.delayed ? test.delayed_x : test.current_x;
    const float expected_y = g_input_map.delayed ? test.delayed_y : test.current_y;
    constexpr unsigned k_expected_flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
        NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    constexpr unsigned k_guide_flags = k_expected_flags | NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    const bool scale_ok = sx == k_render_width && sy == k_render_height;
    const auto now = GetTickCount64();
    const auto previous = g_input_map.last_evaluate_tick[test.instance];
    const bool expected_reset = test.reset || (previous != 0 &&
        now - previous > neo::graphics::FrameTimeline::k_takeover_gap_ms);
    g_input_map.last_evaluate_tick[test.instance] = now;
    const bool bindings_ok = w == k_render_width && h == k_render_height && sub_w == w && sub_h == h &&
        ow == k_display_width && oh == k_display_height && jx == expected_x && jy == expected_y && scale_ok &&
        reset == int(expected_reset) && frame.frame == ordinal &&
        frame.instance == g_input_map.keys[test.instance] && frame.jitter_x == jx && frame.jitter_y == jy &&
        frame.motion_scale_x == sx && frame.motion_scale_y == sy &&
        (g_input_map.create_flags & k_guide_flags) == k_expected_flags;
    std::printf("evaluate frame=%llu key=0x%llX render=%ux%u subrect=%ux%u output=%ux%u jitter_px=(%.6f,%.6f) "
        "expected=(%.6f,%.6f) mv_scale=(%.1f,%.1f) scale_ok=%d reset=%d expected_reset=%d "
        "create_flags=0x%X create_count_total=%u request=%llu ok=%d\n",
        static_cast<unsigned long long>(frame.frame), static_cast<unsigned long long>(frame.instance),
        w, h, sub_w, sub_h, ow, oh, jx, jy, expected_x, expected_y, sx, sy, scale_ok, reset, expected_reset,
        g_input_map.create_flags, g_input_map.create_count, static_cast<unsigned long long>(request), bindings_ok);

    ComPtr<ID3D11Device> device;
    context.GetDevice(device.GetAddressOf());
    GuideSamples depth_samples {}, motion_samples {};
    if (!device || !sample_guide(context, *device.Get(), *depth, DXGI_FORMAT_R32_FLOAT, depth_samples) ||
        !sample_guide(context, *device.Get(), *motion, DXGI_FORMAT_R16G16_FLOAT, motion_samples))
        return NVSDK_NGX_Result_Fail;

    constexpr float k_reprojection_tolerance = 0.30f;
    using DirectX::PackedVector::XMConvertHalfToFloat;
    bool guides_ok = true;
    for (std::size_t i = 0; i < k_probes.size(); ++i)
    {
        const auto &c = k_probes[i];
        const auto &sample = guide_at(ordinal - 1, i);
        const auto delta = sample.previous_minus_current;
        const auto expected = Point { c.x + delta.x, c.y + delta.y };
        const float mx = XMConvertHalfToFloat(static_cast<std::uint16_t>(motion_samples[i]));
        const float my = XMConvertHalfToFloat(static_cast<std::uint16_t>(motion_samples[i] >> 16));
        const float px = c.x + mx * sx, py = c.y + my * sy;
        const auto z = std::bit_cast<float>(depth_samples[i]);
        const bool sign_ok = (delta.x == 0 || mx * delta.x > 0) && (delta.y == 0 || my * delta.y > 0);
        const bool reprojection_ok = std::abs(px - expected.x) <= k_reprojection_tolerance &&
            std::abs(py - expected.y) <= k_reprojection_tolerance;
        const bool depth_ok = z == sample.depth;
        std::printf("guide frame=%u xy=(%d,%d) expected_source_rg10=(%u,%u) depth_to_R32F=%.7g->%.7g depth_ok=%d "
            "motion_RG16F=(%.7g,%.7g) sign_ok=%d previous=(%.3f,%.3f) expected=(%d,%d) reprojection_ok=%d\n",
            ordinal, c.x, c.y, sample.packed_mv & 1023u, (sample.packed_mv >> 10) & 1023u, sample.depth, z, depth_ok,
            mx, my, sign_ok, px, py, expected.x, expected.y, reprojection_ok);
        guides_ok &= depth_ok && sign_ok && reprojection_ok;
    }

    constexpr auto k_color_format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if (!bindings_ok || !guides_ok || !matches_texture(*color, w, h, k_color_format) ||
        !matches_texture(*output, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM)) return NVSDK_NGX_Result_Fail;
    const D3D11_BOX center { w / 2, h / 2, 0, w / 2 + 1, h / 2 + 1, 1 };
    context.CopySubresourceRegion(output, 0, ow / 2, oh / 2, 0, color, 0, &center);
    receipt.request_id = request;
    receipt.context = reinterpret_cast<std::uintptr_t>(&context);
    receipt.handle = reinterpret_cast<std::uintptr_t>(handle);
    receipt.output = reinterpret_cast<std::uintptr_t>(output);
    receipt.provider_frame = ordinal;
    receipt.status = neo::ngx::F6OutputStatus::current_output_ordered;
    return NVSDK_NGX_Result_Success;
}
}

extern "C" __declspec(dllexport) void ConfigureNgxInputMapFixture(bool delayed,
    std::uint64_t key_a, std::uint64_t key_b) noexcept
{
    g_input_map = { true, delayed, { key_a, key_b } };
}

extern "C" __declspec(dllexport) unsigned NgxInputMapFixtureEvaluateCount() noexcept { return g_eval; }

EXPORT NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext *, const NVSDK_NGX_Handle *,
    const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback)
{
    std::puts("receipt fixture: unexpected legacy Evaluate");
    return NVSDK_NGX_Result_Fail;
}
EXPORT F6_NGX_D3D11_EvaluateFeatureV1(ID3D11DeviceContext *context, const NVSDK_NGX_Handle *handle,
    NVSDK_NGX_Parameter *p, PFN_NVSDK_NGX_ProgressCallback, std::uint64_t request, neo::ngx::F6EvalReceiptV1 *receipt)
{
    using namespace neo::ngx;
    const unsigned input_map_ordinal = g_input_map.enabled ? ++g_eval : 0;
    if (!context || !p || !active_feature(handle) || !receipt ||
        receipt->size != sizeof(F6EvalReceiptV1) || receipt->version != 1)
        return NVSDK_NGX_Result_Fail;
    if (input_map_ordinal != 0)
        return evaluate_input_map(*context, handle, *p, request, *receipt, input_map_ordinal);
    if (g_hudless_fixture)
    {
        if (g_hudless_result == 3)
        {
            g_fault = 5;
            provider_fault(5);
        }
        ID3D11Resource *color = nullptr, *output = nullptr;
        unsigned flip = 9;
        if (p->Get("Opti.Hudless", &color) != NVSDK_NGX_Result_Success || color != g_hudless_expected ||
            p->Get("Opti.Hudless.Flip", &flip) != NVSDK_NGX_Result_Success || flip != g_hudless_flip ||
            p->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success || !output ||
            (color && FAILED(color->QueryInterface(IID_PPV_ARGS(&g_hudless_held)))))
            return NVSDK_NGX_Result_Fail;
        receipt->request_id = request;
        receipt->context = reinterpret_cast<std::uintptr_t>(context);
        receipt->handle = reinterpret_cast<std::uintptr_t>(handle);
        receipt->output = reinterpret_cast<std::uintptr_t>(output);
        receipt->status = g_hudless_result == 2 ? F6OutputStatus::no_output : F6OutputStatus::current_output_ordered;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
        if (FAILED(device->CreateUnorderedAccessView(output, nullptr, &uav))) return NVSDK_NGX_Result_Fail;
        constexpr float pixel[] {192.f / 1023.f, 704.f / 1023.f, 352.f / 1023.f, 1.f};
        context->ClearUnorderedAccessViewFloat(uav.Get(), pixel);
        return g_hudless_result == 1 ? NVSDK_NGX_Result_Fail : NVSDK_NGX_Result_Success;
    }
    if (g_ao_fixture)
    {
        using namespace neo::interop;
        ID3D11Resource *color = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr;
        if (!get_binding(*p, NVSDK_NGX_Parameter_Color, color) || !color ||
            !get_binding(*p, NVSDK_NGX_Parameter_Depth, depth) || !depth ||
            !get_binding(*p, NVSDK_NGX_Parameter_MotionVectors, motion) || !motion ||
            !get_binding(*p, NVSDK_NGX_Parameter_Output, output) || !output)
            return NVSDK_NGX_Result_Fail;
        const auto query = reinterpret_cast<QueryBeforeSrFrameV1>(GetProcAddress(
            GetModuleHandleW(L"Dx11FsrBridge.dll"), k_query_before_sr_frame_v1));
        BeforeSrFrameV1 input, unrelated;
        // 2026-11-16：AO 写回颜色必须有自己的 UNORM 视图；不把原游戏 SRV 或未知编码冒充声明。
        if (!query || query(context, color, depth, &input) != S_OK || input.color != color ||
            input.motion != motion || input.depth != depth || input.frame != g_ao_frames + 1 ||
            input.color_storage_format != DXGI_FORMAT_R8G8B8A8_TYPELESS ||
            input.color_view_format != DXGI_FORMAT_R8G8B8A8_UNORM || input.color_transfer != ColorTransfer::unknown ||
            query(context, output, depth, &unrelated) != S_FALSE)
        {
            std::printf("AO producer contract mismatch: view=%u expected=%u transfer=%u\n",
                static_cast<unsigned>(input.color_view_format),
                static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM), static_cast<unsigned>(input.color_transfer));
            return NVSDK_NGX_Result_Fail;
        }
        const auto result = copy_eval_color(context, handle, p);
        if (result == NVSDK_NGX_Result_Success)
        {
            receipt->request_id = request;
            receipt->context = reinterpret_cast<std::uintptr_t>(context);
            receipt->handle = reinterpret_cast<std::uintptr_t>(handle);
            receipt->output = reinterpret_cast<std::uintptr_t>(output);
            receipt->provider_frame = g_ao_frames;
            receipt->status = F6OutputStatus::current_output_ordered;
        }
        return result;
    }
    ID3D11Resource *output = nullptr;
    int reset = 0;
    if (p->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success || !output ||
        p->Get(NVSDK_NGX_Parameter_Reset, &reset) != NVSDK_NGX_Result_Success)
        return NVSDK_NGX_Result_Fail;
    using namespace neo::interop;
    ID3D11Resource *color = nullptr, *depth = nullptr, *motion = nullptr;
    if (p->Get(NVSDK_NGX_Parameter_Color, &color) != NVSDK_NGX_Result_Success || !color ||
        p->Get(NVSDK_NGX_Parameter_Depth, &depth) != NVSDK_NGX_Result_Success || !depth ||
        p->Get(NVSDK_NGX_Parameter_MotionVectors, &motion) != NVSDK_NGX_Result_Success || !motion)
        return NVSDK_NGX_Result_Fail;
    const auto query = reinterpret_cast<QueryBeforeSrFrameV1>(GetProcAddress(
        GetModuleHandleW(L"Dx11FsrBridge.dll"), k_query_before_sr_frame_v1));
    BeforeSrFrameV1 input, mismatch, wrong_version;
    mismatch.frame = UINT64_MAX;
    wrong_version.version = 0;
    const auto expected_view = (g_eval % 2 == 0) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!query || query(context, color, depth, &input) != S_OK || input.context != context ||
        input.color != color || input.depth != depth || input.motion != motion || input.frame != g_eval + 1 ||
        input.render_width != 512 || input.render_height != 512 ||
        input.display_width != g_receipt_display_width || input.display_height != g_receipt_display_height ||
        input.color_storage_format != DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        input.color_view_format != expected_view || input.color_transfer != ColorTransfer::unknown ||
        query(context, output, depth, &mismatch) != S_FALSE || query(context, color, output, &mismatch) != S_FALSE ||
        mismatch.frame != UINT64_MAX || query(context, color, depth, &wrong_version) != E_INVALIDARG)
    {
        std::puts("receipt fixture: BeforeSR input does not match current NGX parameters and source view");
        return NVSDK_NGX_Result_Fail;
    }
    if (g_eval == 0)
    {
        bool isolated = false;
        // 夹具把完全相同的参数交给另一线程，防止进程全局缓存恰好通过资源身份检查。
        std::thread other([&]
        {
            BeforeSrFrameV1 result;
            isolated = query(context, color, depth, &result) == S_FALSE && result.frame == 0;
        });
        other.join();
        if (!isolated) return NVSDK_NGX_Result_Fail;
    }
    ++g_eval;
    std::printf("BeforeSR query: frame=%llu storage=%u view=%u transfer=%u\n",
        static_cast<unsigned long long>(input.frame), input.color_storage_format, input.color_view_format,
        static_cast<unsigned>(input.color_transfer));
    // 未交付的每一帧都必须保留 Reset；最后一次成功后的正常帧应清掉它。
    if (reset != (g_eval <= 7 ? 1 : 0)) return NVSDK_NGX_Result_Fail;
    receipt->request_id = request;
    receipt->context = reinterpret_cast<std::uintptr_t>(context);
    receipt->handle = reinterpret_cast<std::uintptr_t>(handle);
    receipt->output = reinterpret_cast<std::uintptr_t>(output);
    receipt->provider_frame = g_eval;
    if (g_eval == 1)
    {
        receipt->status = F6OutputStatus::no_output;
    }
    else
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D11Device> device;
        context->GetDevice(device.GetAddressOf());
        ComPtr<ID3D11UnorderedAccessView> uav;
        if (!device || FAILED(device->CreateUnorderedAccessView(output, nullptr, uav.GetAddressOf())))
            return NVSDK_NGX_Result_Fail;
        constexpr float poison[] { 1.0f, 1.0f, 1.0f, 1.0f };
        constexpr float frame[] { 192.f / 1023.f, 704.f / 1023.f, 352.f / 1023.f, 1.0f };
        context->ClearUnorderedAccessViewFloat(uav.Get(), g_eval <= 6 ? poison : frame);
        receipt->status = F6OutputStatus::current_output_ordered;
        // 每次只破坏一种证据；毒色不会与原生/正确输出恰好相同。
        if (g_eval == 2) --receipt->request_id;
        if (g_eval == 3) receipt->context = 0;
        if (g_eval == 4) receipt->output = 0;
        if (g_eval == 5) receipt->status = static_cast<F6OutputStatus>(99);
    }
    const auto rc = g_eval == 6 ? NVSDK_NGX_Result_Fail : NVSDK_NGX_Result_Success;
    std::printf("receipt fixture: call=%u reset=%d rc=%X proof=%u\n", g_eval, reset, static_cast<unsigned>(rc),
        static_cast<unsigned>(f6_output_status(*receipt, request, handle, context, output)));
    return rc;
}

extern "C" __declspec(dllexport) BOOL WINAPI BeforeSrFixtureScopeClosed(ID3D11DeviceContext *context,
    neo::interop::QueryBeforeSrFrameV1 query) noexcept
{
    ID3D11Resource *color = nullptr, *depth = nullptr;
    neo::interop::BeforeSrFrameV1 result;
    // 用刚才实际 Evaluate 的参数，不用无效参数制造一个碰巧失败的断言。
    return g_main.Get(NVSDK_NGX_Parameter_Color, &color) == NVSDK_NGX_Result_Success && color &&
        g_main.Get(NVSDK_NGX_Parameter_Depth, &depth) == NVSDK_NGX_Result_Success && depth &&
        query(context, color, depth, &result) == S_FALSE && result.frame == 0;
}
#endif
