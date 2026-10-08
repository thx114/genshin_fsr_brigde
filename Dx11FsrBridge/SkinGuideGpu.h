#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
namespace skinguide {
// Read-only candidate producer. NO NGX parameters, automatic-mask policy,
// consumer interop, frame-generation or presentation behavior is changed.
// Both inputs must be private read-only SRV-capable paired GPU snapshots.
// Returned guide is original input orientation, full resolution, R8_UNORM.
// It is NOT asserted to have DLSSNR.ControlMask semantics.
struct Candidate {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
};
struct FaceAccumulation {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> coverage,reference;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> coverageView,referenceView;
};
// Native face shader draw delta, NOT a draw replay or replacement. Accumulate
// only changes made by the verified constant-face shader into private resources.
bool accumulate_face_delta(ID3D11DeviceContext* context,ID3D11Texture2D* before,
    ID3D11Texture2D* after,uint32_t normalViewFormat,FaceAccumulation& face,
    const char** reason=nullptr) noexcept;
bool generate_face(ID3D11DeviceContext* context,ID3D11Texture2D* finalNormal,
    uint32_t normalViewFormat,ID3D11Texture2D* originalDepth,
    const std::array<uint8_t,256>& stencilIds,bool selectStencil,
    const FaceAccumulation& face,Candidate& result,const char** reason=nullptr) noexcept;
bool generate(ID3D11DeviceContext* context,ID3D11Texture2D* normal,
              uint32_t normalViewFormat,ID3D11Texture2D* originalDepth,
              const std::array<uint8_t,256>& stencilIds,bool selectStencil,
              Candidate& result,const char** reason=nullptr) noexcept;
}
