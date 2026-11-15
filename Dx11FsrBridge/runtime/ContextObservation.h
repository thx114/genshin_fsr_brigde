#pragma once

namespace neo::graphics
{
struct ContextObservation
{
    bool mirror_state;
    bool track_targets;
    bool track_uavs;
};

constexpr ContextObservation context_observation(bool release, bool reads_on_demand,
    bool texture_hook, bool on12_target_tracking)
{
    const bool mirror = !release || !reads_on_demand;
    return { mirror, mirror || texture_hook, on12_target_tracking };
}
}
