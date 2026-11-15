#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <span>

namespace neo::graphics
{
class JitterReadback
{
public:
    template <typename Consume>
    bool with_bound_cb(ID3D11DeviceContext &context, Consume &&consume)
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D11Buffer> source;
        context.PSGetConstantBuffers(0, 1, source.GetAddressOf());
        if (!source) return false;
        D3D11_BUFFER_DESC desc {};
        source->GetDesc(&desc);
        if (desc.ByteWidth < 464 || !(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return false;
        ComPtr<ID3D11Device> device;
        context.GetDevice(device.GetAddressOf());
        if (!device) return false;
        // 2026-11-16：旧回退每次新建 staging 并 Flush；Map(READ) 已等待本次复制，不能读上一代。
        // 只复用容器，不复用内容。池的设备由 staging 持有；性能收益尚无真机测量。
        if (device.Get() != owner_ || desc.ByteWidth != width_)
        {
            D3D11_BUFFER_DESC staging_desc {};
            staging_desc.ByteWidth = desc.ByteWidth;
            staging_desc.Usage = D3D11_USAGE_STAGING;
            staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Buffer> next;
            if (FAILED(device->CreateBuffer(&staging_desc, nullptr, next.GetAddressOf()))) return false;
            staging_.Swap(next);
            owner_ = device.Get();
            width_ = desc.ByteWidth;
        }
        context.CopyResource(staging_.Get(), source.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context.Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        struct Unmap
        {
            ID3D11DeviceContext &context;
            ID3D11Buffer *buffer;
            ~Unmap() { context.Unmap(buffer, 0); }
        } unmap { context, staging_.Get() };
        if (!mapped.pData) return false;
        consume(std::span(static_cast<const float *>(mapped.pData), (std::min)(width_, 1024u) / sizeof(float)));
        return true;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer> staging_;
    ID3D11Device *owner_ = nullptr;
    UINT width_ = 0;
};
}
