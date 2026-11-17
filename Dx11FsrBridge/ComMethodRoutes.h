#pragma once

#include "ComVtableMethods.h"
#include "CreateFunctionRoutes.h"
#include "D3D11CreationScope.h"
#include <d3d11.h>
#include <type_traits>

inline thread_local unsigned context_policy_bypass = 0;

class ContextPolicyBypass
{
public:
    explicit ContextPolicyBypass(ID3D11DeviceContext *) { ++context_policy_bypass; }
    ~ContextPolicyBypass() { --context_policy_bypass; }
    ContextPolicyBypass(const ContextPolicyBypass &) = delete;
    ContextPolicyBypass &operator=(const ContextPolicyBypass &) = delete;
};

// 2026-11-14：生产 DLL 的成功设备回归证实，全局 original 会把 proxy this 传入
// native 实现。正常回调只取匹配的 TLS 帧；内部跨对象调用才解析对象当前的方法槽。
template <typename Function, Function Observe, std::size_t Method, bool SkipNested = false,
    auto AfterInvoke = nullptr>
class ComMethodRoutes;

template <typename Result, typename Object, typename... Args,
    Result(STDMETHODCALLTYPE *Observe)(Object *, Args...), std::size_t Method, bool SkipNested, auto AfterInvoke>
class ComMethodRoutes<Result(STDMETHODCALLTYPE *)(Object *, Args...), Observe, Method, SkipNested, AfterInvoke>
{
    using Function = Result(STDMETHODCALLTYPE *)(Object *, Args...);
    struct Frame
    {
        Frame *previous;
        Function original;
        Object *object;
    };
    inline static thread_local Frame *active_ = nullptr;
    inline static std::atomic<bool> installed_ {};

    static Result invoke(Function original, Object *object, Args... args)
    {
        Frame frame { active_, original, object };
        active_ = &frame;
        struct Restore
        {
            Frame *previous;
            ~Restore() { active_ = previous; }
        } restore { frame.previous };
        // 共享 runtime 表会被后续新设备继承；包装器构造尚未结束时只转发。
        if (D3D11CreationScope::active())
            return frame.original(object, args...);
        // 2026-11-16：真实 WARP 的 Flush/Draw 会改写内嵌方法表；返回策略也覆盖
        // 透传和内部调用，但不能在尚未完成的包装器构造期间查询、重装对象。
        struct Finish
        {
            Object *object;
            ~Finish()
            {
                if constexpr (AfterInvoke != nullptr)
                    AfterInvoke(object);
            }
        } finish { object };
        if constexpr (std::is_same_v<Object, ID3D11DeviceContext>)
            if (context_policy_bypass != 0)
                return frame.original(object, args...);
        // Draw/Dispatch/Present 的业务接管只发生一次；资源与 shader 返回观察独立保留。
        if constexpr (SkipNested)
            if (frame.previous != nullptr)
                return frame.original(object, args...);
        return Observe(object, args...);
    }

    using Routes = CreateFunctionRoutes<Function, nullptr, 16, &invoke>;

public:
    static void *bind(void *target)
    {
        const auto entry = Routes::bind(reinterpret_cast<Function>(target));
        if (entry != nullptr)
            installed_.store(true, std::memory_order_release);
        return reinterpret_cast<void *>(entry);
    }

    static constexpr com_vtable::MethodPatch slot() { return { Method, &bind }; }

    static Function original_for(Object *object)
    {
        for (auto *frame = active_; frame != nullptr; frame = frame->previous)
            if (frame->object == object)
                return frame->original;
        if (object == nullptr)
            return nullptr;
        auto **table = *reinterpret_cast<void ***>(object);
        return Routes::target(reinterpret_cast<Function>(table[Method]));
    }

    Result operator()(Object *object, Args... args) const
    {
        const Function original = original_for(object);
        if (original != nullptr)
            return original(object, args...);
        if constexpr (!std::is_void_v<Result>)
            return static_cast<Result>(E_UNEXPECTED);
    }

    static bool bound_on(Object *object)
    {
        if (!object) return false;
        auto **table = *reinterpret_cast<void ***>(object);
        if (!table) return false;
        const auto entry = reinterpret_cast<Function>(std::atomic_ref<void *>(table[Method]).load(
            std::memory_order_acquire));
        return entry && Routes::owns(entry);
    }

    bool operator==(std::nullptr_t) const { return !installed_.load(std::memory_order_acquire); }
    bool operator!=(std::nullptr_t) const { return installed_.load(std::memory_order_acquire); }
};

// 槽索引、原函数类型和观察器绑定在同一个实例上，省去运行时按回调地址反查。
template <typename Route>
constexpr com_vtable::MethodPatch route_slot(const Route &)
{
    return Route::slot();
}
