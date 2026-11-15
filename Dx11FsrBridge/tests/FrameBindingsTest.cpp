#include "../runtime/FrameBindings.h"

#include <cstdio>

using Microsoft::WRL::ComPtr;
using neo::graphics::FrameBindings;

int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        device.GetAddressOf(), nullptr, context.GetAddressOf()))) return 2;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = desc.Height = 64;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> input, output, depth;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, input.GetAddressOf())) ||
        FAILED(device->CreateTexture2D(&desc, nullptr, output.GetAddressOf())) ||
        FAILED(device->CreateShaderResourceView(input.Get(), nullptr, srv.GetAddressOf())) ||
        FAILED(device->CreateRenderTargetView(output.Get(), nullptr, rtv.GetAddressOf()))) return 2;
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, depth.GetAddressOf())) ||
        FAILED(device->CreateDepthStencilView(depth.Get(), nullptr, dsv.GetAddressOf()))) return 2;
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
    context->PSSetShaderResources(2, 1, srv.GetAddressOf());

    unsigned failures = 0;
    const auto check = [&failures](bool result, const char *what)
    {
        if (!result)
        {
            ++failures;
            std::printf("FAIL: %s\n", what);
        }
    };
    {
        FrameBindings bindings(*context.Get());
        check(bindings.targets()[0] == rtv.Get() && !bindings.targets()[1], "only bound RTV is selected");
        check(bindings.scene_depth().Get() == depth.Get(), "scene depth lease uses the DSV resource");
        check(bindings.inputs()[2] == srv.Get() && !bindings.input_resource(0) && !bindings.input_resource(UINT(-1)),
            "empty and missing slots are not routed");
        check(FrameBindings::texture_2d(bindings.target_resource(0)).Get() == output.Get(), "correct output lease");
        context->ClearState();
        srv.Reset();
        rtv.Reset();
        check(FrameBindings::texture_2d(bindings.input_resource(2)).Get() == input.Get() &&
            FrameBindings::texture_2d(bindings.target_resource(0)).Get() == output.Get(),
            "same-draw lease owns views after rebinding and absence of caller references");
        check(!bindings.scene_depth(), "scene depth is queried on the execution context, not cached globally");
    }
    FrameBindings next(*context.Get());
    check(!next.input_resource(2) && !next.target_resource(0), "next Draw cannot reuse last Draw's bindings");

    D3D11_BUFFER_DESC buffer_desc {};
    buffer_desc.ByteWidth = 64;
    buffer_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(device->CreateBuffer(&buffer_desc, nullptr, buffer.GetAddressOf()))) return 2;
    check(!FrameBindings::texture_2d(FrameBindings::Resource(buffer.Get())), "non-texture resource rejected");
    std::printf("FrameBindings: %u failures\n", failures);
    return failures != 0;
}
