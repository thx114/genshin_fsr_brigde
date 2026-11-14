#pragma once

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
template <typename Function, Function Observe, std::size_t Method, bool SkipNested = false>
class ComMethodRoutes;

template <typename Result, typename Object, typename... Args,
    Result(STDMETHODCALLTYPE *Observe)(Object *, Args...), std::size_t Method, bool SkipNested>
class ComMethodRoutes<Result(STDMETHODCALLTYPE *)(Object *, Args...), Observe, Method, SkipNested>
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

    static Result STDMETHODCALLTYPE invoke(Object *object, Args... args)
    {
        Frame frame { active_, Routes::original(), object };
        active_ = &frame;
        struct Restore
        {
            Frame *previous;
            ~Restore() { active_ = previous; }
        } restore { frame.previous };
        // 共享 runtime 表会被后续新设备继承；包装器构造尚未结束时只转发。
        if (D3D11CreationScope::active())
            return frame.original(object, args...);
        if constexpr (std::is_same_v<Object, ID3D11DeviceContext>)
            if (context_policy_bypass != 0)
                return frame.original(object, args...);
        // Draw/Dispatch/Present 的业务接管只发生一次；资源与 shader 返回观察独立保留。
        if constexpr (SkipNested)
            if (frame.previous != nullptr)
                return frame.original(object, args...);
        return Observe(object, args...);
    }

    using Routes = CreateFunctionRoutes<Function, &invoke>;

public:
    static void *bind(void *target)
    {
        const auto entry = Routes::bind(reinterpret_cast<Function>(target));
        if (entry != nullptr)
            installed_.store(true, std::memory_order_release);
        return reinterpret_cast<void *>(entry);
    }

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

    bool operator==(std::nullptr_t) const { return !installed_.load(std::memory_order_acquire); }
    bool operator!=(std::nullptr_t) const { return installed_.load(std::memory_order_acquire); }
};
