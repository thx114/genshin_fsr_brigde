#pragma once
#include <cstdint>
#include <cstddef>

// Stable, read-only producer ABI for a future DLSS/NR consumer. The current
// runtime only publishes the verified face-only diagnostic guide; it never
// writes NGX ControlMask or changes the active NR path.
#ifdef _WIN32
#define HYSK_FACE_API __declspec(dllexport)
#else
#define HYSK_FACE_API
#endif

extern "C" {
struct HYSK_FACE_GUIDE_INFO {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t width;
    uint32_t height;
    uint32_t row_pitch;
    uint32_t format;       // DXGI_FORMAT_R8_UNORM (61)
    uint32_t flags;        // bit0 face-only, bit1 original orientation, bit2 gpu/cpu equal
    uint32_t reserved;
    uint64_t generation;
    uint64_t instance;
    uint64_t capture_boundary;
    uint64_t snapshot_boundary;
};

// Returns non-zero only when a complete face-only GPU guide has been published.
// The returned metadata is copied into the caller's structure.
HYSK_FACE_API int HYSK_GetFaceGuideInfo(HYSK_FACE_GUIDE_INFO *info);

// Copies the latest original-orientation packed R8 guide. If capacity is too
// small, returns 0 and reports the required byte count in bytes_required.
// No pointer to internal storage escapes the DLL.
HYSK_FACE_API int HYSK_CopyFaceGuide(void *destination, uint32_t capacity,
                                     uint32_t *bytes_required,
                                     HYSK_FACE_GUIDE_INFO *info);

// Test/runtime lifecycle helper. It does not disable NR; it only invalidates
// the optional producer snapshot so a future consumer cannot reuse old frames.
HYSK_FACE_API void HYSK_InvalidateFaceGuide(void);
}

namespace faceinterop {
void publish(const HYSK_FACE_GUIDE_INFO &info, const uint8_t *bytes, size_t size);
void invalidate();
}
