#pragma once

#include <d3d11.h>
#include <string>
#include "F6EvalReceiptV1.h"
#include "FeatureHandle.h"

namespace neo::ngx
{
enum class ProviderMode { automatic, nvidia, external };

struct ProviderApi
{
    using Init = NVSDK_NGX_Result (NVSDK_CONV *)(unsigned long long, const wchar_t *, ID3D11Device *,
        NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *);
    using GetCaps = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter **);
    using Destroy = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter *);
    using Create = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D11DeviceContext *, NVSDK_NGX_Feature,
        NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
    using Evaluate = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D11DeviceContext *, const NVSDK_NGX_Handle *,
        const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback);

    Init init = nullptr;
    GetCaps get_caps = nullptr;
    Destroy destroy = nullptr;
    Create create = nullptr;
    FeatureHandle::Release release = nullptr;
    Evaluate evaluate = nullptr;
    F6EvaluateV1 evaluate_f6 = nullptr;
};

struct ProviderDiscovery
{
    ProviderApi api;
    const wchar_t *source = nullptr;
    std::wstring path;
    std::wstring entry_path;
    bool nvidia = false;

    const wchar_t *failure = L"provider_missing";
    DWORD error = 0;
    bool retryable = false;
    std::wstring detail;
    std::string missing_entries;
};

const char *provider_mode_name(ProviderMode mode) noexcept;
// 2026-11-15：候选完整才交给后端，避免外部模块探测改写活跃的 Create/Evaluate/Release 组合。
ProviderDiscovery discover_provider(ProviderMode mode);
}
