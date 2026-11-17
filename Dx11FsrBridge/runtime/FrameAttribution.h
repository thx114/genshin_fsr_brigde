#pragma once

#include "../Il2CppCallSiteHook.h"

#include <span>

namespace neo::graphics
{
enum class AttributionPath : std::uint32_t
{
    None, OutputWithToken, Bootstrap, RepairOnly, Ambiguous,
};

struct FrameEvidence
{
    const il2cpp_callsite::RenderSnapshot::Entry *capture;
    std::uint64_t previous_color;
    std::uint64_t last_write_tick;
    bool live;
    bool owns_target;
};

struct Attribution
{
    std::uint64_t instance = 0;
    std::uint64_t unique_output_owner = 0;
    il2cpp_callsite::RenderToken token {};
    AttributionPath path = AttributionPath::None;
    std::uint32_t live_candidates = 0;
    std::uint32_t dormant_candidates = 0;
};

bool recently_observed(std::uint64_t observation, std::uint64_t now, std::uint64_t max_age_ms);
Attribution attribute_frame(std::span<const FrameEvidence> candidates, std::uint64_t color,
    std::uint64_t now, std::uint64_t source_memory_ms);
}
