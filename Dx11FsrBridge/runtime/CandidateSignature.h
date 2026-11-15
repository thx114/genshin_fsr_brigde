#pragma once

#include <dxgiformat.h>
#include <array>
#include <climits>
#include <cstdint>
#include <optional>
#include <span>

namespace neo::graphics
{
struct ResourceInfo
{
    std::uint64_t resource_key = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t bind_flags = 0;
    std::uint32_t misc_flags = 0;
};

struct TargetUpscalerDrawInfo
{
    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint64_t constant_buffer_key = 0;
    std::uint64_t color_resource_key = 0;
    std::uint64_t motion_resource_key = 0;
    std::uint32_t color_srv_slot = 0;
    std::uint32_t depth_srv_slot = 2;
    std::uint32_t motion_srv_slot = 3;
    std::uint32_t transparency_srv_slot = UINT_MAX;
    std::uint32_t output_rtv_slot = 1;
};

constexpr std::uint32_t k_min_fsr_constants = 464;
using StandardInputs = std::array<ResourceInfo, 7>;
using StandardTargets = std::array<ResourceInfo, 2>;

struct CandidateGeometry
{
    std::uint32_t viewport_width;
    std::uint32_t viewport_height;
    std::uint32_t cb0_bytes;
    std::uint64_t cb0_key;
};

struct StandardEvidence
{
    bool resources_present;
    bool input_sizes_match;
    bool target_sizes_match;
};

bool fsr_feature_color(const ResourceInfo &info);
std::optional<TargetUpscalerDrawInfo> identify_standard_candidate(const StandardInputs &srvs,
    const StandardTargets &rtvs, CandidateGeometry geometry, StandardEvidence &evidence);
std::optional<TargetUpscalerDrawInfo> identify_feature_candidate(std::span<const ResourceInfo> srvs,
    std::span<const ResourceInfo> rtvs, CandidateGeometry geometry);
}
