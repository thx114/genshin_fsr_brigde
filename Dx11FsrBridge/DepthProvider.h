#pragma once

#include <d3d11.h>
#include <cstdint>
#include "DepthCpuAbi.h"

// Depth frame export interface for ReShade addon
// Based on OptiScaler's depth provider interface

extern "C" {

struct OptiScalerDepthDx11Frame
{
    ID3D11Device* device;
    ID3D11Texture2D* texture;
    uint32_t width;
    uint32_t height;
    uint32_t format;  // DXGI_FORMAT
    bool depth_inverted;
};

// Get current depth frame (caller must call Release when done)
__declspec(dllexport) BOOL WINAPI OptiScalerGetDepthDx11Frame(OptiScalerDepthDx11Frame* frame);

// Release depth frame (decrements refcount)
__declspec(dllexport) void WINAPI OptiScalerReleaseDepthDx11Frame(OptiScalerDepthDx11Frame* frame);

// Does not wait or access D3D11. Caller initializes struct_size to sizeof(frame).
__declspec(dllexport) BOOL WINAPI OptiScalerGetDepthCpuFrame(BridgeDepthCpuFrame* frame);
__declspec(dllexport) void WINAPI OptiScalerReleaseDepthCpuFrame(BridgeDepthCpuFrame* frame);

} // extern "C"

// Internal functions (not exported)
namespace DepthProvider
{
    void PublishDepth(ID3D11Texture2D* texture, uint32_t width, uint32_t height, uint32_t format, bool depth_inverted);
    void ClearDepth();
    // Called only on the game rendering thread at the existing depth capture point.
    // Polls staging textures with DO_NOT_WAIT; never mutates pipeline bindings.
    bool CaptureDepthCpu(ID3D11DeviceContext* context, ID3D11Texture2D* texture, bool inverted);
}
