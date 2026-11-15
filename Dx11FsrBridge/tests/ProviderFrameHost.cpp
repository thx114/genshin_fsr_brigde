#include <windows.h>
#include <d3d11.h>

#include "create-routing/DrawCapture.h"
#include "../game/AnchorPatterns.h"
#include "../ngx/F6EvalReceiptV1.h"
#include "../interop/BeforeSrV1.h"

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
static bool stage_receipt_fixture(const std::filesystem::path &bridge, const std::filesystem::path &provider,
    const wchar_t *provider_name)
{
    namespace fs = std::filesystem;
    const auto run = bridge.parent_path() / (L"receipt-run-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    fs::create_directory(run, error);
    if (error) return false;
    if (!fs::copy_file(bridge, run / L"Dx11FsrBridge.dll", fs::copy_options::overwrite_existing, error) ||
        !fs::copy_file(provider, run / provider_name, fs::copy_options::overwrite_existing, error))
        return false;
    fs::current_path(run, error);
    return !error;
}

namespace
{
using namespace neo::interop;
using Microsoft::WRL::ComPtr;
enum class FixtureRecord { replace, skip, wrong_size, wrong_device };
FixtureRecord g_record = FixtureRecord::replace;
DXGI_FORMAT g_expected_view = DXGI_FORMAT_UNKNOWN;
unsigned g_recorded = 0;
std::uint64_t g_last_frame = 0;
ComPtr<ID3D11Device> g_other_device;
ComPtr<ID3D11Texture2D> g_depth_witness, g_color_witness;
UnregisterBeforeSrV1 g_unregister = nullptr;
bool g_contract_ok = true;

HRESULT WINAPI record_fixture(const BeforeSrFrameV1 *frame, ID3D11Texture2D **out) noexcept
{
    ++g_recorded;
    if (!frame || !frame->context || !frame->color || !frame->depth || !frame->motion || !out ||
        frame->size != sizeof(*frame) || frame->version != 1 || frame->frame <= g_last_frame ||
        frame->color_storage_format != DXGI_FORMAT_R8G8B8A8_TYPELESS || frame->color_view_format != g_expected_view ||
        frame->color_transfer != ColorTransfer::unknown ||
        g_unregister(record_fixture) != HRESULT_FROM_WIN32(ERROR_BUSY))
    {
        g_contract_ok = false;
        return E_FAIL;
    }
    g_last_frame = frame->frame;
    ComPtr<ID3D11Device> device;
    frame->context->GetDevice(device.GetAddressOf());
    D3D11_TEXTURE2D_DESC desc {}, depth {}, motion {};
    frame->color->GetDesc(&desc);
    frame->depth->GetDesc(&depth);
    frame->motion->GetDesc(&motion);
    if (desc.Width != frame->render_width || desc.Height != frame->render_height ||
        depth.Width != desc.Width || depth.Height != desc.Height || depth.Format != DXGI_FORMAT_R32_FLOAT ||
        motion.Width != desc.Width || motion.Height != desc.Height || motion.Format != DXGI_FORMAT_R16G16_FLOAT)
    {
        g_contract_ok = false;
        return E_FAIL;
    }
    if (g_record == FixtureRecord::wrong_device) return g_other_device->CreateTexture2D(&desc, nullptr, out);
    if (g_record == FixtureRecord::wrong_size)
    {
        --desc.Width;
        return device->CreateTexture2D(&desc, nullptr, out);
    }
    const HRESULT hr = device->CreateTexture2D(&desc, nullptr, out);
    if (FAILED(hr)) return hr;
    if (g_record == FixtureRecord::skip) return S_FALSE; // 同时覆盖跳过时返还了非空引用。
    const D3D11_BOX center { desc.Width / 2, desc.Height / 2, 0, desc.Width / 2 + 1, desc.Height / 2 + 1, 1 };
    frame->context->UpdateSubresource(*out, 0, &center, &k_before_sr_filtered_color, sizeof(std::uint32_t), 0);
    desc.Width = desc.Height = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, g_color_witness.ReleaseAndGetAddressOf()))) return E_FAIL;
    desc.Format = depth.Format;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, g_depth_witness.ReleaseAndGetAddressOf()))) return E_FAIL;
    // 仅夹具读回：在回调期间排入原输入采样，Map 发生在 Evaluate/Draw 结束后。
    frame->context->CopySubresourceRegion(g_color_witness.Get(), 0, 0, 0, 0, frame->color, 0, &center);
    frame->context->CopySubresourceRegion(g_depth_witness.Get(), 0, 0, 0, 0, frame->depth, 0, &center);
    return S_OK;
}

bool exact_read(ID3D11DeviceContext &context, ID3D11Texture2D *texture, std::uint32_t expected)
{
    D3D11_MAPPED_SUBRESOURCE data {};
    if (!texture || FAILED(context.Map(texture, 0, D3D11_MAP_READ, 0, &data))) return false;
    std::uint32_t raw {};
    std::memcpy(&raw, data.pData, sizeof(raw));
    context.Unmap(texture, 0);
    return raw == expected;
}

HRESULT WINAPI unexpected_record(const BeforeSrFrameV1 *, ID3D11Texture2D **) noexcept
{
    g_contract_ok = false;
    return E_FAIL;
}
}

int wmain(int argc, wchar_t **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::wstring_view mode = argc > 1 ? argv[1] : L"";
    const bool receipt = mode == L"ngx-receipt";
    const bool before_sr = mode == L"ngx-before-sr";
    const bool fixture = receipt || before_sr;
    const wchar_t *fixture_name = receipt ? L"F6ReceiptProvider.dll" : L"BeforeSrProvider.dll";
    if (argc != 2 && (argc != 4 || !fixture))
    {
        std::puts("Usage: ProviderFrameHost <case> (stage DLLs in working directory)");
        std::puts("       ProviderFrameHost <ngx-receipt|ngx-before-sr> <bridge DLL> <fixture DLL>");
        return 2;
    }
    if (argc == 4 && !stage_receipt_fixture(argv[2], argv[3], fixture_name))
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
    const bool ngx = mode == L"ngx" || mode == L"ngx-resize" || fixture || real_receipt;
    const bool missing = mode == L"native-fallback";
    const bool reject = mode == L"native-reject";
    const bool async = mode == L"native-async" || native_resize || reject || fsr3 || fsr4;
    const bool fallback = missing || reject;
    if (!ngx && !fallback && !async && mode != L"native") return 2;
    const auto root = std::filesystem::current_path();
    const auto bridge = root / L"Dx11FsrBridge.dll";
    const auto provider = root / (fixture ? fixture_name : ngx ? L"OptiScaler.dll" :
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
    HMODULE provider_module = nullptr;
    if (ngx)
    {
        provider_module = LoadLibraryW(provider.c_str());
        if (!provider_module || !GetProcAddress(provider_module, "NVSDK_NGX_D3D11_Init_Ext")) return 3;
        const bool has_receipt = GetProcAddress(provider_module, neo::ngx::k_f6_evaluate_v1) != nullptr;
        const bool expect_receipt = receipt || real_receipt;
        std::fprintf(observations.get(), "provider export: v1=%d expected=%d\n", has_receipt, expect_receipt);
        if (has_receipt != expect_receipt) return 3;
    }
    const HMODULE bridge_module = LoadLibraryW(bridge.c_str());
    if (!bridge_module) return 3;
    const auto subscribe = reinterpret_cast<RegisterBeforeSrV1>(GetProcAddress(bridge_module, k_register_before_sr_v1));
    g_unregister = reinterpret_cast<UnregisterBeforeSrV1>(GetProcAddress(bridge_module, k_unregister_before_sr_v1));
    const auto query = reinterpret_cast<QueryBeforeSrFrameV1>(GetProcAddress(bridge_module, k_query_before_sr_frame_v1));
    if (fixture && (!subscribe || !g_unregister || !query)) return 3;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    const auto hr = D3D11CreateDevice(nullptr, fixture ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf());
    if (FAILED(hr))
    {
        std::printf("D3D11 device failed: %08lX\n", static_cast<unsigned long>(hr));
        return 3;
    }
    const auto capture = [&](CaptureEntry entry, const CaptureFrame &frame)
    {
        return draw_capture(device.Get(), context.Get(), entry, frame, observations.get());
    };
    if (before_sr)
    {
        constexpr auto srgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        constexpr auto unorm = DXGI_FORMAT_R8G8B8A8_UNORM;
        const auto run = [&](FixtureRecord mode, DXGI_FORMAT view, bool use_replacement, UINT size = 512)
        {
            g_record = mode;
            g_expected_view = view;
            CaptureFrame frame { size, size * 2, publish_render, true };
            frame.color_source_view = view;
            frame.exact_provider_pixel = use_replacement ? k_before_sr_filtered_color : k_before_sr_scene_color;
            return capture(CaptureEntry::draw, frame);
        };
        const bool without = run(FixtureRecord::skip, srgb, false) && g_recorded == 0;
        if (subscribe(record_fixture) != S_OK || FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, g_other_device.GetAddressOf(), nullptr, nullptr))) return 3;
        const bool unique = subscribe(unexpected_record) == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        const bool replaced = run(FixtureRecord::replace, srgb, true) &&
            exact_read(*context.Get(), g_color_witness.Get(), k_before_sr_scene_color) &&
            exact_read(*context.Get(), g_depth_witness.Get(), 0x3f000000u);
        const bool skipped = run(FixtureRecord::skip, unorm, false);
        const bool bad_size = run(FixtureRecord::wrong_size, srgb, false);
        const bool bad_device = run(FixtureRecord::wrong_device, unorm, false);
        const bool resized = run(FixtureRecord::replace, unorm, true, 384);
        const bool unsubscribed = g_unregister(record_fixture) == S_OK && g_recorded == 5 &&
            run(FixtureRecord::replace, srgb, false) && g_recorded == 5;
        std::fprintf(observations.get(), "before sr: without=%d unique=%d replaced=%d skipped=%d size=%d device=%d "
            "resized=%d unsubscribed=%d contract=%d\n", without, unique, replaced, skipped, bad_size, bad_device,
            resized, unsubscribed, g_contract_ok);
        g_color_witness.Reset();
        g_depth_witness.Reset();
        g_other_device.Reset();
        return without && unique && replaced && skipped && bad_size && bad_device && resized && unsubscribed &&
            g_contract_ok ? 0 : 1;
    }
    if (receipt && subscribe(unexpected_record) != S_OK) return 3;
    // 真实 V1 skip 用例先要求原生像素，再要求不同颜色的提供方输出；不能靠旧输出通过。
    CaptureFrame first { 512, 1024, publish_render, !fallback && !receipt && !receipt_skip };
    CaptureFrame second { 512, 1024, publish_render, !fallback && !receipt, 704, 352, 128 };
    if (receipt)
    {
        first.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        second.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
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
        CaptureFrame ordered { 512, 1024, publish_render, true, 192, 704, 352 };
        ordered.color_source_view = first.color_source_view;
        const bool confirmed = capture(CaptureEntry::draw, ordered);
        ordered.color_source_view = second.color_source_view;
        const bool continued = capture(CaptureEntry::indexed, ordered);
        using CheckScope = BOOL (WINAPI *)(ID3D11DeviceContext *, QueryBeforeSrFrameV1) noexcept;
        const auto check_scope = reinterpret_cast<CheckScope>(GetProcAddress(provider_module, "BeforeSrFixtureScopeClosed"));
        const bool exclusive = g_unregister(unexpected_record) == S_OK && g_contract_ok;
        const bool scope_closed = check_scope && check_scope(context.Get(), query);
        std::fprintf(observations.get(), "receipt host: missing=%d stale=%d context=%d output=%d unknown=%d "
            "failed=%d ordered=%d continued=%d exclusive=%d scope_closed=%d\n", draw, indexed, wrong_context,
            wrong_output, unknown_status, failed_ordered, confirmed, continued, exclusive, scope_closed);
        return draw && indexed && wrong_context && wrong_output && unknown_status && failed_ordered &&
            confirmed && continued && exclusive && scope_closed ? 0 : 1;
    }
    std::fprintf(observations.get(), "provider host: draw=%d indexed=%d resize_requested=%d resized=%d restored=%d\n",
        draw, indexed, resize, changed_size, restored_size);
    return draw && indexed && changed_size && restored_size ? 0 : 1;
}
