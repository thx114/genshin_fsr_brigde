#pragma once

#include "../interop/BeforeSrV1.h"
#include <wrl/client.h>

namespace neo::ngx
{
struct BeforeSrColorSource
{
    ID3D11Texture2D *texture = nullptr;
    ID3D11ShaderResourceView *view = nullptr;
    interop::ColorTransfer transfer = interop::ColorTransfer::unknown;
};

struct BeforeSrInput
{
    interop::BeforeSrFrameV1 frame;
    // 2026-11-16：原值 Copy 继承来源，AO 描述自己的输出；typed view 不认证像素编码。
    BeforeSrColorSource producer;
};

class BeforeSrQueryScope
{
public:
    explicit BeforeSrQueryScope(const BeforeSrInput &input) noexcept;
    ~BeforeSrQueryScope();
    BeforeSrQueryScope(const BeforeSrQueryScope &) = delete;
    BeforeSrQueryScope &operator=(const BeforeSrQueryScope &) = delete;

private:
    const BeforeSrInput *previous_;
};

bool has_before_sr_callback() noexcept;
Microsoft::WRL::ComPtr<ID3D11Texture2D> record_before_sr(const BeforeSrInput &input,
    ID3D11Device &device) noexcept;
}
