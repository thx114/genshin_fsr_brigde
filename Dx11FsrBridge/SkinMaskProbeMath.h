#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace skinprobe {
// A deliberately broad diagnostic warm-chroma candidate, NOT skin semantics.
// Shadows, makeup, fantasy skin and warm backgrounds can all disagree with it.
inline bool warm_candidate(float r,float g,float b) {
    if (!std::isfinite(r)||!std::isfinite(g)||!std::isfinite(b)||r<0||g<0||b<0) return false;
    float peak=std::max({r,g,b});
    if(peak<0.025f || peak>65504.f) return false;
    r/=peak;g/=peak;b/=peak;
    const float sat=1-std::min({r,g,b});
    return r>=g && g>=b && sat>0.045f && sat<0.72f && g>0.42f && b>0.24f;
}
inline std::array<uint8_t,256> parse_stencil_ids(std::wstring_view text,bool* valid=nullptr) {
    std::array<uint8_t,256> ids{};bool ok=true;
    size_t at=0;
    while(at<text.size()) {
        while(at<text.size() && (text[at]==L' '||text[at]==L','||text[at]==L';'||text[at]==L'\t'))++at;
        if(at==text.size())break;
        unsigned value=0,count=0;
        while(at<text.size() && text[at]>=L'0' && text[at]<=L'9') {
            value=value*10+unsigned(text[at++]-L'0');++count;
            if(value>255 || count>3){ok=false;break;}
        }
        if(!ok || count==0){ok=false;break;}
        if(at<text.size() && text[at]!=L','&&text[at]!=L';'&&text[at]!=L' '&&text[at]!=L'\t'){ok=false;break;}
        ids[value]=1;
    }
    if(!ok)ids.fill(0);
    if(valid)*valid=ok;
    return ids;
}
inline bool stencil8(uint32_t format,const uint8_t* pixel,uint8_t& value) {
    // Numeric constants from DXGI_FORMAT, CPU-only testing needs no SDK.
    if(!pixel)return false;
    switch(format) {
    case 19: case 20: // R32G8X24_TYPELESS / D32_FLOAT_S8X24_UINT
        value=pixel[4];return true;
    case 44: case 45: // R24G8_TYPELESS / D24_UNORM_S8_UINT
        value=pixel[3];return true;
    default:return false; // depth-only views are NOT proof of stencil data.
    }
}
inline uint32_t sample_stride(uint32_t w,uint32_t h,uint32_t maxDim) {
    return std::max(1u,(std::max(w,h)+std::max(1u,maxDim)-1)/std::max(1u,maxDim));
}
inline uint32_t bytes_per_pixel(uint32_t format) {
    switch(format) {
    case 2:return 16; // Private FP32 face reference, never a color heuristic
    case 42:return 4; // Private R32_UINT face coverage
    case 61:return 1; // R8_UNORM diagnostic GPU guide readback
    case 10: case 19: case 20:return 8;
    case 24:case 26:case 27:case 28:case 29:case 39:case 40:case 41:
    case 44:case 45:case 46:case 87:case 90:case 91:return 4;
    default:return 0;
    }
}
inline bool valid_extent(uint32_t w,uint32_t h,uint32_t bpp,uint32_t rowPitch) {
    return w && h && w<=4096 && h<=4096 && bpp &&
        uint64_t(w)*bpp<=rowPitch && uint64_t(rowPitch)*h<=128ull*1024*1024;
}
}
