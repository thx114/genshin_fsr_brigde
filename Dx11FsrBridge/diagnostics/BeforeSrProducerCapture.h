#pragma once

#include "../runtime/ProducerTransferEvidence.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <ostream>
#include <vector>

namespace neo::diagnostics
{
// 2026-11-16：只供颜色来源诊断。快照证明绑定值，不推导 transfer 或 GPU writer 成功。
enum class ProducerStage : unsigned { vertex, pixel, compute };
enum class CaptureState : unsigned { idle, armed, pending, ready, failed };
enum class ProducerWrite : unsigned { copy_resource, copy_subresource, clear_target, update_subresource };
enum class WriterSelection { first_observed, last_before_sdk };

struct ProducerConstant
{
    ProducerStage stage {};
    unsigned slot = 0, first_constant = 0, constant_count = 0, buffer_bytes = 0;
    std::uintptr_t resource = 0;
    std::vector<std::uint8_t> bytes;
};

struct ProducerResource
{
    ProducerStage stage {};
    unsigned slot = 0;
    std::uintptr_t view = 0, resource = 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc {};
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    D3D11_BUFFER_DESC buffer {};
    D3D11_TEXTURE1D_DESC texture1d {};
    D3D11_TEXTURE2D_DESC texture2d {};
    D3D11_TEXTURE3D_DESC texture3d {};
};

struct ProducerSampler
{
    ProducerStage stage {};
    unsigned slot = 0;
    std::uintptr_t state = 0;
    D3D11_SAMPLER_DESC desc {};
};

class BeforeSrProducerCapture
{
public:
    static constexpr unsigned max_shader_bytes = 1024 * 1024;
    static constexpr unsigned max_constant_bytes = 2 * 1024 * 1024;
    static constexpr unsigned max_writer_snapshots = 8;

    explicit BeforeSrProducerCapture(WriterSelection selection = WriterSelection::first_observed)
        : selection_(selection) {}

    static HRESULT remember_shader(ID3D11DeviceChild &shader, const void *data, unsigned size);
    bool arm(ID3D11DeviceContext &context, ID3D11Resource &color);
    bool record_writer(ID3D11DeviceContext &context, std::uint64_t sequence, bool compute);
    void observe_return(ID3D11DeviceContext &context, std::uint64_t sequence,
        const producer::DrawEvidence *evidence = nullptr);
    void observe_overwrite(ID3D11DeviceContext &context, ID3D11Resource *destination, ProducerWrite operation);
    void observe_gap();
    CaptureState poll(ID3D11DeviceContext &context);
    void observe_sdk(ID3D11DeviceContext &context, ID3D11Resource &color,
        std::uint64_t instance, std::uint64_t frame, const producer::ConsumeEvidence *evidence = nullptr);
    void write_json(std::ostream &out) const;

    CaptureState state() const { return state_; }
    // 2026-11-16：快照退休不代表目标 SDK 已消费；其他实例不能抢走唯一回执。
    bool reportable() const noexcept
    {
        return state_ == CaptureState::failed || (state_ == CaptureState::ready && consumption_.observed);
    }
    const std::vector<ProducerConstant> &constants() const { return constants_; }
    std::uint64_t sequence() const { return sequence_; }
    std::uint64_t latest_writer() const { return latest_.sequence; }
    std::size_t retained_bytes() const { return retained_bytes_; }

private:
    using Buffer = Microsoft::WRL::ComPtr<ID3D11Buffer>;
    enum class OutputKind { rtv, om_uav, cs_uav };
    struct CandidateOutput
    {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        unsigned dimension = 0, slot = 0;
        bool found = false, unordered = false;
    };
    void observe_writer_binding(ID3D11DeviceContext &context, std::uint64_t sequence, OutputKind kind,
        DXGI_FORMAT format, unsigned slot, unsigned mip);
    bool capture_constants(ProducerStage stage);
    void capture_shader(ProducerStage stage, ID3D11DeviceChild *shader);
    void capture_inputs(ProducerStage stage);
    void capture_graphics_state();
    void observe_unordered_writer(ID3D11DeviceContext &context, std::uint64_t sequence,
        bool compute, CandidateOutput &output, unsigned first_slot = 0);
    void begin_snapshot(std::uint64_t sequence, bool compute, const CandidateOutput &output);
    void seal_readback(ID3D11DeviceContext &context);

    const WriterSelection selection_;
    CaptureState state_ = CaptureState::idle;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Resource> color_;
    Microsoft::WRL::ComPtr<ID3D11Query> completion_;
    std::vector<Buffer> staging_;
    std::size_t staging_start_ = 0, staging_bytes_ = 0;
    unsigned snapshots_recorded_ = 0;
    bool readback_sealed_ = false;
    std::vector<ProducerConstant> constants_;
    struct Shader { ProducerStage stage; std::vector<std::uint8_t> bytes; unsigned classes = 0; };
    std::vector<Shader> shaders_;
    // 2026-11-16（PID50264 缺少 t0/s0）：冻结 writer 时刻的描述，不长期持有游戏输入纹理。
    std::vector<ProducerResource> resources_;
    std::vector<ProducerSampler> samplers_;
    D3D11_BLEND_DESC1 blend_ {};
    std::array<float, 4> blend_factor_ {};
    unsigned sample_mask_ = 0;
    bool graphics_state_ = false, blend_default_ = true;
    D3D11_RASTERIZER_DESC rasterizer_ {};
    D3D11_DEPTH_STENCIL_DESC depth_stencil_ {};
    unsigned stencil_ref_ = 0;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::vector<D3D11_VIEWPORT> viewports_;
    std::vector<D3D11_RECT> scissors_;
    std::uint64_t sequence_ = 0;
    struct WriterBinding
    {
        std::uint64_t sequence = 0;
        OutputKind kind = OutputKind::rtv;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        unsigned slot = 0, mip = UINT_MAX;
        std::uintptr_t shader = 0, layout = 0;
        producer::DrawReason draw_reason = producer::DrawReason::not_evaluated;
        bool returned = false, layout_observed = false, draw_target_matches = false;
    } latest_;
    struct Consumption
    {
        bool observed = false, writer_matches = false, original_returned = false;
        std::uint64_t latest_writer = 0, instance = 0, frame = 0;
    } consumption_;
    producer::DrawEvidence runtime_draw_ {};
    producer::ConsumeEvidence runtime_consume_ {};
    bool runtime_draw_observed_ = false, runtime_consume_observed_ = false;
    bool original_returned_ = false, supported_events_complete_ = true;
    unsigned overwrite_count_ = 0;
    ProducerWrite last_overwrite_ = ProducerWrite::copy_resource;
    std::uintptr_t last_overwrite_context_ = 0;
    std::size_t retained_bytes_ = 0;
    DXGI_FORMAT view_format_ = DXGI_FORMAT_UNKNOWN;
    unsigned view_dimension_ = 0, slices_ = 0, output_slot_ = 0;
    bool compute_ = false, unordered_output_ = false, complete_ = true;
    HRESULT failure_ = S_OK;
};
}
