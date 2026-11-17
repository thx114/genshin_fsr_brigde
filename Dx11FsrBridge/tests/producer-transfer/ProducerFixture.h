#pragma once

#include "PixelBytecode.h"
#include "VertexBytecode.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>

namespace producer_fixture
{
enum class UpdateMethod { update, map_discard };

struct Controls
{
    float mode = 0, gamma = 1, overlay = 0, exposure = 0;
    std::array<float, 3> linear_rgb {0.18f, 0.42f, 0.73f};
    UpdateMethod update_method = UpdateMethod::update;
    // 仅宿主构造当前状态负例；不声明或修改产品证书。
    std::function<void(ID3D11DeviceContext &)> before_draw;
};

// 2026-11-16：复用39776/44888真实writer，不能让联合宿主伪造sRGB声明。
// PS原始SHA256=8FD4A4A2315A495B7855B3D81A113848BE19F1934D90547669ED770D70FCD15B。
// 本夹具只发原生D3D命令；不调用producer认证、BeforeSR查询或注册接口，不自行创建device。
class ProducerFixture
{
public:
    explicit ProducerFixture(ID3D11Device &device) noexcept
    {
        if (FAILED(device.CreatePixelShader(k_pixel_bytecode, sizeof(k_pixel_bytecode), nullptr,
                pixel_.GetAddressOf())) ||
            FAILED(device.CreateVertexShader(k_vertex_bytecode, sizeof(k_vertex_bytecode), nullptr,
                vertex_.GetAddressOf()))) return;

        constexpr D3D11_INPUT_ELEMENT_DESC elements[] {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        if (FAILED(device.CreateInputLayout(elements, 2, k_vertex_bytecode, sizeof(k_vertex_bytecode),
                layout_.GetAddressOf()))) return;
        constexpr float vertices[] {-1, -1, -1, -1, 3, -1, 3, -1, -1, 3, -1, 3};
        D3D11_BUFFER_DESC buffer {};
        buffer.ByteWidth = sizeof(vertices);
        buffer.Usage = D3D11_USAGE_IMMUTABLE;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA initial {vertices, 0, 0};
        if (FAILED(device.CreateBuffer(&buffer, &initial, vertices_.GetAddressOf()))) return;
        buffer.ByteWidth = sizeof(constants_bytes_);
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.Usage = D3D11_USAGE_DEFAULT;
        const D3D11_SUBRESOURCE_DATA zeros {constants_bytes_.data(), 0, 0};
        if (FAILED(device.CreateBuffer(&buffer, &zeros, constants_.GetAddressOf()))) return;
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device.CreateBuffer(&buffer, &zeros, mapped_constants_.GetAddressOf()))) return;

        D3D11_SAMPLER_DESC sampler {};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device.CreateSamplerState(&sampler, sampler_.GetAddressOf()))) return;
        D3D11_DEPTH_STENCIL_DESC depth {};
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (FAILED(device.CreateDepthStencilState(&depth, depth_.GetAddressOf()))) return;
        D3D11_RASTERIZER_DESC raster {};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        if (FAILED(device.CreateRasterizerState(&raster, raster_.GetAddressOf()))) return;
        ready_ = true;
    }

    bool ready() const noexcept { return ready_; }

    bool record(ID3D11DeviceContext &context, ID3D11Texture2D &target, const Controls &controls = {}) noexcept
    {
        if (!ready_ || context.GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
        D3D11_TEXTURE2D_DESC desc {};
        target.GetDesc(&desc);
        if ((desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) ||
            desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1 ||
            !(desc.BindFlags & D3D11_BIND_RENDER_TARGET)) return false;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        target.GetDevice(device.GetAddressOf());
        D3D11_RENDER_TARGET_VIEW_DESC rtv {};
        rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output;
        if (FAILED(device->CreateRenderTargetView(&target, &rtv, output.GetAddressOf()))) return false;

        // R11G11B10真实取样域；Clear只设置夹具的线性输入，不参与产品认证。
        D3D11_TEXTURE2D_DESC source {};
        source.Width = desc.Width; source.Height = desc.Height;
        source.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        source.MipLevels = source.ArraySize = source.SampleDesc.Count = 1;
        source.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> input;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> clear;
        if (FAILED(device->CreateTexture2D(&source, nullptr, input.GetAddressOf())) ||
            FAILED(device->CreateShaderResourceView(input.Get(), nullptr, view.GetAddressOf())) ||
            FAILED(device->CreateRenderTargetView(input.Get(), nullptr, clear.GetAddressOf()))) return false;
        const float rgb[] {controls.linear_rgb[0], controls.linear_rgb[1], controls.linear_rgb[2], 1};
        context.ClearRenderTargetView(clear.Get(), rgb);

        constants_bytes_.fill(0);
        constants_bytes_[20 * 4] = controls.mode;
        constants_bytes_[20 * 4 + 1] = controls.gamma;
        constants_bytes_[25 * 4] = controls.overlay;
        constants_bytes_[98 * 4] = controls.exposure;
        auto *cb = controls.update_method == UpdateMethod::map_discard ? mapped_constants_.Get() : constants_.Get();
        if (controls.update_method == UpdateMethod::map_discard)
        {
            D3D11_MAPPED_SUBRESOURCE mapped {};
            if (FAILED(context.Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
            std::memcpy(mapped.pData, constants_bytes_.data(), sizeof(constants_bytes_));
            context.Unmap(cb, 0);
        }
        else context.UpdateSubresource(cb, 0, nullptr, constants_bytes_.data(), 0, 0);

        context.OMSetRenderTargets(0, nullptr, nullptr);
        std::array<ID3D11ShaderResourceView *, 8> empty {};
        context.PSSetShaderResources(0, static_cast<UINT>(empty.size()), empty.data());
        const UINT stride = 16, offset = 0;
        context.IASetInputLayout(layout_.Get());
        context.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context.IASetVertexBuffers(0, 1, vertices_.GetAddressOf(), &stride, &offset);
        context.VSSetShader(vertex_.Get(), nullptr, 0);
        context.PSSetShader(pixel_.Get(), nullptr, 0);
        context.PSSetConstantBuffers(0, 1, &cb);
        context.PSSetShaderResources(0, 1, view.GetAddressOf());
        context.PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context.OMSetBlendState(nullptr, nullptr, ~0u);
        context.OMSetDepthStencilState(depth_.Get(), 0);
        context.RSSetState(raster_.Get());
        const D3D11_VIEWPORT viewport {0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1};
        context.RSSetViewports(1, &viewport);
        context.OMSetRenderTargets(1, output.GetAddressOf(), nullptr);
        if (controls.before_draw) controls.before_draw(context);
        context.Draw(3, 0);
        context.OMSetRenderTargets(0, nullptr, nullptr);
        context.PSSetShaderResources(0, 1, empty.data());
        return true;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertices_, constants_, mapped_constants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    std::array<float, 105 * 4> constants_bytes_ {};
    bool ready_ = false;
};
}
