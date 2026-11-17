#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include "create-routing/DrawCapture.h"
#include "NgxInputMapFixture.h"
#include "NgxMultiSceneFixture.h"
#include "../game/AnchorPatterns.h"
#include "../ngx/F6EvalReceiptV1.h"
#include "../interop/BeforeSrV1.h"
#include "../ComMethodRoutes.h"
#include "../NgxBackend.h"

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

struct RenderFixture
{
    alignas(8) std::array<std::uint8_t, 0x80> fields {};
    std::uint32_t frame = 0;

    std::uint64_t key() const { return reinterpret_cast<std::uint64_t>(fields.data()); }

    void publish(CaptureExtent render, CaptureExtent display, float x, float y)
    {
        const auto put = [&](std::size_t offset, const auto &value)
        {
            std::memcpy(fields.data() + offset, &value, sizeof(value));
        };
        put(0x24, x);
        put(0x28, y);
        put(0x50, ++frame);
        put(0x58, render.width);
        put(0x5C, render.height);
        put(0x68, display.width);
        put(0x6C, display.height);
        using Observe = void (*)(void *, void *);
        reinterpret_cast<Observe>(k_methods.data() + k_render_offset)(fields.data(), nullptr);
    }
};

CaptureFrame square_frame(UINT size, bool expect_provider, UINT r = 32, UINT g = 96, UINT b = 900,
    bool same_size = false)
{
    static RenderFixture instance;
    const UINT display_size = same_size ? size : size * 2;
    const CaptureExtent render { size, size }, display { display_size, display_size };
    return { render, display, [=] { instance.publish(render, display, 0.f, 0.f); }, expect_provider, r, g, b };
}

using PixelMapFunction = HRESULT (STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT,
    D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
HRESULT STDMETHODCALLTYPE observe_pixel_map(ID3D11DeviceContext *, ID3D11Resource *, UINT,
    D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
using PixelMapRoutes = ComMethodRoutes<PixelMapFunction, &observe_pixel_map, 14, true>;
PixelMapRoutes g_pixel_map;
std::atomic<unsigned> g_pixel_map_attempts {}, g_pixel_map_successes {};

HRESULT STDMETHODCALLTYPE observe_pixel_map(ID3D11DeviceContext *context, ID3D11Resource *resource,
    UINT subresource, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *mapped)
{
    bool synchronous_pixel = false;
    if (resource && type == D3D11_MAP_READ && flags == 0)
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
        {
            D3D11_TEXTURE2D_DESC desc {};
            texture->GetDesc(&desc);
            synchronous_pixel = desc.Width == 1 && desc.Height == 1 &&
                desc.Usage == D3D11_USAGE_STAGING && (desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ) != 0;
        }
    }
    if (synchronous_pixel) g_pixel_map_attempts.fetch_add(1, std::memory_order_relaxed);
    const HRESULT result = g_pixel_map(context, resource, subresource, type, flags, mapped);
    if (synchronous_pixel && SUCCEEDED(result)) g_pixel_map_successes.fetch_add(1, std::memory_order_relaxed);
    return result;
}

bool run_pixel_readback(ID3D11Device &device, ID3D11DeviceContext &context, HMODULE provider,
    std::FILE *observations, bool expect_samples)
{
    const auto configure = reinterpret_cast<void (*)()>(GetProcAddress(provider, "ConfigureBeforeSrAoFixture"));
    const auto evaluations = reinterpret_cast<unsigned (*)()>(GetProcAddress(provider, "BeforeSrAoFixtureFrames"));
    if (!configure || !evaluations || evaluations() != 0 ||
        !com_vtable::patch(&context, 115, {route_slot(g_pixel_map)})) return false;
    configure();
    RenderFixture instance;
    const CaptureExtent render {32, 32}, display {64, 64};
    const unsigned interval = ngx::k_diagnostic_pixel_readback_interval;
    for (unsigned n = 1; n <= interval + 1; ++n)
    {
        CaptureFrame frame {render, display, [&] { instance.publish(render, display, 0.f, 0.f); }, true};
        frame.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM;
        frame.alternate_scene_pixel = n % 2 ? 0xFF314A65u : 0xFFA47228u;
        frame.exact_provider_pixel = frame.alternate_scene_pixel;
        // 2026-11-16（PID728）：不用 retained_textures；它自己的 1x1 输入读回会污染被测计数。
        // 同一宿主必须在旧 DLL 缺键和新 DLL 显式开启时观测到 6+2，零结果才有意义。
        const bool pixels = draw_capture(&device, &context, CaptureEntry::draw, frame, observations);
        const unsigned attempts = g_pixel_map_attempts.load(std::memory_order_relaxed);
        const unsigned successes = g_pixel_map_successes.load(std::memory_order_relaxed);
        const unsigned expected = expect_samples ? 6u + 2u * (n / interval) : 0u;
        const unsigned evaluated = evaluations();
        const bool correct = pixels && evaluated == n && attempts == expected && successes == expected;
        if (!correct || n == 1 || n == interval - 1 || n == interval || n == interval + 1)
            std::fprintf(observations,
                "pixel readback: render=%u eval_total=%u exact_current=%d map_attempts_total=%u "
                "map_successes_total=%u expected_total=%u\n", n, evaluated, pixels, attempts, successes, expected);
        if (!correct) return false;
    }
    std::fprintf(observations, "pixel readback complete: eval_total=%u exact_current_frames=%u\n",
        evaluations(), interval + 1);
    return true;
}

bool run_input_map(ID3D11Device &device, ID3D11DeviceContext &context, HMODULE provider,
    std::FILE *observations, bool delayed)
{
    using namespace neo::tests::input_map;
    const auto configure = reinterpret_cast<Configure>(GetProcAddress(provider, k_configure));
    const auto evaluate_count = reinterpret_cast<EvaluateCount>(GetProcAddress(provider, k_evaluate_count));
    if (!configure || !evaluate_count) return false;

    const CaptureExtent render { k_render_width, k_render_height }, display { k_display_width, k_display_height };
    RenderFixture instances[2];
    configure(delayed, instances[0].key(), instances[1].key());
    for (unsigned n = 0; n < std::size(k_frames); ++n)
    {
        const auto &test = k_frames[n];
        auto &instance = instances[test.instance];
        const float x = test.phase_x / render.width, y = test.phase_y / render.height;
        std::array<CaptureGuidePixel, k_probes.size()> guides {};
        for (std::size_t i = 0; i < guides.size(); ++i)
        {
            const auto &probe = k_probes[i];
            const auto &content = guide_at(n, i);
            guides[i] = { static_cast<UINT>(probe.x), static_cast<UINT>(probe.y), content.packed_mv, content.depth };
        }
        CaptureFrame frame { render, display, [&] { instance.publish(render, display, x, y); }, true };
        frame.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM;
        frame.alternate_scene_pixel = test.color;
        frame.exact_provider_pixel = test.color;
        frame.guides = guides;
        // 2026-11-15（PR #16）：不同的源/目标存储格式能检测误将源深度直接绑定给 Evaluate。
        frame.typeless_depth = true;
        // 最后一帧用同一绑定再次 Draw、不发布 Render；验证不会再进 F6。
        frame.replay_draw = n + 1 == std::size(k_frames);
        std::printf("source frame=%u instance=%c key=0x%llX game_frame=%u raw_jitter=(%.9g,%.9g) replay=%d\n",
            n + 1, 'A' + static_cast<int>(test.instance), static_cast<unsigned long long>(instance.key()),
            instance.frame + 1,
            x, y, frame.replay_draw);
        const bool written = draw_capture(&device, &context, CaptureEntry::draw, frame, observations);
        const unsigned count = evaluate_count();
        std::printf("delivery frame=%u eval_count_total=%u exact_output=%d\n", n + 1, count, written);
        if (!written || count != n + 1) return false;
    }
    return true;
}

bool run_scene_depth(ID3D11Device &device, ID3D11DeviceContext &context, HMODULE provider,
    std::FILE *observations)
{
    // 2026-11-16：同一实例只改变 DSV，SDK 必须继续收到实际 FSR 输入的 0.5 深度。
    const auto configure = reinterpret_cast<void (*)()>(GetProcAddress(provider, "ConfigureSceneDepthFixture"));
    if (!configure) return false;
    configure();
    RenderFixture instance;
    const CaptureExtent extent { 512, 512 };
    constexpr std::uint32_t colors[] { 0xFF216BC2u, 0xFF73B133u, 0xFFCF465Au, 0xFF3AA4E5u,
        0xFF8859C8u, 0xFFCCAB42u };
    for (unsigned n = 0; n < std::size(colors); ++n)
    {
        CaptureFrame frame { extent, extent, [&] { instance.publish(extent, extent, 0.f, 0.f); }, true };
        frame.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM;
        frame.alternate_scene_pixel = colors[n];
        frame.exact_provider_pixel = colors[n];
        frame.typeless_depth = true;
        if (n >= 2)
        {
            frame.scene_depth_format = n < 4 ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R32_TYPELESS;
            frame.scene_depth_readable = n >= 4;
        }
        const bool delivered = draw_capture(&device, &context, CaptureEntry::draw, frame, observations);
        std::fprintf(observations, "scene depth: frame=%u instance=%c dsv=%u readable=%d delivered=%d\n",
            n + 1, 'A', frame.scene_depth_format,
            frame.scene_depth_readable, delivered);
        if (!delivered) return false;
    }
    return true;
}

bool run_failure_recovery(ID3D11Device &device, ID3D11DeviceContext &context, HMODULE provider,
    std::FILE *observations)
{
    using Configure = void (*)() noexcept;
    using FrameCount = unsigned (*)() noexcept;
    const auto configure = reinterpret_cast<Configure>(GetProcAddress(provider, "ConfigureBeforeSrAoFixture"));
    const auto evaluate_count = reinterpret_cast<FrameCount>(GetProcAddress(provider, "BeforeSrAoFixtureFrames"));
    if (!configure || !evaluate_count) return false;
    configure();
    RenderFixture instances[2];
    struct Step
    {
        unsigned scene;
        bool publish, broken_motion, provider;
        unsigned evaluations;
    };
    constexpr Step steps[] {
        {0, true, false, true, 1}, {1, true, false, true, 2},
        {0, true, true, false, 2}, {0, false, false, false, 2},
        {1, true, false, true, 3}, {0, true, false, true, 4},
        {1, true, false, true, 5}, {0, true, false, true, 6},
    };
    for (unsigned n = 0; n < std::size(steps); ++n)
    {
        const auto &step = steps[n];
        const CaptureExtent extent {step.scene ? 384u : 512u, 256u};
        CaptureFrame frame {extent, extent, [&]
            {
                if (step.publish) instances[step.scene].publish(extent, extent, 0.f, 0.f);
            }, step.provider};
        // 2026-11-16：失败后的 Draw 不发布 Render，但仍绑定完整输入，专门检验无输出时的放行。
        frame.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM;
        frame.alternate_scene_pixel = 0xFF31629Au + n * 0x000B0703u;
        frame.exact_provider_pixel = frame.alternate_scene_pixel;
        frame.typeless_depth = true;
        if (step.broken_motion) frame.motion_source_format = DXGI_FORMAT_R32_UINT;
        const bool pixels = draw_capture(&device, &context, CaptureEntry::draw, frame, observations);
        const unsigned evaluated = evaluate_count();
        std::fprintf(observations,
            "failure recovery: step=%u scene=%c render=%d broken_motion=%d pixels=%d eval=%u expected=%u\n",
            n + 1, 'A' + step.scene, step.publish, step.broken_motion, pixels, evaluated, step.evaluations);
        if (!pixels || evaluated != step.evaluations) return false;
    }
    return true;
}

bool run_guide_views(ID3D11Device &device, ID3D11DeviceContext &context, HMODULE provider,
    std::FILE *observations)
{
    const auto configure = reinterpret_cast<void (*)()>(GetProcAddress(provider, "ConfigureBeforeSrAoFixture"));
    const auto evaluations = reinterpret_cast<unsigned (*)()>(GetProcAddress(provider, "BeforeSrAoFixtureFrames"));
    const auto creates = reinterpret_cast<unsigned (*)()>(GetProcAddress(provider, "ReceiptFixtureCreates"));
    const auto releases = reinterpret_cast<unsigned (*)()>(GetProcAddress(provider, "ReceiptFixtureReleases"));
    if (!configure || !evaluations || !creates || !releases) return false;
    configure();
    RenderFixture instances[2];
    CaptureTextures retained[2];
    struct Case { unsigned scene, width; bool unknown; DXGI_FORMAT view; };
    constexpr Case cases[] {
        {0, 512, false, DXGI_FORMAT_UNKNOWN}, {1, 512, false, DXGI_FORMAT_UNKNOWN},
        {0, 384, true, DXGI_FORMAT_R8G8_UNORM}, {0, 512, false, DXGI_FORMAT_UNKNOWN},
        {1, 384, true, DXGI_FORMAT_R8G8_UNORM}, {1, 512, false, DXGI_FORMAT_UNKNOWN},
        {0, 512, true, DXGI_FORMAT_R8G8_SNORM}, {0, 512, false, DXGI_FORMAT_UNKNOWN},
    };
    unsigned expected_evaluations = 0;
    for (unsigned n = 0; n < std::size(cases); ++n)
    {
        const auto &test = cases[n];
        const unsigned scene = test.scene;
        const CaptureExtent extent {test.width, 256};
        CaptureFrame frame {extent, extent,
            [&] { instances[scene].publish(extent, extent, 0.f, 0.f); }, !test.unknown};
        frame.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM;
        frame.alternate_scene_pixel = 0xFF314A65u + n * 0x00070305u;
        frame.exact_provider_pixel = frame.alternate_scene_pixel;
        frame.typeless_depth = true;
        frame.retained_textures = &retained[scene];
        if (test.unknown)
        {
            // 2026-11-16：重放日志中的资源类型，视图由夹具明确创建，不能据此推定游戏编码。
            frame.depth_source_format = DXGI_FORMAT_R32G8X24_TYPELESS;
            frame.motion_source_format = DXGI_FORMAT_R8G8_TYPELESS;
            frame.motion_source_view = test.view;
        }
        const bool pixels = draw_capture(&device, &context, CaptureEntry::draw, frame, observations);
        const unsigned expected_features = n == 0 ? 1 : 2;
        if (!test.unknown) ++expected_evaluations;
        std::fprintf(observations,
            "guide views: frame=%u scene=%c motion=%u view=%u pixels=%d eval=%u create=%u release=%u\n",
            n + 1, 'A' + scene, static_cast<unsigned>(frame.motion_source_format),
            static_cast<unsigned>(frame.motion_source_view), pixels, evaluations(), creates(), releases());
        if (!pixels || evaluations() != expected_evaluations || creates() != expected_features || releases() != 0)
            return false;
    }
    // 2026-11-16：只在诊断构型要求一次真实 DXBC/CB 回执；正式构型必须裁掉采集。
    std::string log;
    for (unsigned attempt = 0; attempt < 50; ++attempt)
    {
        std::ifstream file("Dx11FsrBridge.log", std::ios::binary);
        log.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (!DX11FSRBRIDGE_TEST_DIAGNOSTICS || log.find("before_sr_consumer_bindings") != std::string::npos) break;
        Sleep(10);
    }
    const auto first = log.find("before_sr_consumer_bindings");
    const bool captured = first != std::string::npos;
    if (captured != static_cast<bool>(DX11FSRBRIDGE_TEST_DIAGNOSTICS)) return false;
    if (captured && (log.find("before_sr_consumer_bindings", first + 1) != std::string::npos ||
        log.find("\"binding_bytes_complete\":true", first) == std::string::npos ||
        log.find("\"view_format\":49", first) == std::string::npos)) return false;
    return true;
}

bool run_candidate_admission(ID3D11Device &device, ID3D11DeviceContext &context, HMODULE provider,
    std::FILE *observations, bool generic)
{
    const auto configure = reinterpret_cast<void (*)()>(GetProcAddress(provider, "ConfigureBeforeSrAoFixture"));
    const auto evaluations = reinterpret_cast<unsigned (*)()>(GetProcAddress(provider, "BeforeSrAoFixtureFrames"));
    if (!configure || !evaluations) return false;
    configure();
    RenderFixture instances[2];
    unsigned expected = 0;
    bool all = true;
    for (unsigned n = 0; n < 6; ++n)
    {
        const unsigned scene = n % 2;
        const CaptureExtent render {128, 96}, display {256, 192};
        const bool dlaa = n >= 4;
        const auto actual_display = dlaa ? render : display;
        // 2026-11-16：假 Draw 和真 accumulation 之间不发布第二个 Render；真输出是未消费 token 的证据。
        CaptureFrame decoy {render, render,
            [&] { instances[scene].publish(render, actual_display, 0.f, 0.f); }, false};
        decoy.color_r10 = decoy.color_g10 = decoy.color_b10 = 1023;
        if (n < 2)
        {
            decoy.layout = CaptureLayout::lighting;
            if (n == 0)
            {
                decoy.motion_source_format = DXGI_FORMAT_R8G8_TYPELESS;
                decoy.motion_source_view = DXGI_FORMAT_R8G8_UNORM;
            }
        }
        else if (dlaa)
        {
            decoy.alias_history = true;
            decoy.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        const bool native = draw_capture(&device, &context, CaptureEntry::draw, decoy, observations);
        const unsigned after_decoy = evaluations();
        CaptureFrame real {render, actual_display, [] {}, true};
        real.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM;
        real.alternate_scene_pixel = 0xFF315B91u + n * 0x00090703u;
        real.exact_provider_pixel = real.alternate_scene_pixel;
        // 重排与 1:1 交替覆盖；不把合成 fixture 的常量 PS 宣传为 temporal 算法认证。
        real.layout = generic || n >= 3 ? CaptureLayout::reordered : CaptureLayout::standard;
        const bool delivered = draw_capture(&device, &context, CaptureEntry::draw, real, observations);
        ++expected;
        std::fprintf(observations,
            "candidate admission: step=%u scene=%c decoy=%s native_pixels=%d eval_after_decoy=%u "
            "expected_before=%u current_pixels=%d eval_after_real=%u expected_after=%u new_render=0\n",
            n + 1, 'A' + scene, n < 2 ? (n ? "lighting-packed" : "lighting-rg8") :
                dlaa ? "alias-history" : "wrong-display", native, after_decoy, expected - 1,
            delivered, evaluations(), expected);
        all &= native && after_decoy == expected - 1 && delivered && evaluations() == expected;
    }
    return all;
}

bool run_multi_scene(ID3D11Device &device, ID3D11DeviceContext &context, HMODULE provider,
    std::FILE *observations)
{
    using namespace neo::tests::multi_scene;
    const auto configure = reinterpret_cast<Configure>(GetProcAddress(provider, "ConfigureMultiSceneFixture"));
    const auto select = reinterpret_cast<Select>(GetProcAddress(provider, "SelectMultiSceneFrame"));
    const auto complete = reinterpret_cast<Complete>(GetProcAddress(provider, "MultiSceneComplete"));
    if (!configure || !select || !complete) return false;
    std::array<RenderFixture, k_scene_count> instances;
    std::array<std::array<CaptureTextures, 2>, k_scene_count> textures;
    std::array<unsigned, k_scene_count> draw_counts {};
    std::array<std::uint64_t, k_scene_count> keys {};
    for (std::size_t i = 0; i < keys.size(); ++i) keys[i] = instances[i].key();
    configure(keys.data());
    std::uint64_t batch_tick = 0;
    for (unsigned n = 0; n < std::size(k_frames); ++n)
    {
        const auto &test = k_frames[n];
        const CaptureExtent extent {test.width, test.height};
        auto &instance = instances[test.scene];
        std::uint64_t pending_age = 0;
        CaptureFrame frame {extent, extent, [&]
            {
                if (n < k_simultaneous_begin) instance.publish(extent, extent, 0.f, 0.f);
                else
                {
                    if ((n - k_simultaneous_begin) % k_scene_count == 0)
                    {
                        // 2026-11-16：第一笔 Draw 准备好后再发布四路，不让 shader 编译占用 token 窗口。
                        batch_tick = GetTickCount64();
                        for (unsigned offset = 0; offset < k_scene_count; ++offset)
                        {
                            const auto &pending = k_frames[n + k_scene_count - 1 - offset];
                            const CaptureExtent size {pending.width, pending.height};
                            instances[pending.scene].publish(size, size, 0.f, 0.f);
                        }
                    }
                    pending_age = GetTickCount64() - batch_tick;
                }
            }, true};
        frame.retained_textures = &textures[test.scene][draw_counts[test.scene]++ % 2];
        frame.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM;
        frame.alternate_scene_pixel = color(n);
        frame.exact_provider_pixel = color(n);
        frame.typeless_depth = true;
        // 2026-11-16：各路携带独立且无关的 DSV；可读性不能覆盖实际 FSR depth SRV。
        frame.scene_depth_format = test.readable_dsv ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT;
        frame.scene_depth_readable = test.readable_dsv;
        select(n);
        const bool delivered = draw_capture(&device, &context, CaptureEntry::draw, frame, observations);
        std::fprintf(observations,
            "multi scene: frame=%u scene=%c simultaneous=%d pending_age_ms=%llu delivered=%d\n",
            n + 1, 'A' + test.scene, n >= k_simultaneous_begin,
            static_cast<unsigned long long>(pending_age), delivered);
        if (!delivered) return false;
    }
    return complete();
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
    const bool prewarm_retry = mode == L"ngx-prewarm-retry";
    const bool prewarm = mode == L"ngx-prewarm" || prewarm_retry;
    const bool check_prewarm = prewarm || mode == L"ngx-prewarm-off";
    const bool receipt = check_prewarm || mode == L"ngx-receipt" || mode == L"ngx-receipt-dlaa";
    const bool same_size = mode == L"ngx-receipt-dlaa" || mode == L"ngx-v1-dlaa" ||
        mode == L"ngx-nvidia-dlaa";
    const bool producer_overwrite = mode == L"ngx-producer-overwrite";
    const bool producer_other_sdk = mode == L"ngx-producer-other-sdk";
    const bool producer_om_uav = mode == L"ngx-producer-om-uav";
    const int producer_write_operation = mode == L"ngx-producer-copy" ? 0 :
        mode == L"ngx-producer-copy-region" ? 1 : mode == L"ngx-producer-clear" ? 2 :
        mode == L"ngx-producer-update" ? 3 : -1;
    const bool producer_capture = producer_overwrite || producer_other_sdk || producer_om_uav ||
        producer_write_operation >= 0 || mode == L"ngx-producer-capture";
    const bool before_sr = mode == L"ngx-before-sr";
    const bool ao_v1 = mode == L"ngx-ao-v1";
    const bool f6_ao = mode == L"ngx-v1-ao-on" || mode == L"ngx-v1-ao-off";
    const bool nvidia_ao = mode == L"ngx-nvidia-ao-on" || mode == L"ngx-nvidia-ao-off";
    const bool present_before_draw = producer_capture || mode == L"ngx-v1-present" || mode == L"ngx-nvidia-present";
    const bool nvidia = nvidia_ao || mode == L"ngx-nvidia-present" || mode == L"ngx-nvidia-dlaa";
    const bool ao_enabled = mode == L"ngx-ao" || ao_v1 || mode.ends_with(L"-ao-on");
    const bool ao_fixture = mode == L"ngx-ao" || mode == L"ngx-ao-off" || ao_v1;
    const bool test_ao = ao_fixture || f6_ao || nvidia_ao;
    const bool delayed_input = mode == L"ngx-input-map-delayed";
    const bool input_map = mode == L"ngx-input-map-current" || delayed_input;
    const bool scene_depth = mode == L"ngx-scene-depth";
    const bool multi_scene = mode == L"ngx-multi-scene";
    const bool failure_recovery = mode == L"ngx-failure-recovery";
    const bool guide_views = mode == L"ngx-guide-views";
    const bool generic_admission = mode == L"ngx-candidate-admission-generic";
    const bool candidate_admission = mode == L"ngx-candidate-admission" || generic_admission;
    const bool pixel_readback_on = mode == L"ngx-pixel-readback-on";
    const bool pixel_readback_legacy = mode == L"ngx-pixel-readback-legacy";
    const bool pixel_readback = pixel_readback_on || pixel_readback_legacy ||
        mode == L"ngx-pixel-readback-default" || mode == L"ngx-pixel-readback-off";
    const bool fixture = receipt || before_sr || input_map || ao_fixture ||
        scene_depth || multi_scene || failure_recovery || producer_capture || guide_views || candidate_admission ||
        pixel_readback;
    const wchar_t *fixture_name = multi_scene ? L"NgxMultiSceneProvider.dll" :
        before_sr || producer_capture || scene_depth || failure_recovery || guide_views || candidate_admission ||
            pixel_readback ||
            (ao_fixture && !ao_v1) ? L"BeforeSrProvider.dll" :
        L"F6ReceiptProvider.dll";
    if (argc != 2 && (argc != 4 || !fixture))
    {
        std::puts("Usage: ProviderFrameHost <case> (stage DLLs in working directory)");
        std::puts("       ProviderFrameHost <ngx-receipt|ngx-before-sr|ngx-input-map-{current,delayed}|ngx-ao[-off|-v1]> "
            "<bridge DLL> <fixture DLL>");
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
    const bool real_receipt = mode == L"ngx-v1" || mode == L"ngx-v1-resize" || receipt_skip || f6_ao ||
        mode == L"ngx-v1-present" || mode == L"ngx-v1-dlaa";
    const bool resize = mode == L"ngx-resize" || mode == L"ngx-v1-resize" || native_resize;
    const bool ngx = mode == L"ngx" || mode == L"ngx-resize" || fixture || real_receipt || nvidia;
    const bool missing = mode == L"native-fallback";
    const bool reject = mode == L"native-reject";
    const bool async = mode == L"native-async" || native_resize || reject || fsr3 || fsr4;
    const bool fallback = missing || reject;
    if (!ngx && !fallback && !async && mode != L"native") return 2;
    const auto root = std::filesystem::current_path();
    const auto bridge = root / L"Dx11FsrBridge.dll";
    const auto provider = root / (fixture ? fixture_name : nvidia ? L"nvngx_dlss.dll" : ngx ? L"OptiScaler.dll" :
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
        "Fsr2Mode2OnDemandState=" << !generic_admission << "\nFsr2Il2CppHook=1\nFfx12=1\nFfx12Hdr=0\nFfx12NonLinear=0\n"
        "Ffx12GpuInterop=1\nFfx12JitterMode=" << (input_map ? 3 : 0) << "\nFfx12JitterDelay=" << delayed_input
        << "\nFfx12AsyncUpscale=" << async << '\n'
        << "Fsr2Il2CppRenderRva=0x" << std::hex << ucb + k_render_offset << "\nFsr2Il2CppUcbRva=0x" << ucb << '\n';
    if (generic_admission) config << "Fsr2FastStateTracking=0\n";
    if (ngx)
    {
        if (producer_capture || guide_views) config << "Fsr2TraceColorProducers=1\nFsr2FamilySkip=0\n";
        config << "Ffx12Backend=ngx\nNgxHdr=0\nNgxLogLevel=3\nNgxProvider="
            << (nvidia ? "nvidia" : "external") << '\n';
        if (pixel_readback_on || mode == L"ngx-pixel-readback-off")
            config << "NgxDiagnosticPixelReadback=" << pixel_readback_on << '\n';
        if (prewarm) config << "NgxPrewarmAtSwapchain=1\n";
        if (failure_recovery) config << "Fsr2FamilySkip=1\n";
        if (test_ao)
            config << "NgxBeforeSrAo=" << ao_enabled << "\nFfx12DepthInverted=1\nFfx12CameraNear=0.5\nFfx12CameraFar=100\n";
        if (input_map)
            config << "Fsr2MotionVectorScaleMode=1\nFfx12MotionScale=1.0\nFfx12MotionDecode=1\n"
                "Ffx12DepthInverted=1\nFsr2MotionVectorsJittered=0\nNgxCrossKeyReuse=1\n";
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
    // 2026-11-15：NVIDIA 组由桥按注册表解析核心；宿主只提供 DLSS 模型，不替它加载 OptiScaler。
    if (ngx && !nvidia)
    {
        provider_module = LoadLibraryW(provider.c_str());
        if (!provider_module || !GetProcAddress(provider_module, "NVSDK_NGX_D3D11_Init_Ext")) return 3;
        const bool has_receipt = GetProcAddress(provider_module, neo::ngx::k_f6_evaluate_v1) != nullptr;
        const bool expect_receipt = receipt || input_map || real_receipt || ao_v1 || multi_scene;
        std::fprintf(observations.get(), "provider export: v1=%d expected=%d\n", has_receipt, expect_receipt);
        if (has_receipt != expect_receipt) return 3;
        if (receipt && same_size)
        {
            using ConfigureDisplay = void (*)(unsigned, unsigned) noexcept;
            const auto configure = reinterpret_cast<ConfigureDisplay>(
                GetProcAddress(provider_module, "ConfigureReceiptFixtureDisplay"));
            if (!configure) return 3;
            configure(512, 512);
        }
    }
    using InitCalls = unsigned (*)();
    const auto init_calls = check_prewarm ? reinterpret_cast<InitCalls>(GetProcAddress(provider_module,
        "ReceiptInitCalls")) : nullptr;
    if (check_prewarm && (!init_calls || init_calls() != 0)) return 3;
    if (prewarm_retry)
    {
        const auto refuse = reinterpret_cast<void (*)()>(GetProcAddress(provider_module, "RefuseEarlyInit"));
        if (!refuse) return 3;
        refuse();
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
    struct FixtureWindow
    {
        HWND handle = nullptr;
        ~FixtureWindow() { if (handle) DestroyWindow(handle); }
    } window;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> presentation;
    if (present_before_draw || check_prewarm)
    {
        // 2026-11-16：旧桥普通 Draw 的回执通过，但真实 Present 后首帧捕获丢失；
        // 必须在读取输出前验证首个 SR，不能靠读 Map 重装后第二帧成功掩盖。
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
        if (FAILED(device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return 3;
        window.handle = CreateWindowExW(0, L"STATIC", L"Neo provider Present fixture", WS_POPUP,
            0, 0, 2560, 1600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Width = producer_capture ? 1024 : 2560;
        desc.Height = producer_capture ? 1024 : 1600;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        desc.SampleDesc.Count = 1;
        desc.BufferCount = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        if (!window.handle || FAILED(factory->CreateSwapChainForHwnd(device.Get(), window.handle, &desc,
            nullptr, nullptr, &presentation))) return 3;
    }
    if (check_prewarm && init_calls() != unsigned(prewarm)) return 4;
    const auto capture = [&](CaptureEntry entry, const CaptureFrame &frame)
    {
        if (presentation)
        {
            const HRESULT result = presentation->Present(0, 0);
            std::fprintf(observations.get(), "present host: hr=%08lX\n", static_cast<unsigned long>(result));
            if (FAILED(result)) return false;
        }
        return draw_capture(device.Get(), context.Get(), entry, frame, observations.get());
    };
    if (scene_depth)
        return run_scene_depth(*device.Get(), *context.Get(), provider_module, observations.get()) ? 0 : 1;
    if (multi_scene)
        return run_multi_scene(*device.Get(), *context.Get(), provider_module, observations.get()) ? 0 : 1;
    if (failure_recovery)
        return run_failure_recovery(*device.Get(), *context.Get(), provider_module, observations.get()) ? 0 : 1;
    if (guide_views)
        return run_guide_views(*device.Get(), *context.Get(), provider_module, observations.get()) ? 0 : 1;
    if (candidate_admission)
        return run_candidate_admission(*device.Get(), *context.Get(), provider_module, observations.get(),
            generic_admission) ? 0 : 1;
    if (pixel_readback)
        return run_pixel_readback(*device.Get(), *context.Get(), provider_module, observations.get(),
            pixel_readback_legacy || (pixel_readback_on && DX11FSRBRIDGE_TEST_DIAGNOSTICS)) ? 0 : 1;
    if (producer_capture)
    {
        RenderFixture target_instance, other_instance;
        const CaptureExtent render {512, 512}, display {1024, 1024};
        CaptureFrame frame {render, display, [&] { target_instance.publish(render, display, 0.f, 0.f); }, true};
        frame.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM;
        frame.exact_provider_pixel = frame.alternate_scene_pixel;
        frame.producer_roundtrip = true;
        frame.producer_overwrite = producer_overwrite;
        frame.producer_om_uav = producer_om_uav;
        frame.producer_write_operation = producer_write_operation;
        if (producer_other_sdk)
        {
            frame.producer_before_consume = [&]
            {
                // 2026-11-16：同 context、不同颜色和实例先经过真实 SDK，再回到原目标。
                CaptureFrame other {render, display,
                    [&] { other_instance.publish(render, display, 0.f, 0.f); }, true};
                other.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM;
                other.alternate_scene_pixel = 0xFF826437u;
                other.exact_provider_pixel = other.alternate_scene_pixel;
                const bool delivered = draw_capture(device.Get(), context.Get(), CaptureEntry::draw,
                    other, observations.get());
                std::fprintf(observations.get(), "producer other SDK: instance=%llu exact_pixels=%d\n",
                    static_cast<unsigned long long>(other_instance.key()), delivered);
                return delivered;
            };
        }
        const bool result = capture(CaptureEntry::draw, frame);
        // 实际异步 logger 写完后再由外层读取日志；不读取或改写游戏配置。
        std::fflush(observations.get());
        Sleep(100);
        std::ifstream log(root / L"Dx11FsrBridge.log");
        const std::string contents((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
        const auto begin = contents.find("before_sr_producer_bindings");
        const auto end = begin == std::string::npos ? begin : contents.find('\n', begin);
        const auto receipt = begin == std::string::npos ? std::string {} : contents.substr(begin, end - begin);
        const bool captured = receipt.find("\"binding_bytes_complete\":true") != std::string::npos &&
            receipt.find("\"sdk_observed\":true") != std::string::npos &&
            receipt.find("\"sdk_instance\":" + std::to_string(target_instance.key()) + ',') != std::string::npos &&
            receipt.find(producer_overwrite || producer_om_uav || producer_write_operation >= 0 ?
                "\"latest_writer_matches\":false" :
                "\"latest_writer_matches\":true") != std::string::npos &&
            receipt.find("\"transfer\":\"unknown\"") != std::string::npos &&
            receipt.find("\"writer_execution\":\"not-proven\"") != std::string::npos &&
            receipt.find("\"original_call_returned\":true") != std::string::npos &&
            (producer_write_operation < 0 ||
                (receipt.find("\"overwrite_count_window\":1") != std::string::npos &&
                    receipt.find("\"last_overwrite\":" + std::to_string(producer_write_operation) + ',') !=
                        std::string::npos)) &&
            receipt.find("0000403f0000403f") != std::string::npos;
        std::fprintf(observations.get(), "producer capture: pixels=%d binding_snapshot=%d\n", result, captured);
        return result && captured ? 0 : 1;
    }
    if (input_map)
    {
        std::printf("input map: mode=3 delay=%d render=%ux%u display=%ux%u "
            "source_depth=R32_TYPELESS/R32_FLOAT_SRV source_motion=R10G10B10A2_UNORM\n", delayed_input,
            neo::tests::input_map::k_render_width, neo::tests::input_map::k_render_height,
            neo::tests::input_map::k_display_width, neo::tests::input_map::k_display_height);
        return run_input_map(*device.Get(), *context.Get(), provider_module, observations.get(), delayed_input) ? 0 : 1;
    }
    if (test_ao)
    {
        using FrameCount = unsigned (*)() noexcept;
        FrameCount frames = nullptr;
        if (ao_fixture)
        {
            using Configure = void (*)() noexcept;
            const auto configure = reinterpret_cast<Configure>(
                GetProcAddress(provider_module, "ConfigureBeforeSrAoFixture"));
            frames = reinterpret_cast<FrameCount>(GetProcAddress(provider_module, "BeforeSrAoFixtureFrames"));
            if (!configure || !frames || (ao_v1 && subscribe(unexpected_record) != S_OK)) return 3;
            configure();
        }
        // 同尺寸、同颜色、同实例内先跑平面，再加入左侧的近表面。
        // 实际 Evaluate 必须读到不同的颜色；不要从 shader 结果钉死测试期望。
        const auto guide_depth = [](float z)
        {
            return 0.5f * (100.f - z) / (z * (100.f - 0.5f));
        };
        constexpr unsigned size = 512, center = size / 2;
        constexpr unsigned zero_motion = (512u << 10) | 512u;
        const float plane = guide_depth(3.45f);
        std::vector<CaptureGuidePixel> wall;
        // 保持中心后景；改变整个半平面的深度，不能依赖一个恰巧相交的 shader 精确半径。
        for (UINT y = center - 21; y <= center + 21; ++y)
            for (UINT x = center - 21; x <= center + 21; ++x)
                wall.push_back({x, y, zero_motion, guide_depth(x < center - 2 ? 3.0f : 3.45f)});
        std::vector<CaptureGuidePixel> flat = wall;
        for (auto &pixel : flat) pixel.depth = plane;
        auto test = square_frame(size, true);
        test.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM;
        test.exact_provider_pixel = test.alternate_scene_pixel;
        test.guides = flat;
        test.typeless_depth = true;
        const bool neutral = capture(CaptureEntry::draw, test) && (!frames || frames() == 1);
        test.guides = wall;
        test.expect_attenuated_color = ao_enabled;
        const bool contact = capture(CaptureEntry::indexed, test) && (!frames || frames() == 2);
        const bool external_recorder_excluded = !ao_v1 || (g_contract_ok && g_unregister(unexpected_record) == S_OK);
        // 2026-11-15：真实 F6 不暴露 mock 计数接口，由外层同时核验 F6 回执与桥的写回。
        const char *label = nvidia_ao ? "nvidia" : f6_ao ? "f6" : ao_v1 ? "fixture-v1" : "fixture-legacy";
        std::fprintf(observations.get(), "native ao: enabled=%d provider=%s neutral=%d contact=%d",
            ao_enabled, label, neutral, contact);
        if (frames) std::fprintf(observations.get(), " eval_total=%u exclusive=%d", frames(), external_recorder_excluded);
        std::fputc('\n', observations.get());
        return neutral && contact && external_recorder_excluded ? 0 : 1;
    }
    if (before_sr)
    {
        constexpr auto srgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        constexpr auto unorm = DXGI_FORMAT_R8G8B8A8_UNORM;
        const auto run = [&](FixtureRecord mode, DXGI_FORMAT view, bool use_replacement, UINT size = 512)
        {
            g_record = mode;
            g_expected_view = view;
            auto frame = square_frame(size, true);
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
    // 2026-11-16（同尺寸入口）：只改变尺寸比，原生红/绿与提供方两帧颜色仍须独立读回。
    auto first = square_frame(512, !fallback && !receipt && !receipt_skip, 32, 96, 900, same_size);
    auto second = square_frame(512, !fallback && !receipt, 704, 352, 128, same_size);
    if (same_size)
        std::fprintf(observations.get(), "provider dimensions: render=%ux%u display=%ux%u\n",
            first.render.width, first.render.height, first.display.width, first.display.height);
    if (receipt)
    {
        first.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        second.color_source_view = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    const bool draw = capture(CaptureEntry::draw, first);
    const bool indexed = capture(CaptureEntry::indexed, second);
    if (check_prewarm && init_calls() != (prewarm_retry ? 2u : 1u)) return 4;
    bool changed_size = true, restored_size = true;
    if (resize)
    {
        // 同一个 Render 实例在两个全尺寸池之间切换；不用原生像素或前一帧蓝色蒙混通过。
        const auto smaller = square_frame(384, true, 192, 704, 352);
        changed_size = capture(CaptureEntry::draw, smaller);
        restored_size = capture(CaptureEntry::indexed, first);
    }
    if (receipt)
    {
        const bool wrong_context = capture(CaptureEntry::draw, first);
        const bool wrong_output = capture(CaptureEntry::indexed, second);
        const bool unknown_status = capture(CaptureEntry::draw, first);
        const bool failed_ordered = capture(CaptureEntry::indexed, second);
        auto ordered = square_frame(512, true, 192, 704, 352, same_size);
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
