#include "../runtime/OutputHistory.h"

#include <array>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
using neo::graphics::OutputHistory;
using Mode = OutputHistory::Mode;

namespace
{
constexpr UINT k_width = 8;
constexpr std::uint32_t k_frame_pixel = 0xff194bb3;
constexpr std::uint32_t k_native_pixel = 0xffb37c24;

struct Device
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    bool initialize()
    {
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf());
        if (FAILED(hr)) std::printf("D3D11CreateDevice failed: %08lx\n", static_cast<unsigned long>(hr));
        return SUCCEEDED(hr);
    }

    ComPtr<ID3D11Texture2D> texture(std::uint32_t pixel, UINT width = k_width,
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE usage = D3D11_USAGE_DEFAULT) const
    {
        std::array<std::uint32_t, k_width * k_width> pixels;
        pixels.fill(pixel);
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = k_width;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format;
        desc.Usage = usage;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA init { pixels.data(), width * UINT(sizeof(pixel)), 0 };
        ComPtr<ID3D11Texture2D> result;
        const HRESULT hr = device->CreateTexture2D(&desc, &init, result.GetAddressOf());
        if (FAILED(hr))
            std::printf("CreateTexture2D width=%u format=%u usage=%u failed: %08lx\n",
                width, static_cast<unsigned>(format), static_cast<unsigned>(usage), static_cast<unsigned long>(hr));
        return result;
    }

    bool matches(ID3D11Texture2D &texture, std::uint32_t pixel) const
    {
        D3D11_TEXTURE2D_DESC desc {};
        texture.GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, readback.GetAddressOf()))) return false;
        context->CopyResource(readback.Get(), &texture);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        bool equal = true;
        for (UINT y = 0; y < desc.Height && equal; ++y)
        {
            const auto *line = static_cast<const unsigned char *>(mapped.pData) + y * mapped.RowPitch;
            for (UINT x = 0; x < desc.Width && equal; ++x)
                equal = std::memcmp(line + x * sizeof(pixel), &pixel, sizeof(pixel)) == 0;
        }
        context->Unmap(readback.Get(), 0);
        return equal;
    }
};
}

int main()
{
    Device device, foreign;
    if (!device.initialize() || !foreign.initialize()) return 2;
    auto canonical = device.texture(k_frame_pixel);
    auto target = device.texture(k_native_pixel);
    auto wrong_size = device.texture(k_native_pixel, k_width / 2);
    auto wrong_format = device.texture(k_native_pixel, k_width, DXGI_FORMAT_R8G8B8A8_UINT);
    auto immutable = device.texture(k_native_pixel, k_width, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_IMMUTABLE);
    auto other_device = foreign.texture(k_native_pixel);
    if (!canonical || !target || !wrong_size || !wrong_format || !immutable || !other_device) return 2;
    auto &context = *device.context.Get();
    auto &dst = *target.Get();
    constexpr std::uint64_t a = 11, b = 12, c = 13, generation = 42;
    unsigned failures = 0;
    const auto check = [&failures](bool ok, const char *what)
    {
        if (!ok)
        {
            ++failures;
            std::printf("FAIL: %s\n", what);
        }
    };

    OutputHistory history;
    check(!history.deliver(context, dst, generation, b, Mode::Repeat), "empty slot has no output");
    history.publish(*canonical.Get(), generation, a);
    const auto same = history.deliver(context, *canonical.Get(), generation, a, Mode::Repeat);
    check(same && same->same_output && same->source == canonical.Get(), "same output is already written");
    canonical.Reset();
    check(!history.deliver(context, dst, 0, b, Mode::Repeat) &&
        !history.deliver(context, dst, generation - 1, b, Mode::Repeat), "missing or stale Render token");
    check(!history.deliver(context, dst, generation, b, Mode::Repair), "unassociated repair cannot identify instance");
    check(!history.deliver(context, *wrong_size.Get(), generation, c, Mode::Repeat) &&
        device.matches(*wrong_size.Get(), k_native_pixel), "different shape does not write or associate");
    check(!history.deliver(context, *wrong_format.Get(), generation, c, Mode::Repeat) &&
        device.matches(*wrong_format.Get(), k_native_pixel), "different format does not write or associate");
    check(!history.deliver(context, *immutable.Get(), generation, c, Mode::Repeat) &&
        device.matches(*immutable.Get(), k_native_pixel), "immutable target not written");
    check(!history.deliver(context, *other_device.Get(), generation, c, Mode::Repeat) &&
        foreign.matches(*other_device.Get(), k_native_pixel), "foreign target not written");
    check(!history.deliver(*foreign.context.Get(), dst, generation, b, Mode::Repeat) && !history.owns(b) &&
        device.matches(dst, k_native_pixel), "foreign execution context not used");

    const auto repeat = history.deliver(context, dst, generation, b, Mode::Repeat);
    check(repeat && !repeat->same_output && history.owns(a) && history.owns(b) &&
        device.matches(dst, k_frame_pixel), "owned source survives caller release and copies exact pixels");
    const auto overwrite = device.texture(k_native_pixel);
    if (!overwrite) return 2;
    context.CopyResource(&dst, overwrite.Get());
    check(history.can_repair(generation) && history.deliver(context, dst, generation, b, Mode::Repair) &&
        device.matches(dst, k_frame_pixel), "paired target repair writes the current generation");
    context.CopyResource(&dst, overwrite.Get());
    check(!history.can_repair(generation) && !history.deliver(context, dst, generation, b, Mode::Repair) &&
        device.matches(dst, k_native_pixel), "only one paired repair per generation");

    history.invalidate_frame();
    check(history.owns(a) && history.owns(b) && !history.deliver(context, dst, generation, b, Mode::Repeat),
        "failure invalidates output but keeps instance evidence");
    history.publish(*overwrite.Get(), generation + 1, c);
    check(history.owns(a) && history.owns(c) && !history.owns(b) && history.can_repair(generation + 1),
        "new generation re-arms repair; association keeps the original replacement order");
    history.forget_instance();
    check(!history.has_association() && !history.can_repair(generation + 1), "handover drops both kinds of state");
    history.publish(*overwrite.Get(), generation + 2, a);
    history = OutputHistory {};
    check(!history.has_association() && !history.deliver(context, dst, generation + 2, a, Mode::Repeat),
        "slot reuse cannot carry an old instance's output or association");
    std::printf("OutputHistory: %u failures\n", failures);
    return failures != 0;
}
