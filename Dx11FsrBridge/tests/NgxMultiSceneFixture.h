#pragma once

#include <array>
#include <cstdint>

namespace neo::tests::multi_scene
{
struct Frame
{
    unsigned scene, width, height;
    bool reset, readable_dsv;
};

// 2026-11-16：A/B 同尺寸，C 独立改尺寸再恢复，D 额外绑定无关的可采样 DSV。
// 回到仍存在的场景必须继续自己的时域历史，不能随另一路的 key 重置。
// 最后两组先同时发布四路 Render，再按 D/C/B/A 消费，覆盖并存 token 与持久输入归属。
inline constexpr Frame k_frames[] {
    {0, 256, 128, true, false}, {1, 256, 128, true, false},
    {2, 192, 128, true, false}, {3, 128, 128, true, true},
    {0, 256, 128, false, false}, {2, 192, 128, false, false},
    {1, 256, 128, false, false}, {3, 128, 128, false, true},
    {2, 160, 128, true, false}, {0, 256, 128, false, false},
    {3, 128, 128, false, true}, {1, 256, 128, false, false},
    {2, 192, 128, true, false}, {1, 256, 128, false, false},
    {0, 256, 128, false, false}, {3, 128, 128, false, true},
    {3, 128, 128, false, true}, {2, 192, 128, false, false},
    {1, 256, 128, false, false}, {0, 256, 128, false, false},
    {3, 128, 128, false, true}, {2, 192, 128, false, false},
    {1, 256, 128, false, false}, {0, 256, 128, false, false},
};
inline constexpr std::size_t k_scene_count = 4;
inline constexpr unsigned k_simultaneous_begin = 16;
inline constexpr std::uint32_t color(unsigned frame)
{
    return 0xFF000000u | (((frame + 1u) * 0x345F97u) & 0xFFFFFFu);
}
using Configure = void (*)(const std::uint64_t *keys) noexcept;
using Select = void (*)(unsigned frame) noexcept;
using Complete = bool (*)() noexcept;
using Attempts = unsigned (*)() noexcept;
}
