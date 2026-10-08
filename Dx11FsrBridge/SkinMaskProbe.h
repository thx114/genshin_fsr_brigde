#pragma once
#include <cstdint>
#include <array>
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11DepthStencilView;
struct ID3D11RenderTargetView;

// Diagnostics only: captures the *pre-NR* bridge inputs without changing them.
// Candidate skin hue is not a semantic material ID and is never submitted to NGX.
namespace skinprobe {
struct Frame {
    ID3D11DeviceContext* context=nullptr;
    ID3D11Texture2D* color=nullptr;
    ID3D11ShaderResourceView* depth=nullptr;
    uint32_t width=0,height=0,outputWidth=0,outputHeight=0;
    uint64_t instance=0,frame=0;
    float jitterX=0,jitterY=0;
    bool pq=false,linear=true;
};
struct TextureIdentity {
    uint64_t key=0;
    uint32_t width=0,height=0,format=0,viewFormat=0;
    bool operator==(const TextureIdentity&) const = default;
};
struct TargetIdentity {
    uint32_t resourceFormat=0,viewFormat=0,viewDimension=0,mipLevels=0,arraySize=0,samples=0,mipSlice=0,misc=0,mrtCount=0;
    bool operator==(const TargetIdentity&) const = default;
};
TargetIdentity describe_render_target(ID3D11RenderTargetView* view);
struct DrawIdentity {
    uint64_t pixelHash=0,vertexHash=0,renderTarget=0,depthTarget=0;
    uint32_t width=0,height=0,stencilRef=0,stencilEnabled=0,writeMask=0,passOp=0;
    TargetIdentity target;
    std::array<TextureIdentity,8> textures{};
    bool operator==(const DrawIdentity&) const = default;
};
// Only queries during bounded post-F8 trace bursts; false in ordinary draws.
void prepare_face_draw(ID3D11DeviceContext* context,ID3D11RenderTargetView* view,
                       ID3D11DepthStencilView* depth,const DrawIdentity& identity);
void finish_face_draw(ID3D11DeviceContext* context,uint64_t token=0) noexcept;
struct FaceDrawScope {
    ID3D11DeviceContext* context;
    uint64_t token=0,previousToken=0;
    explicit FaceDrawScope(ID3D11DeviceContext* c) noexcept;
    ~FaceDrawScope() noexcept;
};
bool wants_draw_trace();
bool gbuffer_enabled();
// Per-frame guide fast path (SkinMaskProbeFastPath). active: the caller must run the cheap
// per-draw observation; interesting: this pixel shader can be the verified face/material
// carrier the observation accepts, so the full light observation is worth running.
bool fastpath_active();
bool fastpath_interesting_shader(uint64_t hash);
void before_geometry_targets_change(ID3D11DeviceContext* context,uint32_t count,
    ID3D11RenderTargetView* const* views,ID3D11DepthStencilView* depth);
void before_geometry_color_clear(ID3D11DeviceContext* context,ID3D11RenderTargetView* view);
bool geometry_extent(uint32_t width,uint32_t height,uint32_t elements);
void remember_geometry_depth(ID3D11DeviceContext* context,ID3D11DepthStencilView* view);
void remember_geometry_target(ID3D11DeviceContext* context,ID3D11RenderTargetView* view,
                              ID3D11DepthStencilView* depth,uint64_t pixelHash);
void remember_geometry_target(ID3D11DeviceContext* context,ID3D11RenderTargetView* view,
                              ID3D11DepthStencilView* depth,uint64_t pixelHash,uint32_t stencilRef);
void before_stencil_clear(ID3D11DeviceContext* context,ID3D11DepthStencilView* view);
void record_draw(const DrawIdentity& identity,uint32_t elements,bool indexed);
void configure(const wchar_t* iniPath);
bool enabled();
void on_frame(const Frame& frame);
}
