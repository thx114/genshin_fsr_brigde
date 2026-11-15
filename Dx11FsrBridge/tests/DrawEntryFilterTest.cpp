#include "../PerfProbe.h"
#include <array>
#include <thread>

int main()
{
    using namespace perf_probe;
    configure_draw_entry_filter(true, 64);
    std::array<unsigned, 4> hits {};
    std::array<std::thread, 4> threads;
    // 每线程都不足一次整批的尾部也要记进全局序号。
    for (unsigned t = 0; t < threads.size(); ++t)
        threads[t] = std::thread([&, t] {
            for (unsigned i = 0; i < 5001; ++i) hits[t] += draw_entry_filter_admit(6).canary;
        });
    for (auto &thread : threads) thread.join();
    unsigned admitted = 0;
    for (auto n : hits) admitted += n;
    if (admitted != 20004 / 64) return 1;
    configure_draw_entry_filter(true, 0);
    if (draw_entry_filter_admit(6).admitted) return 2;
    configure_draw_entry_filter(true, 1);
    if (!draw_entry_filter_admit(6).canary) return 3;
    configure_draw_entry_filter(true, 5);
    if (!draw_entry_filter_admit(6).canary) return 4;
    auto totals = read_draw_entry_filter();
    if (totals.skipped != 20004 - admitted + 1 || totals.canary != admitted + 2) return 5;
    note_draw_identified_element_count(6, DrawEntryKind::indexed, true);
    auto released = draw_entry_filter_admit(6);
    return draw_entry_filter_disabled() && released.admitted && !released.canary ? 0 : 6;
}
