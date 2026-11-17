#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace neo::tests::input_map
{
inline constexpr unsigned k_render_width = 512, k_render_height = 256;
inline constexpr unsigned k_display_width = 1024, k_display_height = 512;

struct FrameCase
{
    unsigned instance;
    float phase_x, phase_y;
    float current_x, current_y;
    float delayed_x, delayed_y;
    int reset;
    std::uint32_t color;
};

// 2026-11-15（PR #16）：合成的像素偏移为固定答案。返回 A 时 delay 应读 A 的上一帧，
// 不是时间上紧邻的 B。首帧无本实例前序样本，沿用现有零初值与 Reset 语义。
inline constexpr FrameCase k_frames[] {
    { 0, 0.25f,  0.875f,  0.25f,  -0.375f,  0.5f,    0.5f,   1, 0xFF216BC2u },
    { 0, 0.875f, 0.25f,  -0.375f,  0.25f,   0.25f,  -0.375f, 0, 0xFF73B133u },
    { 1, 0.375f, 0.125f,  0.125f,  0.375f,  0.5f,    0.5f,   1, 0xFFCF465Au },
    { 1, 0.75f,  0.625f, -0.25f,  -0.125f,  0.125f,  0.375f, 0, 0xFF3AA4E5u },
    { 0, 0.625f, 0.75f,  -0.125f, -0.25f,  -0.375f,  0.25f,  0, 0xFF8859C8u },
    { 0, 0.125f, 0.375f,  0.375f,  0.125f, -0.125f, -0.25f,  0, 0xFFCCAB42u },
};

struct Point
{
    int x, y;
};

struct GuideCase
{
    std::uint32_t packed_mv;
    Point previous_minus_current;
    float depth;
};

// 0 基、左上原点、render 像素坐标。把游戏编码的 RG 固定成 382/637（运动）和
// 509（静止轴）；由合成场景约定的上一帧位置验收，不在 CPU 复刻 HLSL 解码。
// 10 bit UNORM 量化 + FP16 存储，允许 0.30 render px。每帧轮换探点内容，排除旧 guide。
inline constexpr std::array k_probes { Point {72, 49}, Point {147, 214}, Point {349, 117}, Point {473, 233} };
inline constexpr GuideCase k_guides[] {
    { (509u << 10) | 382u, { 32,   0 }, 0.015625f },
    { (509u << 10) | 637u, {-32,   0 }, 0.03125f },
    { (382u << 10) | 509u, {  0,  16 }, 0.375f },
    { (637u << 10) | 509u, {  0, -16 }, 0.875f },
};
static_assert(k_probes.size() == std::size(k_guides));

inline const GuideCase &guide_at(unsigned frame, std::size_t probe)
{
    return k_guides[(frame + probe) % k_probes.size()];
}

// 两个符号只存在于测试 provider，不是桥的公开接口。实例地址由宿主提供，不能拿 handle 猜。
inline constexpr char k_configure[] = "ConfigureNgxInputMapFixture";
inline constexpr char k_evaluate_count[] = "NgxInputMapFixtureEvaluateCount";
using Configure = void (*)(bool delayed, std::uint64_t key_a, std::uint64_t key_b) noexcept;
using EvaluateCount = unsigned (*)() noexcept;
}
