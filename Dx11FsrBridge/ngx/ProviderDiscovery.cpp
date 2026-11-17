#include "ProviderDiscovery.h"

#include <tlhelp32.h>
#include <utility>

namespace neo::ngx
{
namespace
{
constexpr char k_init_name[] = "NVSDK_NGX_D3D11_Init_Ext";

bool bind_candidate(HMODULE module, ProviderDiscovery &found, const wchar_t *source, std::wstring path)
{
    // 2026-11-15：保留原来的十项兼容性门槛；仅存后端实际调用的入口。
    std::string missing;
    auto required = [&](const char *name)
    {
        const FARPROC entry = GetProcAddress(module, name);
        if (!entry)
        {
            if (!missing.empty()) missing += ',';
            missing += name;
        }
        return entry;
    };
    ProviderApi api;
    api.init = reinterpret_cast<ProviderApi::Init>(required(k_init_name));
    api.get_caps = reinterpret_cast<ProviderApi::GetCaps>(required("NVSDK_NGX_D3D11_GetCapabilityParameters"));
    api.destroy = reinterpret_cast<ProviderApi::Destroy>(required("NVSDK_NGX_D3D11_DestroyParameters"));
    api.create = reinterpret_cast<ProviderApi::Create>(required("NVSDK_NGX_D3D11_CreateFeature"));
    api.release = reinterpret_cast<FeatureHandle::Release>(required("NVSDK_NGX_D3D11_ReleaseFeature"));
    api.evaluate = reinterpret_cast<ProviderApi::Evaluate>(required("NVSDK_NGX_D3D11_EvaluateFeature"));
    required("NVSDK_NGX_D3D11_Shutdown1");
    required("NVSDK_NGX_D3D11_AllocateParameters");
    required("NVSDK_NGX_D3D11_GetScratchBufferSize");
    required("NVSDK_NGX_D3D11_GetFeatureRequirements");
    if (!missing.empty())
    {
        if (found.missing_entries.empty()) found.missing_entries = std::move(missing);
        return false;
    }
    api.evaluate_f6 = reinterpret_cast<F6EvaluateV1>(GetProcAddress(module, k_f6_evaluate_v1));
    // 2026-11-17（上游 1a9df8f）：搜索路线不等于实现文件；记录 Create 入口所属模块供初始化取证。
    HMODULE entry_module = nullptr;
    wchar_t entry_path[32768] {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(api.create), &entry_module))
    {
        const DWORD length = GetModuleFileNameW(entry_module, entry_path, static_cast<DWORD>(std::size(entry_path)));
        if (length && length < std::size(entry_path)) found.entry_path.assign(entry_path, length);
    }
    found.api = api;
    found.source = source;
    found.path = std::move(path);
    return true;
}

bool try_nvidia(ProviderDiscovery &found)
{
    wchar_t path[1024] {};
    DWORD size = sizeof(path);
    const LSTATUS rc = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
        L"FullPath", RRF_RT_REG_SZ, nullptr, path, &size);
    if (rc != ERROR_SUCCESS || !path[0])
    {
        found.failure = L"no_registry";
        found.error = static_cast<DWORD>(rc);
        return false;
    }
    auto dll = std::wstring(path) + L"\\_nvngx.dll";
    const HMODULE module = LoadLibraryW(dll.c_str());
    if (!module)
    {
        found.error = GetLastError();
        found.failure = L"load_failed";
        return false;
    }
    if (!bind_candidate(module, found, L"nvidia_registry", std::move(dll)))
    {
        found.failure = L"missing_entry";
        return false;
    }
    found.nvidia = true;
    return true;
}

bool try_external(ProviderDiscovery &found, DWORD &load_error, unsigned &scanned)
{
    // 保留外部 provider 的加载兼容约定：这里可能拿到借用的句柄，不能按普通 LoadLibrary 成功去释放。
    const HMODULE module = LoadLibraryW(L"_nvngx.dll");
    load_error = module ? ERROR_SUCCESS : GetLastError();
    if (module && bind_candidate(module, found, L"nvngx_load", L"_nvngx.dll")) return true;

    // 只在初始化或退避重试枚举；不把探测、文件名或全局加载顺序引进每帧路径。
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    struct Snapshot
    {
        HANDLE handle;
        ~Snapshot() { CloseHandle(handle); }
    } snapshot_lease {snapshot};
    MODULEENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry))
    {
        ++scanned;
        if (!entry.hModule || !GetProcAddress(entry.hModule, k_init_name)) continue;
        if (bind_candidate(entry.hModule, found, L"loaded_module", entry.szExePath)) return true;
    }
    return false;
}
}

const char *provider_mode_name(ProviderMode mode) noexcept
{
    switch (mode)
    {
    case ProviderMode::nvidia: return "nvidia";
    case ProviderMode::external: return "external";
    default: return "auto";
    }
}

ProviderDiscovery discover_provider(ProviderMode mode)
{
    ProviderDiscovery found;
    if (mode != ProviderMode::external)
    {
        if (try_nvidia(found) || mode == ProviderMode::nvidia) return found;
        wchar_t reason[80] {};
        swprintf_s(reason, L"%s(0x%lX) external=", found.failure, found.error);
        found.detail = reason;
    }
    DWORD load_error = ERROR_SUCCESS;
    unsigned scanned = 0;
    if (try_external(found, load_error, scanned)) return found;

    // 外部实现可能还没加载。与明确的驱动路径失败分开，由后端管理重试预算。
    wchar_t details[80] {};
    swprintf_s(details, L"nvngx_load=0x%lX modules=%u", load_error, scanned);
    found.detail += details;
    found.failure = L"provider_missing";
    found.retryable = true;
    return found;
}
}
