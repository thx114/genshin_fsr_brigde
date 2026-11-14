#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <cstddef>

namespace interface_layout
{
template <typename Interface>
bool same_address(IUnknown *object)
{
    Interface *extended = nullptr;
    if (FAILED(object->QueryInterface(__uuidof(Interface), reinterpret_cast<void **>(&extended))) || !extended)
        return false;
    const bool same = static_cast<void *>(extended) == static_cast<void *>(object);
    extended->Release();
    return same;
}

// SDK 的完整继承槽数；只把同址扩展接口的尾部保留在当前对象的克隆表中。
inline std::size_t device(ID3D11Device *object, std::size_t minimum = 43)
{
    if (same_address<ID3D11Device5>(object)) return 69;
    if (same_address<ID3D11Device4>(object)) return 67;
    if (same_address<ID3D11Device3>(object)) return 65;
    if (same_address<ID3D11Device2>(object)) return 54;
    if (same_address<ID3D11Device1>(object)) return 50;
    return minimum;
}

inline std::size_t context(ID3D11DeviceContext *object)
{
    if (same_address<ID3D11DeviceContext4>(object)) return 149;
    if (same_address<ID3D11DeviceContext3>(object)) return 147;
    if (same_address<ID3D11DeviceContext2>(object)) return 144;
    if (same_address<ID3D11DeviceContext1>(object)) return 134;
    return 115;
}

inline std::size_t factory(IDXGIFactory *object, std::size_t minimum = 12)
{
    if (same_address<IDXGIFactory7>(object)) return 32;
    if (same_address<IDXGIFactory6>(object)) return 30;
    if (same_address<IDXGIFactory5>(object)) return 29;
    if (same_address<IDXGIFactory4>(object)) return 28;
    if (same_address<IDXGIFactory3>(object)) return 26;
    if (same_address<IDXGIFactory2>(object)) return 25;
    if (same_address<IDXGIFactory1>(object)) return 14;
    return minimum;
}

inline std::size_t swapchain(IDXGISwapChain *object)
{
    if (same_address<IDXGISwapChain4>(object)) return 41;
    if (same_address<IDXGISwapChain3>(object)) return 40;
    if (same_address<IDXGISwapChain2>(object)) return 36;
    if (same_address<IDXGISwapChain1>(object)) return 29;
    return 18;
}

template <typename Extended>
ID3D11DeviceContext *query_context(ID3D11DeviceContext *object)
{
    Extended *extended = nullptr;
    if (FAILED(object->QueryInterface(__uuidof(Extended), reinterpret_cast<void **>(&extended))) || !extended)
        return nullptr;
    ID3D11DeviceContext *base = nullptr;
    extended->QueryInterface(__uuidof(ID3D11DeviceContext), reinterpret_cast<void **>(&base));
    extended->Release();
    return base;
}

// XXMI 等只封装旧 context 接口、将更高接口透传给 runtime。使用标准 QI 返回的
// base context 观察实际绘制，代理自身的 BeforeDraw/AfterDraw 保持执行。
inline ID3D11DeviceContext *execution_context(ID3D11DeviceContext *object)
{
    if (auto *base = query_context<ID3D11DeviceContext4>(object)) return base;
    if (auto *base = query_context<ID3D11DeviceContext3>(object)) return base;
    if (auto *base = query_context<ID3D11DeviceContext2>(object)) return base;
    if (auto *base = query_context<ID3D11DeviceContext1>(object)) return base;
    return nullptr;
}
}
