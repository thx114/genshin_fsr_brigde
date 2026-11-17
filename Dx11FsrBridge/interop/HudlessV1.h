#pragma once

#include <d3d11.h>
#include <cstddef>
#include <cstdint>

namespace neo::interop
{
inline constexpr char k_register_hudless_v1[] = "Dx11FsrBridge_RegisterHudlessV1";
inline constexpr char k_unregister_hudless_v1[] = "Dx11FsrBridge_UnregisterHudlessV1";
inline constexpr std::uint32_t k_hudless_final_present_color = 1u << 0;
inline constexpr std::uint32_t k_hudless_flip = 1u << 1;

// 2026-11-17：尺寸和调用时机不能证明无 HUD；此接口只接收外部提供方的显式来源声明。
// x64 ABI；所有输入仅在同步回调中借用，禁止修改。output/game_output 仅供排除别名，尚未写回。
struct HudlessRequestV1
{
    std::uint32_t size = sizeof(HudlessRequestV1), version = 1;
    std::uint64_t request_id = 0, frame = 0, instance = 0;
    ID3D11DeviceContext *context = nullptr;
    ID3D11Texture2D *color = nullptr, *depth = nullptr, *motion = nullptr;
    ID3D11Texture2D *output = nullptr, *game_output = nullptr;
    std::uint32_t render_width = 0, render_height = 0;
    std::uint32_t display_width = 0, display_height = 0;
    DXGI_FORMAT output_format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t reserved = 0;
};

struct HudlessSurfaceV1
{
    std::uint32_t size = sizeof(HudlessSurfaceV1), version = 1;
    std::uint64_t request_id = 0, frame = 0, instance = 0;
    ID3D11Texture2D *color = nullptr;
    std::uint32_t flags = 0, reserved = 0;
};
static_assert(sizeof(HudlessRequestV1) == 104 && offsetof(HudlessRequestV1, output_format) == 96);
static_assert(sizeof(HudlessSurfaceV1) == 48 && offsetof(HudlessSurfaceV1, color) == 32);

// S_OK 必须回填当前三个身份、独立同设备/最终尺寸/存储格式的单 mip/slice/sample 纹理及 final 标志。
// final 标志声明同相机、无 HUD，并匹配最终 Present 的曝光/后处理/数值域；Flip 描述相对行向。
// 桥只能验证结构；消费者仍须最终颜色回执，NRWritten/Uncertain 不授权复用 NR 前来源。
// 任何非空 color（包括失败输出）都转交一份 COM 引用；消费者可保留至 GPU 读完。
// 在当前 immediate context 排入工作并恢复状态；禁止阻塞 I/O、GPU 等待、抛异常或桥调度重入。
// 不缓存借用输入；复用自有输出前须遵守消费者实际读退休同步，引用计数不是 GPU 退休证明。
using RecordHudlessV1 = HRESULT (WINAPI *)(const HudlessRequestV1 *, HudlessSurfaceV1 *) noexcept;
// 单提供方；模块卸载前须成功注销。ERROR_BUSY 时在回调外重试，不等待渲染线程。
// 自定义 COM 对象的实现模块还须在消费者持有引用期间保持加载。
using RegisterHudlessV1 = HRESULT (WINAPI *)(RecordHudlessV1) noexcept;
using UnregisterHudlessV1 = HRESULT (WINAPI *)(RecordHudlessV1) noexcept;
}
