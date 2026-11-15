#include "../game/VersionDiscovery.h"

#include <Windows.h>
#include <cstdio>
#include <limits>

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2)
    {
        std::fputs("Usage: FsrVersionCheck <game executable>\n", stderr);
        return 2;
    }
    const HANDLE file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        std::fprintf(stderr, "Cannot open input: %lu\n", GetLastError());
        return 2;
    }
    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > std::numeric_limits<std::size_t>::max())
    {
        CloseHandle(file);
        return 2;
    }
    // 数据映射保留磁盘偏移和首选基址；不装载游戏，不调用其代码。
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const auto *bytes = mapping ? static_cast<const std::uint8_t *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0))
        : nullptr;
    if (!bytes)
        std::fprintf(stderr, "Cannot map input: %lu\n", GetLastError());
    CloseHandle(file);
    if (mapping) CloseHandle(mapping);
    if (!bytes) return 2;
    const auto version = neo::game::inspect_stored_version({bytes, static_cast<std::size_t>(size.QuadPart)});
    const auto menu = neo::game::inspect_stored_menu({bytes, static_cast<std::size_t>(size.QuadPart)});
    UnmapViewOfFile(bytes);
    std::printf("layout=%s exact_gaps=%u/%zu render=%08X update=%08X camera=%08X render_head=%d camera_head=%d\n",
        neo::game::discovery_status(version.status), version.exact_gaps, neo::game::k_fsr2_layout.gaps.size(),
        version.render, version.update, version.camera, version.render_compatible, version.camera_compatible);
    std::printf("menu_ready=%d build=%08X/%u text=%08X/%u update=%08X/%u\n", menu.ready(),
        menu.build.rva, menu.build.matches, menu.text.rva, menu.text.matches, menu.update.rva, menu.update.matches);
    return version.render_compatible && version.camera_compatible && menu.ready() ? 0 : 1;
}
