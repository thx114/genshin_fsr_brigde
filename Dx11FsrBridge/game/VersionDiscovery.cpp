#include "VersionDiscovery.h"
#include "AnchorPatterns.h"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace neo::game
{
namespace
{
using Bytes = std::span<const std::uint8_t>;
constexpr std::size_t k_section_limit = 96; // PE/COFF 映像允许的段数上限。

int image_read_error(unsigned code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

bool copy_image_bytes(void *output, const void *source, std::size_t length)
{
    __try
    {
        std::memcpy(output, source, length);
        return true;
    }
    __except (image_read_error(GetExceptionCode()))
    {
        return false;
    }
}

template <typename T>
bool read_at(Bytes bytes, std::size_t offset, T &out)
{
    return offset <= bytes.size() && sizeof(T) <= bytes.size() - offset &&
        copy_image_bytes(&out, bytes.data() + offset, sizeof(T));
}

struct Image
{
    Bytes bytes;
    std::array<IMAGE_SECTION_HEADER, k_section_limit> sections {};
    std::size_t count = 0;
    std::uint32_t image_size = 0;
    ImageFingerprint fingerprint {};
    std::uintptr_t pointer_base = 0;
    bool stored = false;

    Bytes section_bytes(const IMAGE_SECTION_HEADER &section) const
    {
        if (stored)
            return bytes.subspan(section.PointerToRawData, std::min(section.Misc.VirtualSize, section.SizeOfRawData));
        return bytes.subspan(section.VirtualAddress, section.Misc.VirtualSize);
    }
};

bool parse_image(Bytes mapped, Image &out, bool stored = false)
{
    IMAGE_DOS_HEADER dos {};
    IMAGE_NT_HEADERS64 nt {};
    if (!read_at(mapped, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos))
        return false;
    const auto nt_offset = static_cast<std::size_t>(dos.e_lfanew);
    if (!read_at(mapped, nt_offset, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.FileHeader.NumberOfSections == 0 || nt.FileHeader.NumberOfSections > out.sections.size() ||
        (!stored && nt.OptionalHeader.SizeOfImage > mapped.size()) ||
        nt.OptionalHeader.SizeOfHeaders > mapped.size() || nt.OptionalHeader.SizeOfHeaders > nt.OptionalHeader.SizeOfImage)
        return false;

    const auto table = nt_offset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    const auto count = nt.FileHeader.NumberOfSections;
    const auto header_size = nt.OptionalHeader.SizeOfHeaders;
    if (table > header_size || count * sizeof(IMAGE_SECTION_HEADER) > header_size - table)
        return false;
    const Bytes image = stored ? mapped : mapped.first(nt.OptionalHeader.SizeOfImage);
    const auto image_size = nt.OptionalHeader.SizeOfImage;
    for (std::size_t i = 0; i < count; ++i)
    {
        auto &section = out.sections[i];
        if (!read_at(image, table + i * sizeof(section), section) || section.VirtualAddress > image_size ||
            section.Misc.VirtualSize > image_size - section.VirtualAddress ||
            (stored && (section.PointerToRawData > image.size() ||
                section.SizeOfRawData > image.size() - section.PointerToRawData)))
            return false;
    }
    out.bytes = image;
    out.count = count;
    out.image_size = image_size;
    out.fingerprint = { nt.FileHeader.TimeDateStamp, image_size, nt.OptionalHeader.CheckSum };
    out.stored = stored;
    out.pointer_base = stored ? nt.OptionalHeader.ImageBase : reinterpret_cast<std::uintptr_t>(image.data());
    return true;
}

Bytes mapped_module(std::uintptr_t module)
{
    MODULEINFO info {};
    if (module == 0 || !K32GetModuleInformation(GetCurrentProcess(), reinterpret_cast<HMODULE>(module),
        &info, sizeof(info)))
        return {};
    return { static_cast<const std::uint8_t *>(info.lpBaseOfDll), info.SizeOfImage };
}

bool is_executable(const IMAGE_SECTION_HEADER &section)
{
    return (section.Characteristics & (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE)) ==
        (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE) && section.Misc.VirtualSize != 0;
}

Bytes executable_range(const Image &image, std::uint32_t rva, std::size_t length)
{
    if (rva == 0 || length == 0)
        return {};
    for (std::size_t i = 0; i < image.count; ++i)
    {
        const auto &section = image.sections[i];
        if (!is_executable(section) || rva < section.VirtualAddress)
            continue;
        const auto data = image.section_bytes(section);
        const auto offset = rva - section.VirtualAddress;
        if (offset <= data.size() && length <= data.size() - offset)
            return data.subspan(offset, length);
    }
    return {};
}

bool code_equals(Bytes region, Bytes signature)
{
    __try
    {
        return std::memcmp(region.data(), signature.data(), signature.size()) == 0;
    }
    __except (image_read_error(GetExceptionCode()))
    {
        return false;
    }
}

bool readable_executable(const MEMORY_BASIC_INFORMATION &memory)
{
    // PAGE_EXECUTE 并不保证可读；不触发 guard 页以免改变游戏后续异常行为。
    return memory.State == MEM_COMMIT && !(memory.Protect & PAGE_GUARD) &&
        (memory.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool matches_code(const Image &image, std::uint32_t rva, Bytes expected)
{
    const auto region = executable_range(image, rva, expected.size());
    return !region.empty() && code_equals(region, expected);
}

bool validate_cached_anchor(const Image &image, std::uint32_t rva, Bytes signature)
{
    const auto code = executable_range(image, rva, signature.size());
    MEMORY_BASIC_INFORMATION memory {};
    if (code.empty() || VirtualQuery(code.data(), &memory, sizeof(memory)) != sizeof(memory) ||
        !readable_executable(memory))
        return false;
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    const auto begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    return address >= begin && address - begin <= memory.RegionSize &&
        code.size() <= memory.RegionSize - (address - begin) && code_equals(code, signature);
}

bool find_anchor(Bytes code, std::uint32_t base_rva, Bytes signature, CodeAnchor &found)
{
    __try
    {
        while (found.matches < 2 && code.size() >= signature.size())
        {
            const auto *head = static_cast<const std::uint8_t *>(
                std::memchr(code.data(), signature.front(), code.size() - signature.size() + 1));
            if (!head) break;
            const auto offset = static_cast<std::size_t>(head - code.data());
            if (std::memcmp(head, signature.data(), signature.size()) == 0)
            {
                ++found.matches;
                found.rva = found.matches == 1 ? base_rva + static_cast<std::uint32_t>(offset) : 0;
            }
            code = code.subspan(offset + 1);
            base_rva += static_cast<std::uint32_t>(offset + 1);
        }
        return true;
    }
    __except (image_read_error(GetExceptionCode()))
    {
        return false;
    }
}

MenuDiscovery discover_menu_in(const Image &image, MenuRvas cached)
{
    MenuDiscovery result;
    const bool use_required = !image.stored && validate_cached_anchor(image, cached.build, k_build_cmd_buffers_signature)
        && validate_cached_anchor(image, cached.text, k_get_text_signature);
    const bool use_update = use_required && validate_cached_anchor(image, cached.update, k_update_inner_target_signature);
    if (use_required)
    {
        result.build = {cached.build, 1};
        result.text = {cached.text, 1};
        result.cache_hit = true;
    }
    if (use_update)
    {
        result.update = {cached.update, 1};
        return result;
    }
    const auto scan = [&](Bytes code, std::uint32_t rva)
    {
        return (use_required || (find_anchor(code, rva, k_build_cmd_buffers_signature, result.build) &&
            find_anchor(code, rva, k_get_text_signature, result.text))) &&
            find_anchor(code, rva, k_update_inner_target_signature, result.update);
    };
    for (const auto &section : std::span(image.sections).first(image.count))
    {
        if (!is_executable(section)) continue;
        auto code = image.section_bytes(section);
        if (image.stored)
        {
            if (!scan(code, section.VirtualAddress)) return {};
            continue;
        }
        auto rva = section.VirtualAddress;
        while (!code.empty())
        {
            MEMORY_BASIC_INFORMATION memory {};
            if (VirtualQuery(code.data(), &memory, sizeof(memory)) != sizeof(memory)) return {};
            const auto offset = reinterpret_cast<std::uintptr_t>(code.data()) -
                reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
            if (offset >= memory.RegionSize) return {};
            const auto length = std::min(code.size(), memory.RegionSize - offset);
            if (readable_executable(memory) && !scan(code.first(length), rva)) return {};
            code = code.subspan(length);
            rva += static_cast<std::uint32_t>(length);
        }
    }
    return result;
}

bool collect_methods(Bytes data, std::uintptr_t image_base, std::uint32_t code_begin, std::uint64_t code_end,
    std::vector<std::uint32_t> &out)
{
    // SEH 仅保护外部映像读取；容器由调用者持有，失败时不会泄漏。
    __try
    {
        for (std::size_t i = 0; i < data.size() / sizeof(std::uint64_t); ++i)
        {
            std::uint64_t pointer {};
            std::memcpy(&pointer, data.data() + i * sizeof(pointer), sizeof(pointer));
            if (pointer < image_base)
                continue;
            const auto rva = pointer - image_base;
            if (rva >= code_begin && rva < code_end)
                out.push_back(static_cast<std::uint32_t>(rva));
        }
        return true;
    }
    __except (image_read_error(GetExceptionCode()))
    {
        return false;
    }
}

constexpr int tolerance(std::uint16_t gap)
{
    if (gap == 0xFA0) return 0x400;
    if (gap <= 0x20) return 0x10;
    if (gap <= 0x100) return 0x40;
    return 0x100;
}

constexpr bool gap_matches(std::int64_t drift, std::uint16_t expected)
{
    return drift >= -tolerance(expected) && drift <= tolerance(expected);
}

void select_layout(std::vector<std::uint32_t> &methods, VersionDiscovery &out)
{
    std::sort(methods.begin(), methods.end());
    methods.erase(std::unique(methods.begin(), methods.end()), methods.end());
    const auto &layout = k_fsr2_layout;
    const auto count = layout.gaps.size() + 1;
    if (methods.size() < count)
        return;

    std::size_t best = 0;
    bool tie = false;
    for (std::size_t i = 0; i <= methods.size() - count; ++i)
    {
        // 先校验 Update→Render，再比整族；不为每个短 getter 前缀重复遍历余下 29 项。
        const auto anchor = layout.update_index;
        const auto gap = static_cast<std::int64_t>(methods[i + layout.render_index]) - methods[i + anchor];
        if (!gap_matches(gap - layout.gaps[anchor], layout.gaps[anchor]))
            continue;
        unsigned exact = 0;
        std::int64_t total_drift = 0;
        std::size_t j = 0;
        for (; j < layout.gaps.size(); ++j)
        {
            const auto drift = static_cast<std::int64_t>(methods[i + j + 1]) - methods[i + j] - layout.gaps[j];
            if (!gap_matches(drift, layout.gaps[j]))
                break;
            exact += drift == 0;
            total_drift += drift;
        }
        if (j != layout.gaps.size() || total_drift < -0x1000 || total_drift > 0x1000 || exact < 20)
            continue;
        if (exact > out.exact_gaps)
        {
            best = i;
            out.exact_gaps = exact;
            tie = false;
        }
        else if (exact == out.exact_gaps)
            tie = true;
    }
    if (out.exact_gaps == 0)
        return;
    if (tie)
    {
        out.status = DiscoveryStatus::ambiguous;
        return;
    }
    out.status = DiscoveryStatus::matched;
    out.render = methods[best + layout.render_index];
    out.update = methods[best + layout.update_index];
    out.camera = methods[best + layout.camera_index];
}

bool find_pairs_in_section(Bytes code, std::uint32_t rva, Bytes render, Bytes update, std::uint32_t distance,
    std::vector<std::uint32_t> &output)
{
    __try
    {
        if (render.size() > code.size() || update.size() > distance)
            return true;
        for (std::size_t i = distance; i <= code.size() - render.size() && output.size() < 8; ++i)
            if (std::memcmp(code.data() + i, render.data(), render.size()) == 0 &&
                std::memcmp(code.data() + i - distance, update.data(), update.size()) == 0)
                output.push_back(rva + static_cast<std::uint32_t>(i));
        return true;
    }
    __except (image_read_error(GetExceptionCode()))
    {
        return false;
    }
}

VersionDiscovery discover(const Image &image)
{
    VersionDiscovery out { DiscoveryStatus::unavailable_layout, image.image_size };
    const auto name_is = [](const IMAGE_SECTION_HEADER &s, const char (&name)[9])
    {
        return std::memcmp(s.Name, name, sizeof(s.Name)) == 0;
    };
    const auto sections = std::span<const IMAGE_SECTION_HEADER>(image.sections).first(image.count);
    const IMAGE_SECTION_HEADER *code = nullptr;
    for (const auto &section : sections)
        if (name_is(section, "il2cpp\0\0") && is_executable(section))
        {
            if (code)
                return { DiscoveryStatus::ambiguous, out.image_size };
            code = &section;
        }
    if (!code)
        return out;

    std::vector<std::uint32_t> methods;
    bool data_seen = false;
    for (const auto &section : sections)
    {
        if ((section.Characteristics & (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE)) != IMAGE_SCN_MEM_READ ||
            !(name_is(section, ".rdata\0\0") || name_is(section, ".data\0\0\0")))
            continue;
        data_seen = true;
        if (!collect_methods(image.section_bytes(section), image.pointer_base, code->VirtualAddress,
            static_cast<std::uint64_t>(code->VirtualAddress) + code->Misc.VirtualSize, methods))
            return { DiscoveryStatus::invalid_image, out.image_size };
    }
    if (data_seen)
    {
        out.status = DiscoveryStatus::no_match;
        select_layout(methods, out);
        out.render_compatible = out.matched() && matches_code(image, out.render, k_render_head) &&
            matches_code(image, out.update, k_ucb_head);
        out.camera_compatible = out.matched() && matches_code(image, out.camera, k_camera_head);
    }
    return out;
}
}

VersionDiscovery discover_version(Bytes mapped)
{
    Image image;
    return parse_image(mapped, image) ? discover(image) : VersionDiscovery {};
}

VersionDiscovery inspect_stored_version(Bytes file)
{
    Image image;
    return parse_image(file, image, true) ? discover(image) : VersionDiscovery {};
}

bool module_contains_executable(std::uintptr_t module, std::uint32_t rva, std::size_t size)
{
    Image image;
    return parse_image(mapped_module(module), image) && !executable_range(image, rva, size).empty();
}

std::optional<ImageFingerprint> module_fingerprint(std::uintptr_t module)
{
    Image image;
    if (!parse_image(mapped_module(module), image)) return std::nullopt;
    return image.fingerprint;
}

MenuDiscovery discover_menu(std::uintptr_t module, MenuRvas cached)
{
    Image image;
    return parse_image(mapped_module(module), image) ? discover_menu_in(image, cached) : MenuDiscovery {};
}

MenuDiscovery inspect_stored_menu(Bytes file)
{
    Image image;
    return parse_image(file, image, true) ? discover_menu_in(image, {}) : MenuDiscovery {};
}

const VersionDiscovery &current_version()
{
    // Render 与透明抖动读取同次识别，禁用两功能时不碰映像数据。
    static const auto result = discover_version(mapped_module(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))));
    return result;
}

const char *discovery_status(DiscoveryStatus status)
{
    switch (status)
    {
    case DiscoveryStatus::invalid_image: return "invalid_image";
    case DiscoveryStatus::unavailable_layout: return "unavailable_layout";
    case DiscoveryStatus::no_match: return "no_match";
    case DiscoveryStatus::ambiguous: return "ambiguous";
    case DiscoveryStatus::matched: return "matched";
    }
    return "unknown";
}

std::vector<std::uint32_t> find_prologue_pairs(std::uintptr_t module, Bytes render, Bytes update, std::uint32_t distance)
{
    Image image;
    std::vector<std::uint32_t> results;
    if (render.empty() || update.empty() || distance == 0 || !parse_image(mapped_module(module), image))
        return results;
    for (const auto &section : std::span<const IMAGE_SECTION_HEADER>(image.sections).first(image.count))
        if (is_executable(section) && !find_pairs_in_section(image.section_bytes(section),
            section.VirtualAddress, render, update, distance, results))
            return {};
    return results;
}
}
