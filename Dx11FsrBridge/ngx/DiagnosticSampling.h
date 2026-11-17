#pragma once

#include <cstdint>

namespace neo::ngx
{
// 2026-11-17（上游 3ab1dc0/4dd8406）：1024 与常用抖动周期共振；1021 使采样遍历各相位。
inline constexpr std::uint64_t k_jitter_sample_period = 1021;
inline bool sample_jitter(std::uint64_t frame) noexcept
{
    return frame <= 8 || frame % k_jitter_sample_period == 0;
}
}
