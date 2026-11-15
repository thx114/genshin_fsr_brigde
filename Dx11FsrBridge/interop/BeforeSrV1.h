#pragma once

#include <d3d11.h>
#include <cstddef>
#include <cstdint>

namespace neo::interop
{
inline constexpr char k_register_before_sr_v1[] = "Dx11FsrBridge_RegisterBeforeSrV1";
inline constexpr char k_unregister_before_sr_v1[] = "Dx11FsrBridge_UnregisterBeforeSrV1";
inline constexpr char k_query_before_sr_frame_v1[] = "Dx11FsrBridge_QueryBeforeSrFrameV1";

// 2026-11-15：视图格式描述硬件取样方式，不是产生这些像素的 transfer 声明。
// 当前桥没有独立的游戏输入声明；不能用 IsHDR=0、UNORM、sRGB SRV 或最终交换链代替。
enum class ColorTransfer : std::uint32_t { unknown = 0, srgb = 1, linear = 2, pq = 3 };
inline constexpr std::uint32_t k_before_sr_depth_inverted = 1u << 0;
inline constexpr std::uint32_t k_before_sr_motion_jittered = 1u << 1;
inline constexpr std::uint32_t k_before_sr_reset = 1u << 2;

// x64 ABI。Color/Depth/Motion 是实际 NGX 输入，均为 mip 0、slice 0、原点 (0,0) 的 render-size 纹理。
// 指针及内容仅在本次同步调用内借用。frame 是本桥的派发序号；instance 是路由键，不是资源寿命凭证。
struct BeforeSrFrameV1
{
    std::uint32_t size = sizeof(BeforeSrFrameV1);
    std::uint32_t version = 1;
    std::uint64_t frame = 0;
    std::uint64_t instance = 0;
    ID3D11DeviceContext *context = nullptr;
    ID3D11Texture2D *color = nullptr;
    ID3D11Texture2D *depth = nullptr;  // 已提取的 R32_FLOAT；保留源深度方向。
    ID3D11Texture2D *motion = nullptr; // 已解包的 R16G16_FLOAT；配合 motion_scale 使用。
    std::uint32_t render_width = 0, render_height = 0;
    std::uint32_t display_width = 0, display_height = 0;
    DXGI_FORMAT color_storage_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT color_view_format = DXGI_FORMAT_UNKNOWN; // 本次游戏累积 Draw 读取的实际 SRV 格式。
    ColorTransfer color_transfer = ColorTransfer::unknown;
    std::uint32_t flags = 0;
    float jitter_x = 0, jitter_y = 0; // render 像素。
    float motion_scale_x = 1, motion_scale_y = 1;
};
static_assert(sizeof(BeforeSrFrameV1) == 104 && offsetof(BeforeSrFrameV1, color_transfer) == 80);

// 同一 immediate context 上在 NGX Evaluate 前记录；输入只读，返回前恢复更改过的 context 状态。
// S_OK + 兼容的独立纹理才替代 Color；任何非空输出都转交一份 COM 引用。
// 不做阻塞 I/O、GPU 等待或桥调度重入；外部消费者拥有自己的替代纹理复用策略。
using RecordBeforeSrV1 = HRESULT (WINAPI *)(const BeforeSrFrameV1 *, ID3D11Texture2D **replacement) noexcept;
// 仅一个记录者。卸载回调模块前必须成功注销；ERROR_BUSY 表示仍在回调，须在回调之外重试。
// 若替代纹理由消费者的 DLL 实现，该 COM 对象还须独立保证自己的模块在引用存续期间不卸载。
using RegisterBeforeSrV1 = HRESULT (WINAPI *)(RecordBeforeSrV1) noexcept;
using UnregisterBeforeSrV1 = HRESULT (WINAPI *)(RecordBeforeSrV1) noexcept;

// F6 仅在原 V1 Evaluate 的调用线程、使用从本次原始 NGX 参数袋取得的 Color/Depth 查询。
// out 先初始化 size/version；S_FALSE 表示无当次匹配，不改 out。不得用查询结果替代 GPU 同步证明。
using QueryBeforeSrFrameV1 = HRESULT (WINAPI *)(ID3D11DeviceContext *, ID3D11Resource *color,
    ID3D11Resource *depth, BeforeSrFrameV1 *out) noexcept;
}
