#include "ComMethodRoutes.h"
#include "ComVtableMethods.h"
#include <cstdio>

namespace
{
struct Context
{
    void **table;
};
using Draw = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT);
void STDMETHODCALLTYPE observe(ID3D11DeviceContext *, UINT, UINT);
using Routes = ComMethodRoutes<Draw, &observe, 13, true>;
Routes original;
void *native_table[115] {};
void *proxy_table[115] {};
Context native { native_table };
Context proxy { proxy_table };
unsigned before = 0, after = 0, gpu_draws = 0, takeovers = 0, observed_shader = 0, shader = 0;
unsigned failures = 0;

void STDMETHODCALLTYPE native_draw(ID3D11DeviceContext *object, UINT, UINT)
{
    if (object != reinterpret_cast<ID3D11DeviceContext *>(&native))
        ++failures;
    ++gpu_draws;
}

void STDMETHODCALLTYPE proxy_draw(ID3D11DeviceContext *, UINT count, UINT start)
{
    ++before;
    shader = 42; // 模组的 BeforeDraw 替换输入。
    reinterpret_cast<Draw>(native.table[13])(
        reinterpret_cast<ID3D11DeviceContext *>(&native), count, start);
    ++after;
}

void STDMETHODCALLTYPE observe(ID3D11DeviceContext *object, UINT count, UINT start)
{
    observed_shader = shader;
    if (count == 3)
    {
        ++takeovers;
        return; // 成功 SR 接管吞掉原绘制，仍必须完成代理前后处理。
    }
    original(object, count, start);
}

void expect(bool condition, const char *what)
{
    if (!condition)
    {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

void **racing_table = nullptr;
template <std::uintptr_t Replacement>
void *bind_patch(void *original)
{
    if constexpr (Replacement == 22)
        if (racing_table)
            racing_table[1] = reinterpret_cast<void *>(99);
    return original ? reinterpret_cast<void *>(Replacement) : nullptr;
}

void test_vtable_slots()
{
    auto **table = static_cast<void **>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    table[0] = reinterpret_cast<void *>(1);
    table[1] = reinterpret_cast<void *>(2);
    table[69] = reinterpret_cast<void *>(70); // runtime 私有转发目标，不属于公开复制范围。
    Context instance { table };
    DWORD old = 0;
    VirtualProtect(table, 4096, PAGE_READONLY, &old);
    expect(com_vtable::patch(&instance, 69, {{0, &bind_patch<11>}}),
        "read-only shared vtable accepts a public method patch");
    expect(instance.table == table && table[69] == reinterpret_cast<void *>(70),
        "original vptr and runtime private dispatch remain intact");
    expect(com_vtable::patch(&instance, 69, {{1, &bind_patch<22>}}) &&
        table[0] == reinterpret_cast<void *>(11) && table[1] == reinterpret_cast<void *>(22),
        "later patches merge rather than hitting a stale object cache");
    MEMORY_BASIC_INFORMATION info {};
    VirtualQuery(table, &info, sizeof(info));
    expect(info.Protect == PAGE_READONLY, "read-only page protection restored");
    expect(!com_vtable::patch(&instance, 69, {{69, &bind_patch<71>}}) &&
        table[69] == reinterpret_cast<void *>(70), "public bounds never patch a private slot");
    VirtualProtect(table, 4096, PAGE_READWRITE, &old);
    table[0] = reinterpret_cast<void *>(1);
    table[1] = reinterpret_cast<void *>(2);
    racing_table = table;
    expect(!com_vtable::patch(&instance, 2,
        {{0, &bind_patch<11>}, {1, &bind_patch<22>}}),
        "another injector changing a prepared slot rejects publication");
    racing_table = nullptr;
    expect(table[0] == reinterpret_cast<void *>(1) && table[1] == reinterpret_cast<void *>(99),
        "rollback restores only owned slots and preserves the other injector");
    VirtualFree(table, 0, MEM_RELEASE);

    SYSTEM_INFO system {};
    GetSystemInfo(&system);
    const SIZE_T page = system.dwPageSize;
    auto **pages = static_cast<void **>(VirtualAlloc(nullptr, page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!pages) { expect(false, "allocate two pages"); return; }
    const SIZE_T next = page / sizeof(void *);
    for (SIZE_T i = 0; i <= next; ++i) pages[i] = reinterpret_cast<void *>(1);
    instance.table = pages;
    VirtualProtect(pages, page, PAGE_READONLY, &old);
    VirtualProtect(pages + next, page, PAGE_EXECUTE_READ, &old);
    std::vector<com_vtable::MethodPatch> many { { next, &bind_patch<22> } };
    for (SIZE_T i = 0; i < 40; ++i) many.push_back({ i, &bind_patch<11> });
    expect(com_vtable::patch(&instance, next + 1, many), "overflow storage and separated pages");
    VirtualQuery(pages, &info, sizeof(info));
    expect(info.Protect == PAGE_READONLY && pages[39] == reinterpret_cast<void *>(11), "first page restored");
    VirtualQuery(pages + next, &info, sizeof(info));
    expect(info.Protect == PAGE_EXECUTE_READ && pages[next] == reinterpret_cast<void *>(22), "second page restored");
    VirtualFree(pages, 0, MEM_RELEASE);
}

void transition_complete(ID3D11DeviceContext *object);
void STDMETHODCALLTYPE transition_observe(ID3D11DeviceContext *object, UINT count, UINT start);
using TransitionRoutes = ComMethodRoutes<Draw, &transition_observe, 13, true, &transition_complete>;
TransitionRoutes transition_original;
void *transition_table[116] {};
Context transition_context { transition_table };
unsigned transition_calls = 0, completion_calls = 0, transition_takeovers = 0;

void STDMETHODCALLTYPE transition_native(ID3D11DeviceContext *object, UINT, UINT)
{
    expect(object == reinterpret_cast<ID3D11DeviceContext *>(&transition_context),
        "transition preserves the native receiver");
    ++transition_calls;
    transition_table[13] = reinterpret_cast<void *>(&transition_native);
}

void transition_complete(ID3D11DeviceContext *object)
{
    ++completion_calls;
    if (!TransitionRoutes::bound_on(object))
        expect(com_vtable::patch(object, 115, { TransitionRoutes::slot() }),
            "completion repairs the runtime-written public slot");
}

void STDMETHODCALLTYPE transition_observe(ID3D11DeviceContext *object, UINT count, UINT start)
{
    if (count == 3)
    {
        ++transition_takeovers;
        return;
    }
    transition_original(object, count, start);
}

void test_completion()
{
    // 2026-11-16：重现运行时原位还原入口，确认透传/业务早退/构造旁路各自的完成语义。
    auto *object = reinterpret_cast<ID3D11DeviceContext *>(&transition_context);
    transition_table[13] = TransitionRoutes::bind(reinterpret_cast<void *>(&transition_native));
    transition_table[115] = reinterpret_cast<void *>(116);
    object->Draw(0, 0);
    expect(transition_calls == 1 && completion_calls == 1 && TransitionRoutes::bound_on(object),
        "completion restores a slot before returning to the caller");
    {
        ContextPolicyBypass bypass(object);
        object->Draw(3, 0);
    }
    expect(transition_calls == 2 && completion_calls == 2 && transition_takeovers == 0 &&
        TransitionRoutes::bound_on(object), "internal forwarding still completes route maintenance");
    {
        D3D11CreationScope creation;
        object->Draw(3, 0);
    }
    expect(transition_calls == 3 && completion_calls == 2 && !TransitionRoutes::bound_on(object),
        "unfinished construction forwards without querying or repairing the object");
    expect(com_vtable::patch(object, 115, { TransitionRoutes::slot() }), "completed object can rebind");
    object->Draw(3, 0);
    expect(transition_calls == 3 && completion_calls == 3 && transition_takeovers == 1,
        "business takeover completes once without issuing the original Draw");
    expect(transition_context.table == transition_table && transition_table[115] == reinterpret_cast<void *>(116),
        "completion retains the private tail and the original vptr");
}
}

int main()
{
    native_table[13] = Routes::bind(reinterpret_cast<void *>(&native_draw));
    proxy_table[13] = reinterpret_cast<void *>(&proxy_draw);
    expect(Routes::bound_on(reinterpret_cast<ID3D11DeviceContext *>(&native)) &&
        !Routes::bound_on(reinterpret_cast<ID3D11DeviceContext *>(&proxy)),
        "COM ownership distinguishes a routed native from an unbound forwarding proxy");
    const auto draw = reinterpret_cast<Draw>(proxy_table[13]);
    auto *context = reinterpret_cast<ID3D11DeviceContext *>(&proxy);
    draw(context, 3, 0);
    expect(before == 1 && after == 1 && takeovers == 1 && gpu_draws == 0 && observed_shader == 42,
        "native takeover observes mod changes and retains BeforeDraw/AfterDraw");
    draw(context, 5, 0);
    expect(before == 2 && after == 2 && gpu_draws == 1, "non-takeover preserves native execution");
    {
        ContextPolicyBypass first(context);
        {
            ContextPolicyBypass second(context);
            draw(context, 3, 0);
        }
        expect(context_policy_bypass == 1, "nested bypass restores previous domain");
        draw(context, 3, 0);
    }
    expect(context_policy_bypass == 0 && takeovers == 1 && gpu_draws == 3,
        "internal bypass crosses proxy without executing bridge policy");
    draw(context, 3, 0);
    expect(before == 5 && after == 5 && takeovers == 2 && gpu_draws == 3,
        "takeover resumes after internal work");
    expect(Routes::original_for(reinterpret_cast<ID3D11DeviceContext *>(&native)) == &native_draw,
        "internal direct call resolves the object's own original");
    {
        D3D11CreationScope creation;
        draw(context, 3, 0);
        expect(takeovers == 2 && gpu_draws == 4,
            "later device construction bypasses already shared runtime policy");
        {
            D3D11CreationScope nested;
            draw(context, 3, 0);
        }
        expect(D3D11CreationScope::active() && takeovers == 2 && gpu_draws == 5,
            "nested creation preserves bypass until the outer wrapper finishes");
    }
    draw(context, 3, 0);
    expect(!D3D11CreationScope::active() && takeovers == 3 && gpu_draws == 5,
        "bridge takeover resumes after the completed creation boundary");
    test_vtable_slots();
    test_completion();
    std::printf("ComMethodRoutes takeover/bypass: %u failures\n", failures);
    return failures ? 1 : 0;
}
