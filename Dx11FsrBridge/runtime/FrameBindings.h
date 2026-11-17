#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <span>

namespace neo::graphics
{
// 2026-11-15：验证与写出共用本次 Draw 的引用，不能用上一帧的视图或二次查绑定来认领输出。
class FrameBindings
{
public:
    enum class Extent { signature, legacy };
    using Resource = Microsoft::WRL::ComPtr<ID3D11Resource>;
    using Texture = Microsoft::WRL::ComPtr<ID3D11Texture2D>;

    explicit FrameBindings(ID3D11DeviceContext &context, Extent extent = Extent::signature) noexcept;
    ~FrameBindings();
    FrameBindings(const FrameBindings &) = delete;
    FrameBindings &operator=(const FrameBindings &) = delete;

    std::span<ID3D11RenderTargetView *const> targets() const noexcept;
    std::span<ID3D11ShaderResourceView *const> inputs() noexcept;
    Resource target_resource(UINT slot) const noexcept;
    Resource input_resource(UINT slot) noexcept;
    static Texture texture_2d(const Resource &resource) noexcept;

private:
    ID3D11DeviceContext &context_; // 调用者持有当前 Draw 的 context，绑定快照不跨调用。
    std::array<ID3D11ShaderResourceView *, 8> inputs_ {};
    std::array<ID3D11RenderTargetView *, 4> targets_ {};
    UINT input_count_;
    UINT target_count_;
    bool read_inputs_ = false;
};
}
