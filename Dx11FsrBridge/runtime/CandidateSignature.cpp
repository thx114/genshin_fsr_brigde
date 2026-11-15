#include "CandidateSignature.h"

namespace neo::graphics
{
namespace
{
bool same_size(const ResourceInfo &a, const ResourceInfo &b)
{
    return a.width == b.width && a.height == b.height;
}
}

std::optional<TargetUpscalerDrawInfo> identify_standard_candidate(const StandardInputs &srvs,
    const StandardTargets &rtvs, CandidateGeometry geometry, StandardEvidence &evidence)
{
    // 旧版国际服允许这组经过验证的形状使用不同 guide 格式，因此不能用通用格式表代替。
    const auto &[color, weights, depth, motion, flags, history_metadata, history_color] = srvs;
    const auto &[output_metadata, output_color] = rtvs;
    const auto [viewport_width, viewport_height, cb0_bytes, cb0_key] = geometry;
    evidence = {
        color.resource_key && weights.resource_key && depth.resource_key && motion.resource_key &&
            flags.resource_key && history_metadata.resource_key && history_color.resource_key &&
            output_metadata.resource_key && output_color.resource_key,
        same_size(depth, color) && same_size(motion, color) && same_size(flags, color),
        same_size(output_metadata, output_color) && same_size(history_metadata, output_color) &&
            same_size(history_color, output_color) && viewport_width && viewport_height &&
            output_color.width == viewport_width && output_color.height == viewport_height,
    };
    if (!evidence.resources_present || !evidence.input_sizes_match || !evidence.target_sizes_match ||
        weights.width != 16 || weights.height != 16 || color.width > output_color.width ||
        color.height > output_color.height || cb0_bytes < k_min_fsr_constants)
        return std::nullopt;

    return TargetUpscalerDrawInfo { color.width, color.height, output_color.width, output_color.height,
        cb0_key, color.resource_key, motion.resource_key };
}

static DXGI_FORMAT resource_view_or_format(const ResourceInfo &info)
{
    return info.view_format != DXGI_FORMAT_UNKNOWN ? info.view_format : info.format;
}

static bool fsr_feature_depth(const ResourceInfo &info)
{
    switch (resource_view_or_format(info))
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return true;
    default:
        return info.format == DXGI_FORMAT_R32G8X24_TYPELESS;
    }
}

bool fsr_feature_color(const ResourceInfo &info)
{
    switch (resource_view_or_format(info))
    {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return true;
    default:
        return false;
    }
}

static bool fsr_feature_motion(const ResourceInfo &info)
{
    switch (resource_view_or_format(info))
    {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM: // Genshin's square-encoded MV surface
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return true;
    default:
        return false;
    }
}

static bool fsr_feature_transparency(const ResourceInfo &info)
{
    switch (resource_view_or_format(info))
    {
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
        return true;
    default:
        return false;
    }
}

std::optional<TargetUpscalerDrawInfo> identify_feature_candidate(std::span<const ResourceInfo> srvs,
    std::span<const ResourceInfo> rtvs, CandidateGeometry geometry)
{
    const auto [viewport_width, viewport_height, cb0_bytes, cb0_key] = geometry;
    if (cb0_bytes < k_min_fsr_constants || viewport_width == 0 || viewport_height == 0)
        return std::nullopt;

    int output_slot = -1;
    int output_score = -1;
    for (std::size_t i = 0; i < rtvs.size(); ++i)
    {
        const ResourceInfo &candidate = rtvs[i];
        if (candidate.resource_key == 0 || candidate.width != viewport_width ||
            candidate.height != viewport_height || !fsr_feature_color(candidate))
            continue;
        int score = 1;
        const DXGI_FORMAT format = resource_view_or_format(candidate);
        if (format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R11G11B10_FLOAT ||
            format == DXGI_FORMAT_R16G16B16A16_FLOAT)
            score += 4;
        if (score > output_score)
        {
            output_slot = static_cast<int>(i);
            output_score = score;
        }
    }
    if (output_slot < 0)
        return std::nullopt;
    const ResourceInfo &output = rtvs[output_slot];

    int weight_count = 0;
    int history_count = 0;
    for (const ResourceInfo &srv : srvs)
    {
        weight_count += srv.resource_key != 0 && srv.width == 16 && srv.height == 16 ? 1 : 0;
        history_count += srv.resource_key != 0 && srv.width == output.width && srv.height == output.height ? 1 : 0;
    }
    if (weight_count == 0 || history_count < 2)
        return std::nullopt;

    int depth_slot = -1;
    for (std::size_t i = 0; i < srvs.size(); ++i)
    {
        if (srvs[i].resource_key != 0 && fsr_feature_depth(srvs[i]))
        {
            depth_slot = static_cast<int>(i);
            break;
        }
    }
    if (depth_slot < 0)
        return std::nullopt;
    const ResourceInfo &depth = srvs[depth_slot];

    int color_slot = -1, motion_slot = -1, transparency_slot = -1;
    int color_score = -1, motion_score = -1, transparency_score = -1;
    for (std::size_t i = 0; i < srvs.size(); ++i)
    {
        const ResourceInfo &candidate = srvs[i];
        if (candidate.resource_key == 0 || candidate.width != depth.width ||
            candidate.height != depth.height || static_cast<int>(i) == depth_slot ||
            (candidate.width == 16 && candidate.height == 16))
            continue;
        if (fsr_feature_color(candidate))
        {
            int score = 1;
            const DXGI_FORMAT format = resource_view_or_format(candidate);
            if (format == DXGI_FORMAT_R11G11B10_FLOAT || format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                score += 4;
            if (i == 0) score += 2; // known layout is only a tie-breaker, never a requirement
            if (score > color_score) { color_score = score; color_slot = static_cast<int>(i); }
        }
        if (fsr_feature_motion(candidate))
        {
            int score = 1;
            const DXGI_FORMAT format = resource_view_or_format(candidate);
            if (format == DXGI_FORMAT_R16G16_FLOAT || format == DXGI_FORMAT_R16G16_SNORM ||
                format == DXGI_FORMAT_R32G32_FLOAT) score += 4;
            if (i == 3) score += 2; // tie-breaker for the observed game layout
            if (score > motion_score) { motion_score = score; motion_slot = static_cast<int>(i); }
        }
        if (fsr_feature_transparency(candidate))
        {
            int score = 1;
            if (i == 4) score += 2; // observed layout only breaks ties
            if (score > transparency_score)
            {
                transparency_score = score;
                transparency_slot = static_cast<int>(i);
            }
        }
    }
    if (color_slot < 0 || motion_slot < 0 || color_slot == motion_slot)
        return std::nullopt;
    const ResourceInfo &color = srvs[color_slot];
    if (color.width > output.width || color.height > output.height)
        return std::nullopt;

    return TargetUpscalerDrawInfo {
        color.width, color.height, output.width, output.height, cb0_key,
        color.resource_key, srvs[motion_slot].resource_key,
        static_cast<std::uint32_t>(color_slot), static_cast<std::uint32_t>(depth_slot),
        static_cast<std::uint32_t>(motion_slot),
        transparency_slot >= 0 ? static_cast<std::uint32_t>(transparency_slot) : UINT_MAX,
        static_cast<std::uint32_t>(output_slot)};
}

}
