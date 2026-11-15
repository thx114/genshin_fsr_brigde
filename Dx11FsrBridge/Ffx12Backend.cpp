#include "Ffx12Backend.h"
#include "Ffx12ShaderSource.h"   // 两份 HLSL 正文（T-1 迁出；native 与 ngx 共用）
#include "NgxBackend.h"          // ngx::dispatch / ngx::should_release_all（T-6 分流点）
#include "PerfProbe.h"
#include "native/InteropQueue.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <d3dx12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "ffx_api.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <array>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

using namespace Microsoft::WRL;

namespace ffx12
{
namespace
{

std::atomic_bool g_active { false };
// timed_mutex：shutdown() 必须带超时取锁 —— dispatch() 全程持此锁且中间会阻塞在
// GPU fence 等待上，无限等待会让进程永久挂死在退出路径（见 shutdown() 注释）。
std::timed_mutex g_mutex;
std::wstring g_sdk_path;
// 实际装载的 SDK 路径（与 g_sdk_path 区分——g_sdk_path 会被路由覆盖，
// 而句柄可能来自 preload 的旧路径；装载路径不一致时必须释放重载，否则版本列表错配）。
std::wstring g_sdk_loaded_path;
// preload 候选缓存（提前装载防 OptiScaler LoadLibrary hook 劫持——
// 实测 402c 文件名同样被劫持 err=18）。init_locked 按最终路径从缓存取句柄，不再走 LoadLibrary。
struct PreloadEntry
{
    std::wstring path;
    HMODULE module;
};
std::vector<PreloadEntry> g_preloaded;

// 进程退出标志：由 DllMain(DLL_PROCESS_DETACH, lpReserved != nullptr) 置位。
// 进程终止时**禁止** FreeLibrary：那时已持有 loader lock，释放模块会死锁/崩溃
// （Windows 明确不建议在 DllMain 里调用 FreeLibrary）。此时把句柄交给 OS 回收即可。
std::atomic<bool> g_process_exiting { false };

// 停机已开始标志：由 shutdown() 在取到 g_mutex 之前置位。
// dispatch() 入口检查它并立即返回——避免 atexit 里的 shutdown 正在释放 D3D12 资源时，
// 仍有渲染线程进来并发使用这些资源（atexit 执行时机不保证所有线程都已退出）。
std::atomic<bool> g_shutting_down { false };

ComPtr<ID3D11Device> g_d11dev;
ComPtr<ID3D11On12Device2> g_on12dev;
ComPtr<ID3D12Device> g_d12dev;
neo::native::InteropQueue g_interop;
bool g_dx11on12_available = false;
bool g_gpu_only_transport_available = false;
// async 不在 Present 补交付：只省显式 Flush，仍先在消费该纹理的即时 context 排入 Wait/Copy。
bool g_async_upscale = true;
std::uint64_t g_delivered_count = 0;

// ffx-api runtime entry points.  Keeping the module loaded for the lifetime of
// every context is required because the provider owns the temporal history.
HMODULE g_sdk_module = nullptr;
struct RuntimeFns
{
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxConfigure configure = nullptr;
};
RuntimeFns g_runtime {};
std::uint64_t g_sdk_version_id = 0;

// 共享纹理（D3D11 侧 NT 共享句柄；D3D12 侧 OpenSharedHandle）。
// P2 起**每实例一份**（原先三张全局单份 + 深度/运动/reactive 单份）。
struct SharedTex
{
    ComPtr<ID3D11Texture2D> d11;
    ComPtr<ID3D12Resource> d12;
};

// P2：每个 ffxContext（= 每个 instance_key）独占一整套 GPU 互操作资源。
//
// 为什么必须拆（M4，P1 已实机确证）：游戏同时驱动多个 FSR2 上下文时（双相机/画中画），
// 本后端的互操作资源此前是**进程内全局单份**，两条流水线交替复用它 ⇒ 交付画面里混进
// 另一路视角的内容（跟随角色剪影的半透明覆盖层）。P1 把并存改成"只接管一个"后该问题
// 消失，反证根因就在这份共享层，而不是 provider 内部。
//
// 因此这里把原全局单份逐项落到实例上：
//   · 5 张共享纹理及视图（颜色/深度/解码运动/reactive/输出）；
//   · 尺寸池判等键（rw/rh/dw/dh —— 甲实例改尺寸不再重建乙实例的纹理）；
//   · 调用方 game_context 与 ctx4（★ 写入用调用方 context，Signal/Wait 也必须用**同一个**，
//     否则 D3D12 会在输入拷贝尚未提交时就等到 v1，读到上一实例留在共享纹理里的旧帧）；
//   · b0 常数缓冲（depth 提取与 motion 解码共用 b0.x，全局一份会让两实例互相刷值）；
// fence 仍是全局单条：它是"全局单调时间线"，只要 Signal 严格递增、Wait 指向已/将产生的
// 值，多实例交替 Signal 同一 fence 的等待语义依然成立（协议见 dispatch_gpu_shared 注释）。
struct InstanceRes
{
    // render 尺寸四张输入 + display 尺寸输出。
    SharedTex tex_color;            // 游戏颜色（同格式；D3D11 CopyResource 写入）
    SharedTex tex_depth_share;      // R32_FLOAT 深度提取输出（D3D11 CS 写 → D3D12 直读）
    SharedTex tex_motion_cvt_share; // R16G16_FLOAT 运动解码输出（同上）
    SharedTex tex_reactive_share;   // R8_UNORM reactive（motion B 通道，同 CS 的 u1）
    SharedTex tex_output;           // display 尺寸；FFX UAV 输出 → D3D11 CopyResource 回游戏
    // 这套池的尺寸 = **按实例的判等键**（甲实例改尺寸不得重建乙实例的纹理）
    std::uint32_t rw = 0, rh = 0, dw = 0, dh = 0;
    // ---- 视图（生命周期跟随上面的纹理）----
    ComPtr<ID3D11UnorderedAccessView> depth_share_uav;
    ComPtr<ID3D11UnorderedAccessView> motion_cvt_share_uav;
    ComPtr<ID3D11UnorderedAccessView> reactive_share_uav;
    ComPtr<ID3D11ShaderResourceView> depth_src_srv;  // 游戏深度 SRV（按纹理缓存）
    ID3D11Texture2D *depth_src_last = nullptr;
    ComPtr<ID3D11ShaderResourceView> motion_src_srv; // 游戏 motion SRV（按纹理缓存）
    ID3D11Texture2D *motion_src_last = nullptr;
    // 两个 shader 的 b0 值独立；共用同一物理 buffer 会在参数不等时读到另一 pass 的值。
    ComPtr<ID3D11Buffer> depth_cb, motion_cb;
    float cb_uploaded_depth = std::numeric_limits<float>::quiet_NaN();
    float cb_uploaded_flip = std::numeric_limits<float>::quiet_NaN();
    // ---- 输出落地格式（UAV 不能用 TYPELESS；按本实例的输出纹理定）----
    DXGI_FORMAT output_enc_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    // ---- 本实例最近一次输入格式（供诊断如实上报，避免全局镜像互相覆盖）----
    DXGI_FORMAT in_color_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT in_depth_fmt = DXGI_FORMAT_R32_FLOAT;
    DXGI_FORMAT in_motion_fmt = DXGI_FORMAT_R16G16_FLOAT;
    DXGI_FORMAT in_transparency_fmt = DXGI_FORMAT_R8_UNORM;
    DXGI_FORMAT in_output_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    // ---- ★ P2a：调用方 context（每个实例记住**自己的**那一个）----
    // ctx_src 持引用：既保证"同一对象"判等可靠（否则对象释放后地址可能被复用，会把
    // 新 context 误判成旧的、继续拿已失效的 ctx4 去 Signal），也让 ctx4 永远来自同一对象。
    ComPtr<ID3D11DeviceContext> ctx_src;
    ComPtr<ID3D11DeviceContext4> game_ctx4;
    // ---- ★ P2：该实例的调用方 context 属于**另一个 D3D11 设备**（已知不可用）----
    // 本后端的共享纹理都建在 g_d11dev（首次 init 的那个设备）上；跨设备时
    // CreateShaderResourceView / CopyResource 会静默失败（D3D11 不报错），结果是垃圾
    // 或旧内容 —— 这是"另一路画面混进来"的另一种成因。这种实例一律 fail-open 放行游戏
    // 原生上采样（= P1 的安全回退语义），并只留一次明确日志（避免每帧刷屏）。
    bool ctx_wrong_device = false;
};

// 每实例独立 FSR context（全部实例接管，防历史串流污染）
struct SdkContext
{
    ffxContext ctx = nullptr;
    bool created = false;
    std::uint64_t key = 0;
    UINT64 last_submission = 0;
    bool reuse_needs_wait = false;
    bool first = true;
    std::uint64_t last_use = 0;
    InstanceRes res; // P2：该实例独占的 GPU 互操作资源（尺寸池判等键在 res.rw/rh/dw/dh）
};
std::vector<SdkContext> g_sdk_ctxs;

// 前向声明：这两个 helper 要用到下方才声明的全局（g_d11dev / g_shared_fence11 / 诊断镜像），
// 但实例槽位的 LRU 驱逐路径（ctx_for_key）必须先落地挂起帧、再整套释放该实例的资源。
void release_instance_res(SdkContext &sc);
bool retire_instance(SdkContext &sc);
void destroy_context(SdkContext &sc);
std::string g_version_name = "ffx12";
// 实际匹配到的 provider 版本名（如 "4.1.1"/"4.0.2c"/"3.1.5"）——日志如实上报。
std::string g_sdk_matched_name = "?";
// 版本判定简化为大版本前缀（"2."/"3."/"4."），SDK 更新（4.1.1→4.2.x 等）免适配。
std::string g_sdk_version_prefix = "2.";
std::uint64_t g_d11_luid = 0;  // D3D11 适配器 LUID（诊断）
std::uint64_t g_d12_luid = 0;  // D3D12 适配器 LUID（诊断）
float g_velocity_factor = 1.0f; // FSR2 高亮像素时间稳定性（0 改善高亮边缘闪烁）

// ---- AMD SDK 消息回调（fpMessage）----
std::wstring g_sdk_messages;
std::mutex g_sdk_msg_mutex;

// ---- 管线诊断：中间纹理中央像素（readback 采集）----
// 正式版（RELEASE_RUNTIME）不编译诊断链：canary/chain readback/debug pixels/
// dump frames/output mark/decode test/debug layer 全部移除。
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
struct DebugPixel
{
    float v[4] = {0, 0, 0, 0};
};
DebugPixel g_debug_pixels[4]; // 0=PQ解码(color_linear) 1=ffxDispatch(output_linear) 2=PQ编码(输出共享) 3=motion解码(motion_cvt)
std::mutex g_debug_px_mutex;
bool g_debug_layer = false;           // D3D12 debug layer + info queue
std::uint32_t g_dump_frames = 0;      // 2026-08-25：dump 前 N 帧 FSR4 输出（诊断边缘抖动）
ComPtr<ID3D12InfoQueue> g_info_queue;
#endif
void sdk_message_cb(std::uint32_t type, const wchar_t *message)
{
    if (message == nullptr)
        return;
    try
    {
        std::lock_guard lock(g_sdk_msg_mutex);
        if (g_sdk_messages.size() < 8192)
        {
            if (!g_sdk_messages.empty())
                g_sdk_messages += L" | ";
            g_sdk_messages += (type == FFX_API_MESSAGE_TYPE_ERROR ? L"[ERR] " :
                               type == FFX_API_MESSAGE_TYPE_WARNING ? L"[WARN] " : L"[INFO] ");
            g_sdk_messages += message;
        }
    }
    catch (...)
    {
    }
}

// P2：原先的 g_tex_color/g_tex_motion/g_tex_output 全局单份已搬进 SdkContext::res。
// 下面两个是**诊断镜像**（debug_output_texture / debug_color_texture 用），指向"最近一次
// 派发"那个实例的纹理；持引用，避免实例被 LRU 驱逐后留下悬空指针。
ComPtr<ID3D11Texture2D> g_mirror_color_tex;
ComPtr<ID3D11Texture2D> g_mirror_output_tex;
// 保存最近一次的格式给诊断；尺寸以各实例的资源池为准。
bool g_last_reset = false;
std::uint64_t g_ctx_recreates = 0;
constexpr int kFfxDispatchNotReached = -2147483647;
std::atomic_int g_last_ffx_dispatch_rc { kFfxDispatchNotReached };
DXGI_FORMAT g_input_color_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
DXGI_FORMAT g_input_depth_fmt = DXGI_FORMAT_R32_FLOAT;
DXGI_FORMAT g_input_motion_fmt = DXGI_FORMAT_R16G16_FLOAT;
DXGI_FORMAT g_input_transparency_fmt = DXGI_FORMAT_R8_UNORM;
DXGI_FORMAT g_input_output_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;

// ---- ：原生 D3D11 纯 GPU 传输（NT 共享句柄 + D3D11.4 共享 fence）----
// 设计依据（推翻 2026-08-24"legacy 共享纹理在 D3D12 读为 0"结论）：
//   D3D11_RESOURCE_MISC_SHARED（legacy 共享面）句柄 D3D12 无法正确打开 → 读零；
//   正确机制 = D3D11_RESOURCE_MISC_SHARED_NTHANDLE + IDXGIResource1::CreateSharedHandle
//   + ID3D12Device::OpenSharedHandle + D3D11.4 ID3D11Fence 共享同步。
//   GpuInteropProbe 在本机（RX 9070 XT，LUID 0x123A3，游戏同适配器）已字节级验证：
//   R8G8B8A8/R10G10B10A2_TYPELESS/R16G16_FLOAT/R32_FLOAT 双向 GPU 流转全 PASS；
//   D3D12→D3D11 直接导入不支持（生产不需要）。OptiScaler/D3DSharingTests 佐证
//   D3D11→D3D12 方向为跨 API 共享的普遍可行方向。
bool g_gpu_interop = false;        // 配置请求
bool g_gpu_interop_ready = false;  // 初始化成功（D3D11.4 + 共享 fence + CS 编译）
// 深度提取：游戏 R32G8X24_TYPELESS 不可共享 → D3D11 CS 提取到共享 R32_FLOAT（FFX 期望格式）
// P2：深度/运动/reactive 三张共享纹理、其 UAV/SRV、b0 常数缓冲均已搬进 InstanceRes。
ComPtr<ID3D11ComputeShader> g_depth_extract_cs;
// D3D11 侧 motion 解码（金丝雀证实原 D3D12 decode pass 从不写 cvt）
ComPtr<ID3D11ComputeShader> g_motion_decode_cs;   // D3D11 decode compute
ComPtr<ID3D11ComputeShader> g_motion_decode_dz_cs; // 静止死区变体
ComPtr<ID3D11ComputeShader> g_motion_raw_cs;       // 不解码变体（Ffx12MotionDecode=0 时选用）
bool g_motion_deadzone = false;                  // =true 用死区变体（微小 motion 归零）
// P2 诊断（**默认关**，由桥的 Ffx12SingleInstanceDiag 打开）：池重建 / context 重建留痕。
// 上限 32 条：g_sdk_messages 是 8192 字符的累积缓冲，不设上限会把其它消息挤掉。
bool g_instance_diag = false;
std::atomic_uint32_t g_instance_diag_notes { 0 };
bool instance_diag_note_allowed()
{
    return g_instance_diag &&
        g_instance_diag_notes.fetch_add(1, std::memory_order_relaxed) < 32;
}

// 运行期配置（桥 load_config 时设置；启动后固定）
bool g_depth_inverted = true;   // 游戏深度逆方向（0=far）—— 2026-08-23 采样验证
bool g_decode_motion = true;    // 游戏 motion 为 R10G10B10A2 平方编码
std::uint32_t g_create_flags = 0; // 实际创建 flags（日志按真值输出，勿硬编码）
float g_motion_flip = 1.0f;     // XeSS/DLSS 定向——motion 方向翻转（默认 +1 = FSR 方向）
float g_depth_scale = 1.0f;     // XeSS/DLSS 定向——depth 值域归一化（XeSS 期望 [0,1]）
// b0 常数缓冲（g_motion_cb）与其"已上传值"缓存已搬进 InstanceRes（P2：按实例隔离）。
bool g_motion_vectors_jittered = false; // 游戏配置：motion 是否已包含投影 jitter
bool g_hdr_input = true;        // 游戏 10-bit HDR 管线（useRealType）
bool g_auto_exposure = true;    // 自动曝光（OptiScaler 日志 initFlags 实证：AutoExposure=true）
bool g_non_linear = true;       // 非线性色彩空间（OptiScaler 日志：FsrNonLinearColorSpace=true）
bool g_use_pq_chain = false;    // PQ 链开关（2026-08-24 定案：游戏原生直喂 PQ 值 + HDR|NON_LINEAR|AUTO_EXPOSURE
                                // 标志（initFlags 0x129），不做 PqToLinear/LinearToPq；本开关保留旧链作对照）
// 后端选择（SDS §3.9A.3(b)）：由桥的 ini 读取点（Ffx12Backend 键）经 set_backend() 设置。
BackendKind g_backend = BackendKind::Native;   // 初值 Native = 改造前行为（REQ-04）


// 读取 D3D12 info queue 消息（诊断；正式版不编译）
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
static void drain_info_queue();

// 读取 D3D12 info queue 消息并追加到 SDK 消息缓冲（诊断）
static void drain_info_queue()
{
    if (!g_info_queue)
        return;
    try
    {
        const UINT64 n = g_info_queue->GetNumStoredMessages();
        if (n == 0)
            return;
        for (UINT64 i = 0; i < n && i < 64; ++i) // P0-3 可观测性：原 8 条截断，扩到 64
        {
            SIZE_T len = 0;
            g_info_queue->GetMessage(static_cast<UINT>(i), nullptr, &len);
            if (len == 0)
                continue;
            std::vector<std::uint8_t> buf(len);
            D3D12_MESSAGE *msg = reinterpret_cast<D3D12_MESSAGE *>(buf.data());
            if (SUCCEEDED(g_info_queue->GetMessage(static_cast<UINT>(i), msg, &len)) && msg->pDescription)
            {
                std::wstring wmsg;
                const int wlen = MultiByteToWideChar(CP_UTF8, 0, msg->pDescription, -1, nullptr, 0);
                if (wlen > 1)
                {
                    wmsg.resize(static_cast<std::size_t>(wlen) - 1);
                    MultiByteToWideChar(CP_UTF8, 0, msg->pDescription, -1, wmsg.data(), wlen);
                }
                else
                {
                    wmsg = L"<d3d12 msg>";
                }
                std::lock_guard lock(g_sdk_msg_mutex);
                if (g_sdk_messages.size() < 8192)
                {
                    if (!g_sdk_messages.empty())
                        g_sdk_messages += L" | ";
                    g_sdk_messages += L"[D3D12] ";
                    g_sdk_messages += wmsg;
                }
            }
        }
        g_info_queue->ClearStoredMessages();
    }
    catch (...)
    {
    }
}
#endif

#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
// 旧 CPU/PQ 样本从未由 GPU 互操作路径填充，诊断接口保留无样本语义。
bool g_motion_decode_test = false;
bool g_output_mark = false;
bool g_decode_test = false;
#endif

// SDK 诊断注记（追加到 g_sdk_messages，随 sdk_msgs 上日志）
static void sdk_note(const wchar_t *fmt, ...)
{
    wchar_t buf[512] {};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    std::lock_guard lock(g_sdk_msg_mutex);
    if (g_sdk_messages.size() < 8192)
    {
        if (!g_sdk_messages.empty())
            g_sdk_messages += L" | ";
        g_sdk_messages += L"[sdk234] ";
        g_sdk_messages += buf;
    }
}

// ---- 崩溃安全步进日志（fopen/fprintf/fclose 同步写；进程被看门狗/设备移除杀死也不丢）----

// 本 DLL 所在目录（含尾部反斜杠）。
//
// ⚠️ 2026-09-19：步进日志此前用**相对路径** `"sdk234_steps.log"` ——
// 落盘位置取决于进程当前工作目录（游戏启动方式不同则位置不同，甚至可能不可写），
// 排查时找不到文件。改为与本 DLL 同目录的绝对路径（与主日志 `Dx11FsrBridge.log`
// 的约定一致）。
//
// 注意必须用 GetModuleHandleExW(FROM_ADDRESS) 取**本 DLL** 的句柄：
// GetModuleFileNameW(nullptr, ...) 返回的是**宿主 exe** 路径，不是本模块。
static const std::wstring &sdk234_log_dir()
{
    static const std::wstring dir = []
    {
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&sdk234_log_dir), &self) == FALSE ||
            self == nullptr)
        {
            return std::wstring {};
        }
        wchar_t path[MAX_PATH] {};
        const DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return std::wstring {};
        const std::wstring full(path, path + length);
        const std::size_t slash = full.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring {} : full.substr(0, slash + 1);
    }();
    return dir;
}

static void sdk234_step(const char *step)
{
#if defined(FFX12_DEBUG_STEPS)
    const std::wstring &dir = sdk234_log_dir();
    if (dir.empty())
        return; // 取不到模块目录则不写（避免又落到不可预测的位置）
    const std::wstring full = dir + L"sdk234_steps.log";
    FILE *f = nullptr;
    if (_wfopen_s(&f, full.c_str(), L"a") == 0 && f)
    {
        SYSTEMTIME st {};
        GetLocalTime(&st);
        fprintf(f, "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, step);
        fclose(f);
    }
#else
    (void)step;
#endif
}

// 步进日志附参（memcpy 边界等）
static void sdk234_info(const char *fmt, ...)
{
#if defined(FFX12_DEBUG_STEPS)
    const std::wstring &dir = sdk234_log_dir();
    if (dir.empty())
        return;
    const std::wstring full = dir + L"sdk234_steps.log";
    FILE *f = nullptr;
    if (_wfopen_s(&f, full.c_str(), L"a") == 0 && f)
    {
        SYSTEMTIME st {};
        GetLocalTime(&st);
        fprintf(f, "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fclose(f);
    }
#else
    (void)fmt;
#endif
}

#if defined(FFX12_DEBUG_STEPS)
static LONG WINAPI sdk234_exception_filter(EXCEPTION_POINTERS *ep)
{
    // 与 sdk234_step/sdk234_info 统一用模块目录绝对路径。
    // 注意：异常处理器内不做可能抛异常/分配的操作 —— sdk234_log_dir() 返回的是
    // 已初始化的静态 wstring（首次调用在正常路径完成），此处仅做拼接与 fopen。
    // 该处理器只在 FFX12_DEBUG_STEPS 下被安装（见 SetUnhandledExceptionFilter 调用点）。
    const std::wstring &dir = sdk234_log_dir();
    if (dir.empty())
        return EXCEPTION_CONTINUE_SEARCH;
    const std::wstring full = dir + L"sdk234_steps.log";
    FILE *f = nullptr;
    if (_wfopen_s(&f, full.c_str(), L"a") == 0 && f)
    {
        const std::uint8_t *addr = static_cast<const std::uint8_t *>(ep->ExceptionRecord->ExceptionAddress);
        const std::uint8_t *base =
            reinterpret_cast<const std::uint8_t *>(GetModuleHandleW(L"Dx11FsrBridge.dll"));
        fprintf(f, "EXCEPTION code=0x%08X addr=%p bridge_offset=0x%llX",
                static_cast<unsigned>(ep->ExceptionRecord->ExceptionCode),
                ep->ExceptionRecord->ExceptionAddress,
                base ? static_cast<unsigned long long>(addr - base) : 0ull);
        if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 &&
            ep->ExceptionRecord->NumberParameters >= 2)
        {
            fprintf(f, " access=%s fault_va=0x%llX",
                    ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE" : "READ",
                    static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
        }
        fprintf(f, "\n");
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// NT 共享纹理（SHARED|SHARED_NTHANDLE）——D3D12 可正确打开的跨 API
// 共享机制（legacy GetSharedHandle 在 D3D12 读零，见 GpuInteropProbe 实证）。
ComPtr<ID3D11Texture2D> make_shared_texture_nt(UINT w, UINT h, DXGI_FORMAT fmt, UINT bind_flags)
{
    D3D11_TEXTURE2D_DESC d {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind_flags;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(g_d11dev->CreateTexture2D(&d, nullptr, &t)))
        return nullptr;
    return t;
}

// 用 IDXGIResource1::CreateSharedHandle 打开为 D3D12（NT 共享，正确机制）。
bool open_shared_nt(SharedTex &st)
{
    if (st.d12)
        return true;
    if (!st.d11 || !g_d12dev)
        return false;
    ComPtr<IDXGIResource1> r1;
    if (FAILED(st.d11.As(&r1)))
        return false;
    HANDLE h = nullptr;
    if (FAILED(r1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)))
        return false;
    const HRESULT hr = g_d12dev->OpenSharedHandle(h, IID_PPV_ARGS(&st.d12));
    CloseHandle(h);
    return SUCCEEDED(hr) && st.d12;
}

// P2：尺寸池**按实例**判等与重建 —— 甲实例的尺寸变化不再重建乙实例的纹理。
// （顺带消掉原先"两实例尺寸不同 ⇒ 每次 dispatch 都重建整套共享纹理"的反复分配。）
bool simple_copy_shape(const D3D11_TEXTURE2D_DESC &desc, UINT width, UINT height)
{
    return desc.Width == width && desc.Height == height && desc.MipLevels == 1 && desc.ArraySize == 1 &&
        desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0;
}

DXGI_FORMAT typed_color(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return format;
    }
}

bool ensure_pool(const FrameInput &input, SdkContext &sc)
{
    const InstanceRes &old = sc.res;
    D3D11_TEXTURE2D_DESC color {}, output {};
    input.color->GetDesc(&color);
    input.output_target->GetDesc(&output);
    if (!simple_copy_shape(color, input.render_w, input.render_h) ||
        !simple_copy_shape(output, input.display_w, input.display_h) || output.Usage == D3D11_USAGE_IMMUTABLE ||
        g_use_pq_chain)
        return false;
    if (old.rw == input.render_w && old.rh == input.render_h &&
        old.dw == input.display_w && old.dh == input.display_h &&
        old.in_color_fmt == color.Format && old.in_output_fmt == output.Format)
        return true;

    // 全部对象创建成功才替换池；中途失败不会让尺寸相同的半成品被当作已初始化。
    InstanceRes prepared;
    prepared.in_color_fmt = color.Format;
    prepared.in_output_fmt = output.Format;
    prepared.output_enc_format = typed_color(output.Format);
    prepared.rw = input.render_w;
    prepared.rh = input.render_h;
    prepared.dw = input.display_w;
    prepared.dh = input.display_h;
    constexpr UINT k_shader = D3D11_BIND_SHADER_RESOURCE;
    constexpr UINT k_uav = k_shader | D3D11_BIND_UNORDERED_ACCESS;
    const auto make = [](SharedTex &tex, UINT w, UINT h, DXGI_FORMAT format, UINT bind)
    {
        tex.d11 = make_shared_texture_nt(w, h, format, bind);
        return open_shared_nt(tex);
    };
    const auto view = [](SharedTex &tex, DXGI_FORMAT format, auto &uav)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC desc {};
        desc.Format = format;
        desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        return SUCCEEDED(g_d11dev->CreateUnorderedAccessView(tex.d11.Get(), &desc, uav.GetAddressOf()));
    };
    const auto constant_buffer = [](auto &target)
    {
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = 16;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        return SUCCEEDED(g_d11dev->CreateBuffer(&desc, nullptr, target.GetAddressOf()));
    };
    if (!make(prepared.tex_color, prepared.rw, prepared.rh, color.Format, k_shader) ||
        !make(prepared.tex_output, prepared.dw, prepared.dh, output.Format, k_uav) ||
        !make(prepared.tex_depth_share, prepared.rw, prepared.rh, DXGI_FORMAT_R32_FLOAT, k_uav) ||
        !make(prepared.tex_motion_cvt_share, prepared.rw, prepared.rh, DXGI_FORMAT_R16G16_FLOAT, k_uav) ||
        !make(prepared.tex_reactive_share, prepared.rw, prepared.rh, DXGI_FORMAT_R8_UNORM, k_uav) ||
        !view(prepared.tex_depth_share, DXGI_FORMAT_R32_FLOAT, prepared.depth_share_uav) ||
        !view(prepared.tex_motion_cvt_share, DXGI_FORMAT_R16G16_FLOAT, prepared.motion_cvt_share_uav) ||
        !view(prepared.tex_reactive_share, DXGI_FORMAT_R8_UNORM, prepared.reactive_share_uav) ||
        !constant_buffer(prepared.depth_cb) || !constant_buffer(prepared.motion_cb) || !retire_instance(sc))
        return false;

    if (instance_diag_note_allowed())
        sdk_note(L"inst pool rebuild inst=%llu old=%ux%u->%ux%u new=%ux%u->%ux%u",
            static_cast<unsigned long long>(sc.key), old.rw, old.rh, old.dw, old.dh,
            prepared.rw, prepared.rh, prepared.dw, prepared.dh);
    if (sc.created)
    {
        destroy_context(sc);
        ++g_ctx_recreates;
    }
    release_instance_res(sc);
    sc.res = std::move(prepared);
    sc.first = true;
    return true;
}

bool retire_instance(SdkContext &sc)
{
    if (!g_interop.retire(sc.last_submission)) return false;
    sc.last_submission = 0;
    sc.reuse_needs_wait = false;
    return true;
}

void destroy_context(SdkContext &sc)
{
    if (sc.created && g_runtime.destroy != nullptr)
    {
        try
        {
            g_runtime.destroy(&sc.ctx, nullptr);
        }
        catch (...)
        {
            sdk_note(L"ffxDestroyContext threw C++ exception stage=ctx");
        }
    }
    sc.ctx = nullptr;
    sc.created = false;
}

// 2026-11-15：淘汰只在该实例最后一次 GPU work 退役之后发生；尺寸变化也走相同顺序。
SdkContext *ctx_for_key(std::uint64_t key)
{
    SdkContext *victim = nullptr;
    for (SdkContext &sc : g_sdk_ctxs)
    {
        if (sc.key == key)
        {
            sc.last_use = GetTickCount64();
            return &sc;
        }
        if (!victim || sc.last_use < victim->last_use) victim = &sc;
    }
    if (g_sdk_ctxs.size() < 4)
    {
        g_sdk_ctxs.emplace_back();
        victim = &g_sdk_ctxs.back();
    }
    if (!retire_instance(*victim)) return nullptr;
    if (victim->created)
    {
        destroy_context(*victim);
        ++g_ctx_recreates;
    }
    release_instance_res(*victim);
    victim->key = key;
    victim->last_use = GetTickCount64();
    victim->first = true;
    return victim;
}

bool create_context(SdkContext &sc)
{
    if (sc.created)
        return true;
    // P2：maxRenderSize/maxUpscaleSize 取自**本实例**的池（原先取全局镜像，多实例下会
    // 拿到"上一个派发实例"的尺寸 ⇒ 建出尺寸不符的 context）。
    if (sc.res.rw == 0 || sc.res.rh == 0 || sc.res.dw == 0 || sc.res.dh == 0)
        return false;

    if (!g_runtime.create || g_sdk_version_id == 0)
        return false;

    ffxCreateBackendDX12Desc backend {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = g_d12dev.Get();

    ffxOverrideVersion override_version {};
    override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override_version.versionId = g_sdk_version_id;
    backend.header.pNext = &override_version.header;

    ffxCreateContextDescUpscale desc {};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backend.header;
    desc.maxRenderSize = {sc.res.rw, sc.res.rh};
    desc.maxUpscaleSize = {sc.res.dw, sc.res.dh};
    desc.fpMessage = sdk_message_cb;
    // 游戏深度为逆深度（0=far，透视编码）：必须翻转 FSR2 的深度假设，否则
    // disocclusion/depth-clip 全反 → 历史每帧被拒 → 输出退化为单帧放大（无 AA + 抖动）。
    // 色彩/曝光：参照 OptiScaler 实测 initFlags 0x129（HDR|DEPTH_INVERTED|AUTO_EXPOSURE|NON_LINEAR_COLORSPACE）。
    desc.flags = (g_depth_inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0u) |
                 (g_hdr_input ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0u) |
                 (g_auto_exposure ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0u) |
                 (g_non_linear ? FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE : 0u) |
                 // 仅当输入 motion 已包含投影 jitter 时启用抵消；否则该标志会对
                 // 未带 jitter 的矢量重复施加补偿，表现为静止画面细微抖动。
                 (g_motion_vectors_jittered ? FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION : 0u);
    g_create_flags = desc.flags; // 供日志按真值输出（旧日志此处是硬编码字符串）
    // 外部运行时失败不能穿过 Draw hook；本 TU 在 Release 也启用正常的 C++ 展开。
    ffxReturnCode_t rc = FFX_API_RETURN_ERROR;
    try
    {
        rc = g_runtime.create(
            &sc.ctx, reinterpret_cast<ffxCreateContextDescHeader *>(&desc), nullptr);
    }
    catch (const std::exception &e)
    {
        sdk_note(L"ffxFsr2ContextCreate threw std::exception stage=ctx");
        sc.ctx = nullptr;
        return false;
    }
    catch (...)
    {
        sdk_note(L"ffxFsr2ContextCreate threw unknown exception stage=ctx");
        sc.ctx = nullptr;
        return false;
    }
    if (rc != FFX_API_RETURN_OK || sc.ctx == nullptr)
    {
        sdk_note(L"ffxFsr2ContextCreate rc=%d version=%s stage=ctx",
                 static_cast<int>(rc), g_version_name.c_str());
        return false;
    }
    sc.created = true;
    if (g_runtime.configure != nullptr)
    {
        float velocity = g_velocity_factor;
        ffxConfigureDescUpscaleKeyValue config {};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
        config.key = FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR;
        config.ptr = &velocity;
        try
        {
            g_runtime.configure(&sc.ctx, reinterpret_cast<ffxConfigureDescHeader *>(&config));
        }
        catch (...)
        {
            // 速度因子是可选项；不能将 configure 成功写进日志，也不能继续使用不明状态的 context。
            sdk_note(L"ffxConfigure threw C++ exception stage=ctx");
            destroy_context(sc);
            return false;
        }
    }
    return true;
}



// 原生 D3D11 纯 GPU 传输（游戏设备保持原生 D3D11，不劫持为 On12）。
// 与 dispatch_on12_direct 等价的全 GPU 链路，但资源经 NT 共享句柄（D3D11 建 /
// D3D12 打开）跨设备流转，同步用 D3D11.4 共享 fence：
//   ① D3D11（游戏 stream）：深度 CS 提取 + CopyResource 输入 → Signal(f11, v1)
//   ② D3D12 队列 Wait(f12, v1) → motion 解码 + ffxDispatch（共享纹理 SRV/UAV）→
//     Signal(f12, v2)
//   ③ D3D11 Wait(f11, v2) → CopyResource 共享输出 → 游戏输出
//
// 【P2：为什么 fence 仍可全局单条】
//   fence 在这里不是"某个实例的私有信号量"，而是一条**全局单调时间线**（每次 Signal
//   严格 +1，由 g_mutex 完全串行地推进）。多实例交替使用同一 fence 时：
//     甲 Signal(1) → D3D12 Wait(1) → D3D12 Signal(2) → 乙 ctx4 Wait(2) → 乙 Signal(3)
//     → D3D12 Wait(3) → D3D12 Signal(4) → …
//   每个 Wait(v) 等的是"时间线推进到 v"，而 v 一定由**正确的前驱**在本次等待之前
//   （或之后、但在同一串行序里）Signal —— 值交错不破坏语义，因为等待的是"达到过 v"，
//   而不是"谁到达了 v"。两个必要条件都成立：① Signal 严格递增（单一 +1 点，持锁）；
//   ② 每个 Wait 的目标值都由本串行序中的生产者登记过。故不需要 per-instance fence；
//   一旦去掉 g_mutex 串行化（真并发提交），这两条同时失效 —— 那时才必须每实例一条。
// 全程无 CPU staging / Map / UpdateSubresource。单变量原则：FFX 参数与 On12 路径一致。
bool dispatch_gpu_shared(const FrameInput &input, ID3D11DeviceContext *game_context,
                         SdkContext &sc, bool reset)
{
    InstanceRes &r = sc.res;
    if (!g_interop.available() || !game_context || game_context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return false;
    // ★ P2a：**每个实例记住自己的调用方 context**（原先是全局唯一、只 QI 第一次见到的那个）。
    // 为什么必须按调用方：下面所有 D3D11 命令（深度提取 / motion 解码 CS、颜色 CopyResource）
    // 都发在 game_context 上，而步骤①末尾的 Signal 必须落在**同一个 context** 上。
    // 全局缓存会把"第二个实例的 D3D11 命令流"与"第一个实例的 Signal"配错对：Signal 可能
    // 在输入拷贝还没提交时就完成 ⇒ D3D12 读到上一实例留在共享纹理里的旧帧（正是那个
    // "跟随角色剪影的半透明覆盖层"）。这里改为按实例缓存，并在每次 dispatch 校验来源。
    if (r.ctx_wrong_device && r.ctx_src.Get() == game_context)
        return false; // 已知跨设备：放行游戏原生（不重复留痕）
    if (r.ctx_src.Get() != game_context || !r.game_ctx4)
    {
        r.game_ctx4.Reset();
        r.ctx_src = game_context; // 保留原接口身份；避免把同地址的新包装器误认成旧对象。
        sc.reuse_needs_wait = sc.last_submission != 0;
        r.ctx_wrong_device = false;
        // 跨设备保护（见 InstanceRes::ctx_wrong_device）：本后端的共享纹理都建在
        // g_d11dev 上，另一个设备的 context 用不了它们（D3D11 静默失败 ⇒ 旧内容/垃圾）。
        ID3D11Device *bind_dev = nullptr;
        game_context->GetDevice(&bind_dev);
        const bool same_device = bind_dev != nullptr && bind_dev == g_d11dev.Get();
        const std::uintptr_t bind_dev_bits = reinterpret_cast<std::uintptr_t>(bind_dev);
        if (bind_dev)
            bind_dev->Release();
        // 低频留痕（每实例一次 + 换 context 时）：实机用它确认"游戏到底有几个
        // immediate context / 几个设备" ⇒ 判定最小方案 P2a 是否已经足够修 M4。
        sdk_note(L"inst ctx4 bound inst=%llu ctx=%llu dev=%llu same_dev=%d",
                 static_cast<unsigned long long>(sc.key),
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(game_context)),
                 static_cast<unsigned long long>(bind_dev_bits),
                 same_device ? 1 : 0);
        if (!same_device)
        {
            r.ctx_wrong_device = true;
            sdk_note(L"inst ctx4 wrong device -> passthrough inst=%llu",
                     static_cast<unsigned long long>(sc.key));
            return false;
        }
        if (FAILED(game_context->QueryInterface(IID_PPV_ARGS(&r.game_ctx4))) || !r.game_ctx4)
        {
            sdk_note(L"gpu interop ctx4 unavailable stage=direct");
            return false;
        }
    }
    // 诊断镜像：debug_output_texture/debug_color_texture 取"最近一次派发"的纹理
    // （原先是全局单份池）。持引用，实例被 LRU 驱逐后不会留下悬空指针。
    g_mirror_color_tex = r.tex_color.d11;
    g_mirror_output_tex = r.tex_output.d11;
    // 诊断只看当前实例，不能读到另一个相机的尺寸和格式。
    g_input_color_fmt = r.in_color_fmt;
    g_input_output_fmt = r.in_output_fmt;
    // 常态同一即时 context 的上一次 Wait/Copy 已排队，无需每帧再等。只有上一轮未能交付，
    // 或接口发生变化时，才必须补依赖以保护共享输入/输出不被提前复用。
    if (sc.reuse_needs_wait && !g_interop.order_before_use(*r.game_ctx4.Get(), sc.last_submission))
        return false;
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    perf_probe::Scope prep(perf_probe::Segment::interop_prep);
#endif
    // 两份 guide 都必须能绑定；不以池里上一帧恰好还在的深度或 MV 代替。
    const auto bind_guide = [&](ID3D11Texture2D *texture, ID3D11Texture2D *&last, auto &view,
                                DXGI_FORMAT &actual, bool depth)
    {
        if (texture == last && view) return true;
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        if (desc.Width < r.rw || desc.Height < r.rh || desc.SampleDesc.Count != 1 || desc.ArraySize != 1)
            return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        srv.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        if (depth)
        {
            switch (desc.Format)
            {
            case DXGI_FORMAT_R24G8_TYPELESS:
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
            case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
                srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                break;
            case DXGI_FORMAT_R32G8X24_TYPELESS:
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
                srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
                break;
            default:
                srv.Format = DXGI_FORMAT_R32_FLOAT;
                break;
            }
        }
        ComPtr<ID3D11ShaderResourceView> prepared;
        if (FAILED(g_d11dev->CreateShaderResourceView(texture, &srv, prepared.GetAddressOf())))
            return false;
        view = std::move(prepared);
        last = texture;
        actual = desc.Format;
        return true;
    };
    if (!bind_guide(input.depth, r.depth_src_last, r.depth_src_srv, r.in_depth_fmt, true) ||
        !bind_guide(input.motion, r.motion_src_last, r.motion_src_srv, r.in_motion_fmt, false))
        return false;

    const auto prepare_guide = [&](ID3D11ComputeShader *shader, ID3D11ShaderResourceView *srv,
                                   UINT num_uav, ID3D11UnorderedAccessView *const *uavs,
                                   ID3D11Buffer *buffer, float value, float &uploaded)
    {
        if (uploaded != value)
        {
            const float constants[4] = {value, 0.0f, 0.0f, 0.0f};
            game_context->UpdateSubresource(buffer, 0, nullptr, constants, 0, 0);
            uploaded = value;
        }
        game_context->CSSetConstantBuffers(0, 1, &buffer);
        game_context->CSSetShader(shader, nullptr, 0);
        game_context->CSSetShaderResources(0, 1, &srv);
        game_context->CSSetUnorderedAccessViews(0, num_uav, uavs, nullptr);
        game_context->Dispatch((r.rw + 7u) / 8u, (r.rh + 7u) / 8u, 1u);
        ID3D11UnorderedAccessView *null_uavs[2] {};
        game_context->CSSetUnorderedAccessViews(0, num_uav, null_uavs, nullptr);
        srv = nullptr;
        game_context->CSSetShaderResources(0, 1, &srv);
    };
    ID3D11UnorderedAccessView *depth_uav = r.depth_share_uav.Get();
    prepare_guide(g_depth_extract_cs.Get(), r.depth_src_srv.Get(), 1, &depth_uav,
        r.depth_cb.Get(), g_depth_scale, r.cb_uploaded_depth);
    ID3D11ComputeShader *motion_shader = g_motion_decode_cs.Get();
    if (!g_decode_motion && g_motion_raw_cs) motion_shader = g_motion_raw_cs.Get();
    else if (g_motion_deadzone && g_motion_decode_dz_cs) motion_shader = g_motion_decode_dz_cs.Get();
    ID3D11UnorderedAccessView *motion_uavs[] = {r.motion_cvt_share_uav.Get(), r.reactive_share_uav.Get()};
    prepare_guide(motion_shader, r.motion_src_srv.Get(), 2, motion_uavs,
        r.motion_cb.Get(), g_motion_flip, r.cb_uploaded_flip);
    game_context->CSSetShader(nullptr, nullptr, 0);
    game_context->CopyResource(r.tex_color.d11.Get(), input.color);
    g_input_depth_fmt = r.in_depth_fmt;
    g_input_motion_fmt = r.in_motion_fmt;
    if (input.transparency)
    {
        D3D11_TEXTURE2D_DESC details {};
        input.transparency->GetDesc(&details);
        r.in_transparency_fmt = details.Format;
    }
    g_input_transparency_fmt = r.in_transparency_fmt;
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    prep.finish();
    perf_probe::Scope submit(perf_probe::Segment::interop_submit);
#endif
    ID3D12GraphicsCommandList *const cmd = g_interop.begin();
    if (!cmd) return false;
    const auto barrier = [&](ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                             D3D12_RESOURCE_STATES after)
    {
        if (resource == nullptr || before == after)
            return;
        D3D12_RESOURCE_BARRIER item {};
        item.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        item.Transition.pResource = resource;
        item.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        item.Transition.StateBefore = before;
        item.Transition.StateAfter = after;
        cmd->ResourceBarrier(1, &item);
    };

    // 共享输入统一先到 NON_PIXEL_SHADER_RESOURCE（FFX 将从这里再转自身需要的状态）
    // motion 输入 = 共享解码 R16G16（D3D11 CS 已解码）；raw motion 不再进 D3D12
    ID3D12Resource *inputs[4] = {};
    inputs[0] = r.tex_color.d12.Get();
    inputs[1] = r.tex_depth_share.d12.Get();
    inputs[2] = r.tex_motion_cvt_share.d12.Get();
    if (input.use_reactive_mask && r.tex_reactive_share.d12)
        inputs[3] = r.tex_reactive_share.d12.Get();
    for (ID3D12Resource *resource : inputs)
        barrier(resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // FFX：color/depth/motion 全为共享纹理（SRV），输出直写共享输出（UAV）
    barrier(r.tex_output.d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ffxDispatchDescUpscale dispatch {};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = cmd;
    dispatch.color = ffxApiGetResourceDX12(r.tex_color.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.color.description.format = ffxApiGetSurfaceFormatDX12(r.in_color_fmt);
    dispatch.depth = ffxApiGetResourceDX12(r.tex_depth_share.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;
    dispatch.motionVectors =
        ffxApiGetResourceDX12(r.tex_motion_cvt_share.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors.description.format = FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    // reactive（motion B 通道提取，共享 R8）——历史"无改善"结论在 motion=0 坏基线上，重估
    if (input.use_reactive_mask && r.tex_reactive_share.d12)
    {
        dispatch.reactive =
            ffxApiGetResourceDX12(r.tex_reactive_share.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.reactive.description.format = FFX_API_SURFACE_FORMAT_R8_UNORM;
    }
    dispatch.output = ffxApiGetResourceDX12(r.tex_output.d12.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    // P2：输出格式取**本实例**池的落地格式（全局镜像会被另一实例的池构建覆盖）
    dispatch.output.description.format = ffxApiGetSurfaceFormatDX12(r.output_enc_format);
    dispatch.jitterOffset = {input.jitter_x, input.jitter_y};
    dispatch.motionVectorScale = {input.motion_scale_x, input.motion_scale_y};
    dispatch.renderSize = {input.render_w, input.render_h};
    dispatch.enableSharpening = input.enable_sharpening;
    dispatch.sharpness = input.sharpness;
    dispatch.frameTimeDelta = input.frame_time_delta_ms;
    dispatch.preExposure = input.pre_exposure;
    dispatch.reset = reset;
    dispatch.cameraNear = input.camera_near;
    dispatch.cameraFar = input.camera_far;
    dispatch.cameraFovAngleVertical = input.camera_fov_vertical;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    ffxReturnCode_t rc = FFX_API_RETURN_ERROR;
    try
    {
        rc = g_runtime.dispatch(&sc.ctx, reinterpret_cast<const ffxDispatchDescHeader *>(&dispatch));
    }
    catch (...)
    {
        sdk_note(L"ffxDispatch threw C++ exception stage=record");
    }
    g_last_ffx_dispatch_rc.store(static_cast<int>(rc), std::memory_order_relaxed);
    if (rc != FFX_API_RETURN_OK)
    {
        // 失败的 SDK 可能已录入一部分指令；不能执行或把池内旧输出复制到当前帧。
        g_interop.discard();
        sdk_note(L"gpu ffxDispatch rc=%d stage=record", static_cast<int>(rc));
        return false;
    }

    barrier(r.tex_output.d12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    for (ID3D12Resource *resource : inputs)
        barrier(resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    const auto submission = g_interop.submit(*r.game_ctx4.Get());
    if (submission.ticket)
    {
        sc.last_submission = submission.ticket;
        sc.reuse_needs_wait = true;
    }
    if (!submission.signal_ordered)
    {
        sdk_note(L"gpu FFX submission failed stage=submit");
        return false;
    }
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    submit.finish();
    perf_probe::Scope wait(perf_probe::Segment::finish_wait);
#endif
    if (!g_interop.order_before_use(*r.game_ctx4.Get(), submission.ticket))
    {
        sdk_note(L"gpu FFX output Wait failed stage=delivery");
        return false;
    }
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    wait.finish();
    perf_probe::Scope copy(perf_probe::Segment::finish_copy);
#endif
    // 2026-11-15：游戏常在 Present 之前消费目标。把依赖和复制放在当前 D3D11 流中，
    // 而不是发布一张还没被写的纹理。此处只建立 GPU 顺序，不宣称 CPU 已看到像素。
    game_context->CopyResource(input.output_target, r.tex_output.d11.Get());
    if (!g_async_upscale) game_context->Flush();
    sc.reuse_needs_wait = false;
    g_gpu_only_transport_available = true;
    const auto count = ++g_delivered_count;
    if (count <= 4 || count % 1024u == 0)
        sdk_note(L"gpu interop ffx12 output_ordered count=%llu no_cpu_bridge=1",
                 static_cast<unsigned long long>(count));
    return true;
}

// 调用方必须已确认最后一个 GPU ticket 退役，或设备正处于移除状态。
void release_instance_res(SdkContext &sc)
{
    InstanceRes &r = sc.res;
    // 诊断镜像若指向本实例的纹理，必须一起清掉（否则留下悬空指针）
    if (g_mirror_color_tex.Get() == r.tex_color.d11.Get())
        g_mirror_color_tex.Reset();
    if (g_mirror_output_tex.Get() == r.tex_output.d11.Get())
        g_mirror_output_tex.Reset();
    r = {}; // 仅在调用方确认 ticket 退役后销毁；新池不得继承旧视图与 CB。
}

} // namespace

// ★ 粘在匿名命名空间收口之后——此处【仍在 namespace ffx12 内】。★不要★再写 namespace ffx12 包裹：
//   定义会落进 ffx12::ffx12，编译期完全干净、链接期 LNK2019 x4（SDS §3.9A.3(e)）。
//   自验：grep -c "^namespace\|^} // namespace" Ffx12Backend.cpp 必须仍是 4。
// §3.9A.2 的两个 getter + §3.9A.3(c) 的两个访问器（SDS §3.9A.3(f) 逐字）：
bool hdr_input() { return g_hdr_input; }
bool motion_vectors_jittered() { return g_motion_vectors_jittered; }
bool depth_inverted() { return g_depth_inverted; }

void set_backend(BackendKind k) { g_backend = k; }
BackendKind backend() { return g_backend; }

// 进程退出标志的对外入口（g_process_exiting 在匿名命名空间内，此处仅转发）。
// 由桥的 DllMain(DLL_PROCESS_DETACH, lpReserved != nullptr) 调用。
void set_process_exiting()
{
    g_process_exiting.store(true, std::memory_order_release);
}

bool init_locked(ID3D11Device *game_device, const wchar_t *sdk_dll_path)
{
    if (g_active.load(std::memory_order_relaxed))
        return true;
    if (game_device == nullptr)
        return false;
    // 允许 shutdown 之后重新 init（测试/设备重建路径）：清掉停机标志。
    g_shutting_down.store(false, std::memory_order_release);
    g_d11dev = game_device;
    // 清理：On12 引导已移除（不发生产品路线）。自建 D3D12 设备 + 共享句柄互操作。
    g_gpu_only_transport_available = false; // enabled only after direct gpu-interop dispatch lands.
#if defined(FFX12_DEBUG_STEPS)
    SetUnhandledExceptionFilter(sdk234_exception_filter);
#endif
    sdk234_step("init begin");
#if defined(FFX12_DEBUG_STEPS)
    std::printf("[sdk234] init: device=%p dll=%ls\n", game_device, sdk_dll_path);
    std::fflush(stdout);
#endif

    // 同适配器建 D3D12 设备（共享句柄互操作必需同一 LUID）
    ComPtr<IDXGIDevice> dxgi_dev;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(g_d11dev.As(&dxgi_dev)) || FAILED(dxgi_dev->GetAdapter(&adapter)))
        return false;
#if defined(FFX12_DEBUG_STEPS)
    std::printf("[sdk234] init: adapter obtained\n");
    std::fflush(stdout);
#endif
    // D3D12 设备（debug layer 可配：先启用 debug interface；正式版不编译）
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
    if (g_debug_layer)
    {
        ComPtr<ID3D12Debug> debug;
        const HRESULT dbg_hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if (FAILED(dbg_hr) || !debug)
        {
            sdk_note(L"d3d12 debug layer unavailable hr=0x%08X", static_cast<unsigned>(dbg_hr));
        }
        else
        {
            debug->EnableDebugLayer();
        }
    }
#endif
    {
        const HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_d12dev));
        if (FAILED(hr) || !g_d12dev)
            return false;
#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
        if (g_debug_layer)
        {
            if (FAILED(g_d12dev.As(&g_info_queue)))
                g_info_queue.Reset();
            else
                g_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        }
#endif
    }
    // 适配器 LUID（验证 D3D11/D3D12 同适配器）
    {
        DXGI_ADAPTER_DESC ad {};
        adapter->GetDesc(&ad);
        g_d11_luid = (static_cast<std::uint64_t>(ad.AdapterLuid.HighPart) << 32) | ad.AdapterLuid.LowPart;
        const LUID d12 = g_d12dev->GetAdapterLuid();
        g_d12_luid = (static_cast<std::uint64_t>(d12.HighPart) << 32) | d12.LowPart;
    }
#if defined(FFX12_DEBUG_STEPS)
    std::printf("[sdk234] init: d3d12 device ok\n");
    std::fflush(stdout);
#endif
    g_gpu_interop_ready = false;
    if (g_gpu_interop)
    {
        ComPtr<ID3D11Device5> device5;
        bool setup_ok = SUCCEEDED(g_d11dev.As(&device5)) && device5 &&
            g_interop.initialize(*device5.Get(), *g_d12dev.Get());
        // 深度提取 CS 编译
        if (setup_ok)
        {
            ComPtr<ID3DBlob> blob, err;
            const HRESULT chr = D3DCompile(g_depth_extract_hlsl, std::strlen(g_depth_extract_hlsl),
                                           nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0,
                                           &blob, &err);
            if (FAILED(chr) || !blob)
                setup_ok = false;
            else if (FAILED(g_d11dev->CreateComputeShader(
                             blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                             &g_depth_extract_cs)))
                setup_ok = false;
        }
        // D3D11 侧 motion 解码 CS（绕过从不写 cvt 的 D3D12 decode pass）
        if (setup_ok)
        {
            ComPtr<ID3DBlob> blob, err;
            const HRESULT chr = D3DCompile(g_motion_decode_d11_hlsl, std::strlen(g_motion_decode_d11_hlsl),
                                           nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0,
                                           &blob, &err);
            if (FAILED(chr) || !blob)
                setup_ok = false;
            else if (FAILED(g_d11dev->CreateComputeShader(
                             blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                             &g_motion_decode_cs)))
                setup_ok = false;
        }
        // 静止死区变体（微小 motion 归零；触发 FSR 静止锁定）
        if (setup_ok)
        {
            ComPtr<ID3DBlob> blob, err;
            const D3D_SHADER_MACRO defs[] = {{"FFX12_MOTION_DEADZONE", "1"}, {nullptr, nullptr}};
            const HRESULT chr = D3DCompile(g_motion_decode_d11_hlsl, std::strlen(g_motion_decode_d11_hlsl),
                                           nullptr, defs, nullptr, "main", "cs_5_0", 0, 0,
                                           &blob, &err);
            if (FAILED(chr) || !blob)
                setup_ok = false;
            else if (FAILED(g_d11dev->CreateComputeShader(
                             blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                             &g_motion_decode_dz_cs)))
                setup_ok = false;
        }
        // 不解码变体（Ffx12MotionDecode=0 时启用；让该开关真正生效）
        if (setup_ok)
        {
            ComPtr<ID3DBlob> blob, err;
            const D3D_SHADER_MACRO defs[] = {{"FFX12_MOTION_RAW", "1"}, {nullptr, nullptr}};
            const HRESULT chr = D3DCompile(g_motion_decode_d11_hlsl, std::strlen(g_motion_decode_d11_hlsl),
                                           nullptr, defs, nullptr, "main", "cs_5_0", 0, 0,
                                           &blob, &err);
            if (FAILED(chr) || !blob)
                setup_ok = false;
            else if (FAILED(g_d11dev->CreateComputeShader(
                             blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                             &g_motion_raw_cs)))
                setup_ok = false;
        }
        if (setup_ok)
        {
            // b0 常数缓冲已改为**每实例**在 ensure_pool 里创建（P2）：depth 提取与 motion
            // 解码共用 b0.x，全局一份时两个实例会互相把对方的值刷进同一缓冲。
            // 注意这里不再因 b0 创建失败而影响 ready 判定——旧实现即使 CreateBuffer 失败
            // 也照样置 ready=true，故行为等价。
            g_gpu_interop_ready = true;
            sdk_note(L"gpu interop ready stage=init (NT shared handles + D3D11.4 fence)");
        }
        else
        {
            g_gpu_interop_ready = false;
            sdk_note(L"gpu interop unavailable stage=init");
            g_interop = {};
            g_depth_extract_cs.Reset();
        }
    }
    // Load the known-good ffx-api provider and select exactly the configured
    // effect version.  Do not silently fall back: a version mismatch must fail
    // open to the game's native path rather than alter the temporal baseline.
    if (sdk_dll_path == nullptr || sdk_dll_path[0] == L'\0')
    {
        sdk_note(L"ffx-api path missing stage=runtime");
        return false;
    }
    // 路由可能把 SDK 路径从默认 4.1.1 切到 402c（RDNA2/N 卡 16-50/Intel Arc）。
    // 切换时从 preload 候选缓存取句柄（候选已在 OptiScaler hook 装好前装载，
    // 避免 LoadLibrary 被劫持 err=18 → 4070 Super 实测）；缓存未命中才兜底 LoadLibrary。
    if (g_sdk_module != nullptr && g_sdk_loaded_path == sdk_dll_path)
    {
        // 路径一致：复用当前句柄
    }
    else
    {
        HMODULE target = nullptr;
        // 起：候选缓存优先（preload 的干净句柄，防 OptiScaler hook 劫持 err=18），
        // 缓存未命中才 LoadLibrary 兜底（Auto402c=0 手动路径场景：与 OptiScaler 模块对齐）。
        // 曾尝试统一 LoadLibrary——破坏手动接管，回滚恢复本逻辑。
        for (const PreloadEntry &e : g_preloaded)
        {
            if (e.path == sdk_dll_path)
            {
                target = e.module;
                break;
            }
        }
        if (target == nullptr)
            target = LoadLibraryW(sdk_dll_path); // 候选外路径兜底
        if (target == nullptr)
        {
            sdk_note(L"LoadLibrary ffx-api failed err=%lu stage=runtime",
                     static_cast<unsigned long>(GetLastError()));
            return false;
        }
        if (g_sdk_module != nullptr)
            FreeLibrary(g_sdk_module); // 释放旧主句柄（切换）
        g_sdk_module = target;
        g_sdk_loaded_path = sdk_dll_path;
    }
    g_runtime.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g_sdk_module, "ffxCreateContext"));
    g_runtime.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g_sdk_module, "ffxDestroyContext"));
    g_runtime.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g_sdk_module, "ffxDispatch"));
    g_runtime.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(g_sdk_module, "ffxQuery"));
    g_runtime.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(g_sdk_module, "ffxConfigure"));
    if (!g_runtime.create || !g_runtime.destroy || !g_runtime.dispatch || !g_runtime.query || !g_runtime.configure)
    {
        sdk_note(L"ffx-api export resolve failed stage=runtime");
        FreeLibrary(g_sdk_module);
        g_sdk_module = nullptr;
        g_runtime = {};
        return false;
    }
    std::uint64_t version_count = 0;
    ffxQueryDescGetVersions query {};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    query.device = g_d12dev.Get();
    query.outputCount = &version_count;
    if (g_runtime.query(nullptr, reinterpret_cast<ffxQueryDescHeader *>(&query)) != FFX_API_RETURN_OK ||
        version_count == 0 || version_count > 64)
    {
        sdk_note(L"ffx-api version query failed count=%llu stage=runtime", version_count);
        FreeLibrary(g_sdk_module);
        g_sdk_module = nullptr;
        g_runtime = {};
        return false;
    }
    std::vector<std::uint64_t> version_ids(static_cast<std::size_t>(version_count));
    std::vector<const char *> version_names(static_cast<std::size_t>(version_count));
    std::uint64_t version_capacity = version_count;
    query.outputCount = &version_capacity;
    query.versionIds = version_ids.data();
    query.versionNames = version_names.data();
    if (g_runtime.query(nullptr, reinterpret_cast<ffxQueryDescHeader *>(&query)) != FFX_API_RETURN_OK)
    {
        sdk_note(L"ffx-api version enumerate failed stage=runtime");
        FreeLibrary(g_sdk_module);
        g_sdk_module = nullptr;
        g_runtime = {};
        return false;
    }
    g_sdk_version_id = 0;
    // （N 卡降级）：大版本前缀匹配，无匹配时自动降级 4→3→2。
    // 例：N 卡上 4.1.1 provider 的 GET_VERSIONS 列表不含 "4.x"（provider 按 GPU 能力过滤：
    // FSR4 仅 RDNA4/RDNA3 dGPU 官方支持），请求 4.x 应自动回落 3.1.5（理论预期），
    // 而非硬失败导致每帧 DISPATCH_FAILED。降级结果同步 OSD 显示名，避免误导。
    std::string matched_name;
    std::string prefix = g_sdk_version_prefix; // "4."/"3."/"2."
    while (!prefix.empty() && g_sdk_version_id == 0)
    {
        for (std::uint64_t i = 0; i < version_capacity; ++i)
        {
            if (version_names[i] != nullptr &&
                std::strncmp(version_names[i], prefix.c_str(), prefix.size()) == 0)
            {
                g_sdk_version_id = version_ids[i];
                matched_name = version_names[i];
                break;
            }
        }
        if (g_sdk_version_id == 0)
        {
            if (prefix == "4.")
            {
                prefix = "3.";
                sdk_note(L"ffx-api version 4.x unavailable on this GPU, falling back to 3.x");
            }
            else if (prefix == "3.")
            {
                prefix = "2.";
                sdk_note(L"ffx-api version 3.x unavailable on this GPU, falling back to 2.x");
            }
            else
            {
                break; // 2.x 也无匹配
            }
        }
    }
    if (g_sdk_version_id == 0)
    {
        sdk_note(L"requested ffx-api version unavailable stage=runtime");
        FreeLibrary(g_sdk_module);
        g_sdk_module = nullptr;
        g_runtime = {};
        return false;
    }
    // 显示名按实际匹配版本（含降级）更新，OSD 如实反映
    if (!matched_name.empty())
    {
        g_sdk_matched_name = matched_name;
        const char major = matched_name[0];
        if (major == '4')
            g_version_name = "ffx12/FSR4";
        else if (major == '3')
            g_version_name = "ffx12/FSR3";
        else if (major == '2')
            g_version_name = "ffx12/FSR2";
    }


    g_active.store(true, std::memory_order_release);
    return true;
}

bool initialize_runtime(ID3D11Device *game_device, const wchar_t *path)
{
    try
    {
        return init_locked(game_device, path);
    }
    catch (...)
    {
        // 失败发生在 context 创建与 GPU 提交之前，不授权任何纹理交付或 fence 退休。
        sdk_note(L"provider initialization threw C++ exception stage=runtime");
        return false;
    }
}

bool init(ID3D11Device *game_device, const wchar_t *sdk_dll_path)
{
    std::lock_guard<std::timed_mutex> lock(g_mutex);
    return initialize_runtime(game_device, sdk_dll_path);
}

void shutdown()
{
    // 先置停机标志再取锁：这样正在等 g_mutex 的 dispatch() 拿到锁后会立刻退出，
    // 而不是继续使用即将被释放的 D3D12 资源。
    g_shutting_down.store(true, std::memory_order_release);

    // ⚠️ 必须带超时取锁，不能无限等 —— 这是"游戏窗口关了、进程不退出"的根因。
    //
    // dispatch() 是**全程持 g_mutex** 的，中间有 Wait(v_prev) / ctx4->Wait 等 GPU fence
    // 阻塞等待。退出时渲染线程若正卡在这些等待里，它就持着 g_mutex 不放；
    // g_shutting_down 只能拦住"还没进 dispatch"的线程，拦不住**已经在里面**的那个。
    // 旧实现用 lock_guard 无限等待 → 进程永久挂死。
    // 实测证据（2026-09-18，修复 shutdown_trace 的共享模式后首次拿到）：
    //   [SHUTDOWN] final_shutdown_begin
    //   （再无 final_shutdown_end）
    //
    // 拿不到锁就直接放弃释放：进程已在退出路径，句柄与设备资源交 OS 回收。
    // 拿不到锁时**绝不能**继续往下走 —— 下面的 Reset() 会与仍在使用这些资源的
    // 渲染线程并发，那是 use-after-free。
    std::unique_lock<std::timed_mutex> lock(g_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(500)))
    {
        return;
    }

    if (!g_process_exiting.load(std::memory_order_acquire) && !g_interop.drain())
    {
        sdk_note(L"native queue not retired; backend ownership kept stage=shutdown");
        return;
    }
    g_active.store(false, std::memory_order_release);
    for (SdkContext &sc : g_sdk_ctxs)
    {
        destroy_context(sc);
        release_instance_res(sc);
    }
    g_sdk_ctxs.clear();
    g_mirror_color_tex.Reset();
    g_mirror_output_tex.Reset();
    g_depth_extract_cs.Reset();
    g_motion_decode_cs.Reset();
    g_motion_decode_dz_cs.Reset();
    g_motion_raw_cs.Reset();
    g_gpu_interop_ready = false;
    g_interop = {};
    g_delivered_count = 0;
    g_on12dev.Reset();
    g_gpu_only_transport_available = false;
    g_d12dev.Reset();
    // preload 缓存：**每个**候选句柄都要释放。旧实现只 FreeLibrary(g_sdk_module)，
    // 而 g_sdk_module 只是 g_preloaded 里第一个命中项，其余候选（默认 4.1.1 + 402c
    // 两个）的句柄永久泄漏。g_sdk_module 就是其中一个元素，故整表释放即可，
    // 之后必须置空以免重复释放。
    // 进程终止（DllMain detach 且 lpReserved != nullptr）时不能释放：已持有 loader lock。
    if (!g_process_exiting.load(std::memory_order_acquire))
    {
        // 去重：同一 DLL 以不同路径被 LoadLibrary 两次会返回同一 HMODULE
        // （引用计数 +1），按条目逐个 FreeLibrary 会重复释放。
        std::vector<HMODULE> freed;
        freed.reserve(g_preloaded.size());
        for (const PreloadEntry &entry : g_preloaded)
        {
            if (entry.module == nullptr)
                continue;
            if (std::find(freed.begin(), freed.end(), entry.module) != freed.end())
                continue;
            freed.push_back(entry.module);
            FreeLibrary(entry.module);
        }
    }
    g_preloaded.clear();
    g_sdk_module = nullptr;
    g_runtime = {};
    g_sdk_version_id = 0;
    // b0 缓存（含其失效）已随实例资源一起释放/失效，见 release_instance_res。
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

// 设备移除恢复（recover_device_removed_locked）：放弃当前 D3D12/互操作后端并复位活动标志，
// 下一次 dispatch 惰性重建（新设备/队列/共享纹理/FFX context）。
// 坏设备上不调用 runtime.destroy（可能 AV）；旧 context 随进程回收（一次事故级泄漏可接受）。
// 调用方必须已持有 g_mutex（dispatch 路径）。
void recover_device_removed_locked()
{
    if (!g_active.load(std::memory_order_relaxed))
        return;
    g_active.store(false, std::memory_order_release);
    sdk234_step("recover device removed");
    for (SdkContext &sc : g_sdk_ctxs)
    {
        destroy_context(sc);
        release_instance_res(sc);
    }
    g_sdk_ctxs.clear();
    g_mirror_color_tex.Reset();
    g_mirror_output_tex.Reset();
    g_interop = {};
    g_depth_extract_cs.Reset();
    g_motion_decode_cs.Reset();
    g_motion_decode_dz_cs.Reset();
    g_motion_raw_cs.Reset();
    g_gpu_interop_ready = false;
    g_d12dev.Reset();
    g_delivered_count = 0;
}

DispatchResult dispatch(const FrameInput &input, ID3D11DeviceContext *game_context, std::uint64_t instance_key)
{
    // 【诊断】PerfProbe：本函数 = 每游戏帧一次的 upscale 调用 ⇒ 它就是**帧边界**
    // （Present 钩子在本机直连模式下默认不装，见 swapchain_present_hook_needed()）。
#if defined(DX11FSRBRIDGE_ENABLE_DIAGNOSTICS)
    perf_probe::Scope perf_scope(perf_probe::Segment::upscale_dispatch);
    perf_probe::note_frame(perf_probe::FrameSource::upscale);
    perf_probe::maybe_flush();
#endif
    // 停机中：立即返回，不取 g_mutex、不碰 D3D12 资源。
    // 在取锁**之前**判定是必须的——shutdown() 可能已持锁在释放资源，此时再进来等锁
    // 只会白等，拿到锁之后用的还是已被 Reset 的 ComPtr。
    if (g_shutting_down.load(std::memory_order_acquire))
        return DispatchResult::failed;
    std::lock_guard<std::timed_mutex> lock(g_mutex);
    // 拿到锁后再判一次：等锁期间 shutdown() 可能已经开始。
    if (g_shutting_down.load(std::memory_order_acquire))
        return DispatchResult::failed;
    g_last_ffx_dispatch_rc.store(kFfxDispatchNotReached, std::memory_order_relaxed);
    if (!game_context || !input.color || !input.depth || !input.motion || !input.output_target ||
        !input.render_w || !input.render_h || !input.display_w || !input.display_h)
        return DispatchResult::failed;
#if defined(DX11FSRBRIDGE_ENABLE_NGX)
    if (g_backend == BackendKind::Ngx)
        return ngx::dispatch(input, game_context, instance_key);
#endif
#if defined(FFX12_DEBUG_STEPS)
    std::printf("[sdk234] dispatch enter active=%d\n", g_active.load(std::memory_order_relaxed) ? 1 : 0);
    std::fflush(stdout);
#endif
    // 懒初始化：用游戏 context 的设备 + 配置的 DLL 路径（调用方已持锁）
    if (!g_active.load(std::memory_order_relaxed))
    {
        if (g_sdk_path.empty()) return DispatchResult::failed;
        // GetDevice 的临时引用和 init_locked 持有的后端引用不能混在一起；失败重试也要归还前者。
        ComPtr<ID3D11Device> device;
        game_context->GetDevice(device.GetAddressOf());
        if (!device || !initialize_runtime(device.Get(), g_sdk_path.c_str()))
            return DispatchResult::failed;
    }
#if defined(FFX12_DEBUG_STEPS)
    std::printf("[sdk234] init done, ensuring pool %ux%u -> %ux%u\n", input.render_w, input.render_h,
                input.display_w, input.display_h);
    std::fflush(stdout);
#endif

    // 设备移除检测（P0-3 可观测性）：D3D12/D3D11 任一侧移除即失败并留痕
    if (g_d12dev)
    {
        const HRESULT drr = g_d12dev->GetDeviceRemovedReason();
        if (FAILED(drr))
        {
            sdk_note(L"dispatch device-removed d3d12 reason=0x%08X stage=entry",
                     static_cast<unsigned>(drr));
            recover_device_removed_locked(); // 放弃后端，下次 dispatch 惰性重建
            return DispatchResult::failed;
        }
    }
    if (g_d11dev)
    {
        const HRESULT drr11 = g_d11dev->GetDeviceRemovedReason();
        if (FAILED(drr11))
        {
            sdk_note(L"dispatch device-removed d3d11 reason=0x%08X stage=entry",
                     static_cast<unsigned>(drr11));
            recover_device_removed_locked();
            return DispatchResult::failed;
        }
    }
    if (!g_interop.available()) return DispatchResult::failed;
    // 不同场景实例的 temporal history 和共享纹理不能混用。
    SdkContext *sc = ctx_for_key(instance_key);
    if (!sc)
    {
        sdk_note(L"ctx slot exhausted stage=ctx");
        return DispatchResult::failed;
    }
    if (!ensure_pool(input, *sc))
    {
        sdk_note(L"ensure_pool failed render=%ux%u->%ux%u stage=pool",
                 input.render_w, input.render_h, input.display_w, input.display_h);
        return DispatchResult::failed;
    }
    if (!create_context(*sc))
    {
        sdk_note(L"create_context failed render=%ux%u->%ux%u stage=ctx",
                 input.render_w, input.render_h, input.display_w, input.display_h);
        return DispatchResult::failed;
    }
    const bool reset = input.reset || sc->first;
    g_last_reset = reset;
    const bool delivered = dispatch_gpu_shared(input, game_context, *sc, reset);
    sc->first = !delivered;
    return delivered ? DispatchResult::native_ordered : DispatchResult::failed;
}

void set_async_upscale(bool enable)
{
    std::lock_guard<std::timed_mutex> lock(g_mutex);
    g_async_upscale = enable;
}

void set_instance_diag(bool enable)
{
    // 只读标志 + 计数上限；不加锁（桥在 load_config 里调用一次，启动后固定）。
    g_instance_diag = enable;
    g_instance_diag_notes.store(0, std::memory_order_relaxed);
}

bool async_upscale_enabled()
{
    std::lock_guard<std::timed_mutex> lock(g_mutex);
    return g_async_upscale;
}

const char *selected_version_name()
{
    return g_version_name.empty() ? "?" : g_version_name.c_str();
}

const char *matched_version_name()
{
    return g_sdk_matched_name.empty() ? "?" : g_sdk_matched_name.c_str();
}

void set_sdk_dll_path(const wchar_t *path)
{
    if (path != nullptr)
        g_sdk_path = path;
}

void set_depth_inverted(bool inverted)
{
    g_depth_inverted = inverted;
}

void set_decode_motion(bool decode)
{
    g_decode_motion = decode;
}

std::uint32_t create_flags()
{
    return g_create_flags;
}

bool pq_chain()
{
    return g_use_pq_chain;
}

bool decode_motion_wired()
{
    return g_motion_raw_cs != nullptr;
}

// flags 的可读解码。**为什么必须由后端提供**：FFX_UPSCALE_ENABLE_* 枚举在
// ffx_upscale.h（后端私有 SDK 头）里，桥 TU 不引入它。
// 存在的意义：日志曾把 flags 硬编码成 "hdr,deptinv,autoexp"，与运行时无关——
// 出厂 ini（Ffx12Hdr=0/Ffx12NonLinear=0）下会让人误以为 HDR 已启用。
const char *create_flags_text()
{
    // ⚠️ 必须 thread_local（2026-09-19）：此前是普通 `static std::string`，
    // 注释假设"单线程（dispatch 日志路径）使用"——但该假设**不成立**：
    // 函数可能被渲染线程与其它线程同时调用，而 `clear()` + 逐段 `+=` 会让
    // 另一线程读到**半构造**的字符串（撕裂读）。
    // 改为 thread_local：每线程独立实例，零同步开销。
    // 安全性：唯一调用点（Dx11FsrBridge.cpp:10796）把结果**立即拼进 std::string**
    // （值拷贝），不保留指针，故每线程缓冲足够。
    thread_local std::string text;
    text.clear();
    auto add = [](bool on, const char *name)
    {
        if (!on)
            return;
        if (!text.empty())
            text += ",";
        text += name;
    };
    add((g_create_flags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE) != 0, "hdr");
    add((g_create_flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) != 0, "deptinv");
    add((g_create_flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) != 0, "autoexp");
    add((g_create_flags & FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) != 0, "nonlinear");
    add((g_create_flags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) != 0, "mvjitcancel");
    add((g_create_flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0, "dispmv");
    add((g_create_flags & FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION) != 0, "dynres");
    if (text.empty())
        text = "none";
    return text.c_str();
}

const char *create_flags_hex()
{
    // 同上：改 thread_local（原为普通 static char[16]，跨线程写同一缓冲有竞争）。
    thread_local char buffer[16] {};
    std::snprintf(buffer, sizeof(buffer), "0x%X", static_cast<unsigned>(g_create_flags));
    return buffer;
}

void set_motion_flip(float flip) // XeSS/DLSS 定向——motion 方向翻转（±1）
{
    g_motion_flip = flip;
}

void set_depth_scale(float scale) // XeSS/DLSS 定向——depth 值域归一化（XeSS 期望 [0,1]）
{
    g_depth_scale = scale;
}

void set_motion_vectors_jittered(bool jittered)
{
    g_motion_vectors_jittered = jittered;
}

void set_hdr_input(bool hdr)
{
    g_hdr_input = hdr;
}

void set_auto_exposure(bool auto_exposure)
{
    g_auto_exposure = auto_exposure;
}

void set_non_linear(bool non_linear)
{
    g_non_linear = non_linear;
}

void set_use_pq_chain(bool use_chain)
{
    g_use_pq_chain = use_chain;
}

void set_velocity_factor(float factor)
{
    g_velocity_factor = factor;
}

// 版本命名规范——对外统一 ffx12 品牌（ffx12-fsr4.1.1 / ffx12-fsr3.1.5 /
// ffx12-fsr2.3.4），内部映射回 ffx-api 枚举名（4.1.1/3.1.5/2.3.4）做版本匹配。
// 版本判定简化为大版本（ffx12-fsr4.x / ffx12-fsr3.x / ffx12-fsr2.x）——
// 按前缀匹配 provider 支持的版本列表（4.x 命中 4.0.2c/4.1.1/未来 4.2.x），SDK 更新免适配。
void set_sdk_version(const char *name)
{
    if (name == nullptr || name[0] == '\0')
        return;
    std::string input = name;
    // 兼容旧值（2.3.4/3.1.5/4.1.1 直接可用；新规范带 ffx12-fsr 前缀）
    const char *prefix = "ffx12-";
    if (input.rfind(prefix, 0) == 0)
        input = input.substr(std::strlen(prefix));
    if (input.rfind("fsr", 0) == 0)
        input = input.substr(3); // "fsr4.x" -> "4.x"（ffx-api 枚举名前缀）
    // 大版本前缀（只认 2/3/4 系）
    if (!input.empty() && input[0] >= '2' && input[0] <= '4')
        g_sdk_version_prefix = std::string(1, input[0]) + ".";
    else
        g_sdk_version_prefix.clear();
    // 显示名统一为 ffx12 品牌（4.0.2c 为 RDNA2 专用 FSR4 模型）
    if (g_sdk_version_prefix == "4.")
        g_version_name = "ffx12/FSR4";
    else if (g_sdk_version_prefix == "3.")
        g_version_name = "ffx12/FSR3";
    else if (g_sdk_version_prefix == "2.")
        g_version_name = "ffx12/FSR2";
    else
        g_version_name = "ffx12";
}

void adapter_luids(std::uint64_t &d11_luid, std::uint64_t &d12_luid)
{
    d11_luid = g_d11_luid;
    d12_luid = g_d12_luid;
}

void interop_capabilities(bool &dx11on12, bool &gpu_only_transport)
{
    std::lock_guard<std::timed_mutex> lock(g_mutex);
    dx11on12 = g_dx11on12_available;
    gpu_only_transport = g_gpu_only_transport_available;
}

void get_sdk_messages(std::wstring &out)
{
    std::lock_guard lock(g_sdk_msg_mutex);
    out = g_sdk_messages;
}

void set_motion_deadzone(bool enable)
{
    g_motion_deadzone = enable;
}

// 与 OptiScaler 共存兼容。
// preload 提前装载**全部候选** provider（默认 4.1.1 + 402c）并缓存句柄——
// OptiScaler 的 LoadLibrary hook 会劫持任意 ffx-api 文件名（含 402c，4070 Super 实测 err=18），
// 必须在 hook 装好前（DllMain attach 早期）加载；init_locked 按最终路径从缓存取句柄。
void preload(const wchar_t *path)
{
    if (path == nullptr || path[0] == L'\0')
        return;
    HMODULE h = LoadLibraryW(path);
    if (h == nullptr)
        return;
    std::lock_guard<std::timed_mutex> lock(g_mutex);
    for (const PreloadEntry &e : g_preloaded)
        if (e.path == path)
            return;
    g_preloaded.push_back({path, h});
    if (g_sdk_module == nullptr)
    {
        g_sdk_module = h;
        g_sdk_path = path;
        g_sdk_loaded_path = path;
    }
}

void set_gpu_interop(bool enable)
{
    g_gpu_interop = enable;
}

bool gpu_interop_ready()
{
    return g_gpu_interop_ready;
}

ID3D11Texture2D *debug_output_texture()
{
    // P2：池按实例 ⇒ 返回"最近一次派发实例"的输出纹理（诊断镜像持引用，见 release_instance_res）
    return g_mirror_output_tex.Get();
}

ID3D11Texture2D *debug_color_texture()
{
    return g_mirror_color_tex.Get();
}

// 实际输入格式（桥日志诊断用）
void input_formats(std::uint32_t &color, std::uint32_t &depth, std::uint32_t &motion, std::uint32_t &output)
{
    std::lock_guard<std::timed_mutex> lock(g_mutex);
    color = static_cast<std::uint32_t>(g_input_color_fmt);
    depth = static_cast<std::uint32_t>(g_input_depth_fmt);
    motion = static_cast<std::uint32_t>(g_input_motion_fmt);
    output = static_cast<std::uint32_t>(g_input_output_fmt);
}

// 诊断：最近一次 dispatch 的 reset 标志 + 上下文重建累计次数
void debug_state(bool &last_reset, std::uint64_t &ctx_recreates)
{
    std::lock_guard<std::timed_mutex> lock(g_mutex);
    last_reset = g_last_reset;
    ctx_recreates = g_ctx_recreates;
}

int last_ffx_dispatch_return_code()
{
    return g_last_ffx_dispatch_rc.load(std::memory_order_relaxed);
}

#if !defined(DX11FSRBRIDGE_RELEASE_RUNTIME)
void set_dump_frames(std::uint32_t n)
{
    g_dump_frames = n;
}
void set_output_mark(bool mark)
{
    g_output_mark = mark;
}
void set_decode_test(bool test)
{
    g_decode_test = test;
}
void set_motion_decode_test(bool test)
{
    g_motion_decode_test = test;
}
void set_debug_layer(bool enable)
{
    g_debug_layer = enable;
}
void debug_pixels(float out_pixels[16])
{
    std::lock_guard lock(g_debug_px_mutex);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out_pixels[i * 4 + j] = g_debug_pixels[i].v[j];
}

void get_output_samples(std::uint32_t raw[5], std::uint32_t &w, std::uint32_t &h, bool &valid)
{
    std::fill_n(raw, 5, 0u);
    w = h = 0;
    valid = false;
}

void get_chain_samples(ChainSampleData &out)
{
    out = {};
}
#endif

} // namespace ffx12
