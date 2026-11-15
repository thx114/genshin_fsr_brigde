#pragma once

#include <Windows.h>
#include <cstddef>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <span>
#include <vector>

namespace com_vtable
{
struct MethodPatch
{
    std::size_t index;
    void *(*bind)(void *target);
};

// 2026-11-15：真实 D3D11 GetFeatureLevel 还通过私有槽69转发。公开接口长度
// 只能约束补丁索引，不能决定运行时虚表的完整长度；保留原表也保留其私有调用。
inline bool patch(void *instance, std::size_t public_methods,
    std::span<const MethodPatch> patches)
{
    if (!instance || patches.empty())
        return false;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    void **table = *reinterpret_cast<void ***>(instance);
    if (!table)
        return false;

    struct Entry
    {
        void **slot;
        void *original;
        void *replacement;
        DWORD protection = 0; // 非零：本槽负责恢复所在页。
        bool published = false;
    };
    // 2026-11-16：归档至少八次 Present 重装；常用表不超过 32 槽，大表仍支持。
    constexpr std::size_t k_inline_entries = 32;
    std::array<Entry, k_inline_entries> local;
    std::vector<Entry> overflow;
    if (patches.size() > local.size()) overflow.resize(patches.size());
    std::span<Entry> entries(overflow.empty() ? local.data() : overflow.data(), patches.size());
    std::size_t used = 0;
    for (const auto &[index, bind] : patches)
    {
        if (index >= public_methods) return false;
        void *const observed = table[index];
        if (!observed) return false;
        for (const auto &entry : entries.first(used))
            if (entry.slot == table + index) return false;
        void *replacement = bind(observed);
        if (!replacement) return false;
        entries[used++] = { table + index, observed, replacement };
    }

    struct Restore
    {
        std::span<Entry> entries;
        bool committed = false;
        ~Restore()
        {
            // 所有页面仍可写时先撤回本次成功的槽；绝不把别人的并发替换覆盖回去。
            if (!committed)
                for (auto it = entries.rbegin(); it != entries.rend(); ++it)
                    if (it->published) InterlockedCompareExchangePointer(it->slot, it->original, it->replacement);
            for (auto it = entries.rbegin(); it != entries.rend(); ++it)
            {
                if (!it->protection) continue;
                DWORD ignored = 0;
                if (!VirtualProtect(it->slot, sizeof(void *), it->protection, &ignored))
                    OutputDebugStringA("FSR bridge: cannot restore COM table protection\n");
            }
        }
    } restore { entries };
    static const std::uintptr_t page_mask = [] {
        SYSTEM_INFO info {};
        GetSystemInfo(&info);
        return ~(static_cast<std::uintptr_t>(info.dwPageSize) - 1);
    }();
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        auto &entry = entries[i];
        if (entry.original == entry.replacement) continue;
        const auto page = reinterpret_cast<std::uintptr_t>(entry.slot) & page_mask;
        bool writable = false;
        for (const auto &prior : entries.first(i))
            if (prior.protection && (reinterpret_cast<std::uintptr_t>(prior.slot) & page_mask) == page)
            {
                writable = true;
                break;
            }
        if (!writable)
        {
            DWORD old = 0;
            if (!VirtualProtect(entry.slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return false;
            entry.protection = old;
        }
    }
    for (auto &entry : entries)
    {
        if (entry.original == entry.replacement) continue;
        if (InterlockedCompareExchangePointer(entry.slot, entry.replacement, entry.original) != entry.original)
            return false;
        entry.published = true;
    }
    restore.committed = true;
    return true;
}

inline bool patch(void *instance, std::size_t public_methods, std::initializer_list<MethodPatch> patches)
{
    return patch(instance, public_methods, std::span<const MethodPatch>(patches.begin(), patches.size()));
}
}
