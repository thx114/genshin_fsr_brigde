#pragma once
#include "SkinMaskProbe.h"
#include "SkinShaderFamily.h"
#include <cstddef>
#include <cstdint>
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
namespace skinmaterial {
void configure(const wchar_t* iniPath);
bool enabled();
bool family_enabled();
skinfamily::Result alpha_family(uint64_t hash,uint32_t stencilRef,bool request);
void register_pixel_shader(uint64_t hash,const void* bytecode,std::size_t size);
void begin_session(uint32_t session,uint64_t instance=0,uint64_t frame=0);
void end_session();
void observe_draw(ID3D11DeviceContext* context,const skinprobe::DrawIdentity& identity,
                  ID3D11ShaderResourceView* const* views,uint32_t count);
void poll(ID3D11DeviceContext* context);
}
