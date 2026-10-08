#include "FaceGuideInterop.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
std::mutex g_mutex;
HYSK_FACE_GUIDE_INFO g_info{};
std::vector<uint8_t> g_bytes;
uint64_t g_generation=0;
}

namespace faceinterop {
void publish(const HYSK_FACE_GUIDE_INFO &source,const uint8_t *bytes,size_t size) {
    if(!bytes||!size||source.width==0||source.height==0||source.row_pitch<source.width||
       source.format!=61||uint64_t(source.row_pitch)*source.height!=size)return;
    std::lock_guard lock(g_mutex);
    g_bytes.assign(bytes,bytes+size);
    g_info=source;g_info.struct_size=sizeof(HYSK_FACE_GUIDE_INFO);g_info.abi_version=1;g_info.generation=++g_generation;
}
void invalidate() {std::lock_guard lock(g_mutex);g_bytes.clear();g_info={};}
}

extern "C" HYSK_FACE_API int HYSK_GetFaceGuideInfo(HYSK_FACE_GUIDE_INFO *info) {
    if(!info)return 0;std::lock_guard lock(g_mutex);
    if(g_bytes.empty())return 0;const auto requested=info->struct_size;
    const auto copy=std::min<uint32_t>(requested?requested:sizeof(HYSK_FACE_GUIDE_INFO),sizeof(HYSK_FACE_GUIDE_INFO));
    std::memset(info,0,requested?requested:sizeof(HYSK_FACE_GUIDE_INFO));std::memcpy(info,&g_info,copy);return 1;
}
extern "C" HYSK_FACE_API int HYSK_CopyFaceGuide(void *destination,uint32_t capacity,uint32_t *bytes_required,HYSK_FACE_GUIDE_INFO *info) {
    std::lock_guard lock(g_mutex);if(g_bytes.empty())return 0;
    if(bytes_required)*bytes_required=static_cast<uint32_t>(g_bytes.size());
    if(info) {const auto requested=info->struct_size;const auto copy=std::min<uint32_t>(requested?requested:sizeof(HYSK_FACE_GUIDE_INFO),sizeof(HYSK_FACE_GUIDE_INFO));std::memset(info,0,requested?requested:sizeof(HYSK_FACE_GUIDE_INFO));std::memcpy(info,&g_info,copy);}
    if(!destination||capacity<g_bytes.size())return 0;std::memcpy(destination,g_bytes.data(),g_bytes.size());return 1;
}
extern "C" HYSK_FACE_API void HYSK_InvalidateFaceGuide(void) {faceinterop::invalidate();}
