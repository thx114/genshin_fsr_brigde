#include "../runtime/FrameTimeline.h"
#include <cstdio>

#define CHECK(...) do { if (!(__VA_ARGS__)) { std::puts("Failed: " #__VA_ARGS__); return 1; } } while (false)

int main()
{
    using neo::graphics::FrameTimeline;
    FrameTimeline state;
    CHECK(!state.is_new(0));
    const auto first = state.prepare(7, 3, 1000, 1000000, false);
    CHECK(first.delta_ms == FrameTimeline::k_default_delta_ms && !first.reset && state.last_write_tick() == 0);
    state.commit(first);
    CHECK(state.generation() == 7 && !state.is_new(6) && !state.is_new(7) && state.is_new(8));
    const auto next = state.prepare(8, 4, 1013, 1013000, false);
    CHECK(next.delta_ms == 13.0f && state.last_write_tick() == 1000);
    state.commit(next);
    const auto repeat = state.prepare(9, 4, 1014, 1014800, false);
    CHECK(repeat.delta_ms == 13.0f && state.same_game_frame(4));
    state.commit(repeat);
    CHECK(state.prepare(10, 5, 1026, 1026000, false).delta_ms == 13.0f);
    CHECK(state.prepare(10, 5, 1013, 1010000, false).delta_ms == FrameTimeline::k_default_delta_ms);
    const auto gap = state.prepare(10, 5, 1515, 1515000, false);
    CHECK(gap.gap_ms == 501 && gap.reset && gap.delta_ms == FrameTimeline::k_default_delta_ms);
    state.invalidate();
    const auto recovery = state.prepare(11, 5, 1516, 1516000, false);
    CHECK(recovery.reset && recovery.delta_ms == FrameTimeline::k_default_delta_ms && state.generation() == 9);
    state.commit(recovery);
    state.relinquish();
    state.commit(repeat); // 迟到的旧结果无法恢复这条刚交出的历史。
    CHECK(state.generation() == 11 && state.last_write_tick() == 0 && !state.same_game_frame(5));
    CHECK(state.prepare(12, 0, 1517, 1517000, false).reset);
    std::puts("Timeline: commit, same-frame dt, gaps, stale generation, invalidate and handoff passed");
    return 0;
}
