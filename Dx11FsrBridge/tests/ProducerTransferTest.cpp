#include "../runtime/ProducerTransfer.cpp"
#include "producer-transfer/ProducerFixture.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace neo::producer;
using neo::interop::ColorTransfer;

namespace
{
unsigned checks = 0;
void check(bool value, const char *label)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s\n", label); std::exit(1); }
}

void repair(ID3D11DeviceContext *context);
template <auto Callback, std::size_t Slot>
using Route = ComMethodRoutes<decltype(Callback), Callback, Slot, false, &repair>;

HRESULT STDMETHODCALLTYPE shader(ID3D11Device *device, const void *data, SIZE_T size,
    ID3D11ClassLinkage *linkage, ID3D11PixelShader **out);
HRESULT STDMETHODCALLTYPE vertex(ID3D11Device *device, const void *data, SIZE_T size,
    ID3D11ClassLinkage *linkage, ID3D11VertexShader **out);
HRESULT STDMETHODCALLTYPE buffer(ID3D11Device *device, const D3D11_BUFFER_DESC *desc,
    const D3D11_SUBRESOURCE_DATA *initial, ID3D11Buffer **out);
void STDMETHODCALLTYPE target(ID3D11DeviceContext *context, UINT count, ID3D11RenderTargetView *const *views,
    ID3D11DepthStencilView *depth);
void STDMETHODCALLTYPE draw(ID3D11DeviceContext *context, UINT count, UINT first);
void STDMETHODCALLTYPE indexed(ID3D11DeviceContext *context, UINT count, UINT first, INT base);
HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext *context, ID3D11Resource *resource, UINT subresource,
    D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *out);
void STDMETHODCALLTYPE unmap(ID3D11DeviceContext *context, ID3D11Resource *resource, UINT subresource);
void STDMETHODCALLTYPE update(ID3D11DeviceContext *context, ID3D11Resource *resource, UINT subresource,
    const D3D11_BOX *box, const void *data, UINT row, UINT depth);
void STDMETHODCALLTYPE copy(ID3D11DeviceContext *context, ID3D11Resource *destination, ID3D11Resource *source);
void STDMETHODCALLTYPE clear(ID3D11DeviceContext *context, ID3D11RenderTargetView *view, const FLOAT *values);
void STDMETHODCALLTYPE compute_targets(ID3D11DeviceContext *context, UINT first, UINT count,
    ID3D11UnorderedAccessView *const *views, const UINT *initial);
void STDMETHODCALLTYPE dispatch(ID3D11DeviceContext *context, UINT x, UINT y, UINT z);

using Pixel = ComMethodRoutes<decltype(&shader), &shader, 15>;
using Vertex = ComMethodRoutes<decltype(&vertex), &vertex, 12>;
using Buffer = ComMethodRoutes<decltype(&buffer), &buffer, 3>;
using Draw = Route<&draw, 13>;
using Indexed = Route<&indexed, 12>;

HRESULT STDMETHODCALLTYPE shader(ID3D11Device *device, const void *data, SIZE_T size,
    ID3D11ClassLinkage *linkage, ID3D11PixelShader **out)
{
    const auto result = Pixel {}(device, data, size, linkage, out);
    if (SUCCEEDED(result) && out && *out) remember_shader(**out, data, size);
    return result;
}
HRESULT STDMETHODCALLTYPE vertex(ID3D11Device *device, const void *data, SIZE_T size,
    ID3D11ClassLinkage *linkage, ID3D11VertexShader **out)
{
    const auto result = Vertex {}(device, data, size, linkage, out);
    if (SUCCEEDED(result) && out && *out) remember_vertex(**out, data, size);
    return result;
}
HRESULT STDMETHODCALLTYPE buffer(ID3D11Device *device, const D3D11_BUFFER_DESC *desc,
    const D3D11_SUBRESOURCE_DATA *initial, ID3D11Buffer **out)
{
    const auto result = Buffer {}(device, desc, initial, out);
    if (SUCCEEDED(result) && desc && out && *out) remember_buffer(**out, *desc, initial);
    return result;
}
void STDMETHODCALLTYPE target(ID3D11DeviceContext *context, UINT count, ID3D11RenderTargetView *const *views,
    ID3D11DepthStencilView *depth)
{
    Route<&target, 33> {}(context, count, views, depth);
    refresh_targets(*context);
}
void STDMETHODCALLTYPE draw(ID3D11DeviceContext *context, UINT count, UINT first)
{
    auto writer = begin_draw(*context, count, false, first, 0);
    Draw {}(context, count, first);
    writer.returned();
}
void STDMETHODCALLTYPE indexed(ID3D11DeviceContext *context, UINT count, UINT first, INT base)
{
    auto writer = begin_draw(*context, count, true, first, base);
    Indexed {}(context, count, first, base);
    writer.returned();
}
HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext *context, ID3D11Resource *resource, UINT subresource,
    D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *out)
{
    const auto result = Route<&map, 14> {}(context, resource, subresource, type, flags, out);
    mapped(*context, resource, subresource, type, result, out);
    return result;
}
void STDMETHODCALLTYPE unmap(ID3D11DeviceContext *context, ID3D11Resource *resource, UINT subresource)
{
    unmapping(*context, resource, subresource);
    Route<&unmap, 15> {}(context, resource, subresource);
}
void STDMETHODCALLTYPE update(ID3D11DeviceContext *context, ID3D11Resource *resource, UINT subresource,
    const D3D11_BOX *box, const void *data, UINT row, UINT depth)
{
    update_buffer(*context, resource, subresource, box, data);
    Route<&update, 48> {}(context, resource, subresource, box, data, row, depth);
}
void STDMETHODCALLTYPE copy(ID3D11DeviceContext *context, ID3D11Resource *destination, ID3D11Resource *source)
{
    invalidate(destination);
    Route<&copy, 47> {}(context, destination, source);
}
void STDMETHODCALLTYPE clear(ID3D11DeviceContext *context, ID3D11RenderTargetView *view, const FLOAT *values)
{
    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    invalidate(resource.Get());
    Route<&clear, 50> {}(context, view, values);
}
void STDMETHODCALLTYPE compute_targets(ID3D11DeviceContext *context, UINT first, UINT count,
    ID3D11UnorderedAccessView *const *views, const UINT *initial)
{
    Route<&compute_targets, 68> {}(context, first, count, views, initial);
    refresh_targets(*context, true);
}
void STDMETHODCALLTYPE dispatch(ID3D11DeviceContext *context, UINT x, UINT y, UINT z)
{
    unordered_write(*context);
    Route<&dispatch, 41> {}(context, x, y, z);
}

void install(ID3D11DeviceContext *context)
{
    std::vector<com_vtable::MethodPatch> methods {Draw::slot(), Indexed::slot(), Route<&target, 33>::slot(),
        Route<&map, 14>::slot(), Route<&unmap, 15>::slot(), Route<&update, 48>::slot(),
        Route<&copy, 47>::slot(), Route<&clear, 50>::slot(), Route<&compute_targets, 68>::slot(),
        Route<&dispatch, 41>::slot()};
    const auto extra = extra_methods(&repair);
    methods.insert(methods.end(), extra.begin(), extra.end());
    check(com_vtable::patch(context, interface_layout::context(context), methods), "base method installation");
    check(bind_extended(*context), "extended method installation");
    attach_context(*context);
}
void repair(ID3D11DeviceContext *context)
{
    if (context && (!Draw::bound_on(context) || !Indexed::bound_on(context))) install(context);
}

std::uint32_t pixel(ID3D11Device &device, ID3D11DeviceContext &context, ID3D11Texture2D &source)
{
    D3D11_TEXTURE2D_DESC desc {};
    source.GetDesc(&desc);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(SUCCEEDED(device.CreateTexture2D(&desc, nullptr, staging.GetAddressOf())), "test staging");
    context.CopyResource(staging.Get(), &source);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    check(SUCCEEDED(context.Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "test pixel readback");
    std::uint32_t value;
    std::memcpy(&value, static_cast<const std::uint8_t *>(mapped.pData) + 4 * 3 + mapped.RowPitch * 3, 4);
    context.Unmap(staging.Get(), 0);
    return value;
}

ComPtr<ID3DBlob> compile(const char *text, const char *profile)
{
    ComPtr<ID3DBlob> code, errors;
    check(SUCCEEDED(D3DCompile(text, std::strlen(text), nullptr, nullptr, nullptr, "main", profile,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), errors.GetAddressOf())), "GPU writer shader");
    return code;
}

void zero_geometry(ID3D11Device &device, ID3D11DeviceContext &context, ID3D11Buffer &buffer, bool stream_output)
{
    if (!stream_output)
    {
        const auto code = compile("RWByteAddressBuffer b : register(u0);"
            "[numthreads(12,1,1)] void main(uint i : SV_DispatchThreadID) { b.Store(i*4,0); }", "cs_5_0");
        ComPtr<ID3D11ComputeShader> shader;
        check(SUCCEEDED(device.CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
            shader.GetAddressOf())), "UAV geometry writer");
        D3D11_UNORDERED_ACCESS_VIEW_DESC desc {};
        desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.Buffer.NumElements = 12; desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        ComPtr<ID3D11UnorderedAccessView> view;
        check(SUCCEEDED(device.CreateUnorderedAccessView(&buffer, &desc, view.GetAddressOf())), "geometry UAV");
        context.CSSetShader(shader.Get(), nullptr, 0);
        context.CSSetUnorderedAccessViews(0, 1, view.GetAddressOf(), nullptr);
        context.Dispatch(1, 1, 1);
        ID3D11UnorderedAccessView *empty = nullptr;
        context.CSSetUnorderedAccessViews(0, 1, &empty, nullptr);
        context.CSSetShader(nullptr, nullptr, 0);
    }
    else
    {
        const auto code = compile("struct V { float4 p : SV_Position; };"
            "[maxvertexcount(3)] void main(triangle V p[3],inout TriangleStream<V> s)"
            "{ V v; v.p=0; s.Append(v); s.Append(v); s.Append(v); }", "gs_5_0");
        const D3D11_SO_DECLARATION_ENTRY entry {0, "SV_Position", 0, 0, 4, 0};
        const UINT stride = 16, offset = 0;
        ComPtr<ID3D11GeometryShader> shader;
        check(SUCCEEDED(device.CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(), code->GetBufferSize(),
            &entry, 1, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, shader.GetAddressOf())), "SO geometry writer");
        ID3D11Buffer *output = &buffer;
        context.GSSetShader(shader.Get(), nullptr, 0);
        context.SOSetTargets(1, &output, &offset);
        context.Draw(3, 0);
        context.SOSetTargets(0, nullptr, nullptr);
        context.GSSetShader(nullptr, nullptr, 0);
    }
    D3D11_BUFFER_DESC desc {};
    buffer.GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    check(SUCCEEDED(device.CreateBuffer(&desc, nullptr, staging.GetAddressOf())), "GPU geometry readback buffer");
    context.CopyResource(staging.Get(), &buffer);
    D3D11_MAPPED_SUBRESOURCE mapping {};
    check(SUCCEEDED(context.Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping)), "GPU geometry readback");
    const auto *words = static_cast<const UINT *>(mapping.pData);
    check(std::all_of(words, words + 12, [](UINT value) { return value == 0; }), "GPU really overwrote all geometry");
    context.Unmap(staging.Get(), 0);
}
}

int main()
{
    set_enabled(true);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        device.GetAddressOf(), nullptr, context.GetAddressOf())), "WARP device");
    check(com_vtable::patch(device.Get(), interface_layout::device(device.Get()),
        {Pixel::slot(), Vertex::slot(), Buffer::slot()}), "device creation observers");
    check(bind_device(*device.Get()), "input layout observer");
    install(context.Get());
    producer_fixture::ProducerFixture fixture(*device.Get());
    check(fixture.ready(), "actual 8FD4/7B6E shader fixture");
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = desc.Height = 16;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> color;
    check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, color.GetAddressOf())), "same source texture");
    const auto expect = [&](producer_fixture::Controls controls, ColorTransfer expected, const char *label)
    {
        check(fixture.record(*context.Get(), *color.Get(), controls), "actual producer draw");
        const auto actual = take_transfer(*context.Get(), *color.Get());
        std::printf("%s transfer=%u expected=%u\n", label, unsigned(actual), unsigned(expected));
        check(actual == expected, label);
        return pixel(*device.Get(), *context.Get(), *color.Get());
    };
    const auto first = expect({}, ColorTransfer::srgb, "cold full-frame Update");
    check(take_transfer(*context.Get(), *color.Get()) == ColorTransfer::unknown, "writer certificate consumed once");
    producer_fixture::Controls controls;
    controls.update_method = producer_fixture::UpdateMethod::map_discard;
    controls.linear_rgb = {.61f, .14f, .27f};
    const auto second = expect(controls, ColorTransfer::srgb, "Map-discard current values");
    check(first != second, "two actual colors differ");
    check((first & 255) > 90 && ((first >> 16) & 255) > 180, "nontrivial OETF pixel");
    controls.gamma = 1.25f;
    expect(controls, ColorTransfer::unknown, "same resource gamma changed");
    controls.gamma = 1;
    expect(controls, ColorTransfer::srgb, "same resource gamma recovered");
    controls.overlay = 1;
    expect(controls, ColorTransfer::unknown, "overlay cannot inherit encoding");
    controls.overlay = 0;
    controls.mode = 1;
    expect(controls, ColorTransfer::unknown, "PQ coordinates through unknown LUT");
    controls.mode = 0;
    controls.before_draw = [](auto &ctx)
    {
        const D3D11_VIEWPORT half {0,0,8,16,0,1};
        ctx.RSSetViewports(1, &half);
    };
    expect(controls, ColorTransfer::unknown, "half viewport rejected");
    controls.before_draw = [&](auto &ctx)
    {
        D3D11_RASTERIZER_DESC raster {};
        raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.ScissorEnable = TRUE;
        ComPtr<ID3D11RasterizerState> state;
        check(SUCCEEDED(device->CreateRasterizerState(&raster, state.GetAddressOf())), "scissor raster");
        const D3D11_RECT rect {0,0,8,16};
        ctx.RSSetState(state.Get()); ctx.RSSetScissorRects(1, &rect);
    };
    expect(controls, ColorTransfer::unknown, "partial scissor rejected");
    controls.before_draw = [&](auto &ctx)
    {
        D3D11_DEPTH_STENCIL_DESC depth {};
        depth.DepthEnable = TRUE; depth.DepthFunc = D3D11_COMPARISON_LESS;
        ComPtr<ID3D11DepthStencilState> state;
        check(SUCCEEDED(device->CreateDepthStencilState(&depth, state.GetAddressOf())), "depth state");
        ctx.OMSetDepthStencilState(state.Get(), 0);
    };
    expect(controls, ColorTransfer::unknown, "unknown depth occlusion rejected");
    controls.before_draw = [&](auto &ctx)
    {
        D3D11_DEPTH_STENCIL_DESC depth {};
        depth.DepthEnable = TRUE; depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ComPtr<ID3D11DepthStencilState> state;
        check(SUCCEEDED(device->CreateDepthStencilState(&depth, state.GetAddressOf())), "always depth state");
        ctx.OMSetDepthStencilState(state.Get(), 0);
    };
    expect(controls, ColorTransfer::srgb, "actual-game ALWAYS depth accepted");
    ComPtr<ID3D11Device1> device1;
    check(SUCCEEDED(device.As(&device1)), "Device1");
    controls.before_draw = [&](auto &ctx)
    {
        D3D11_BLEND_DESC1 blend {};
        blend.RenderTarget[0].RenderTargetWriteMask = 15;
        blend.RenderTarget[0].LogicOpEnable = TRUE;
        blend.RenderTarget[0].LogicOp = D3D11_LOGIC_OP_COPY_INVERTED;
        ComPtr<ID3D11BlendState1> state;
        check(SUCCEEDED(device1->CreateBlendState1(&blend, state.GetAddressOf())), "logic-op state");
        ctx.OMSetBlendState(state.Get(), nullptr, ~0u);
    };
    expect(controls, ColorTransfer::unknown, "logic op cannot masquerade as disabled blend");
    controls.before_draw = [&](auto &ctx)
    {
        const float points[] {-1,-1,-1,-1, 0,-1,0,-1, -1,0,-1,0};
        D3D11_BUFFER_DESC b {}; b.ByteWidth = sizeof(points); b.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA initial {points,0,0};
        ComPtr<ID3D11Buffer> vertices;
        check(SUCCEEDED(device->CreateBuffer(&b, &initial, vertices.GetAddressOf())), "small triangle buffer");
        const UINT stride = 16, offset = 0;
        ctx.IASetVertexBuffers(0,1,vertices.GetAddressOf(),&stride,&offset);
    };
    expect(controls, ColorTransfer::unknown, "same VS with partial geometry rejected");
    controls.before_draw = {};
    expect(controls, ColorTransfer::srgb, "full geometry recovered");
    check(fixture.record(*context.Get(), *color.Get()), "current before unknown copy");
    ComPtr<ID3D11Texture2D> other;
    check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, other.GetAddressOf())), "unknown source");
    context->CopyResource(color.Get(), other.Get());
    check(take_transfer(*context.Get(), *color.Get()) == ColorTransfer::unknown, "unknown overwrite cannot inherit");
    expect({}, ColorTransfer::srgb, "overwrite recovery needs fresh writer");
    controls.before_draw = [&](auto &ctx)
    {
        ComPtr<ID3D11Buffer> cb;
        ctx.PSGetConstantBuffers(0,1,cb.GetAddressOf());
        D3D11_BUFFER_DESC b {}; cb->GetDesc(&b); b.Usage = D3D11_USAGE_DEFAULT; b.CPUAccessFlags = 0;
        ComPtr<ID3D11Buffer> source;
        check(SUCCEEDED(device->CreateBuffer(&b,nullptr,source.GetAddressOf())), "unknown GPU CB");
        ctx.PSSetConstantBuffers(0,0,nullptr);
        ctx.CopyResource(cb.Get(),source.Get());
    };
    expect(controls, ColorTransfer::unknown, "GPU copy to CB invalidates CPU mirror");
    controls.before_draw = {};
    expect(controls, ColorTransfer::srgb, "CPU rewrite restores CB contract");
    controls.before_draw = [](auto &ctx)
    {
        ComPtr<ID3D11Buffer> cb;
        ctx.PSGetConstantBuffers(0,1,cb.GetAddressOf());
        ComPtr<ID3D11DeviceContext1> extended;
        check(SUCCEEDED(ctx.QueryInterface(IID_PPV_ARGS(extended.GetAddressOf()))), "Context1");
        const float gamma = 1;
        const D3D11_BOX box {324,0,0,328,1,1};
        extended->UpdateSubresource1(cb.Get(),0,&box,&gamma,0,0,0);
    };
    expect(controls, ColorTransfer::unknown, "four-byte boxed CB update rejected");
    controls.before_draw = {};
    expect(controls, ColorTransfer::srgb, "partial failure recovery");
    for (const bool stream_output : {false, true})
    {
        for (const bool indexed_geometry : {false, true})
        {
            std::uint32_t untouched = 0;
            controls.before_draw = [&](auto &ctx)
            {
                const float points[] {-1,-1,-1,-1, 3,-1,3,-1, -1,3,-1,3};
                const UINT indices[] {0,1,2,0,0,0,0,0,0,0,0,0};
                D3D11_BUFFER_DESC b {};
                b.ByteWidth = sizeof(points);
                b.BindFlags = (indexed_geometry ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER) |
                    (stream_output ? D3D11_BIND_STREAM_OUTPUT : D3D11_BIND_UNORDERED_ACCESS);
                b.MiscFlags = stream_output ? 0 : D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
                const D3D11_SUBRESOURCE_DATA initial {indexed_geometry ? static_cast<const void *>(indices) : points,0,0};
                ComPtr<ID3D11Buffer> geometry;
                check(SUCCEEDED(device->CreateBuffer(&b, &initial, geometry.GetAddressOf())), "GPU-writable geometry");
                zero_geometry(*device.Get(), ctx, *geometry.Get(), stream_output);
                ComPtr<ID3D11RenderTargetView> view;
                ctx.OMGetRenderTargets(1, view.GetAddressOf(), nullptr);
                const float marker[] {.05f,.10f,.15f,1};
                ctx.ClearRenderTargetView(view.Get(), marker);
                untouched = pixel(*device.Get(), ctx, *color.Get());
                if (indexed_geometry)
                {
                    ctx.IASetIndexBuffer(geometry.Get(), DXGI_FORMAT_R32_UINT, 0);
                    ctx.DrawIndexed(3, 0, 0);
                }
                else
                {
                    const UINT stride = 16, offset = 0;
                    ctx.IASetVertexBuffers(0, 1, geometry.GetAddressOf(), &stride, &offset);
                    ctx.Draw(3, 0);
                }
                check(pixel(*device.Get(), ctx, *color.Get()) == untouched, "degenerate GPU geometry left target untouched");
                check(take_transfer(ctx, *color.Get()) == ColorTransfer::unknown,
                    stream_output ? "SO-overwritten VB/IB cannot inherit CPU coverage" :
                        "UAV-overwritten VB/IB cannot inherit CPU coverage");
                // 夹具还会追加一条普通Draw；点拓扑使这条调用同样没有可认证的全屏覆盖。
                ctx.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            };
            const auto actual = expect(controls, ColorTransfer::unknown,
                stream_output ? "SO geometry rejects whole writer" : "UAV geometry rejects whole writer");
            check(actual == untouched, "no full-frame output from GPU-overwritten geometry");
        }
    }
    controls.before_draw = {};
    expect(controls, ColorTransfer::srgb, "ordinary immutable geometry still admitted");
    check(fixture.record(*context.Get(), *color.Get()), "current before command list");
    ComPtr<ID3D11DeviceContext> deferred;
    ComPtr<ID3D11CommandList> list;
    check(SUCCEEDED(device->CreateDeferredContext(0, deferred.GetAddressOf())), "deferred context");
    check(SUCCEEDED(deferred->FinishCommandList(FALSE, list.GetAddressOf())), "opaque list");
    context->ExecuteCommandList(list.Get(), TRUE);
    check(take_transfer(*context.Get(), *color.Get()) == ColorTransfer::unknown, "ExecuteCommandList invalidates epoch");
    check(fixture.record(*context.Get(), *color.Get()), "same immutable geometry after opaque");
    check(take_transfer(*context.Get(), *color.Get()) == ColorTransfer::srgb,
        "immutable creation contract preserves geometry across opaque epoch");
    std::printf("ProducerTransferTest PASS checks=%u\n", checks);
    return 0;
}
