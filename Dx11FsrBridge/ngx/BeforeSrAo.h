#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

namespace neo::ngx
{
struct AoProjection
{
    float near_plane;
    float far_plane;
    float vertical_fov;
    bool reversed_depth;
};

// 2026-11-15：只消费桥本帧准备好的输入。不持有 Present/插件状态，不做颜色转换或历史重投影。
// 原色必须为桥自有的输入池；只在工作完成后写回该副本，绝不写游戏源纹理。
class BeforeSrAo
{
public:
    bool record(ID3D11Device &device, ID3D11DeviceContext &context, ID3D11Texture2D &color,
        ID3D11Texture2D &depth, const AoProjection &projection) noexcept;

private:
    struct Constants
    {
        float near_plane;
        float near_over_far;
        float ray_step;
        UINT reversed_depth;
        bool operator==(const Constants &) const = default;
    } last_constants_ {};

    bool prepare(ID3D11Device &device, ID3D11DeviceContext &context, ID3D11Texture2D &color,
        ID3D11Texture2D &depth) noexcept;

    bool initialize(ID3D11Device &device, ID3D11DeviceContext &context, ID3D11Texture2D &color,
        ID3D11Texture2D &depth) noexcept;

    static_assert(sizeof(Constants) == 16);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color_, depth_, result_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> execution_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> attempted_context_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvs_[2];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> result_uav_;
    UINT width_ = 0, height_ = 0;
};
}
