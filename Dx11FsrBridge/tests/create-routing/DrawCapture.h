#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <vector>
#include "../../runtime/CandidateSignature.h"

// 2026-11-15：真实资源与像素校验区分公共调用透传和实际 Draw 捕获；
// 不伪造 il2cpp 参数，也不把识别到 FSR 候选当作 NGX 派发成功。
enum class CaptureEntry
{
    draw, indexed, instanced, indexed_instanced, indirect, indexed_indirect, command_list, dispatch
};

inline constexpr std::uint32_t k_before_sr_scene_color = 0xFF286AA4u;
inline constexpr std::uint32_t k_before_sr_filtered_color = 0xFFC9512Bu;

struct CaptureExtent
{
    UINT width, height;
};

struct CaptureGuidePixel
{
    UINT x, y;
    std::uint32_t motion;
    float depth;
};

struct CaptureTextures
{
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 9> textures;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader;
};

enum class CaptureLayout { standard, reordered, lighting };

struct CaptureFrame
{
    CaptureExtent render {64, 64};
    CaptureExtent display {128, 128};
    std::function<void()> publish_render;
    bool expect_provider = false;
    UINT color_r10 = 32, color_g10 = 96, color_b10 = 900;
    DXGI_FORMAT color_source_view = DXGI_FORMAT_UNKNOWN;
    std::optional<std::uint32_t> exact_provider_pixel;
    std::uint32_t alternate_scene_pixel = k_before_sr_scene_color;
    std::span<const CaptureGuidePixel> guides;
    bool typeless_depth = false;
    bool replay_draw = false;
    DXGI_FORMAT scene_depth_format = DXGI_FORMAT_UNKNOWN;
    bool scene_depth_readable = false;
    bool expect_attenuated_color = false;
    DXGI_FORMAT motion_source_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT motion_source_view = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depth_source_format = DXGI_FORMAT_UNKNOWN;
    CaptureTextures *retained_textures = nullptr;
    bool producer_roundtrip = false;
    // 2026-11-16：真实writer必须在最终初始化之后写入SR本次读取的同一资源。
    std::function<bool(ID3D11DeviceContext &, ID3D11Texture2D &)> color_producer;
    bool producer_overwrite = false;
    std::function<bool()> producer_before_consume;
    bool producer_om_uav = false;
    int producer_write_operation = -1;
    CaptureLayout layout = CaptureLayout::standard;
    bool alias_history = false;
};

inline bool draw_capture(ID3D11Device *device, ID3D11DeviceContext *context,
    CaptureEntry entry = CaptureEntry::draw, const CaptureFrame &frame = {},
    std::FILE *pixel_receipt = nullptr)
{
    using Microsoft::WRL::ComPtr;
    const bool compute = entry == CaptureEntry::dispatch;
    const char shader[] =
        "float4 vs(uint n:SV_VertexID):SV_Position {"
        "return float4(n==2?3:-1,n==1?3:-1,0,1); }"
        "struct O { float4 a:SV_Target0; float4 b:SV_Target1; };"
        "O ps() { O o; o.a=float4(1,0,0,1); o.b=float4(0,1,0,1); return o; }"
        "RWTexture2D<float4> a:register(u0), b:register(u1);"
        "[numthreads(8,8,1)] void cs(uint3 n:SV_DispatchThreadID) {"
        "a[n.xy]=float4(1,0,0,1); b[n.xy]=float4(0,1,0,1); }";
    // 2026-11-16（PID44888）：照明型夹具确实解码法线/读取衰减，不用 shader 哈希伪造 temporal 证据。
    const char lighting_shader[] =
        "float4 vs(uint n:SV_VertexID):SV_Position {"
        "return float4(n==2?3:-1,n==1?3:-1,0,1); }"
        "Texture2D<float4> normal:register(t4); Texture2D<float2> attenuation:register(t1);"
        "struct O { float4 color:SV_Target0; uint flags:SV_Target1; };"
        "O ps(float4 position:SV_Position) { int3 p=int3(position.xy,0);"
        "float3 n=normalize(normal.Load(p).xyz*2-1); float2 a=attenuation.Load(p);"
        "O o; o.color=float4(n.x>0?1:0,a.x<0?1:0,a.y<0?1:0,1); o.flags=0; return o; }";
    const auto compile = [&](const char *name, const char *profile, ComPtr<ID3DBlob> &code) {
        const bool lighting = frame.layout == CaptureLayout::lighting;
        return SUCCEEDED(D3DCompile(lighting ? lighting_shader : shader,
            lighting ? sizeof(lighting_shader) : sizeof(shader), nullptr, nullptr, nullptr, name, profile,
            0, 0, &code, nullptr));
    };
    ComPtr<ID3DBlob> first_code, ps_code;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    if (compute)
    {
        if (!compile("cs", "cs_5_0", first_code) || FAILED(device->CreateComputeShader(first_code->GetBufferPointer(),
            first_code->GetBufferSize(), nullptr, &cs))) return false;
    }
    else if (frame.retained_textures && frame.retained_textures->vertex_shader &&
        frame.retained_textures->pixel_shader)
    {
        vs = frame.retained_textures->vertex_shader;
        ps = frame.retained_textures->pixel_shader;
    }
    else if (!compile("vs", "vs_5_0", first_code) || !compile("ps", "ps_5_0", ps_code) ||
        FAILED(device->CreateVertexShader(first_code->GetBufferPointer(), first_code->GetBufferSize(), nullptr, &vs)) ||
        FAILED(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps)))
        return false;
    if (frame.retained_textures && !compute)
    {
        frame.retained_textures->vertex_shader = vs;
        frame.retained_textures->pixel_shader = ps;
    }
    std::array<ComPtr<ID3D11Texture2D>, 9> textures;
    std::array<ComPtr<ID3D11ShaderResourceView>, 7> views;
    std::array<ID3D11ShaderResourceView *, 7> inputs {};
    std::array<ComPtr<ID3D11RenderTargetView>, 2> targets;
    std::array<ComPtr<ID3D11UnorderedAccessView>, 2> uavs;
    ID3D11RenderTargetView *outputs[2] {};
    ID3D11UnorderedAccessView *writes[2] {};
    D3D11_TEXTURE2D_DESC desc {};
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    std::vector<std::uint32_t> pixels;
    std::vector<std::uint64_t> wide_depth;
    std::vector<std::uint16_t> narrow_motion;
    // 非原生分支只可能由场景输入取得蓝色；原生目标一个红、一个绿。
    for (unsigned n = 0; n < textures.size(); ++n)
    {
        const auto size = n == 1 ? CaptureExtent {16, 16} : n < 5 ? frame.render : frame.display;
        desc.Width = size.width;
        desc.Height = size.height;
        desc.Format = n == 2 ? DXGI_FORMAT_R32_FLOAT : n == 3 ?
            (frame.publish_render ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16_FLOAT) :
            (n == 0 && frame.publish_render ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM);
        // 2026-11-16（完整矩阵 mirrored 失败）：默认正例也必须提供单通道权重，不能依赖旧宽松判据。
        // flags 也保持真实单通道；1:1 的 RGBA 占位 flags 会被合法分配为另一幅 history，污染别名负控。
        if (n == 1 || n == 4) desc.Format = DXGI_FORMAT_R8_UNORM;
        if (n == 1 && frame.layout != CaptureLayout::standard)
            desc.Format = frame.layout == CaptureLayout::lighting ?
                DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16_UNORM;
        if (frame.layout == CaptureLayout::reordered && (n == 5 || n == 7))
            desc.Format = DXGI_FORMAT_R8G8_UNORM;
        if (frame.layout == CaptureLayout::lighting && n == 0)
            desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        if (n == 3 && frame.motion_source_format != DXGI_FORMAT_UNKNOWN)
            desc.Format = frame.motion_source_format;
        const bool alternate_color = n == 0 && frame.color_source_view != DXGI_FORMAT_UNKNOWN;
        const bool typeless_depth = n == 2 && frame.typeless_depth;
        if (alternate_color) desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        if (typeless_depth) desc.Format = DXGI_FORMAT_R32_TYPELESS;
        if (n == 2 && frame.depth_source_format != DXGI_FORMAT_UNKNOWN)
            desc.Format = frame.depth_source_format;
        desc.BindFlags = n < 7 ? D3D11_BIND_SHADER_RESOURCE :
            (compute ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_RENDER_TARGET);
        if (n == 0 && (frame.producer_roundtrip || frame.color_producer)) desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
        if (n == 0 && frame.producer_om_uav) desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        if (n == 2 && desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS)
            desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
        if (n == 3 && desc.Format == DXGI_FORMAT_R8G8_TYPELESS)
            desc.BindFlags |= D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
        D3D11_SUBRESOURCE_DATA init {};
        if (frame.publish_render)
        {
            // 0.5/0.5 是 Genshin packed MV 的零运动，颜色让 RT 结果区别于原生像素。
            const auto packed_scene = 0xC0000000u | (frame.color_b10 << 20) | (frame.color_g10 << 10) | frame.color_r10;
            const auto value = n == 0 ? (alternate_color ? frame.alternate_scene_pixel : packed_scene) :
                n == 2 ? 0x3f000000u : n == 3 ? (512u << 10) | 512u : 0u;
            pixels.assign(static_cast<std::size_t>(desc.Width) * desc.Height, value);
            // 2026-11-16：三通道法线不与 packed MV 同属 R10，避免旧评分把唯一 motion 同时选作 color。
            // 真实 R10 法线/RG8 衰减组合由 CandidateSignatureTest 的原始绑定用例单独覆盖。
            if (frame.layout == CaptureLayout::lighting && n == 0)
                std::fill(pixels.begin(), pixels.end(), 960u | (960u << 11) | (480u << 22));
            if (n == 2 || n == 3)
            {
                // 2026-11-15（PR #16）：四个不同坐标，才能检测轴互换、旧内容复用以及错误读回行距。
                for (const auto &probe : frame.guides)
                {
                    if (probe.x >= desc.Width || probe.y >= desc.Height) return false;
                    const auto index = static_cast<std::size_t>(probe.y) * desc.Width + probe.x;
                    pixels[index] = n == 2 ? std::bit_cast<std::uint32_t>(probe.depth) : probe.motion;
                }
            }
            init.pSysMem = pixels.data();
            init.SysMemPitch = desc.Width * sizeof(std::uint32_t);
            if (n == 1 && desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
            {
                wide_depth.assign(pixels.size(), 0);
                init.pSysMem = wide_depth.data();
                init.SysMemPitch = desc.Width * sizeof(std::uint64_t);
            }
            if (n == 2 && desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS)
            {
                wide_depth.assign(pixels.size(), 0x3F000000ull);
                init.pSysMem = wide_depth.data();
                init.SysMemPitch = desc.Width * sizeof(std::uint64_t);
            }
            if (n == 3 && desc.Format == DXGI_FORMAT_R8G8_TYPELESS)
            {
                narrow_motion.assign(pixels.size(), 0x8080u);
                init.pSysMem = narrow_motion.data();
                init.SysMemPitch = desc.Width * sizeof(std::uint16_t);
            }
        }
        if (frame.retained_textures && frame.retained_textures->textures[n])
        {
            const auto &previous = frame.retained_textures->textures[n];
            D3D11_TEXTURE2D_DESC old {};
            previous->GetDesc(&old);
            if (old.Width == desc.Width && old.Height == desc.Height &&
                old.Format == desc.Format && old.BindFlags == desc.BindFlags)
                textures[n] = previous;
        }
        if (textures[n])
        {
            if (init.pSysMem) context->UpdateSubresource(textures[n].Get(), 0, nullptr,
                init.pSysMem, init.SysMemPitch, 0);
        }
        else if (FAILED(device->CreateTexture2D(&desc, init.pSysMem ? &init : nullptr, &textures[n])))
            return false;
        if (frame.retained_textures) frame.retained_textures->textures[n] = textures[n];
        if (n < 7)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC view {};
            view.Format = typeless_depth ? DXGI_FORMAT_R32_FLOAT : frame.color_source_view;
            if (n == 2 && desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS)
                view.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            if (n == 3 && frame.motion_source_view != DXGI_FORMAT_UNKNOWN)
                view.Format = frame.motion_source_view;
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = 1;
            const auto *explicit_view = alternate_color || typeless_depth ||
                (n == 2 && desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS) ||
                (n == 3 && frame.motion_source_view != DXGI_FORMAT_UNKNOWN) ? &view : nullptr;
            if (FAILED(device->CreateShaderResourceView(textures[n].Get(), explicit_view, &views[n]))) return false;
            inputs[n] = views[n].Get();
        }
        else if (compute)
        {
            if (FAILED(device->CreateUnorderedAccessView(textures[n].Get(), nullptr, &uavs[n - 7]))) return false;
            writes[n - 7] = uavs[n - 7].Get();
        }
        else
        {
            if (FAILED(device->CreateRenderTargetView(textures[n].Get(), nullptr, &targets[n - 7]))) return false;
            outputs[n - 7] = targets[n - 7].Get();
        }
    }
    if (frame.retained_textures)
    {
        // 2026-11-16（方法槽实测）：WARP 更新旧资源会更换 Draw 入口；真实 Map 走既有路由恢复，
        // 同时独立核验输入已更新，避免将夹具绕过钩子误判为多实例匹配失败。
        D3D11_TEXTURE2D_DESC sample_desc {};
        textures[0]->GetDesc(&sample_desc);
        sample_desc.Width = sample_desc.Height = 1;
        sample_desc.Usage = D3D11_USAGE_STAGING;
        sample_desc.BindFlags = 0;
        sample_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> sample;
        if (FAILED(device->CreateTexture2D(&sample_desc, nullptr, &sample))) return false;
        const D3D11_BOX center {frame.render.width / 2, frame.render.height / 2, 0,
            frame.render.width / 2 + 1, frame.render.height / 2 + 1, 1};
        context->CopySubresourceRegion(sample.Get(), 0, 0, 0, 0, textures[0].Get(), 0, &center);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(sample.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        std::uint32_t actual = 0;
        std::memcpy(&actual, mapped.pData, sizeof(actual));
        context->Unmap(sample.Get(), 0);
        if (actual != frame.alternate_scene_pixel) return false;
    }
    ComPtr<ID3D11Texture2D> scene_depth;
    ComPtr<ID3D11DepthStencilView> scene_dsv;
    if (frame.scene_depth_format != DXGI_FORMAT_UNKNOWN)
    {
        D3D11_TEXTURE2D_DESC scene_desc {};
        scene_desc.Width = frame.display.width;
        scene_desc.Height = frame.display.height;
        scene_desc.MipLevels = scene_desc.ArraySize = scene_desc.SampleDesc.Count = 1;
        scene_desc.Format = frame.scene_depth_format;
        scene_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL |
            (frame.scene_depth_readable ? D3D11_BIND_SHADER_RESOURCE : 0u);
        D3D11_DEPTH_STENCIL_VIEW_DESC view {};
        view.Format = DXGI_FORMAT_D32_FLOAT;
        view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        // 2026-11-16：WARP 的 ClearDepthStencilView 会重置 Draw 方法槽，绕过被测桥。
        // 创建时写入深度，保证本用例实际经过同一条捕获入口。
        const std::vector<float> depth_pixels(static_cast<std::size_t>(scene_desc.Width) * scene_desc.Height, 0.25f);
        const D3D11_SUBRESOURCE_DATA initial_depth { depth_pixels.data(), scene_desc.Width * sizeof(float), 0 };
        if (FAILED(device->CreateTexture2D(&scene_desc, &initial_depth, &scene_depth)) ||
            FAILED(device->CreateDepthStencilView(scene_depth.Get(), &view, &scene_dsv))) return false;
    }
    ComPtr<ID3D11DeviceContext> deferred;
    if (entry == CaptureEntry::command_list && FAILED(device->CreateDeferredContext(0, &deferred))) return false;
    auto *commands = deferred ? deferred.Get() : context;
    if (frame.color_producer && !frame.color_producer(*commands, *textures[0].Get())) return false;
    ComPtr<ID3D11Buffer> constants, indices, arguments;
    const auto bind_graphics = [&]
    {
        D3D11_VIEWPORT viewport {0, 0, static_cast<float>(frame.display.width),
            static_cast<float>(frame.display.height), 0, 1};
        commands->RSSetViewports(1, &viewport);
        commands->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->VSSetShader(vs.Get(), nullptr, 0);
        commands->PSSetShader(ps.Get(), nullptr, 0);
        commands->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        auto bound_inputs = inputs;
        std::array<ID3D11RenderTargetView *, 2> bound_outputs {outputs[0], outputs[1]};
        if (frame.alias_history) bound_inputs[6] = bound_inputs[0];
        if (frame.layout == CaptureLayout::reordered)
        {
            bound_inputs = {inputs[1], inputs[3], inputs[5], inputs[0], inputs[4], inputs[6], inputs[2]};
            bound_outputs = {outputs[1], outputs[0]};
        }
        if (frame.layout == CaptureLayout::lighting)
            bound_inputs = {inputs[2], inputs[3], inputs[4], inputs[1], inputs[0], inputs[5], inputs[6]};
        commands->PSSetShaderResources(0, 7, bound_inputs.data());
        commands->OMSetRenderTargets(frame.layout == CaptureLayout::lighting ? 1 : 2,
            bound_outputs.data(), scene_dsv.Get());
        if (scene_dsv)
        {
            ComPtr<ID3D11DepthStencilView> bound;
            commands->OMGetRenderTargets(0, nullptr, &bound);
            if (bound != scene_dsv) return false;
        }
        return true;
    };
    if (!compute)
    {
        D3D11_BUFFER_DESC cb {};
        cb.ByteWidth = neo::graphics::k_min_fsr_constants;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        std::array<std::uint8_t, neo::graphics::k_min_fsr_constants> zero_constants {};
        const D3D11_SUBRESOURCE_DATA initial_cb {zero_constants.data(), 0, 0};
        if (FAILED(device->CreateBuffer(&cb, &initial_cb, &constants))) return false;
        if (!bind_graphics()) return false;
    }
    if (entry == CaptureEntry::indexed || entry == CaptureEntry::indexed_instanced ||
        entry == CaptureEntry::indexed_indirect)
    {
        const std::uint16_t vertices[] = {0, 1, 2};
        D3D11_BUFFER_DESC ib {};
        ib.ByteWidth = sizeof(vertices);
        ib.Usage = D3D11_USAGE_IMMUTABLE;
        ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA initial {vertices};
        if (FAILED(device->CreateBuffer(&ib, &initial, &indices))) return false;
        commands->IASetIndexBuffer(indices.Get(), DXGI_FORMAT_R16_UINT, 0);
    }
    if (entry == CaptureEntry::indirect || entry == CaptureEntry::indexed_indirect)
    {
        const UINT args[] = {3, 1, 0, 0, 0};
        D3D11_BUFFER_DESC indirect {};
        indirect.ByteWidth = sizeof(args);
        indirect.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        const D3D11_SUBRESOURCE_DATA initial {args};
        if (FAILED(device->CreateBuffer(&indirect, &initial, &arguments))) return false;
    }
    if (frame.publish_render) frame.publish_render();
    if (frame.layout == CaptureLayout::lighting && pixel_receipt)
    {
        D3D11_TEXTURE2D_DESC normal_desc {}, motion_desc {};
        textures[0]->GetDesc(&normal_desc);
        textures[3]->GetDesc(&motion_desc);
        std::fprintf(pixel_receipt, "lighting binding: normal_format=%u motion_format=%u one_rtv=1\n",
            normal_desc.Format, motion_desc.Format);
        if (normal_desc.Format != DXGI_FORMAT_R11G11B10_FLOAT) return false;
    }
    switch (entry)
    {
    case CaptureEntry::draw:
    case CaptureEntry::command_list:
        commands->Draw(3, 0);
        break;
    case CaptureEntry::indexed:
        commands->DrawIndexed(3, 0, 0);
        break;
    case CaptureEntry::instanced:
        commands->DrawInstanced(3, 1, 0, 0);
        break;
    case CaptureEntry::indexed_instanced:
        commands->DrawIndexedInstanced(3, 1, 0, 0, 0);
        break;
    case CaptureEntry::indirect:
        commands->DrawInstancedIndirect(arguments.Get(), 0);
        break;
    case CaptureEntry::indexed_indirect:
        commands->DrawIndexedInstancedIndirect(arguments.Get(), 0);
        break;
    case CaptureEntry::dispatch:
        commands->CSSetShader(cs.Get(), nullptr, 0);
        commands->CSSetUnorderedAccessViews(0, 2, writes, nullptr);
        commands->Dispatch((frame.display.width + 7) / 8, (frame.display.height + 7) / 8, 1);
        writes[0] = writes[1] = nullptr;
        commands->CSSetUnorderedAccessViews(0, 2, writes, nullptr);
        break;
    }
    if (deferred)
    {
        ComPtr<ID3D11CommandList> list;
        if (FAILED(deferred->FinishCommandList(FALSE, &list))) return false;
        context->ExecuteCommandList(list.Get(), FALSE);
    }
    if (scene_dsv)
    {
        ComPtr<ID3D11DepthStencilView> restored;
        context->OMGetRenderTargets(0, nullptr, &restored);
        if (restored != scene_dsv) return false;
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &readback))) return false;
    context->OMSetRenderTargets(0, nullptr, nullptr);
    const auto read_outputs = [&](std::optional<std::uint32_t> expected)
    {
        bool pixels_match = true;
        for (unsigned n = 0; n < (frame.layout == CaptureLayout::lighting ? 1u : 2u); ++n)
        {
            const bool metadata_pair = frame.layout == CaptureLayout::reordered && n == 0;
            auto target_readback = readback;
            if (metadata_pair)
            {
                auto metadata_desc = desc;
                metadata_desc.Format = DXGI_FORMAT_R8G8_UNORM;
                if (FAILED(device->CreateTexture2D(&metadata_desc, nullptr, &target_readback))) return false;
            }
            context->CopyResource(target_readback.Get(), textures[n + 7].Get());
            D3D11_MAPPED_SUBRESOURCE mapped {};
            if (FAILED(context->Map(target_readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
            const auto *pixel = static_cast<const unsigned char *>(mapped.pData) +
                (frame.display.height / 2) * mapped.RowPitch + (frame.display.width / 2) * (metadata_pair ? 2 : 4);
            const bool native_pixel = pixel[n] == 255 && pixel[n ^ 1u] == 0 &&
                (metadata_pair || (pixel[2] == 0 && pixel[3] == 255));
            const auto close_to_input = [](unsigned value, unsigned ten_bit)
            {
                const int expected = static_cast<int>((ten_bit * 255 + 511) / 1023);
                const int delta = static_cast<int>(value) - expected;
                return delta >= -26 && delta <= 26;
            };
            std::uint32_t raw {};
            std::memcpy(&raw, pixel, sizeof(raw));
            const auto attenuated = [&]
            {
                if (pixel[3] != (frame.alternate_scene_pixel >> 24)) return false;
                for (unsigned i = 0; i < 3; ++i)
                {
                    const unsigned original = (frame.alternate_scene_pixel >> (8u * i)) & 255u;
                    if (pixel[i] + 2u >= original || pixel[i] * 2u < original) return false;
                }
                return true;
            };
            const bool provider_pixel = n == 0 ? pixel[0] == 0 && pixel[1] == 0 && (metadata_pair || pixel[2] == 0) :
                frame.expect_attenuated_color ? attenuated() :
                expected ? raw == *expected :
                close_to_input(pixel[0], frame.color_r10) && close_to_input(pixel[1], frame.color_g10) &&
                close_to_input(pixel[2], frame.color_b10);
            const bool correct = frame.expect_provider ? provider_pixel : native_pixel;
            if (pixel_receipt)
                std::fprintf(pixel_receipt, "target=%u pixel=%u,%u,%u,%u expected=%s ok=%u\n", n,
                    pixel[0], pixel[1], pixel[2], pixel[3], frame.expect_provider ? "provider" : "native", correct);
            context->Unmap(target_readback.Get(), 0);
            pixels_match &= correct;
        }
        return pixels_match;
    };
    bool pixels_match = read_outputs(frame.exact_provider_pixel);
    if (pixels_match && frame.producer_roundtrip)
    {
        // 2026-11-16：首个 SDK 已武装这张源颜色；现在用真实 Draw 写同一资源。
        // 后续更新 CB，再进入 SDK，验证捕获的是 writer 时刻而非 SDK 时刻的常量。
        ComPtr<ID3D11RenderTargetView> producer;
        D3D11_RENDER_TARGET_VIEW_DESC view {};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (compute || !frame.publish_render ||
            FAILED(device->CreateRenderTargetView(textures[0].Get(), &view, producer.GetAddressOf()))) return false;
        std::array<ID3D11ShaderResourceView *, 7> none {};
        commands->PSSetShaderResources(0, 7, none.data());
        commands->OMSetRenderTargets(1, producer.GetAddressOf(), nullptr);
        D3D11_VIEWPORT viewport {0, 0, static_cast<float>(frame.render.width),
            static_cast<float>(frame.render.height), 0, 1};
        commands->RSSetViewports(1, &viewport);
        std::array<std::uint32_t, neo::graphics::k_min_fsr_constants / 4> values;
        values.fill(0x3f400000);
        commands->UpdateSubresource(constants.Get(), 0, nullptr, values.data(), 0, 0);
        commands->Draw(3, 0);
        commands->OMSetRenderTargets(0, nullptr, nullptr);
        values.fill(0);
        commands->UpdateSubresource(constants.Get(), 0, nullptr, values.data(), 0, 0);
        if (frame.producer_overwrite)
        {
            // 旧状态表仍记住另一 RTV，实际 OM UAV 接口恢复源颜色后再写一次。
            commands->OMSetRenderTargets(1, outputs, nullptr);
            commands->OMSetRenderTargetsAndUnorderedAccessViews(1, producer.GetAddressOf(), nullptr,
                1, 0, nullptr, nullptr);
            commands->Draw(3, 0);
            commands->OMSetRenderTargets(0, nullptr, nullptr);
        }
        if (frame.producer_om_uav)
        {
            // 2026-11-16：实际像素着色器经 OM UAV 覆盖源颜色，RTV 绑定看不到这笔 writer。
            constexpr char uav_shader[] = "RWTexture2D<float4> color:register(u1);"
                "void main(float4 p:SV_Position){color[uint2(p.xy)]=float4(0,1,0,1);}";
            ComPtr<ID3DBlob> code;
            ComPtr<ID3D11PixelShader> uav_pixel;
            ComPtr<ID3D11UnorderedAccessView> source_uav;
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav_view {};
            uav_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            uav_view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (FAILED(D3DCompile(uav_shader, sizeof(uav_shader), nullptr, nullptr, nullptr, "main", "ps_5_0",
                0, 0, &code, nullptr)) || FAILED(device->CreatePixelShader(code->GetBufferPointer(),
                    code->GetBufferSize(), nullptr, &uav_pixel)) ||
                FAILED(device->CreateUnorderedAccessView(textures[0].Get(), &uav_view, &source_uav))) return false;
            commands->PSSetShader(uav_pixel.Get(), nullptr, 0);
            commands->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1,
                source_uav.GetAddressOf(), nullptr);
            commands->Draw(3, 0);
            ID3D11UnorderedAccessView *none_uav = nullptr;
            commands->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1, &none_uav, nullptr);
        }
        if (frame.producer_write_operation >= 0)
        {
            // 2026-11-16：实际改写 SDK 颜色后再消费；四种入口必须使旧 writer 失配。
            D3D11_TEXTURE2D_DESC source_desc {};
            textures[0]->GetDesc(&source_desc);
            source_desc.BindFlags = 0;
            std::vector<std::uint32_t> green(static_cast<std::size_t>(source_desc.Width) * source_desc.Height,
                0xff00ff00);
            const D3D11_SUBRESOURCE_DATA data {green.data(), source_desc.Width * 4u, 0};
            ComPtr<ID3D11Texture2D> source;
            if (FAILED(device->CreateTexture2D(&source_desc, &data, &source))) return false;
            switch (frame.producer_write_operation)
            {
            case 0: commands->CopyResource(textures[0].Get(), source.Get()); break;
            case 1:
                commands->CopySubresourceRegion(textures[0].Get(), 0, 0, 0, 0, source.Get(), 0, nullptr);
                break;
            case 2:
            {
                const FLOAT value[] {0, 1, 0, 1};
                commands->ClearRenderTargetView(producer.Get(), value);
                break;
            }
            case 3:
                commands->UpdateSubresource(textures[0].Get(), 0, nullptr, green.data(), data.SysMemPitch, 0);
                break;
            default: return false;
            }
        }
        if (frame.producer_before_consume)
        {
            // 2026-11-16：测试宿主先实读源像素，保证 writer 前的 CB 快照已退休，
            // 再让另一实例 SDK 先到；不能靠 GPU 尚未完成使错误的报告门控碰巧通过。
            D3D11_TEXTURE2D_DESC sample_desc {};
            textures[0]->GetDesc(&sample_desc);
            sample_desc.Width = sample_desc.Height = 1;
            sample_desc.Usage = D3D11_USAGE_STAGING;
            sample_desc.BindFlags = 0;
            sample_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> sample;
            if (FAILED(device->CreateTexture2D(&sample_desc, nullptr, &sample))) return false;
            const D3D11_BOX center {frame.render.width / 2, frame.render.height / 2, 0,
                frame.render.width / 2 + 1, frame.render.height / 2 + 1, 1};
            context->CopySubresourceRegion(sample.Get(), 0, 0, 0, 0, textures[0].Get(), 0, &center);
            D3D11_MAPPED_SUBRESOURCE mapped {};
            if (FAILED(context->Map(sample.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
            std::uint32_t actual = 0;
            std::memcpy(&actual, mapped.pData, sizeof(actual));
            context->Unmap(sample.Get(), 0);
            if (actual != 0xff0000ff || !frame.producer_before_consume()) return false;
        }
        for (unsigned next = 0; next < 3; ++next)
        {
            constexpr FLOAT clear[4] {};
            for (auto *output : outputs) commands->ClearRenderTargetView(output, clear);
            if (!bind_graphics()) return false;
            std::array<ID3D11ShaderResourceView *, 7> checked {};
            commands->PSGetShaderResources(0, 7, checked.data());
            bool bindings_ok = true;
            for (unsigned i = 0; i < checked.size(); ++i)
            {
                bindings_ok &= checked[i] == inputs[i];
                if (pixel_receipt) std::fprintf(pixel_receipt, "producer binding round=%u slot=%u exact=%u\n",
                    next, i, checked[i] == inputs[i]);
                if (checked[i]) checked[i]->Release();
            }
            if (!bindings_ok) return false;
            frame.publish_render();
            commands->Draw(3, 0);
            commands->OMSetRenderTargets(0, nullptr, nullptr);
            pixels_match &= read_outputs(frame.producer_om_uav || frame.producer_write_operation >= 0 ?
                0xff00ff00 : 0xff0000ff);
        }
    }
    if (pixels_match && frame.replay_draw && entry == CaptureEntry::draw)
    {
        // 2026-11-15（PR #16）：先验首笔写回。重复 Draw 可能回落原生，另由宿主验 Evaluate 次数。
        if (!bind_graphics()) return false;
        commands->Draw(3, 0);
        commands->OMSetRenderTargets(0, nullptr, nullptr);
    }
    return pixels_match && SUCCEEDED(device->GetDeviceRemovedReason());
}
