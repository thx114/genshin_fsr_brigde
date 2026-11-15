#include <d3d11.h>
#include <wrl/client.h>
#include "../ngx/F6EvalReceiptV1.h"
#include "../interop/BeforeSrV1.h"
#include "../third_party/ngx/nvsdk_ngx_params.h"

#include <cstdio>
#include <string>
#include <thread>
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
unsigned g_params_issued = 0;
#if !defined(BEFORE_SR_LEGACY_FIXTURE)
unsigned g_eval = 0;
#endif
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
#if defined(BEFORE_SR_LEGACY_FIXTURE)
EXPORT NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext *context, const NVSDK_NGX_Handle *handle,
    const NVSDK_NGX_Parameter *p, PFN_NVSDK_NGX_ProgressCallback)
{
    ID3D11Resource *color = nullptr, *output = nullptr;
    unsigned render = 0, height = 0;
    if (!context || !p || handle != &g_feature || !g_active ||
        p->Get(NVSDK_NGX_Parameter_Color, &color) != NVSDK_NGX_Result_Success || !color ||
        p->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success || !output ||
        p->Get(NVSDK_NGX_Parameter_Width, &render) != NVSDK_NGX_Result_Success || !render ||
        p->Get(NVSDK_NGX_Parameter_Height, &height) != NVSDK_NGX_Result_Success || !height)
        return NVSDK_NGX_Result_Fail;
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Texture2D> target;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(target.GetAddressOf())))) return NVSDK_NGX_Result_Fail;
    D3D11_TEXTURE2D_DESC desc {};
    target->GetDesc(&desc);
    // 从实际绑定给 Evaluate 的颜色取一个精确像素，不用固定输出掩盖接错或调用顺序错误。
    const D3D11_BOX center { render / 2, height / 2, 0, render / 2 + 1, height / 2 + 1, 1 };
    context->CopySubresourceRegion(output, 0, desc.Width / 2, desc.Height / 2, 0, color, 0, &center);
    return NVSDK_NGX_Result_Success;
}
#else
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
        input.render_width != 512 || input.render_height != 512 || input.display_width != 1024 ||
        input.display_height != 1024 || input.color_storage_format != DXGI_FORMAT_R8G8B8A8_TYPELESS ||
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
