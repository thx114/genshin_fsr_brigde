#include "Hudless.h"
#include "../ComMethodRoutes.h"

#include <atomic>
#include <utility>

using namespace neo::interop;
using Microsoft::WRL::ComPtr;

namespace
{
HRESULT WINAPI callback_in_progress(const HudlessRequestV1 *, HudlessSurfaceV1 *) noexcept { return E_UNEXPECTED; }
std::atomic<RecordHudlessV1> g_callback { nullptr };

bool same_object(IUnknown *a, IUnknown *b) noexcept
{
    if (!a || !b) return false;
    if (a == b) return true;
    ComPtr<IUnknown> left, right;
    return SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(left.GetAddressOf()))) &&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(right.GetAddressOf()))) && left.Get() == right.Get();
}

bool color_format(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return true;
    default:
        return false;
    }
}

bool compatible(const HudlessRequestV1 &request, ID3D11Device &device, ID3D11Texture2D &color) noexcept
{
    // 2026-11-17：排除本次输入和未完成输出，防止把 SR 池或游戏 HUD 目标冒充独立来源。
    for (auto *input : { request.color, request.depth, request.motion, request.output, request.game_output })
        if (same_object(&color, input)) return false;
    D3D11_TEXTURE2D_DESC desc {};
    color.GetDesc(&desc);
    if (!color_format(request.output_format) || desc.Format != request.output_format ||
        desc.Width != request.display_width || desc.Height != request.display_height ||
        desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0)
        return false;
    ComPtr<ID3D11Device> owner;
    color.GetDevice(owner.GetAddressOf());
    return same_object(owner.Get(), &device);
}
}

namespace neo::ngx
{
HudlessInput record_hudless(const HudlessRequestV1 &request, ID3D11Device &device) noexcept
{
    auto callback = g_callback.load(std::memory_order_acquire);
    if (!callback || callback == callback_in_progress ||
        !g_callback.compare_exchange_strong(callback, callback_in_progress, std::memory_order_acq_rel))
        return {};
    struct ReleaseCallback
    {
        RecordHudlessV1 callback;
        ~ReleaseCallback() { g_callback.store(callback, std::memory_order_release); }
    } keep_registered { callback };
    if (!request.context || request.size != sizeof(request) || request.version != 1 || request.reserved != 0 ||
        request.request_id == 0 || request.frame == 0 || !request.display_width || !request.display_height ||
        request.context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return {};
    HudlessSurfaceV1 surface;
    HRESULT hr;
    {
        const ContextPolicyBypass bypass(request.context);
        hr = callback(&request, &surface);
    }
    ComPtr<ID3D11Texture2D> owned;
    owned.Attach(surface.color);
    if (hr != S_OK || !owned || surface.size != sizeof(surface) || surface.version != 1 ||
        surface.request_id != request.request_id || surface.frame != request.frame ||
        surface.instance != request.instance ||
        surface.reserved != 0 || !(surface.flags & k_hudless_final_present_color) ||
        (surface.flags & ~(k_hudless_final_present_color | k_hudless_flip)) != 0 ||
        !compatible(request, device, *owned.Get()))
        return {};
    return { std::move(owned), (surface.flags & k_hudless_flip) != 0 };
}

HudlessParameterScope::HudlessParameterScope(NVSDK_NGX_Parameter &parameters, HudlessInput input) noexcept :
    parameters_(parameters), input_(std::move(input))
{
    parameters_.Set("Opti.Hudless", static_cast<ID3D11Resource *>(input_.color.Get()));
    parameters_.Set("Opti.Hudless.Flip", input_.color && input_.flip ? 1u : 0u);
}

HudlessParameterScope::~HudlessParameterScope()
{
    // 2026-11-17：参数袋跨帧共用；先清裸指针再释放桥持有的引用，失败回执也不得继承旧帧。
    parameters_.Set("Opti.Hudless", static_cast<ID3D11Resource *>(nullptr));
    parameters_.Set("Opti.Hudless.Flip", 0u);
}
}

extern "C" __declspec(dllexport) HRESULT WINAPI Dx11FsrBridge_RegisterHudlessV1(RecordHudlessV1 callback) noexcept
{
    if (!callback) return E_INVALIDARG;
    RecordHudlessV1 expected = nullptr;
    return g_callback.compare_exchange_strong(expected, callback, std::memory_order_acq_rel)
        ? S_OK : HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
}

extern "C" __declspec(dllexport) HRESULT WINAPI Dx11FsrBridge_UnregisterHudlessV1(RecordHudlessV1 callback) noexcept
{
    if (!callback) return E_INVALIDARG;
    auto expected = callback;
    if (g_callback.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) return S_OK;
    if (expected == callback_in_progress) return HRESULT_FROM_WIN32(ERROR_BUSY);
    return expected ? E_INVALIDARG : S_FALSE;
}
