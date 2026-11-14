#include "ComMethodRoutes.h"
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
}

int main()
{
    native_table[13] = Routes::bind(reinterpret_cast<void *>(&native_draw));
    proxy_table[13] = reinterpret_cast<void *>(&proxy_draw);
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
    std::printf("ComMethodRoutes takeover/bypass: %u failures\n", failures);
    return failures ? 1 : 0;
}
