#pragma once
#include <vector>
#include <utility>
#include <algorithm>
#include <cstddef>

namespace skinprobe {
// Shared by the actual device/context installers and CPU regression. A probe
// hook must not disappear behind the development-only tracing guard again.
inline bool append_probe_hook(std::vector<std::pair<std::size_t,void*>>& patches,
                              bool probeEnabled,std::size_t index,void* function) {
    if(!probeEnabled || !function)return false;
    if(std::any_of(patches.begin(),patches.end(),[&](const auto& p){return p.first==index;}))return false;
    patches.emplace_back(index,function);return true;
}
}
