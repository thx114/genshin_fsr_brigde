#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
int failures = 0;
void expect(bool condition, const char *what)
{
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", what);
    failures += !condition;
}

// 给两个真实 PE 导入表设置不同提供者，模拟第三方 loader 已建立的两层调用。
bool set_import(HMODULE module, const char *name, FARPROC target)
{
    auto *base = reinterpret_cast<unsigned char *>(module);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; descriptor->Name; ++descriptor)
    {
        if (_stricmp(reinterpret_cast<char *>(base + descriptor->Name), "d3d11.dll") != 0)
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

HRESULT call_combined(FARPROC target)
{
    return reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(target)(nullptr,
        D3D_DRIVER_TYPE_NULL, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, nullptr,
        nullptr, nullptr, nullptr, nullptr);
}
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 7)
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
    const bool nested = mode == L"nested" || mode == L"nested-success";
    const bool resize_routing = mode == L"resize-routing" || mode == L"resize-routing-reversed";
    const bool hdr_swapchain = mode == L"swapchain4" || resize_routing;
    const bool swapchain_mode = mode == L"swapchain" || hdr_swapchain;
    const bool success = mode == L"success" || mode == L"nested-success" || swapchain_mode;
    const HMODULE first = LoadLibraryW(argv[2]);
    const HMODULE second = LoadLibraryW(argv[3]);
    const HMODULE imports1 = LoadLibraryW(argv[4]);
    const HMODULE imports2 = LoadLibraryW(argv[5]);
    if (!first || !second || !imports1 || !imports2 || first == second)
        return 3;
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
    const auto bridge_path = run / L"Dx11FsrBridge.dll";
    std::filesystem::copy_file(argv[1], bridge_path, std::filesystem::copy_options::overwrite_existing);
    std::ofstream(run / L"Dx11FsrBridge.ini") <<
        "[Dx11FsrBridge]\nEnabled=1\nTransparentJitter=0\nFsr2Il2CppHook=0\nRenderScaleMenu=0\nFfx12Backend=ngx\n";
    const HMODULE bridge = LoadLibraryW(bridge_path.c_str());
    if (!bridge)
        return 6;

    // 反向查询第二提供者，不能让查询顺序或 IAT 首次绑定决定 original。
    const auto device2 = GetProcAddress(second, names[0]);
    const auto combined2 = GetProcAddress(second, names[1]);
    const auto device1 = GetProcAddress(first, names[0]);
    const auto combined1 = GetProcAddress(first, names[1]);
    expect(belongs_to(bridge, reinterpret_cast<void *>(device1)) &&
        belongs_to(bridge, reinterpret_cast<void *>(device2)) &&
        belongs_to(bridge, reinterpret_cast<void *>(combined1)) &&
        belongs_to(bridge, reinterpret_cast<void *>(combined2)), "both creation APIs actually enter production bridge");
    if (resize_routing)
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
            expect(belongs_to(bridge, (*reinterpret_cast<void ***>(chains[i]))[14]),
                "each ResizeTarget actually enters production bridge");
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
    else if (mode == L"warp")
    {
        wchar_t system[MAX_PATH] {};
        GetSystemDirectoryW(system, MAX_PATH);
        const HMODULE runtime = LoadLibraryW((std::filesystem::path(system) / L"d3d11.dll").c_str());
        const auto create = reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(runtime, names[0]));
        expect(belongs_to(bridge, reinterpret_cast<void *>(create)), "system WARP create actually enters bridge");
        for (unsigned round = 0; round < 2; ++round)
        {
            ID3D11Device *device = nullptr;
            ID3D11DeviceContext *context = nullptr;
            const HRESULT created = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &device, nullptr, &context);
            expect(SUCCEEDED(created) && device && context, "real WARP device and context returned");
            if (FAILED(created) || !device || !context)
                return 8;
            expect(belongs_to(bridge, (*reinterpret_cast<void ***>(device))[3]) &&
                belongs_to(bridge, (*reinterpret_cast<void ***>(context))[47]), "real device/context methods enter bridge");
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
                expect(belongs_to(bridge, (*reinterpret_cast<void ***>(chain3))[13]),
                    "SwapChain3 actually hooked by production DLL");
                expect(chain3->ResizeBuffers1(2, 640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                    nullptr, nullptr) == expected, "SwapChain3 final slot39 survives full-table clone");
                if (hdr_swapchain)
                {
                    IDXGISwapChain4 *chain4 = nullptr;
                    expect(SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain4),
                        reinterpret_cast<void **>(&chain4))) && chain4, "guarded SwapChain4 returned");
                    if (!chain4)
                        return 11;
                    void **methods = *reinterpret_cast<void ***>(chain4);
                    expect(!belongs_to(bridge, methods[39]) && belongs_to(bridge, methods[40]),
                        "SwapChain4 hooks HDR slot40 and preserves ResizeBuffers1 slot39");
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
    std::printf("production DLL routing: %d failures\n", failures);
    std::fflush(stdout);
    ExitProcess(failures ? 1 : 0);
}
