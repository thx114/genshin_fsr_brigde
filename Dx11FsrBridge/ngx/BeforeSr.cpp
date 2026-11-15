#include "BeforeSr.h"
#include "../ComMethodRoutes.h"

#include <atomic>

using namespace neo::interop;
using Microsoft::WRL::ComPtr;

namespace
{
// 2026-11-15：占用期间禁止注销/替换，避免消费者卸载后调用旧地址；渲染线程不等注册方。
HRESULT WINAPI callback_in_progress(const BeforeSrFrameV1 *, ID3D11Texture2D **) noexcept { return E_UNEXPECTED; }
std::atomic<RecordBeforeSrV1> g_callback { nullptr };
thread_local const neo::ngx::BeforeSrInput *g_current_input = nullptr;

BeforeSrFrameV1 describe_input(const neo::ngx::BeforeSrInput &input) noexcept
{
    BeforeSrFrameV1 frame = input.frame;
    if (input.source_view)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
        input.source_view->GetDesc(&desc);
        // 只有全纹理原字节副本与源视图读同一子资源时，才把源视图语义交给下游。
        if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && desc.Texture2D.MostDetailedMip == 0)
            frame.color_view_format = desc.Format;
    }
    return frame;
}

bool can_replace_color(const BeforeSrFrameV1 &frame, ID3D11Device &device, ID3D11Texture2D &replacement) noexcept
{
    if (&replacement == frame.color || &replacement == frame.depth || &replacement == frame.motion) return false;
    D3D11_TEXTURE2D_DESC desc {};
    replacement.GetDesc(&desc);
    if (desc.Width != frame.render_width || desc.Height != frame.render_height ||
        desc.Format != frame.color_storage_format || desc.MipLevels != 1 || desc.ArraySize != 1 ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 ||
        (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0)
        return false;
    ComPtr<ID3D11Device> owner;
    replacement.GetDevice(owner.GetAddressOf());
    if (owner.Get() == &device) return true;
    ComPtr<IUnknown> a, b;
    return owner && SUCCEEDED(owner.As(&a)) &&
        SUCCEEDED(device.QueryInterface(IID_PPV_ARGS(b.GetAddressOf()))) && a.Get() == b.Get();
}
}

namespace neo::ngx
{
BeforeSrQueryScope::BeforeSrQueryScope(const BeforeSrInput &input) noexcept : previous_(g_current_input)
{
    g_current_input = &input;
}

BeforeSrQueryScope::~BeforeSrQueryScope()
{
    g_current_input = previous_;
}

bool has_before_sr_callback() noexcept
{
    return g_callback.load(std::memory_order_relaxed) != nullptr;
}

ComPtr<ID3D11Texture2D> record_before_sr(const BeforeSrInput &input, ID3D11Device &device) noexcept
{
    auto callback = g_callback.load(std::memory_order_acquire);
    if (!callback || callback == callback_in_progress ||
        !g_callback.compare_exchange_strong(callback, callback_in_progress, std::memory_order_acq_rel))
        return {};
    struct ReleaseCallback
    {
        RecordBeforeSrV1 callback;
        ~ReleaseCallback() { g_callback.store(callback, std::memory_order_release); }
    } keep_registered { callback };

    const auto frame = describe_input(input);
    ComPtr<ID3D11Texture2D> replacement;
    HRESULT hr;
    {
        // 消费者的工作不是一个新的游戏 Draw；保留其他代理的调用链，但绕过本桥的重入与观测。
        const ContextPolicyBypass bypass(frame.context);
        hr = callback(&frame, replacement.GetAddressOf());
    }
    if (hr != S_OK || !replacement || !can_replace_color(frame, device, *replacement.Get())) return {};
    return replacement;
}
}

extern "C" __declspec(dllexport) HRESULT WINAPI Dx11FsrBridge_RegisterBeforeSrV1(RecordBeforeSrV1 callback) noexcept
{
    if (!callback) return E_INVALIDARG;
    RecordBeforeSrV1 expected = nullptr;
    return g_callback.compare_exchange_strong(expected, callback, std::memory_order_acq_rel)
        ? S_OK : HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
}

extern "C" __declspec(dllexport) HRESULT WINAPI Dx11FsrBridge_UnregisterBeforeSrV1(RecordBeforeSrV1 callback) noexcept
{
    if (!callback) return E_INVALIDARG;
    auto expected = callback;
    if (g_callback.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) return S_OK;
    if (expected == callback_in_progress) return HRESULT_FROM_WIN32(ERROR_BUSY);
    return expected ? E_INVALIDARG : S_FALSE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI Dx11FsrBridge_QueryBeforeSrFrameV1(ID3D11DeviceContext *context,
    ID3D11Resource *color, ID3D11Resource *depth, BeforeSrFrameV1 *out) noexcept
{
    if (!out || out->size != sizeof(*out) || out->version != 1 || !context || !color || !depth)
        return E_INVALIDARG;
    const auto *input = g_current_input;
    if (!input || input->frame.context != context || input->frame.color != color || input->frame.depth != depth)
        return S_FALSE;
    *out = describe_input(*input);
    return S_OK;
}
