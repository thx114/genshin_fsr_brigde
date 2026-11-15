#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace neo::game
{
// 来自原桥 FFX_FSR2 的 30 个连续方法，不依赖国服/国际服的固定 RVA。
struct MethodLayout
{
    std::array<std::uint16_t, 29> gaps;
    std::size_t camera_index;
    std::size_t update_index;
    std::size_t render_index;
};

inline constexpr MethodLayout k_fsr2_layout {
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0xD0, 0x10, 0x170, 0x30, 0x20,
      0x10, 0x130, 0x20, 0xB0, 0x70, 0x3E0, 0x1D0, 0x1CE0, 0x18C0, 0x120, 0x70, 0xFA0,
      0x280, 0x260, 0x110 },
    19, 24, 25,
};

enum class DiscoveryStatus
{
    invalid_image,
    unavailable_layout,
    no_match,
    incompatible_anchors,
    ambiguous,
    matched,
};

struct VersionDiscovery
{
    DiscoveryStatus status = DiscoveryStatus::invalid_image;
    std::uint32_t image_size = 0;
    std::uint32_t render = 0;
    std::uint32_t update = 0;
    std::uint32_t camera = 0;
    unsigned exact_gaps = 0;
    bool render_compatible = false;
    bool camera_compatible = false;

    // matched 至少有一组入口通过验证；Camera 可独立匹配，不代表 Render 可用。
    bool matched() const { return status == DiscoveryStatus::matched; }
};

// 输入为已映射映像；段用 RVA、方法表用当前 VA，不能直接传磁盘文件字节。
VersionDiscovery discover_version(std::span<const std::uint8_t> image);
// 只读文件布局入口；不装载也不执行待验程序。
VersionDiscovery inspect_stored_version(std::span<const std::uint8_t> file);
const VersionDiscovery &current_version();
bool module_contains_executable(std::uintptr_t module, std::uint32_t rva, std::size_t size);
const char *discovery_status(DiscoveryStatus status);

struct ImageFingerprint
{
    std::uint32_t timestamp;
    std::uint32_t image_size;
    std::uint32_t checksum;
};

struct MenuRvas
{
    std::uint32_t build = 0;
    std::uint32_t text = 0;
    std::uint32_t update = 0;
};

struct CodeAnchor
{
    std::uint32_t rva = 0;
    unsigned matches = 0; // 2 表示歧义（至少两处），不携带可用 RVA。
};

struct MenuDiscovery
{
    CodeAnchor build, text, update;
    bool cache_hit = false;

    bool ready() const { return build.rva && text.rva; }
    MenuRvas rvas() const { return { build.rva, text.rva, update.rva }; }
};

std::optional<ImageFingerprint> module_fingerprint(std::uintptr_t module);
MenuDiscovery discover_menu(std::uintptr_t module, MenuRvas cached = {});
MenuDiscovery inspect_stored_menu(std::span<const std::uint8_t> file);

// 专供未安装时的诊断，无权选择或安装任何候选。
std::vector<std::uint32_t> find_prologue_pairs(std::uintptr_t module,
    std::span<const std::uint8_t> render_head, std::span<const std::uint8_t> update_head, std::uint32_t distance);
}
