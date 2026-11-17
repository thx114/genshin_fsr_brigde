#include "../runtime/CandidateSignature.h"

#include <cstdio>
#include <algorithm>

using namespace neo::graphics;

namespace
{
constexpr std::uint32_t k_display_w = 3840, k_display_h = 2160;
constexpr std::uint32_t k_render_w = 2304, k_render_h = 1296;
constexpr CandidateGeometry k_geometry { k_display_w, k_display_h, k_min_fsr_constants, 765 };

constexpr ResourceInfo render(std::uint64_t key, DXGI_FORMAT format)
{
    return { key, k_render_w, k_render_h, format };
}

constexpr ResourceInfo display(std::uint64_t key, DXGI_FORMAT format)
{
    return { key, k_display_w, k_display_h, format };
}
}

int main()
{
    unsigned failures = 0;
    const auto check = [&failures](bool ok, const char *what)
    {
        if (!ok)
        {
            ++failures;
            std::printf("FAIL: %s\n", what);
        }
    };
    const auto weights = ResourceInfo { 2, 16, 16, DXGI_FORMAT_R8_UNORM };
    const auto metadata = display(8, DXGI_FORMAT_R8G8B8A8_UNORM);
    const auto output = display(9, DXGI_FORMAT_R16G16B16A16_FLOAT);
    const StandardInputs fixed {
        render(1, DXGI_FORMAT_R8G8B8A8_UNORM), weights, render(3, DXGI_FORMAT_R8G8B8A8_UNORM),
        render(4, DXGI_FORMAT_R8G8B8A8_UNORM), render(5, DXGI_FORMAT_R8_UNORM),
        display(6, DXGI_FORMAT_R8G8B8A8_UNORM), display(7, DXGI_FORMAT_R16G16B16A16_FLOAT),
    };
    StandardEvidence evidence {};
    const StandardTargets targets { metadata, output };
    const auto standard = identify_standard_candidate(fixed, targets, k_geometry, evidence);
    check(standard && standard->constant_buffer_key == k_geometry.cb0_key &&
        standard->render_width == k_render_w && standard->output_height == k_display_h &&
        standard->color_srv_slot == 0 && standard->depth_srv_slot == 2 && standard->motion_srv_slot == 3 &&
        standard->motion_resource_key == 4 && standard->output_rtv_slot == 1,
        "legacy fixed signature accepts its original guide formats and proven dimensions");
    check(!identify_feature_candidate(fixed, targets, k_geometry), "legacy fallback is not a feature-format match");

    auto invalid_fixed = fixed;
    invalid_fixed[6].resource_key = 0;
    check(!identify_standard_candidate(invalid_fixed, targets, k_geometry, evidence) && !evidence.resources_present,
        "missing fixed-layout history rejected");
    invalid_fixed = fixed;
    --invalid_fixed[2].width;
    check(!identify_standard_candidate(invalid_fixed, targets, k_geometry, evidence) && !evidence.input_sizes_match,
        "mismatched fixed-layout guide rejected");
    auto invalid_geometry = k_geometry;
    invalid_geometry.viewport_width = 0;
    check(!identify_standard_candidate(fixed, targets, invalid_geometry, evidence) && !evidence.target_sizes_match,
        "no viewport cannot match fixed layout");

    const StandardInputs reordered {
        weights, render(4, DXGI_FORMAT_R16G16_FLOAT), display(6, DXGI_FORMAT_R8G8B8A8_UNORM),
        render(1, DXGI_FORMAT_R11G11B10_FLOAT), render(5, DXGI_FORMAT_R8_UNORM),
        display(7, DXGI_FORMAT_R16G16B16A16_FLOAT),
        { 3, k_render_w, k_render_h, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT },
    };
    const StandardTargets swapped_targets { output, metadata };
    const auto adaptive = identify_feature_candidate(reordered, swapped_targets, k_geometry);
    check(adaptive && adaptive->color_srv_slot == 3 && adaptive->depth_srv_slot == 6 &&
        adaptive->motion_srv_slot == 1 && adaptive->transparency_srv_slot == 4 && adaptive->output_rtv_slot == 0 &&
        adaptive->color_resource_key == 1 && adaptive->motion_resource_key == 4,
        "reordered guides and output resolved from typed view formats, not hard-coded slots");
    check(!identify_standard_candidate(reordered, swapped_targets, k_geometry, evidence),
        "reordered guides need the adaptive signature");
    invalid_geometry = k_geometry;
    --invalid_geometry.cb0_bytes;
    check(!identify_feature_candidate(reordered, swapped_targets, invalid_geometry) &&
        !identify_standard_candidate(fixed, targets, invalid_geometry, evidence), "short constants rejected by both");
    invalid_geometry = k_geometry;
    --invalid_geometry.viewport_height;
    check(!identify_feature_candidate(reordered, swapped_targets, invalid_geometry), "mismatched viewport rejected");
    auto invalid_inputs = reordered;
    invalid_inputs[1].view_format = DXGI_FORMAT_R8_UINT;
    check(!identify_feature_candidate(invalid_inputs, swapped_targets, k_geometry), "unknown motion view rejected");
    invalid_inputs = reordered;
    invalid_inputs[5] = {};
    check(!identify_feature_candidate(invalid_inputs, swapped_targets, k_geometry), "missing adaptive history rejected");
    invalid_inputs = reordered;
    invalid_inputs[1] = {};
    invalid_inputs[3].format = DXGI_FORMAT_R10G10B10A2_UNORM;
    check(!identify_feature_candidate(invalid_inputs, swapped_targets, k_geometry), "one surface cannot fill two roles");

    // 2026-11-16（PID44888）：同尺寸 G-buffer 不能充当历史；packed MV 也不能替照明 pass 补齐证据。
    const CandidateGeometry lighting_geometry {1536, 960, 3184, 77};
    const StandardInputs lighting {
        ResourceInfo {11, 1536, 960, DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS},
        ResourceInfo {12, 1536, 960, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM},
        ResourceInfo {13, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB},
        ResourceInfo {14, 16, 16, DXGI_FORMAT_R16G16B16A16_FLOAT},
        ResourceInfo {15, 1536, 960, DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM},
        ResourceInfo {16, 1536, 960, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB},
        ResourceInfo {17, 1536, 960, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB},
    };
    const StandardTargets lighting_targets {ResourceInfo {18, 1536, 960, DXGI_FORMAT_R11G11B10_FLOAT}, {}};
    check(!identify_feature_candidate(lighting, lighting_targets, lighting_geometry),
        "captured lighting single RT and same-size G-buffer are not temporal history");
    auto packed_lighting = lighting;
    packed_lighting[1].format = packed_lighting[1].view_format = DXGI_FORMAT_R16G16_FLOAT;
    check(!identify_feature_candidate(packed_lighting, lighting_targets, lighting_geometry),
        "supported motion format cannot turn single-RT lighting into accumulation");
    auto fake_mrt = lighting_targets;
    fake_mrt[1] = {19, 1536, 960, DXGI_FORMAT_R8G8B8A8_UNORM};
    check(!identify_feature_candidate(packed_lighting, fake_mrt, lighting_geometry),
        "arbitrary 16x16 RGBA lighting data is not a weight LUT or matching history pair");

    auto aliased = fixed;
    aliased[6].resource_key = aliased[0].resource_key;
    check(!identify_standard_candidate(aliased, targets, k_geometry, evidence),
        "fixed layout cannot reuse current color as history");
    auto aliased_targets = swapped_targets;
    aliased_targets[1].resource_key = aliased_targets[0].resource_key;
    check(!identify_feature_candidate(reordered, aliased_targets, k_geometry), "MRT resources must be distinct");

    auto dlaa = reordered;
    for (auto &srv : dlaa)
        if (srv.width == k_render_w) { srv.width = k_display_w; srv.height = k_display_h; }
    const auto adaptive_dlaa = identify_feature_candidate(dlaa, swapped_targets, k_geometry);
    check(adaptive_dlaa && adaptive_dlaa->render_width == adaptive_dlaa->output_width,
        "1:1 accumulation keeps independent history and output roles");
    auto same_family = dlaa;
    same_family[3].format = same_family[5].format = DXGI_FORMAT_R8G8B8A8_UNORM;
    const StandardTargets same_family_targets {display(9, DXGI_FORMAT_R8G8B8A8_UNORM), metadata};
    check(identify_feature_candidate(same_family, same_family_targets, k_geometry).has_value(),
        "history and current color sharing a format family must still admit a complete assignment");
    auto float_weights = reordered;
    float_weights[0].format = DXGI_FORMAT_R32_FLOAT;
    const auto lut_first = identify_feature_candidate(float_weights, swapped_targets, k_geometry);
    check(lut_first && lut_first->weights_srv_slot == 0 && lut_first->depth_srv_slot == 6,
        "R32_FLOAT weight LUT before real depth does not become the guide");
    std::array<ResourceInfo, 8> extra_depth {};
    extra_depth[0] = {20, 320, 200, DXGI_FORMAT_R32_FLOAT};
    std::copy(reordered.begin(), reordered.end(), extra_depth.begin() + 1);
    const auto later_depth = identify_feature_candidate(extra_depth, swapped_targets, k_geometry);
    check(later_depth && later_depth->depth_srv_slot == 7 && later_depth->color_srv_slot == 4,
        "unrelated depth before a complete accumulation does not mask the real extent");
    std::printf("CandidateSignature: %u failures\n", failures);
    return failures != 0;
}
