#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "../../runtime/CandidateSignature.h"

// 2026-11-15：真实资源与像素校验区分公共调用透传和实际 Draw 捕获；
// 不伪造 il2cpp 参数，也不把识别到 FSR 候选当作 NGX 派发成功。
enum class CaptureEntry
{
    draw, indexed, instanced, indexed_instanced, indirect, indexed_indirect, command_list, dispatch
};

struct CaptureFrame
{
    UINT render_size = 64;
    UINT display_size = 128;
    void (*publish_render)(UINT render, UINT display) = nullptr;
    bool expect_provider = false;
    UINT color_r10 = 32, color_g10 = 96, color_b10 = 900;
};

inline bool draw_capture(ID3D11Device *device, ID3D11DeviceContext *context,
    CaptureEntry entry = CaptureEntry::draw, const CaptureFrame &frame = {})
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
    const auto compile = [&](const char *name, const char *profile, ComPtr<ID3DBlob> &code) {
        return SUCCEEDED(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, name, profile,
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
    else if (!compile("vs", "vs_5_0", first_code) || !compile("ps", "ps_5_0", ps_code) ||
        FAILED(device->CreateVertexShader(first_code->GetBufferPointer(), first_code->GetBufferSize(), nullptr, &vs)) ||
        FAILED(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps)))
        return false;
    std::array<ComPtr<ID3D11Texture2D>, 9> textures;
    std::array<ComPtr<ID3D11ShaderResourceView>, 7> views;
    std::array<ID3D11ShaderResourceView *, 7> inputs {};
    std::array<ComPtr<ID3D11RenderTargetView>, 2> targets;
    std::array<ComPtr<ID3D11UnorderedAccessView>, 2> uavs;
    ID3D11RenderTargetView *outputs[2] {};
    ID3D11UnorderedAccessView *writes[2] {};
    D3D11_TEXTURE2D_DESC desc {};
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    auto pixels = frame.publish_render
        ? std::vector<std::uint32_t>(static_cast<std::size_t>(frame.display_size) * frame.display_size) :
          std::vector<std::uint32_t> {};
    // 非原生分支只可能由场景输入取得蓝色；原生目标一个红、一个绿。
    for (unsigned n = 0; n < textures.size(); ++n)
    {
        desc.Width = desc.Height = n == 1 ? 16 : n < 5 ? frame.render_size : frame.display_size;
        desc.Format = n == 2 ? DXGI_FORMAT_R32_FLOAT : n == 3 ?
            (frame.publish_render ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16_FLOAT) :
            (n == 0 && frame.publish_render ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM);
        desc.BindFlags = n < 7 ? D3D11_BIND_SHADER_RESOURCE :
            (compute ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_RENDER_TARGET);
        D3D11_SUBRESOURCE_DATA init {};
        if (frame.publish_render)
        {
            // 0.5/0.5 是 Genshin packed MV 的零运动，颜色让 RT 结果区别于原生像素。
            const auto value = n == 0 ? (0xC0000000u | (frame.color_b10 << 20) | (frame.color_g10 << 10) | frame.color_r10) :
                n == 2 ? 0x3f000000u : n == 3 ? (512u << 10) | 512u : 0u;
            std::fill(pixels.begin(), pixels.begin() + static_cast<std::size_t>(desc.Width) * desc.Height, value);
            init.pSysMem = pixels.data();
            init.SysMemPitch = desc.Width * sizeof(std::uint32_t);
        }
        if (FAILED(device->CreateTexture2D(&desc, init.pSysMem ? &init : nullptr, &textures[n]))) return false;
        if (n < 7)
        {
            if (FAILED(device->CreateShaderResourceView(textures[n].Get(), nullptr, &views[n]))) return false;
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
    ComPtr<ID3D11DeviceContext> deferred;
    if (entry == CaptureEntry::command_list && FAILED(device->CreateDeferredContext(0, &deferred))) return false;
    auto *commands = deferred ? deferred.Get() : context;
    ComPtr<ID3D11Buffer> constants, indices, arguments;
    if (!compute)
    {
        D3D11_BUFFER_DESC cb {};
        cb.ByteWidth = neo::graphics::k_min_fsr_constants;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        std::array<std::uint8_t, neo::graphics::k_min_fsr_constants> zero_constants {};
        const D3D11_SUBRESOURCE_DATA initial_cb {zero_constants.data(), 0, 0};
        if (FAILED(device->CreateBuffer(&cb, &initial_cb, &constants))) return false;
        const auto size = static_cast<float>(frame.display_size);
        D3D11_VIEWPORT viewport {0, 0, size, size, 0, 1};
        commands->RSSetViewports(1, &viewport);
        commands->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->VSSetShader(vs.Get(), nullptr, 0);
        commands->PSSetShader(ps.Get(), nullptr, 0);
        commands->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        commands->PSSetShaderResources(0, 7, inputs.data());
        commands->OMSetRenderTargets(2, outputs, nullptr);
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
    if (frame.publish_render) frame.publish_render(frame.render_size, frame.display_size);
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
        commands->Dispatch(16, 16, 1);
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
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &readback))) return false;
    context->OMSetRenderTargets(0, nullptr, nullptr);
    bool pixels_match = true;
    for (unsigned n = 0; n < 2; ++n)
    {
        context->CopyResource(readback.Get(), textures[n + 7].Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        const auto center = frame.display_size / 2;
        const auto *pixel = static_cast<const unsigned char *>(mapped.pData) + center * mapped.RowPitch + center * 4;
        const bool native_pixel = pixel[n] == 255 && pixel[n ^ 1u] == 0 && pixel[2] == 0 && pixel[3] == 255;
        const auto close_to_input = [](unsigned value, unsigned ten_bit)
        {
            const int expected = static_cast<int>((ten_bit * 255 + 511) / 1023);
            const int delta = static_cast<int>(value) - expected;
            return delta >= -26 && delta <= 26;
        };
        const bool provider_pixel = n == 0 ? pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 :
            close_to_input(pixel[0], frame.color_r10) && close_to_input(pixel[1], frame.color_g10) &&
            close_to_input(pixel[2], frame.color_b10);
        const bool correct = frame.expect_provider ? provider_pixel : native_pixel;
        if (frame.publish_render)
            std::printf("target=%u pixel=%u,%u,%u,%u expected=%s ok=%u\n", n,
                pixel[0], pixel[1], pixel[2], pixel[3], frame.expect_provider ? "provider" : "native", correct);
        context->Unmap(readback.Get(), 0);
        pixels_match &= correct;
    }
    return pixels_match && SUCCEEDED(device->GetDeviceRemovedReason());
}
