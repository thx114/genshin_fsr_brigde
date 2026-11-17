#include "../diagnostics/BeforeSrProducerCapture.h"
#include <d3dcompiler.h>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace neo::diagnostics;
namespace
{
unsigned checks = 0;
void check(bool value, const char *message)
{
    ++checks;
    if (!value) { std::cerr << "FAIL " << message << '\n'; std::exit(1); }
}
void ok(HRESULT hr, const char *message) { check(SUCCEEDED(hr), message); }
ComPtr<ID3DBlob> compile(const char *text, const char *profile)
{
    ComPtr<ID3DBlob> code, error;
    const HRESULT hr = D3DCompile(text, std::strlen(text), nullptr, nullptr, nullptr, "main", profile,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), error.GetAddressOf());
    if (FAILED(hr) && error) std::cerr << static_cast<const char *>(error->GetBufferPointer());
    ok(hr, "compile");
    return code;
}
struct Device
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Device()
    {
        ok(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
            device.GetAddressOf(), nullptr, context.GetAddressOf()), "WARP");
    }
    explicit Device(D3D_FEATURE_LEVEL level)
    {
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
            D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf());
        check(SUCCEEDED(hr) || hr == E_INVALIDARG || hr == DXGI_ERROR_UNSUPPORTED,
            "optional WARP capability query has no unexpected failure");
        if (FAILED(hr))
            std::cout << "SKIP graphics UAV high slot: WARP feature level " << std::hex << level
                << " unavailable, hr=" << static_cast<unsigned>(hr) << std::dec << '\n';
    }
    ComPtr<ID3D11Texture2D> color()
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = desc.Height = 16; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> texture;
        ok(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()), "color");
        return texture;
    }
    ComPtr<ID3D11RenderTargetView> target(ID3D11Resource &resource)
    {
        D3D11_RENDER_TARGET_VIEW_DESC desc {};
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> view;
        ok(device->CreateRenderTargetView(&resource, &desc, view.GetAddressOf()), "RTV");
        return view;
    }
    ComPtr<ID3D11Buffer> constant(unsigned words, unsigned value)
    {
        std::vector<unsigned> data(words, value);
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = words * 4; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial {data.data(), 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        ok(device->CreateBuffer(&desc, &initial, buffer.GetAddressOf()), "CB");
        return buffer;
    }
};
void finish(BeforeSrProducerCapture &capture, Device &d)
{
    // Flush 和限时等待只属于测试宿主，生产 poll 两者均不执行。
    d.context->Flush();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (capture.poll(*d.context.Get()) == CaptureState::pending && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(capture.state() == CaptureState::ready, "snapshot retired");
}
void word(const ProducerConstant &item, unsigned expected)
{
    check(!item.bytes.empty(), "CB contains bytes");
    for (std::size_t offset = 0; offset < item.bytes.size(); offset += 4)
    {
        unsigned value = 0;
        std::memcpy(&value, item.bytes.data() + offset, 4);
        check(value == expected, "writer-time bytes survive later update");
    }
}
void check_graphics_uav_writer(Device &d, unsigned slot)
{
    // 2026-11-16：只绑定 UAV 不能证明覆盖实际发生；独立读回绿色像素后才检查来源失配。
    d.context->ClearState();
    auto color = d.color();
    auto target = d.target(*color.Get());
    const auto vertex_code = compile("float4 main(uint i:SV_VertexID):SV_Position{"
        "return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
    const auto pixel_code = compile("float4 main():SV_Target{return float4(1,0,0,1);}", "ps_5_0");
    const std::string uav_source = "RWTexture2D<float4> color:register(u" + std::to_string(slot) +
        ");void main(float4 p:SV_Position){color[uint2(p.xy)]=float4(0,1,0,1);}";
    const auto uav_code = compile(uav_source.c_str(), "ps_5_0");
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel, uav_pixel;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ok(d.device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), nullptr,
        vertex.GetAddressOf()), "graphics UAV VS");
    ok(d.device->CreatePixelShader(pixel_code->GetBufferPointer(), pixel_code->GetBufferSize(), nullptr,
        pixel.GetAddressOf()), "graphics UAV RTV PS");
    ok(d.device->CreatePixelShader(uav_code->GetBufferPointer(), uav_code->GetBufferSize(), nullptr,
        uav_pixel.GetAddressOf()), "graphics UAV PS");
    ok(BeforeSrProducerCapture::remember_shader(*vertex.Get(), vertex_code->GetBufferPointer(),
        static_cast<unsigned>(vertex_code->GetBufferSize())), "graphics UAV VS DXBC");
    ok(BeforeSrProducerCapture::remember_shader(*pixel.Get(), pixel_code->GetBufferPointer(),
        static_cast<unsigned>(pixel_code->GetBufferSize())), "graphics UAV RTV PS DXBC");
    ok(BeforeSrProducerCapture::remember_shader(*uav_pixel.Get(), uav_code->GetBufferPointer(),
        static_cast<unsigned>(uav_code->GetBufferSize())), "graphics UAV PS DXBC");
    D3D11_UNORDERED_ACCESS_VIEW_DESC view {};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    ok(d.device->CreateUnorderedAccessView(color.Get(), &view, uav.GetAddressOf()), "graphics UAV view");
    auto constant = d.constant(16, 0x12345678);
    d.context->VSSetShader(vertex.Get(), nullptr, 0);
    d.context->PSSetShader(pixel.Get(), nullptr, 0);
    d.context->PSSetConstantBuffers(0, 1, constant.GetAddressOf());
    d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D11_VIEWPORT viewport {0, 0, 16, 16, 0, 1};
    d.context->RSSetViewports(1, &viewport);
    d.context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    BeforeSrProducerCapture overwritten;
    check(overwritten.arm(*d.context.Get(), *color.Get()), "arm graphics UAV overwrite");
    check(overwritten.record_writer(*d.context.Get(), 41, false), "RTV writer before UAV overwrite");
    d.context->Draw(3, 0);
    std::array<unsigned, 16> later;
    later.fill(0x87654321);
    d.context->UpdateSubresource(constant.Get(), 0, nullptr, later.data(), 0, 0);
    d.context->PSSetShader(uav_pixel.Get(), nullptr, 0);
    d.context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, slot, 1,
        uav.GetAddressOf(), nullptr);
    check(!overwritten.record_writer(*d.context.Get(), 42, false), "OM UAV overwrite is not recaptured");
    d.context->Draw(3, 0);
    ID3D11UnorderedAccessView *none = nullptr;
    d.context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, slot, 1, &none, nullptr);
    overwritten.observe_sdk(*d.context.Get(), *color.Get(), 7, 105);
    finish(overwritten, d);
    check(overwritten.sequence() == 41, "OM UAV preserves original snapshot identity");
    check(overwritten.constants().size() == 1, "OM UAV overwrite retains original CB list");
    word(overwritten.constants()[0], 0x12345678);
    std::ostringstream stale;
    overwritten.write_json(stale);
    check(stale.str().find("\"latest_writer_matches\":false") != std::string::npos,
        "OM UAV overwrite invalidates original writer");
    check(stale.str().find("\"sdk_latest_writer\":42") != std::string::npos,
        "SDK latches graphics UAV writer sequence");

    D3D11_TEXTURE2D_DESC sample_desc {};
    sample_desc.Width = sample_desc.Height = sample_desc.MipLevels = sample_desc.ArraySize = 1;
    sample_desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    sample_desc.SampleDesc.Count = 1;
    sample_desc.Usage = D3D11_USAGE_STAGING;
    sample_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> sample;
    ok(d.device->CreateTexture2D(&sample_desc, nullptr, sample.GetAddressOf()), "graphics UAV pixel staging");
    const D3D11_BOX center {8, 8, 0, 9, 9, 1};
    d.context->CopySubresourceRegion(sample.Get(), 0, 0, 0, 0, color.Get(), 0, &center);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    ok(d.context->Map(sample.Get(), 0, D3D11_MAP_READ, 0, &mapped), "graphics UAV pixel readback");
    unsigned actual = 0;
    std::memcpy(&actual, mapped.pData, sizeof(actual));
    d.context->Unmap(sample.Get(), 0);
    check(actual == 0xff00ff00, "graphics UAV Draw actually wrote green");

    // 2026-11-16：首笔就经 OM UAV 的来源也要保留实际槽位与视图类型，不能写成 RTV 维度。
    BeforeSrProducerCapture captured;
    check(captured.arm(*d.context.Get(), *color.Get()), "arm initial graphics UAV source");
    d.context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, slot, 1,
        uav.GetAddressOf(), nullptr);
    check(captured.record_writer(*d.context.Get(), 43, false), "initial graphics UAV snapshot");
    d.context->Draw(3, 0);
    later.fill(0);
    d.context->UpdateSubresource(constant.Get(), 0, nullptr, later.data(), 0, 0);
    d.context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, slot, 1, &none, nullptr);
    captured.observe_sdk(*d.context.Get(), *color.Get(), 7, 106);
    finish(captured, d);
    check(captured.constants().size() == 1, "initial graphics UAV captures pixel CB");
    word(captured.constants()[0], 0x87654321);
    std::ostringstream snapshot;
    captured.write_json(snapshot);
    check(snapshot.str().find("\"latest_writer_matches\":true") != std::string::npos,
        "initial graphics UAV writer remains current");
    const std::string view_metadata = "\"kind\":\"uav\",\"format\":" +
        std::to_string(DXGI_FORMAT_R8G8B8A8_UNORM) + ",\"dimension\":" +
        std::to_string(D3D11_UAV_DIMENSION_TEXTURE2D);
    check(snapshot.str().find(view_metadata) != std::string::npos,
        "graphics UAV view metadata is typed as UAV");
    check(snapshot.str().find("\"slot\":" + std::to_string(slot) + '}') != std::string::npos,
        "graphics UAV preserves output slot");
    check(snapshot.str().find("\"binding_bytes_complete\":true") != std::string::npos,
        "initial graphics UAV has shader and constant bytes");
    check(snapshot.str().find("\"transfer\":\"unknown\"") != std::string::npos,
        "graphics UAV transfer remains unknown");
    check(SUCCEEDED(d.device->GetDeviceRemovedReason()), "graphics UAV device remains healthy");
    std::cout << "GRAPHICS_UAV slot=" << slot << " actual_pixel=" << std::hex << actual << std::dec
        << " overwrite=" << stale.str() << " capture=" << snapshot.str() << '\n';
    d.context->ClearState();
}
void check_mip_zero_selection(Device &d, ID3D11VertexShader &vertex, ID3D11PixelShader &pixel)
{
    // 2026-11-16：同资源的不同 mip 可同时绑定；拒绝 mip1 不能污染随后有效 mip0 的视图类型。
    // 本用例只验证实际绑定选择和常量快照，不把未执行的 Draw 当作 GPU writer 证据。
    d.context->ClearState();
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = desc.Height = 16;
    desc.MipLevels = 2;
    desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> color;
    ok(d.device->CreateTexture2D(&desc, nullptr, color.GetAddressOf()), "multiple-mip color");
    auto target = d.target(*color.Get());
    D3D11_UNORDERED_ACCESS_VIEW_DESC view {};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11UnorderedAccessView> mip_zero, mip_one;
    ok(d.device->CreateUnorderedAccessView(color.Get(), &view, mip_zero.GetAddressOf()), "mip0 UAV");
    view.Texture2D.MipSlice = 1;
    ok(d.device->CreateUnorderedAccessView(color.Get(), &view, mip_one.GetAddressOf()), "mip1 UAV");
    auto constant = d.constant(16, 0x13579bdf);
    d.context->VSSetShader(&vertex, nullptr, 0);
    d.context->PSSetShader(&pixel, nullptr, 0);
    d.context->PSSetConstantBuffers(0, 1, constant.GetAddressOf());

    BeforeSrProducerCapture rtv;
    check(rtv.arm(*d.context.Get(), *color.Get()), "arm multiple-mip RTV selection");
    d.context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1,
        mip_one.GetAddressOf(), nullptr);
    check(!rtv.record_writer(*d.context.Get(), 51, false), "mip1-only writer is unsupported");
    check(rtv.state() == CaptureState::armed, "mip1 rejection keeps capture armed");
    d.context->OMSetRenderTargetsAndUnorderedAccessViews(1, target.GetAddressOf(), nullptr, 1, 1,
        mip_one.GetAddressOf(), nullptr);
    ComPtr<ID3D11RenderTargetView> bound_target;
    ComPtr<ID3D11UnorderedAccessView> bound_uav;
    d.context->OMGetRenderTargetsAndUnorderedAccessViews(1, bound_target.GetAddressOf(), nullptr, 1, 1,
        bound_uav.GetAddressOf());
    check(bound_target.Get() == target.Get() && bound_uav.Get() == mip_one.Get(),
        "same-resource mip0 RTV and mip1 UAV are actually bound");
    check(rtv.record_writer(*d.context.Get(), 52, false), "valid mip0 RTV survives mip1 UAV candidate");
    rtv.observe_sdk(*d.context.Get(), *color.Get(), 7, 107);
    finish(rtv, d);
    check(rtv.constants().size() == 1, "mip0 RTV captures original CB");
    word(rtv.constants()[0], 0x13579bdf);
    std::ostringstream rtv_json;
    rtv.write_json(rtv_json);
    const std::string rtv_metadata = "\"kind\":\"rtv\",\"format\":" +
        std::to_string(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) + ",\"dimension\":" +
        std::to_string(D3D11_RTV_DIMENSION_TEXTURE2D) + ",\"mip\":0";
    check(rtv_json.str().find(rtv_metadata) != std::string::npos,
        "rejected mip1 UAV does not leave UAV kind on mip0 RTV snapshot");
    check(rtv_json.str().find("\"slot\":0}") != std::string::npos, "valid mip0 RTV keeps output slot");
    check(rtv_json.str().find("\"sdk_latest_writer\":52") != std::string::npos,
        "mip0 RTV owns accepted writer identity");

    // 2026-11-16：第一个同资源 UAV 是 mip1 时必须继续找，不得让它饿死后面的 mip0。
    ID3D11UnorderedAccessView *pair[] {mip_one.Get(), mip_zero.Get()};
    d.context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 2, pair, nullptr);
    std::array<ID3D11UnorderedAccessView *, 2> observed {};
    d.context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1,
        static_cast<UINT>(observed.size()), observed.data());
    const bool both_bound = observed[0] == mip_one.Get() && observed[1] == mip_zero.Get();
    for (auto *item : observed)
        if (item) item->Release();
    check(both_bound, "same-resource mip1 and mip0 UAVs are actually bound in order");
    BeforeSrProducerCapture uav;
    check(uav.arm(*d.context.Get(), *color.Get()), "arm multiple-mip UAV selection");
    check(uav.record_writer(*d.context.Get(), 53, false), "later mip0 UAV remains eligible");
    uav.observe_sdk(*d.context.Get(), *color.Get(), 7, 108);
    finish(uav, d);
    check(uav.constants().size() == 1, "later mip0 UAV captures original CB");
    word(uav.constants()[0], 0x13579bdf);
    std::ostringstream uav_json;
    uav.write_json(uav_json);
    const std::string uav_metadata = "\"kind\":\"uav\",\"format\":" +
        std::to_string(DXGI_FORMAT_R8G8B8A8_UNORM) + ",\"dimension\":" +
        std::to_string(D3D11_UAV_DIMENSION_TEXTURE2D) + ",\"mip\":0";
    check(uav_json.str().find(uav_metadata) != std::string::npos, "later UAV uses valid mip0 metadata");
    check(uav_json.str().find("\"slot\":2}") != std::string::npos,
        "mip1 first candidate does not hide mip0 in second UAV slot");
    check(uav_json.str().find("\"binding_bytes_complete\":true") != std::string::npos,
        "later mip0 UAV snapshot is complete");
    check(SUCCEEDED(d.device->GetDeviceRemovedReason()), "multiple-mip binding device remains healthy");
    std::cout << "MIP_ZERO_SELECTION rtv=" << rtv_json.str() << " uav=" << uav_json.str() << '\n';
    d.context->ClearState();
}

void check_input_state_snapshot(Device &d)
{
    // 2026-11-16（真实 producer 缺 t0/s0）：先证明这些输入真的影响像素，再检验重绑后的冻结状态。
    d.context->ClearState();
    auto color = d.color();
    D3D11_RENDER_TARGET_VIEW_DESC output_desc {};
    output_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    output_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> target;
    ok(d.device->CreateRenderTargetView(color.Get(), &output_desc, target.GetAddressOf()), "input-state RTV");
    D3D11_TEXTURE2D_DESC source_desc {};
    source_desc.Width = 2; source_desc.Height = source_desc.MipLevels = source_desc.ArraySize = 1;
    source_desc.SampleDesc.Count = 1;
    source_desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const std::array<unsigned, 2> pixels {0xff808080, 0xff404040};
    D3D11_SUBRESOURCE_DATA data {pixels.data(), sizeof(pixels), 0};
    ComPtr<ID3D11Texture2D> source;
    ok(d.device->CreateTexture2D(&source_desc, &data, source.GetAddressOf()), "input-state source");
    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc {};
    view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srgb, unorm;
    ok(d.device->CreateShaderResourceView(source.Get(), &view_desc, srgb.GetAddressOf()), "input-state sRGB SRV");
    view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ok(d.device->CreateShaderResourceView(source.Get(), &view_desc, unorm.GetAddressOf()), "input-state UNORM SRV");
    D3D11_TEXTURE3D_DESC volume_desc {};
    volume_desc.Width = 2; volume_desc.Height = 3; volume_desc.Depth = 4; volume_desc.MipLevels = 1;
    volume_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    volume_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture3D> volume;
    ComPtr<ID3D11ShaderResourceView> volume_view;
    ok(d.device->CreateTexture3D(&volume_desc, nullptr, volume.GetAddressOf()), "input-state 3D resource");
    ok(d.device->CreateShaderResourceView(volume.Get(), nullptr, volume_view.GetAddressOf()), "input-state 3D SRV");
    D3D11_SAMPLER_DESC sampler_desc {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> point, linear;
    ok(d.device->CreateSamplerState(&sampler_desc, point.GetAddressOf()), "input-state point sampler");
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    ok(d.device->CreateSamplerState(&sampler_desc, linear.GetAddressOf()), "input-state linear sampler");
    const auto vs_code = compile("float4 main(uint i:SV_VertexID):SV_Position{"
        "return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
    const auto ps_code = compile("Texture2D<float4> source:register(t0);SamplerState point_sampler:register(s0);"
        "float4 main():SV_Target{return source.SampleLevel(point_sampler,float2(0.375,0.5),0);}", "ps_5_0");
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ok(d.device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr,
        vertex.GetAddressOf()), "input-state VS");
    ok(d.device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr,
        pixel.GetAddressOf()), "input-state PS");
    ok(BeforeSrProducerCapture::remember_shader(*vertex.Get(), vs_code->GetBufferPointer(),
        static_cast<unsigned>(vs_code->GetBufferSize())), "input-state VS DXBC");
    ok(BeforeSrProducerCapture::remember_shader(*pixel.Get(), ps_code->GetBufferPointer(),
        static_cast<unsigned>(ps_code->GetBufferSize())), "input-state PS DXBC");
    D3D11_BLEND_DESC blend_desc {};
    auto &blend_target = blend_desc.RenderTarget[0];
    blend_target.BlendEnable = TRUE;
    blend_target.SrcBlend = D3D11_BLEND_BLEND_FACTOR; blend_target.DestBlend = D3D11_BLEND_ZERO;
    blend_target.SrcBlendAlpha = D3D11_BLEND_ONE; blend_target.DestBlendAlpha = D3D11_BLEND_ZERO;
    blend_target.BlendOp = blend_target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;
    ok(d.device->CreateBlendState(&blend_desc, blend.GetAddressOf()), "input-state blend");
    const std::array<float, 4> factor {0.5f, 0.25f, 0.75f, 1.f};
    D3D11_RASTERIZER_DESC raster_desc {};
    raster_desc.FillMode = D3D11_FILL_SOLID; raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.DepthClipEnable = raster_desc.ScissorEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ok(d.device->CreateRasterizerState(&raster_desc, rasterizer.GetAddressOf()), "input-state rasterizer");
    d.context->VSSetShader(vertex.Get(), nullptr, 0); d.context->PSSetShader(pixel.Get(), nullptr, 0);
    d.context->PSSetShaderResources(0, 1, srgb.GetAddressOf());
    d.context->PSSetShaderResources(5, 1, volume_view.GetAddressOf());
    d.context->VSSetShaderResources(127, 1, srgb.GetAddressOf());
    d.context->PSSetSamplers(0, 1, point.GetAddressOf()); d.context->VSSetSamplers(15, 1, point.GetAddressOf());
    d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    d.context->OMSetBlendState(blend.Get(), factor.data(), UINT_MAX);
    d.context->RSSetState(rasterizer.Get());
    const D3D11_VIEWPORT viewport {0, 0, 16, 16, 0, 1};
    const D3D11_RECT scissor {0, 0, 16, 16};
    d.context->RSSetViewports(1, &viewport); d.context->RSSetScissorRects(1, &scissor);
    const auto references = [&] { source->AddRef(); return source->Release(); };
    const auto before_references = references();
    BeforeSrProducerCapture capture;
    check(capture.arm(*d.context.Get(), *color.Get()), "arm actual-input snapshot");
    check(capture.record_writer(*d.context.Get(), 61, false), "capture actual input state");
    check(references() == before_references, "metadata snapshot does not pin input resource lifetime");
    d.context->Draw(3, 0);
    capture.observe_sdk(*d.context.Get(), *color.Get(), 9, 3);
    d.context->ClearState();
    d.context->PSSetShaderResources(0, 1, unorm.GetAddressOf());
    d.context->PSSetSamplers(0, 1, linear.GetAddressOf());
    finish(capture, d);
    D3D11_TEXTURE2D_DESC readback_desc {};
    color->GetDesc(&readback_desc);
    readback_desc.BindFlags = 0; readback_desc.Usage = D3D11_USAGE_STAGING;
    readback_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    ok(d.device->CreateTexture2D(&readback_desc, nullptr, readback.GetAddressOf()), "input-state readback");
    d.context->CopyResource(readback.Get(), color.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    ok(d.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "input-state pixel Map");
    const auto *actual = static_cast<const unsigned char *>(mapped.pData) + 8 * mapped.RowPitch + 8 * 4;
    const bool sampled = actual[0] >= 27 && actual[0] <= 29 && actual[1] >= 13 && actual[1] <= 15 &&
        actual[2] >= 40 && actual[2] <= 42 && actual[3] == 255;
    std::cout << "INPUT_STATE_PIXEL rgba=" << static_cast<unsigned>(actual[0]) << ','
        << static_cast<unsigned>(actual[1]) << ',' << static_cast<unsigned>(actual[2]) << ','
        << static_cast<unsigned>(actual[3]) << " expected=28,14,41,255\n";
    d.context->Unmap(readback.Get(), 0);
    check(sampled, "actual sRGB read, point sample and blend affected GPU pixels");
    std::ostringstream json; capture.write_json(json);
    const auto text = json.str();
    const auto resource_prefix = [&](unsigned stage, unsigned slot, ID3D11ShaderResourceView *view,
        ID3D11Resource *resource, unsigned format, unsigned dimension)
    {
        return "{\"stage\":" + std::to_string(stage) + ",\"slot\":" + std::to_string(slot) +
            ",\"view\":" + std::to_string(reinterpret_cast<std::uintptr_t>(view)) + ",\"resource\":" +
            std::to_string(reinterpret_cast<std::uintptr_t>(resource)) + ",\"view_format\":" +
            std::to_string(format) + ",\"view_dimension\":" + std::to_string(dimension);
    };
    check(text.find(resource_prefix(1, 0, srgb.Get(), source.Get(), 29, 4)) != std::string::npos,
        "PS t0 keeps actual sRGB view and source after rebinding UNORM");
    check(text.find(resource_prefix(0, 127, srgb.Get(), source.Get(), 29, 4)) != std::string::npos,
        "VS high SRV slot is not omitted");
    check(text.find(resource_prefix(1, 5, volume_view.Get(), volume.Get(), 10, 8)) != std::string::npos,
        "3D LUT resource and view dimension are retained");
    const std::string sampler = "{\"stage\":1,\"slot\":0,\"state\":" +
        std::to_string(reinterpret_cast<std::uintptr_t>(point.Get())) + ",\"filter\":0,\"address\":[3,3,3]";
    check(text.find(sampler) != std::string::npos, "original PS sampler survives later linear/wrap rebind");
    check(text.find("\"stage\":0,\"slot\":15,\"state\":") != std::string::npos,
        "VS high sampler slot is retained");
    check(text.find("\"captured\":true,\"default_blend\":false,\"sample_mask\":4294967295") !=
        std::string::npos, "actual blend state and sample mask survive ClearState");
    check(text.find("\"blend_factor_words\":[1056964608,1048576000,1061158912,1065353216]") !=
        std::string::npos, "actual blend factors are stored bit-exactly");
    check(text.find("\"topology\":4") != std::string::npos &&
        text.find("\"scissor_words\":[[0,0,16,16]]") != std::string::npos,
        "raster input topology and scissor survive ClearState");
    check(text.find("\"transfer\":\"unknown\"") != std::string::npos &&
        text.find("\"writer_execution\":\"not-proven\"") != std::string::npos,
        "input state does not certify transfer or writer execution");
    check(text.find("\"resource_contents\":\"not-captured\"") != std::string::npos &&
        text.find("\"binding_bytes_complete\":true") != std::string::npos,
        "metadata completeness does not claim texture contents");
    std::cout << "INPUT_STATE_SNAPSHOT " << text << '\n';
    check(SUCCEEDED(d.device->GetDeviceRemovedReason()), "input-state device remains healthy");
    d.context->ClearState();

    const auto cs_code = compile("Texture2D<float4> source:register(t0);SamplerState sample_state:register(s0);"
        "RWTexture2D<float4> output:register(u0);[numthreads(1,1,1)]"
        "void main(uint3 id:SV_DispatchThreadID){output[id.xy]="
        "source.SampleLevel(sample_state,float2(0.375,0.5),0);}", "cs_5_0");
    ComPtr<ID3D11ComputeShader> compute;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ok(d.device->CreateComputeShader(cs_code->GetBufferPointer(), cs_code->GetBufferSize(), nullptr,
        compute.GetAddressOf()), "input-state CS");
    ok(BeforeSrProducerCapture::remember_shader(*compute.Get(), cs_code->GetBufferPointer(),
        static_cast<unsigned>(cs_code->GetBufferSize())), "input-state CS DXBC");
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc {};
    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    ok(d.device->CreateUnorderedAccessView(color.Get(), &uav_desc, uav.GetAddressOf()), "input-state compute UAV");
    d.context->CSSetShader(compute.Get(), nullptr, 0);
    d.context->CSSetShaderResources(0, 1, srgb.GetAddressOf());
    d.context->CSSetSamplers(0, 1, point.GetAddressOf());
    d.context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    BeforeSrProducerCapture compute_capture;
    check(compute_capture.arm(*d.context.Get(), *color.Get()), "arm compute-input snapshot");
    check(compute_capture.record_writer(*d.context.Get(), 62, true), "capture compute input state");
    d.context->Dispatch(16, 16, 1);
    d.context->ClearState();
    finish(compute_capture, d);
    d.context->CopyResource(readback.Get(), color.Get());
    ok(d.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "compute-input pixel Map");
    actual = static_cast<const unsigned char *>(mapped.pData) + 8 * mapped.RowPitch + 8 * 4;
    const bool compute_sampled = actual[0] >= 54 && actual[0] <= 56 && actual[1] == actual[0] &&
        actual[2] == actual[0] && actual[3] == 255;
    d.context->Unmap(readback.Get(), 0);
    check(compute_sampled, "compute input sRGB sample affects GPU pixels");
    std::ostringstream compute_json; compute_capture.write_json(compute_json);
    check(compute_json.str().find(resource_prefix(2, 0, srgb.Get(), source.Get(), 29, 4)) != std::string::npos,
        "CS input resource is recorded with compute stage");
    check(compute_json.str().find("\"stage\":2,\"slot\":0,\"state\":") != std::string::npos,
        "CS input sampler is recorded with compute stage");
    check(compute_json.str().find("\"graphics_state\":{\"captured\":false}") != std::string::npos,
        "compute snapshot does not invent graphics output state");
    std::cout << "COMPUTE_INPUT_SNAPSHOT " << compute_json.str() << '\n';
    d.context->ClearState();
}

void check_writer_events(Device &d, Device &other)
{
    d.context->ClearState();
    auto color = d.color(), unrelated = d.color();
    auto target = d.target(*color.Get());
    const auto vs_code = compile("float4 main(uint i:SV_VertexID):SV_Position{"
        "return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
    const auto ps_code = compile("float4 main():SV_Target{return float4(1,0,0,1);}", "ps_5_0");
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ok(d.device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr,
        vertex.GetAddressOf()), "writer-event VS");
    ok(d.device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr,
        pixel.GetAddressOf()), "writer-event PS");
    ok(BeforeSrProducerCapture::remember_shader(*vertex.Get(), vs_code->GetBufferPointer(),
        static_cast<unsigned>(vs_code->GetBufferSize())), "writer-event VS bytes");
    ok(BeforeSrProducerCapture::remember_shader(*pixel.Get(), ps_code->GetBufferPointer(),
        static_cast<unsigned>(ps_code->GetBufferSize())), "writer-event PS bytes");
    d.context->VSSetShader(vertex.Get(), nullptr, 0); d.context->PSSetShader(pixel.Get(), nullptr, 0);
    d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D11_VIEWPORT viewport {0, 0, 16, 16, 0, 1};
    d.context->RSSetViewports(1, &viewport);
    d.context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    const auto description = [](const BeforeSrProducerCapture &capture)
    {
        std::ostringstream out; capture.write_json(out); return out.str();
    };
    BeforeSrProducerCapture returned;
    check(returned.arm(*d.context.Get(), *color.Get()), "arm returned command");
    check(returned.record_writer(*d.context.Get(), 71, false), "capture returned command");
    check(description(returned).find("\"original_call_returned\":false") != std::string::npos,
        "pre-Draw snapshot does not claim original return");
    returned.observe_return(*other.context.Get(), 71); returned.observe_return(*d.context.Get(), 72);
    check(description(returned).find("\"original_call_returned\":false") != std::string::npos,
        "wrong context or command sequence cannot complete original call");
    d.context->Draw(3, 0);
    returned.observe_return(*d.context.Get(), 71);
    returned.observe_overwrite(*d.context.Get(), unrelated.Get(), ProducerWrite::clear_target);
    returned.observe_sdk(*d.context.Get(), *color.Get(), 17, 1);
    returned.observe_overwrite(*d.context.Get(), color.Get(), ProducerWrite::update_subresource);
    finish(returned, d);
    check(description(returned).find("\"original_call_returned\":true") != std::string::npos,
        "matching command return is latched at SDK consumption");
    check(description(returned).find("\"latest_writer_matches\":true") != std::string::npos,
        "unrelated resource and post-consumption write preserve original receipt");
    check(description(returned).find("\"writer_execution\":\"not-proven\"") != std::string::npos,
        "CPU return is not GPU execution evidence");
    BeforeSrProducerCapture missed;
    check(missed.arm(*d.context.Get(), *color.Get()), "arm observation-gap snapshot");
    check(missed.record_writer(*d.context.Get(), 73, false), "capture observation-gap snapshot");
    missed.observe_return(*d.context.Get(), 73);
    missed.observe_sdk(*d.context.Get(), *color.Get(), 17, 2);
    missed.observe_gap();
    finish(missed, d);
    check(description(missed).find("\"latest_writer_matches\":false") != std::string::npos &&
        description(missed).find("\"supported_events_complete\":false") != std::string::npos,
        "a delayed contention gap cannot leave a certified latest writer");
    BeforeSrProducerCapture other_context_write;
    check(other_context_write.arm(*d.context.Get(), *color.Get()), "arm cross-context overwrite");
    check(other_context_write.record_writer(*d.context.Get(), 74, false), "capture cross-context overwrite");
    other_context_write.observe_overwrite(*other.context.Get(), color.Get(), ProducerWrite::copy_resource);
    other_context_write.observe_sdk(*d.context.Get(), *color.Get(), 17, 3);
    finish(other_context_write, d);
    check(description(other_context_write).find("\"latest_writer_matches\":false") != std::string::npos,
        "same resource written through another context conservatively invalidates provenance");

    D3D11_TEXTURE2D_DESC desc {};
    color->GetDesc(&desc);
    desc.BindFlags = 0;
    std::array<unsigned, 16 * 16> green; green.fill(0xff00ff00);
    D3D11_SUBRESOURCE_DATA data {green.data(), 16 * sizeof(unsigned), 0};
    ComPtr<ID3D11Texture2D> source, readback;
    ok(d.device->CreateTexture2D(&desc, &data, source.GetAddressOf()), "overwrite green source");
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ok(d.device->CreateTexture2D(&desc, nullptr, readback.GetAddressOf()), "overwrite readback");
    for (unsigned n = 0; n != 4; ++n)
    {
        BeforeSrProducerCapture overwritten;
        check(overwritten.arm(*d.context.Get(), *color.Get()), "arm write-operation snapshot");
        check(overwritten.record_writer(*d.context.Get(), 80 + n, false), "capture write-operation snapshot");
        d.context->Draw(3, 0);
        overwritten.observe_return(*d.context.Get(), 80 + n);
        overwritten.observe_overwrite(*d.context.Get(), color.Get(), static_cast<ProducerWrite>(n));
        if (n == 0) d.context->CopyResource(color.Get(), source.Get());
        if (n == 1) d.context->CopySubresourceRegion(color.Get(), 0, 0, 0, 0, source.Get(), 0, nullptr);
        if (n == 2)
        {
            const FLOAT value[] {0, 1, 0, 1};
            d.context->ClearRenderTargetView(target.Get(), value);
        }
        if (n == 3) d.context->UpdateSubresource(color.Get(), 0, nullptr, green.data(), data.SysMemPitch, 0);
        overwritten.observe_sdk(*d.context.Get(), *color.Get(), 17, 4 + n);
        finish(overwritten, d);
        d.context->CopyResource(readback.Get(), color.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        ok(d.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "overwrite pixel Map");
        unsigned actual = 0;
        std::memcpy(&actual, static_cast<const unsigned char *>(mapped.pData) + 8 * mapped.RowPitch + 8 * 4, 4);
        d.context->Unmap(readback.Get(), 0);
        check(actual == 0xff00ff00, "write operation actually changed current pixels to green");
        const auto json = description(overwritten);
        check(json.find("\"latest_writer_matches\":false") != std::string::npos &&
            json.find("\"overwrite_count_window\":1") != std::string::npos &&
            json.find("\"last_overwrite\":" + std::to_string(n)) != std::string::npos,
            "copy, partial copy, clear and update cannot reuse old writer provenance");
        std::cout << "WRITER_EVENT operation=" << n << " pixel=" << std::hex << actual << std::dec
            << " snapshot=" << json << '\n';
    }
    d.context->ClearState();
}
}
int main()
{
    Device d, other;
    auto color = d.color(), unrelated = d.color();
    auto target = d.target(*color.Get()), wrong = d.target(*unrelated.Get());
    auto pc = compile("float4 main():SV_Target{return float4(0.25,0.5,0.75,1);}", "ps_5_0");
    auto vc = compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i,0,0,1);}", "vs_5_0");
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11VertexShader> vertex;
    ok(d.device->CreatePixelShader(pc->GetBufferPointer(), pc->GetBufferSize(), nullptr, pixel.GetAddressOf()), "PS");
    ok(d.device->CreateVertexShader(vc->GetBufferPointer(), vc->GetBufferSize(), nullptr, vertex.GetAddressOf()), "VS");
    ok(BeforeSrProducerCapture::remember_shader(*pixel.Get(), pc->GetBufferPointer(),
        static_cast<unsigned>(pc->GetBufferSize())), "PS DXBC");
    ok(BeforeSrProducerCapture::remember_shader(*vertex.Get(), vc->GetBufferPointer(),
        static_cast<unsigned>(vc->GetBufferSize())), "VS DXBC");
    check(FAILED(BeforeSrProducerCapture::remember_shader(*pixel.Get(), nullptr, 0)), "missing DXBC rejected");
    d.context->PSSetShader(pixel.Get(), nullptr, 0); d.context->VSSetShader(vertex.Get(), nullptr, 0);
    auto cb0 = d.constant(16, 0x11111111), cb3 = d.constant(32, 0x33333333), vs2 = d.constant(16, 0x22222222);
    d.context->PSSetConstantBuffers(0, 1, cb0.GetAddressOf());
    d.context->PSSetConstantBuffers(3, 1, cb3.GetAddressOf());
    d.context->VSSetConstantBuffers(2, 1, vs2.GetAddressOf());
    BeforeSrProducerCapture capture;
    check(!capture.arm(*other.context.Get(), *color.Get()), "foreign device rejected");
    check(capture.arm(*d.context.Get(), *color.Get()), "arm source");
    check(!capture.arm(*d.context.Get(), *color.Get()), "one bounded capture");
    d.context->OMSetRenderTargets(1, wrong.GetAddressOf(), nullptr);
    check(!capture.record_writer(*d.context.Get(), 10, false), "wrong target rejected");
    d.context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    check(!capture.record_writer(*other.context.Get(), 11, false), "foreign context rejected");
    check(capture.record_writer(*d.context.Get(), 12, false), "graphics snapshot");
    check(capture.state() == CaptureState::pending, "SDK observation before readback retirement");
    capture.observe_sdk(*other.context.Get(), *color.Get(), 99, 99);
    capture.observe_sdk(*d.context.Get(), *unrelated.Get(), 99, 99);
    capture.observe_sdk(*d.context.Get(), *color.Get(), 7, 101);
    std::array<unsigned, 32> later; later.fill(0xeeeeeeee);
    for (auto *buffer : {cb0.Get(), cb3.Get(), vs2.Get()})
        d.context->UpdateSubresource(buffer, 0, nullptr, later.data(), 0, 0);
    check(!capture.record_writer(*d.context.Get(), 13, false), "pending work not duplicated");
    capture.observe_sdk(*d.context.Get(), *color.Get(), 8, 102);
    finish(capture, d);
    check(capture.constants().size() == 3, "all graphics CB slots");
    for (const auto &item : capture.constants()) word(item, item.stage == ProducerStage::vertex ?
        0x22222222 : (item.slot == 0 ? 0x11111111 : 0x33333333));
    std::ostringstream json; capture.write_json(json);
    check(json.str().find("\"format\":29") != std::string::npos, "actual SRGB RTV");
    check(json.str().find("\"transfer\":\"unknown\"") != std::string::npos, "transfer never guessed");
    check(json.str().find("\"binding_bytes_complete\":true") != std::string::npos, "complete CB and DXBC");
    check(json.str().find("\"sdk_observed\":true") != std::string::npos, "SDK observed while pending");
    check(json.str().find("\"latest_writer_matches\":true") != std::string::npos, "original writer remains matched");
    check(json.str().find("\"sdk_instance\":7,\"sdk_frame\":101,\"sdk_latest_writer\":12") !=
        std::string::npos, "later SDK cannot replace original consumption");
    std::cout << "SNAPSHOT " << json.str() << '\n';

    // 2026-11-16：GPU 快照退休不等于目标 SDK 已消费，其他实例不能抢走唯一报告配额。
    BeforeSrProducerCapture awaiting_sdk;
    check(awaiting_sdk.arm(*d.context.Get(), *color.Get()), "arm awaiting SDK source");
    check(awaiting_sdk.record_writer(*d.context.Get(), 17, false), "awaiting SDK snapshot");
    check(!awaiting_sdk.reportable(), "pending snapshot is not reportable");
    finish(awaiting_sdk, d);
    check(!awaiting_sdk.reportable(), "ready snapshot needs SDK association");
    awaiting_sdk.observe_sdk(*other.context.Get(), *color.Get(), 99, 99);
    check(!awaiting_sdk.reportable(), "foreign SDK context cannot report");
    awaiting_sdk.observe_sdk(*d.context.Get(), *unrelated.Get(), 99, 99);
    check(!awaiting_sdk.reportable(), "other SDK color cannot report");
    awaiting_sdk.observe_sdk(*d.context.Get(), *color.Get(), 7, 104);
    check(awaiting_sdk.reportable(), "target SDK association permits report");
    std::ostringstream associated; awaiting_sdk.write_json(associated);
    check(associated.str().find("\"sdk_instance\":7,\"sdk_frame\":104,\"sdk_latest_writer\":17") !=
        std::string::npos, "target SDK owns retired snapshot");

    // 2026-11-16：旧全局 RTV 历史会漏记 OM UAV 路线；只按真实 context 输出判定覆盖。
    BeforeSrProducerCapture overwritten;
    check(overwritten.arm(*d.context.Get(), *color.Get()), "arm overwritten source");
    check(overwritten.record_writer(*d.context.Get(), 14, false), "first writer captured");
    d.context->OMSetRenderTargets(1, wrong.GetAddressOf(), nullptr);
    check(!overwritten.record_writer(*d.context.Get(), 15, false), "other output preserves snapshot");
    d.context->OMSetRenderTargetsAndUnorderedAccessViews(1, target.GetAddressOf(), nullptr, 1, 0, nullptr, nullptr);
    check(!overwritten.record_writer(*d.context.Get(), 16, false), "overwrite tracked without recapture");
    overwritten.observe_sdk(*d.context.Get(), *color.Get(), 7, 102);
    finish(overwritten, d);
    std::ostringstream stale; overwritten.write_json(stale);
    check(stale.str().find("\"latest_writer_matches\":false") != std::string::npos, "actual overwrite rejects old snapshot");
    check(stale.str().find("\"sdk_latest_writer\":16") != std::string::npos, "SDK latches actual latest output");
    check(overwritten.sequence() == 14, "original snapshot identity preserved");

    ComPtr<ID3D11DeviceContext1> context1; ok(d.context.As(&context1), "Context1");
    auto ranged = d.constant(256, 0xabc12345);
    ID3D11Buffer *raw = ranged.Get(); UINT first = 16, count = 16;
    context1->PSSetConstantBuffers1(5, 1, &raw, &first, &count);
    BeforeSrProducerCapture offset;
    check(offset.arm(*d.context.Get(), *color.Get()), "arm offset");
    check(offset.record_writer(*d.context.Get(), 21, false), "offset snapshot"); finish(offset, d);
    bool found = false;
    for (const auto &item : offset.constants())
        if (item.stage == ProducerStage::pixel && item.slot == 5)
        {
            found = true;
            check(item.first_constant == 16 && item.constant_count == 16 && item.bytes.size() == 256, "CB range");
            word(item, 0xabc12345);
        }
    check(found, "nonzero ranged slot captured");

    // 错 writer 与缺少 DXBC 仍是缺证，不因为快照已经退休而标成完整。
    ComPtr<ID3D11PixelShader> missing_pixel;
    ok(d.device->CreatePixelShader(pc->GetBufferPointer(), pc->GetBufferSize(), nullptr,
        missing_pixel.GetAddressOf()), "PS without saved DXBC");
    d.context->PSSetShader(missing_pixel.Get(), nullptr, 0);
    first = 64; count = 16;
    context1->PSSetConstantBuffers1(5, 1, &raw, &first, &count);
    BeforeSrProducerCapture incomplete;
    check(incomplete.arm(*d.context.Get(), *color.Get()), "arm incomplete source");
    check(incomplete.record_writer(*d.context.Get(), 25, false), "incomplete snapshot");
    check(!incomplete.record_writer(*d.context.Get(), 26, false), "incomplete snapshot also tracks later writer");
    incomplete.observe_sdk(*d.context.Get(), *color.Get(), 7, 103);
    finish(incomplete, d);
    std::ostringstream missing; incomplete.write_json(missing);
    check(missing.str().find("\"binding_bytes_complete\":false") != std::string::npos, "missing DXBC explicit");
    check(missing.str().find("\"latest_writer_matches\":false") != std::string::npos, "stale writer explicit");
    found = false;
    for (const auto &item : incomplete.constants())
        if (item.stage == ProducerStage::pixel && item.slot == 5)
        {
            found = true;
            check(item.first_constant == 64 && item.bytes.empty(), "range outside buffer has no captured bytes");
        }
    check(found, "out-of-buffer range retains binding metadata");

    first = 0; count = 0;
    context1->PSSetConstantBuffers1(5, 1, &raw, &first, &count);
    BeforeSrProducerCapture empty;
    check(empty.arm(*d.context.Get(), *color.Get()), "arm empty range");
    check(empty.record_writer(*d.context.Get(), 27, false), "zero range snapshot");
    finish(empty, d);
    for (const auto &item : empty.constants())
        if (item.stage == ProducerStage::pixel && item.slot == 5)
            check(item.constant_count == 0 && item.bytes.empty(), "zero binding range has no captured bytes");

    auto cc = compile("RWTexture2D<float4> o:register(u0);[numthreads(1,1,1)]"
        "void main(uint3 id:SV_DispatchThreadID){o[id.xy]=1;}", "cs_5_0");
    ComPtr<ID3D11ComputeShader> cs;
    ok(d.device->CreateComputeShader(cc->GetBufferPointer(), cc->GetBufferSize(), nullptr, cs.GetAddressOf()), "CS");
    ok(BeforeSrProducerCapture::remember_shader(*cs.Get(), cc->GetBufferPointer(),
        static_cast<unsigned>(cc->GetBufferSize())), "CS DXBC");
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud {};
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ok(d.device->CreateUnorderedAccessView(color.Get(), &ud, uav.GetAddressOf()), "UAV");
    d.context->OMSetRenderTargets(0, nullptr, nullptr);
    d.context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr); d.context->CSSetShader(cs.Get(), nullptr, 0);
    auto cs6 = d.constant(16, 0x66666666); d.context->CSSetConstantBuffers(6, 1, cs6.GetAddressOf());
    BeforeSrProducerCapture compute;
    check(compute.arm(*d.context.Get(), *color.Get()), "arm compute");
    check(compute.record_writer(*d.context.Get(), 31, true), "compute snapshot");
    d.context->UpdateSubresource(cs6.Get(), 0, nullptr, later.data(), 0, 0); finish(compute, d);
    check(compute.constants().size() == 1 && compute.constants()[0].slot == 6, "CS6 captured");
    word(compute.constants()[0], 0x66666666);
    check_graphics_uav_writer(d, 1);
    check_mip_zero_selection(d, *vertex.Get(), *pixel.Get());
    Device high_slots(D3D_FEATURE_LEVEL_11_1);
    if (high_slots.device) check_graphics_uav_writer(high_slots, 37);
    check_input_state_snapshot(d);
    check_writer_events(d, other);
    std::cout << "BeforeSrProducerCapture: " << checks << " checks passed\n";
}
