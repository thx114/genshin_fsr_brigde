#pragma once

#include "../interop/BeforeSrV1.h"
#include <wrl/client.h>

namespace neo::ngx
{
struct BeforeSrInput
{
    interop::BeforeSrFrameV1 frame;
    ID3D11Texture2D *source_color = nullptr; // CopyResource 的游戏源纹理，不是 NGX 的副本。
    ID3D11ShaderResourceView *source_view = nullptr; // 借用同次 Draw 的实际 SRV。
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
