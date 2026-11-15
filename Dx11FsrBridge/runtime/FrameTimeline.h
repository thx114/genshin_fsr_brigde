#pragma once

#include <algorithm>
#include <cstdint>

namespace neo::graphics
{
// 2026-11-15：时间和代次只由已排入的写回推进；构造 SDK 入参不修改已发布历史。
class FrameTimeline
{
public:
    struct Attempt
    {
        std::uint64_t generation;
        std::uint32_t frame;
        std::uint64_t tick_ms;
        std::uint64_t time_us;
        float delta_ms;
        bool reset;
        std::uint64_t gap_ms;
    };

    bool is_new(std::uint64_t generation) const noexcept { return generation > generation_; }
    bool same_game_frame(std::uint32_t frame) const noexcept { return has_frame_ && frame == frame_; }
    std::uint64_t generation() const noexcept { return generation_; }
    std::uint64_t last_write_tick() const noexcept { return write_tick_; }

    Attempt prepare(std::uint64_t generation, std::uint32_t frame, std::uint64_t tick_ms,
        std::uint64_t time_us, bool force_reset) const noexcept
    {
        const std::uint64_t gap = write_tick_ != 0 && tick_ms >= write_tick_ ? tick_ms - write_tick_ : 0;
        const bool reset_gap = gap > k_takeover_gap_ms;
        float dt = k_default_delta_ms;
        if (!reset_gap && time_us_ != 0)
        {
            if (same_game_frame(frame))
                dt = delta_ms_;
            else if (time_us > time_us_)
                dt = std::clamp(static_cast<float>(time_us - time_us_) / 1000.0f, 1.0f, 100.0f);
        }
        return { generation, frame, tick_ms, time_us, dt, force_reset || reset_next_ || reset_gap, gap };
    }

    void commit(const Attempt &attempt) noexcept
    {
        if (!is_new(attempt.generation)) return;
        if (!same_game_frame(attempt.frame) || time_us_ == 0 || attempt.gap_ms > k_takeover_gap_ms)
            time_us_ = attempt.time_us;
        generation_ = attempt.generation;
        frame_ = attempt.frame;
        has_frame_ = true;
        write_tick_ = attempt.tick_ms;
        delta_ms_ = attempt.delta_ms;
        reset_next_ = false;
    }

    void invalidate() noexcept
    {
        reset_next_ = true;
        write_tick_ = time_us_ = 0;
        delta_ms_ = k_default_delta_ms;
    }

    void relinquish() noexcept
    {
        invalidate();
        has_frame_ = false; // 上次的游戏帧号已无语义；代次仍不能重放。
    }

    static constexpr std::uint64_t k_takeover_gap_ms = 500;
    static constexpr float k_default_delta_ms = 16.7f;

private:
    std::uint64_t generation_ = 0;
    std::uint64_t write_tick_ = 0, time_us_ = 0;
    std::uint32_t frame_ = 0;
    float delta_ms_ = k_default_delta_ms;
    bool has_frame_ = false;
    bool reset_next_ = false;
};
}
