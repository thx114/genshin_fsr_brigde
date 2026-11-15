#pragma once

#include "../BridgeLogger.h"
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace neo::runtime
{
struct Config
{
    bool enabled = true;
    bool enable_logging = false;
    // 渲染精度 hook 总开关（ini [Dx11FsrBridge] RenderScaleMenu，默认 1=开启）。
    // 关闭后：不启动渲染精度菜单模块（不 hook 菜单文本/渲染命令构建/精度应用，
    // 也不扩展候选精度档位），游戏回到原生精度档位。用于 A/B 与故障隔离。
    bool render_scale_menu = true;
    // 日志系统配置（[Log] 段）。等级由调用点显式声明，不做内容推断。
    struct LoggingConfig
    {
        blog::Level level = blog::Level::Info;
        bool to_file = true;
        bool to_debugger = false;
        bool truncate_on_start = true;
        // 兼容模式（**默认关**）：把 `log_line()` 回调传来的裸字符串按消息前缀归类。
        //
        // ⚠️ 2026-09-23（`[待办 1]` ①）：本文件 **28 处调用点已全部迁移**到 `LOG_*` 宏，
        // 因此本开关**现在只服务一条路径** —— `initialize_render_scale_menu` 的回调
        // （`RenderScaleMenu.cpp` 只有 `void(const std::string&)` 这一种回调形态，
        // 携带不了分类）。**要移除本开关，须先改回调签名并迁移该模块的 18 处调用。**
        // 关掉时这些行统一落 `core/INFO`，**不会被静默丢弃**。
        bool compat_prefix_categories = false;
        std::uint32_t max_file_kb = 16384;
        std::uint32_t rotate_keep = 2;
        // 异步写队列容量（条）。此前 Config::queue_capacity 从未被读取，
        // 环形队列硬编码 16384，该字段是死配置。
        std::uint32_t queue_capacity = 8192;
        std::vector<std::pair<std::string, blog::Level>> categories;
        // load_config() 在 blog::init() **之前**执行，此时日志器还是关闭状态，
        // 配置解析期的告警会被直接丢弃（"日志撒谎"的同类问题：报告了等于没报告）。
        // 统一缓存到这里，init 之后立刻回放。
        std::vector<std::string> pending_warnings;
    };
    LoggingConfig logging;
    DWORD target_process_id = 0;
    std::wstring target_process_name;
    bool log_all_dispatch = false;
    bool log_resource_ops = false;
    bool log_loader_activity = false;
    bool log_interesting_dispatch_details = false;
    bool hook_present = false;
    bool final_scene_probe = false;
    std::uint32_t final_scene_probe_limit = 6;
    std::uint32_t final_scene_probe_signature_limit = 128;
    bool final_scene_snapshot = false;
    std::uint32_t final_scene_snapshot_interval_frames = 240;
    int dlssg_dxgi_workaround = -1;
    // Exposes HDR10 capability to the game while keeping the physical output
    // on the SDR color space. This is an isolated experimental path.
    bool hdr_swapchain_spoof = false;
    // Actively requests the legacy linear HDR color space on the physical
    // swapchain. It never rewrites swapchain or render-target formats.
    bool hdr_swapchain_force = false;
    // Records the system advanced-color capability query used by the engine.
    // This diagnostic path never changes the returned display state.
    bool hdr_environment_probe = false;
    // Records DXGI output color capabilities without changing output objects.
    bool hdr_output_desc_probe = false;
    // Reports HDR output capability to the game through the DXGI output
    // descriptor only. It does not change the physical display state.
    bool hdr_output_desc_spoof = false;
    // Experimental native-LDR test: request UNORM swapchain buffers instead of sRGB.
    bool native_ldr_swapchain_unorm = false;
    // Experimental process-start renderer replacement.  It is deliberately
    // opt-in: enables a D3D11On12 device plus a DX12 flip-model swapchain.
    bool dx11_on12_swapchain = false;
    // Experimental native-LDR test: rewrite only full-resolution RTV|SRV targets.
    bool native_ldr_final_target_unorm = false;
    // Compresses the spoofed HDR backbuffer to the SDR display range immediately
    // before Present. This is intentionally isolated from OptiScaler and the
    // game's render passes.
    bool hdr_sdr_tone_map = false;
    bool hdr_sdr_tone_map_pq_input = true;
    std::uint32_t hdr_sdr_tone_map_paper_white = 80;
    std::uint32_t hdr_sdr_tone_map_peak = 100;
    // Read-only identification of the last full-resolution draw into a known
    // swapchain target. It is used to select a future tone-map insertion point.
    bool hdr_composite_probe = false;
    std::uint32_t hdr_composite_probe_limit = 32;
    bool dump_pixel_shaders = false;
    bool trace_pixel_shader_draws = false;
    bool trace_texture_creates = false;
    std::uint32_t texture_trace_hotkey = VK_F11;
    std::uint32_t texture_trace_duration_ms = 10000;
    std::uint32_t texture_trace_limit = 128;
    std::uint64_t trace_pixel_shader_hash = 0x78057A29AF6C2D99ull;
    std::uint32_t pixel_shader_trace_limit = 512;
    std::uint64_t target_pixel_shader_hash = 0x78057A29AF6C2D99ull;
    std::uint32_t pixel_shader_replacement_mode = 0;
#if defined(DX11FSRBRIDGE_ENABLE_FSR2_TRANSLATION_EXPERIMENTAL)
    std::uint32_t fsr2_translation_mode = 0;
    bool fsr2_fast_state_tracking = false;
    bool fsr2_mode2_on_demand_state = true;
    std::uint32_t fsr2_output_validation_target = 0;
    bool fsr2_motion_vectors_jittered = false;
    bool fsr2_positive_motion_vector_scale = false;
    bool fsr2_use_reactive_mask = false;
    bool fsr2_use_transparency_mask = false;
    std::uint32_t fsr2_jitter_mode = 0;
    std::uint32_t fsr2_dump_input_textures = 0;
    // ⚠️ 2026-09-27：`fsr2_compare_output_capture` 已按死代码删除（零读取点、零消费者）。
    std::uint32_t fsr2_sharpness_percent = 0;
    bool fsr2_use_native_exposure = true;
    bool fsr2_compact_linear_output = false;
    bool fsr2_lock_color_producer_shader = true;
    bool fsr2_reset_on_color_path_change = false;
    bool fsr2_reset_on_optiscaler_config_change = false;
    std::uint32_t fsr2_optiscaler_config_reset_frames = 4;
    bool fsr2_reset_on_optiscaler_log_change = false;
    std::uint32_t fsr2_optiscaler_log_reset_duration_ms = 4000;
    bool fsr2_trace_color_producers = false;
    bool fsr2_early_output_probe = false;
    std::uint32_t fsr2_early_output_probe_frames = 60;
    bool block_dx11_on12_upscalers = true;
    // Phase 1：FSR2 5-PS 合成族跳过（默认关闭）。开启后在"上一次累积 pass 被桥成功替换"
    // 的前提下跳过 4 个 render-size 预处理 pass，消除双跑残余。见 Fsr2FamilyTakeover.h。
    bool fsr2_family_skip = false;
    std::uint32_t fsr2_family_expire_ms = 500;
    // Phase 1.5：il2cpp 调用点钩子（默认关闭）。FFX_FSR2::Render 的 inline patch。
    // observe 模式透传+计数（验证调用点/频率）；skip 模式实验性（切断 Mode 2 的
    // jitter/触发链，勿与 Fsr2TranslationMode=2 同开）。
    bool fsr2_il2cpp_hook = false;
    bool fsr2_il2cpp_skip_render = false;
    std::uint32_t fsr2_il2cpp_render_rva = 0x06B59670;
    std::uint32_t fsr2_il2cpp_ucb_rva = 0x06B59600;
    // 2026-08-26（AA 修复）：抓取游戏真实相机参数（near/far/FOV，RE 未确定）——
    // ConfigureJitteredProjectionMatrix(camera) 每帧被 OnPreCull 调用。
    bool ffx12_probe_camera = true;
    bool ffx12_camera_hook = false;    // 只在已验证完整序言后显式启用
    std::uint32_t ffx12_camera_rva = 0x06B558D0;
    bool ffx12_projection_hook = false; // 观察已构造的 Camera projection matrix
    std::uint32_t ffx12_projection_setter_rva = 0x013DC510;
    // 【正式功能】透明队列抖动修复（TransparentJitter，默认**开**）。
    // 强制 Camera.set_useJitteredProjectionMatrixForTransparentRendering 的实参为 true，
    // 让透明队列物件参与超分的时间重建（修复翅膀水晶羽片 / 月环边缘硬阶梯）。
    // 定位走签名发现（锚点 = 特征识别出的 ConfigureJitteredProjectionMatrix），不写死 RVA。
    // 详见 TransparentJitterHook.h。
    bool transparent_jitter = true;
    // 【诊断】分段性能探针（PerfProbe，默认**关**）。
    // 症状"帧率上不去但 GPU 占用低"⇒ 需要知道每帧的时间花在哪一段；
    // 见 PerfProbe.h。`PerfProbeIntervalMs` = 汇总行间隔（默认 1000 ms），
    // `PerfProbeDrawSample` = draw/dispatch 钩子的抽样步长（默认 32，避免逐 draw 计时）。
    bool perf_probe = false;
    std::uint32_t perf_probe_interval_ms = 1000;
    std::uint32_t perf_probe_draw_sample = 32;
    // 【正式功能】入口级过滤（B109，**默认开**）：`inspect_target_upscaler_draw_on_demand`
    // 的调用入口处用调用实参淘汰 `element_count != 3` 的绘制（零 COM 调用）。
    //
    // 依据（真机 `perf_probe_draw_shape_draw` 第 4 行元素数直方图）：`count == 3` 是
    // **唯一有 hits 的桶**（`elem_ix 3` / `elem_dr 3`），其余 24 个区间 hits 全 0，
    // `outside_hits = 0` ⇒ 样本内约 6000 次识别全部满足 `count == 3`；
    // 每区间约 121,100 次调用里可拒掉约 92.2% ⇒ 省约 `0.45 µs × 490 × 92.2% ≈ 203 µs/帧`。
    //
    // ⚠️ 代码层面**两套签名都不检查 `element_count`**（on-demand 路径的注释明说"不硬性
    // 要求 3"）⇒ 判据只是"样本内的必要条件"、**不是代码级等价** ⇒ 因此带**自我证伪保险**
    // （识别成功处检查 element_count，出现非 3 就一次性停用过滤器并打告警行，fail-open）
    // 与 **canary 抽样放行**（让保险可观测，见 PerfProbe.h 的说明）。
    bool draw_entry_filter = true;
    // canary 抽样步长：N = 每 N 个非 3 绘制放行 1 个进内省（保险的探针）；
    // 0 = 不放行（完全信任判据）；1 = 全部放行（等价于不过滤）。
    std::uint32_t draw_entry_filter_canary = 64;
    // Phase 1：进程内 FSR2 2.3.4 后端（ffxApi → amd_fidelityfx_upscaler_dx12.dll）。
    // 桥直接驱动 AMD SDK（不经 OptiScaler）；Phase 2 的调用点接管会调用它的 dispatch。
    bool ffx12 = false;
    std::wstring ffx12_dll_path;
    bool ffx12_fail_closed = false; // 禁止回退原生（测试/故障显式暴露）
    // 【正式功能】P1 单实例接管（默认**关** = 全量接管）：置 1 时多个 FSR2 上下文并存
    // 只接管**一个**实例，其余实例**放行游戏原生上采样**（返回 false，让游戏的累积 draw
    // 照常执行；返回 true 会吞掉那一路的累积 pass ⇒ 画面冻住）。
    // 历史（为什么默认不开）：引入时（2026-09-28）FFX12 的 GPU 互操作共享层是**全局单份**
    // （共享纹理/fence/单槽 pending/尺寸池），只支持一条流水线；现场实测有两个长期并存的
    // FSR2 上下文（各有一份 ffxContext、历史各归各的）⇒ 两路交替复用同一份共享资源，交付帧
    // 混进另一视角的角色，故先用 P1 消除"并存"。同日 f9e52b3 起该共享层改为**按实例独占**
    // （纹理/UAV/SRV/尺寸池/context/挂起槽全部按 instance_key 隔离，只剩 fence 是全局），
    // 68ed170（2026-10-02）遂把缺省改为 0 全量接管，实测左右 1:1 分屏两路各自 100% 接管
    // （0 拒绝 / 0 吞掉 / 无覆盖层 / 无冻结）。2026-11-10 合并 NGX 后端时被改回 1（回归，
    // 提交说明所谓"与发布 ini 对齐"不成立——改动前两处本来都是 0），2026-11-13 恢复 0。
    // ⚠️ NGX 后端例外：它的 feature/资源池是真全局单实例 ⇒ 无视本键，一律按 1 处理
    //   （见 single_instance_takeover_effective()）。
    bool ffx12_single_instance = false;
    // 接管哪一个（默认 0 = 自动）：0 = render 面积大者优先、面积相同取**最早出现**；
    // 1 = 强制最早出现；2 = 强制最新出现（与 1 互补：改一行 ini 即可换成"另一路"验证）。
    std::uint32_t ffx12_single_instance_pick = 0;
    // ---- P2 追加：诊断 + 两个**默认关**的实验开关（默认值下行为与 P1 完全一致）----
    // diag：把裁决的输入/输出逐项落日志（候选集的年龄/次序/面积、每次交棒的原因、
    //   接管权进入/退出事件、匹配路径）。用户需要证据定位"≤5 秒一次闪烁"的成因。
    bool ffx12_single_instance_diag = false;
    // sticky：粘滞裁决——只要上一任接管者仍在 hold_ms 内被匹配到就**不换人**。
    //   用途：判定"闪烁是否由交接棒本身引起"（A/B 实验）。代价见 ini 注释。
    bool ffx12_single_instance_sticky = false;
    std::uint32_t ffx12_single_instance_hold_ms = 10000;
    // strict：严格匹配实验——接管者这一 draw 若**不是**由"输出关联"匹配上的
    //   （即只靠 token bootstrap 认领，而两路 render 尺寸相同 ⇒ 无法区分），
    //   则不接管、放行原生。用于判定"闪烁是否由 视图↔实例 归属错配引起"。
    bool ffx12_single_instance_strict = false;
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    bool ffx12_probe = false; // 一次性槽位/cb0 探测（诊断用，默认关）
#endif
    // 后端输出读回采样（诊断）。**默认关**：它会 CopySubresourceRegion + Flush +
    // 同步 Map(READ)，阻塞渲染线程直到 GPU 画完，期间 GPU 空转。
    // 此前无条件运行，是正式版里唯一持续存在的 GPU 同步点。
    bool ffx12_readback_probes = false;
    // Present 参数/帧间隔诊断（默认关）。用于定位"焦点窗口下帧数降低"是否来自呈现限速。
    bool ffx12_present_probe = false;
    bool ffx12_feature_fallback = true; // 特征识别兜底：1=运行时特征优先+已有样本(硬编码)兜底；0=纯特征识别（验证用——关闭所有版本特定样本）
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    bool optiscaler_bridge_probe = false; // 遗留 OptiScaler 候选桥路径（frames.jsonl 记录，默认关）
#endif
    // 抖动生成模式。**默认 3**（2026-09-18 由 4 改为 3）：
    //   游戏 jitter 为 [0,1) 的 Halton 相位；FSR2 的 jitterOffset 期望**零中心**亚像素偏移。
    //   模式 4 = -(norm×width)      → 值域 [-1,0]，**恒定带 -0.5px 偏置**、从不跨零；
    //   模式 3 = -(norm×width)+0.5  → 值域 [-0.5,+0.5]，零中心（= v1.2.3 的公式）。
    // 证据（2026-09-18 issue）：DLSS M/L（第二代 transformer）对重投影相位敏感，
    //   模式 4 下累积朝错误方向 → 规则网格状黑线；改模式 3 即恢复正常。
    //   其他上采样模型对半像素偏置较宽容，故长期未被发现 —— 但偏置是**普遍存在**的。
    // 0=+norm*width-0.5, 1=+norm*width(符号反→整体抖), 2=raw, 3=-norm*width+0.5(默认), 4=-norm*width, 5=零
    std::uint32_t ffx12_jitter_mode = 3;
    bool ffx12_depth_inverted = true; // 游戏深度逆方向（0=far）；FSR2 默认 0=near
    bool ffx12_decode_motion = true;  // 游戏 motion 为 R10G10B10A2 平方编码 → 解码 R16G16_FLOAT
    bool ffx12_hdr_input = true;      // 游戏 10-bit HDR 管线
    bool ffx12_auto_exposure = true;  // 2026-08-25 用户确认：原神需要自动曝光
    bool ffx12_non_linear = true;     // 非线性色彩空间（OptiScaler 实证）
    float ffx12_velocity_factor = 0.5f; // FSR2 高亮稳定性（OptiScaler 用 0.5）
    // 运动矢量量级二分：保持已验证方向，只缩放送入 FSR2 的像素量级。
    // 1.0 为现有行为；0.5/2.0 用于隔离“解码量级错误”造成的历史重投影拖影。
    float ffx12_motion_scale = 1.0f;
    float ffx12_camera_near = 0.25f;
    float ffx12_camera_far = 6000.0f;
    // 同一 Render generation 的双缓冲输出只复制首个 SDK 结果，绝不二次推进 FSR2 history。
    bool ffx12_reuse_same_generation = true;
    bool ffx12_output_mark = false;     // 输出标记（诊断：验证画面来源是否 ffx12）
    float ffx12_fov_scale = 1.0f;       // FOV 缩放（AA 重投影验证：1.0=45°）
    // 2026-11-10 实机复测推翻 2026-08-25 的结论：本版本游戏**不再**滞后一帧。
    // 设 1（用上一帧 jitter）会让整个画面变模糊，且不解决 K 预设拖影；设 0 才与 FFX 持平。
    // 四处统一为 0：结构体默认 / load_config fallback / package.ini / Deploy-Bridge.ps1。
    bool ffx12_jitter_delay = false;
    bool ffx12_force_reset = false;   // 诊断：每帧强制 FSR2 reset（与正常对比：相同=历史零贡献/单帧重建）
    // 原生 D3D11 纯 GPU 传输（自建 D3D12 + NT 共享句柄 + D3D11.4 fence）。
    // 默认开：GpuInteropProbe 已在本机字节级验证；不劫持游戏设备为 On12（进场前冻结根因）。
    bool ffx12_gpu_interop = true;
#endif
    bool show_osd = false;
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    bool enable_similarity_probe = false;
    bool reset_similarity_on_recording = true;
#endif
    std::uint32_t candidate_limit_per_frame = 64;
    std::uint32_t interesting_dispatch_log_limit = 256;
    std::uint32_t interesting_dispatch_phase_gap_ms = 1500;
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    std::uint32_t similarity_report_interval_ms = 2000;
#endif
    std::wstring run_label;
};

struct ConfigLoad
{
    Config config;
    bool backend_auto = false;
};

// 启动阶段一次性读取并设置后端参数，再由调用方发布配置和安装 hook。
ConfigLoad read_config(const std::filesystem::path &module_dir);
std::string narrow(const std::wstring &value);
}
