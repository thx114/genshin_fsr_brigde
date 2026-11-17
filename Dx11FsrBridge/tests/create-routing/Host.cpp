#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include "DrawCapture.h"

namespace
{
// 2026-11-14：宿主选择 ngx，但 OFF 构型不接受该后端。没有其它启用条件时，
// 交换链应保留原方法；ON 构型仍须检查生产 DLL 确实接管，而非只验证透传结果。
constexpr bool k_ngx_swapchain_hooks = DX11FSRBRIDGE_TEST_NGX != 0;
int failures = 0;
HANDLE capture_receipt = nullptr;
HANDLE hwnd_receipt = nullptr;
HANDLE draw_receipt = nullptr;
std::atomic_uint draw_entries = 0;
unsigned expected_draw_entries = 3;

void WINAPI capture_debug_line(LPCSTR line)
{
    if (std::strstr(line, "fsr2_translation_candidate render=64x64 output=128x128"))
        SetEvent(capture_receipt);
    if (hwnd_receipt && std::strstr(line, "hooked CreateSwapChainForHwnd size="))
        SetEvent(hwnd_receipt);
    if (draw_receipt)
    {
        const unsigned entry = std::strstr(line, "draw_indexed_hook_active") ? 2 :
            std::strstr(line, "draw_hook_active") ? 1 : 0;
        if (entry && ((draw_entries.fetch_or(entry) | entry) & expected_draw_entries) == expected_draw_entries)
            SetEvent(draw_receipt);
    }
}

void expect(bool condition, const char *what)
{
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", what);
    failures += !condition;
}

// 给两个真实 PE 导入表设置不同提供者，模拟第三方 loader 已建立的两层调用。
bool set_import(HMODULE module, const char *name, FARPROC target, const char *library = "d3d11.dll")
{
    auto *base = reinterpret_cast<unsigned char *>(module);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; descriptor->Name; ++descriptor)
    {
        if (library && _stricmp(reinterpret_cast<char *>(base + descriptor->Name), library) != 0)
            continue;
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->OriginalFirstThunk);
        auto *slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            auto *symbol = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char *>(symbol->Name), name) != 0)
                continue;
            DWORD previous = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &previous))
                return false;
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(target);
            DWORD ignored = 0;
            return VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), previous, &ignored) != FALSE;
        }
    }
    return false;
}

HRESULT call_device(FARPROC target, ID3D11Device **device = nullptr, ID3D11DeviceContext **context = nullptr)
{
    return reinterpret_cast<decltype(&D3D11CreateDevice)>(target)(nullptr, D3D_DRIVER_TYPE_NULL,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device, nullptr, context);
}

bool belongs_to(HMODULE module, void *address)
{
    HMODULE owner = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &owner) && owner == module;
}

using Microsoft::WRL::ComPtr;

bool same_identity(IUnknown *first, IUnknown *second)
{
    ComPtr<IUnknown> a, b;
    return first && second && SUCCEEDED(first->QueryInterface(IID_PPV_ARGS(&a))) &&
        SUCCEEDED(second->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() == b.Get();
}

std::array<void *, 2> draw_routes(ID3D11DeviceContext *context)
{
    auto **table = *reinterpret_cast<void ***>(context);
    return {table[12], table[13]};
}

void print_context(const char *label, ID3D11DeviceContext *context)
{
    if (!context) return;
    ComPtr<IUnknown> identity;
    context->QueryInterface(IID_PPV_ARGS(&identity));
    std::printf("context %s ptr=%p IUnknown=%p vtable=%p\n", label, context, identity.Get(),
        *reinterpret_cast<void ***>(context));
    const auto routes = draw_routes(context);
    for (unsigned n = 0; n < routes.size(); ++n)
    {
        HMODULE owner = nullptr;
        wchar_t path[MAX_PATH] {};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(routes[n]), &owner))
            GetModuleFileNameW(owner, path, MAX_PATH);
        std::printf("  %s=%p module=%ls\n", n == 0 ? "DrawIndexed" : "Draw", routes[n], path);
    }
}

void check_alternate_draws(ID3D11Device *device, ID3D11DeviceContext *context)
{
    for (const auto &[entry, description] : std::array {
        std::pair {CaptureEntry::instanced, "DrawInstanced pixels"},
        std::pair {CaptureEntry::indexed_instanced, "DrawIndexedInstanced pixels"},
        std::pair {CaptureEntry::indirect, "DrawInstancedIndirect pixels"},
        std::pair {CaptureEntry::indexed_indirect, "DrawIndexedInstancedIndirect pixels"},
        std::pair {CaptureEntry::command_list, "deferred Draw / ExecuteCommandList pixels"},
        std::pair {CaptureEntry::dispatch, "Dispatch pixels"}})
    {
        expect(draw_capture(device, context, entry), description);
        context->ClearState();
    }
}

void check_command_retention(HMODULE bridge, ID3D11Device *draw_device,
    ID3D11DeviceContext *draw_context, bool first_window)
{
    const auto observed = [&](ID3D11DeviceContext *context)
    {
        const auto routes = draw_routes(context);
        return belongs_to(bridge, routes[0]) && belongs_to(bridge, routes[1]);
    };
    // 2026-11-16：独立门禁检出 Flush 丢钩；硬件预热先建立合法的
    // shader/索引状态，且独立模式不得污染其它夹具的首次回执负控。
    expect(draw_capture(draw_device, draw_context, CaptureEntry::indexed, {}, stdout),
        "retention precondition draws exact pixels with a valid pipeline");
    // 候选日志只报首次；不能清掉唯一回执后要求第二轮重复上报。
    if (first_window)
        expect(WaitForSingleObject(capture_receipt, 5000) == WAIT_OBJECT_0,
            "retention precondition reached production capture");
    for (unsigned cycle = 0; cycle < 3; ++cycle)
    {
        draw_context->Draw(0, 0);
        expect(observed(draw_context), "warmup Draw retains both production routes");
        draw_context->DrawIndexed(0, 0, 0);
        expect(observed(draw_context), "warmup DrawIndexed retains both production routes");
        draw_context->Flush();
        expect(observed(draw_context), "warmup Flush retains both production routes");
    }
    expect(SUCCEEDED(draw_device->GetDeviceRemovedReason()),
        "retention warmup keeps the hardware device usable");
    D3D11_TEXTURE2D_DESC color_desc {};
    color_desc.Width = color_desc.Height = 128;
    color_desc.MipLevels = color_desc.ArraySize = color_desc.SampleDesc.Count = 1;
    color_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color, copied;
    ComPtr<ID3D11RenderTargetView> color_view;
    expect(SUCCEEDED(draw_device->CreateTexture2D(&color_desc, nullptr, &color)) &&
        SUCCEEDED(draw_device->CreateTexture2D(&color_desc, nullptr, &copied)) &&
        SUCCEEDED(draw_device->CreateRenderTargetView(color.Get(), nullptr, &color_view)),
        "retention clear/copy precondition creates valid color resources");
    if (color_view && copied)
    {
        // 2026-11-16：实读硬件把0.5清成127，WARP为128；端点颜色隔离命令路由与量化。
        const FLOAT value[] = {0.0f, 1.0f, 1.0f, 1.0f};
        draw_context->ClearRenderTargetView(color_view.Get(), value);
        expect(observed(draw_context), "target clear retains both production routes before readback");
        // 各操作独立开窗，前一项失配不能污染后一项的起点。
        draw_context->Flush();
        expect(observed(draw_context), "Flush establishes the next copy observation window");
        draw_context->CopySubresourceRegion(copied.Get(), 0, 0, 0, 0, color.Get(), 0, nullptr);
        expect(observed(draw_context), "subresource copy retains both production routes before readback");
        draw_context->Flush();
        color_desc.Usage = D3D11_USAGE_STAGING;
        color_desc.BindFlags = 0;
        color_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;
        expect(SUCCEEDED(draw_device->CreateTexture2D(&color_desc, nullptr, &readback)),
            "retention clear/copy readback texture exists");
        if (readback)
        {
            draw_context->CopyResource(readback.Get(), copied.Get());
            D3D11_MAPPED_SUBRESOURCE mapped {};
            const HRESULT read = draw_context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped);
            expect(SUCCEEDED(read), "retention clear/copy readback maps");
            if (SUCCEEDED(read))
            {
                const auto *pixel = static_cast<const unsigned char *>(mapped.pData);
                std::printf("retention copied pixel=%u,%u,%u,%u\n",
                    pixel[0], pixel[1], pixel[2], pixel[3]);
                expect(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 255 && pixel[3] == 255,
                    "retention clear and subresource copy preserve exact pixels");
                draw_context->Unmap(readback.Get(), 0);
            }
        }
    }
    D3D11_TEXTURE2D_DESC depth_desc {};
    depth_desc.Width = depth_desc.Height = 128;
    depth_desc.MipLevels = depth_desc.ArraySize = depth_desc.SampleDesc.Count = 1;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> depth_view;
    expect(SUCCEEDED(draw_device->CreateTexture2D(&depth_desc, nullptr, &depth)) &&
        SUCCEEDED(draw_device->CreateDepthStencilView(depth.Get(), nullptr, &depth_view)),
        "retention depth-clear precondition creates a valid DSV");
    if (depth_view)
    {
        draw_context->ClearDepthStencilView(depth_view.Get(), D3D11_CLEAR_DEPTH, 0.25f, 0);
        expect(observed(draw_context), "depth clear retains both production routes before readback");
    }
}

struct ForHwndFixture
{
    using Create = decltype(&D3D11CreateDevice);
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D11Device> device, witness;
    ComPtr<ID3D11DeviceContext> context;

    HRESULT init(bool warp, bool mixed)
    {
        // 混合用例明确经过加载器所用的 F6 DXGI 代理导出。系统工厂有时仅因
        // 后台 GPU 枚举已安装共享方法 detour 才进 F6，不能把这种时序当前置条件。
        const auto create = mixed ? reinterpret_cast<decltype(&CreateDXGIFactory1)>(
            GetProcAddress(GetModuleHandleW(L"OptiScaler.dll"), "CreateDXGIFactory1")) : &CreateDXGIFactory1;
        if (!create) return E_NOINTERFACE;
        // 2026-11-15：F6 的初始化线程可能晚于 LoadLibrary 返回；普通链不能冒充
        // 混合正控。传入的隔离配置须将 debug 日志写到 ./OptiScaler.log。
        // 以本宿主实际进入 F6 factory 的回执为闸门，超时失败，不以固定延时放行。
        const auto deadline = GetTickCount64() + 10000;
        wchar_t executable[MAX_PATH] {};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        const auto marker = "hkCreateDXGIFactory1 Caller: " + std::filesystem::path(executable).filename().string();
        unsigned attempts = 0;
        for (;;)
        {
            factory.Reset();
            const auto hr = create(IID_PPV_ARGS(&factory));
            if (FAILED(hr)) return hr;
            ++attempts;
            if (!mixed) break;
            std::ifstream log("OptiScaler.log");
            std::string line;
            bool ready = false;
            while (std::getline(log, line))
                if (line.find(marker) != std::string::npos) { ready = true; break; }
            if (ready)
            {
                std::printf("F6 factory hook receipt observed after %u calls\n", attempts);
                break;
            }
            if (GetTickCount64() >= deadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            Sleep(20);
        }
        if (!warp) return factory->EnumAdapters(0, &adapter);
        ComPtr<IDXGIFactory4> factory4;
        const auto qi = factory.As(&factory4);
        return FAILED(qi) ? qi : factory4->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    }

    HRESULT create_device(Create create, bool target)
    {
        return create(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, target ? device.GetAddressOf() : witness.GetAddressOf(), nullptr,
            target ? context.GetAddressOf() : nullptr);
    }

    void run(HMODULE bridge, Create create, bool device_first, bool returned, bool mixed, bool mirrored,
        bool multithread, unsigned present_version, bool retention)
    {
        // 设备先用例只通过另一设备登记同一 factory；不能借此登记待测 context。
        const HRESULT created = create_device(create, !device_first);
        expect(SUCCEEDED(created) && device && context, "same-adapter device creation succeeds");
        if (FAILED(created) || !device || !context) return;
        const auto observed = [&](ID3D11DeviceContext *which) {
            const auto routes = draw_routes(which);
            return belongs_to(bridge, routes[0]) && belongs_to(bridge, routes[1]);
        };
        print_context("original-before-ForHwnd", context.Get());
        expect(observed(context.Get()) == !device_first, "original Draw routes before ForHwnd match load order");
        if (!mixed)
            expect(belongs_to(bridge, (*reinterpret_cast<void ***>(factory.Get()))[15]),
                "the actual factory receiver is hooked, not just another instance");

        DXGI_SWAP_CHAIN_DESC1 desc {};
        // 混合用例沿实际会话的窗口链描述走 F6 的 DX11-w-DX12 包装，不退回普通链充当正控。
        desc.Width = mixed ? 2560 : 128;
        desc.Height = mixed ? 1600 : 128;
        desc.Format = mixed ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferCount = mixed ? 1 : 2;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | (mixed ? DXGI_USAGE_SHADER_INPUT : 0);
        desc.SwapEffect = mixed ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = mixed ? DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH : 0;
        ComPtr<IDXGISwapChain1> chain;
        auto installed = draw_routes(context.Get());
        if (!mixed)
            expect(FAILED(factory->CreateSwapChainForHwnd(device.Get(), nullptr, &desc, nullptr, nullptr, &chain)) &&
                !chain && draw_routes(context.Get()) == installed, "failed ForHwnd cannot change the context's routes");

        const bool compare_alternates = returned && !mixed;
        const unsigned rounds = mixed ? 1 : 2;
        for (unsigned round = 0; round < rounds; ++round)
        {
            const HWND window = CreateWindowExW(0, L"STATIC", L"FSR bridge routing fixture", WS_POPUP,
                0, 0, desc.Width, desc.Height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            const auto hr = window ? factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr,
                nullptr, &chain) : E_FAIL;
            std::printf("CreateSwapChainForHwnd hr=%08lX chain=%p\n", static_cast<unsigned long>(hr), chain.Get());
            expect(SUCCEEDED(hr) && chain, "real ForHwnd returns a swap chain");
            if (!chain)
            {
                if (window) DestroyWindow(window);
                return;
            }
            // 2026-11-15：混合宿主曾在普通交换链上通过绘制断言；必须另证实际
            // 返回的是 F6 包装，才能把该用例计为 DX11-w-DX12 路径的正控。
            if (mixed)
                expect(belongs_to(GetModuleHandleW(L"OptiScaler.dll"),
                    (*reinterpret_cast<void ***>(chain.Get()))[7]),
                    "mixed ForHwnd returns an F6-owned GetDevice implementation");
            if (present_version != 0)
            {
                // 2026-11-16：PID290040 的内部链回执早于 F6 最终返回；不能用内部 Present 代表游戏链。
                auto **table = *reinterpret_cast<void ***>(chain.Get());
                expect(belongs_to(bridge, table[8]) && belongs_to(bridge, table[22]),
                    "final game-facing chain binds both Neo Present routes");
            }
            ComPtr<ID3D11Device> chain_device;
            const auto get_device = chain->GetDevice(IID_PPV_ARGS(&chain_device));
            std::printf("swapchain GetDevice hr=%08lX device=%p original=%p\n",
                static_cast<unsigned long>(get_device), chain_device.Get(), device.Get());
            expect(SUCCEEDED(get_device) && same_identity(device.Get(), chain_device.Get()),
                "returned chain belongs to the original DX11 device");
            ComPtr<ID3D11DeviceContext> fetched;
            if (chain_device) chain_device->GetImmediateContext(&fetched);
            expect(same_identity(context.Get(), fetched.Get()), "chain's context has the original COM identity");
            print_context("original-after-ForHwnd", context.Get());
            print_context("swapchain-immediate", fetched.Get());
            ComPtr<ID3D11DeviceContext4> extended;
            const auto query = fetched ? fetched.As(&extended) : E_POINTER;
            std::printf("context4 QI hr=%08lX\n", static_cast<unsigned long>(query));
            if (extended)
            {
                auto *execution = static_cast<ID3D11DeviceContext *>(extended.Get());
                print_context("swapchain-context4", execution);
                expect(same_identity(fetched.Get(), execution), "extended context retains the COM identity");
                expect(observed(execution), "extended interface has both production Draw routes");
            }
            expect(observed(context.Get()) && fetched && observed(fetched.Get()),
                "original and chain-derived contexts both have production Draw routes");
            if (compare_alternates || mirrored || !mixed)
            {
                const auto table = *reinterpret_cast<void ***>(context.Get());
                const bool expect_mirror = mirrored || !DX11FSRBRIDGE_TEST_RELEASE;
                // 公开 ID3D11DeviceContext 槽：4 个状态 setter 与 OMSetRenderTargets。
                for (const auto method : { 8, 9, 16, 33, 44 })
                    expect(belongs_to(bridge, table[method]) == expect_mirror,
                        "same execution context binds only configuration-required state observers");
            }
            if (round == 0)
                installed = draw_routes(context.Get());
            else
                expect(draw_routes(context.Get()) == installed, "repeat observation keeps the same Draw routes");
            expect(WaitForSingleObject(hwnd_receipt, 5000) == WAIT_OBJECT_0,
                "production success receipt proves real ForHwnd entered the bridge");
            ResetEvent(hwnd_receipt);

            auto *draw_context = returned ? fetched.Get() : context.Get();
            auto *draw_device = returned ? chain_device.Get() : device.Get();
            if (present_version != 0)
            {
                // 2026-11-16：PID284936 只见 Present、不见 Draw；原混合夹具未覆盖首次 Present 后的入口。
                for (unsigned frame = 0; frame < 3; ++frame)
                {
                    const DXGI_PRESENT_PARAMETERS parameters {};
                    const HRESULT result = present_version == 1 ? chain->Present(0, 0) :
                        chain->Present1(0, 0, &parameters);
                    expect(SUCCEEDED(result), "real Present succeeds before the first game Draw");
                    expect(observed(draw_context), "first Present retains both production Draw routes");
                }
            }
            if (multithread && draw_context && draw_device)
            {
                ComPtr<ID3D11Multithread> protection;
                expect(SUCCEEDED(draw_context->QueryInterface(IID_PPV_ARGS(protection.GetAddressOf()))) && protection,
                    "execution context exposes multithread protection");
                if (!protection) return;
                ComPtr<ID3D11DeviceContext> execution;
                expect(SUCCEEDED(protection.As(&execution)) && same_identity(draw_context, execution.Get()),
                    "protection QI reaches the same execution context");
                const auto all_routes = [&]
                {
                    auto **table = *reinterpret_cast<void ***>(draw_context);
                    for (const auto slot : {12, 13, 14, 15, 47, 48})
                        if (!belongs_to(bridge, std::atomic_ref<void *>(table[slot]).load(std::memory_order_acquire)))
                            return false;
                    for (const auto slot : {8, 9, 16, 33, 44})
                        if (belongs_to(bridge, std::atomic_ref<void *>(table[slot]).load(std::memory_order_acquire)) !=
                            (mirrored || !DX11FSRBRIDGE_TEST_RELEASE)) return false;
                    return belongs_to(bridge, (*reinterpret_cast<void ***>(protection.Get()))[5]);
                };
                const BOOL initial = protection->GetMultithreadProtected();
                std::size_t switches = 0;
                for (const BOOL wanted : {TRUE, TRUE, FALSE, FALSE, TRUE})
                {
                    const BOOL prior = protection->GetMultithreadProtected();
                    expect(protection->SetMultithreadProtected(wanted) == prior &&
                        protection->GetMultithreadProtected() == wanted, "switch preserves BOOL result and new state");
                    expect(all_routes(), "switch retains precisely the configured capture routes");
                    const bool indexed_first = ++switches % 2 == 0;
                    for (const auto entry : indexed_first ? std::array {CaptureEntry::indexed, CaptureEntry::draw} :
                        std::array {CaptureEntry::draw, CaptureEntry::indexed})
                    {
                        expect(draw_capture(draw_device, draw_context, entry), "switched Draw preserves exact pixels");
                        expect(all_routes(), "Draw and blocking readback retain capture routes");
                    }
                    draw_context->ClearState();
                    expect(all_routes(), "ClearState retains capture routes");
                }
                protection->Enter();
                const BOOL prior = protection->SetMultithreadProtected(TRUE);
                protection->Leave();
                expect(prior == TRUE && all_routes(), "caller-held protection allows a nested switch");
                protection->SetMultithreadProtected(initial);
                expect(all_routes(), "restoring initial protection retains capture routes");
            }
            if (draw_context && draw_device)
            {
                if (retention)
                    check_command_retention(bridge, draw_device, draw_context, round == 0);
                if (compare_alternates && round == 0)
                    check_alternate_draws(draw_device, draw_context);
                if (round == rounds - 1)
                {
                    // 下一次 ForHwnd 回执穿过同一日志队列，避免把尚未消费的候选当作负控成功。
                    if (compare_alternates)
                        expect(draw_entries.load() == 0 && WaitForSingleObject(capture_receipt, 0) == WAIT_TIMEOUT,
                            "alternate calls produce pixels but do not enter the two production Draw hooks");
                    if (expected_draw_entries & 2)
                        expect(draw_capture(draw_device, draw_context, CaptureEntry::indexed),
                            "selected immediate context DrawIndexed preserves real pixels");
                    if (expected_draw_entries & 1)
                        expect(draw_capture(draw_device, draw_context),
                            "selected immediate context Draw preserves real pixels");
                    draw_context->ClearState();
                }
            }
            chain.Reset();
            DestroyWindow(window);
        }
        expect(WaitForSingleObject(draw_receipt, 5000) == WAIT_OBJECT_0,
            "selected immediate context reached the requested production Draw entrypoints");
    }
};

HRESULT call_combined(FARPROC target)
{
    return reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(target)(nullptr,
        D3D_DRIVER_TYPE_NULL, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, nullptr,
        nullptr, nullptr, nullptr, nullptr);
}
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 7 && argc != 9)
        return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *fault) -> LONG
    {
        std::printf("fixture exception=%08lX at=%p\n", fault->ExceptionRecord->ExceptionCode,
            fault->ExceptionRecord->ExceptionAddress);
        void *frames[16] {};
        const USHORT count = CaptureStackBackTrace(0, 16, frames, nullptr);
        for (USHORT i = 0; i < count; ++i)
        {
            HMODULE module = nullptr;
            wchar_t path[MAX_PATH] {};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(frames[i]), &module);
            if (module)
                GetModuleFileNameW(module, path, MAX_PATH);
            std::printf("  %u %p %ls+%llX\n", i, frames[i], path,
                reinterpret_cast<unsigned long long>(frames[i]) - reinterpret_cast<unsigned long long>(module));
        }
        return EXCEPTION_EXECUTE_HANDLER;
    });
    const std::wstring mode = argv[6];
    const bool device_only = mode.ends_with(L"device-only");
    const bool context_only = mode.ends_with(L"context-only");
    const bool real_combined = mode.find(L"combined") != std::wstring::npos;
    const bool real_device = mode.starts_with(L"warp") || mode.starts_with(L"hardware");
    const bool resize_routing = mode == L"resize-routing" || mode == L"resize-routing-reversed";
    const bool hdr_swapchain = mode == L"swapchain4" || resize_routing;
    const bool swapchain_mode = mode == L"swapchain" || hdr_swapchain;
    const bool creation_boundary = mode == L"wrapping-boundary" || mode == L"wrapping-boundary-combined" ||
        mode == L"wrapping-boundary-retry";
    const bool nested = mode == L"nested" || mode == L"nested-success" || creation_boundary;
    const bool success = mode == L"success" || mode == L"nested-success" || swapchain_mode || creation_boundary;
    const bool for_hwnd = mode.find(L"for-hwnd") != std::wstring::npos;
    const bool device_first = for_hwnd && mode.find(L"device-first") != std::wstring::npos;
    const bool returned = for_hwnd && mode.ends_with(L"-returned");
    const bool mirrored = for_hwnd && mode.ends_with(L"-mirrored");
    const bool mixed = argc == 9;
    const bool bridge_before_f6 = mixed && mode.find(L"bridge-first") != std::wstring::npos;
    if (mixed && (!for_hwnd || !mode.starts_with(L"hardware"))) return 2;
    if (for_hwnd && mode.starts_with(L"warp"))
        expected_draw_entries = mode.ends_with(L"-indexed") ? 2 : 1;
    const bool cached_factory = for_hwnd || mode.find(L"cached-factory") != std::wstring::npos;
    const bool free_bridge = mode.find(L"free-library") != std::wstring::npos;
    const HMODULE first = LoadLibraryW(argv[2]);
    const HMODULE second = LoadLibraryW(argv[3]);
    const HMODULE imports1 = LoadLibraryW(argv[4]);
    const HMODULE imports2 = LoadLibraryW(argv[5]);
    if (!first || !second || !imports1 || !imports2 || first == second)
        return 3;
    wchar_t cached_system[MAX_PATH] {};
    GetSystemDirectoryW(cached_system, MAX_PATH);
    const HMODULE cached_runtime = cached_factory ?
        LoadLibraryW((std::filesystem::path(cached_system) / L"d3d11.dll").c_str()) : nullptr;
    const auto cached_device = cached_factory ? reinterpret_cast<decltype(&D3D11CreateDevice)>(
        GetProcAddress(cached_runtime, "D3D11CreateDevice")) : nullptr;
    const auto cached_combined = cached_factory ? reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(
        GetProcAddress(cached_runtime, "D3D11CreateDeviceAndSwapChain")) : nullptr;
    const auto original_get_proc = &GetProcAddress;
    const char *names[] = { "D3D11CreateDevice", "D3D11CreateDeviceAndSwapChain" };
    for (const char *name : names)
        if (!set_import(imports2, name, original_get_proc(second, name)))
            return 4;
    const auto import_device1 = reinterpret_cast<HRESULT(WINAPI *)()>(GetProcAddress(imports1, "CallDevice"));
    const auto import_device2 = reinterpret_cast<HRESULT(WINAPI *)()>(GetProcAddress(imports2, "CallDevice"));
    const auto import_combined1 = reinterpret_cast<HRESULT(WINAPI *)()>(GetProcAddress(imports1, "CallCombined"));
    const auto import_combined2 = reinterpret_cast<HRESULT(WINAPI *)()>(GetProcAddress(imports2, "CallCombined"));
    if (!import_device1 || !import_device2 || !import_combined1 || !import_combined2)
        return 5;
    if (nested)
        reinterpret_cast<void(WINAPI *)(HMODULE)>(GetProcAddress(second, "SetInner"))(first);
    if (success)
    {
        reinterpret_cast<void(WINAPI *)(BOOL)>(GetProcAddress(first, "SetSuccess"))(TRUE);
        reinterpret_cast<void(WINAPI *)(BOOL)>(GetProcAddress(second, "SetSuccess"))(TRUE);
        for (const HMODULE provider : {first, second})
        {
            reinterpret_cast<void(WINAPI *)(BOOL)>(GetProcAddress(provider, "SetExtended"))(nested);
            reinterpret_cast<void(WINAPI *)(BOOL)>(GetProcAddress(provider, "SetSwapchain"))(swapchain_mode);
            reinterpret_cast<void(WINAPI *)(BOOL)>(GetProcAddress(provider, "SetHdrSwapchain"))(hdr_swapchain);
            // 错配专项也运行旧 DLL 负控，保留旧版本所读的额外 backing 槽。
            // 精确页边界继续由其余成功路径用例独立覆盖。
            if (!resize_routing)
                reinterpret_cast<void(WINAPI *)()>(GetProcAddress(provider, "GuardTables"))();
        }
    }

    // 每个子进程使用自己的配置/日志目录，不触碰安装目录或用户预设。
    wchar_t executable[MAX_PATH] {};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto run = std::filesystem::path(executable).parent_path() /
        (L"create-routing-run-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(run);
    const auto load_f6 = [&] {
        // 只在宿主目录装入调用者明确指定的副本，不从游戏或启动器选择 DLL/配置。
        const auto opti_path = run / L"OptiScaler.dll";
        std::filesystem::copy_file(argv[7], opti_path, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(argv[8], run / L"OptiScaler.ini", std::filesystem::copy_options::overwrite_existing);
        return SetCurrentDirectoryW(run.c_str()) && LoadLibraryW(opti_path.c_str());
    };
    if (mixed && !bridge_before_f6 && !load_f6()) return 17;
    ForHwndFixture hwnd_fixture;
    const auto init_fixture = [&] {
        const auto hr = cached_device ? hwnd_fixture.init(mode.starts_with(L"warp"), mixed) : E_NOINTERFACE;
        std::printf("ForHwnd fixture init hr=%08lX\n", static_cast<unsigned long>(hr));
        return SUCCEEDED(hr) && (!device_first || SUCCEEDED(hwnd_fixture.create_device(cached_device, true)));
    };
    if (for_hwnd && !bridge_before_f6 && !init_fixture()) return 16;
    const auto bridge_path = run / L"Dx11FsrBridge.dll";
    std::filesystem::copy_file(argv[1], bridge_path, std::filesystem::copy_options::overwrite_existing);
    std::ofstream(run / L"Dx11FsrBridge.ini") <<
        "[Dx11FsrBridge]\nEnabled=1\nTransparentJitter=0\nFsr2Il2CppHook=0\nRenderScaleMenu=0\nFfx12Backend=ngx\n"
        << (mirrored ? "Fsr2Mode2OnDemandState=0\n" : "Fsr2Mode2OnDemandState=1\n")
        << "[Log]\nto_debugger=1\n";
    const HMODULE bridge = LoadLibraryW(bridge_path.c_str());
    if (!bridge)
        return 6;
    if (bridge_before_f6 && (!load_f6() || !init_fixture())) return 17;
    if (real_device)
    {
        // 2026-11-15：文件流可一直缓冲到退出；在隔离进程的既有调试sink上
        // 等生产消息回执，不能把ExitProcess或固定延时当作日志队列已排空。
        capture_receipt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (for_hwnd)
        {
            hwnd_receipt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            draw_receipt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!hwnd_receipt || !draw_receipt) return 12;
        }
        if (!capture_receipt || !set_import(bridge, "OutputDebugStringA",
            reinterpret_cast<FARPROC>(&capture_debug_line), nullptr))
            return 12;
    }
    if (free_bridge)
    {
        // 2026-11-15：已公开 IAT/vtable 回调的桥必须存活至进程退出。
        // 释放宿主 LoadLibrary 引用后继续真实绘制，覆盖仅检查模块句柄的盲区。
        expect(FreeLibrary(bridge) != FALSE, "host releases its bridge module reference");
        expect(GetModuleHandleW(bridge_path.c_str()) == bridge,
            "bridge callbacks remain mapped after host FreeLibrary");
    }

    // 反向查询第二提供者，不能让查询顺序或 IAT 首次绑定决定 original。
    const auto device2 = GetProcAddress(second, names[0]);
    const auto combined2 = GetProcAddress(second, names[1]);
    const auto device1 = GetProcAddress(first, names[0]);
    const auto combined1 = GetProcAddress(first, names[1]);
    expect(mixed ? device1 && device2 && combined1 && combined2 :
        belongs_to(bridge, reinterpret_cast<void *>(device1)) &&
        belongs_to(bridge, reinterpret_cast<void *>(device2)) &&
        belongs_to(bridge, reinterpret_cast<void *>(combined1)) &&
        belongs_to(bridge, reinterpret_cast<void *>(combined2)), "both creation APIs actually enter production bridge");
    if (creation_boundary)
    {
        if (mode == L"wrapping-boundary-retry")
        {
            for (const HMODULE provider : {first, second})
                reinterpret_cast<void(WINAPI *)(BOOL)>(GetProcAddress(provider, "SetSuccess"))(FALSE);
            expect(call_device(device2) == E_INVALIDARG, "failed nested creation retains exact failure");
            for (const HMODULE provider : {first, second})
                reinterpret_cast<void(WINAPI *)(BOOL)>(GetProcAddress(provider, "SetSuccess"))(TRUE);
        }
        ID3D11Device *device = nullptr;
        ID3D11DeviceContext *context = nullptr;
        HRESULT result = E_UNEXPECTED;
        if (mode == L"wrapping-boundary-combined")
        {
            reinterpret_cast<void(WINAPI *)(BOOL)>(GetProcAddress(second, "SetCombinedUsesDevice"))(TRUE);
            result = reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(combined2)(nullptr,
                D3D_DRIVER_TYPE_NULL, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, nullptr,
                nullptr, &device, nullptr, &context);
        }
        else
            result = call_device(device2, &device, &context);
        expect(result == S_OK && device && context, "outer wrapper creation succeeds");
        const unsigned premature = reinterpret_cast<unsigned(WINAPI *)()>(
            GetProcAddress(second, "GetEarlyObservation"))();
        std::printf("methods changed before wrapper returned: %u\n", premature);
        expect(premature == 0, "inner device, context and factory remain untouched during wrapping");
        if (result != S_OK || !device || !context)
            return 15;
        expect(belongs_to(bridge, (*reinterpret_cast<void ***>(device))[3]),
            "outer returned device is observed after wrapping");
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = 16;
        expect(device->CreateBuffer(&desc, nullptr, nullptr) == E_INVALIDARG,
            "completed wrapper keeps matching receiver and original");
        const auto native_context = reinterpret_cast<ID3D11DeviceContext *(WINAPI *)()>(
            GetProcAddress(first, "GetContext"))();
        expect(belongs_to(bridge, (*reinterpret_cast<void ***>(native_context))[13]) &&
            !belongs_to(bridge, (*reinterpret_cast<void ***>(context))[13]),
            "native execution is observed after return while proxy retains draw processing");
        context->Draw(5, 1);
        for (const HMODULE provider : {first, second})
        {
            expect(reinterpret_cast<unsigned(WINAPI *)()>(GetProcAddress(provider, "GetDraws"))() == 1,
                "proxy and runtime each process the draw exactly once");
            expect(reinterpret_cast<unsigned(WINAPI *)()>(GetProcAddress(provider, "GetWrongThis"))() == 0,
                "construction boundary preserves exact COM receiver");
        }
    }
    else if (resize_routing)
    {
        ID3D11Device *device = nullptr;
        expect(call_device(device1, &device) == S_OK && device, "single provider creates device");
        if (!device)
            return 12;
        IDXGIDevice *dxgi = nullptr;
        IDXGIAdapter *adapter = nullptr;
        IDXGIFactory2 *factory = nullptr;
        if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi))) ||
            FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory))))
            return 13;
        const auto select_wrapper = reinterpret_cast<void(WINAPI *)(BOOL)>(GetProcAddress(first, "SetWrapperChain"));
        const bool reversed = mode == L"resize-routing-reversed";
        IDXGISwapChain1 *chains[2] {};
        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Width = 640;
        desc.Height = 360;
        for (unsigned i = 0; i < 2; ++i)
        {
            select_wrapper(reversed ? i == 0 : i != 0);
            expect(SUCCEEDED(factory->CreateSwapChainForHwnd(device, nullptr, &desc, nullptr, nullptr, &chains[i])) &&
                chains[i], "factory returns distinct-layout swapchain");
            if (!chains[i])
                return 14;
            expect(belongs_to(bridge, (*reinterpret_cast<void ***>(chains[i]))[14]) == k_ngx_swapchain_hooks,
                "ResizeTarget hook presence matches the compiled backend");
        }
        expect(chains[0] != chains[1], "two live swapchains have distinct receiver identities");
        DXGI_MODE_DESC target {};
        target.Width = 640;
        target.Height = 360;
        for (unsigned pass = 0; pass < 2; ++pass)
            for (auto *chain : chains)
                expect(chain->ResizeTarget(&target) == E_ABORT,
                    "interleaved ResizeTarget retains matching receiver, layout and original");
    }
    else if (for_hwnd)
        hwnd_fixture.run(bridge, cached_device, device_first, returned, mixed, mirrored,
            mode.find(L"multithread") != std::wstring::npos,
            mode.ends_with(L"-present1") ? 2 : mode.ends_with(L"-present") ? 1 : 0,
            mode.ends_with(L"-retention"));
    else if (real_device)
    {
        wchar_t system[MAX_PATH] {};
        GetSystemDirectoryW(system, MAX_PATH);
        const HMODULE runtime = LoadLibraryW((std::filesystem::path(system) / L"d3d11.dll").c_str());
        const auto create = cached_factory ? cached_device :
            reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(runtime, names[0]));
        expect(cached_factory || belongs_to(bridge, reinterpret_cast<void *>(create)), "system WARP create route is callable");
        for (unsigned round = 0; round < 2; ++round)
        {
            ID3D11Device *device = nullptr;
            ID3D11DeviceContext *context = nullptr;
            const auto driver = mode.starts_with(L"hardware") ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
            IDXGISwapChain *chain = nullptr;
            HWND window = nullptr;
            HRESULT created = E_FAIL;
            if (real_combined)
            {
                window = CreateWindowExW(0, L"STATIC", L"FSR bridge isolated draw test", WS_POPUP,
                    0, 0, 128, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                DXGI_SWAP_CHAIN_DESC desc {};
                desc.BufferDesc.Width = desc.BufferDesc.Height = 128;
                desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = desc.BufferCount = 1;
                desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                desc.OutputWindow = window;
                desc.Windowed = TRUE;
                const auto combined = cached_factory ? cached_combined :
                    reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(GetProcAddress(runtime, names[1]));
                expect(cached_factory || belongs_to(bridge, reinterpret_cast<void *>(combined)),
                    "system combined creation actually enters bridge");
                created = combined(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                    &desc, &chain, &device, nullptr, nullptr);
            }
            else
                created = create(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                    context_only ? nullptr : &device, nullptr, device_only ? nullptr : &context);
            if (SUCCEEDED(created) && context && context_only)
                context->GetDevice(&device);
            if (SUCCEEDED(created) && device && device_only)
                device->GetImmediateContext(&context);
            expect(SUCCEEDED(created) && device && context, "real WARP device and context returned");
            if (FAILED(created) || !device || !context)
                return 8;
            expect(belongs_to(bridge, (*reinterpret_cast<void ***>(device))[3]) &&
                belongs_to(bridge, (*reinterpret_cast<void ***>(context))[47]), "real device/context methods enter bridge");
            const auto level = device->GetFeatureLevel();
            expect(level >= D3D_FEATURE_LEVEL_11_0, "real GetFeatureLevel preserves runtime private dispatch");
            expect(SUCCEEDED(device->GetDeviceRemovedReason()), "real device status remains callable");
            expect(draw_capture(device, context), "real shaders draw both outputs and preserve exact pixels");
            context->ClearState();
            ID3D11DeviceContext *fetched = nullptr;
            device->GetImmediateContext(&fetched);
            expect(fetched != nullptr, "real GetImmediateContext remains callable");
            if (fetched)
                fetched->Release();
            unsigned data[16] {};
            for (unsigned i = 0; i < 16; ++i)
                data[i] = 0x12345678 + i + round;
            D3D11_BUFFER_DESC desc {};
            desc.ByteWidth = sizeof(data);
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA initial {};
            initial.pSysMem = data;
            ID3D11Buffer *source = nullptr;
            ID3D11Buffer *readback = nullptr;
            expect(SUCCEEDED(device->CreateBuffer(&desc, &initial, &source)) && source, "WARP source buffer created");
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            expect(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &readback)) && readback, "WARP readback buffer created");
            if (!source || !readback)
                return 9;
            context->CopyResource(readback, source);
            D3D11_MAPPED_SUBRESOURCE mapped {};
            const HRESULT read = context->Map(readback, 0, D3D11_MAP_READ, 0, &mapped);
            expect(SUCCEEDED(read) && mapped.pData, "WARP copy completed before CPU mapping");
            if (SUCCEEDED(read))
            {
                expect(std::memcmp(data, mapped.pData, sizeof(data)) == 0, "real WARP bytes preserved through routed calls");
                context->Unmap(readback, 0);
            }
            readback->Release();
            source->Release();
            if (chain) chain->Release();
            if (window) DestroyWindow(window);
            // 保留至进程退出，避免旧桥本身未退休的实例地址重用干扰本回归。
        }
    }
    else if (!success)
    {
        expect(call_device(device1) == E_ABORT, "GPA first device provider");
        expect(call_device(device2) == E_INVALIDARG, "GPA second device provider");
        expect(call_combined(combined1) == E_ABORT, "GPA first combined provider");
        expect(call_combined(combined2) == E_INVALIDARG, "GPA second combined provider");
        expect(import_device1() == E_ABORT, "IAT first device provider");
        expect(import_device2() == E_INVALIDARG, "IAT second device provider");
        expect(import_combined1() == E_ABORT, "IAT first combined provider");
        expect(import_combined2() == E_INVALIDARG, "IAT second combined provider");
        const auto count1 = reinterpret_cast<unsigned(WINAPI *)()>(GetProcAddress(first, "GetCalls"));
        const auto count2 = reinterpret_cast<unsigned(WINAPI *)()>(GetProcAddress(second, "GetCalls"));
        expect(count1() == (nested ? 8u : 4u) && count2() == 4,
            "proxy/native execution counts preserve complete chain");
    }
    else
    {
        ID3D11Device *native_device = nullptr;
        ID3D11Device *proxy_device = nullptr;
        ID3D11DeviceContext *native_context = nullptr;
        ID3D11DeviceContext *proxy_context = nullptr;
        expect(call_device(device1, &native_device, &native_context) == S_OK && native_device, "native successful device observed");
        expect(call_device(device2, &proxy_device, &proxy_context) == S_OK && proxy_device, "proxy successful device observed");
        expect(belongs_to(bridge, (*reinterpret_cast<void ***>(native_device))[3]) &&
            belongs_to(bridge, (*reinterpret_cast<void ***>(proxy_device))[3]), "both device methods actually enter bridge");
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = 16;
        expect(native_device->CreateBuffer(&desc, nullptr, nullptr) == E_ABORT,
            "native device retains its own original method");
        expect(proxy_device->CreateBuffer(&desc, nullptr, nullptr) == E_INVALIDARG,
            "proxy device retains its own original method");
        expect(native_context && proxy_context &&
            belongs_to(bridge, (*reinterpret_cast<void ***>(native_context))[13]) &&
            belongs_to(bridge, (*reinterpret_cast<void ***>(proxy_context))[13]) == !nested,
            "native context enters bridge while forwarding proxy retains its original Draw");
        native_context->Draw(5, 1);
        proxy_context->Draw(5, 1);
        for (const HMODULE provider : {first, second})
        {
            const auto wrong = reinterpret_cast<unsigned(WINAPI *)()>(GetProcAddress(provider, "GetWrongThis"));
            const auto draws = reinterpret_cast<unsigned(WINAPI *)()>(GetProcAddress(provider, "GetDraws"));
            const unsigned expected_draws = nested && provider == first ? 2 : 1;
            expect(wrong() == 0 && draws() == expected_draws, "Draw forwards exact receiver once per layer");
        }
        for (ID3D11Device *device : {native_device, proxy_device})
        {
            IDXGIDevice *dxgi = nullptr;
            IDXGIAdapter *adapter = nullptr;
            IDXGIFactory2 *factory = nullptr;
            if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi))) ||
                FAILED(dxgi->GetAdapter(&adapter)) ||
                FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory))))
                return 7;
            expect(belongs_to(bridge, (*reinterpret_cast<void ***>(factory))[10]) &&
                belongs_to(bridge, (*reinterpret_cast<void ***>(factory))[15]), "both factory methods actually enter bridge");
            DXGI_SWAP_CHAIN_DESC legacy {};
            DXGI_SWAP_CHAIN_DESC1 hwnd_desc {};
            legacy.BufferDesc.Width = hwnd_desc.Width = 640;
            legacy.BufferDesc.Height = hwnd_desc.Height = 360;
            const HRESULT expected = device == native_device ? E_ABORT : E_INVALIDARG;
            IDXGISwapChain *chain = nullptr;
            IDXGISwapChain1 *hwnd_chain = nullptr;
            const HRESULT legacy_result = factory->CreateSwapChain(device, &legacy, swapchain_mode ? &chain : nullptr);
            expect(legacy_result == (swapchain_mode ? S_OK : expected),
                "Factory2 and same-address Factory keep legacy original");
            expect(factory->CreateSwapChainForHwnd(device, nullptr, &hwnd_desc, nullptr, nullptr,
                swapchain_mode ? &hwnd_chain : nullptr) == (swapchain_mode ? S_OK : expected),
                "ForHwnd keeps matching factory original");
            if (swapchain_mode)
            {
                IDXGISwapChain3 *chain3 = nullptr;
                expect(chain && SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain3),
                    reinterpret_cast<void **>(&chain3))) && chain3, "guarded SwapChain3 returned");
                if (!chain3)
                    return 10;
                expect(belongs_to(bridge, (*reinterpret_cast<void ***>(chain3))[13]) == k_ngx_swapchain_hooks,
                    "SwapChain3 hook presence matches the compiled backend");
                expect(chain3->ResizeBuffers1(2, 640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                    nullptr, nullptr) == expected, "SwapChain3 final slot39 retains exact provider routing");
                if (hdr_swapchain)
                {
                    IDXGISwapChain4 *chain4 = nullptr;
                    expect(SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain4),
                        reinterpret_cast<void **>(&chain4))) && chain4, "guarded SwapChain4 returned");
                    if (!chain4)
                        return 11;
                    void **methods = *reinterpret_cast<void ***>(chain4);
                    expect(!belongs_to(bridge, methods[39]) &&
                        belongs_to(bridge, methods[40]) == k_ngx_swapchain_hooks,
                        "SwapChain4 HDR slot40 matches backend and preserves ResizeBuffers1 slot39");
                    expect(chain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr) == expected,
                        "HDR metadata retains matching provider and exact receiver");
                    chain4->Release();
                }
                chain3->Release();
            }
        }
        for (const HMODULE provider : {first, second})
            expect(reinterpret_cast<unsigned(WINAPI *)()>(GetProcAddress(provider, "GetFactoryCalls"))() == 2,
                "each real factory called exactly twice");
    }
    if (real_device)
    {
        expect(WaitForSingleObject(capture_receipt, 5000) == WAIT_OBJECT_0,
            "real Draw reached the production FSR signature capture before process exit");
    }
    std::printf("production DLL routing: %d failures\n", failures);
    std::fflush(stdout);
    ExitProcess(failures ? 1 : 0);
}
