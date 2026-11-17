#include "FrameAttribution.h"
#include "../Fsr2FamilyTakeover.h"

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
    const Entry *output_match = nullptr, *source_match = nullptr, *possible = nullptr, *latest = nullptr;
    const auto consider = [](const Entry *&best, const Entry &candidate)
    {
        if (!best || candidate.pending.generation > best->pending.generation) best = &candidate;
    };
    Attribution result;
    bool shared_output = false;
    for (const auto &evidence : candidates)
    {
        const auto &capture = *evidence.capture;
        if (!capture.dimensions_match) continue;
        if (evidence.owns_target)
        {
            if (!result.unique_output_owner) result.unique_output_owner = capture.instance;
            else if (result.unique_output_owner != capture.instance) shared_output = true;
        }
        if (!capture.pending.generation) continue;
        if (evidence.owns_target) consider(output_match, capture);
        if (evidence.live) ++result.live_candidates;
        else ++result.dormant_candidates;
        consider(latest, capture);
        if (!color) continue;
        if (evidence.previous_color == color) consider(source_match, capture);
        else if (!fsr2_family_takeover::p1_exclude_candidate_by_source(evidence.previous_color != 0, false,
                     recently_observed(evidence.last_write_tick, now, source_memory_ms)))
            consider(possible, capture);
    }
    // 两实例曾写同一纹理时仍可用新 token，但历史关联本身不足以授权无 token 的修复。
    if (shared_output) result.unique_output_owner = 0;
    if (output_match)
    {
        result.instance = output_match->instance;
        result.token = output_match->pending;
        result.path = AttributionPath::OutputWithToken;
        result.live_candidates = result.dormant_candidates = 0;
    }
    else if (latest)
    {
        // 全部来源记忆冲突时保留代次兜底；下游的多义拒绝与接管裁决仍独立执行。
        const auto &chosen = *(source_match ? source_match : possible ? possible : latest);
        result.instance = chosen.instance;
        result.token = chosen.pending;
        result.path = AttributionPath::Bootstrap;
    }
    else if (result.unique_output_owner)
    {
        result.instance = result.unique_output_owner;
        result.path = AttributionPath::RepairOnly;
    }
    return result;
}
}
