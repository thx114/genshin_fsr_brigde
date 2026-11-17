#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2)
    {
        std::fputs("Usage: BridgeCpuParityBench <isolated bridge DLL>\n", stderr);
        return 2;
    }
    const std::filesystem::path dll = std::filesystem::absolute(argv[1]);
    auto ini = dll;
    ini.replace_extension(L".ini");
    // 2026-11-16：由对照脚本提供独立副本目录，避免覆盖正在使用的桥配置。
    std::ofstream config(ini);
    config << "[Dx11FsrBridge]\nEnabled=1\nRenderScaleMenu=0\nTransparentJitter=0\n"
        "Fsr2TranslationMode=2\nFsr2Mode2OnDemandState=1\nFsr2Il2CppHook=0\n"
        "Ffx12=1\nFfx12Backend=native\nDrawEntryFilter=1\nDrawEntryFilterCanary=64\n"
        "[Log]\nlevel=error\nto_file=0\nto_debugger=0\n";
    config.close();
    if (!config) return 3;
    const HMODULE bridge = LoadLibraryW(dll.c_str());
    if (!bridge)
    {
        std::fprintf(stderr, "Bridge loading failed: error=%lu\n", GetLastError());
        return 4;
    }
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    const HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf());
    if (FAILED(result))
    {
        std::fprintf(stderr, "WARP device creation failed: hr=%08lX\n", static_cast<unsigned long>(result));
        return 5;
    }
    const auto owned = [&](unsigned slot)
    {
        HMODULE owner = nullptr;
        void *entry = (*reinterpret_cast<void ***>(context.Get()))[slot];
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(entry), &owner) &&
            owner == bridge;
    };
    unsigned hook_checks = 0;
    const auto hooks_owned = [&](const char *phase)
    {
        // 2026-11-16：WARP 更新/Flush 可能切换方法表；计时前后都须仍归属于被测 DLL。
        if (!owned(12) || !owned(13))
        {
            std::fprintf(stderr, "Hook ownership lost at %s; Draw=%d DrawIndexed=%d\n",
                phase, owned(13), owned(12));
            return false;
        }
        ++hook_checks;
        return true;
    };
    if (!hooks_owned("after creation")) return 6;
    LARGE_INTEGER frequency {};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return 7;
    constexpr unsigned k_iterations = 200000;
    constexpr unsigned k_rounds = 9;
    std::array<std::array<double, k_rounds>, 3> timings {};
    const auto invoke = [&](unsigned mode)
    {
        if (mode == 0) context->Draw(0, 0);
        else if (mode == 1) context->DrawIndexed(0, 0, 0);
        else context->Draw(3, 0);
    };
    for (unsigned mode = 0; mode < timings.size(); ++mode)
    {
        invoke(mode);
        if (!hooks_owned("after first warmup Draw")) return 6;
        for (unsigned i = 1; i < 20000; ++i) invoke(mode);
        if (!hooks_owned("before warmup Flush")) return 6;
        context->Flush();
        if (!hooks_owned("after warmup Flush")) return 6;
    }
    // 2026-11-16：整批只读两次时钟，避免逐 draw 探针污染成本；Flush 留在计时区间外。
    for (unsigned round = 0; round < k_rounds; ++round)
    {
        for (unsigned step = 0; step < timings.size(); ++step)
        {
            const unsigned mode = (step + round) % static_cast<unsigned>(timings.size());
            if (!hooks_owned("before timed batch")) return 6;
            LARGE_INTEGER start {}, end {};
            QueryPerformanceCounter(&start);
            for (unsigned i = 0; i < k_iterations; ++i) invoke(mode);
            QueryPerformanceCounter(&end);
            timings[mode][round] = double(end.QuadPart - start.QuadPart) * 1.e9 /
                (double(frequency.QuadPart) * k_iterations);
            if (!hooks_owned("after timed batch before Flush")) return 6;
            context->Flush();
            if (!hooks_owned("after timed batch Flush")) return 6;
        }
    }
    std::printf("{\"iterations\":%u,\"rounds\":%u,\"draw_owned\":true,\"indexed_owned\":true,"
        "\"hook_checks\":%u,\"samples_ns\":[", k_iterations, k_rounds, hook_checks);
    for (unsigned mode = 0; mode < timings.size(); ++mode)
    {
        std::printf("%s[", mode ? "," : "");
        for (unsigned round = 0; round < k_rounds; ++round)
            std::printf("%s%.6f", round ? "," : "", timings[mode][round]);
        std::printf("]");
    }
    std::puts("]}");
    return 0;
}
