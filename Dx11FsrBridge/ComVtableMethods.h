#pragma once

#include <Windows.h>
#include <cstddef>
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
        DWORD protection = 0;
        bool writable = false;
        bool published = false;
    };
    std::vector<Entry> entries;
    entries.reserve(patches.size());
    for (const auto &[index, bind] : patches)
    {
        if (index >= public_methods)
            return false;
        void *const observed = table[index];
        if (!observed)
            return false;
        for (const auto &entry : entries)
            if (entry.slot == table + index)
                return false;
        void *replacement = bind(observed);
        if (!replacement)
            return false;
        entries.push_back({ table + index, observed, replacement });
    }

    // 修改前先取得每个页面的写权限；逆序恢复同页重叠保护，失败不留下半张补丁表。
    struct Restore
    {
        std::vector<Entry> &entries;
        bool committed = false;
        ~Restore()
        {
            for (auto it = entries.rbegin(); it != entries.rend(); ++it)
            {
                if (!committed && it->published)
                    InterlockedCompareExchangePointer(it->slot, it->original, it->replacement);
                if (it->writable)
                {
                    DWORD ignored = 0;
                    if (!VirtualProtect(it->slot, sizeof(void *), it->protection, &ignored))
                        OutputDebugStringA("FSR bridge: cannot restore COM table protection\n");
                }
            }
        }
    } restore { entries };
    for (auto &entry : entries)
    {
        if (entry.original == entry.replacement)
            continue;
        if (!VirtualProtect(entry.slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &entry.protection))
            return false;
        entry.writable = true;
    }
    for (auto &entry : entries)
    {
        if (entry.original == entry.replacement)
            continue;
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
