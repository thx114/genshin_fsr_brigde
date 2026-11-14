#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>

// 2026-11-15：真实资源与像素校验区分公共调用透传和实际 Draw 捕获；
// 不伪造 il2cpp 参数，也不把识别到 FSR 候选当作 NGX 派发成功。
inline bool draw_capture(ID3D11Device *device, ID3D11DeviceContext *context)
{
    using Microsoft::WRL::ComPtr;
    const char shader[] =
        "float4 vs(uint n:SV_VertexID):SV_Position {"
        "return float4(n==2?3:-1,n==1?3:-1,0,1); }"
        "struct O { float4 a:SV_Target0; float4 b:SV_Target1; };"
        "O ps() { O o; o.a=float4(1,0,0,1); o.b=float4(0,1,0,1); return o; }";
    ComPtr<ID3DBlob> vs_code, ps_code, error;
    if (FAILED(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "vs", "vs_5_0",
        0, 0, &vs_code, &error)) || FAILED(D3DCompile(shader, sizeof(shader), nullptr, nullptr,
        nullptr, "ps", "ps_5_0", 0, 0, &ps_code, &error))) return false;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    if (FAILED(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs)) ||
        FAILED(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps)))
        return false;
    std::array<ComPtr<ID3D11Texture2D>, 9> textures;
    std::array<ComPtr<ID3D11ShaderResourceView>, 7> views;
    std::array<ID3D11ShaderResourceView *, 7> inputs {};
    std::array<ComPtr<ID3D11RenderTargetView>, 2> targets;
    ID3D11RenderTargetView *outputs[2] {};
    D3D11_TEXTURE2D_DESC desc {};
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    for (unsigned n = 0; n < textures.size(); ++n)
    {
        desc.Width = desc.Height = n == 1 ? 16 : n < 5 ? 64 : 128;
        desc.BindFlags = n < 7 ? D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &textures[n]))) return false;
        if (n < 7)
        {
            if (FAILED(device->CreateShaderResourceView(textures[n].Get(), nullptr, &views[n]))) return false;
            inputs[n] = views[n].Get();
        }
        else
        {
            if (FAILED(device->CreateRenderTargetView(textures[n].Get(), nullptr, &targets[n - 7]))) return false;
            outputs[n - 7] = targets[n - 7].Get();
        }
    }
    ComPtr<ID3D11Buffer> constants;
    D3D11_BUFFER_DESC cb {};
    cb.ByteWidth = 464;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &constants))) return false;
    D3D11_VIEWPORT viewport {0, 0, 128, 128, 0, 1};
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    context->PSSetShaderResources(0, 7, inputs.data());
    context->OMSetRenderTargets(2, outputs, nullptr);
    context->Draw(3, 0);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &readback))) return false;
    context->OMSetRenderTargets(0, nullptr, nullptr);
    for (unsigned n = 0; n < 2; ++n)
    {
        context->CopyResource(readback.Get(), textures[n + 7].Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        const auto *pixel = static_cast<const unsigned char *>(mapped.pData) + 64 * mapped.RowPitch + 64 * 4;
        const bool correct = pixel[n] == 255 && pixel[n ^ 1u] == 0 && pixel[2] == 0 && pixel[3] == 255;
        context->Unmap(readback.Get(), 0);
        if (!correct) return false;
    }
    context->ClearState();
    return SUCCEEDED(device->GetDeviceRemovedReason());
}
