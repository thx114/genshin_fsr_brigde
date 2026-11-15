#include <windows.h>
#include <d3d11.h>

#include "create-routing/DrawCapture.h"
#include "../game/AnchorPatterns.h"
#include "../ngx/F6EvalReceiptV1.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <share.h>
#include <string_view>

namespace
{
constexpr std::size_t k_render_offset = 0x70;
constexpr auto build_fixture_methods()
{
    // 宿主 PE 自己含受控的方法；桥按真实配置兜底和序言验证安装观察器。
    constexpr std::uint8_t tail[] {0x5B, 0x5D, 0x5F, 0x5E, 0x41, 0x5C, 0x41, 0x5D, 0x41, 0x5E, 0x41, 0x5F, 0xC3};
    std::array<std::uint8_t, k_render_offset + sizeof(neo::game::k_render_head) + sizeof(tail)> code {};
    std::copy_n(neo::game::k_ucb_head, sizeof(neo::game::k_ucb_head), code.begin());
    auto after_head = std::copy_n(neo::game::k_render_head, sizeof(neo::game::k_render_head),
        code.begin() + k_render_offset);
    std::copy_n(tail, sizeof(tail), after_head);
    return code;
}

#pragma section(".neort", read, execute)
__declspec(allocate(".neort")) constinit const auto k_methods = build_fixture_methods();

void publish_render(UINT render, UINT display)
{
    alignas(8) static std::array<std::uint8_t, 0x80> instance {};
    static std::uint32_t frame = 0;
    const auto put = [&](std::size_t offset, const auto &value)
    {
        std::memcpy(instance.data() + offset, &value, sizeof(value));
    };
    const float zero = 0.0f;
    put(0x24, zero);
    put(0x28, zero);
    put(0x50, ++frame);
    put(0x58, render);
    put(0x5C, render);
    put(0x68, display);
    put(0x6C, display);
    using Observe = void (*)(void *, void *);
    reinterpret_cast<Observe>(k_methods.data() + k_render_offset)(instance.data(), nullptr);
}
}

// CTest 使用本进程的独立目录，配置/日志不触碰构建根、F6 或游戏安装。
static bool stage_receipt_fixture(const std::filesystem::path &bridge, const std::filesystem::path &provider)
{
    namespace fs = std::filesystem;
    const auto run = bridge.parent_path() / (L"receipt-run-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    fs::create_directory(run, error);
    if (error) return false;
    if (!fs::copy_file(bridge, run / L"Dx11FsrBridge.dll", fs::copy_options::overwrite_existing, error) ||
        !fs::copy_file(provider, run / L"F6ReceiptProvider.dll", fs::copy_options::overwrite_existing, error))
        return false;
    fs::current_path(run, error);
    return !error;
}

int wmain(int argc, wchar_t **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2 && (argc != 4 || std::wstring_view(argv[1]) != L"ngx-receipt"))
    {
        std::puts("Usage: ProviderFrameHost <case> (stage DLLs in working directory)");
        std::puts("       ProviderFrameHost ngx-receipt <bridge DLL> <receipt fixture DLL>");
        return 2;
    }
    const std::wstring_view mode = argv[1];
    if (argc == 4 && !stage_receipt_fixture(argv[2], argv[3]))
    {
        std::puts("Failed to stage the isolated WARP receipt fixture");
        return 2;
    }
    const bool fsr3 = mode == L"native-fsr3";
    const bool fsr4 = mode == L"native-fsr4";
    const bool native_resize = mode == L"native-resize";
    const bool receipt_skip = mode == L"ngx-v1-skip-control";
    const bool real_receipt = mode == L"ngx-v1" || mode == L"ngx-v1-resize" || receipt_skip;
    const bool resize = mode == L"ngx-resize" || mode == L"ngx-v1-resize" || native_resize;
    const bool receipt = mode == L"ngx-receipt";
    const bool ngx = mode == L"ngx" || mode == L"ngx-resize" || receipt || real_receipt;
    const bool missing = mode == L"native-fallback";
    const bool reject = mode == L"native-reject";
    const bool async = mode == L"native-async" || native_resize || reject || fsr3 || fsr4;
    const bool fallback = missing || reject;
    if (!ngx && !fallback && !async && mode != L"native") return 2;
    const auto root = std::filesystem::current_path();
    const auto bridge = root / L"Dx11FsrBridge.dll";
    const auto provider = root / (receipt ? L"F6ReceiptProvider.dll" : ngx ? L"OptiScaler.dll" :
        reject ? L"FsrRejectProvider.dll" : L"amd_fidelityfx_upscaler_dx12.dll");
    if (!std::filesystem::is_regular_file(bridge) || (!missing && !std::filesystem::is_regular_file(provider))) return 2;
    // SDK 日志可在多个线程写 stdout，不能与机器验收行共享输出流。
    std::unique_ptr<std::FILE, decltype(&std::fclose)> observations(
        _wfsopen((root / L"provider-receipt.txt").c_str(), L"wb", _SH_DENYNO), &std::fclose);
    if (!observations) return 2;
    std::setvbuf(observations.get(), nullptr, _IONBF, 0);
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto ucb = reinterpret_cast<std::uintptr_t>(k_methods.data()) - module;
    // 调用者提供独立目录；只写本夹具所需的配置，不使用或改写安装/发布配置。
    std::ofstream config(root / L"Dx11FsrBridge.ini", std::ios::out | std::ios::trunc);
    config << "[Dx11FsrBridge]\nEnabled=1\nRenderScaleMenu=0\nTransparentJitter=0\nFsr2TranslationMode=2\n"
        "Fsr2Mode2OnDemandState=1\nFsr2Il2CppHook=1\nFfx12=1\nFfx12Hdr=0\nFfx12NonLinear=0\n"
        "Ffx12GpuInterop=1\nFfx12JitterMode=0\nFfx12JitterDelay=0\n"
        "Ffx12AsyncUpscale=" << async << '\n'
        << "Fsr2Il2CppRenderRva=0x" << std::hex << ucb + k_render_offset << "\nFsr2Il2CppUcbRva=0x" << ucb << '\n';
    if (ngx)
    {
        config << "Ffx12Backend=ngx\nNgxProvider=external\nNgxHdr=0\nNgxLogLevel=3\n";
    }
    else
    {
        config << "Ffx12Backend=native\nFfx12Version=" << (fsr3 ? "ffx12-fsr3.x" : fsr4 ? "ffx12-fsr4.x" : "ffx12-fsr2.x")
            << "\nFfx12DllPath=" << (missing ? "missing-provider.dll" : provider.filename().string()) << '\n';
    }
    config << "[Log]\nto_file=1\nlevel=debug\n";
    config.close();
    if (!config) return 2;
    if (ngx)
    {
        const HMODULE opti = LoadLibraryW(provider.c_str());
        if (!opti || !GetProcAddress(opti, "NVSDK_NGX_D3D11_Init_Ext")) return 3;
        const bool has_receipt = GetProcAddress(opti, neo::ngx::k_f6_evaluate_v1) != nullptr;
        const bool expect_receipt = receipt || real_receipt;
        std::fprintf(observations.get(), "provider export: v1=%d expected=%d\n", has_receipt, expect_receipt);
        if (has_receipt != expect_receipt) return 3;
    }
    if (!LoadLibraryW(bridge.c_str())) return 3;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    const auto hr = D3D11CreateDevice(nullptr, receipt ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf());
    if (FAILED(hr))
    {
        std::printf("hardware D3D11 device failed: %08lX\n", static_cast<unsigned long>(hr));
        return 3;
    }
    const auto capture = [&](CaptureEntry entry, const CaptureFrame &frame)
    {
        return draw_capture(device.Get(), context.Get(), entry, frame, observations.get());
    };
    // 真实 V1 skip 用例先要求原生像素，再要求不同颜色的提供方输出；不能靠旧输出通过。
    const CaptureFrame first { 512, 1024, publish_render, !fallback && !receipt && !receipt_skip };
    const CaptureFrame second { 512, 1024, publish_render, !fallback && !receipt, 704, 352, 128 };
    const bool draw = capture(CaptureEntry::draw, first);
    const bool indexed = capture(CaptureEntry::indexed, second);
    bool changed_size = true, restored_size = true;
    if (resize)
    {
        // 同一个 Render 实例在两个全尺寸池之间切换；不用原生像素或前一帧蓝色蒙混通过。
        const CaptureFrame smaller { 384, 768, publish_render, true, 192, 704, 352 };
        changed_size = capture(CaptureEntry::draw, smaller);
        restored_size = capture(CaptureEntry::indexed, first);
    }
    if (receipt)
    {
        const bool wrong_context = capture(CaptureEntry::draw, first);
        const bool wrong_output = capture(CaptureEntry::indexed, second);
        const bool unknown_status = capture(CaptureEntry::draw, first);
        const bool failed_ordered = capture(CaptureEntry::indexed, second);
        const CaptureFrame ordered { 512, 1024, publish_render, true, 192, 704, 352 };
        const bool confirmed = capture(CaptureEntry::draw, ordered);
        const bool continued = capture(CaptureEntry::indexed, ordered);
        std::fprintf(observations.get(), "receipt host: missing=%d stale=%d context=%d output=%d unknown=%d failed=%d ordered=%d continued=%d\n",
            draw, indexed, wrong_context, wrong_output, unknown_status, failed_ordered, confirmed, continued);
        return draw && indexed && wrong_context && wrong_output && unknown_status && failed_ordered &&
            confirmed && continued ? 0 : 1;
    }
    std::fprintf(observations.get(), "provider host: draw=%d indexed=%d resize_requested=%d resized=%d restored=%d\n",
        draw, indexed, resize, changed_size, restored_size);
    return draw && indexed && changed_size && restored_size ? 0 : 1;
}
