#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <optional>

namespace neo::graphics
{
// 关联只用于选实例；只有本实例本代次写成的实际纹理可以交付给另一输出。
class OutputHistory
{
public:
    enum class Mode { Repeat, Repair };

    struct Delivery
    {
        ID3D11Texture2D *source; // 借用本 history 的纹理，调用者在本次 Draw 使用。
        bool same_output;
    };

    void publish(ID3D11Texture2D &canonical, std::uint64_t generation, std::uint64_t output) noexcept;
    void invalidate_frame() noexcept;
    void forget_instance() noexcept;
    bool owns(std::uint64_t output) const noexcept;
    bool has_association() const noexcept { return a_ != 0 || b_ != 0; }
    std::uint64_t first() const noexcept { return a_; }
    std::uint64_t second() const noexcept { return b_; }
    bool can_repair(std::uint64_t generation) const noexcept;
    std::optional<Delivery> deliver(ID3D11DeviceContext &context, ID3D11Texture2D &target,
        std::uint64_t generation, std::uint64_t output, Mode mode) noexcept;

private:
    void note_output(std::uint64_t output) noexcept;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> canonical_;
    std::uint64_t generation_ = 0;
    std::uint64_t repair_generation_ = 0;
    std::uint64_t a_ = 0, b_ = 0;
};
}
