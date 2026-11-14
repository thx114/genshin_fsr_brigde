#pragma once

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>

// 2026-11-14：Rocket 与系统 D3D11 可以同名共存；实测单个 original 会把第二个
// provider 的调用转给第一个。按真实 callable 绑定入口，嵌套时只恢复当前线程上下文。
template <typename Function, Function Observe, std::size_t Capacity = 16>
class CreateFunctionRoutes;

template <typename Result, typename... Args, Result(WINAPI *Observe)(Args...), std::size_t Capacity>
class CreateFunctionRoutes<Result(WINAPI *)(Args...), Observe, Capacity>
{
    using Function = Result(WINAPI *)(Args...);
    inline static std::array<std::atomic<Function>, Capacity> originals_ {};
    inline static std::mutex install_mutex_;
    inline static thread_local Function current_ = nullptr;

    struct CallScope
    {
        Function previous = current_;

        explicit CallScope(Function original)
        {
            current_ = original;
        }

        ~CallScope()
        {
            current_ = previous;
        }
    };

    template <std::size_t Index>
    static Result WINAPI invoke(Args... args)
    {
        CallScope scope(originals_[Index].load(std::memory_order_acquire));
        return Observe(args...);
    }

    template <std::size_t... Indices>
    static constexpr auto make_entries(std::index_sequence<Indices...>)
    {
        return std::array<Function, Capacity> { &invoke<Indices>... };
    }

    inline static constexpr auto entries_ = make_entries(std::make_index_sequence<Capacity> {});

public:
    static Function original()
    {
        return current_;
    }

    static Function target(Function entry)
    {
        for (std::size_t i = 0; i < Capacity; ++i)
            if (entry == entries_[i])
                return originals_[i].load(std::memory_order_acquire);
        return entry;
    }

    static Function bind(Function target, bool *added = nullptr)
    {
        if (added != nullptr)
            *added = false;
        if (target == nullptr)
            return nullptr;
        // IAT 重扫可能看到已安装入口；不能再把自己的入口当作新的 original。
        for (Function entry : entries_)
            if (target == entry)
                return entry;

        std::lock_guard lock(install_mutex_);
        for (std::size_t i = 0; i < Capacity; ++i)
            if (originals_[i].load(std::memory_order_relaxed) == target)
                return entries_[i];
        for (std::size_t i = 0; i < Capacity; ++i)
        {
            if (originals_[i].load(std::memory_order_relaxed) == nullptr)
            {
                originals_[i].store(target, std::memory_order_release);
                if (added != nullptr)
                    *added = true;
                return entries_[i];
            }
        }
        // 无可用静态入口时保留原调用，不能退回其它 provider 的 original。
        return nullptr;
    }
};
