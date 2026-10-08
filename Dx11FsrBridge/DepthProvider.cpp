#include "DepthProvider.h"
#include <mutex>
#include <atomic>
#include <array>
#include <vector>
#include <memory>
#include <cstring>
#include <wrl/client.h>

namespace {
    void clear_cpu_depth();
    std::mutex g_depth_mutex;
    ID3D11Texture2D* g_depth_texture = nullptr;
    uint32_t g_depth_width = 0;
    uint32_t g_depth_height = 0;
    uint32_t g_depth_format = 0;
    bool g_depth_inverted = false;
    std::atomic<bool> g_depth_logged{false};
}

namespace DepthProvider
{
    void PublishDepth(ID3D11Texture2D* texture, uint32_t width, uint32_t height, uint32_t format, bool depth_inverted)
    {
        if (texture == nullptr || width == 0 || height == 0)
            return;

        std::lock_guard<std::mutex> lock(g_depth_mutex);

        // Release previous texture
        if (g_depth_texture != nullptr)
        {
            g_depth_texture->Release();
            g_depth_texture = nullptr;
        }

        // Store new depth info
        g_depth_texture = texture;
        g_depth_texture->AddRef();
        g_depth_width = width;
        g_depth_height = height;
        g_depth_format = format;
        g_depth_inverted = depth_inverted;

        // Log once
        if (!g_depth_logged.exchange(true))
        {
            // Note: BridgeLogger not included to avoid circular dependency
            // Logging handled by caller if needed
        }
    }

    void ClearDepth()
    {
        std::lock_guard<std::mutex> lock(g_depth_mutex);

        if (g_depth_texture != nullptr)
        {
            g_depth_texture->Release();
            g_depth_texture = nullptr;
        }

        g_depth_width = 0;
        g_depth_height = 0;
        g_depth_format = 0;
        g_depth_inverted = false;
        g_depth_logged = false;
        clear_cpu_depth();
    }
}

extern "C" {

BOOL WINAPI OptiScalerGetDepthDx11Frame(OptiScalerDepthDx11Frame* frame)
{
    if (frame == nullptr)
        return FALSE;

    ID3D11Texture2D* texture_to_ref = nullptr;
    ID3D11Device* device_to_return = nullptr;

    // Critical section: copy values and add references under lock
    {
        std::lock_guard<std::mutex> lock(g_depth_mutex);

        if (g_depth_texture == nullptr || g_depth_width == 0 || g_depth_height == 0)
            return FALSE;

        texture_to_ref = g_depth_texture;
        texture_to_ref->AddRef();

        frame->width = g_depth_width;
        frame->height = g_depth_height;
        frame->format = g_depth_format;
        frame->depth_inverted = g_depth_inverted;
    }

    // GetDevice is called OUTSIDE the lock to avoid potential deadlock
    // (GetDevice might be hooked and could try to acquire other locks)
    texture_to_ref->GetDevice(&device_to_return);

    frame->device = device_to_return;
    frame->texture = texture_to_ref;

    return TRUE;
}

void WINAPI OptiScalerReleaseDepthDx11Frame(OptiScalerDepthDx11Frame* frame)
{
    if (frame == nullptr)
        return;

    if (frame->texture != nullptr)
    {
        frame->texture->Release();
        frame->texture = nullptr;
    }

    if (frame->device != nullptr)
    {
        frame->device->Release();
        frame->device = nullptr;
    }
}

} // extern "C"

namespace {
using Microsoft::WRL::ComPtr;
struct CpuSnapshot { std::vector<float> pixels; uint32_t width=0,height=0; uint64_t id=0,tick=0; bool inverted=false; };
struct ReadbackSlot { ComPtr<ID3D11Texture2D> texture; bool pending=false,inverted=false; uint64_t id=0,tick=0; };
std::atomic<bool> g_cpu_requested{false};
std::mutex g_capture_mutex, g_snapshot_mutex;
ComPtr<ID3D11Device> g_capture_device;
D3D11_TEXTURE2D_DESC g_capture_desc = {};
std::array<ReadbackSlot,3> g_readbacks;
std::shared_ptr<const CpuSnapshot> g_snapshot;
uint64_t g_capture_id=0;
void clear_cpu_depth() {
    std::lock_guard<std::mutex> capture_lock(g_capture_mutex);
    for(auto &slot:g_readbacks) slot=ReadbackSlot{};
    g_capture_device.Reset(); g_capture_desc={};
    std::lock_guard<std::mutex> snapshot_lock(g_snapshot_mutex); g_snapshot.reset();
}

unsigned depth_stride(DXGI_FORMAT format) {
    switch(format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return 8;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return 4;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM: return 2;
    default: return 0;
    }
}
float decode_depth(const uint8_t* pixel, DXGI_FORMAT format) {
    if (depth_stride(format)==2) { uint16_t v; std::memcpy(&v,pixel,2); return v/65535.0f; }
    if (format==DXGI_FORMAT_R24G8_TYPELESS || format==DXGI_FORMAT_D24_UNORM_S8_UINT || format==DXGI_FORMAT_R24_UNORM_X8_TYPELESS) {
        uint32_t v; std::memcpy(&v,pixel,4); return (v & 0xFFFFFFu)/16777215.0f;
    }
    float v; std::memcpy(&v,pixel,4); return v;
}
}

bool DepthProvider::CaptureDepthCpu(ID3D11DeviceContext* context, ID3D11Texture2D* texture, bool inverted)
{
    if (!g_cpu_requested.load(std::memory_order_relaxed) || !context || !texture) return false;
    D3D11_TEXTURE2D_DESC desc; texture->GetDesc(&desc);
    const unsigned stride=depth_stride(desc.Format);
    if (!stride || desc.SampleDesc.Count!=1 || desc.ArraySize!=1 || desc.MipLevels!=1) return false;
    ComPtr<ID3D11Device> device; texture->GetDevice(&device);
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    if (g_capture_device.Get()!=device.Get() || g_capture_desc.Width!=desc.Width || g_capture_desc.Height!=desc.Height || g_capture_desc.Format!=desc.Format) {
        g_capture_device=device; g_capture_desc=desc;
        for(auto &slot:g_readbacks) slot=ReadbackSlot{};
        { std::lock_guard<std::mutex> snapshot_lock(g_snapshot_mutex); g_snapshot.reset(); }
    }
    bool published=false;
    // Complete the oldest submitted readback first. WAS_STILL_DRAWING simply
    // means this frame is not ready; continue normal game rendering immediately.
    for(unsigned attempt=0;attempt<g_readbacks.size();++attempt) {
        ReadbackSlot* oldest=nullptr;
        for(auto &slot:g_readbacks) if(slot.pending && (!oldest || slot.id<oldest->id)) oldest=&slot;
        if(!oldest) break;
        D3D11_MAPPED_SUBRESOURCE mapped={};
        const HRESULT hr=context->Map(oldest->texture.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
        if(hr==DXGI_ERROR_WAS_STILL_DRAWING) break;
        if(FAILED(hr)) { oldest->pending=false; continue; }
        std::shared_ptr<CpuSnapshot> snapshot;
        try { snapshot=std::make_shared<CpuSnapshot>(); snapshot->pixels.resize(size_t(desc.Width)*desc.Height); }
        catch(const std::bad_alloc&) { context->Unmap(oldest->texture.Get(),0); oldest->pending=false; return false; }
        snapshot->width=desc.Width; snapshot->height=desc.Height; snapshot->id=oldest->id; snapshot->tick=oldest->tick; snapshot->inverted=oldest->inverted;
        for(UINT y=0;y<desc.Height;++y) {
            const auto* row=static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch;
            for(UINT x=0;x<desc.Width;++x) snapshot->pixels[size_t(y)*desc.Width+x]=decode_depth(row+size_t(x)*stride,desc.Format);
        }
        context->Unmap(oldest->texture.Get(),0); oldest->pending=false;
        { std::lock_guard<std::mutex> snapshot_lock(g_snapshot_mutex); g_snapshot=std::move(snapshot); }
        published=true;
    }
    for(auto &slot:g_readbacks) if(!slot.pending) {
        if(!slot.texture) {
            auto staging=desc; staging.Usage=D3D11_USAGE_STAGING; staging.BindFlags=staging.MiscFlags=0; staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            if(FAILED(device->CreateTexture2D(&staging,nullptr,&slot.texture))) break;
        }
        context->CopyResource(slot.texture.Get(),texture);
        slot.pending=true; slot.id=++g_capture_id; slot.tick=GetTickCount64(); slot.inverted=inverted;
        break;
    }
    return published;
}

extern "C" BOOL WINAPI OptiScalerGetDepthCpuFrame(BridgeDepthCpuFrame* frame)
{
    if(!frame || frame->struct_size!=sizeof(BridgeDepthCpuFrame)) return FALSE;
    g_cpu_requested.store(true,std::memory_order_relaxed);
    std::shared_ptr<const CpuSnapshot> snapshot;
    { std::lock_guard<std::mutex> lock(g_snapshot_mutex); snapshot=g_snapshot; }
    if(!snapshot || GetTickCount64()-snapshot->tick>2000) return FALSE;
    auto* reference=new(std::nothrow) std::shared_ptr<const CpuSnapshot>(snapshot);
    if(!reference) return FALSE;
    frame->abi_version=1; frame->width=snapshot->width; frame->height=snapshot->height;
    frame->row_pitch=snapshot->width*sizeof(float); frame->format=DXGI_FORMAT_R32_FLOAT; frame->flags=snapshot->inverted?1u:0u; frame->reserved=0;
    frame->frame_id=snapshot->id; frame->capture_tick_ms=snapshot->tick;
    frame->data=snapshot->pixels.data(); frame->token=reference;
    return TRUE;
}
extern "C" void WINAPI OptiScalerReleaseDepthCpuFrame(BridgeDepthCpuFrame* frame)
{
    if(!frame) return;
    delete static_cast<std::shared_ptr<const CpuSnapshot>*>(frame->token);
    frame->token=nullptr; frame->data=nullptr;
}
