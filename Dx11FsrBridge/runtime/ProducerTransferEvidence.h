#pragma once

#include "../interop/BeforeSrV1.h"
#include <d3d11.h>
#include <array>
#include <cstdint>
#include <type_traits>

namespace neo::producer
{
// 仅保存同步认证已读取的数值，不持有COM对象，不把CPU返回冒充GPU完成。
enum class DrawReason : std::uint32_t
{
    not_evaluated,
    disabled,
    context_untracked,
    draw_count,
    target_untracked,
    context_deferred,
    target_view,
    pixel_program,
    cb_missing,
    cb_range,
    cb_shadow_missing,
    cb_mapped,
    cb_unknown,
    cb_epoch,
    cb_controls_nonfinite,
    cb_controls,
    viewport,
    rasterizer,
    rasterizer_extended,
    scissor,
    depth_stencil,
    predication,
    extra_shader_stage,
    vertex_program,
    layout_missing,
    layout_untracked,
    topology,
    vertex_buffer,
    vertex_stride,
    index_buffer,
    index_format,
    geometry_shadow_missing,
    geometry_mapped,
    geometry_epoch,
    geometry_range,
    geometry_unknown,
    geometry_version,
    geometry_nonfinite,
    geometry_degenerate,
    geometry_coverage,
    sample_mask,
    blend,
    logic_op,
    awaiting_return,
    return_geometry_mapped,
    return_geometry_version,
    return_serial,
    return_epoch,
    return_cb_version,
    return_cb_mapped,
    return_cb_unknown,
    certified
};
enum class ConsumeReason : std::uint32_t
{
    not_evaluated,
    disabled,
    source_untracked,
    context_untracked,
    uncertified,
    owner_mismatch,
    epoch_mismatch,
    accepted
};
enum class DrawGate : std::uint32_t
{
    enabled = 1u << 0, execution = 1u << 1, target = 1u << 2, pixel = 1u << 3,
    controls = 1u << 4, coverage = 1u << 5, geometry = 1u << 6, blend = 1u << 7,
    returned = 1u << 8
};

inline const char *reason_name(DrawReason reason) noexcept
{
    switch (reason)
    {
    case DrawReason::not_evaluated: return "not_evaluated";
    case DrawReason::disabled: return "disabled";
    case DrawReason::context_untracked: return "context_untracked";
    case DrawReason::draw_count: return "draw_count";
    case DrawReason::target_untracked: return "target_untracked";
    case DrawReason::context_deferred: return "context_deferred";
    case DrawReason::target_view: return "target_view";
    case DrawReason::pixel_program: return "pixel_program";
    case DrawReason::cb_missing: return "cb_missing";
    case DrawReason::cb_range: return "cb_range";
    case DrawReason::cb_shadow_missing: return "cb_shadow_missing";
    case DrawReason::cb_mapped: return "cb_mapped";
    case DrawReason::cb_unknown: return "cb_unknown";
    case DrawReason::cb_epoch: return "cb_epoch";
    case DrawReason::cb_controls_nonfinite: return "cb_controls_nonfinite";
    case DrawReason::cb_controls: return "cb_controls";
    case DrawReason::viewport: return "viewport";
    case DrawReason::rasterizer: return "rasterizer";
    case DrawReason::rasterizer_extended: return "rasterizer_extended";
    case DrawReason::scissor: return "scissor";
    case DrawReason::depth_stencil: return "depth_stencil";
    case DrawReason::predication: return "predication";
    case DrawReason::extra_shader_stage: return "extra_shader_stage";
    case DrawReason::vertex_program: return "vertex_program";
    case DrawReason::layout_missing: return "layout_missing";
    case DrawReason::layout_untracked: return "layout_untracked";
    case DrawReason::topology: return "topology";
    case DrawReason::vertex_buffer: return "vertex_buffer";
    case DrawReason::vertex_stride: return "vertex_stride";
    case DrawReason::index_buffer: return "index_buffer";
    case DrawReason::index_format: return "index_format";
    case DrawReason::geometry_shadow_missing: return "geometry_shadow_missing";
    case DrawReason::geometry_mapped: return "geometry_mapped";
    case DrawReason::geometry_epoch: return "geometry_epoch";
    case DrawReason::geometry_range: return "geometry_range";
    case DrawReason::geometry_unknown: return "geometry_unknown";
    case DrawReason::geometry_version: return "geometry_version";
    case DrawReason::geometry_nonfinite: return "geometry_nonfinite";
    case DrawReason::geometry_degenerate: return "geometry_degenerate";
    case DrawReason::geometry_coverage: return "geometry_coverage";
    case DrawReason::sample_mask: return "sample_mask";
    case DrawReason::blend: return "blend";
    case DrawReason::logic_op: return "logic_op";
    case DrawReason::awaiting_return: return "awaiting_return";
    case DrawReason::return_geometry_mapped: return "return_geometry_mapped";
    case DrawReason::return_geometry_version: return "return_geometry_version";
    case DrawReason::return_serial: return "return_serial";
    case DrawReason::return_epoch: return "return_epoch";
    case DrawReason::return_cb_version: return "return_cb_version";
    case DrawReason::return_cb_mapped: return "return_cb_mapped";
    case DrawReason::return_cb_unknown: return "return_cb_unknown";
    case DrawReason::certified: return "certified";
    }
    return "invalid";
}
inline const char *reason_name(ConsumeReason reason) noexcept
{
    switch (reason)
    {
    case ConsumeReason::not_evaluated: return "not_evaluated";
    case ConsumeReason::disabled: return "disabled";
    case ConsumeReason::source_untracked: return "source_untracked";
    case ConsumeReason::context_untracked: return "context_untracked";
    case ConsumeReason::uncertified: return "uncertified";
    case ConsumeReason::owner_mismatch: return "owner_mismatch";
    case ConsumeReason::epoch_mismatch: return "epoch_mismatch";
    case ConsumeReason::accepted: return "accepted";
    }
    return "invalid";
}

struct InputElementEvidence
{
    bool position0 = false;
    UINT semantic_index = 0, slot = 0, offset = 0, classification = 0, step = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
enum class LayoutReason : std::uint32_t
{
    not_observed, accepted, unsupported_element, offset_range, position_rate, missing_position
};
struct LayoutEvidence
{
    UINT count = 0, inspected = 0, rejected_element = UINT_MAX;
    LayoutReason reason = LayoutReason::not_observed;
    std::array<InputElementEvidence, D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT> elements {};
};
struct BufferEvidence
{
    std::uintptr_t resource = 0;
    D3D11_BUFFER_DESC desc {};
    bool desc_observed = false, shadow_present = false, mapped = false, immutable = false;
    UINT read_offset = 0, read_bytes = 0, reads_checked = 0, reads_passed = 0;
    std::uint64_t epoch = 0, version_begin = 0, version_return = 0;
};
struct ControlEvidence
{
    std::uintptr_t resource = 0;
    UINT first = 0, count = 0, known = 0;
    bool shadow_present = false, mapped = false;
    std::uint64_t epoch = 0, version_begin = 0, version_return = 0;
    std::array<float, 3> values {};
};
struct DrawEvidence
{
    DrawReason reason = DrawReason::not_evaluated;
    std::uint32_t checked = 0, passed = 0;
    std::uintptr_t context = 0, target_resource = 0, vertex_shader = 0, layout = 0;
    UINT elements = 0, start = 0;
    INT base = 0;
    bool indexed = false, layout_observed = false;
    DXGI_FORMAT target_format = DXGI_FORMAT_UNKNOWN, index_format = DXGI_FORMAT_UNKNOWN;
    UINT target_width = 0, target_height = 0, vertex_stride = 0, vertex_offset = 0, index_offset = 0;
    UINT position_slot = 0, position_offset = 0, position_bytes = 0, corners = 0;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::uint64_t owner = 0, begin_epoch = 0, return_epoch = 0, serial_begin = 0, serial_return = 0;
    ControlEvidence controls {};
    LayoutEvidence layout_metadata {};
    std::array<BufferEvidence, 2> buffers {};
    std::array<std::int64_t, 3> indices {};
    std::array<std::array<float, 2>, 3> points {};
};
struct ConsumeEvidence
{
    ConsumeReason reason = ConsumeReason::not_evaluated;
    interop::ColorTransfer transfer = interop::ColorTransfer::unknown;
    std::uintptr_t context = 0, source_resource = 0;
    std::uint64_t owner = 0, certificate_owner = 0, current_epoch = 0, certificate_epoch = 0, serial = 0;
    bool execution_present = false, contents_present = false, was_srgb = false;
    bool owner_matches = false, epoch_matches = false, certificate_cleared = false;
};
static_assert(std::is_trivially_copyable_v<DrawEvidence> && std::is_trivially_copyable_v<ConsumeEvidence>);
}
