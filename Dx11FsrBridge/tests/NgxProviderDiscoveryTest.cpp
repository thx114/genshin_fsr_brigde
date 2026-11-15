#include "../ngx/ProviderDiscovery.h"

#include <cstdio>
#include <string_view>

// 模拟早已加载、但缺了一整组调用入口的提供方；不得调用它或继承它的可选回执。
extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D11_Init_Ext(
    unsigned long long, const wchar_t *, ID3D11Device *, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *)
{
    std::puts("Unexpected Init on incomplete provider");
    return NVSDK_NGX_Result_Fail;
}

int wmain(int argc, wchar_t **argv)
{
    using namespace neo::ngx;
    if (argc != 2) return 2;
    const auto missing = discover_provider(ProviderMode::external);
    if (missing.source || !missing.retryable || missing.api.init || missing.api.evaluate_f6 ||
        missing.missing_entries.find("NVSDK_NGX_D3D11_EvaluateFeature") == std::string::npos)
    {
        std::puts("Incomplete provider was not rejected as a unit");
        return 1;
    }
    if (!LoadLibraryW(argv[1])) return 2;
    const auto loaded = discover_provider(ProviderMode::external);
    if (!loaded.source || std::wstring_view(loaded.source) != L"loaded_module" || loaded.nvidia ||
        !loaded.api.init || !loaded.api.get_caps || !loaded.api.destroy || !loaded.api.create ||
        !loaded.api.release || !loaded.api.evaluate || !loaded.api.evaluate_f6)
    {
        std::puts("Complete provider failed to replace rejected candidate");
        return 1;
    }
    std::puts("NGX discovery rejected a partial ABI, then bound a complete provider and its own F6 receipt");
    return 0;
}
