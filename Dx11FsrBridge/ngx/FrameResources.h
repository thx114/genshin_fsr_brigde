#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <optional>

namespace ffx12 { struct FrameInput; }

namespace neo::ngx
{
struct FrameShape
{
    UINT render_width = 0, render_height = 0;
    UINT output_width = 0, output_height = 0;
    DXGI_FORMAT color_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT output_format = DXGI_FORMAT_UNKNOWN;
    bool operator==(const FrameShape &) const = default;
};

std::optional<FrameShape> copy_shape(const ffx12::FrameInput &input,
    const D3D11_TEXTURE2D_DESC &color, const D3D11_TEXTURE2D_DESC &output) noexcept;

class GuideView
{
public:
    enum class Kind { depth, motion };
    bool bind(ID3D11Device &device, ID3D11Texture2D *source, Kind kind, const FrameShape &shape) noexcept;
    ID3D11ShaderResourceView *get() const noexcept { return view_.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view_;
    ID3D11Texture2D *source_ = nullptr; // view_ 持有该资源，因而缓存命中时不会发生地址复用。
};

struct FrameResources
{
    FrameShape shape {};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color, depth, motion, reactive, output;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> depth_uav, motion_uav, reactive_uav;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
    GuideView depth_src, motion_src;

    struct BuildResult
    {
        HRESULT hr;
        const char *step;
    };

    // 2026-11-15：尺寸、纹理与视图一次发布，失败不得把半套池交给 Create/Evaluate。
    [[nodiscard]] BuildResult rebuild(ID3D11Device &device, const FrameShape &next) noexcept;
    bool ready() const noexcept { return output != nullptr; }
    bool bind_guides(ID3D11Device &device, const ffx12::FrameInput &input) noexcept;
    void record_inputs(ID3D11DeviceContext &context, const ffx12::FrameInput &input,
        ID3D11ComputeShader *depth_cs, ID3D11ComputeShader *motion_cs) const noexcept;
};
}
