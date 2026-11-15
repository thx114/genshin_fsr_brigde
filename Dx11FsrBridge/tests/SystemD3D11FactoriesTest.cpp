#include "SystemD3D11Factories.h"
#include <cstdio>
#include <thread>

namespace
{
std::atomic<unsigned> device_calls {}, combined_calls {};
SystemD3D11Factories hooks;
__declspec(noinline) HRESULT WINAPI device(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT flags,
    const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **)
{
    volatile UINT value = flags;
    return static_cast<HRESULT>(value + 13);
}
__declspec(noinline) HRESULT WINAPI combined(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT flags,
    const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **,
    ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **)
{
    volatile UINT value = flags;
    return static_cast<HRESULT>(value + 29);
}
HRESULT WINAPI observe_device(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE m, UINT f,
    const D3D_FEATURE_LEVEL *l, UINT n, UINT s, ID3D11Device **d, D3D_FEATURE_LEVEL *v, ID3D11DeviceContext **c)
{
    ++device_calls;
    return hooks.device(a, t, m, f, l, n, s, d, v, c);
}
HRESULT WINAPI observe_combined(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE m, UINT f,
    const D3D_FEATURE_LEVEL *l, UINT n, UINT s, const DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **chain,
    ID3D11Device **d, D3D_FEATURE_LEVEL *v, ID3D11DeviceContext **c)
{
    ++combined_calls;
    return hooks.combined(a, t, m, f, l, n, s, desc, chain, d, v, c);
}
}
int main()
{
    const auto cached_device = &device;
    const auto cached_combined = &combined;
    bool attempted = false;
    // 2026-11-15：借来的事务失败时不能 Abort 调用方的事务，失败后仍须能重试。
    if (DetourTransactionBegin() != NO_ERROR)
        return 6;
    const LONG conflict = hooks.install(cached_device, cached_combined, observe_device, observe_combined, attempted);
    const LONG preserved = DetourTransactionCommit();
    if (conflict != ERROR_INVALID_OPERATION || !attempted || preserved != NO_ERROR || hooks.device || hooks.combined)
        return 7;
    // 第二个入口准备失败须撤销第一个入口，不能把半安装状态留给下一次重试。
    const LONG rolled_back = hooks.install(cached_device, cached_combined, observe_device, nullptr, attempted);
    if (rolled_back == NO_ERROR || !attempted || !hooks.device_receipt.trampoline || hooks.device || hooks.combined ||
        cached_device(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 7, nullptr, 0, 0, nullptr, nullptr, nullptr) != 20 ||
        cached_combined(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 11, nullptr, 0, 0,
            nullptr, nullptr, nullptr, nullptr, nullptr) != 40 || device_calls.load() || combined_calls.load())
        return 11;

    std::atomic<bool> stop {}, corrupt {};
    std::atomic<unsigned> rounds {};
    std::thread callers[4];
    for (auto &caller : callers)
        caller = std::thread([&]()
        {
            while (!stop.load(std::memory_order_acquire))
            {
                if (cached_device(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 7, nullptr, 0, 0, nullptr, nullptr, nullptr) != 20 ||
                    cached_combined(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 11, nullptr, 0, 0,
                        nullptr, nullptr, nullptr, nullptr, nullptr) != 40)
                    corrupt.store(true, std::memory_order_relaxed);
                rounds.fetch_add(1, std::memory_order_release);
            }
        });
    while (rounds.load(std::memory_order_acquire) < 100)
        Sleep(0);
    const LONG installed = hooks.install(cached_device, cached_combined, observe_device, observe_combined, attempted);
    const bool installed_once = attempted;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (installed == NO_ERROR &&
        (device_calls.load(std::memory_order_relaxed) < 1000 || combined_calls.load(std::memory_order_relaxed) < 1000) &&
        GetTickCount64() < deadline)
        Sleep(0);
    stop.store(true, std::memory_order_release);
    for (auto &caller : callers)
        caller.join();
    if (installed != NO_ERROR || !installed_once || corrupt.load() || device_calls.load() < 1000 || combined_calls.load() < 1000)
        return 8;
    device_calls = combined_calls = 0;
    if (hooks.install(cached_device, cached_combined, observe_device, observe_combined, attempted) != NO_ERROR || attempted)
        return 2;
    if (cached_device(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 7, nullptr, 0, 0, nullptr, nullptr, nullptr) != 20 ||
        cached_combined(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 11, nullptr, 0, 0, nullptr, nullptr, nullptr, nullptr, nullptr) != 40 ||
        device_calls != 1 || combined_calls != 1 || hooks.device_receipt.target != reinterpret_cast<void *>(&device))
        return 3;
    if (DetourTransactionBegin() != NO_ERROR)
        return 9;
    const LONG detach_conflict = hooks.detach(observe_device, observe_combined);
    const LONG detach_preserved = DetourTransactionCommit();
    if (detach_conflict != ERROR_INVALID_OPERATION || detach_preserved != NO_ERROR ||
        cached_device(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 7, nullptr, 0, 0, nullptr, nullptr, nullptr) != 20 ||
        device_calls.load() != 2)
        return 10;
    if (hooks.detach(observe_device, observe_combined) != NO_ERROR ||
        hooks.detach(observe_device, observe_combined) != NO_ERROR)
        return 4;
    if (cached_device(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 7, nullptr, 0, 0, nullptr, nullptr, nullptr) != 20 ||
        cached_combined(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 11, nullptr, 0, 0, nullptr, nullptr, nullptr, nullptr, nullptr) != 40 ||
        device_calls != 2 || combined_calls != 1)
        return 5;
    std::puts("System D3D11 factories: concurrent cached entries, transaction conflicts/rollback, exact trampoline, attach/detach passed");
    return 0;
}
