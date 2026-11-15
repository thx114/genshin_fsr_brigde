#include "../game/VersionDiscovery.h"
#include "../game/AnchorPatterns.h"

#include <Windows.h>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace
{
using neo::game::DiscoveryStatus;
using neo::game::k_fsr2_layout;
constexpr std::uint32_t k_code_rva = 0x1000;
constexpr std::uint32_t k_second_family = 0xc000;
constexpr std::uint32_t k_data_rva = 0x20000;
constexpr std::uint32_t k_image_size = k_data_rva + 0x1000;
constexpr auto k_nt = sizeof(IMAGE_DOS_HEADER);
constexpr auto k_sections = k_nt + sizeof(IMAGE_NT_HEADERS64);
using MethodRvas = std::array<std::uint32_t, k_fsr2_layout.gaps.size() + 1>;

struct Image
{
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(k_image_size);
    std::size_t data_used = 0;

    Image()
    {
        IMAGE_DOS_HEADER dos {};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = k_nt;
        put(0, dos);
        IMAGE_NT_HEADERS64 nt {};
        nt.Signature = IMAGE_NT_SIGNATURE;
        nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt.FileHeader.NumberOfSections = 2;
        nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt.OptionalHeader.SizeOfImage = k_image_size;
        nt.OptionalHeader.SizeOfHeaders = k_code_rva;
        put(k_nt, nt);
        IMAGE_SECTION_HEADER code {};
        std::memcpy(code.Name, "il2cpp", 6);
        code.VirtualAddress = k_code_rva;
        code.Misc.VirtualSize = k_data_rva - k_code_rva;
        code.Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
        put(k_sections, code);
        IMAGE_SECTION_HEADER data {};
        std::memcpy(data.Name, ".rdata", 6);
        data.VirtualAddress = k_data_rva;
        data.Misc.VirtualSize = k_image_size - k_data_rva;
        data.Characteristics = IMAGE_SCN_MEM_READ;
        put(k_sections + sizeof(code), data);
    }

    template <typename T>
    void put(std::size_t offset, const T &value)
    {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    void put_anchors(const MethodRvas &methods)
    {
        put(methods[k_fsr2_layout.render_index], neo::game::k_render_head);
        put(methods[k_fsr2_layout.update_index], neo::game::k_ucb_head);
        put(methods[k_fsr2_layout.camera_index], neo::game::k_camera_head);
    }

    MethodRvas family(std::uint32_t start, int edit_index = -1, unsigned delta = 0)
    {
        MethodRvas methods {};
        methods[0] = start;
        for (std::size_t i = 0; i < k_fsr2_layout.gaps.size(); ++i)
            methods[i + 1] = methods[i] + k_fsr2_layout.gaps[i] + (static_cast<int>(i) == edit_index ? delta : 0u);
        const auto push = [&](std::uint32_t rva)
        {
            const auto pointer = reinterpret_cast<std::uintptr_t>(bytes.data()) + rva;
            put(k_data_rva + data_used, pointer);
            data_used += sizeof(pointer);
        };
        // 方法表无需有序；重复引用不能跨越匹配窗口或伪造第二匹配。
        for (auto it = methods.rbegin(); it != methods.rend(); ++it)
            push(*it);
        push(methods.front());
        return methods;
    }
};

unsigned failures = 0;
void expect(bool ok, const char *test)
{
    if (!ok)
    {
        ++failures;
        std::printf("FAIL: %s\n", test);
    }
}
}

int main()
{
    Image first;
    const auto source = first.family(k_code_rva + 0x100);
    const auto layout_only = neo::game::discover_version(first.bytes);
    expect(layout_only.status == DiscoveryStatus::incompatible_anchors && !layout_only.render_compatible &&
        !layout_only.camera_compatible && layout_only.render == 0 && layout_only.update == 0 && layout_only.camera == 0,
        "matching relative offsets alone do not authorize hook installation");
    first.put_anchors(source);
    const auto version = neo::game::discover_version(first.bytes);
    expect(version.render_compatible && version.camera_compatible,
        "complete executable prologues validated independently");
    first.put(source[k_fsr2_layout.update_index], std::uint8_t { 0xcc });
    const auto hooks = neo::game::discover_version(first.bytes);
    expect(hooks.matched() && !hooks.render_compatible && hooks.camera_compatible,
        "bad update prologue rejects Render but not the independent camera anchor");
    first.put(source[k_fsr2_layout.update_index], neo::game::k_ucb_head);

    // 同一签名在磁盘上是首选 VA，文件偏移与 RVA 不相等。
    constexpr std::uintptr_t preferred_base = 0x140000000;
    constexpr std::size_t file_data = 0x300;
    const auto file_code = file_data + first.data_used;
    std::vector<std::uint8_t> file(file_code + (k_data_rva - k_code_rva));
    std::memcpy(file.data(), first.bytes.data(), file_data);
    const auto put_raw = [&](std::size_t offset, const auto &value)
    {
        std::memcpy(file.data() + offset, &value, sizeof(value));
    };
    put_raw(k_nt + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + offsetof(IMAGE_OPTIONAL_HEADER64, ImageBase),
        preferred_base);
    put_raw(k_nt + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfHeaders),
        static_cast<DWORD>(file_data));
    const auto raw_fields = k_sections + offsetof(IMAGE_SECTION_HEADER, SizeOfRawData);
    put_raw(raw_fields, static_cast<DWORD>(k_data_rva - k_code_rva));
    put_raw(raw_fields + sizeof(DWORD), static_cast<DWORD>(file_code));
    put_raw(raw_fields + sizeof(IMAGE_SECTION_HEADER), static_cast<DWORD>(first.data_used));
    put_raw(raw_fields + sizeof(IMAGE_SECTION_HEADER) + sizeof(DWORD), static_cast<DWORD>(file_data));
    std::memcpy(file.data() + file_code, first.bytes.data() + k_code_rva, k_data_rva - k_code_rva);
    for (std::size_t i = 0; i < source.size(); ++i)
        put_raw(file_data + sizeof(std::uintptr_t) * i, preferred_base + source[i]);
    const auto stored = neo::game::inspect_stored_version(file);
    expect(stored.render == version.render && stored.render_compatible && stored.camera_compatible,
        "stored image uses raw offsets and preferred VA without loading the program");
    constexpr std::uint32_t build_rva = k_code_rva + 0x20, text_rva = k_code_rva + 0x40;
    constexpr std::uint32_t update_rva = k_code_rva + 0x80;
    const auto raw_offset = [&](std::uint32_t rva) { return file_code + (rva - k_code_rva); };
    put_raw(raw_offset(build_rva), neo::game::k_build_cmd_buffers_signature);
    put_raw(raw_offset(text_rva), neo::game::k_get_text_signature);
    put_raw(raw_offset(update_rva), neo::game::k_update_inner_target_signature);
    auto menu = neo::game::inspect_stored_menu(file);
    expect(menu.ready() && menu.build.rva == build_rva && menu.text.rva == text_rva && menu.update.rva == update_rva,
        "menu anchors resolved as RVAs from executable code in stored PE");
    put_raw(raw_offset(update_rva + 0x40), neo::game::k_update_inner_target_signature);
    menu = neo::game::inspect_stored_menu(file);
    expect(menu.ready() && menu.update.matches == 2 && menu.update.rva == 0,
        "ambiguous optional menu anchor preserves late build fallback");
    put_raw(raw_offset(text_rva + 0x2000), neo::game::k_get_text_signature);
    menu = neo::game::inspect_stored_menu(file);
    expect(!menu.ready() && menu.text.matches == 2 && menu.text.rva == 0,
        "ambiguous required menu anchor cannot authorize installation");
    put_raw(raw_offset(build_rva), std::uint8_t {0xcc});
    put_raw(file_data, neo::game::k_build_cmd_buffers_signature);
    expect(neo::game::inspect_stored_menu(file).build.matches == 0, "data section cannot supply a menu anchor");
    file.pop_back();
    expect(neo::game::inspect_stored_version(file).status == DiscoveryStatus::invalid_image,
        "truncated raw section rejected");
    expect(version.matched() && version.exact_gaps == k_fsr2_layout.gaps.size() &&
        version.render == source[k_fsr2_layout.render_index] && version.update == source[k_fsr2_layout.update_index] &&
        version.camera == source[k_fsr2_layout.camera_index], "matches a family at the final possible sorted index");

    Image updated;
    const auto moved = updated.family(k_code_rva + 0x1000, 15, 0x10);
    updated.put_anchors(moved);
    const auto relocated = neo::game::discover_version(updated.bytes);
    expect(relocated.matched() && relocated.render_compatible && relocated.exact_gaps + 1 == version.exact_gaps &&
        relocated.render == moved[k_fsr2_layout.render_index] && relocated.render != version.render,
        "relocated image and one changed method gap do not need an RVA update");

    // 2026-11-15：验证映像更新时，缺入口证据的高分窗口不能挡住可执行的布局变体。
    const auto best = updated.family(k_second_family);
    const auto with_layout_decoy = neo::game::discover_version(updated.bytes);
    expect(with_layout_decoy.render == relocated.render && with_layout_decoy.render_compatible &&
        with_layout_decoy.exact_gaps == relocated.exact_gaps,
        "layout-only decoy cannot hide a lower scoring executable family");
    updated.put_anchors(best);
    expect(neo::game::discover_version(updated.bytes).render == best[k_fsr2_layout.render_index],
        "a strictly better executable layout wins instead of first match");
    updated.put(best[k_fsr2_layout.update_index], std::uint8_t { 0xcc });
    updated.put(moved[k_fsr2_layout.camera_index], std::uint8_t { 0xcc });
    const auto render_only = neo::game::discover_version(updated.bytes);
    expect(render_only.render == relocated.render && render_only.render_compatible &&
        !render_only.camera_compatible && render_only.camera == 0,
        "verified Render pair beats camera-only candidate without taking its camera");
    updated.put(moved[k_fsr2_layout.render_index], std::uint8_t { 0xcc });
    const auto camera_only = neo::game::discover_version(updated.bytes);
    expect(camera_only.matched() && camera_only.camera_compatible &&
        camera_only.camera == best[k_fsr2_layout.camera_index] && !camera_only.render_compatible &&
        camera_only.render == 0 && camera_only.update == 0,
        "verified camera remains available when neither Render pair is compatible");

    const auto peer = first.family(k_second_family);
    const auto with_tied_decoy = neo::game::discover_version(first.bytes);
    expect(with_tied_decoy.render_compatible && with_tied_decoy.render == version.render,
        "unverified equal-score layout cannot make a verified family ambiguous");
    first.put_anchors(peer);
    const auto ambiguous = neo::game::discover_version(first.bytes);
    expect(ambiguous.status == DiscoveryStatus::ambiguous && ambiguous.render == 0 &&
        ambiguous.update == 0 && ambiguous.camera == 0, "tied verified candidates are not eligible for auto installation");

    Image no_match;
    no_match.family(k_code_rva + 0x100, static_cast<int>(k_fsr2_layout.update_index), 0x1000);
    expect(neo::game::discover_version(no_match.bytes).status == DiscoveryStatus::no_match,
        "incompatible execution layout stays native");
    expect(neo::game::discover_version(std::span(first.bytes).first(128)).status == DiscoveryStatus::invalid_image,
        "truncated mapped image rejected");
    first.put(k_sections + offsetof(IMAGE_SECTION_HEADER, Misc), DWORD { 0xffffffff });
    expect(neo::game::discover_version(first.bytes).status == DiscoveryStatus::invalid_image,
        "section end overflow rejected before reading");
    std::printf("VersionDiscovery: %u failures\n", failures);
    return failures != 0;
}
