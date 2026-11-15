#include "../runtime/JitterReadback.h"
#include <array>

int main()
{
    using Microsoft::WRL::ComPtr;
    neo::graphics::JitterReadback reader;
    for (int pass = 0; pass < 2; ++pass)
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf()))) return 1;
        // 相同长度跨设备也不能复用旧设备的 staging；短 CB 不得扫过末尾。
        for (UINT width : { 464u, 448u, 1056u, 464u })
        {
            std::array<float, 264> values {};
            D3D11_BUFFER_DESC desc {};
            desc.ByteWidth = width;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            ComPtr<ID3D11Buffer> cb;
            if (FAILED(device->CreateBuffer(&desc, nullptr, cb.GetAddressOf()))) return 2;
            context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
            for (int frame = 1; frame <= 2; ++frame)
            {
                values.fill(float(pass * 10 + frame));
                context->UpdateSubresource(cb.Get(), 0, nullptr, values.data(), 0, 0);
                bool correct = false;
                bool read = reader.with_bound_cb(*context.Get(), [&](std::span<const float> data)
                {
                    correct = data.size() == (std::min)(width, 1024u) / sizeof(float) &&
                        data.front() == values.front() && data.back() == values.front();
                });
                if (read != (width >= 464) || correct != read) return 3;
            }
        }
    }
    return 0;
}
