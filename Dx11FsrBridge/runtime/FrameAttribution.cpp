#include "FrameAttribution.h"

namespace neo::graphics
{
bool recently_observed(std::uint64_t observation, std::uint64_t now, std::uint64_t max_age_ms)
{
    return observation && observation <= now && now - observation <= max_age_ms;
}

Attribution attribute_frame(std::span<const FrameEvidence> candidates, std::uint64_t color,
    std::uint64_t now, std::uint64_t source_memory_ms)
{
    using Entry = il2cpp_callsite::RenderSnapshot::Entry;
    struct Matches
    {
        const Entry *entry = nullptr;
        std::uint32_t count = 0;

        void add(const Entry &candidate)
        {
            entry = &candidate;
            ++count;
        }
    };
    Matches owners, outputs, sources, intersection, pending;
    Attribution result;
    for (const auto &evidence : candidates)
    {
        const auto &capture = *evidence.capture;
        if (!capture.dimensions_match) continue;
        if (evidence.owns_target) owners.add(capture);
        if (!capture.pending.generation) continue;
        pending.add(capture);
        const bool source_fresh = recently_observed(evidence.last_write_tick, now, source_memory_ms);
        const bool source_matches = color && evidence.previous_color == color && source_fresh;
        if (evidence.owns_target) outputs.add(capture);
        if (source_matches) sources.add(capture);
        if (evidence.owns_target && source_matches) intersection.add(capture);
        if (evidence.live) ++result.live_candidates;
        else ++result.dormant_candidates;
    }
    if (owners.count == 1) result.unique_output_owner = owners.entry->instance;

    // 同一输出可以被相机依次复用；无 token 的旧 owner 只参与重复输出修复，不否决真实新 Render。
    if (!pending.count)
    {
        if (result.unique_output_owner)
        {
            result.instance = result.unique_output_owner;
            result.path = AttributionPath::RepairOnly;
        }
        else if (owners.count) result.path = AttributionPath::Ambiguous;
        return result;
    }

    // 2026-11-17：冷启动同尺寸正序宿主把 A 归入 B，generation 仅表示提交先后，不能证明机位。
    // 先合并正面资源证据；共享或冲突时必须唯一相交，不能由较新的 token 覆盖另一机位的归属。
    const Entry *chosen = nullptr;
    if (outputs.count && sources.count)
    {
        if (intersection.count == 1) chosen = intersection.entry;
    }
    else if (outputs.count == 1) chosen = outputs.entry;
    else if (sources.count == 1) chosen = sources.entry;
    else if (!outputs.count && !sources.count)
    {
        // 换源/双缓冲时旧颜色不同不能排除该机位；没有正面归属时只接受唯一匹配 token。
        if (pending.count == 1) chosen = pending.entry;
    }

    if (!chosen)
    {
        if (outputs.count || sources.count || result.live_candidates || result.dormant_candidates)
            result.path = AttributionPath::Ambiguous;
        return result;
    }
    result.instance = chosen->instance;
    result.token = chosen->pending;
    const bool owns_output = (outputs.count == 1 && outputs.entry == chosen) ||
        (intersection.count == 1 && intersection.entry == chosen);
    result.path = owns_output ? AttributionPath::OutputWithToken : AttributionPath::Bootstrap;
    if (owns_output) result.live_candidates = result.dormant_candidates = 0;
    return result;
}
}
