#include "CandidateSignature.h"

#include <algorithm>

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

    const TargetUpscalerDrawInfo layout { color.width, color.height, output_color.width, output_color.height,
        cb0_key, color.resource_key, motion.resource_key };
    return validate_accumulation_candidate(srvs, rtvs, geometry, layout) ? std::optional(layout) : std::nullopt;
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

static DXGI_FORMAT format_family(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_SNORM:
        return DXGI_FORMAT_R8G8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:
        return DXGI_FORMAT_R16G16_TYPELESS;
    default:
        return format;
    }
}

static bool history_matches(const ResourceInfo &history, const ResourceInfo &output)
{
    return history.resource_key && same_size(history, output) &&
        format_family(resource_view_or_format(history)) == format_family(resource_view_or_format(output));
}

static bool fsr_feature_weights(const ResourceInfo &info)
{
    if (info.width != 16 || info.height != 16) return false;
    switch (resource_view_or_format(info))
    {
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        return true;
    default:
        return false;
    }
}

bool validate_accumulation_candidate(std::span<const ResourceInfo> srvs, std::span<const ResourceInfo> rtvs,
    CandidateGeometry geometry, const TargetUpscalerDrawInfo &layout)
{
    const std::array slots {layout.color_srv_slot, layout.weights_srv_slot, layout.depth_srv_slot,
        layout.motion_srv_slot, layout.history_metadata_srv_slot, layout.history_color_srv_slot};
    if (geometry.cb0_bytes < k_min_fsr_constants || !geometry.viewport_width || !geometry.viewport_height ||
        layout.output_rtv_slot >= rtvs.size() || layout.metadata_rtv_slot >= rtvs.size() ||
        layout.output_rtv_slot == layout.metadata_rtv_slot)
        return false;
    std::array<std::uint64_t, 9> keys {};
    std::size_t count = 0;
    const auto distinct = [&](std::uint64_t key)
    {
        if (!key || std::find(keys.begin(), keys.begin() + count, key) != keys.begin() + count) return false;
        keys[count++] = key;
        return true;
    };
    for (const auto slot : slots)
        if (slot >= srvs.size() || !distinct(srvs[slot].resource_key)) return false;
    const auto &color = srvs[layout.color_srv_slot], &weights = srvs[layout.weights_srv_slot];
    const auto &depth = srvs[layout.depth_srv_slot], &motion = srvs[layout.motion_srv_slot];
    const auto &history_metadata = srvs[layout.history_metadata_srv_slot];
    const auto &history_color = srvs[layout.history_color_srv_slot];
    const auto &output = rtvs[layout.output_rtv_slot], &metadata = rtvs[layout.metadata_rtv_slot];
    if (!distinct(output.resource_key) || !distinct(metadata.resource_key) || !color.width || !color.height ||
        !same_size(color, depth) || !same_size(color, motion) || !same_size(output, metadata) ||
        !same_size(output, history_metadata) || !same_size(output, history_color) ||
        output.width != geometry.viewport_width || output.height != geometry.viewport_height ||
        color.width > output.width || color.height > output.height || weights.width != 16 || weights.height != 16)
        return false;
    const auto flags_slot = layout.standard_layout ? 4u : layout.transparency_srv_slot;
    if (flags_slot != UINT_MAX)
    {
        if (flags_slot >= srvs.size()) return false;
        const auto &flags = srvs[flags_slot];
        if (!same_size(color, flags) || !distinct(flags.resource_key)) return false;
    }
    // 2026-11-16（PID44888）：固定布局保留国际服已验证的 guide 格式；重排布局必须有独立历史对。
    // 这只验证绑定关系，不认证 shader 的 temporal 算法或颜色传递函数。
    return layout.standard_layout || (fsr_feature_weights(weights) && fsr_feature_color(color) &&
        fsr_feature_depth(depth) && fsr_feature_motion(motion) && fsr_feature_color(output) &&
        history_matches(history_metadata, metadata) && history_matches(history_color, output));
}

namespace
{
enum Role : unsigned { Color, Motion, Depth, Weights, HistoryMetadata, HistoryColor, RoleCount };
// D3D11 的 PS 输入最多 128 槽；匹配每个角色最多遍历这张固定表，不枚举资源笛卡尔积。
constexpr std::size_t k_input_slots = 128;
struct RoleAssignment
{
    std::span<const ResourceInfo> inputs;
    std::array<std::array<signed char, k_input_slots>, RoleCount> scores {};
    std::array<std::uint32_t, RoleCount> slots {UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX};

    bool assign(unsigned role, unsigned &visited)
    {
        if (visited & (1u << role)) return false;
        visited |= 1u << role;
        // 先用空闲资源，只有缺角色时才重排已选项；1:1 且两历史同格式时不会抢走唯一 guide。
        for (unsigned occupied = 0; occupied < 2; ++occupied)
            for (int score = 7; score >= 0; --score)
                for (std::size_t i = 0; i < inputs.size(); ++i)
                {
                    if (scores[role][i] != score) continue;
                    unsigned owner = RoleCount;
                    for (unsigned j = 0; j < RoleCount; ++j)
                        if (slots[j] != UINT_MAX && inputs[slots[j]].resource_key == inputs[i].resource_key)
                            owner = j;
                    if (occupied != static_cast<unsigned>(owner != RoleCount)) continue;
                    if (owner != RoleCount && !assign(owner, visited)) continue;
                    slots[role] = static_cast<std::uint32_t>(i);
                    return true;
                }
        return false;
    }
};
}

std::optional<TargetUpscalerDrawInfo> identify_feature_candidate(std::span<const ResourceInfo> srvs,
    std::span<const ResourceInfo> rtvs, CandidateGeometry geometry)
{
    const auto [viewport_width, viewport_height, cb0_bytes, cb0_key] = geometry;
    if (cb0_bytes < k_min_fsr_constants || viewport_width == 0 || viewport_height == 0)
        return std::nullopt;

    srvs = srvs.first(std::min(srvs.size(), k_input_slots));
    rtvs = rtvs.first(std::min<std::size_t>(rtvs.size(), 8));
    std::array<const ResourceInfo *, k_input_slots> render_extents {};
    std::size_t extent_count = 0;
    for (const auto &input : srvs)
    {
        if (!input.resource_key || !input.width || !input.height || !fsr_feature_depth(input)) continue;
        if (std::any_of(render_extents.begin(), render_extents.begin() + extent_count,
            [&](const auto *extent) { return same_size(input, *extent); })) continue;
        render_extents[extent_count++] = &input;
    }

    std::optional<TargetUpscalerDrawInfo> best;
    int best_score = -1;
    for (std::size_t output_slot = 0; output_slot < rtvs.size(); ++output_slot)
    {
        const auto &output = rtvs[output_slot];
        if (!output.resource_key || output.width != viewport_width || output.height != viewport_height ||
            !fsr_feature_color(output))
            continue;
        for (std::size_t metadata_slot = 0; metadata_slot < rtvs.size(); ++metadata_slot)
        {
            const auto &metadata = rtvs[metadata_slot];
            if (!metadata.resource_key || metadata.resource_key == output.resource_key || !same_size(metadata, output))
                continue;
            for (const auto *extent : std::span(render_extents).first(extent_count))
            {
                const auto &depth = *extent;
                if (depth.width > output.width || depth.height > output.height) continue;
                RoleAssignment roles {srvs};
                for (auto &scores : roles.scores) scores.fill(-1);
                for (std::size_t i = 0; i < srvs.size(); ++i)
                {
                    const auto &input = srvs[i];
                    if (!input.resource_key || input.resource_key == output.resource_key ||
                        input.resource_key == metadata.resource_key) continue;
                    if (history_matches(input, metadata)) roles.scores[HistoryMetadata][i] = 1;
                    if (history_matches(input, output)) roles.scores[HistoryColor][i] = 1;
                    if (fsr_feature_weights(input)) roles.scores[Weights][i] = 1;
                    if (!same_size(input, depth) || (input.width == 16 && input.height == 16)) continue;
                    if (fsr_feature_depth(input)) roles.scores[Depth][i] = 1;
                    const auto format = resource_view_or_format(input);
                    if (fsr_feature_color(input))
                        roles.scores[Color][i] = static_cast<signed char>(1 + (i == 0 ? 2 : 0) +
                            (format == DXGI_FORMAT_R11G11B10_FLOAT || format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 4 : 0));
                    if (fsr_feature_motion(input))
                        roles.scores[Motion][i] = static_cast<signed char>(1 + (i == 3 ? 2 : 0) +
                            (format == DXGI_FORMAT_R16G16_FLOAT || format == DXGI_FORMAT_R16G16_SNORM ||
                                format == DXGI_FORMAT_R32G32_FLOAT ? 4 : 0));
                }
                bool complete = true;
                for (unsigned role = 0; role < RoleCount; ++role)
                {
                    unsigned visited = 0;
                    if (!roles.assign(role, visited)) { complete = false; break; }
                }
                if (!complete) continue;
                const auto &slots = roles.slots;
                const auto &color = srvs[slots[Color]];
                TargetUpscalerDrawInfo layout {color.width, color.height, output.width, output.height, cb0_key,
                    color.resource_key, srvs[slots[Motion]].resource_key, slots[Color], slots[Depth], slots[Motion],
                    UINT_MAX, static_cast<std::uint32_t>(output_slot), slots[Weights], slots[HistoryMetadata],
                    slots[HistoryColor], static_cast<std::uint32_t>(metadata_slot), false};
                for (std::size_t i = 0; i < srvs.size(); ++i)
                {
                    if (!same_size(srvs[i], color) || !fsr_feature_transparency(srvs[i])) continue;
                    layout.transparency_srv_slot = static_cast<std::uint32_t>(i);
                    if (validate_accumulation_candidate(srvs, rtvs, geometry, layout)) break;
                    layout.transparency_srv_slot = UINT_MAX;
                }
                if (!validate_accumulation_candidate(srvs, rtvs, geometry, layout)) continue;
                const auto format = resource_view_or_format(output);
                const int score = 1 + (format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                    format == DXGI_FORMAT_R11G11B10_FLOAT || format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 4 : 0);
                if (score > best_score) { best = layout; best_score = score; }
            }
        }
    }
    return best;
}

}
