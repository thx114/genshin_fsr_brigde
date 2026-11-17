#include "BeforeSrProducerCapture.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <iomanip>

namespace neo::diagnostics
{
using Microsoft::WRL::ComPtr;
namespace
{
// 私有字节由真实 shader 对象持有，不按可能重用的地址从全局表借旧 DXBC。
constexpr GUID k_bytecode {0x116f6b52, 0x7ab2, 0x4db1, {0xb5, 0xd9, 0x60, 0xf4, 0x67, 0x11, 0xa3, 0x2a}};

void hex_bytes(std::ostream &out, const std::vector<std::uint8_t> &bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    for (auto value : bytes) out << digits[value >> 4] << digits[value & 15];
}

template <class Description>
void descriptor_words(std::ostream &out, const Description &desc)
{
    static_assert(sizeof(desc) % sizeof(std::uint32_t) == 0);
    std::array<std::uint32_t, sizeof(desc) / sizeof(std::uint32_t)> words;
    std::memcpy(words.data(), &desc, sizeof(desc));
    out << '[';
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        if (i) out << ',';
        out << words[i];
    }
    out << ']';
}

const char *json_bool(bool value) { return value ? "true" : "false"; }
void json_float(std::ostream &out, float value)
{
    if (std::isfinite(value)) out << value;
    else out << "null";
}
void write_draw_evidence(std::ostream &out, const producer::DrawEvidence &value, bool observed,
    std::uintptr_t captured_resource)
{
    out << "{\"observed\":" << json_bool(observed)
        << ",\"scope\":\"rtv0\",\"same_captured_resource\":"
        << json_bool(observed && value.target_resource && value.target_resource == captured_resource)
        << ",\"reason\":\"" << producer::reason_name(value.reason) << "\",\"checked\":" << value.checked
        << ",\"passed\":" << value.passed << ",\"context\":" << value.context
        << ",\"target_resource\":" << value.target_resource << ",\"target_format\":" << value.target_format
        << ",\"target_width\":" << value.target_width << ",\"target_height\":" << value.target_height
        << ",\"elements\":" << value.elements << ",\"indexed\":" << json_bool(value.indexed)
        << ",\"start\":" << value.start << ",\"base\":" << value.base
        << ",\"owner\":" << value.owner << ",\"begin_epoch\":" << value.begin_epoch
        << ",\"return_epoch\":" << value.return_epoch << ",\"serial_begin\":" << value.serial_begin
        << ",\"serial_return\":" << value.serial_return << ",\"controls\":{\"resource\":"
        << value.controls.resource << ",\"first\":" << value.controls.first << ",\"count\":" << value.controls.count
        << ",\"shadow_present\":" << json_bool(value.controls.shadow_present)
        << ",\"mapped\":" << json_bool(value.controls.mapped) << ",\"known\":" << value.controls.known
        << ",\"epoch\":" << value.controls.epoch << ",\"version_begin\":" << value.controls.version_begin
        << ",\"version_return\":" << value.controls.version_return << ",\"values\":[";
    for (unsigned i = 0; i < value.controls.values.size(); ++i)
        { if (i) out << ','; json_float(out, value.controls.values[i]); }
    out << "]},\"ia\":{\"vertex_shader\":" << value.vertex_shader << ",\"layout\":" << value.layout
        << ",\"layout_observed\":" << json_bool(value.layout_observed)
        << ",\"layout_reason\":" << static_cast<unsigned>(value.layout_metadata.reason)
        << ",\"layout_count\":" << value.layout_metadata.count
        << ",\"layout_inspected\":" << value.layout_metadata.inspected
        << ",\"layout_rejected_element\":" << value.layout_metadata.rejected_element
        << ",\"layout_elements\":[";
    for (unsigned i = 0; i < value.layout_metadata.count && i < value.layout_metadata.elements.size(); ++i)
    {
        const auto &item = value.layout_metadata.elements[i];
        if (i) out << ',';
        out << "{\"position0\":" << json_bool(item.position0) << ",\"semantic_index\":" << item.semantic_index
            << ",\"slot\":" << item.slot << ",\"offset\":" << item.offset << ",\"format\":" << item.format
            << ",\"classification\":" << item.classification << ",\"step\":" << item.step << '}';
    }
    out << "],\"topology\":" << value.topology << ",\"position_slot\":" << value.position_slot
        << ",\"position_offset\":" << value.position_offset << ",\"position_bytes\":" << value.position_bytes
        << ",\"vertex_stride\":" << value.vertex_stride << ",\"vertex_offset\":" << value.vertex_offset
        << ",\"index_format\":" << value.index_format << ",\"index_offset\":" << value.index_offset
        << ",\"buffers\":[";
    for (unsigned i = 0; i < value.buffers.size(); ++i)
    {
        const auto &item = value.buffers[i];
        if (i) out << ',';
        out << "{\"resource\":" << item.resource << ",\"desc_observed\":" << json_bool(item.desc_observed)
            << ",\"desc_words\":";
        descriptor_words(out, item.desc);
        out << ",\"shadow_present\":" << json_bool(item.shadow_present) << ",\"mapped\":" << json_bool(item.mapped)
            << ",\"immutable\":" << json_bool(item.immutable) << ",\"epoch\":" << item.epoch
            << ",\"version_begin\":" << item.version_begin << ",\"version_return\":" << item.version_return
            << ",\"read_offset\":" << item.read_offset << ",\"read_bytes\":" << item.read_bytes
            << ",\"reads_checked\":" << item.reads_checked << ",\"reads_passed\":" << item.reads_passed << '}';
    }
    out << "],\"indices\":[";
    for (unsigned i = 0; i < value.indices.size(); ++i) { if (i) out << ','; out << value.indices[i]; }
    out << "],\"points\":[";
    for (unsigned i = 0; i < value.points.size(); ++i)
        { if (i) out << ','; out << '['; json_float(out, value.points[i][0]); out << ',';
          json_float(out, value.points[i][1]); out << ']'; }
    out << "],\"covered_corners\":" << value.corners << "}}";
}
void write_consume_evidence(std::ostream &out, const producer::ConsumeEvidence &value, bool observed)
{
    out << "{\"observed\":" << json_bool(observed) << ",\"reason\":\""
        << producer::reason_name(value.reason) << "\",\"transfer\":" << static_cast<unsigned>(value.transfer)
        << ",\"context\":" << value.context << ",\"source_resource\":" << value.source_resource
        << ",\"execution_present\":" << json_bool(value.execution_present)
        << ",\"contents_present\":" << json_bool(value.contents_present) << ",\"owner\":" << value.owner
        << ",\"certificate_owner\":" << value.certificate_owner << ",\"current_epoch\":" << value.current_epoch
        << ",\"certificate_epoch\":" << value.certificate_epoch << ",\"serial\":" << value.serial
        << ",\"was_srgb\":" << json_bool(value.was_srgb) << ",\"owner_matches\":" << json_bool(value.owner_matches)
        << ",\"epoch_matches\":" << json_bool(value.epoch_matches)
        << ",\"certificate_cleared\":" << json_bool(value.certificate_cleared) << '}';
}
}

HRESULT BeforeSrProducerCapture::remember_shader(ID3D11DeviceChild &shader, const void *data, unsigned size)
{
    if (!data || !size || size > max_shader_bytes) return E_INVALIDARG;
    return shader.SetPrivateData(k_bytecode, size, data);
}

bool BeforeSrProducerCapture::arm(ID3D11DeviceContext &context, ID3D11Resource &color)
{
    if (state_ != CaptureState::idle || context.GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ComPtr<ID3D11Device> color_device, context_device;
    color.GetDevice(color_device.GetAddressOf());
    context.GetDevice(context_device.GetAddressOf());
    if (!context_device || context_device.Get() != color_device.Get()) return false;
    device_ = std::move(context_device);
    context_ = &context;
    color_ = &color;
    state_ = CaptureState::armed;
    return true;
}

void BeforeSrProducerCapture::capture_shader(ProducerStage stage, ID3D11DeviceChild *shader)
{
    Shader snapshot {stage, {}};
    unsigned size = 0;
    if (!shader || FAILED(shader->GetPrivateData(k_bytecode, &size, nullptr)) || !size || size > max_shader_bytes)
        complete_ = false;
    else
    {
        snapshot.bytes.resize(size);
        if (FAILED(shader->GetPrivateData(k_bytecode, &size, snapshot.bytes.data())))
        {
            snapshot.bytes.clear();
            complete_ = false;
        }
    }
    shaders_.push_back(std::move(snapshot));
}

void BeforeSrProducerCapture::capture_inputs(ProducerStage stage)
{
    // 2026-11-16（实际 PS 使用 t0/s0）：只在首次 writer 快照查询，后续重绑不得覆盖证据。
    std::array<ID3D11ShaderResourceView *, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> views {};
    std::array<ID3D11SamplerState *, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers {};
    const auto view_count = static_cast<UINT>(views.size());
    const auto sampler_count = static_cast<UINT>(samplers.size());
    if (stage == ProducerStage::vertex)
    {
        context_->VSGetShaderResources(0, view_count, views.data());
        context_->VSGetSamplers(0, sampler_count, samplers.data());
    }
    else if (stage == ProducerStage::pixel)
    {
        context_->PSGetShaderResources(0, view_count, views.data());
        context_->PSGetSamplers(0, sampler_count, samplers.data());
    }
    else
    {
        context_->CSGetShaderResources(0, view_count, views.data());
        context_->CSGetSamplers(0, sampler_count, samplers.data());
    }
    for (unsigned slot = 0; slot < views.size(); ++slot)
    {
        ComPtr<ID3D11ShaderResourceView> view;
        view.Attach(views[slot]);
        if (!view) continue;
        ProducerResource item;
        item.stage = stage;
        item.slot = slot;
        item.view = reinterpret_cast<std::uintptr_t>(view.Get());
        view->GetDesc(&item.view_desc);
        ComPtr<ID3D11Resource> resource;
        view->GetResource(resource.GetAddressOf());
        item.resource = reinterpret_cast<std::uintptr_t>(resource.Get());
        resource->GetType(&item.dimension);
        HRESULT hr = E_NOINTERFACE;
        switch (item.dimension)
        {
        case D3D11_RESOURCE_DIMENSION_BUFFER:
        {
            ComPtr<ID3D11Buffer> buffer;
            hr = resource.As(&buffer);
            if (SUCCEEDED(hr)) buffer->GetDesc(&item.buffer);
            break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        {
            ComPtr<ID3D11Texture1D> texture;
            hr = resource.As(&texture);
            if (SUCCEEDED(hr)) texture->GetDesc(&item.texture1d);
            break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        {
            ComPtr<ID3D11Texture2D> texture;
            hr = resource.As(&texture);
            if (SUCCEEDED(hr)) texture->GetDesc(&item.texture2d);
            break;
        }
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
        {
            ComPtr<ID3D11Texture3D> texture;
            hr = resource.As(&texture);
            if (SUCCEEDED(hr)) texture->GetDesc(&item.texture3d);
            break;
        }
        default:
            break;
        }
        complete_ &= SUCCEEDED(hr);
        resources_.push_back(std::move(item));
    }
    for (unsigned slot = 0; slot < samplers.size(); ++slot)
    {
        ComPtr<ID3D11SamplerState> sampler;
        sampler.Attach(samplers[slot]);
        if (!sampler) continue;
        ProducerSampler item;
        item.stage = stage;
        item.slot = slot;
        item.state = reinterpret_cast<std::uintptr_t>(sampler.Get());
        sampler->GetDesc(&item.desc);
        samplers_.push_back(item);
    }
}

void BeforeSrProducerCapture::capture_graphics_state()
{
    graphics_state_ = true;
    ComPtr<ID3D11BlendState> blend;
    context_->OMGetBlendState(blend.GetAddressOf(), blend_factor_.data(), &sample_mask_);
    blend_default_ = !blend;
    if (blend)
    {
        ComPtr<ID3D11BlendState1> blend1;
        if (SUCCEEDED(blend.As(&blend1))) blend1->GetDesc1(&blend_);
        else
        {
            D3D11_BLEND_DESC desc {};
            blend->GetDesc(&desc);
            blend_.AlphaToCoverageEnable = desc.AlphaToCoverageEnable;
            blend_.IndependentBlendEnable = desc.IndependentBlendEnable;
            for (unsigned i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            {
                const auto &from = desc.RenderTarget[i];
                auto &to = blend_.RenderTarget[i];
                to.BlendEnable = from.BlendEnable;
                to.SrcBlend = from.SrcBlend; to.DestBlend = from.DestBlend; to.BlendOp = from.BlendOp;
                to.SrcBlendAlpha = from.SrcBlendAlpha; to.DestBlendAlpha = from.DestBlendAlpha;
                to.BlendOpAlpha = from.BlendOpAlpha; to.RenderTargetWriteMask = from.RenderTargetWriteMask;
                to.LogicOp = D3D11_LOGIC_OP_NOOP;
            }
        }
    }
    else
    {
        // 空状态是 D3D 默认值，不能用全零描述将 ONE/ADD/写掩码错误记录为零。
        for (auto &target : blend_.RenderTarget)
        {
            target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ONE;
            target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_ZERO;
            target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            target.LogicOp = D3D11_LOGIC_OP_NOOP;
            target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        }
    }
    ComPtr<ID3D11RasterizerState> rasterizer;
    context_->RSGetState(rasterizer.GetAddressOf());
    if (rasterizer) rasterizer->GetDesc(&rasterizer_);
    else
    {
        rasterizer_.FillMode = D3D11_FILL_SOLID;
        rasterizer_.CullMode = D3D11_CULL_BACK;
        rasterizer_.DepthClipEnable = TRUE;
    }
    ComPtr<ID3D11DepthStencilState> depth;
    context_->OMGetDepthStencilState(depth.GetAddressOf(), &stencil_ref_);
    if (depth) depth->GetDesc(&depth_stencil_);
    else
    {
        depth_stencil_.DepthEnable = TRUE;
        depth_stencil_.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth_stencil_.DepthFunc = D3D11_COMPARISON_LESS;
        depth_stencil_.StencilReadMask = depth_stencil_.StencilWriteMask = D3D11_DEFAULT_STENCIL_READ_MASK;
        depth_stencil_.FrontFace = depth_stencil_.BackFace =
            {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    }
    context_->IAGetPrimitiveTopology(&topology_);
    UINT viewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    viewports_.resize(viewports);
    context_->RSGetViewports(&viewports, viewports_.data());
    viewports_.resize(viewports);
    UINT scissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    scissors_.resize(scissors);
    context_->RSGetScissorRects(&scissors, scissors_.data());
    scissors_.resize(scissors);
}

bool BeforeSrProducerCapture::capture_constants(ProducerStage stage)
{
    constexpr unsigned slots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
    std::array<Buffer, slots> buffers;
    std::array<ID3D11Buffer *, slots> raw {};
    std::array<unsigned, slots> first {}, count {};
    count.fill(D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT);
    ComPtr<ID3D11DeviceContext1> context1;
    context_.As(&context1);
    if (context1)
    {
        if (stage == ProducerStage::vertex) context1->VSGetConstantBuffers1(0, slots, raw.data(), first.data(), count.data());
        if (stage == ProducerStage::pixel) context1->PSGetConstantBuffers1(0, slots, raw.data(), first.data(), count.data());
        if (stage == ProducerStage::compute) context1->CSGetConstantBuffers1(0, slots, raw.data(), first.data(), count.data());
    }
    else
    {
        if (stage == ProducerStage::vertex) context_->VSGetConstantBuffers(0, slots, raw.data());
        if (stage == ProducerStage::pixel) context_->PSGetConstantBuffers(0, slots, raw.data());
        if (stage == ProducerStage::compute) context_->CSGetConstantBuffers(0, slots, raw.data());
    }
    for (unsigned slot = 0; slot < slots; ++slot) buffers[slot].Attach(raw[slot]);
    for (unsigned slot = 0; slot < slots; ++slot)
    {
        if (!buffers[slot]) continue;
        D3D11_BUFFER_DESC source {};
        buffers[slot]->GetDesc(&source);
        if (count[slot] > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT || first[slot] > UINT_MAX / 16)
            return false;
        const unsigned begin = first[slot] * 16;
        const unsigned bytes = begin < source.ByteWidth ?
            (std::min)(count[slot] * 16, source.ByteWidth - begin) : 0;
        if (retained_bytes_ + source.ByteWidth > max_constant_bytes) return false;
        ProducerConstant item {stage, slot, first[slot], count[slot], source.ByteWidth,
            reinterpret_cast<std::uintptr_t>(buffers[slot].Get()), {}};
        Buffer staging;
        if (bytes)
        {
            D3D11_BUFFER_DESC desc {};
            desc.ByteWidth = source.ByteWidth;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            failure_ = device_->CreateBuffer(&desc, nullptr, staging.GetAddressOf());
            if (FAILED(failure_)) return false;
            // 先冻结完整 buffer，退休后再按绑定范围截取，避免部分 CB 复制的限制。
            context_->CopyResource(staging.Get(), buffers[slot].Get());
        }
        retained_bytes_ += source.ByteWidth;
        constants_.push_back(std::move(item));
        staging_.push_back(std::move(staging));
    }
    return true;
}

void BeforeSrProducerCapture::observe_unordered_writer(ID3D11DeviceContext &context,
    std::uint64_t sequence, bool compute, unsigned first_slot)
{
    std::array<ComPtr<ID3D11UnorderedAccessView>, D3D11_1_UAV_SLOT_COUNT> views;
    std::array<ID3D11UnorderedAccessView *, D3D11_1_UAV_SLOT_COUNT> raw {};
    const UINT slots = device_->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ?
        D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
    if (first_slot >= slots) return;
    if (compute)
        context.CSGetUnorderedAccessViews(first_slot, slots - first_slot, raw.data() + first_slot);
    else
        context.OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
            first_slot, slots - first_slot, raw.data() + first_slot);
    for (unsigned i = 0; i < slots; ++i) views[i].Attach(raw[i]);
    for (unsigned i = 0; i < slots; ++i)
    {
        ComPtr<ID3D11Resource> resource;
        if (views[i]) views[i]->GetResource(resource.GetAddressOf());
        if (resource.Get() != color_.Get()) continue;
        latest_sequence_ = sequence;
        if (state_ != CaptureState::armed) return;
        if (writer_view_) return;
        D3D11_UNORDERED_ACCESS_VIEW_DESC desc {};
        views[i]->GetDesc(&desc);
        // 2026-11-16：不同 mip 可合法同时写入；不让不支持的候选盖掉 mip0 或遮住后续槽。
        if (desc.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D || desc.Texture2D.MipSlice != 0) continue;
        writer_view_ = views[i];
        unordered_output_ = true;
        view_format_ = desc.Format; view_dimension_ = desc.ViewDimension;
        mip_ = desc.Texture2D.MipSlice; slices_ = 1; output_slot_ = i;
        return;
    }
}

bool BeforeSrProducerCapture::record_writer(ID3D11DeviceContext &context, std::uint64_t sequence, bool compute)
{
    if ((state_ != CaptureState::armed && state_ != CaptureState::pending && state_ != CaptureState::ready) ||
        &context != context_.Get() || !sequence || consumption_.observed) return false;
    if (state_ == CaptureState::armed)
    {
        writer_view_.Reset();
        unordered_output_ = false;
    }
    // 2026-11-16：直接读取本 context 的真实输出，不借全局 DispatchState 的其他设备绑定。
    if (compute)
        observe_unordered_writer(context, sequence, true);
    else
    {
        std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> views;
        std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> raw {};
        context.OMGetRenderTargets(static_cast<UINT>(raw.size()), raw.data(), nullptr);
        unsigned unordered_start = 0;
        for (unsigned i = 0; i < raw.size(); ++i)
        {
            views[i].Attach(raw[i]);
            if (raw[i]) unordered_start = i + 1;
        }
        for (unsigned i = 0; i < views.size(); ++i)
        {
            ComPtr<ID3D11Resource> resource;
            if (views[i]) views[i]->GetResource(resource.GetAddressOf());
            if (resource.Get() != color_.Get()) continue;
            latest_sequence_ = sequence;
            if (state_ != CaptureState::armed) return false;
            D3D11_RENDER_TARGET_VIEW_DESC desc {};
            views[i]->GetDesc(&desc);
            if (desc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || desc.Texture2D.MipSlice != 0) continue;
            writer_view_ = views[i];
            view_format_ = desc.Format; view_dimension_ = desc.ViewDimension;
            mip_ = desc.Texture2D.MipSlice; slices_ = 1; output_slot_ = i;
            break;
        }
        // 2026-11-16：Draw 也能经像素着色器写 OM UAV；只看 RTV 会漏掉覆盖，误配旧常量。
        // pending 的 writer_view_ 是旧快照，仍须观察本次实际 UAV 以更新 latest_sequence_。
        observe_unordered_writer(context, sequence, false, unordered_start);
    }
    if (state_ != CaptureState::armed || !writer_view_ || mip_ != 0) return false;
    sequence_ = sequence;
    compute_ = compute;
    D3D11_QUERY_DESC query {D3D11_QUERY_EVENT, 0};
    failure_ = device_->CreateQuery(&query, completion_.GetAddressOf());
    if (FAILED(failure_)) { state_ = CaptureState::failed; return false; }
    if (compute)
    {
        ComPtr<ID3D11ComputeShader> shader;
        UINT classes = 0;
        context.CSGetShader(shader.GetAddressOf(), nullptr, &classes);
        capture_shader(ProducerStage::compute, shader.Get());
        shaders_.back().classes = classes;
        complete_ &= classes == 0;
        complete_ &= capture_constants(ProducerStage::compute);
        capture_inputs(ProducerStage::compute);
    }
    else
    {
        ComPtr<ID3D11PixelShader> pixel;
        ComPtr<ID3D11VertexShader> vertex;
        UINT pixel_classes = 0, vertex_classes = 0;
        context.PSGetShader(pixel.GetAddressOf(), nullptr, &pixel_classes);
        context.VSGetShader(vertex.GetAddressOf(), nullptr, &vertex_classes);
        capture_shader(ProducerStage::pixel, pixel.Get());
        shaders_.back().classes = pixel_classes;
        capture_shader(ProducerStage::vertex, vertex.Get());
        shaders_.back().classes = vertex_classes;
        complete_ &= pixel_classes == 0 && vertex_classes == 0;
        complete_ &= capture_constants(ProducerStage::pixel);
        complete_ &= capture_constants(ProducerStage::vertex);
        capture_inputs(ProducerStage::pixel);
        capture_inputs(ProducerStage::vertex);
        capture_graphics_state();
    }
    // End 只标记常量快照复制的退休；既不证明随后的 Draw 成功，也不认证像素编码。
    context.End(completion_.Get());
    state_ = CaptureState::pending;
    return true;
}

void BeforeSrProducerCapture::observe_return(ID3D11DeviceContext &context, std::uint64_t sequence,
    const producer::DrawEvidence *evidence)
{
    // 2026-11-16：只认刚捕获的实际命令返回，不把下一笔 Draw 或另一 context 当作此笔执行。
    if (&context == context_.Get() && sequence && sequence == sequence_ && !consumption_.observed)
    {
        original_returned_ = true;
        if (!runtime_draw_observed_ && evidence && evidence->context == reinterpret_cast<std::uintptr_t>(&context))
        { runtime_draw_ = *evidence; runtime_draw_observed_ = true; }
    }
}

void BeforeSrProducerCapture::observe_overwrite(ID3D11DeviceContext &context,
    ID3D11Resource *destination, ProducerWrite operation)
{
    if (destination != color_.Get() || !sequence_ || consumption_.observed)
        return;
    // 复制/清除/更新没有返回成功码；宁可拒绝旧快照，不把旧常量认证给可能已经改变的颜色。
    ++overwrite_count_;
    last_overwrite_ = operation;
    last_overwrite_context_ = reinterpret_cast<std::uintptr_t>(&context);
}

void BeforeSrProducerCapture::observe_gap()
{
    if (sequence_) supported_events_complete_ = false;
}

CaptureState BeforeSrProducerCapture::poll(ID3D11DeviceContext &context)
{
    if (state_ != CaptureState::pending || context_.Get() != &context) return state_;
    BOOL done = FALSE;
    failure_ = context.GetData(completion_.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (failure_ == S_FALSE || (failure_ == S_OK && !done)) return state_;
    // 失败不当作 GPU 完成；保持持有实际 staging/query/context，不在此回收未知工作。
    if (FAILED(failure_)) { state_ = CaptureState::failed; return state_; }
    for (std::size_t i = 0; i < staging_.size(); ++i)
    {
        if (!staging_[i]) continue;
        D3D11_BUFFER_DESC desc {};
        staging_[i]->GetDesc(&desc);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        failure_ = context.Map(staging_[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (failure_ == DXGI_ERROR_WAS_STILL_DRAWING) return state_;
        if (FAILED(failure_)) { state_ = CaptureState::failed; return state_; }
        if (mapped.pData)
        {
            const auto &item = constants_[i];
            const unsigned offset = item.first_constant * 16;
            const unsigned bytes = offset < desc.ByteWidth ?
                (std::min)(item.constant_count * 16, desc.ByteWidth - offset) : 0;
            if (bytes)
            {
                auto begin = static_cast<const std::uint8_t *>(mapped.pData) + offset;
                constants_[i].bytes.assign(begin, begin + bytes);
            }
        }
        else complete_ = false;
        context.Unmap(staging_[i].Get(), 0);
    }
    staging_.clear();
    completion_.Reset();
    state_ = CaptureState::ready;
    return state_;
}

void BeforeSrProducerCapture::observe_sdk(ID3D11DeviceContext &context, ID3D11Resource &color,
    std::uint64_t instance, std::uint64_t frame, const producer::ConsumeEvidence *evidence)
{
    if (consumption_.observed || !sequence_ || state_ == CaptureState::armed ||
        &context != context_.Get() || &color != color_.Get()) return;
    // 2026-11-16：SDK 输入属于此刻的 writer；异步 Map 完成时可能已是下一源帧。
    // 后续 writer 同样读取实际输出，不能借旧全局状态中可能漏记 OM UAV 的历史认证。
    // 这里只固定来源关系，不把 SDK/CPU 事件当作 GPU 快照退休。
    consumption_ = {true, sequence_ == latest_sequence_ && !overwrite_count_ && supported_events_complete_,
        original_returned_, latest_sequence_, instance, frame};
    if (evidence && evidence->context == reinterpret_cast<std::uintptr_t>(&context) &&
        evidence->source_resource == reinterpret_cast<std::uintptr_t>(&color))
    { runtime_consume_ = *evidence; runtime_consume_observed_ = true; }
}

void BeforeSrProducerCapture::write_json(std::ostream &out) const
{
    out << "{\"state\":" << static_cast<unsigned>(state_) << ",\"sequence\":" << sequence_
        << ",\"context\":" << reinterpret_cast<std::uintptr_t>(context_.Get())
        << ",\"color\":" << reinterpret_cast<std::uintptr_t>(color_.Get())
        << ",\"binding_bytes_complete\":" << (complete_ && state_ == CaptureState::ready ? "true" : "false")
        << ",\"sdk_observed\":" << (consumption_.observed ? "true" : "false")
        << ",\"latest_writer_matches\":" <<
            (consumption_.writer_matches && supported_events_complete_ ? "true" : "false")
        << ",\"sdk_instance\":" << consumption_.instance << ",\"sdk_frame\":" << consumption_.frame
        << ",\"sdk_latest_writer\":" << consumption_.latest_writer
        << ",\"original_call_returned\":" <<
            ((consumption_.observed ? consumption_.original_returned : original_returned_) ? "true" : "false")
        << ",\"supported_events_complete\":" << (supported_events_complete_ ? "true" : "false")
        << ",\"overwrite_count_window\":" << overwrite_count_
        << ",\"last_overwrite\":" << (overwrite_count_ ? static_cast<unsigned>(last_overwrite_) : UINT_MAX)
        << ",\"last_overwrite_context\":" << last_overwrite_context_
        << ",\"runtime_transfer_attempt\":";
    write_draw_evidence(out, runtime_draw_, runtime_draw_observed_, reinterpret_cast<std::uintptr_t>(color_.Get()));
    out << ",\"runtime_transfer_consumption\":";
    write_consume_evidence(out, runtime_consume_, runtime_consume_observed_);
    out << ",\"transfer\":\"unknown\",\"writer_execution\":\"not-proven\",\"compute\":" << compute_
        << ",\"output\":{\"kind\":\"" << (unordered_output_ ? "uav" : "rtv")
        << "\",\"format\":" << view_format_ << ",\"dimension\":" << view_dimension_
        << ",\"mip\":" << mip_ << ",\"first_slice\":" << first_slice_ << ",\"slices\":" << slices_
        << ",\"slot\":" << output_slot_ << "},\"constant_bytes\":" << retained_bytes_
        << ",\"hr\":" << static_cast<unsigned>(failure_) << ",\"shaders\":[";
    for (std::size_t i = 0; i < shaders_.size(); ++i)
    {
        if (i) out << ',';
        out << "{\"stage\":" << static_cast<unsigned>(shaders_[i].stage)
            << ",\"class_instances\":" << shaders_[i].classes << ",\"dxbc_hex\":\"";
        hex_bytes(out, shaders_[i].bytes);
        out << "\"}";
    }
    out << "],\"constants\":[";
    for (std::size_t i = 0; i < constants_.size(); ++i)
    {
        if (i) out << ',';
        const auto &item = constants_[i];
        out << "{\"stage\":" << static_cast<unsigned>(item.stage) << ",\"slot\":" << item.slot
            << ",\"resource\":" << item.resource << ",\"first_constant\":" << item.first_constant
            << ",\"constant_count\":" << item.constant_count << ",\"buffer_bytes\":" << item.buffer_bytes
            << ",\"visible_bytes\":" << item.bytes.size()
            << ",\"data_hex\":\"";
        hex_bytes(out, item.bytes);
        out << "\"}";
    }
    out << "],\"resource_views\":[";
    for (std::size_t i = 0; i < resources_.size(); ++i)
    {
        if (i) out << ',';
        const auto &item = resources_[i];
        out << "{\"stage\":" << static_cast<unsigned>(item.stage) << ",\"slot\":" << item.slot
            << ",\"view\":" << item.view << ",\"resource\":" << item.resource
            << ",\"view_format\":" << item.view_desc.Format
            << ",\"view_dimension\":" << item.view_desc.ViewDimension << ",\"view_descriptor_words\":";
        descriptor_words(out, item.view_desc);
        out << ",\"resource_dimension\":" << item.dimension << ",\"resource_descriptor_words\":";
        switch (item.dimension)
        {
        case D3D11_RESOURCE_DIMENSION_BUFFER: descriptor_words(out, item.buffer); break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D: descriptor_words(out, item.texture1d); break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D: descriptor_words(out, item.texture2d); break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D: descriptor_words(out, item.texture3d); break;
        default: out << "[]"; break;
        }
        out << '}';
    }
    out << "],\"samplers\":[";
    for (std::size_t i = 0; i < samplers_.size(); ++i)
    {
        if (i) out << ',';
        const auto &item = samplers_[i];
        out << "{\"stage\":" << static_cast<unsigned>(item.stage) << ",\"slot\":" << item.slot
            << ",\"state\":" << item.state << ",\"filter\":" << item.desc.Filter
            << ",\"address\":[" << item.desc.AddressU << ',' << item.desc.AddressV << ',' << item.desc.AddressW
            << "],\"descriptor_words\":";
        descriptor_words(out, item.desc);
        out << '}';
    }
    out << "],\"graphics_state\":{\"captured\":" << (graphics_state_ ? "true" : "false");
    if (graphics_state_)
    {
        out << ",\"default_blend\":" << (blend_default_ ? "true" : "false")
            << ",\"sample_mask\":" << sample_mask_ << ",\"blend_factor_words\":";
        descriptor_words(out, blend_factor_);
        out << ",\"blend_descriptor_words\":";
        descriptor_words(out, blend_);
        out << ",\"rasterizer_descriptor_words\":";
        descriptor_words(out, rasterizer_);
        out << ",\"depth_stencil_descriptor_words\":";
        descriptor_words(out, depth_stencil_);
        out << ",\"stencil_ref\":" << stencil_ref_ << ",\"topology\":" << topology_
            << ",\"viewport_words\":[";
        for (std::size_t i = 0; i < viewports_.size(); ++i)
        {
            if (i) out << ',';
            descriptor_words(out, viewports_[i]);
        }
        out << "],\"scissor_words\":[";
        for (std::size_t i = 0; i < scissors_.size(); ++i)
        {
            if (i) out << ',';
            descriptor_words(out, scissors_[i]);
        }
        out << ']';
    }
    // 固定宽度字数组保留 float 位和视图联合体；布局严格采用 Windows SDK D3D11 描述符。
    out << "},\"descriptor_encoding\":\"d3d11-sdk-u32-words\""
        << ",\"input_scope\":\"captured-shader-stages-only\""
        << ",\"resource_identity_scope\":\"writer-binding-snapshot\""
        << ",\"write_observation_scope\":\"draw-dispatch-copy-clear-update-hooks-only\""
        << ",\"unobserved_writers\":\"map-resolve-uav-clear-context1-instanced-indirect-deferred-other\""
        << ",\"resource_contents\":\"not-captured\",\"scope\":\"direct-writer-bindings-only\"}";
}
}
