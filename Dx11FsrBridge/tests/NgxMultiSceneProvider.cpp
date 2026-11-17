#include "NgxMultiSceneFixture.h"
#include "../Il2CppCallSiteHook.h"
#include "../runtime/FrameTimeline.h"
#include "../interop/BeforeSrV1.h"
#include "../ngx/F6EvalReceiptV1.h"
#include "../third_party/ngx/nvsdk_ngx_params.h"
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <variant>

struct NVSDK_NGX_Handle { unsigned id; };

namespace
{
using namespace neo::tests::multi_scene;
using Microsoft::WRL::ComPtr;

class Parameters final : public NVSDK_NGX_Parameter
{
public:
#define PARAM(T) \
    void Set(const char *name, T value) override { values_[name] = value; } \
    NVSDK_NGX_Result Get(const char *name, T *value) const override \
    { \
        const auto it = values_.find(name); \
        const auto *stored = it != values_.end() ? std::get_if<T>(&it->second) : nullptr; \
        if (!value || !stored) return NVSDK_NGX_Result_Fail; \
        *value = *stored; return NVSDK_NGX_Result_Success; \
    }
    PARAM(unsigned long long)
    PARAM(float)
    PARAM(double)
    PARAM(unsigned int)
    PARAM(int)
    PARAM(ID3D11Resource *)
    PARAM(ID3D12Resource *)
    PARAM(void *)
#undef PARAM
    void Reset() override { values_.clear(); }
private:
    using Value = std::variant<unsigned long long, float, double, unsigned int, int,
        ID3D11Resource *, ID3D12Resource *, void *>;
    std::unordered_map<std::string, Value> values_;
};

struct Feature
{
    NVSDK_NGX_Handle handle {};
    bool active = false;
    std::uint64_t owner = 0;
    unsigned width = 0, height = 0;
    ID3D11Resource *color = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr;
};
std::array<Feature, il2cpp_callsite::k_render_instance_capacity> g_features {};
Parameters g_params, g_query;
unsigned g_params_count = 0, g_created = 0, g_released = 0, g_evaluated = 0, g_frame = 0;
std::array<std::uint64_t, k_scene_count> g_keys {};
std::array<std::uint64_t, k_scene_count> g_last_evaluate_tick {};

bool sample(ID3D11DeviceContext &context, ID3D11Resource *source, std::uint32_t &pixel)
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11Device> device;
    if (!source || FAILED(source->QueryInterface(IID_PPV_ARGS(&texture)))) return false;
    context.GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    const D3D11_BOX center {desc.Width / 2, desc.Height / 2, 0, desc.Width / 2 + 1, desc.Height / 2 + 1, 1};
    desc.Width = desc.Height = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (!device || FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
    context.CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source, 0, &center);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context.Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    std::memcpy(&pixel, mapped.pData, sizeof(pixel));
    context.Unmap(staging.Get(), 0);
    return true;
}
}

extern "C" __declspec(dllexport) void ConfigureMultiSceneFixture(const std::uint64_t *keys) noexcept
{
    std::copy_n(keys, g_keys.size(), g_keys.begin());
}
extern "C" __declspec(dllexport) void SelectMultiSceneFrame(unsigned frame) noexcept { g_frame = frame; }
extern "C" __declspec(dllexport) bool MultiSceneComplete() noexcept
{
    std::printf("multi scene: delivered=%u expected=%zu created=%u released=%u\n",
        g_evaluated, std::size(k_frames), g_created, g_released);
    return g_evaluated == std::size(k_frames) && g_created == 6 && g_released == 2;
}

#define EXPORT extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
EXPORT NVSDK_NGX_D3D11_Init_Ext(unsigned long long, const wchar_t *, ID3D11Device *,
    NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *) { return NVSDK_NGX_Result_Success; }
EXPORT NVSDK_NGX_D3D11_GetCapabilityParameters(NVSDK_NGX_Parameter **params)
{
    auto &selected = g_params_count++ == 0 ? g_params : g_query;
    selected.Set(NVSDK_NGX_Parameter_SuperSampling_Available, 1);
    *params = &selected;
    return NVSDK_NGX_Result_Success;
}
EXPORT NVSDK_NGX_D3D11_AllocateParameters(NVSDK_NGX_Parameter **params)
{ return NVSDK_NGX_D3D11_GetCapabilityParameters(params); }
EXPORT NVSDK_NGX_D3D11_DestroyParameters(NVSDK_NGX_Parameter *) { return NVSDK_NGX_Result_Success; }
EXPORT NVSDK_NGX_D3D11_Shutdown1(ID3D11Device *) { return NVSDK_NGX_Result_Success; }
EXPORT NVSDK_NGX_D3D11_GetScratchBufferSize(NVSDK_NGX_Feature, const NVSDK_NGX_Parameter *, size_t *bytes)
{
    *bytes = 0;
    return NVSDK_NGX_Result_Success;
}
EXPORT NVSDK_NGX_D3D11_GetFeatureRequirements(IDXGIAdapter *, const NVSDK_NGX_FeatureDiscoveryInfo *,
    NVSDK_NGX_FeatureRequirement *requirements)
{
    *requirements = {};
    return NVSDK_NGX_Result_Success;
}
EXPORT NVSDK_NGX_D3D11_CreateFeature(ID3D11DeviceContext *, NVSDK_NGX_Feature kind,
    NVSDK_NGX_Parameter *params, NVSDK_NGX_Handle **handle)
{
    if (kind != NVSDK_NGX_Feature_SuperSampling || !params || !handle) return NVSDK_NGX_Result_Fail;
    for (auto &feature : g_features)
    {
        if (feature.active) continue;
        feature = {};
        if (params->Get(NVSDK_NGX_Parameter_Width, &feature.width) != NVSDK_NGX_Result_Success ||
            params->Get(NVSDK_NGX_Parameter_Height, &feature.height) != NVSDK_NGX_Result_Success)
            return NVSDK_NGX_Result_Fail;
        feature.handle.id = ++g_created;
        feature.active = true;
        *handle = &feature.handle;
        return NVSDK_NGX_Result_Success;
    }
    return NVSDK_NGX_Result_Fail;
}
EXPORT NVSDK_NGX_D3D11_ReleaseFeature(NVSDK_NGX_Handle *handle)
{
    for (auto &feature : g_features)
        if (feature.active && handle == &feature.handle)
        {
            feature.active = false;
            ++g_released;
            return NVSDK_NGX_Result_Success;
        }
    return NVSDK_NGX_Result_Fail;
}
EXPORT NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext *, const NVSDK_NGX_Handle *,
    const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback) { return NVSDK_NGX_Result_Fail; }

EXPORT F6_NGX_D3D11_EvaluateFeatureV1(ID3D11DeviceContext *context, const NVSDK_NGX_Handle *handle,
    NVSDK_NGX_Parameter *params, PFN_NVSDK_NGX_ProgressCallback, std::uint64_t request,
    neo::ngx::F6EvalReceiptV1 *receipt)
{
    using namespace neo::interop;
    if (!context || !params || !receipt || g_frame >= std::size(k_frames) || g_frame != g_evaluated)
        return NVSDK_NGX_Result_Fail;
    auto *feature = static_cast<Feature *>(nullptr);
    for (auto &candidate : g_features)
        if (candidate.active && &candidate.handle == handle) feature = &candidate;
    if (!feature) return NVSDK_NGX_Result_Fail;
    ID3D11Resource *color = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr;
    int reset = -1;
    unsigned ow = 0, oh = 0;
    if (params->Get(NVSDK_NGX_Parameter_Color, &color) != NVSDK_NGX_Result_Success ||
        params->Get(NVSDK_NGX_Parameter_Depth, &depth) != NVSDK_NGX_Result_Success ||
        params->Get(NVSDK_NGX_Parameter_MotionVectors, &motion) != NVSDK_NGX_Result_Success ||
        params->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success ||
        params->Get(NVSDK_NGX_Parameter_Reset, &reset) != NVSDK_NGX_Result_Success ||
        params->Get(NVSDK_NGX_Parameter_OutWidth, &ow) != NVSDK_NGX_Result_Success ||
        params->Get(NVSDK_NGX_Parameter_OutHeight, &oh) != NVSDK_NGX_Result_Success)
        return NVSDK_NGX_Result_Fail;
    const auto query = reinterpret_cast<QueryBeforeSrFrameV1>(GetProcAddress(
        GetModuleHandleW(L"Dx11FsrBridge.dll"), k_query_before_sr_frame_v1));
    BeforeSrFrameV1 frame;
    const auto &test = k_frames[g_frame];
    const auto now = GetTickCount64();
    const auto previous = g_last_evaluate_tick[test.scene];
    const bool expected_reset = test.reset || (previous != 0 &&
        now - previous > neo::graphics::FrameTimeline::k_takeover_gap_ms);
    const bool stable = !feature->owner || (feature->owner == g_keys[test.scene] && feature->color == color &&
        feature->depth == depth && feature->motion == motion && feature->output == output);
    const bool matched = query && query(context, color, depth, &frame) == S_OK &&
        frame.instance == g_keys[test.scene] && feature->width == test.width && feature->height == test.height &&
        ow == test.width && oh == test.height && reset == int(expected_reset) && stable;
    std::printf("multi frame=%u scene=%u feature=%u reset=%d expected=%d stable=%d matched=%d\n",
        g_frame + 1, test.scene, feature->handle.id, reset, expected_reset, stable, matched);
    std::uint32_t input_pixel = 0, depth_pixel = 0;
    // 2026-11-16：只认当前 FSR 的深度；旁边可读 DSV 的 0.25 不属于本次输入。
    const float expected_depth = 0.5f;
    std::uint32_t depth_bits = 0;
    std::memcpy(&depth_bits, &expected_depth, sizeof(depth_bits));
    const bool sampled = matched && sample(*context, color, input_pixel) && sample(*context, depth, depth_pixel);
    std::printf("multi guides: frame=%u sampled=%d color=%08X expected=%08X depth=%08X expected=%08X\n",
        g_frame + 1, sampled, input_pixel, neo::tests::multi_scene::color(g_frame), depth_pixel, depth_bits);
    if (!sampled || input_pixel != neo::tests::multi_scene::color(g_frame) || depth_pixel != depth_bits)
        return NVSDK_NGX_Result_Fail;
    feature->owner = frame.instance;
    feature->color = color;
    feature->depth = depth;
    feature->motion = motion;
    feature->output = output;
    const D3D11_BOX center {test.width / 2, test.height / 2, 0, test.width / 2 + 1, test.height / 2 + 1, 1};
    context->CopySubresourceRegion(output, 0, test.width / 2, test.height / 2, 0, color, 0, &center);
    receipt->request_id = request;
    receipt->context = reinterpret_cast<std::uintptr_t>(context);
    receipt->handle = reinterpret_cast<std::uintptr_t>(handle);
    receipt->output = reinterpret_cast<std::uintptr_t>(output);
    receipt->provider_frame = ++g_evaluated;
    g_last_evaluate_tick[test.scene] = now;
    receipt->status = neo::ngx::F6OutputStatus::current_output_ordered;
    return NVSDK_NGX_Result_Success;
}
