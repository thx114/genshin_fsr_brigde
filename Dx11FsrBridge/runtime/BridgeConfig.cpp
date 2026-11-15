#include "BridgeConfig.h"
#include "../Ffx12Backend.h"
#include "../Fsr2InputDump.h"
#include "../NgxBackend.h"

#include <Windows.h>
#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <cwctype>

namespace neo::runtime
{
std::string narrow(const std::wstring &value)
{
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

ConfigLoad read_config(const std::filesystem::path &module_dir)
{
    ConfigLoad loaded;
    auto &config = loaded.config;
    const std::filesystem::path config_path = module_dir / L"Dx11FsrBridge.ini";
    config.enabled = GetPrivateProfileIntW(L"Dx11FsrBridge", L"Enabled", 1, config_path.c_str()) != 0;
    // ⚠️ 2026-09-23（用户要求）：**不再读旧键 `EnableLogging`**。
    // 日志开关改由 `[Log] level` 派生（`level=off` 即关闭），见下方 [Log] 段。
    // 这里只给**保守初值**：在 [Log] 段解析完成前保持开启，避免早期日志被丢。
    config.enable_logging = true;
    config.dlssg_dxgi_workaround =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"DlssgDxgiWorkaround", -1, config_path.c_str());
    // ---- 【诊断】HDR / 输出路径（native-LDR）/ D3D11On12 / 元数据抓取 整块 ----
    // 归属判定（按"读取点 + 消费者"实证，不按名字猜）：
    //   - `HdrEnvironmentProbe` / `HdrOutputDescProbe` / `HdrCompositeProbe*` 是**只读观测**，
    //     源码注释本身就写着 "never changes the returned display state" ⇒ 纯诊断；
    //   - `HdrSwapchainSpoof` / `HdrSwapchainForce` / `HdrOutputDescSpoof` / `HdrSdrToneMap*` /
    //     `NativeLdr*` / `Dx11On12Swapchain` 是 "HDR / 输出路径" **实验装置**
    //     （伪装 HDR 能力再压回 SDR、改写交换链格式），发布 ini 模板里**一个键都没有**；
    //     发布的 HDR 通路是 `Ffx12Hdr` / `Ffx12PqChain` / `Ffx12NonLinear`，**不在本块内**；
    //     ini 模板头部已把"HDR 与输出路径探针"划归**开发构建专属**；
    //   - `CaptureMetadataOnly` 在读取点之外**没有任何消费者** ⇒ 幻影键 ⇒ 一并排除。
    // ⇒ 整块只在诊断构建里读取；发布构建连键名字符串都不进产物。
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    config.hdr_swapchain_spoof =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrSwapchainSpoof", 0, config_path.c_str()) != 0;
    config.hdr_swapchain_force =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrSwapchainForce", 0, config_path.c_str()) != 0;
    config.hdr_environment_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrEnvironmentProbe", 0, config_path.c_str()) != 0;
    config.hdr_output_desc_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrOutputDescProbe", 0, config_path.c_str()) != 0;
    config.hdr_output_desc_spoof =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrOutputDescSpoof", 0, config_path.c_str()) != 0;
    config.native_ldr_swapchain_unorm =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"NativeLdrSwapchainUnorm", 0, config_path.c_str()) != 0;
    config.dx11_on12_swapchain =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Dx11On12Swapchain", 0, config_path.c_str()) != 0;
    config.native_ldr_final_target_unorm =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"NativeLdrFinalTargetUnorm", 0, config_path.c_str()) != 0;
    config.hdr_sdr_tone_map =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrSdrToneMap", 0, config_path.c_str()) != 0;
    config.hdr_sdr_tone_map_pq_input =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrSdrToneMapPqInput", 1, config_path.c_str()) != 0;
    config.hdr_sdr_tone_map_paper_white = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"HdrSdrToneMapPaperWhite", 80, config_path.c_str())),
        1u,
        1000u);
    config.hdr_sdr_tone_map_peak = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"HdrSdrToneMapPeak", 100, config_path.c_str())),
        1u,
        10000u);
    config.hdr_composite_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"HdrCompositeProbe", 0, config_path.c_str()) != 0;
    config.hdr_composite_probe_limit = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"HdrCompositeProbeLimit", 32, config_path.c_str())),
        1u,
        512u);
#endif // DX11FSRBRIDGE_ENABLE_DIAGNOSTICS（HDR / native-LDR / D3D11On12）
    // FSR2 输入纹理转储（诊断，默认关）。放在无条件区，避免被下面的 #if 吞掉。
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    fsr2dump::configure(config_path.c_str());
#endif
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    // ⚠️ 2026-09-27：ini 键 `EnableFsr2GetProcAddressShim` 的读取点与
    //   `Config::enable_fsr2_get_proc_address_shim` 字段**已删除** —— 它是**幻影键**：
    //   读取点之外没有任何消费者（`fsr2_get_proc_address_shim_query_mask()` 走的是
    //   `DX11FSRBRIDGE_SERVER_DEBUG_RUNTIME` 构建宏，与本 ini 键无关）⇒ 用户设了等于没设。
    config.fsr2_translation_mode = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2TranslationMode", 2, config_path.c_str()));
    config.fsr2_mode2_on_demand_state =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2Mode2OnDemandState", 1, config_path.c_str()) != 0;
    config.fsr2_motion_vectors_jittered =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2MotionVectorsJittered", 0, config_path.c_str()) != 0;
    config.fsr2_positive_motion_vector_scale =
        // 2026-08-25（AA 修复，依据游戏 accumulate 反汇编）：游戏原生重投影 = UV − mv_g
        // （mv_g=+sign·4d² 正向、UV 空间）；FSR2 期望 historyUV = UV + scale·mv_sample。
        // 我们的解码 mv_sample = −mv_g（反向），故 scale 必须为 +renderSize（cb scale=+1）
        // → 有效运动 = −mv_g ✓ 与游戏一致。旧默认 0（−renderSize）把运动再翻转回 +mv_g
        // → 历史重投影方向全反 → 运动中历史每帧被拒/错位 → AA 退化为单帧放大（锯齿）。
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2MotionVectorScaleMode", 1, config_path.c_str()) == 1;
    config.fsr2_use_reactive_mask =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2UseReactiveMask", 1, config_path.c_str()) != 0;
    config.fsr2_use_transparency_mask =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2UseTransparencyMask", 0, config_path.c_str()) != 0;
    config.fsr2_jitter_mode = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2JitterMode", 3, config_path.c_str()));
    // ⚠️ 2026-09-27：ini 键 `Fsr2Hdr10PqColor` 的读取点与字段**已删除**（幻影键：
    //   声明 + 读取点之外无任何消费者）。
    config.fsr2_use_native_exposure =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2UseNativeExposure", 1, config_path.c_str()) != 0;
    // ⚠️ 2026-09-27：ini 键 `Fsr2FastMetadataCopy` 的读取点与字段**已删除**（幻影键同上）。
    config.fsr2_compact_linear_output =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2CompactLinearOutput", 1, config_path.c_str()) != 0;
    config.fsr2_lock_color_producer_shader =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2LockColorProducerShader", 1, config_path.c_str()) != 0;
    config.fsr2_reset_on_color_path_change =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2ResetOnColorPathChange", 1, config_path.c_str()) != 0;
    config.block_dx11_on12_upscalers =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"BlockDx11On12Upscalers", 0, config_path.c_str()) != 0;
    config.fsr2_family_skip =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2FamilySkip", 0, config_path.c_str()) != 0;
    config.fsr2_family_expire_ms = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"Fsr2FamilyExpireMs", 500, config_path.c_str())),
        100u,
        5000u);
    config.fsr2_il2cpp_hook =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2Il2CppHook", 0, config_path.c_str()) != 0;
    config.fsr2_il2cpp_skip_render =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2Il2CppSkipRender", 0, config_path.c_str()) != 0;
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    // 【诊断】相机/投影观测钩子（只读抓取游戏侧矩阵用于对照）——
    // 发布的相机参数走**静态 ini 值**（Ffx12CameraNear / Ffx12CameraFar / Ffx12FovScale），
    // 不依赖这三个钩子 ⇒ 归 B 组，只在诊断构建里读取。
    config.ffx12_probe_camera =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12ProbeCamera", 0, config_path.c_str()) != 0;
    config.ffx12_camera_hook =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12CameraHook", 0, config_path.c_str()) != 0;
    config.ffx12_projection_hook =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12ProjectionHook", 0, config_path.c_str()) != 0;
#endif
    // 【正式功能】透明队列抖动修复（TransparentJitter，默认 **1 = 修复生效**）。
    // ⚠️ 键名/段位必须与发布 ini 一致：段必须是 `[Dx11FsrBridge]`（放错段读不到，
    // 本项目踩过），且必须在**生产分支**读取（load_config 无条件执行，不受
    // DX11FSRBRIDGE_RELEASE_RUNTIME 影响）。
    //
    // ⚠️ 2026-09-27：**旧键兼容层已整体删除** —— `JitterFlagProbe` / `JitterFlagForce` /
    // `JitterFlagProbeFrames` / `JitterFlagHotkey` 的读取、旧语义折算（probe && force）、
    // 以及三条迁移告警串（`legacy_transparent_jitter_keys_ignored` /
    // `legacy_transparent_jitter_keys_deprecated` / `removed_transparent_jitter_keys_present`）
    // 全部移除。依据：发布更新方式是**覆盖安装**，不需要为旧版 ini 保留兼容；
    // 留兼容层只会造成"两个键都生效、谁也说不清谁管用"。
    // ⇒ 现在 `TransparentJitter` 是**唯一权威**，四个旧键**彻底不存在**（写了也无任何反应）。
    config.transparent_jitter =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"TransparentJitter", 1, config_path.c_str()) != 0;
    const auto read_hex_rva = [&](const wchar_t *key, std::uint32_t fallback) {
        wchar_t buf[32] {};
        GetPrivateProfileStringW(L"Dx11FsrBridge", key, L"", buf, static_cast<DWORD>(std::size(buf)),
                                 config_path.c_str());
        const std::wstring text(buf);
        if (text.empty())
            return fallback;
        try
        {
            return static_cast<std::uint32_t>(std::stoul(text, nullptr, 0));
        }
        catch (...)
        {
            return fallback;
        }
    };
    config.fsr2_il2cpp_render_rva = read_hex_rva(L"Fsr2Il2CppRenderRva", 0x06B59670);
    config.fsr2_il2cpp_ucb_rva = read_hex_rva(L"Fsr2Il2CppUcbRva", 0x06B59600);
    config.ffx12_camera_rva = read_hex_rva(L"Ffx12CameraRva", 0x06B558D0);
    config.ffx12_projection_setter_rva =
        read_hex_rva(L"Ffx12ProjectionSetterRva", 0x013DC510);
    // 国际服（GenshinImpact.exe——打包偏移不同）RVA 覆盖：g_MethodPointers
    // 30 方法 gap 匹配唯一命中（Render 0x06B580D0 / UCB 0x06B58060 / Camera 0x06B54420）
    {
        wchar_t exe_path[MAX_PATH] {};
        if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0)
        {
            std::wstring exe_name = std::filesystem::path(exe_path).filename().wstring();
            for (wchar_t &ch : exe_name)
                ch = static_cast<wchar_t>(towlower(ch));
            if (exe_name.find(L"genshinimpact") != std::wstring::npos ||
                exe_name.find(L"global") != std::wstring::npos)
            {
                config.fsr2_il2cpp_render_rva = read_hex_rva(L"Fsr2Il2CppRenderRvaGlobal", 0x06B580D0);
                config.fsr2_il2cpp_ucb_rva = read_hex_rva(L"Fsr2Il2CppUcbRvaGlobal", 0x06B58060);
                config.ffx12_camera_rva = read_hex_rva(L"Ffx12CameraRvaGlobal", 0x06B54420);
            }
        }
    }
    config.ffx12 =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12", 0, config_path.c_str()) != 0;
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    config.ffx12_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12Probe", 0, config_path.c_str()) != 0;
#endif
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    config.optiscaler_bridge_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"OptiScalerBridgeProbe", 0, config_path.c_str()) != 0;
#endif
    config.ffx12_jitter_mode = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12JitterMode", 3, config_path.c_str()));
    config.ffx12_depth_inverted =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12DepthInverted", 1, config_path.c_str()) != 0;
    config.ffx12_decode_motion =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12MotionDecode", 1, config_path.c_str()) != 0;
    config.ffx12_jitter_delay =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12JitterDelay", 0, config_path.c_str()) != 0;
    config.ffx12_force_reset =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12ForceReset", 0, config_path.c_str()) != 0;
    config.ffx12_hdr_input =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12Hdr", 1, config_path.c_str()) != 0;
    config.ffx12_auto_exposure =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12AutoExposure", 1, config_path.c_str()) != 0;
    config.ffx12_non_linear =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12NonLinear", 1, config_path.c_str()) != 0;
    ffx12::set_use_pq_chain(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12PqChain", 0, config_path.c_str()) != 0);
    config.ffx12_velocity_factor = std::clamp<float>(
        static_cast<float>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12VelocityFactor", 50, config_path.c_str())) / 100.0f,
        0.0f, 1.0f);
    {
        wchar_t motion_scale_buf[32] {};
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"Ffx12MotionScale", L"1.0",
                                 motion_scale_buf, static_cast<DWORD>(std::size(motion_scale_buf)),
                                 config_path.c_str());
    config.ffx12_motion_scale = std::clamp<float>(
            std::wcstof(motion_scale_buf, nullptr), 0.0f, 4.0f);
    }
    config.ffx12_reuse_same_generation =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12ReuseSameGeneration", 0,
                              config_path.c_str()) != 0;
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    // 【诊断】`Ffx12OutputMark`：源码注释即"输出标记（诊断：验证画面来源是否 ffx12）"，
    // 且消费点 `ffx12::set_output_mark()` 本身就在 `!RELEASE_RUNTIME` 内
    // ⇒ 发布构建里它连消费者都没有（幻影）⇒ 归 B 组。
    config.ffx12_output_mark =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12OutputMark", 0, config_path.c_str()) != 0;
#endif
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    ffx12::set_output_mark(config.ffx12_output_mark);
#endif
    {
        wchar_t fov_buf[32] {};
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"Ffx12FovScale", L"1.0", fov_buf,
                                 static_cast<DWORD>(std::size(fov_buf)), config_path.c_str());
        config.ffx12_fov_scale = std::clamp<float>(std::wcstof(fov_buf, nullptr), 0.1f, 3.0f);
    }
    {
        wchar_t near_buf[32] {}, far_buf[32] {};
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"Ffx12CameraNear", L"0.25", near_buf,
                                 static_cast<DWORD>(std::size(near_buf)), config_path.c_str());
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"Ffx12CameraFar", L"6000.0", far_buf,
                                 static_cast<DWORD>(std::size(far_buf)), config_path.c_str());
        config.ffx12_camera_near = std::clamp<float>(std::wcstof(near_buf, nullptr), 0.001f, 100.0f);
        config.ffx12_camera_far = std::clamp<float>(std::wcstof(far_buf, nullptr), 10.0f, 100000.0f);
    }
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    // 键归属（此前两行都写同一个 setter，第一行被第二行覆盖，等于死代码）：
    //   Ffx12DecodeTest       -> g_decode_test（输出标记/解码测试）
    //   Ffx12MotionDecodeTest -> g_motion_decode_test（motion 解码单独测试）
    // 两者是后端里两个独立标志，不要再让其中一个写两次。
    ffx12::set_decode_test(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12DecodeTest", 0, config_path.c_str()) != 0);
    ffx12::set_motion_decode_test(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12MotionDecodeTest", 0, config_path.c_str()) != 0);
    ffx12::set_debug_layer(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12DebugLayer", 0, config_path.c_str()) != 0);
#endif
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    // 【诊断】`Ffx12MotionDeadzone`：已实测**无效**的 motion 死区实验键
    // （审查矩阵：102 轮实测 motion<0.0005UV 归零无效果）⇒ 归 B 组，不再进发布构建。
    ffx12::set_motion_deadzone(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12MotionDeadzone", 0, config_path.c_str()) != 0);
#endif
    ffx12::set_depth_inverted(config.ffx12_depth_inverted);
    ffx12::set_decode_motion(config.ffx12_decode_motion);
    ffx12::set_motion_vectors_jittered(config.fsr2_motion_vectors_jittered);
    config.ffx12_gpu_interop =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12GpuInterop", 1, config_path.c_str()) != 0;
    ffx12::set_gpu_interop(config.ffx12_gpu_interop);
    // 2026-11-15：两种模式都把写回排在当前即时命令流中，避免目标在 Present 之前被读取。
    // 默认让 D3D11 批提交；设 0 在排入复制后显式 Flush，不在 CPU 上等待。
    ffx12::set_async_upscale(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12AsyncUpscale", 1, config_path.c_str()) != 0);
    ffx12::set_hdr_input(config.ffx12_hdr_input);
    // ---- D13 十键读取（SDS §3.13.1）+ 键 11 NgxCrossKeyReuse：紧邻 Ffx12Hdr 的读取点 ----
    // 此处 blog::init() 尚未执行 ⇒ 11 个 setter 只存值、一行日志都不打；
    // 唯一需要输出的是非法值告警，它先进 pending_warnings，由 initialize() 回放。
    //
    // ⚠️ Ffx12Backend 只接受**精确小写**的 auto / native / ngx：不 tolower、不 trim。
    //    缺键（lpDefault=L"auto"，与发布模板一致，见 AGENTS.md §二.7）与显式 auto 等效：
    //    先按 native 落位，等首个适配器信息到手再按型号判（resolve_backend_auto）；
    //    显式 native ⇒ native 且不告警；
    //    显式空值 / NGX / Native / 拼错 ⇒ native + backend_invalid 告警。
#if defined(DX11FSRBRIDGE_ENABLE_NGX)
    // ---- 键 1：Ffx12Backend（唯一走 ffx12 命名空间的键——不存在 ngx::set_backend）----
    wchar_t backend_buf[64] {};
    GetPrivateProfileStringW(L"Dx11FsrBridge", L"Ffx12Backend", L"auto",
                             backend_buf, static_cast<DWORD>(std::size(backend_buf)),
                             config_path.c_str());
    const std::wstring backend_value(backend_buf);
    const bool ngx_mode = (backend_value == L"ngx");
    const bool native_mode = (backend_value == L"native");
    const bool auto_mode = (backend_value == L"auto");
    if (!ngx_mode && !native_mode && !auto_mode)
        config.logging.pending_warnings.emplace_back(
            "ngx_core backend=native config_warn bits=backend_invalid");
    // auto：此刻还没有适配器信息（设备还没创建），先按 native 落位；
    // 真正的判定发生在 route_from_d3d11_device -> resolve_backend_auto（设备创建 hook 内，
    // 早于 swapchain_hook_needed() 的求值）。
    loaded.backend_auto = auto_mode;
    ffx12::set_backend(ngx_mode ? ffx12::BackendKind::Ngx : ffx12::BackendKind::Native);

    // ---- 键 2：NgxApplicationId（uint64；0 → 1337，warn 位由 set_ 内部记）----
    wchar_t appid_buf[32] {};
    GetPrivateProfileStringW(L"Dx11FsrBridge", L"NgxApplicationId", L"1337",
                             appid_buf, static_cast<DWORD>(std::size(appid_buf)),
                             config_path.c_str());
    const unsigned long long appid =
        static_cast<unsigned long long>(_wcstoui64(appid_buf, nullptr, 10));
    ngx::set_application_id(appid);

    // ---- 键 3：NgxFeatureDirs（分号分隔；空 = 自动探测）----
    wchar_t dirs_buf[2048] {};
    GetPrivateProfileStringW(L"Dx11FsrBridge", L"NgxFeatureDirs", L"",
                             dirs_buf, static_cast<DWORD>(std::size(dirs_buf)),
                             config_path.c_str());
    std::vector<std::wstring> ngx_dirs;   // 空 vector = 让 ensure_init 走自动探测
    if (dirs_buf[0] != L'\0')
    {
        const std::wstring all(dirs_buf);
        std::size_t pos = 0;
        while (pos <= all.size())
        {
            const std::size_t sep = all.find(L';', pos);
            const std::size_t end = (sep == std::wstring::npos) ? all.size() : sep;
            if (end > pos) ngx_dirs.emplace_back(all, pos, end - pos);
            if (sep == std::wstring::npos) break;
            pos = sep + 1;
        }
    }
    ngx::set_feature_dirs(ngx_dirs);

    // ---- 键 4：NgxHdr（T-8 三态：auto / 1 / 0；默认 0 = 强制 SDR）----
    // 为什么默认从 auto 改成 0（实机 E2 证据，同机同场景对比）：
    //   NgxHdr=auto ⇒ 解析成 HDR（create_flags=0xB，含 IsHDR）⇒ 角色走动/转视角时拖影明显；
    //   NgxHdr=0    ⇒ create_flags=0xA（无 IsHDR）⇒ 拖影大幅改善。
    // 机制：桥交给 NGX 的是 **PQ 码值**，不是线性光。置 IsHDR 会让 DLSS 按 HDR 管线解释这些
    //   码值，色调映射与运动补偿的判断偏离画面实际内容 ⇒ 走动/转视角时大面积拖影。
    // ⚠️ 该结论只在**绕过 OptiScaler** 时才能测出 —— OptiScaler 会把 IsHDR 强制改回 1。
    // auto 三态仍然可用（只是不再是默认）：
    //   auto = 自动探测（① swapchain color space > ② swapchain 缓冲格式 > ③ 输入纹理格式）；
    //   1 = 强制 HDR；0 = 强制 SDR。旧配置写 1/0 仍然有效。
    //   需要复现原行为（HDR 会话解析成 HDR）时，显式写 auto。
    {
        wchar_t hdr_buf[32] {};
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"NgxHdr", L"0",
                                 hdr_buf, static_cast<DWORD>(std::size(hdr_buf)),
                                 config_path.c_str());
        std::wstring hdr_s(hdr_buf);
        // trim 两侧空白
        const auto b = hdr_s.find_first_not_of(L" \t");
        const auto e = hdr_s.find_last_not_of(L" \t");
        hdr_s = (b == std::wstring::npos) ? std::wstring() : hdr_s.substr(b, e - b + 1);
        for (auto &ch : hdr_s) ch = static_cast<wchar_t>(towlower(ch));
        int hdr_mode = 0;   // auto
        if (hdr_s == L"1" || hdr_s == L"true" || hdr_s == L"on")       hdr_mode = 1;
        else if (hdr_s == L"0" || hdr_s == L"false" || hdr_s == L"off") hdr_mode = 2;
        // 其它（含 "auto"、空、无法识别）一律 auto
        ngx::set_hdr_mode(hdr_mode);
    }

    // ---- 键 5：NgxRenderPreset（P-11：默认 0 = 不设 preset，跟随 NGX 自身规则）----
    // 0 = 六个 preset 参数一个都不设，由 NGX 按性能档自行决定（与 OptiScaler 的
    // RenderPresetForAll=0 同义）。合法值 5..15 可显式指定档位。
    const int ngx_preset =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"NgxRenderPreset", 0, config_path.c_str());
    ngx::set_render_preset(ngx_preset);

    // ---- 键 6：NgxSetFrameTimeDelta（默认 1）----
    const int ngx_dt =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"NgxSetFrameTimeDelta", 1, config_path.c_str());
    ngx::set_frame_time_delta(ngx_dt != 0);

    // ---- 键 7：NgxUseReactiveMask（默认 0）----
    const int ngx_react =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"NgxUseReactiveMask", 0, config_path.c_str());
    ngx::set_use_reactive_mask(ngx_react != 0);

    // ---- 键 8：NgxLogLevel（默认 1；0=全关 1=error 2=info 3=debug）----
    const int ngx_lv =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"NgxLogLevel", 1, config_path.c_str());
    ngx::set_log_level(ngx_lv);

    // ---- 键 9：NgxPassthrough（默认 0）----
    const int ngx_pt =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"NgxPassthrough", 0, config_path.c_str());
    ngx::set_passthrough(ngx_pt != 0);
    ngx::set_before_sr_ao(GetPrivateProfileIntW(L"Dx11FsrBridge", L"NgxBeforeSrAo", 0, config_path.c_str()) == 1);

    // ---- 键 10：NgxProvider（2026-11-10 增，B154；默认 auto）----
    // 只接受**精确小写**的 auto / nvidia / external：与 Ffx12Backend 同风格（不 tolower、不 trim）。
    // 其它取值（显式空值 / 拼错 / ngx / NVIDIA）⇒ auto + provider_invalid 告警（位由 setter 记），
    // 因为"拼错"与"故意用默认"在日志里必须能区分开。
    {
        wchar_t provider_buf[32] {};
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"NgxProvider", L"auto",
                                 provider_buf, static_cast<DWORD>(std::size(provider_buf)),
                                 config_path.c_str());
        const std::wstring provider_value(provider_buf);
        int provider_mode = -1;   // -1 = 非法（交给 set_provider_mode 记位并回 auto）
        if (provider_value == L"auto")
            provider_mode = 0;
        else if (provider_value == L"nvidia")
            provider_mode = 1;
        else if (provider_value == L"external")
            provider_mode = 2;
        ngx::set_provider_mode(provider_mode);
    }
    // ---- 键 11：NgxCrossKeyReuse（默认 1）----
    // 1 = instance_key 变化但尺寸/格式全等时复用现有资源池与 feature（消除进出角色界面
    //     ~500ms 卡顿 + 短暂黑屏：根因是 destroy_instance() 的 g_release(feature)，实测 502ms）；
    // 0 = 每次 key 变化都销毁重建，与引入本键之前**逐字节同一条路径**。
    // ★ 语义是 int -> bool 折叠（!= 0），与上面 8 个二分开关一致：非 0 即开。
    const int ngx_ckr =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"NgxCrossKeyReuse", 1, config_path.c_str());
    ngx::set_cross_key_reuse(ngx_ckr != 0);
#else
    // ---- REQ-08：NGX 未编译进本产物 ----
    // 十个键里只有 Ffx12Backend 有消费者（后端选择器归 ffx12），其余 9 个 NGX 键在 OFF
    // 构型下没有任何消费者，故不读。缺键（lpDefault=L"auto"）/ 显式 auto ⇒ 交给
    // resolve_backend_auto 按型号判；显式 native ⇒ 静默 native（不告警）；
    // 精确 ngx 与非法值 ⇒ 强制 Native + 一条固定告警。
    {
        wchar_t backend_buf[64] {};
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"Ffx12Backend", L"auto",
                                 backend_buf, static_cast<DWORD>(std::size(backend_buf)),
                                 config_path.c_str());
        // auto 在本构型下**合法**、不告警：型号判为 RTX 时才有必要提示"想走 ngx 但没编进来"，
        // 而那时才拿得到适配器信息（由 resolve_backend_auto 打这一行）。
        const std::wstring backend_value(backend_buf);
        loaded.backend_auto = (backend_value == L"auto");
        if (backend_value != L"native" && !loaded.backend_auto)
            config.logging.pending_warnings.emplace_back(
                "ngx_core backend=native config_warn bits=ngx_not_compiled");
        ffx12::set_backend(ffx12::BackendKind::Native);
    }
#endif
    ffx12::set_auto_exposure(config.ffx12_auto_exposure);
    ffx12::set_non_linear(config.ffx12_non_linear);
    ffx12::set_velocity_factor(config.ffx12_velocity_factor);
    // 【正式功能】P1 单实例接管（默认**关** = 全量接管）——段必须是 `[Dx11FsrBridge]`。
    // 它读在诊断守卫之外：这是**功能键**（消费者改变行为），发布构建必须保留。
    // 缺省 0 必须与发布模板一致（Dx11FsrBridge.package.ini）：FFX 后端的共享层已按实例隔离
    // （f9e52b3），全量接管才是分屏场景的正确值；NGX 后端无视本键、一律按 1 处理
    // （见 single_instance_takeover_effective()）。
    config.ffx12_single_instance =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12SingleInstance", 0, config_path.c_str()) != 0;
    config.ffx12_single_instance_pick = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"Ffx12SingleInstancePick", 0, config_path.c_str())),
        0u,
        2u);
    // P2 追加：诊断 + 两个默认关的实验开关（同样是**功能键**，读在诊断守卫之外）。
    config.ffx12_single_instance_diag =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12SingleInstanceDiag", 0, config_path.c_str()) != 0;
    config.ffx12_single_instance_sticky =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12SingleInstanceSticky", 0, config_path.c_str()) != 0;
    config.ffx12_single_instance_hold_ms = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(GetPrivateProfileIntW(
            L"Dx11FsrBridge", L"Ffx12SingleInstanceHoldMs", 10000, config_path.c_str())),
        3000u,
        60000u);
    config.ffx12_single_instance_strict =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12SingleInstanceStrictMatch", 0, config_path.c_str()) != 0;
    // 后端侧同开关：池重建 / context 重建留痕（默认关；见 Ffx12Backend.h 的说明）
    ffx12::set_instance_diag(config.ffx12_single_instance_diag);
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    ffx12::set_dump_frames(static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12DumpFrames", 0, config_path.c_str())));
#endif
    {
        wchar_t ver_buf[32] {};
        // 默认请求最高版本前缀（4.x）——provider 的 GET_VERSIONS 按 GPU
        // 能力过滤，降级链 4→3→2 自动兜底；无需按显卡分类强制降级。
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"Ffx12Version", L"ffx12-fsr4.x", ver_buf,
                                 static_cast<DWORD>(std::size(ver_buf)), config_path.c_str());
        ffx12::set_sdk_version(narrow(ver_buf).c_str());
    }
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    // 【诊断】像素着色器 dump（把 .cso 落盘）。消费者 `dump_pixel_shader_bytecode` /
    // `dump_vertex_shader_bytecode` 本身都在 `!RELEASE_RUNTIME` 内 ⇒ 归 B 组。
    // 诊断 shader dump 也必须在 RELEASE 分支读取（非 RELEASE 分支的读取不生效）
    config.dump_pixel_shaders = GetPrivateProfileIntW(L"Dx11FsrBridge", L"DumpPixelShaders", 0, config_path.c_str()) != 0;
#endif
    // 后端输出读回采样（诊断，默认关）。开启会引入同步 GPU 等待 → 掉帧，仅排查用。
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    // 【诊断】`Ffx12ReadbackProbes`：**本项目唯一持续 GPU 同步读回点**
    // （Begin/End + GetData；源码注释即"开启会引入同步 GPU 等待 → 掉帧，仅排查用"）
    // ⇒ 归 B 组，发布构建整块不参与编译。
    config.ffx12_readback_probes =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12ReadbackProbes", 0, config_path.c_str()) != 0;
#endif
    // ⚠️ Fsr2FastStateTracking —— 生产构建下**必须读**，且默认开启（=1）。
    //
    // 该键门控的是**状态镜像钩子的整体旁路**，共 11 处：
    //   PSSetShaderResources / PSSetConstantBuffers / OMSetRenderTargets / RSSetViewports
    //   在"已学到目标 PS hash"后**整钩子直接透传**，不再做每调用的资源内省；
    //   PSSetShader 只记一个 hash；inspect_target_upscaler_draw 走廉价预筛。
    //
    // 而它在旧代码里**只在非 RELEASE 分支被读取**（生产构建定义
    // DX11FSRBRIDGE_RELEASE_RUNTIME=1 → 恒为默认 false）→ 这些旁路**从未生效**，
    // 每次资源绑定都执行 read_resource_info（GetResource/GetType/QueryInterface/
    // GetDesc + AddRef/Release）。实测后果（用户报告，9800X3D）：
    //   - 单核占用最高 80%（单线程热点）、GPU 空转（CPU 喂不上）
    //   - 渲染精度 0.2 与 0.5 帧数无差别（瓶颈不在像素吞吐）
    //   - 角色模型精度越高帧数越低（绑定调用数随 draw 数增长）
    // 默认值取 1：这是该开关的设计意图（学到目标后旁路），也是发布配置期望的行为。
    config.fsr2_fast_state_tracking =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2FastStateTracking", 1, config_path.c_str()) != 0;
    // ---- 【诊断束】呈现探针 / 着色器与纹理追踪 / 像素着色器替换 ----
    // 归属判定（读取点 + 消费者）：
    //   - `Ffx12PresentProbe` 只 LOG present 参数与帧间隔（源码注释：仅记录）；
    //   - `TracePixelShaderDraws` + `PixelShaderTraceLimit` + `TargetPixelShaderHash` +
    //     `PixelShaderReplacementMode` + `DumpPixelShaders` 是同一支"目标 PS 追踪/替换"实验
    //     （消费者只写 JSON 追踪流与替换 shader，不参与发布通路）；
    //   - `TraceTextureCreates` + `TextureTrace*`（热键触发的限时纹理创建追踪）同理。
    // ⇒ 整束归 B 组，只在诊断构建里读取。
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    // Present 参数/帧间隔诊断（默认关）。仅记录 sync_interval/flags/帧间隔/前台状态。
    config.ffx12_present_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12PresentProbe", 0, config_path.c_str()) != 0;
    config.trace_pixel_shader_draws =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"TracePixelShaderDraws", 0, config_path.c_str()) != 0;
    // 纹理创建追踪（热键触发的限时追踪）
    config.trace_texture_creates =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"TraceTextureCreates", 0, config_path.c_str()) != 0;
    config.texture_trace_hotkey = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"TextureTraceHotkey", VK_F11, config_path.c_str()));
    config.texture_trace_duration_ms = static_cast<std::uint32_t>(std::max<INT>(
        1, GetPrivateProfileIntW(L"Dx11FsrBridge", L"TextureTraceDurationMs", 10000, config_path.c_str())));
    config.texture_trace_limit = static_cast<std::uint32_t>(std::max<INT>(
        1, GetPrivateProfileIntW(L"Dx11FsrBridge", L"TextureTraceLimit", 128, config_path.c_str())));
    // 像素着色器替换（target hash 为 16 进制字符串）
    {
        wchar_t hash_buf[32] {};
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"TargetPixelShaderHash", L"78057A29AF6C2D99", hash_buf,
                                 static_cast<DWORD>(std::size(hash_buf)), config_path.c_str());
        wchar_t *hash_end = nullptr;
        const unsigned long long parsed = std::wcstoull(hash_buf, &hash_end, 16);
        if (hash_end != hash_buf)
            config.target_pixel_shader_hash = parsed;
        config.pixel_shader_replacement_mode = static_cast<std::uint32_t>(
            GetPrivateProfileIntW(L"Dx11FsrBridge", L"PixelShaderReplacementMode", 0, config_path.c_str()));
    }
    config.pixel_shader_trace_limit = static_cast<std::uint32_t>(std::max<INT>(
        1, GetPrivateProfileIntW(L"Dx11FsrBridge", L"PixelShaderTraceLimit", 512, config_path.c_str())));
#endif // DX11FSRBRIDGE_ENABLE_DIAGNOSTICS（呈现探针 / PS+纹理追踪 / PS 替换）
    // FSR2 锐化：**此前只在非 RELEASE 分支读取**，导致 sdk_in.enable_sharpening 那处
    // "日志撒谎修复"在生产里拿到的永远是默认 0 —— 等于该功能从未生效。
    config.fsr2_sharpness_percent = static_cast<std::uint32_t>(std::clamp<INT>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2SharpnessPercent", 0, config_path.c_str()), 0, 100));
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    // ---- 【诊断束】色彩产出者追踪 / 早输出探针（`Fsr2GpuTiming` 整支已按死代码删除）----
    //   - `Fsr2TraceColorProducers`：色彩产出者写历史追踪（F6 手动落盘）；
    //   - `Fsr2EarlyOutputProbe(+Frames)`：早输出探针。
    config.fsr2_trace_color_producers =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2TraceColorProducers", 0, config_path.c_str()) != 0;
    config.fsr2_early_output_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2EarlyOutputProbe", 0, config_path.c_str()) != 0;
    config.fsr2_early_output_probe_frames = static_cast<std::uint32_t>(std::max<INT>(
        1, GetPrivateProfileIntW(L"Dx11FsrBridge", L"Fsr2EarlyOutputProbeFrames", 60, config_path.c_str())));
#endif // DX11FSRBRIDGE_ENABLE_DIAGNOSTICS（色彩产出者追踪 / 早输出探针）
    {
        wchar_t buf[520] {};
        GetPrivateProfileStringW(L"Dx11FsrBridge", L"Ffx12DllPath", L"", buf,
                                 static_cast<DWORD>(std::size(buf)), config_path.c_str());
        config.ffx12_dll_path = buf;
        if (config.ffx12_dll_path.empty())
            // 默认指向 Bridge 目录自身副本（不再依赖 OptiScaler 目录布局——
            // OptiScaler 更新后其 SDK DLL 移入子目录，外路径失配导致 LoadLibrary 失败 → 黑屏）。
            // 正式版 SDK 统一放上一级 payload\AMD（Bridge 目录只留桥本体）。
            config.ffx12_dll_path = module_dir.parent_path() / L"AMD" / L"amd_fidelityfx_upscaler_dx12.dll";
        ffx12::set_sdk_dll_path(config.ffx12_dll_path.c_str());
    }
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    // 【诊断】`Ffx12FailClosed`：源码注释即"禁止回退原生（测试/故障显式暴露）"——
    // 它把 dispatch 失败从"回退原生"改成"直接失败"（=黑屏式故障注入）。
    // 审查文档也把它记为 fail-closed 诊断 ⇒ 归 B 组，发布构建不再读取。
    config.ffx12_fail_closed =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12FailClosed", 0, config_path.c_str()) != 0;
#endif
#if defined(DX11FSRBRIDGE_ENABLE_NGX)
    // 2026-11-09（PR #12 审查次要项 10）：`NgxPassthrough=1` 与 `Ffx12FailClosed=1` 的
    // **代码级互斥**。AGENTS.md §二.7 要求评估「两个『关闭/接管』语义的开关同时开启
    // 是否互相吞掉」—— 这一对恰好是其中最坏的一种：
    //   ① NgxPassthrough=1 ⇒ ngx::dispatch 在第 0 步就 return false（不超分）；
    //   ② 该 false 传回 ffx12::dispatch ⇒ 走到 draw 钩子的派发失败分支；
    //   ③ Ffx12FailClosed=1 ⇒ 该分支 return true = **吞掉这次 draw 且不产出任何图**。
    //   ⇒ 不超分 + 不回退 + draw 被吞 = **每帧黑屏**（不崩不冻，最难排查的一类）。
    //
    // 处理方向：**强制关掉 Ffx12FailClosed**，而不是关 passthrough。
    //   理由：passthrough 是用户显式要的观测手段（SDS 判据 14 依赖它），关掉它等于
    //   让用户「设了没反应」；而 fail_closed 是**故障注入式诊断**（源码注释即「禁止
    //   回退原生（测试/故障显式暴露）」），它的正确语义本就是「暴露失败」，
    //   不是「制造黑屏」。两者冲突时保留用户的观测意图、放弃故障注入，方向更安全。
    //
    // ★ 位置硬约束：必须在**两个键都读完**之后（NgxPassthrough 在键 9、Ffx12FailClosed
    //   在本块上方）且在任何消费者之前（唯一消费者是 draw 钩子的派发失败分支）。
    // ★ 为什么用 pending_warnings 而不是 LOG_WARN：load_config() 跑在 blog::init() 之前，
    //   此时直接写日志会被丢弃（见本文件 §logging 的说明与 pending_warnings 的回放点）。
    //
    // ★ 为什么用 ngx::passthrough() 而不是就地重读 ini，也不用局部变量 ngx_pt：
    //   · 不重读 ini：一次 GetPrivateProfileIntW 有文件解析开销，且会与键 9 形成
    //     **两个解析点**——两处若哪天改了一处默认值就会静默不一致。
    //   · 不用 ngx_pt：它虽然在同一函数作用域内可见（:4196，`#endif` 不关闭 C++ 作用域），
    //     但 ngx::passthrough() 返回的正是 4198 行 set_passthrough 存进去的那个值，
    //     也就是**下游消费者读到的同一个值** ⇒ 本互斥的判据与消费者同源，
    //     不会出现「互斥按 ini 判、消费者按全局判」的两套口径。
    //
    // ⚠️ 适用范围（如实记录）：`Ffx12FailClosed` 的**读取点**本身在
    //   `#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)` 内 ⇒ 发布构建里
    //   config.ffx12_fail_closed 恒为 false，本互斥在发布构建中**永不触发**。
    //   也就是说这对组合只在诊断构建里能凑出来 —— 但它恰恰是诊断构建最容易踩的组合
    //   （排查黑屏时同时开「直通」与「失败关闭」来定位到底哪一段吞了图），必须挡住。
    if (ngx::passthrough() && config.ffx12_fail_closed)
    {
        config.ffx12_fail_closed = false;
        config.logging.pending_warnings.emplace_back(
            "config_mutex key=Ffx12FailClosed forced=0 reason=NgxPassthrough=1 "
            "why=passthrough_plus_failclosed_black_screen");
    }
#endif
    // -----------------------------------------------------------------------
    // 日志配置。新方案：显式等级 + 分类过滤（[Log] 段），不做内容推断。
    // `[Log] level` 是**唯一权威**（缺省 info）。
    //
    // ⚠️ 2026-09-27：旧键存在性探测（`LogLevel` / `Ffx12FullLogging` / `EnableLogging`）
    // 与 `legacy_log_keys_ignored` 迁移告警**已删除** —— 发布更新方式是**覆盖安装**，
    // 不为旧版 ini 保留兼容。旧键现在**彻底不存在**（写了也不会有任何反应，
    // 也不会再污染"已废弃键"的告警噪声）。
    // -----------------------------------------------------------------------
    {
        wchar_t level_text[32] {};
        GetPrivateProfileStringW(L"Log", L"level", L"info", level_text,
                                 static_cast<DWORD>(std::size(level_text)), config_path.c_str());
        blog::Level parsed {};
        if (!blog::parse_level(narrow(level_text).c_str(), parsed))
        {
            // 解析失败也回落 info（而非 TRACE）—— 与"默认 info"一致
            parsed = blog::Level::Info;
            config.logging.pending_warnings.push_back(
                std::string("log_level_unparsed value=") + narrow(level_text) + " fallback=info");
        }
        config.logging.level = parsed;

        // 日志开关由等级派生：`level=off` 即关闭
        config.enable_logging = (parsed != blog::Level::Off);

        config.logging.to_debugger =
            GetPrivateProfileIntW(L"Log", L"to_debugger", 0, config_path.c_str()) != 0;
        config.logging.to_file =
            GetPrivateProfileIntW(L"Log", L"to_file", 1, config_path.c_str()) != 0;
        config.logging.truncate_on_start =
            GetPrivateProfileIntW(L"Log", L"truncate_on_start", 1, config_path.c_str()) != 0;
        config.logging.compat_prefix_categories =
            GetPrivateProfileIntW(L"Log", L"compat_prefix", 0, config_path.c_str()) != 0;
        config.logging.max_file_kb = std::clamp<std::uint32_t>(
            static_cast<std::uint32_t>(
                GetPrivateProfileIntW(L"Log", L"max_file_kb", 16384, config_path.c_str())),
            0u, 4u * 1024u * 1024u);
        config.logging.rotate_keep = std::clamp<std::uint32_t>(
            static_cast<std::uint32_t>(
                GetPrivateProfileIntW(L"Log", L"rotate_keep", 2, config_path.c_str())),
            0u, 8u);
        config.logging.queue_capacity = std::clamp<std::uint32_t>(
            static_cast<std::uint32_t>(
                GetPrivateProfileIntW(L"Log", L"queue_capacity", 8192, config_path.c_str())),
            1024u, 262144u);

        // 分类覆盖：[Log.Categories] 段，每行 category=level
        wchar_t section[4096] {};
        const DWORD section_len = GetPrivateProfileSectionW(
            L"Log.Categories", section, static_cast<DWORD>(std::size(section)), config_path.c_str());
        config.logging.categories.clear();
        // 不按 section_len 迭代：缓冲区不足时 GetPrivateProfileSectionW 返回 nSize-2，
        // 按该值推进可能踩到未写入区域。直接以双 NUL 结尾为准，天然不会越界。
        for (const wchar_t *cursor = section; *cursor != L'\0';)
        {
            const std::wstring row(cursor);
            cursor += row.size() + 1;
            if (row.empty())
                continue;
            const std::size_t eq = row.find(L'=');
            if (eq == std::wstring::npos || eq == 0)
                continue;
            const std::wstring key = row.substr(0, eq);
            const std::wstring value = row.substr(eq + 1);
            blog::Level cat_level {};
            if (blog::parse_level(narrow(value).c_str(), cat_level))
                config.logging.categories.emplace_back(narrow(key), cat_level);
        }
        // 段被缓冲区截断时必须显式暴露：否则表现为"某个分类覆盖静默不生效"。
        if (section_len >= static_cast<DWORD>(std::size(section)) - 2)
        {
            config.logging.pending_warnings.push_back("log_categories_section_truncated buffer=" +
                std::to_string(std::size(section)) + " parsed=" +
                std::to_string(config.logging.categories.size()));
        }
    }

    // 渲染精度 hook 总开关（**正式版必须在此读取**——非正式版分支另有一份同样的读取，
    // 只写一边会让该开关在正式版静默失效；历史上 trace 键正是这样失效过）。
    config.render_scale_menu =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"RenderScaleMenu", 1, config_path.c_str()) != 0;
#endif
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    config.show_osd = GetPrivateProfileIntW(L"Dx11FsrBridge", L"ShowOSD", 0, config_path.c_str()) != 0;
#endif
    config.ffx12_feature_fallback =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"Ffx12FeatureFallback", 1, config_path.c_str()) != 0;
    // ⚠️ 2026-09-27：ini 键 `AssumePhaseOrder` 的读取点与字段**已删除**，同时删除其
    //   唯一（死）消费者 `mode_name_for_phase()` —— 该函数全仓没有任何调用点，
    //   读到的值不改变任何行为（幻影键）。
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    config.enable_similarity_probe = GetPrivateProfileIntW(L"Dx11FsrBridge", L"EnableSimilarityProbe", 0, config_path.c_str()) != 0;
    config.reset_similarity_on_recording = GetPrivateProfileIntW(L"Dx11FsrBridge", L"ResetSimilarityOnRecording", 1, config_path.c_str()) != 0;
#endif
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    // 【诊断束】候选上限 + 有趣 dispatch 的日志节流参数。
    //   - `CandidateLimitPerFrame`：唯一消费者是 `maybe_track_fsr2_color_candidate`
    //     （每帧候选记录上限），消费者本身与色彩产出者追踪同属诊断观测；
    //   - `InterestingDispatch*`：只控制"有趣 dispatch"日志的条数与相位间隔 ⇒ 日志节流参数。
    config.candidate_limit_per_frame = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"CandidateLimitPerFrame", 64, config_path.c_str()));
    config.interesting_dispatch_log_limit = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"InterestingDispatchLogLimit", 256, config_path.c_str()));
    config.interesting_dispatch_phase_gap_ms = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"InterestingDispatchPhaseGapMs", 1500, config_path.c_str()));
#endif
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    config.similarity_report_interval_ms = static_cast<std::uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge", L"SimilarityReportIntervalMs", 2000, config_path.c_str()));
#endif
    wchar_t label_buffer[128] {};
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    GetPrivateProfileStringW(L"Dx11FsrBridge", L"RunLabel", L"", label_buffer, static_cast<DWORD>(std::size(label_buffer)), config_path.c_str());
    config.run_label = label_buffer;
#endif

    // 【诊断】分段性能探针（默认关）。⚠️ 三个键只在这里读一次（配置读取本身
    // 就是被怀疑的对象之一：`GetPrivateProfile*` 是文件/注册表 IO，
    // **绝不允许出现在每帧路径上** —— 本探针会把它计数并证明它不在热路径）。
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    config.perf_probe =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"PerfProbe", 0, config_path.c_str()) != 0;
    config.perf_probe_interval_ms = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"PerfProbeIntervalMs", 1000, config_path.c_str()));
    config.perf_probe_draw_sample = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"PerfProbeDrawSample", 32, config_path.c_str()));
#endif

    // 【正式功能】入口级过滤（B109）——**段必须是 `[Dx11FsrBridge]`**（放错段读不到，
    // 本项目踩过这个坑）。⚠️ 它与 `PerfProbe` 总开关**无关**：探针关着它照样生效
    // （保险的告警行也照样会打出来 —— 它走的是无条件注入的 sink）。
    //
    // `DrawEntryFilter=1`（默认）= 在调用入口用 `element_count == 3` 早退；
    // `=0` = 关闭，一切走原路径（保险触发后用户也可以手动关）。
    config.draw_entry_filter =
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"DrawEntryFilter", 1, config_path.c_str()) != 0;
    // canary 抽样步长（保险的探针，见 PerfProbe.h）：0 = 不放行；1 = 全部放行；N = 每 N 个 1 个。
    config.draw_entry_filter_canary = static_cast<std::uint32_t>(
        GetPrivateProfileIntW(L"Dx11FsrBridge", L"DrawEntryFilterCanary", 64, config_path.c_str()));
    return loaded;
}
}
