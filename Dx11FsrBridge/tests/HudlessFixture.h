#pragma once

#include "../interop/HudlessV1.h"
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace hudless_fixture
{
using namespace neo::interop;
using Microsoft::WRL::ComPtr;
using Configure = void (*)(ID3D11Resource *, unsigned, unsigned) noexcept;
using Check = bool (*)() noexcept;
using Held = ID3D11Texture2D *(*)() noexcept;
inline Configure configure = nullptr;
inline RegisterHudlessV1 subscribe = nullptr;
inline UnregisterHudlessV1 unsubscribe = nullptr;
inline ComPtr<ID3D11Device> other_device;
inline unsigned mode = 0, result = 0, calls = 0;
inline bool contract_ok = true;
inline std::atomic_int alive { 0 };
inline constexpr std::uint32_t k_pixel = (3u << 30) | (173u << 20) | (621u << 10) | 913u;

class Lifetime final : public IUnknown
{
public:
    Lifetime() { ++alive; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
private:
    ~Lifetime() { --alive; }
    std::atomic_ulong references_ { 1 };
};

inline HRESULT WINAPI unexpected(const HudlessRequestV1 *, HudlessSurfaceV1 *) noexcept { return E_FAIL; }

inline HRESULT WINAPI record(const HudlessRequestV1 *request, HudlessSurfaceV1 *surface) noexcept
{
    ++calls;
    contract_ok = contract_ok && request && surface && request->size == sizeof(*request) && request->version == 1 &&
        request->context && request->color && request->depth && request->motion && request->output &&
        request->game_output && request->request_id && request->frame &&
        request->display_width == request->render_width * 2;
    if (!contract_ok) return E_FAIL;
    bool busy = false;
    std::thread control([&]
    {
        busy = unsubscribe(record) == HRESULT_FROM_WIN32(ERROR_BUSY) &&
            subscribe(unexpected) == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    });
    control.join();
    contract_ok = contract_ok && busy;
    surface->request_id = request->request_id;
    surface->frame = request->frame;
    surface->instance = request->instance;
    surface->flags = k_hudless_final_present_color | (mode == 1 ? k_hudless_flip : 0);
    if (mode == 2) return S_FALSE;
    if (mode >= 13 && mode <= 17)
    {
        ID3D11Texture2D *aliases[] {
            request->color, request->depth, request->motion, request->output, request->game_output };
        surface->color = aliases[mode - 13];
        surface->color->AddRef();
        return S_OK;
    }
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = request->display_width - (mode == 3 ? 1 : 0);
    desc.Height = request->display_height;
    desc.MipLevels = mode == 5 ? 2 : 1;
    desc.ArraySize = mode == 6 ? 2 : 1;
    desc.Format = mode == 4 ? (request->output_format == DXGI_FORMAT_R8G8B8A8_UNORM ?
        DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM) : request->output_format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Device> device;
    request->context->GetDevice(&device);
    if (mode == 7) device = other_device;
    ComPtr<ID3D11Texture2D> color;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &color))) return E_FAIL;
    const GUID tag {0xc4d34fa9, 0x9191, 0x4812, {0x93,0x11,0x30,0x20,0x11,0x05,0x19,0x01}};
    auto *lifetime = new Lifetime;
    const auto tagged = color->SetPrivateDataInterface(tag, lifetime);
    lifetime->Release();
    if (FAILED(tagged)) return E_FAIL;
    if (mode <= 1)
    {
        std::vector<std::uint32_t> pixels(desc.Width * desc.Height, k_pixel);
        request->context->UpdateSubresource(color.Get(), 0, nullptr, pixels.data(), desc.Width * 4, 0);
        configure(color.Get(), mode == 1 ? 1u : 0u, result);
    }
    surface->color = color.Detach();
    if (mode == 8) --surface->request_id;
    if (mode == 9) --surface->frame;
    if (mode == 10) ++surface->instance;
    if (mode == 11) surface->flags = 0;
    if (mode == 12) surface->flags |= 0x80000000u;
    if (mode == 18) surface->version = 2;
    if (mode == 19) --surface->size;
    if (mode == 20) surface->reserved = 1;
    if (mode == 21) return E_FAIL;
    if (mode == 22) return S_FALSE;
    return S_OK;
}

template<class Capture, class Frame, class Read>
int run(HMODULE bridge, HMODULE provider, ID3D11DeviceContext &context, Capture capture, Frame frame, Read read)
{
    subscribe = reinterpret_cast<RegisterHudlessV1>(GetProcAddress(bridge, k_register_hudless_v1));
    unsubscribe = reinterpret_cast<UnregisterHudlessV1>(GetProcAddress(bridge, k_unregister_hudless_v1));
    configure = reinterpret_cast<Configure>(GetProcAddress(provider, "ConfigureHudlessFixture"));
    const auto check = reinterpret_cast<Check>(GetProcAddress(provider, "HudlessFixtureClosed"));
    const auto held = reinterpret_cast<Held>(GetProcAddress(provider, "HudlessFixtureHeld"));
    if (!subscribe || !unsubscribe || !configure || !check || !held ||
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
            &other_device, nullptr, nullptr))) return 3;
    unsigned checks = 0;
    const auto verify = [&](bool ok, const char *label)
    {
        ++checks;
        if (!ok) std::printf("HUDless FAIL %u %s mode=%u\n", checks, label, mode);
        return ok;
    };
    const auto draw = [&](bool accepted, unsigned size = 512)
    {
        configure(nullptr, 0, result);
        const bool rendered = capture(frame(size, result == 0));
        bool retained = held() == nullptr;
        if (accepted && held())
        {
            ComPtr<ID3D11Device> device;
            context.GetDevice(&device);
            D3D11_TEXTURE2D_DESC desc {};
            held()->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = desc.MiscFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            retained = SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging));
            if (retained)
            {
                context.CopyResource(staging.Get(), held());
                retained = read(context, staging.Get(), k_pixel);
            }
        }
        const bool closed = check();
        return rendered && retained && closed && alive == 0 && contract_ok;
    };
    if (!verify(subscribe(nullptr) == E_INVALIDARG && unsubscribe(nullptr) == E_INVALIDARG, "null registration") ||
        !verify(unsubscribe(record) == S_FALSE, "empty unregister") ||
        !verify(draw(false) && calls == 0, "no provider") ||
        !verify(subscribe(record) == S_OK && subscribe(record) == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) &&
            unsubscribe(unexpected) == E_INVALIDARG, "single owner")) return 1;
    for (mode = 0; mode <= 22; ++mode)
        if (!verify(draw(mode <= 1), "admission and cleanup")) return 1;
    mode = 1;
    if (!verify(draw(true, 384), "resize")) return 1;
    for (result = 1; result <= 2; ++result)
        if (!verify(draw(true), "failed or no-output Evaluate")) return 1;
    result = 0;
    const auto previous = calls;
    if (!verify(unsubscribe(record) == S_OK && draw(false) && calls == previous, "unregister")) return 1;
    other_device.Reset();
    std::printf("HUDless actual DLL: %u checks passed; independent pixels, COM retirement, SR and parameter cleanup\n",
        checks);
    return 0;
}
}
