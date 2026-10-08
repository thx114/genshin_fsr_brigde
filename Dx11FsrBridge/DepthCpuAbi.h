#pragma once
#include <cstdint>
// Immutable packed R32_FLOAT snapshot. Version 1; x64 ABI, 64 bytes.
// flags bit 0: reverse Z; coordinates retain the game's source orientation.
struct BridgeDepthCpuFrame {
    uint32_t struct_size, abi_version, width, height, row_pitch, format, flags, reserved;
    uint64_t frame_id, capture_tick_ms;
    const float *data;
    void *token; // Provider-owned reference; only ReleaseDepthCpuFrame may release it.
};
static_assert(sizeof(BridgeDepthCpuFrame) == 64, "Depth CPU ABI requires x64");
