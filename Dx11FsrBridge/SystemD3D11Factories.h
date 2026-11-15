#pragma once

#include <Windows.h>
#include <TlHelp32.h>
#include <d3d11.h>
#include <detours/detours.h>
#include <atomic>
#include <mutex>
#include <new>
#include <vector>

// 2026-11-15：缓存的系统导出地址绕过 IAT 替换；只补两个创建入口，
// 沿用既有 provider 路由，不在逐 Draw／Dispatch 路径安装代码钩子。
class SystemD3D11Factories
{
    struct Threads
    {
        std::vector<HANDLE> handles;
        ~Threads() { for (HANDLE handle : handles) CloseHandle(handle); }
        LONG enlist()
        {
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return static_cast<LONG>(GetLastError());
            struct Snapshot { HANDLE handle; ~Snapshot() { CloseHandle(handle); } } guard { snapshot };
            THREADENTRY32 entry {};
            entry.dwSize = sizeof(entry);
            if (!Thread32First(snapshot, &entry))
                return static_cast<LONG>(GetLastError());
            const DWORD process = GetCurrentProcessId(), current = GetCurrentThreadId();
            do
            {
                if (entry.th32OwnerProcessID != process || entry.th32ThreadID == current)
                    continue;
                const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                    THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                if (!thread)
                {
                    const DWORD error = GetLastError();
                    if (error == ERROR_INVALID_PARAMETER) continue; // 枚举后线程已退出
                    return static_cast<LONG>(error);
                }
                try { handles.push_back(thread); }
                catch (const std::bad_alloc &)
                {
                    CloseHandle(thread);
                    return ERROR_NOT_ENOUGH_MEMORY;
                }
            } while (Thread32Next(snapshot, &entry));
            if (GetLastError() != ERROR_NO_MORE_FILES)
                return static_cast<LONG>(GetLastError());
            // 先完成枚举和分配，再挂起调用线程；Commit／Abort 恢复线程前
            // 句柄必须存活，否则 Detours 无法安全调整正在执行的入口地址。
            for (HANDLE thread : handles)
            {
                const LONG result = DetourUpdateThread(thread);
                if (result != NO_ERROR) return result;
            }
            return NO_ERROR;
        }
    };
public:
    using Device = decltype(&D3D11CreateDevice);
    using Combined = decltype(&D3D11CreateDeviceAndSwapChain);
    struct Receipt
    {
        void *entry = nullptr;
        void *target = nullptr;
        PDETOUR_TRAMPOLINE trampoline = nullptr;
    };
    Device device = nullptr;
    Combined combined = nullptr;
    Receipt device_receipt, combined_receipt;

    LONG install(Device native_device, Combined native_combined, Device hook_device,
        Combined hook_combined, bool &attempted)
    {
        attempted = false;
        if (installed_.load(std::memory_order_acquire) || closing_.load(std::memory_order_acquire))
            return NO_ERROR;
        if (!native_device || !native_combined)
            return ERROR_PROC_NOT_FOUND;
        std::lock_guard lock(mutex_);
        if (installed_.load(std::memory_order_relaxed) || closing_.load(std::memory_order_relaxed))
            return NO_ERROR;
        attempted = true;
        // 桥还发布 IAT/vtable 回调，生命周期须覆盖整个进程；
        // 固定回调模块，避免显式卸载或摘钩失败后跳转到已解除映射的地址。
        HMODULE owner = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(hook_device), &owner))
            return static_cast<LONG>(GetLastError());
        Threads threads;
        LONG result = DetourTransactionBegin();
        if (result != NO_ERROR)
        {
            if (result != ERROR_INVALID_OPERATION) DetourTransactionAbort();
            return result;
        }
        device = native_device;
        combined = native_combined;
        device_receipt = { reinterpret_cast<void *>(native_device) };
        combined_receipt = { reinterpret_cast<void *>(native_combined) };
        // trampoline 准备可能分配内存，先准备两个入口再登记调用者，缩短暂停窗口。
        result = DetourAttachEx(reinterpret_cast<PVOID *>(&device), reinterpret_cast<PVOID>(hook_device),
            &device_receipt.trampoline, &device_receipt.target, nullptr);
        if (result == NO_ERROR)
            result = DetourAttachEx(reinterpret_cast<PVOID *>(&combined), reinterpret_cast<PVOID>(hook_combined),
                &combined_receipt.trampoline, &combined_receipt.target, nullptr);
        if (result == NO_ERROR)
            result = threads.enlist();
        if (result == NO_ERROR)
            result = DetourTransactionCommit();
        else
            DetourTransactionAbort();
        if (result == NO_ERROR)
            installed_.store(true, std::memory_order_release);
        else
        {
            device = nullptr;
            combined = nullptr;
        }
        return result;
    }

    // 仅供已停止并 join 所有创建调用者的宿主使用；生产模块固定至进程退出，
    // 不在持 loader lock 的 DllMain 中尝试等待调用者或摘钩。
    LONG detach(Device hook_device, Combined hook_combined)
    {
        closing_.store(true, std::memory_order_release);
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return ERROR_BUSY;
        if (!installed_.load(std::memory_order_acquire))
            return NO_ERROR;
        Threads threads;
        LONG result = DetourTransactionBegin();
        if (result != NO_ERROR)
        {
            if (result != ERROR_INVALID_OPERATION) DetourTransactionAbort();
            return result;
        }
        result = DetourDetach(reinterpret_cast<PVOID *>(&combined), reinterpret_cast<PVOID>(hook_combined));
        if (result == NO_ERROR)
            result = DetourDetach(reinterpret_cast<PVOID *>(&device), reinterpret_cast<PVOID>(hook_device));
        if (result == NO_ERROR)
            result = threads.enlist();
        if (result == NO_ERROR)
            result = DetourTransactionCommit();
        else
            DetourTransactionAbort();
        if (result == NO_ERROR)
            installed_.store(false, std::memory_order_release);
        return result;
    }

private:
    std::mutex mutex_;
    std::atomic<bool> installed_ {}, closing_ {};
};
