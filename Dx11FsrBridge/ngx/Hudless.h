#pragma once

#include "../interop/HudlessV1.h"
#include "../third_party/ngx/nvsdk_ngx_params.h"
#include <wrl/client.h>

namespace neo::ngx
{
struct HudlessInput
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color;
    bool flip = false;
};

HudlessInput record_hudless(const interop::HudlessRequestV1 &request, ID3D11Device &device) noexcept;

class HudlessParameterScope
{
public:
    HudlessParameterScope(NVSDK_NGX_Parameter &parameters, HudlessInput input) noexcept;
    ~HudlessParameterScope();
    HudlessParameterScope(const HudlessParameterScope &) = delete;
    HudlessParameterScope &operator=(const HudlessParameterScope &) = delete;

private:
    NVSDK_NGX_Parameter &parameters_;
    HudlessInput input_;
};
}
