#pragma once
#include <windows.h>
#include <cstring>
namespace neo::tests
{
// 仅用于隔离测试进程的已加载 PE，不修改桌面键盘或其他进程。
inline bool replace_import(HMODULE module, const char *library, const char *name, FARPROC target)
{
    auto *base = reinterpret_cast<unsigned char *>(module);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; descriptor->Name; ++descriptor)
    {
        if (_stricmp(reinterpret_cast<char *>(base + descriptor->Name), library)) continue;
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->OriginalFirstThunk);
        auto *slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto *symbol = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char *>(symbol->Name), name)) continue;
            DWORD previous = 0, ignored = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &previous)) return false;
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(target);
            return VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), previous, &ignored) &&
                slots->u1.Function == reinterpret_cast<ULONG_PTR>(target);
        }
    }
    return false;
}
}
