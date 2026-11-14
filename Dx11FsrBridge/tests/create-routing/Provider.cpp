#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <atomic>
#include <cstring>

namespace
{
HMODULE inner_module = nullptr;
std::atomic<unsigned> calls {};
bool successful = false;
bool extended_context = false;
bool successful_swapchain = false;
bool hdr_swapchain = false;
bool return_wrapper_chain = false;
bool combined_uses_device = false;
std::atomic<unsigned> early_observation {};
struct Device
{
    void **vtable;
};
void *device_methods[80] {};
Device device_object { device_methods };
void *context_methods[149] {};
Device context_object { context_methods };
void *dxgi_methods[12] {};
Device dxgi_object { dxgi_methods };
void *adapter_methods[10] {};
Device adapter_object { adapter_methods };
void *factory_methods[40] {};
Device factory_object { factory_methods };
void *swapchain_methods[41] {};
void *wrapper_methods[41] {};
unsigned native_layout_tag = 0;
unsigned operations_layout_tag = 0;
struct NativeSwapchain
{
    void **vtable;
    const void *original;
};
struct WrappedSwapchain
{
    void **vtable;
    const void *operations;
    unsigned marker;
};
NativeSwapchain swapchain_object { swapchain_methods, &native_layout_tag };
WrappedSwapchain wrapper_object { wrapper_methods, &operations_layout_tag, 42 };
std::atomic<unsigned> wrong_this {};
std::atomic<unsigned> draws {};
std::atomic<unsigned> factory_calls {};

void STDMETHODCALLTYPE unused() {}
ULONG STDMETHODCALLTYPE release(void *) { return 1; }

bool is_swapchain(void *object)
{
    return object == &swapchain_object || object == &wrapper_object;
}

void *selected_swapchain()
{
    return return_wrapper_chain ? static_cast<void *>(&wrapper_object) : static_cast<void *>(&swapchain_object);
}

HRESULT STDMETHODCALLTYPE query_interface(void *object, REFIID iid, void **output)
{
    if (output)
        *output = nullptr;
    if (object == &device_object && iid == __uuidof(IDXGIDevice) && output)
    {
        *output = &dxgi_object;
        return S_OK;
    }
    if (object == &factory_object && iid == __uuidof(IDXGIFactory2) && output)
    {
        *output = &factory_object;
        return S_OK;
    }
    if (is_swapchain(object) && output &&
        (iid == __uuidof(IDXGISwapChain3) || (iid == __uuidof(IDXGISwapChain4) && hdr_swapchain)))
    {
        *output = object;
        return S_OK;
    }
    if (object == &context_object && output)
    {
        if (iid == __uuidof(ID3D11DeviceContext))
        {
            *output = &context_object;
            return S_OK;
        }
        if (iid == __uuidof(ID3D11DeviceContext4) && extended_context)
        {
            if (inner_module)
            {
                const auto get_context = reinterpret_cast<ID3D11DeviceContext *(WINAPI *)()>(
                    GetProcAddress(inner_module, "GetContext"));
                return get_context()->QueryInterface(iid, output);
            }
            *output = &context_object;
            return S_OK;
        }
    }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE get_adapter(void *, IDXGIAdapter **adapter)
{
    *adapter = reinterpret_cast<IDXGIAdapter *>(&adapter_object);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE get_parent(void *, REFIID iid, void **output)
{
    if (iid == __uuidof(IDXGIFactory) || iid == __uuidof(IDXGIFactory2))
    {
        *output = &factory_object;
        return S_OK;
    }
    *output = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE get_desc(void *, DXGI_ADAPTER_DESC *)
{
    return E_FAIL;
}

HRESULT STDMETHODCALLTYPE create_swapchain(void *object, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **output)
{
    ++factory_calls;
    if (successful_swapchain && output && object == &factory_object)
    {
        *output = reinterpret_cast<IDXGISwapChain *>(selected_swapchain());
        return S_OK;
    }
    return object == &factory_object ? static_cast<HRESULT>(FIXTURE_RESULT) : E_UNEXPECTED;
}

HRESULT STDMETHODCALLTYPE create_hwnd(void *object, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **output)
{
    ++factory_calls;
    if (successful_swapchain && output && object == &factory_object)
    {
        *output = reinterpret_cast<IDXGISwapChain1 *>(selected_swapchain());
        return S_OK;
    }
    return object == &factory_object ? static_cast<HRESULT>(FIXTURE_RESULT) : E_UNEXPECTED;
}

HRESULT STDMETHODCALLTYPE swapchain_desc(void *, DXGI_SWAP_CHAIN_DESC *desc)
{
    *desc = {};
    desc->BufferDesc.Width = 640;
    desc->BufferDesc.Height = 360;
    desc->BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE resize_tail(void *object, UINT, UINT, UINT, DXGI_FORMAT, UINT,
    const UINT *, IUnknown *const *)
{
    return is_swapchain(object) ? static_cast<HRESULT>(FIXTURE_RESULT) : E_UNEXPECTED;
}

HRESULT STDMETHODCALLTYPE native_resize_target(void *object, const DXGI_MODE_DESC *target)
{
    // 把跨布局的错误 this 转为可观察失败，避免负控在解引用错误布局前崩溃。
    if (object != &swapchain_object || !target || target->Width != 640 || target->Height != 360)
        return E_UNEXPECTED;
    return static_cast<NativeSwapchain *>(object)->original == &native_layout_tag
        ? static_cast<HRESULT>(FIXTURE_RESULT) : E_UNEXPECTED;
}

HRESULT STDMETHODCALLTYPE wrapper_resize_target(void *object, const DXGI_MODE_DESC *target)
{
    if (object != &wrapper_object || !target || target->Width != 640 || target->Height != 360)
        return E_UNEXPECTED;
    const auto *wrapper = static_cast<WrappedSwapchain *>(object);
    return wrapper->operations == &operations_layout_tag && wrapper->marker == 42
        ? static_cast<HRESULT>(FIXTURE_RESULT) : E_UNEXPECTED;
}

HRESULT STDMETHODCALLTYPE set_hdr_metadata(void *object, DXGI_HDR_METADATA_TYPE type, UINT size, void *metadata)
{
    return is_swapchain(object) && type == DXGI_HDR_METADATA_TYPE_NONE && size == 0 && metadata == nullptr
        ? static_cast<HRESULT>(FIXTURE_RESULT) : E_UNEXPECTED;
}

void STDMETHODCALLTYPE draw(void *object, UINT, UINT)
{
    ++draws;
    if (object != &context_object)
        ++wrong_this;
    if (inner_module && successful)
    {
        const auto get_context = reinterpret_cast<ID3D11DeviceContext *(WINAPI *)()>(
            GetProcAddress(inner_module, "GetContext"));
        get_context()->Draw(5, 1);
    }
}

HRESULT STDMETHODCALLTYPE create_buffer(void *object, const D3D11_BUFFER_DESC *,
    const D3D11_SUBRESOURCE_DATA *, ID3D11Buffer **)
{
    return object == &device_object ? static_cast<HRESULT>(FIXTURE_RESULT) : E_UNEXPECTED;
}
}

extern "C" __declspec(dllexport) void WINAPI SetSuccess(BOOL enabled)
{
    successful = enabled != FALSE;
    for (auto &method : device_methods)
        method = reinterpret_cast<void *>(&unused);
    for (auto &method : context_methods)
        method = reinterpret_cast<void *>(&unused);
    for (auto &method : factory_methods)
        method = reinterpret_cast<void *>(&unused);
    for (auto &method : swapchain_methods)
        method = reinterpret_cast<void *>(&unused);
    device_methods[0] = reinterpret_cast<void *>(&query_interface);
    device_methods[3] = reinterpret_cast<void *>(&create_buffer);
    context_methods[0] = reinterpret_cast<void *>(&query_interface);
    context_methods[2] = reinterpret_cast<void *>(&release);
    context_methods[13] = reinterpret_cast<void *>(&draw);
    dxgi_methods[2] = reinterpret_cast<void *>(&release);
    dxgi_methods[7] = reinterpret_cast<void *>(&get_adapter);
    adapter_methods[2] = reinterpret_cast<void *>(&release);
    adapter_methods[6] = reinterpret_cast<void *>(&get_parent);
    adapter_methods[8] = reinterpret_cast<void *>(&get_desc);
    factory_methods[2] = reinterpret_cast<void *>(&release);
    factory_methods[0] = reinterpret_cast<void *>(&query_interface);
    factory_methods[10] = reinterpret_cast<void *>(&create_swapchain);
    factory_methods[15] = reinterpret_cast<void *>(&create_hwnd);
    swapchain_methods[0] = reinterpret_cast<void *>(&query_interface);
    swapchain_methods[2] = reinterpret_cast<void *>(&release);
    swapchain_methods[12] = reinterpret_cast<void *>(&swapchain_desc);
    swapchain_methods[14] = reinterpret_cast<void *>(&native_resize_target);
    swapchain_methods[39] = reinterpret_cast<void *>(&resize_tail);
    swapchain_methods[40] = reinterpret_cast<void *>(&set_hdr_metadata);
    std::memcpy(wrapper_methods, swapchain_methods, sizeof(wrapper_methods));
    wrapper_methods[14] = reinterpret_cast<void *>(&wrapper_resize_target);
}

extern "C" __declspec(dllexport) void WINAPI SetWrapperChain(BOOL enabled)
{
    return_wrapper_chain = enabled != FALSE;
}

extern "C" __declspec(dllexport) void WINAPI SetSwapchain(BOOL enabled)
{
    successful_swapchain = enabled != FALSE;
}

extern "C" __declspec(dllexport) void WINAPI SetHdrSwapchain(BOOL enabled)
{
    hdr_swapchain = enabled != FALSE;
}

extern "C" __declspec(dllexport) void WINAPI SetExtended(BOOL enabled)
{
    extended_context = enabled != FALSE;
}

void **guarded_table(void **source, unsigned count)
{
    SYSTEM_INFO info {};
    GetSystemInfo(&info);
    auto *pages = static_cast<unsigned char *>(VirtualAlloc(nullptr, info.dwPageSize * 2,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD previous = 0;
    if (!pages || !VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous))
        return nullptr;
    auto **table = reinterpret_cast<void **>(pages + info.dwPageSize) - count;
    std::memcpy(table, source, count * sizeof(void *));
    return table;
}

extern "C" __declspec(dllexport) void WINAPI GuardTables()
{
    device_object.vtable = guarded_table(device_methods, 43);
    context_object.vtable = guarded_table(context_methods, extended_context ? 149 : 115);
    factory_object.vtable = guarded_table(factory_methods, 25);
    swapchain_object.vtable = guarded_table(swapchain_methods, hdr_swapchain ? 41 : 40);
    wrapper_object.vtable = guarded_table(wrapper_methods, hdr_swapchain ? 41 : 40);
}

extern "C" __declspec(dllexport) unsigned WINAPI GetWrongThis() { return wrong_this.load(); }
extern "C" __declspec(dllexport) unsigned WINAPI GetDraws() { return draws.load(); }
extern "C" __declspec(dllexport) unsigned WINAPI GetFactoryCalls() { return factory_calls.load(); }
extern "C" __declspec(dllexport) ID3D11DeviceContext *WINAPI GetContext()
{
    return reinterpret_cast<ID3D11DeviceContext *>(&context_object);
}

extern "C" __declspec(dllexport) void WINAPI SetInner(HMODULE module)
{
    inner_module = module;
}

extern "C" __declspec(dllexport) void WINAPI SetCombinedUsesDevice(BOOL enabled)
{
    combined_uses_device = enabled != FALSE;
}

extern "C" __declspec(dllexport) unsigned WINAPI GetMutatedMethods()
{
    return (device_object.vtable[3] != reinterpret_cast<void *>(&create_buffer)) +
        (context_object.vtable[13] != reinterpret_cast<void *>(&draw)) +
        (factory_object.vtable[10] != reinterpret_cast<void *>(&create_swapchain));
}

extern "C" __declspec(dllexport) unsigned WINAPI GetEarlyObservation()
{
    return early_observation.load();
}

void observe_before_wrapping()
{
    const auto mutated = reinterpret_cast<unsigned(WINAPI *)()>(GetProcAddress(inner_module, "GetMutatedMethods"));
    early_observation += mutated();
}

extern "C" __declspec(dllexport) unsigned WINAPI GetCalls()
{
    return calls.load();
}

extern "C" __declspec(dllexport) HRESULT WINAPI FixtureCreateDevice(
    IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL *levels, UINT count, UINT sdk, ID3D11Device **device,
    D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context)
{
    ++calls;
    if (inner_module != nullptr)
    {
        const auto inner = reinterpret_cast<decltype(&D3D11CreateDevice)>(
            GetProcAddress(inner_module, "D3D11CreateDevice"));
        const HRESULT expected = successful ? S_OK : E_ABORT;
        if (!inner || inner(adapter, driver, software, flags, levels, count, sdk, device, level, context) != expected)
            return E_UNEXPECTED;
        if (successful)
            observe_before_wrapping();
    }
    if (successful)
    {
        if (device)
            *device = reinterpret_cast<ID3D11Device *>(&device_object);
        if (context)
            *context = reinterpret_cast<ID3D11DeviceContext *>(&context_object);
        return S_OK;
    }
    return static_cast<HRESULT>(FIXTURE_RESULT);
}

extern "C" __declspec(dllexport) HRESULT WINAPI FixtureCreateDeviceAndSwapChain(
    IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL *levels, UINT count, UINT sdk, const DXGI_SWAP_CHAIN_DESC *desc,
    IDXGISwapChain **swapchain, ID3D11Device **device, D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context)
{
    ++calls;
    if (inner_module != nullptr)
    {
        HRESULT result = E_UNEXPECTED;
        if (combined_uses_device)
        {
            const auto inner = reinterpret_cast<decltype(&D3D11CreateDevice)>(
                GetProcAddress(inner_module, "D3D11CreateDevice"));
            if (inner)
                result = inner(adapter, driver, software, flags, levels, count, sdk, device, level, context);
        }
        else
        {
            const auto inner = reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(
                GetProcAddress(inner_module, "D3D11CreateDeviceAndSwapChain"));
            if (inner)
                result = inner(adapter, driver, software, flags, levels, count, sdk,
                    desc, swapchain, device, level, context);
        }
        if (result != (successful ? S_OK : E_ABORT))
            return E_UNEXPECTED;
        if (successful)
            observe_before_wrapping();
    }
    if (successful)
    {
        if (device)
            *device = reinterpret_cast<ID3D11Device *>(&device_object);
        if (context)
            *context = reinterpret_cast<ID3D11DeviceContext *>(&context_object);
        return S_OK;
    }
    return static_cast<HRESULT>(FIXTURE_RESULT);
}
