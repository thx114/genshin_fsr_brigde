#pragma once

#include "../interop/BeforeSrV1.h"
#include <wrl/client.h>

namespace neo::ngx
{
struct BeforeSrInput
{
    interop::BeforeSrFrameV1 frame;
    ID3D11ShaderResourceView *source_view; // 与 frame 的原始 Color 来自同一份 Draw 快照。
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
