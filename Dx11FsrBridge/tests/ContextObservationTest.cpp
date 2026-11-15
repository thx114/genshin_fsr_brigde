#include "../runtime/ContextObservation.h"

#include <cstdio>

using neo::graphics::context_observation;

int main()
{
    constexpr auto on_demand = context_observation(true, true, false, false);
    constexpr auto mirrored = context_observation(true, false, false, false);
    constexpr auto texture = context_observation(true, true, true, false);
    constexpr auto uav = context_observation(true, true, false, true);
    constexpr auto development = context_observation(false, true, false, false);
    static_assert(!on_demand.mirror_state && !on_demand.track_targets && !on_demand.track_uavs);
    static_assert(mirrored.mirror_state && mirrored.track_targets);
    static_assert(!texture.mirror_state && texture.track_targets);
    static_assert(!uav.mirror_state && uav.track_uavs);
    static_assert(development.mirror_state && development.track_targets);
    std::puts("ContextObservation: on-demand, mirrored, texture/UAV and development plans verified");
}
