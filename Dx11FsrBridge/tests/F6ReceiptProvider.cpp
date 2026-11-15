#include <d3d11.h>
#include <wrl/client.h>
#include "../ngx/F6EvalReceiptV1.h"
#include "../third_party/ngx/nvsdk_ngx_params.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <variant>

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
NVSDK_NGX_Handle g_feature { 17 };
unsigned g_params_issued = 0, g_eval = 0;
bool g_active = false;
}

#define EXPORT extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV

EXPORT NVSDK_NGX_D3D11_Init_Ext(unsigned long long, const wchar_t *, ID3D11Device *,
    NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *)
{
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
    NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **out)
{
    if (!out || g_active || kind != NVSDK_NGX_Feature_SuperSampling) return NVSDK_NGX_Result_Fail;
    *out = &g_feature;
    g_active = true;
    return NVSDK_NGX_Result_Success;
}
EXPORT NVSDK_NGX_D3D11_ReleaseFeature(NVSDK_NGX_Handle *handle)
{
    if (!g_active || handle != &g_feature) return NVSDK_NGX_Result_Fail;
    g_active = false;
    return NVSDK_NGX_Result_Success;
}
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
    if (!context || !p || !g_active || handle != &g_feature || !receipt ||
        receipt->size != sizeof(F6EvalReceiptV1) || receipt->version != 1)
        return NVSDK_NGX_Result_Fail;
    ID3D11Resource *output = nullptr;
    int reset = 0;
    if (p->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success || !output ||
        p->Get(NVSDK_NGX_Parameter_Reset, &reset) != NVSDK_NGX_Result_Success)
        return NVSDK_NGX_Result_Fail;
    ++g_eval;
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
