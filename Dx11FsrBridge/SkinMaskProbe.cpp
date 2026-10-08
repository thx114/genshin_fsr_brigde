#include "SkinMaskProbe.h"
#include "SkinMaterialEvidence.h"
#include "SkinMaskProbeMath.h"
#include "SkinGBufferProbeMath.h"
#include "SkinGuideGpu.h"
#include "FaceGuideInterop.h"
#include "Fsr2InputDump.h"
#include "BridgeLogger.h"
#include <Windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <memory>

namespace skinprobe {
namespace {
using Microsoft::WRL::ComPtr;
struct Settings {
    bool enabled=false;
    uint32_t frames=3,maxDim=1024,hotkey=VK_F8,interval=250,autoStart=0,maxSessions=3;
    std::filesystem::path directory;
    std::array<uint8_t,256> stencilIds{};
    bool selected=false,heuristic=false,gbuffer=false,gbufferSelected=false,gbufferFresh=false,gpu=false,faceOnly=false;
    // Continuous: keep re-arming captures while enabled (no per-process session limit) so the
    // published face guide stays live. Quiet: publish the guide but skip the diagnostic dumps.
    bool continuous=false,quiet=false;
    // Fast path: re-arm the same-interval face pair every fastpathStride frames and publish
    // only the R8 guide, bypassing the paired-MRT wait, the multi-texture readback and the
    // analysis worker. Independent of the diagnostic (continuous/hotkey) capture path.
    bool fastpath=false;
    uint32_t fastpathStride=2;
    std::array<uint8_t,256> gbufferStencilIds{};
};
struct Image { uint32_t width=0,height=0,format=0,pitch=0; std::vector<uint8_t> bytes; };
struct NormalAdmission {uint64_t pixelHash=0,key=0;TargetIdentity target;uint32_t decodeFormat=0,rejection=0;skinfamily::Result family;};
struct DrawRecord {DrawIdentity identity;uint32_t calls=0;uint64_t elements=0;bool indexed=false;};
struct Pending {
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> color,depth,materialDepth,normal,gpu,faceCoverage,faceReference;
    ComPtr<ID3D11Query> ready;
    uint64_t queued=0,frame=0,instance=0,sequence=0;
    uint64_t fsrDepthKey=0,materialDepthKey=0,materialSnapshotAgeMs=0;
    bool materialBeforeClear=false;
    uint64_t normalKey=0,normalPixelHash=0,normalSnapshotAgeMs=0;
    bool normalBeforeClear=false,normalBeforeColorClear=false;
    uint32_t normalDecodeFormat=0,normalResourceFormat=0,normalSnapshotPhase=0;
    uint64_t normalSnapshotBoundary=0,captureBoundary=0,normalSnapshotInstance=0;
    bool normalSameInterval=false,normalFreshRequested=false,gpuRequested=false;
    const char* gpuStatus="not-requested";
    bool faceOnly=false;uint32_t faceDraws=0;uint64_t faceShaderHash=0;
    const char* faceStatus="not-requested";
    std::vector<NormalAdmission> admissions;
    uint32_t width=0,height=0,outW=0,outH=0;
    float jitterX=0,jitterY=0;
    bool linear=true,pq=false;
    std::vector<DrawRecord> draws;
    uint32_t traceDropped=0;
};
struct Job { Settings settings;Pending metadata; Image color,depth,materialDepth,normal,gpu,faceCoverage,faceReference; };
std::atomic_bool active{false},writerBusy{false},gbufferActive{false},gpuActive{false},faceActive{false};
std::mutex mutex;
Settings settings;
std::unique_ptr<Pending> pending;
uint64_t startTick=0,nextCapture=0,sequence=0;
uint32_t remaining=0,sessions=0;
// Set from Settings::quiet: suppresses the per-capture PNG/R8 diagnostic dump while still
// publishing the face guide, so continuous capture does not write ~11 MB per capture.
std::atomic<bool> quietDumps{false};
// --- per-frame guide fast path ---------------------------------------------------------
// One slot per in-flight guide readback. A slot that the GPU has not finished is simply
// retried on a later frame, so the render thread never waits on a readback.
struct FastSlot {
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Query> ready;
    ComPtr<ID3D11DeviceContext> context;
    uint32_t width=0,height=0;
    uint64_t instance=0,captureBoundary=0,snapshotBoundary=0;
    bool busy=false;
};
constexpr uint32_t kFastSlots=2;
std::array<FastSlot,kFastSlots> fastSlots;
std::atomic_bool fastActive{false};
// Last admitted original MRT0 carrier shader hash, learned from a full observation. Until it
// is known every draw is classified once (bounded bootstrap), afterwards only carriers are.
std::atomic<uint64_t> fastCarrierHash{0};
uint64_t fastFrame=0,fastLastPublish=0,fastLastGap=0,fastLastBytes=0;
uint32_t fastPublished=0,fastStalls=0,fastSkips=0,fastQueued=0;
uint64_t fastWindowStart=0;
bool fastPairArmed=false;
const char* fastStatus="not-started";
bool hotkeyDown=false,autoFired=false;
std::atomic<uint32_t> drawBudget{0};
std::atomic_bool freshActive{false},pairRequested{false};
std::atomic<uint64_t> bridgeBoundary{0},lastBridgeInstance{0};
uint64_t freshWaitStart=0;
std::mutex drawMutex;
std::vector<DrawRecord> drawRecords;
uint32_t droppedDraws=0;
constexpr uint32_t kDrawQueryBudget=2048,kUniqueDrawLimit=128;
std::atomic<uint32_t> renderHintWidth{0},renderHintHeight{0};
ComPtr<ID3D11Texture2D> geometryDepth;
ComPtr<ID3D11DeviceContext> geometryContext;
ComPtr<ID3D11Texture2D> geometryBeforeClear;
uint64_t geometrySnapshotTick=0,geometrySnapshotKey=0;
ComPtr<ID3D11Texture2D> geometryNormal,geometryNormalBeforeClear,normalPairedDepthBeforeClear;
ComPtr<ID3D11DeviceContext> normalContext;
ComPtr<ID3D11DepthStencilView> normalDepthView;
uint64_t normalDepthKey=0,normalPixelHash=0,normalSnapshotTick=0,normalSnapshotKey=0,normalSnapshotPixelHash=0;
bool normalSnapshotBeforeColor=false,normalDirty=false;
uint32_t normalDecodeFormat=0,normalSnapshotDecodeFormat=0,normalSnapshotResourceFormat=0,normalSnapshotPhase=0;
uint64_t normalSnapshotDepthKey=0,normalSnapshotBoundary=0,normalSnapshotInstance=0;
std::vector<NormalAdmission> normalAdmissions;
ComPtr<ID3D11Texture2D> faceBefore,faceAfter,faceTarget;
ComPtr<ID3D11DeviceContext> faceContext;
skinguide::FaceAccumulation faceAccum;
uint64_t faceBoundary=0,faceInstance=0,faceDepthKey=0,faceTargetKey=0,faceShaderHash=0;
uint32_t faceDraws=0,faceCopyWidth=0,faceCopyHeight=0;
std::atomic_bool facePrepared{false};
thread_local uint64_t faceScopeToken=0,faceScopeSerial=0;
uint64_t preparedFaceToken=0;
const char* faceStatus="not-requested";
struct FaceWarmup {uint64_t ps=0,rt=0,ds=0,instance=0;};
std::vector<FaceWarmup> faceWarmed;
uint64_t faceWarmupBoundary=0;
void reset_face_locked() {
    faceBefore.Reset();faceAfter.Reset();faceTarget.Reset();faceContext.Reset();faceAccum={};
    faceBoundary=faceInstance=faceDepthKey=faceTargetKey=faceShaderHash=0;faceDraws=0;facePrepared=false;faceStatus="not-requested";
}
const char* admission_reason(uint32_t reason) {
    switch(reason){case 0:return "accepted";case 1:return "unsupported-view";case 2:return "array-or-msaa";
    case 3:return "unsupported-or-incompatible-typed-format";case 4:return "wrong-render-extent";case 5:return "shared-resource";
    case 6:return "unverified-shader-family";case 7:return "shader-family-analysis-pending";
    default:return "unknown";}
}


bool write(const std::filesystem::path& path,const std::vector<uint8_t>& bytes) {
    if(quietDumps.load(std::memory_order_relaxed))return false;
    std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);
    std::ofstream stream(path,std::ios::binary|std::ios::trunc);
    if(!stream)return false;
    stream.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}
float uf(uint32_t bits,unsigned mantissa) {
    const uint32_t exponent=bits>>mantissa,fraction=bits&((1u<<mantissa)-1);
    if(exponent==31)return NAN;
    if(exponent==0)return std::ldexp(float(fraction),-14-int(mantissa));
    return std::ldexp(1.f+float(fraction)/float(1u<<mantissa),int(exponent)-15);
}
bool color_at(const Image& image,uint32_t x,uint32_t y,float rgb[3]) {
    if(x>=image.width||y>=image.height)return false;
    const auto bpp=bytes_per_pixel(image.format);
    if(!valid_extent(image.width,image.height,bpp,image.pitch))return false;
    const size_t offset=size_t(y)*image.pitch+size_t(x)*bpp;
    if(offset+bpp>image.bytes.size())return false;
    const auto* p=image.bytes.data()+offset;
    switch(image.format) {
    case 10: {
        uint16_t value[4];std::memcpy(value,p,8);
        for(unsigned i=0;i<3;++i)rgb[i]=fsr2dump::half_to_float(value[i]);return true;
    }
    case 26: {
        uint32_t value;std::memcpy(&value,p,4);
        rgb[0]=uf(value&0x7ff,6);rgb[1]=uf((value>>11)&0x7ff,6);rgb[2]=uf(value>>22,5);return true;
    }
    case 24: {
        uint32_t value;std::memcpy(&value,p,4);
        rgb[0]=float(value&1023)/1023;rgb[1]=float((value>>10)&1023)/1023;rgb[2]=float((value>>20)&1023)/1023;return true;
    }
    case 27:case 28:case 29:
        rgb[0]=p[0]/255.f;rgb[1]=p[1]/255.f;rgb[2]=p[2]/255.f;return true;
    case 87:case 90:case 91:
        rgb[0]=p[2]/255.f;rgb[1]=p[1]/255.f;rgb[2]=p[0]/255.f;return true;
    default:return false;
    }
}
uint8_t display_value(float value,bool linear) {
    if(!std::isfinite(value)||value<0)return 0;
    value=std::clamp(value,0.f,1.f);
    if(linear)value=value<=0.0031308f?value*12.92f:1.055f*std::pow(value,1.f/2.4f)-0.055f;
    return uint8_t(std::round(std::clamp(value,0.f,1.f)*255));
}
void analyze(const Job& job) {
    const auto& m=job.metadata;
    const uint32_t stride=sample_stride(job.color.width,job.color.height,job.settings.maxDim);
    const uint32_t w=(job.color.width+stride-1)/stride,h=(job.color.height+stride-1)/stride;
    const size_t pixels=size_t(w)*h;
    std::vector<uint8_t> scene(pixels*4),mask(pixels*4),overlay(pixels*4),classes(pixels*4),materialClasses(pixels*4),rawMask(pixels),rawStencil(pixels),rawMaterialStencil(pixels);
    std::array<uint64_t,256> histogram{},warmHistogram{},materialHistogram{};
    bool stencilAvailable=job.depth.format==19||job.depth.format==20||job.depth.format==44||job.depth.format==45;
    const bool materialAvailable=job.materialDepth.format==19||job.materialDepth.format==20||job.materialDepth.format==44||job.materialDepth.format==45;
    const bool normalAvailable=m.normalSnapshotPhase!=0&&job.normal.width==job.color.width&&job.normal.height==job.color.height&&gbuffer_alpha_format(job.normal.format)&&!job.normal.bytes.empty();
    std::vector<uint8_t> normalAlpha(pixels*4),skinMask(pixels*4),skinOverlay(pixels*4),rawAlpha(pixels),rawSkin(pixels);
    std::array<uint64_t,256> alphaHistogram{};
    const bool faceAvailable=m.faceOnly&&normalAvailable&&job.faceCoverage.format==42&&job.faceReference.format==2&&
        job.faceCoverage.width==job.color.width&&job.faceCoverage.height==job.color.height&&
        job.faceReference.width==job.color.width&&job.faceReference.height==job.color.height&&!job.faceCoverage.bytes.empty()&&!job.faceReference.bytes.empty();
    const bool gpuAvailable=job.gpu.format==61&&job.gpu.width==job.color.width&&job.gpu.height==job.color.height&&!job.gpu.bytes.empty();
    // With the fast path enabled it owns publication (per-frame, current interval); this
    // diagnostic capture must not overwrite a fresher published guide with an older frame.
    if(!fastActive.load(std::memory_order_relaxed)&&m.faceOnly&&gpuAvailable&&job.gpu.bytes.size()==size_t(job.gpu.pitch)*job.gpu.height) {
        HYSK_FACE_GUIDE_INFO info{};info.struct_size=sizeof(info);info.width=job.gpu.width;info.height=job.gpu.height;
        info.row_pitch=job.gpu.pitch;info.format=job.gpu.format;info.flags=1u|2u;info.instance=m.instance;
        info.capture_boundary=m.captureBoundary;info.snapshot_boundary=m.normalSnapshotBoundary;
        faceinterop::publish(info,job.gpu.bytes.data(),job.gpu.bytes.size());
    }
    std::vector<uint8_t> gpuMask(pixels*4),gpuOverlay(pixels*4),rawGpu(pixels);
    uint64_t gpuPixels=0,gpuMismatch=0;
    uint64_t flagPixels=0,skinPixels=0,alphaDecoded=0;
    uint64_t warmCount=0,selectedCount=0,decoded=0;
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
        const uint32_t sx=std::min(x*stride,job.color.width-1),sy=std::min(y*stride,job.color.height-1);
        const size_t index=size_t(y)*w+x,at=index*4;
        float rgb[3]{};const bool gotColor=color_at(job.color,sx,sy,rgb);decoded+=gotColor;
        // Diagnostic candidates are refused for PQ, not guessed from its nonlinear code values.
        const bool warm=gotColor&&!job.metadata.pq&&warm_candidate(rgb[0],rgb[1],rgb[2]);
        warmCount+=warm;
        uint8_t stencil=0;
        bool gotStencil=false;
        if(stencilAvailable&&!job.depth.bytes.empty()) {
            const uint32_t dx=std::min(uint32_t(uint64_t(sx)*job.depth.width/job.color.width),job.depth.width-1);
            const uint32_t dy=std::min(uint32_t(uint64_t(sy)*job.depth.height/job.color.height),job.depth.height-1);
            const uint32_t bpp=bytes_per_pixel(job.depth.format);
            const size_t offset=size_t(dy)*job.depth.pitch+size_t(dx)*bpp;
            if(offset+bpp<=job.depth.bytes.size())gotStencil=stencil8(job.depth.format,job.depth.bytes.data()+offset,stencil);
        }
        if(gotStencil){++histogram[stencil];warmHistogram[stencil]+=warm;rawStencil[index]=stencil;}
        uint8_t materialStencil=0;bool gotMaterial=false;
        if(materialAvailable && !job.materialDepth.bytes.empty()) {
            const uint32_t mx=std::min(uint32_t(uint64_t(sx)*job.materialDepth.width/job.color.width),job.materialDepth.width-1);
            const uint32_t my=std::min(uint32_t(uint64_t(sy)*job.materialDepth.height/job.color.height),job.materialDepth.height-1);
            const uint32_t bpp=bytes_per_pixel(job.materialDepth.format);
            const size_t offset=size_t(my)*job.materialDepth.pitch+size_t(mx)*bpp;
            if(offset+bpp<=job.materialDepth.bytes.size())gotMaterial=stencil8(job.materialDepth.format,job.materialDepth.bytes.data()+offset,materialStencil);
        }
        if(gotMaterial){++materialHistogram[materialStencil];rawMaterialStencil[index]=materialStencil;}
        float alpha=0;bool gotAlpha=false;
        if(normalAvailable) {
            const uint32_t bpp=gbuffer_bytes_per_pixel(job.normal.format);
            const size_t offset=size_t(sy)*job.normal.pitch+size_t(sx)*bpp;
            if(offset+bpp<=job.normal.bytes.size())gotAlpha=packed_gbuffer_alpha(job.normal.format,job.normal.bytes.data()+offset,bpp,alpha);
        }
        const bool flag=gotAlpha&&gbuffer_skin_flag(alpha);flagPixels+=flag;alphaDecoded+=gotAlpha;
        bool skin=flag&&(!job.settings.gbufferSelected||(gotMaterial&&job.settings.gbufferStencilIds[materialStencil]));
        if(m.faceOnly) {
            skin=false;
            if(faceAvailable) {
                uint32_t covered=0;float finalValue[4]{},faceValue[4]{};
                const auto coverAt=size_t(sy)*job.faceCoverage.pitch+size_t(sx)*4;
                const auto refAt=size_t(sy)*job.faceReference.pitch+size_t(sx)*16;
                const auto normalAt=size_t(sy)*job.normal.pitch+size_t(sx)*4;
                if(coverAt+4<=job.faceCoverage.bytes.size()&&refAt+16<=job.faceReference.bytes.size()&&normalAt+4<=job.normal.bytes.size()) {
                    std::memcpy(&covered,job.faceCoverage.bytes.data()+coverAt,4);
                    if(covered) {
                        std::memcpy(faceValue,job.faceReference.bytes.data()+refAt,16);
                        skin=packed_r10_value(job.normal.bytes.data()+normalAt,4,finalValue)&&face_reference_matches(finalValue,faceValue)&&
                            (!job.settings.gbufferSelected||(gotMaterial&&job.settings.gbufferStencilIds[materialStencil]));
                    }
                }
            }
        }
        skinPixels+=skin;
        if(gotAlpha){rawAlpha[index]=uint8_t(std::round(alpha*255.f));++alphaHistogram[rawAlpha[index]];}
        rawSkin[index]=skin?255:0;
        uint8_t gpuValue=0;
        if(gpuAvailable) {
            const size_t offset=size_t(sy)*job.gpu.pitch+sx;
            if(offset<job.gpu.bytes.size())gpuValue=job.gpu.bytes[offset];
            rawGpu[index]=gpuValue;gpuPixels+=gpuValue!=0;
            if(!m.faceOnly||faceAvailable)gpuMismatch+=gpuValue!=rawSkin[index];
        }

        // Selected diagnostic IDs retain their original FSR-input scope. The
        // independent material mask is a separate observation, not silently substituted.
        const bool selected=job.settings.selected ? gotStencil&&job.settings.stencilIds[stencil] : job.settings.heuristic&&warm;
        selectedCount+=selected;
        for(unsigned c=0;c<3;++c)scene[at+c]=overlay[at+c]=display_value(rgb[c],m.linear);
        scene[at+3]=overlay[at+3]=mask[at+3]=classes[at+3]=materialClasses[at+3]=255;
        mask[at]=mask[at+1]=mask[at+2]=rawMask[index]=selected?255:0;
        for(unsigned c=0;c<3;++c) {
            normalAlpha[at+c]=rawAlpha[index];skinMask[at+c]=rawSkin[index];skinOverlay[at+c]=scene[at+c];
        }
        normalAlpha[at+3]=skinMask[at+3]=skinOverlay[at+3]=255;
        for(unsigned c=0;c<3;++c){gpuMask[at+c]=gpuValue;gpuOverlay[at+c]=scene[at+c];}gpuMask[at+3]=gpuOverlay[at+3]=255;
        if(gpuValue){gpuOverlay[at]=uint8_t(scene[at]/2);gpuOverlay[at+1]=uint8_t(128+scene[at+1]/2);gpuOverlay[at+2]=uint8_t(scene[at+2]/2);}
        if(skin){skinOverlay[at]=uint8_t(scene[at]/2);skinOverlay[at+1]=uint8_t(128+scene[at+1]/2);skinOverlay[at+2]=uint8_t(scene[at+2]/2);}
        if(selected) {
            // Green is *candidate*, never a promise of correctly identified skin.
            overlay[at]=uint8_t(scene[at]/2);overlay[at+1]=uint8_t(128+scene[at+1]/2);overlay[at+2]=uint8_t(scene[at+2]/2);
        }
        if(gotStencil&&stencil) {
            classes[at]=uint8_t((unsigned(stencil)*73)%200+55);
            classes[at+1]=uint8_t((unsigned(stencil)*151)%200+55);
            classes[at+2]=uint8_t((unsigned(stencil)*199)%200+55);
        }
        if(gotMaterial&&materialStencil) {
            materialClasses[at]=uint8_t((unsigned(materialStencil)*73)%200+55);
            materialClasses[at+1]=uint8_t((unsigned(materialStencil)*151)%200+55);
            materialClasses[at+2]=uint8_t((unsigned(materialStencil)*199)%200+55);
        }
    }
    const std::string stem="p"+std::to_string(GetCurrentProcessId())+"_s"+std::to_string(m.sequence)+"_i"+std::to_string(m.instance)+"_f"+std::to_string(m.frame);
    auto png=[&](const char* suffix,const std::vector<uint8_t>& data) {
        if(quietDumps.load(std::memory_order_relaxed))return false; // skip the PNG encode cost too
        return write(job.settings.directory/(stem+suffix+".png"),fsr2dump::encode_png_rgba(data.data(),w,h));
    };
    bool saved=png("_color",scene)&&png("_candidate",mask)&&png("_overlay",overlay)&&png("_stencil",classes);
    // A second diagnostic orientation is explicit. This is NOT a production flip fix.
    std::vector<uint8_t> flipped(overlay.size());
    for(uint32_t y=0;y<h;++y)std::memcpy(flipped.data()+size_t(y)*w*4,overlay.data()+size_t(h-1-y)*w*4,size_t(w)*4);
    saved=png("_overlay_flipY",flipped)&&saved;
    saved=write(job.settings.directory/(stem+"_candidate.R8.raw"),rawMask)&&saved;
    if(stencilAvailable)saved=write(job.settings.directory/(stem+"_stencil.R8.raw"),rawStencil)&&saved;
    if(materialAvailable) {
        saved=png("_material_stencil",materialClasses)&&saved;
        std::vector<uint8_t> materialFlipped(materialClasses.size());
        for(uint32_t y=0;y<h;++y)std::memcpy(materialFlipped.data()+size_t(y)*w*4,materialClasses.data()+size_t(h-1-y)*w*4,size_t(w)*4);
        saved=png("_material_stencil_flipY",materialFlipped)&&saved;
        saved=write(job.settings.directory/(stem+"_material_stencil.R8.raw"),rawMaterialStencil)&&saved;
    }
    if(normalAvailable) {
        saved=png("_gbuffer_alpha",normalAlpha)&&saved;
        saved=png("_material_skin_candidate",skinMask)&&saved;
        saved=png("_material_skin_overlay",skinOverlay)&&saved;
        std::vector<uint8_t> fullFlip(skinOverlay.size()),maskFlip(skinOverlay.size());
        for(uint32_t y=0;y<h;++y) {
            std::memcpy(fullFlip.data()+size_t(y)*w*4,skinOverlay.data()+size_t(h-1-y)*w*4,size_t(w)*4);
            for(uint32_t x=0;x<w;++x) {
                const size_t i=size_t(y)*w+x,at=i*4;
                const bool flippedSkin=rawSkin[size_t(h-1-y)*w+x]!=0;
                for(unsigned c=0;c<3;++c)maskFlip[at+c]=scene[at+c];maskFlip[at+3]=255;
                if(flippedSkin){maskFlip[at]=uint8_t(scene[at]/2);maskFlip[at+1]=uint8_t(128+scene[at+1]/2);maskFlip[at+2]=uint8_t(scene[at+2]/2);}
            }
        }
        saved=png("_material_skin_overlay_flipY",fullFlip)&&saved;
        saved=png("_material_skin_overlay_maskflipY",maskFlip)&&saved;
        saved=write(job.settings.directory/(stem+"_gbuffer_alpha.R8.raw"),rawAlpha)&&saved;
        saved=write(job.settings.directory/(stem+"_material_skin_candidate.R8.raw"),rawSkin)&&saved;
    }
    if(gpuAvailable) {
        saved=png("_gpu_skin_candidate",gpuMask)&&saved;saved=png("_gpu_skin_overlay",gpuOverlay)&&saved;
        std::vector<uint8_t> upright(gpuOverlay.size());
        for(uint32_t y=0;y<h;++y)std::memcpy(upright.data()+size_t(y)*w*4,gpuOverlay.data()+size_t(h-1-y)*w*4,size_t(w)*4);
        saved=png("_gpu_skin_overlay_flipY",upright)&&saved;
        saved=write(job.settings.directory/(stem+"_gpu_skin_candidate.R8.raw"),rawGpu)&&saved;
        // Full-resolution raw data retains original guide orientation and exact
        // format bytes. CPU preview/reference sampling is independently bounded.
        saved=write(job.settings.directory/(stem+"_gpu_skin_full.R8.raw"),job.gpu.bytes)&&saved;
    }
    if(m.faceOnly&&gpuAvailable) {
        saved=png("_face_candidate",gpuMask)&&saved;
        saved=png("_face_overlay",gpuOverlay)&&saved;
        std::vector<uint8_t> upright(gpuOverlay.size());
        for(uint32_t y=0;y<h;++y)std::memcpy(upright.data()+size_t(y)*w*4,gpuOverlay.data()+size_t(h-1-y)*w*4,size_t(w)*4);
        saved=png("_face_overlay_flipY",upright)&&saved;
        saved=write(job.settings.directory/(stem+"_face_candidate.R8.raw"),rawGpu)&&saved;
    }
    std::ostringstream meta;
    meta<<R"JSON({
  "schema":1,
  "phase":"bridge_before_ngx_nr",
  "consumer":"none-diagnostic-only",
  "semanticSkinConfirmed":false,
  "selection":")JSON"<<(job.settings.selected?"explicit-stencil-id-unverified":job.settings.heuristic?"warm-chroma-heuristic-not-semantic":"no-verified-material-rule")<<R"JSON(",
  "instance":)JSON"<<m.instance<<R"JSON(,
  "frame":)JSON"<<m.frame<<R"JSON(,
  "captureWidth":)JSON"<<job.color.width<<R"JSON(,
  "captureHeight":)JSON"<<job.color.height<<R"JSON(,
  "outputWidth":)JSON"<<m.outW<<R"JSON(,
  "outputHeight":)JSON"<<m.outH<<R"JSON(,
  "imageWidth":)JSON"<<w<<R"JSON(,
  "imageHeight":)JSON"<<h<<R"JSON(,
  "sampleStride":)JSON"<<stride<<R"JSON(,
  "orientation":"unaltered-bridge-input",
  "jitter":[)JSON"<<m.jitterX<<","<<m.jitterY<<R"JSON(],
  "colorFormat":)JSON"<<job.color.format<<R"JSON(,
  "depthFormat":)JSON"<<job.depth.format<<R"JSON(,
  "pq":)JSON"<<(m.pq?"true":"false")<<R"JSON(,
  "colorDecoded":)JSON"<<decoded<<R"JSON(,
  "stencilAvailable":)JSON"<<(stencilAvailable?"true":"false")<<R"JSON(,
  "warmCandidatePixels":)JSON"<<warmCount<<R"JSON(,
  "selectedPixels":)JSON"<<selectedCount<<R"JSON(,
  "stencilClasses":[)JSON";
    bool first=true;
    for(unsigned id=0;id<256;++id)if(histogram[id]) {
        if(!first)meta<<",";first=false;
        meta<<R"JSON({"id":)JSON"<<id<<R"JSON(,"pixels":)JSON"<<histogram[id]<<R"JSON(,"warmPixels":)JSON"<<warmHistogram[id]<<"}";
    }
    meta<<R"JSON(],"fsrDepthKey":)JSON"<<m.fsrDepthKey<<R"JSON(,"materialDepthKey":)JSON"<<m.materialDepthKey
        <<R"JSON(,"materialDepthFormat":)JSON"<<job.materialDepth.format<<R"JSON(,"materialStencilAvailable":)JSON"<<(materialAvailable?"true":"false")
        <<R"JSON(,"materialBeforeClear":)JSON"<<(m.materialBeforeClear?"true":"false")
        <<R"JSON(,"materialSnapshotAgeMs":)JSON"<<m.materialSnapshotAgeMs<<R"JSON(,"materialStencilClasses":[)JSON";
    bool firstClass=true;
    for(unsigned id=0;id<256;++id)if(materialHistogram[id]) {
        if(!firstClass)meta<<",";firstClass=false;
        meta<<R"JSON({"id":)JSON"<<id<<R"JSON(,"pixels":)JSON"<<materialHistogram[id]<<"}";
    }
    meta<<R"JSON(],"gbufferAvailable":)JSON"<<(normalAvailable?"true":"false")
        <<R"JSON(,"gbufferKey":)JSON"<<m.normalKey<<R"JSON(,"gbufferPixelHash":)JSON"<<m.normalPixelHash
        <<R"JSON(,"gbufferFormat":)JSON"<<job.normal.format<<R"JSON(,"gbufferBeforeClear":)JSON"<<(m.normalBeforeClear?"true":"false")
        <<R"JSON(,"gbufferResourceFormat":)JSON"<<m.normalResourceFormat
        <<R"JSON(,"gbufferDecodeFormat":)JSON"<<m.normalDecodeFormat<<R"JSON(,"gbufferSnapshotPhase":)JSON"<<m.normalSnapshotPhase
        <<R"JSON(,"gbufferBeforeTargetChange":)JSON"<<(m.normalSnapshotPhase==3?"true":"false")
        <<R"JSON(,"gbufferBeforeColorClear":)JSON"<<(m.normalBeforeColorClear?"true":"false")
        <<R"JSON(,"gbufferSnapshotAgeMs":)JSON"<<m.normalSnapshotAgeMs
        <<R"JSON(,"gbufferFreshRequested":)JSON"<<(m.normalFreshRequested?"true":"false")
        <<R"JSON(,"gbufferSnapshotBoundary":)JSON"<<m.normalSnapshotBoundary<<R"JSON(,"gbufferCaptureBoundary":)JSON"<<m.captureBoundary
        <<R"JSON(,"gbufferSnapshotInstance":)JSON"<<m.normalSnapshotInstance
        <<R"JSON(,"gbufferSameBridgeInterval":)JSON"<<(m.normalSameInterval?"true":"false")
        <<R"JSON(,"gbufferSameFrameConfirmed":false,"gbufferPixelAlignmentConfirmed":false,"gbufferClassification":"captured-game-MRT0-alpha-0.333-candidate-not-NR-mask")JSON"
        <<R"JSON(,"gpuGuideRequested":)JSON"<<(m.gpuRequested?"true":"false")
        <<R"JSON(,"gpuGuideStatus":")JSON"<<m.gpuStatus<<R"JSON(","gpuGuideAvailable":)JSON"<<(gpuAvailable?"true":"false")
        <<R"JSON(,"gpuGuideFormat":)JSON"<<job.gpu.format<<R"JSON(,"gpuGuideWidth":)JSON"<<job.gpu.width<<R"JSON(,"gpuGuideHeight":)JSON"<<job.gpu.height
        <<R"JSON(,"gpuGuideScope":")JSON"<<(m.faceOnly?"verified-native-face-draw-delta":"material-alpha-candidate")<<R"JSON(")JSON"
        <<R"JSON(,"faceOnlyRequested":)JSON"<<(m.faceOnly?"true":"false")<<R"JSON(,"faceReferenceAvailable":)JSON"<<(faceAvailable?"true":"false")
        <<R"JSON(,"faceNativeDraws":)JSON"<<m.faceDraws<<R"JSON(,"faceShaderHash":)JSON"<<m.faceShaderHash<<R"JSON(,"faceStatus":")JSON"<<m.faceStatus<<R"JSON(")JSON"
        <<R"JSON(,"gpuVsCpuComparisonAvailable":)JSON"<<(gpuAvailable&&(!m.faceOnly||faceAvailable)?"true":"false")
        <<R"JSON(,"gpuGuideCandidatePixels":)JSON"<<gpuPixels<<R"JSON(,"gpuVsCpuMismatchPixels":)JSON"<<gpuMismatch
        <<R"JSON(,"gpuGuideConsumer":"none-diagnostic-only","gpuGuideOriginalOrientation":true,"gpuGuideFullRawRowPitch":)JSON"<<job.gpu.pitch
        <<R"JSON(,"gbufferAlphaDecoded":)JSON"<<alphaDecoded<<R"JSON(,"gbufferFlagPixels":)JSON"<<flagPixels<<R"JSON(,"materialSkinCandidatePixels":)JSON"<<skinPixels
        <<R"JSON(,"gbufferStencilSelection":[)JSON";
    bool firstSelector=true;for(unsigned id=0;id<256;++id)if(job.settings.gbufferStencilIds[id]){if(!firstSelector)meta<<',';firstSelector=false;meta<<id;}
    meta<<R"JSON(],"gbufferAlphaClasses":[)JSON";
    bool firstAlpha=true;for(unsigned id=0;id<256;++id)if(alphaHistogram[id]){if(!firstAlpha)meta<<',';firstAlpha=false;meta<<R"JSON({"alphaByte":)JSON"<<id<<R"JSON(,"pixels":)JSON"<<alphaHistogram[id]<<'}';}
    meta<<R"JSON(],"gbufferAdmissions":[)JSON";
    bool firstAdmission=true;
    for(const auto& a:m.admissions) {
        if(!firstAdmission)meta<<',';firstAdmission=false;const auto& t=a.target;
        meta<<R"JSON({"ps":)JSON"<<a.pixelHash<<R"JSON(,"rt":)JSON"<<a.key
            <<R"JSON(,"resourceFormat":)JSON"<<t.resourceFormat<<R"JSON(,"viewFormat":)JSON"<<t.viewFormat
            <<R"JSON(,"viewDimension":)JSON"<<t.viewDimension<<R"JSON(,"mipLevels":)JSON"<<t.mipLevels
            <<R"JSON(,"arraySize":)JSON"<<t.arraySize<<R"JSON(,"samples":)JSON"<<t.samples
            <<R"JSON(,"decodeFormat":)JSON"<<a.decodeFormat<<R"JSON(,"alphaFamilyKind":)JSON"<<uint32_t(a.family.kind)
            <<R"JSON(,"alphaFamilyReason":")JSON"<<a.family.reason<<R"JSON(","alphaSampleLine":)JSON"<<a.family.sampleLine
            <<R"JSON(,"alphaBucketLine":)JSON"<<a.family.bucketLine<<R"JSON(,"alphaClassLine":)JSON"<<a.family.classLine
            <<R"JSON(,"alphaOutputLine":)JSON"<<a.family.outputLine<<R"JSON(,"reason":")JSON"<<admission_reason(a.rejection)<<R"JSON("})JSON";
    }
    meta<<"]\n}\n";
    const auto text=meta.str();saved=write(job.settings.directory/(stem+"_meta.json"),{text.begin(),text.end()})&&saved;
    std::ostringstream draws;
    draws<<R"JSON({"schema":1,"classification":"unverified-material-candidates","sample":"geometry-draw-state-before-original-or-bridge-takeover","captureInstance":)JSON"<<m.instance
         <<R"JSON(,"captureFrame":)JSON"<<m.frame<<R"JSON(,"queryBudget":2048,"uniqueLimit":128,"dropped":)JSON"<<m.traceDropped<<R"JSON(,"draws":[)JSON";
    auto hash=[](uint64_t value) {std::ostringstream out;out<<"0x"<<std::hex<<std::setw(16)<<std::setfill('0')<<value;return out.str();};
    bool firstDraw=true;
    for(const auto& record:m.draws) {
        const auto& d=record.identity;
        if(!firstDraw)draws<<",";firstDraw=false;
        draws<<R"JSON({"ps":")JSON"<<hash(d.pixelHash)<<R"JSON(","vs":")JSON"<<hash(d.vertexHash)
             <<R"JSON(","rt":")JSON"<<hash(d.renderTarget)<<R"JSON(","dsv":")JSON"<<hash(d.depthTarget)
             <<R"JSON(","width":)JSON"<<d.width<<R"JSON(,"height":)JSON"<<d.height
             <<R"JSON(,"stencilRef":)JSON"<<d.stencilRef<<R"JSON(,"stencilEnabled":)JSON"<<d.stencilEnabled
             <<R"JSON(,"writeMask":)JSON"<<d.writeMask<<R"JSON(,"passOp":)JSON"<<d.passOp
             <<R"JSON(,"calls":)JSON"<<record.calls<<R"JSON(,"elements":)JSON"<<record.elements
             <<R"JSON(,"rtResourceFormat":)JSON"<<d.target.resourceFormat<<R"JSON(,"rtViewFormat":)JSON"<<d.target.viewFormat
             <<R"JSON(,"rtViewDimension":)JSON"<<d.target.viewDimension<<R"JSON(,"rtMipLevels":)JSON"<<d.target.mipLevels
             <<R"JSON(,"rtArraySize":)JSON"<<d.target.arraySize<<R"JSON(,"rtSamples":)JSON"<<d.target.samples
             <<R"JSON(,"rtMipSlice":)JSON"<<d.target.mipSlice<<R"JSON(,"rtMisc":)JSON"<<d.target.misc
             <<R"JSON(,"mrtCount":)JSON"<<d.target.mrtCount<<R"JSON(,"textures":[)JSON";
        for(unsigned slot=0;slot<d.textures.size();++slot) {
            if(slot)draws<<",";const auto& t=d.textures[slot];
            draws<<R"JSON({"slot":)JSON"<<slot<<R"JSON(,"key":")JSON"<<hash(t.key)<<R"JSON(","width":)JSON"<<t.width
                 <<R"JSON(,"height":)JSON"<<t.height<<R"JSON(,"format":)JSON"<<t.format<<R"JSON(,"viewFormat":)JSON"<<t.viewFormat<<"}";
        }
        draws<<"]}";
    }
    draws<<"]}\n";const auto drawText=draws.str();
    write(job.settings.directory/(stem+"_draws.json"),{drawText.begin(),drawText.end()});
    LOG_INFO(blog::cat::core,"skin_probe output="+stem+" saved="+std::to_string(saved)+" stencil="+std::to_string(stencilAvailable)+" pixels="+std::to_string(selectedCount)+" semantic_skin=unverified consumer=none");
}
bool read_image(ID3D11DeviceContext* ctx,ID3D11Texture2D* texture,Image& image,uint32_t decodeFormat=0) {
    if(!texture)return false;
    D3D11_TEXTURE2D_DESC desc;texture->GetDesc(&desc);
    const uint32_t format=decodeFormat?decodeFormat:desc.Format;
    const auto bpp=decodeFormat?gbuffer_bytes_per_pixel(decodeFormat):bytes_per_pixel(desc.Format);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(ctx->Map(texture,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped)))return false;
    if(!mapped.pData || !valid_extent(desc.Width,desc.Height,bpp,mapped.RowPitch)){ctx->Unmap(texture,0);return false;}
    image={desc.Width,desc.Height,format,desc.Width*bpp,{}};
    try {
        image.bytes.resize(size_t(image.pitch)*image.height);
        for(uint32_t y=0;y<image.height;++y)std::memcpy(image.bytes.data()+size_t(y)*image.pitch,
            static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,image.pitch);
    } catch(...) {ctx->Unmap(texture,0);return false;}
    ctx->Unmap(texture,0);return true;
}
bool queue_texture(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11Texture2D* source,
                   uint32_t w,uint32_t h,ComPtr<ID3D11Texture2D>& staging,uint32_t decodeFormat=0) {
    if(!source)return false;
    D3D11_TEXTURE2D_DESC d;source->GetDesc(&d);
    const uint32_t bpp=decodeFormat?gbuffer_bytes_per_pixel(decodeFormat):bytes_per_pixel(d.Format);
    if(!bpp || d.SampleDesc.Count!=1 || d.ArraySize!=1 || !w || !h || w>d.Width || h>d.Height ||
       !valid_extent(w,h,bpp,w*bpp))return false;
    const bool depthBound=(d.BindFlags&D3D11_BIND_DEPTH_STENCIL)!=0;
    // D3D11 requires a whole-subresource copy with a NULL box for DS-bound
    // sources, even if a supplied box happens to cover the entire texture.
    if(depthBound&&(w!=d.Width||h!=d.Height))return false;
    d.Width=w;d.Height=h;d.MipLevels=1;d.ArraySize=1;d.SampleDesc={1,0};
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
    if(FAILED(dev->CreateTexture2D(&d,nullptr,&staging)))return false;
    D3D11_BOX box{0,0,0,w,h,1};ctx->CopySubresourceRegion(staging.Get(),0,0,0,0,source,0,depthBound?nullptr:&box);return true;
}
// Fast path copy: reuses the slot's staging texture across frames (created once per extent)
// so a per-frame publish does not allocate, and copies only the R8 guide.
bool queue_guide_staging(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11Texture2D* source,
                         uint32_t w,uint32_t h,ComPtr<ID3D11Texture2D>& staging) {
    if(!dev||!ctx||!source||!w||!h)return false;
    D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
    if(w>d.Width||h>d.Height)return false;
    if(staging){D3D11_TEXTURE2D_DESC s{};staging->GetDesc(&s);if(s.Width!=w||s.Height!=h)staging.Reset();}
    if(!staging) {
        D3D11_TEXTURE2D_DESC s=d;s.Width=w;s.Height=h;s.MipLevels=1;s.ArraySize=1;s.SampleDesc={1,0};
        s.Usage=D3D11_USAGE_STAGING;s.BindFlags=0;s.CPUAccessFlags=D3D11_CPU_ACCESS_READ;s.MiscFlags=0;
        if(FAILED(dev->CreateTexture2D(&s,nullptr,&staging)))return false;
    }
    D3D11_BOX box{0,0,0,w,h,1};ctx->CopySubresourceRegion(staging.Get(),0,0,0,0,source,0,&box);return true;
}
}
// Fast path predicates: cheap enough for every draw of every frame. The shader filter keeps
// the per-draw cost to one shader hash lookup plus a switch/comparison.
bool fastpath_active(){return fastActive.load(std::memory_order_acquire);}
bool fastpath_interesting_shader(uint64_t hash) {
    if(!fastActive.load(std::memory_order_relaxed)||!hash)return false;
    if(alpha_carrier_shader(hash))return true;
    const auto learned=fastCarrierHash.load(std::memory_order_relaxed);
    return learned==0||learned==hash;
}
void configure(const wchar_t* iniPath) {
    std::lock_guard lock(mutex);if(!iniPath)return;
    Settings next;
    next.enabled=GetPrivateProfileIntW(L"Dx11FsrBridge",L"SkinMaskProbe",0,iniPath)!=0;
    next.faceOnly=GetPrivateProfileIntW(L"Dx11FsrBridge",L"SkinMaskProbeFaceOnly",0,iniPath)!=0;
    next.gpu=GetPrivateProfileIntW(L"Dx11FsrBridge",L"SkinMaskProbeGBufferGPU",0,iniPath)!=0;
    next.gbufferFresh=GetPrivateProfileIntW(L"Dx11FsrBridge",L"SkinMaskProbeGBufferFresh",0,iniPath)!=0;
    next.gbuffer=GetPrivateProfileIntW(L"Dx11FsrBridge",L"SkinMaskProbeGBuffer",0,iniPath)!=0;
    next.heuristic=GetPrivateProfileIntW(L"Dx11FsrBridge",L"SkinMaskProbeHeuristic",0,iniPath)!=0;
    auto read=[&](const wchar_t* name,uint32_t def,uint32_t min,uint32_t max){
        return std::clamp<uint32_t>(GetPrivateProfileIntW(L"Dx11FsrBridge",name,def,iniPath),min,max);};
    next.frames=read(L"SkinMaskProbeFrames",3,1,6);
    next.maxDim=read(L"SkinMaskProbeMaxDim",1024,64,2048);
    next.hotkey=read(L"SkinMaskProbeHotkey",VK_F8,0,255);
    next.interval=read(L"SkinMaskProbeIntervalMs",250,100,5000);
    next.autoStart=read(L"SkinMaskProbeAutoStartSec",0,0,300);
    next.maxSessions=read(L"SkinMaskProbeMaxSessions",3,0,4096);
    next.continuous=GetPrivateProfileIntW(L"Dx11FsrBridge",L"SkinMaskProbeContinuous",0,iniPath)!=0;
    // An explicit SkinMaskProbeQuiet wins in both directions; when the key is absent the
    // default stays "quiet while continuous" (a diagnostic run sets Quiet=0 to get PNGs).
    const int quietSetting=GetPrivateProfileIntW(L"Dx11FsrBridge",L"SkinMaskProbeQuiet",-1,iniPath);
    next.quiet=quietSetting>=0?quietSetting!=0:next.continuous;
    next.fastpath=GetPrivateProfileIntW(L"Dx11FsrBridge",L"SkinMaskProbeFastPath",1,iniPath)!=0;
    next.fastpathStride=read(L"SkinMaskProbeFastPathStride",2,1,60);
    wchar_t path[2048]{};GetPrivateProfileStringW(L"Dx11FsrBridge",L"SkinMaskProbeDir",L"",path,2048,iniPath);
    next.directory=path[0]?std::filesystem::path(path):std::filesystem::path(iniPath).parent_path()/L"fsr2dump"/L"skinmask";
    if(next.directory.is_relative())next.directory=std::filesystem::path(iniPath).parent_path()/next.directory;
    wchar_t ids[1024]{};GetPrivateProfileStringW(L"Dx11FsrBridge",L"SkinMaskProbeStencilValues",L"",ids,1024,iniPath);
    bool valid=false;next.stencilIds=parse_stencil_ids(ids,&valid);
    next.selected=std::any_of(next.stencilIds.begin(),next.stencilIds.end(),[](auto x){return x!=0;});
    if(!valid){next.enabled=false;LOG_WARN(blog::cat::core,"skin_probe invalid stencil ID list; disabled");}
    wchar_t normalIds[1024]{};GetPrivateProfileStringW(L"Dx11FsrBridge",L"SkinMaskProbeGBufferStencilValues",L"",normalIds,1024,iniPath);
    bool normalValid=false;next.gbufferStencilIds=parse_stencil_ids(normalIds,&normalValid);
    next.gbufferSelected=std::any_of(next.gbufferStencilIds.begin(),next.gbufferStencilIds.end(),[](auto x){return x!=0;});
    if(!normalValid){next.enabled=false;LOG_WARN(blog::cat::core,"skin_probe invalid G-buffer stencil selector; disabled");}
    settings=next;active.store(next.enabled,std::memory_order_release);gbufferActive.store(next.enabled&&next.gbuffer);freshActive.store(next.enabled&&next.gbuffer&&next.gbufferFresh);
    gpuActive.store(next.enabled&&next.gbuffer&&next.gbufferFresh&&next.gpu);
    faceActive.store(next.enabled&&next.gbuffer&&next.gbufferFresh&&next.gpu&&next.faceOnly);
    pairRequested.store(false);bridgeBoundary.store(0);lastBridgeInstance.store(0);freshWaitStart=0;
    remaining=sessions=0;startTick=nextCapture=0;hotkeyDown=autoFired=false;pending.reset();
    quietDumps.store(next.quiet,std::memory_order_relaxed);
    fastActive.store(next.enabled&&next.fastpath,std::memory_order_release);
    fastCarrierHash.store(0,std::memory_order_relaxed);
    fastFrame=fastLastPublish=fastLastGap=fastLastBytes=fastWindowStart=0;
    fastPublished=fastStalls=fastSkips=fastQueued=0;fastPairArmed=false;
    for(auto& slot:fastSlots){slot.busy=false;slot.context.Reset();slot.ready.Reset();slot.staging.Reset();slot.width=slot.height=0;}
    fastStatus="not-started";
    drawBudget.store(0);
    {std::lock_guard drawLock(drawMutex);drawRecords.clear();droppedDraws=0;geometryDepth.Reset();geometryContext.Reset();geometryBeforeClear.Reset();geometrySnapshotTick=geometrySnapshotKey=0;geometryNormal.Reset();geometryNormalBeforeClear.Reset();normalPairedDepthBeforeClear.Reset();normalContext.Reset();normalDepthView.Reset();normalDepthKey=normalPixelHash=normalSnapshotTick=normalSnapshotKey=normalSnapshotPixelHash=0;normalSnapshotBeforeColor=normalDirty=false;normalDecodeFormat=normalSnapshotDecodeFormat=normalSnapshotResourceFormat=normalSnapshotPhase=0;normalSnapshotDepthKey=normalSnapshotBoundary=normalSnapshotInstance=0;normalAdmissions.clear();reset_face_locked();faceWarmed.clear();faceWarmupBoundary=0;}
    if(!next.enabled||!next.faceOnly)faceinterop::invalidate();
    if(next.enabled)LOG_INFO(blog::cat::core,"skin_probe enabled: F8/configured hotkey captures bounded pre-NR color and stencil; no NR parameter changes");
    if(next.enabled&&next.continuous)LOG_INFO(blog::cat::core,"skin_probe continuous capture enabled: interval="+std::to_string(next.interval)+"ms frames="+std::to_string(next.frames)+" quietDumps="+(next.quiet?"1":"0")+" sessionLimit=none");
    if(next.enabled&&next.fastpath)LOG_INFO(blog::cat::core,"skin_probe fastpath enabled: publish every "+std::to_string(next.fastpathStride)+" frame(s) source="+(next.faceOnly?"verified-face-delta":"gbuffer-material")+" readback=guide-only-pool"+std::to_string(kFastSlots)+" pair=same-bridge-interval quietDumps="+(next.quiet?"1":"0"));
    if(next.enabled&&next.fastpath&&(!next.gbuffer||!next.gbufferFresh))
        LOG_WARN(blog::cat::core,"skin_probe fastpath needs SkinMaskProbeGBuffer=1 and SkinMaskProbeGBufferFresh=1: no same-interval pair exists, no guide will be published");
    if(next.enabled&&next.fastpath&&next.faceOnly&&!next.gpu)
        LOG_WARN(blog::cat::core,"skin_probe fastpath needs SkinMaskProbeGBufferGPU=1 with FaceOnly=1: the verified face pipeline is off, no face-only guide will be published");
}
bool wants_draw_trace() {
    auto budget=drawBudget.load(std::memory_order_relaxed);
    while(budget && !drawBudget.compare_exchange_weak(budget,budget-1,std::memory_order_relaxed)){}
    return budget!=0;
}
// The bounded diagnostic trace and the per-frame fast path both need the MRT/stencil
// observation hooks armed; only the trace consumes drawBudget.
bool observe_active() {
    return drawBudget.load(std::memory_order_relaxed)!=0||fastActive.load(std::memory_order_relaxed);
}
bool geometry_extent(uint32_t width,uint32_t height,uint32_t elements) {
    return width && height && width==renderHintWidth.load(std::memory_order_relaxed) &&
        height==renderHintHeight.load(std::memory_order_relaxed) && elements>6;
}
void remember_geometry_depth(ID3D11DeviceContext* context,ID3D11DepthStencilView* view) {
    if(!context||!view)return;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if(!resource||FAILED(resource.As(&texture)))return;
    D3D11_DEPTH_STENCIL_VIEW_DESC vd;view->GetDesc(&vd);
    D3D11_TEXTURE2D_DESC td;texture->GetDesc(&td);
    if(vd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice!=0 ||
        td.SampleDesc.Count!=1 || td.Width!=renderHintWidth.load() || td.Height!=renderHintHeight.load())return;
    std::lock_guard lock(drawMutex);geometryDepth=texture;geometryContext=context;
}
TargetIdentity describe_render_target(ID3D11RenderTargetView* view) {
    TargetIdentity out;if(!view)return out;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;
    if(!resource||FAILED(resource.As(&texture)))return out;
    D3D11_RENDER_TARGET_VIEW_DESC vd{};view->GetDesc(&vd);D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
    out.resourceFormat=td.Format;out.viewFormat=vd.Format;out.viewDimension=vd.ViewDimension;
    out.mipLevels=td.MipLevels;out.arraySize=td.ArraySize;out.samples=td.SampleDesc.Count;out.misc=td.MiscFlags;
    if(vd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D)out.mipSlice=vd.Texture2D.MipSlice;
    return out;
}
void remember_geometry_target(ID3D11DeviceContext* context,ID3D11RenderTargetView* view,
                              ID3D11DepthStencilView* depth,uint64_t pixelHash) {
    remember_geometry_target(context,view,depth,pixelHash,UINT32_MAX);
}
void remember_geometry_target(ID3D11DeviceContext* context,ID3D11RenderTargetView* view,
                              ID3D11DepthStencilView* depth,uint64_t pixelHash,uint32_t stencilRef) try {
    if(!gbufferActive.load()||!observe_active()||!context||!view||!depth||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    skinfamily::Result family;
    if(alpha_carrier_shader(pixelHash))family={pixelHash==0xb492850d11d32059ull?skinfamily::Kind::ConstantFace:skinfamily::Kind::Material2,0,0,0,0,"previously-verified-original-shader-hash"};
    else {
        if(!skinmaterial::family_enabled())return;
        family=skinmaterial::alpha_family(pixelHash,stencilRef,true);
    }
    ComPtr<ID3D11Resource> resource,ds;view->GetResource(&resource);depth->GetResource(&ds);
    ComPtr<ID3D11Texture2D> texture;if(!resource||!ds||FAILED(resource.As(&texture)))return;
    const auto target=describe_render_target(view);D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
    const auto decode=typed_gbuffer_format(target.resourceFormat,target.viewFormat);
    uint32_t reject=0;
    if(!skinfamily::accepted(family.kind))reject=family.kind==skinfamily::Kind::Pending?7:6;
    else if(target.viewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||target.mipSlice!=0)reject=1;
    else if(td.ArraySize!=1||td.SampleDesc.Count!=1)reject=2;
    else if(!decode)reject=3;
    else if(td.Width!=renderHintWidth.load()||td.Height!=renderHintHeight.load())reject=4;
    else if(td.MiscFlags&(D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX|D3D11_RESOURCE_MISC_SHARED_NTHANDLE))reject=5;
    std::lock_guard lock(drawMutex);
    const auto rt=reinterpret_cast<uint64_t>(texture.Get());bool seen=false;
    for(auto& a:normalAdmissions)if(a.pixelHash==pixelHash&&a.key==rt&&a.target==target) {
        if(a.family.kind!=family.kind){a.family=family;a.rejection=reject;}
        seen=true;break;
    }
    if(!seen&&normalAdmissions.size()<128) {
        normalAdmissions.push_back({pixelHash,rt,target,decode,reject,family});
        LOG_INFO(blog::cat::core,"skin_probe MRT admission ps="+std::to_string(pixelHash)+" resource_format="+std::to_string(td.Format)+
            " view_format="+std::to_string(target.viewFormat)+" dimension="+std::to_string(target.viewDimension)+" mips="+std::to_string(td.MipLevels)+
            " array="+std::to_string(td.ArraySize)+" samples="+std::to_string(td.SampleDesc.Count)+" reason="+admission_reason(reject));
    }
    if(reject)return;
    const auto key=reinterpret_cast<uint64_t>(ds.Get());
    if(geometryNormal.Get()!=texture.Get()||normalContext.Get()!=context||normalDepthKey!=key||normalDecodeFormat!=decode) {
        geometryNormalBeforeClear.Reset();normalPairedDepthBeforeClear.Reset();normalSnapshotTick=normalSnapshotKey=normalSnapshotDepthKey=0;normalSnapshotPhase=0;
    }
    geometryNormal=texture;normalContext=context;normalDepthView=depth;normalDepthKey=key;normalPixelHash=pixelHash;normalDecodeFormat=decode;normalDirty=true;
    fastCarrierHash.store(pixelHash,std::memory_order_relaxed);
} catch(...) {LOG_WARN(blog::cat::core,"skin_probe original MRT observation skipped");}
FaceDrawScope::FaceDrawScope(ID3D11DeviceContext* c) noexcept:context(c) {
    if(!faceActive.load())return;
    previousToken=faceScopeToken;token=++faceScopeSerial;faceScopeToken=token;
    // A reentrant draw must not broaden the 'before/after native face draw'
    // interval to unrelated geometry. Reject the span rather than guessing.
    if(facePrepared.load())try {std::lock_guard lock(drawMutex);facePrepared=false;faceStatus="nested-face-draw-span-refused";}catch(...) {facePrepared=false;}
}
FaceDrawScope::~FaceDrawScope() noexcept {
    if(!token)return;finish_face_draw(context,token);faceScopeToken=previousToken;
}
void prepare_face_draw(ID3D11DeviceContext* context,ID3D11RenderTargetView* view,
                       ID3D11DepthStencilView* depth,const DrawIdentity& identity) try {
    if(!faceActive.load()||!pairRequested.load()||!context||!view||!depth||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||
       identity.target.mrtCount!=6||!identity.stencilEnabled||identity.stencilRef>255)return;
    auto family=identity.pixelHash==0xb492850d11d32059ull?
        skinfamily::Result{skinfamily::Kind::ConstantFace,0,0,0,0,"previously-verified-face-shader"}:
        skinmaterial::alpha_family(identity.pixelHash,identity.stencilRef,false);
    if(family.kind!=skinfamily::Kind::ConstantFace)return;
    ComPtr<ID3D11Resource> resource,ds;view->GetResource(&resource);depth->GetResource(&ds);
    ComPtr<ID3D11Texture2D> texture;if(!resource||!ds||FAILED(resource.As(&texture)))return;
    D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);D3D11_RENDER_TARGET_VIEW_DESC vd{};view->GetDesc(&vd);
    if(vd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||vd.Texture2D.MipSlice!=0||td.ArraySize!=1||td.SampleDesc.Count!=1||
       td.Width!=renderHintWidth.load()||td.Height!=renderHintHeight.load()||typed_gbuffer_format(td.Format,vd.Format)!=24||
       uint64_t(td.Width)*td.Height*28>128ull*1024*1024||
       (td.MiscFlags&(D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX|D3D11_RESOURCE_MISC_SHARED_NTHANDLE)))return;
    std::lock_guard lock(drawMutex);
    if(settings.gbufferSelected&&!settings.gbufferStencilIds[identity.stencilRef])return;
    const auto epoch=bridgeBoundary.load(),instance=lastBridgeInstance.load();const auto rt=reinterpret_cast<uint64_t>(texture.Get()),dsKey=reinterpret_cast<uint64_t>(ds.Get());
    bool warmed=false;
    for(const auto& w:faceWarmed)if(w.ps==identity.pixelHash&&w.rt==rt&&w.ds==dsKey&&w.instance==instance){warmed=true;break;}
    if(!warmed) {
        if(faceWarmed.size()>=64){faceStatus="face-warmup-cache-budget-exhausted";return;}
        faceWarmed.push_back({identity.pixelHash,rt,dsKey,instance});faceWarmupBoundary=epoch;
        reset_face_locked();faceStatus="new-face-family-warming-one-bridge-interval";
    }
    // A background classifier can become ready halfway through a native face
    // pass. Skip that ENTIRE interval, including any already accumulated parts.
    // Begin observation on the next complete interval instead of saving6pixels.
    if(faceWarmupBoundary==epoch)return;
    if(faceCopyWidth!=td.Width||faceCopyHeight!=td.Height) {
        // Extent change: the private copy/accumulator textures must match the new target.
        reset_face_locked();faceCopyWidth=td.Width;faceCopyHeight=td.Height;
    }
    if(faceBoundary!=epoch||faceInstance!=instance||faceTargetKey!=rt||faceDepthKey!=dsKey||faceContext.Get()!=context) {
        // A new bridge interval only needs the coverage mask cleared. The private textures stay
        // allocated: the per-frame fast path publishes every stride frames and must not
        // reallocate tens of MB of accumulator per interval.
        faceBoundary=epoch;faceInstance=instance;faceTargetKey=rt;faceDepthKey=dsKey;faceContext=context;faceTarget=texture;
        faceDraws=0;facePrepared=false;
        if(faceAccum.coverage) {
            ComPtr<ID3D11Device> device;context->GetDevice(&device);ComPtr<ID3D11UnorderedAccessView> coverage;
            if(device&&SUCCEEDED(device->CreateUnorderedAccessView(faceAccum.coverage.Get(),nullptr,&coverage))) {
                const UINT zero[4]{};context->ClearUnorderedAccessViewUint(coverage.Get(),zero);
            }
        }
    }
    facePrepared=false;if(faceDraws>=8){faceStatus="face-draw-budget-exhausted";return;}
    if(!faceBefore||!faceAfter) {
        ComPtr<ID3D11Device> device;context->GetDevice(&device);ComPtr<ID3D11On12Device> on12;if(SUCCEEDED(device.As(&on12)))return;
        td.MipLevels=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.CPUAccessFlags=td.MiscFlags=0;
        if(FAILED(device->CreateTexture2D(&td,nullptr,&faceBefore))||FAILED(device->CreateTexture2D(&td,nullptr,&faceAfter))){faceStatus="face-copy-creation-failed";return;}
    }
    context->CopySubresourceRegion(faceBefore.Get(),0,0,0,0,texture.Get(),0,nullptr);
    faceShaderHash=identity.pixelHash;preparedFaceToken=faceScopeToken;facePrepared=true;faceStatus="before-native-face-draw";
} catch(...) {facePrepared=false;}
void finish_face_draw(ID3D11DeviceContext* context,uint64_t token) noexcept {
    if(!faceActive.load()||!facePrepared.load())return;
    try {
        std::lock_guard lock(drawMutex);
        if(!facePrepared||faceContext.Get()!=context||preparedFaceToken!=token)return;facePrepared=false;
        if(faceBoundary!=bridgeBoundary.load()||faceInstance!=lastBridgeInstance.load()||!faceTarget||!faceBefore||!faceAfter){faceStatus="face-draw-boundary-changed";return;}
        context->CopySubresourceRegion(faceAfter.Get(),0,0,0,0,faceTarget.Get(),0,nullptr);
        if(skinguide::accumulate_face_delta(context,faceBefore.Get(),faceAfter.Get(),24,faceAccum,&faceStatus))++faceDraws;
    } catch(...) {facePrepared=false;faceStatus="face-draw-capture-exception";}
}
static bool capture_normal_pair_locked(ID3D11DeviceContext* context,uint32_t phase) {
    if(!gbufferActive.load()||!normalDirty||!geometryNormal||!normalDepthView||normalContext.Get()!=context)return false;
    if(faceActive.load()&&(faceWarmupBoundary==bridgeBoundary.load()||!faceAccum.coverage||!faceAccum.reference||!faceDraws||faceContext.Get()!=context||
        faceBoundary!=bridgeBoundary.load()||faceInstance!=lastBridgeInstance.load()||faceTargetKey!=reinterpret_cast<uint64_t>(geometryNormal.Get())||faceDepthKey!=normalDepthKey))return false;
    const bool fresh=freshActive.load();
    if(fresh&&!pairRequested.load())return false;
    const uint64_t now=GetTickCount64();
    if(!fresh&&normalSnapshotTick&&now-normalSnapshotTick<100)return false;
    ComPtr<ID3D11Resource> ds;normalDepthView->GetResource(&ds);ComPtr<ID3D11Texture2D> depth;
    if(!ds||FAILED(ds.As(&depth))||reinterpret_cast<uint64_t>(depth.Get())!=normalDepthKey)return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};normalDepthView->GetDesc(&dv);D3D11_TEXTURE2D_DESC dd{},nd{};depth->GetDesc(&dd);geometryNormal->GetDesc(&nd);
    if(dv.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D||dv.Texture2D.MipSlice!=0||dd.MipLevels!=1||dd.ArraySize!=1||dd.SampleDesc.Count!=1||
       dd.Width!=nd.Width||dd.Height!=nd.Height||!valid_extent(dd.Width,dd.Height,bytes_per_pixel(dd.Format),dd.Width*bytes_per_pixel(dd.Format))||
       !valid_extent(nd.Width,nd.Height,gbuffer_bytes_per_pixel(normalDecodeFormat),nd.Width*gbuffer_bytes_per_pixel(normalDecodeFormat)))return false;
    ComPtr<ID3D11Device> device;context->GetDevice(&device);ComPtr<ID3D11On12Device> on12;if(SUCCEEDED(device.As(&on12)))return false;
    if(!geometryNormalBeforeClear) {
        auto copy=nd;copy.MipLevels=1;copy.Usage=D3D11_USAGE_DEFAULT;copy.BindFlags=gpuActive.load()?D3D11_BIND_SHADER_RESOURCE:0;copy.CPUAccessFlags=copy.MiscFlags=0;
        if(FAILED(device->CreateTexture2D(&copy,nullptr,&geometryNormalBeforeClear)))return false;
    }
    if(!normalPairedDepthBeforeClear) {
        auto copy=dd;copy.Usage=D3D11_USAGE_DEFAULT;copy.BindFlags=gpuActive.load()?D3D11_BIND_SHADER_RESOURCE:0;copy.CPUAccessFlags=copy.MiscFlags=0;
        if(FAILED(device->CreateTexture2D(&copy,nullptr,&normalPairedDepthBeforeClear)))return false;
    }
    // Original MRT mip0 + its own DSV captured together, independent from a
    // later unrelated geometry/depth pass. No state rewrites or GPU waits.
    context->CopySubresourceRegion(geometryNormalBeforeClear.Get(),0,0,0,0,geometryNormal.Get(),0,nullptr);
    context->CopyResource(normalPairedDepthBeforeClear.Get(),depth.Get());
    normalSnapshotTick=now;normalSnapshotKey=reinterpret_cast<uint64_t>(geometryNormal.Get());normalSnapshotDepthKey=normalDepthKey;
    normalSnapshotPixelHash=normalPixelHash;normalSnapshotDecodeFormat=normalDecodeFormat;normalSnapshotResourceFormat=nd.Format;normalSnapshotPhase=phase;
    normalSnapshotBeforeColor=phase==2;normalDirty=false;
    normalSnapshotBoundary=bridgeBoundary.load();normalSnapshotInstance=lastBridgeInstance.load();
    if(fresh)pairRequested.store(false);
    LOG_INFO(blog::cat::core,"skin_probe paired MRT0 snapshot phase="+std::to_string(phase)+" resource_format="+std::to_string(nd.Format)+
        " decode_format="+std::to_string(normalDecodeFormat)+" depth="+std::to_string(normalSnapshotDepthKey));
    return true;
}
static void capture_before_geometry_clear(ID3D11DeviceContext* context,ID3D11DepthStencilView* view) try {
    if(!observe_active()||!context||!view||
       context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    std::lock_guard lock(drawMutex);
    if(!geometryDepth || geometryContext.Get()!=context)return;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    if(resource.Get()!=geometryDepth.Get())return;
    const uint64_t now=GetTickCount64();
    if(geometrySnapshotTick && now-geometrySnapshotTick<100)return; // bounded diagnostic copies
    D3D11_TEXTURE2D_DESC desc;geometryDepth->GetDesc(&desc);
    if(desc.SampleDesc.Count!=1 || desc.ArraySize!=1 || !bytes_per_pixel(desc.Format) ||
       !valid_extent(desc.Width,desc.Height,bytes_per_pixel(desc.Format),desc.Width*bytes_per_pixel(desc.Format)))return;
    if(geometryBeforeClear) {
        D3D11_TEXTURE2D_DESC old;geometryBeforeClear->GetDesc(&old);
        if(old.Width!=desc.Width || old.Height!=desc.Height || old.Format!=desc.Format)geometryBeforeClear.Reset();
    }
    if(!geometryBeforeClear) {
        ComPtr<ID3D11Device> device;context->GetDevice(&device);
        ComPtr<ID3D11On12Device> on12;if(SUCCEEDED(device.As(&on12)))return;
        desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=0;desc.CPUAccessFlags=0;desc.MiscFlags=0;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&geometryBeforeClear)))return;
    }
    context->CopyResource(geometryBeforeClear.Get(),geometryDepth.Get());
    LOG_INFO(blog::cat::core,"skin_probe material stencil snapshot before ClearDepthStencilView key="+
        std::to_string(reinterpret_cast<uint64_t>(geometryDepth.Get()))+" size="+std::to_string(desc.Width)+"x"+std::to_string(desc.Height));
    geometrySnapshotTick=now;geometrySnapshotKey=reinterpret_cast<uint64_t>(geometryDepth.Get());
} catch(...) {LOG_WARN(blog::cat::core,"skin_probe pre-clear snapshot skipped");}
void before_stencil_clear(ID3D11DeviceContext* context,ID3D11DepthStencilView* view) try {
    if(gbufferActive.load()&&observe_active()&&context&&view) {
        std::lock_guard lock(drawMutex);ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
        if(resource&&reinterpret_cast<uint64_t>(resource.Get())==normalDepthKey){capture_normal_pair_locked(context,1);normalDirty=false;}
    }
    capture_before_geometry_clear(context,view);
} catch(...) {LOG_WARN(blog::cat::core,"skin_probe paired stencil snapshot skipped");}
bool gbuffer_enabled(){return gbufferActive.load();}
void before_geometry_color_clear(ID3D11DeviceContext* context,ID3D11RenderTargetView* view) try {
    if(!gbufferActive.load()||!observe_active()||!context||!view||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    std::lock_guard lock(drawMutex);if(!geometryNormal||normalContext.Get()!=context)return;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);if(resource.Get()!=geometryNormal.Get())return;
    capture_normal_pair_locked(context,2);normalDirty=false;
} catch(...) {LOG_WARN(blog::cat::core,"skin_probe pre-RTV snapshot skipped");}
void before_geometry_targets_change(ID3D11DeviceContext* context,uint32_t count,
    ID3D11RenderTargetView* const* views,ID3D11DepthStencilView* depth) try {
    if(!gbufferActive.load()||!observe_active()||!context||count>8||(count&&!views)||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    std::lock_guard lock(drawMutex);if(!normalDirty||!geometryNormal||normalContext.Get()!=context)return;
    ComPtr<ID3D11Resource> nextRT,nextDepth;
    if(count&&views[0])views[0]->GetResource(&nextRT);if(depth)depth->GetResource(&nextDepth);
    // KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL is >8 and was rejected above.
    // Rebinding the same G-buffer is not a pass boundary.
    if(count>=3&&nextRT.Get()==geometryNormal.Get()&&reinterpret_cast<uint64_t>(nextDepth.Get())==normalDepthKey)return;
    capture_normal_pair_locked(context,3);normalDirty=false;
} catch(...) {LOG_WARN(blog::cat::core,"skin_probe end-MRT-pass snapshot skipped");}
void record_draw(const DrawIdentity& identity,uint32_t elements,bool indexed) {
    std::lock_guard lock(drawMutex);
    for(auto& d:drawRecords)if(d.identity==identity) {++d.calls;d.elements+=elements;return;}
    if(drawRecords.size()>=kUniqueDrawLimit){++droppedDraws;return;}
    drawRecords.push_back({identity,1,elements,indexed});
}
bool enabled(){return active.load(std::memory_order_acquire);}
void on_frame(const Frame& frame) try {
    skinmaterial::poll(frame.context);
    if(!enabled()||!frame.context||!frame.color||!frame.width||!frame.height)return;
    if(frame.context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    std::lock_guard lock(mutex);
    const uint64_t currentBoundary=bridgeBoundary.fetch_add(1)+1;lastBridgeInstance.store(frame.instance);
    renderHintWidth.store(frame.width,std::memory_order_relaxed);renderHintHeight.store(frame.height,std::memory_order_relaxed);
    const auto now=GetTickCount64();
    if(!startTick) {
        startTick=now;
        LOG_INFO(blog::cat::core,"skin_probe dispatch ready: render="+std::to_string(frame.width)+"x"+std::to_string(frame.height)+" hotkey="+std::to_string(settings.hotkey)+" consumer=none");
    }
    if(pending && frame.context==pending->context.Get()) {
        if(now-pending->queued>2000) {
            LOG_WARN(blog::cat::core,"skin_probe capture expired: no blocking readback attempted");pending.reset();
        } else {
            BOOL done=FALSE;
            const HRESULT ready=pending->context->GetData(pending->ready.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(ready==S_OK&&done&&!writerBusy.load()) {
                auto job=std::make_shared<Job>();job->settings=settings;
                const bool gotColor=read_image(pending->context.Get(),pending->color.Get(),job->color);
                const bool gotDepth=read_image(pending->context.Get(),pending->depth.Get(),job->depth);
                const bool gotFaceCoverage=read_image(pending->context.Get(),pending->faceCoverage.Get(),job->faceCoverage);
                const bool gotFaceReference=read_image(pending->context.Get(),pending->faceReference.Get(),job->faceReference);
                const bool gotGpu=read_image(pending->context.Get(),pending->gpu.Get(),job->gpu);
                const bool gotNormal=read_image(pending->context.Get(),pending->normal.Get(),job->normal,pending->normalDecodeFormat);
                const bool gotMaterial=read_image(pending->context.Get(),pending->materialDepth.Get(),job->materialDepth);
                if(gotColor&&(!pending->depth||gotDepth)&&(!pending->materialDepth||gotMaterial)&&(!pending->normal||gotNormal)&&(!pending->gpu||gotGpu)&&(!pending->faceCoverage||gotFaceCoverage)&&(!pending->faceReference||gotFaceReference)) {
                    job->metadata=*pending;
                    // Background work owns CPU data only; never call D3D on its thread.
                    job->metadata.color.Reset();job->metadata.depth.Reset();job->metadata.materialDepth.Reset();job->metadata.normal.Reset();job->metadata.gpu.Reset();job->metadata.faceCoverage.Reset();job->metadata.faceReference.Reset();job->metadata.context.Reset();job->metadata.ready.Reset();
                    pending.reset();writerBusy.store(true);
                    HMODULE pinned=nullptr;
                    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                        reinterpret_cast<LPCWSTR>(&on_frame),&pinned)) {
                        writerBusy.store(false);LOG_WARN(blog::cat::core,"skin_probe worker refused: module pin failed");
                    } else {
                        try {
                            std::thread([job]{
                                try {analyze(*job);}
                                catch(...) {LOG_WARN(blog::cat::core,"skin_probe analysis failed; diagnostic output skipped");}
                                writerBusy.store(false);
                            }).detach();
                        } catch(...) {writerBusy.store(false);LOG_WARN(blog::cat::core,"skin_probe writer could not start");}
                    }
                }
            } else if(FAILED(ready))pending.reset();
        }
    }
    // --- per-frame guide fast path ------------------------------------------------------
    // Publishes the same-interval guide every SkinMaskProbeFastPathStride frames from the
    // GPU mask the probe already computes. It never waits for a paired MRT, never reads back
    // the diagnostic textures and never runs on a worker: a slot the GPU has not finished is
    // retried on a later frame, so the render thread is never blocked by a readback.
    if(fastActive.load(std::memory_order_relaxed)) {
        ++fastFrame;
        for(auto& slot:fastSlots) {
            if(!slot.busy)continue;
            if(!slot.context)continue;
            if(slot.context.Get()!=frame.context){slot.busy=false;slot.context.Reset();continue;}
            BOOL done=FALSE;Image image;
            const HRESULT ready=slot.context->GetData(slot.ready.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(ready==S_OK&&done&&read_image(slot.context.Get(),slot.staging.Get(),image)&&image.format==61&&
               image.width==slot.width&&image.height==slot.height) {
                HYSK_FACE_GUIDE_INFO info{};info.struct_size=sizeof(info);info.width=slot.width;info.height=slot.height;
                info.row_pitch=image.pitch;info.format=image.format;info.flags=1u|2u;info.instance=slot.instance;
                info.capture_boundary=slot.captureBoundary;info.snapshot_boundary=slot.snapshotBoundary;
                faceinterop::publish(info,image.bytes.data(),image.bytes.size());
                ++fastPublished;fastLastBytes=image.bytes.size();
                if(fastLastPublish)fastLastGap=now-fastLastPublish;fastLastPublish=now;
                slot.busy=false;slot.context.Reset();
            } else if(ready!=S_OK&&FAILED(ready)) {slot.busy=false;slot.context.Reset();}
            else ++fastStalls;
        }
        if(!fastWindowStart)fastWindowStart=now;
        if(now-fastWindowStart>=1000) {
            uint32_t inFlight=0;for(const auto& slot:fastSlots)if(slot.busy)++inFlight;
            LOG_INFO(blog::cat::core,"skin_probe fastpath publish rate="+std::to_string(fastPublished)+"/s lastGapMs="+std::to_string(fastLastGap)+
                " bytes="+std::to_string(fastLastBytes)+" queued="+std::to_string(fastQueued)+" skipped="+std::to_string(fastSkips)+
                " readbackNotReady="+std::to_string(fastStalls)+" inFlight="+std::to_string(inFlight)+" status="+std::string(fastStatus));
            fastPublished=fastStalls=fastSkips=fastQueued=0;fastWindowStart=now;
        }
        const auto stride=settings.fastpathStride?settings.fastpathStride:1;
        bool freeSlot=false;for(const auto& slot:fastSlots)if(!slot.busy)freeSlot=true;
        if(!fastPairArmed&&freeSlot&&fastFrame%stride==0) {
            // Arm the observation for the interval that starts after this dispatch.
            pairRequested.store(true,std::memory_order_relaxed);
            fastPairArmed=true;
        }
        if(fastPairArmed) {
            std::lock_guard drawLock(drawMutex);
            const bool faceOnly=settings.faceOnly,face=faceOnly&&faceActive.load();
            // A face-only consumer must never receive an unverified material candidate: with
            // FaceOnly=1 the guide is published only when the verified face pipeline is live.
            const bool sourceReady=(!faceOnly||faceActive.load())&&geometryNormalBeforeClear&&normalPairedDepthBeforeClear&&
                normalContext.Get()==frame.context&&
                normalSnapshotPhase&&same_bridge_interval(normalSnapshotBoundary,currentBoundary,normalSnapshotInstance,frame.instance);
            const bool faceReady=!face||(faceAccum.coverage&&faceAccum.reference&&faceDraws&&faceBoundary==normalSnapshotBoundary&&
                faceInstance==normalSnapshotInstance&&faceTargetKey==normalSnapshotKey&&faceDepthKey==normalSnapshotDepthKey);
            if(sourceReady&&faceReady) {
                ComPtr<ID3D11Device> device;frame.context->GetDevice(&device);
                FastSlot* target=nullptr;for(auto& slot:fastSlots)if(!slot.busy){target=&slot;break;}
                if(device&&target) {
                    skinguide::Candidate guide;bool generated=false;
                    if(face)generated=skinguide::generate_face(frame.context,geometryNormalBeforeClear.Get(),normalSnapshotDecodeFormat,
                        normalPairedDepthBeforeClear.Get(),settings.gbufferStencilIds,settings.gbufferSelected,faceAccum,guide,&fastStatus);
                    else generated=skinguide::generate(frame.context,geometryNormalBeforeClear.Get(),normalSnapshotDecodeFormat,
                        normalPairedDepthBeforeClear.Get(),settings.gbufferStencilIds,settings.gbufferSelected,guide,&fastStatus);
                    if(generated&&queue_guide_staging(device.Get(),frame.context,guide.texture.Get(),frame.width,frame.height,target->staging)) {
                        D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
                        if(SUCCEEDED(device->CreateQuery(&query,&target->ready))) {
                            frame.context->End(target->ready.Get());
                            target->context=frame.context;target->width=frame.width;target->height=frame.height;
                            target->instance=frame.instance;target->captureBoundary=currentBoundary;
                            target->snapshotBoundary=normalSnapshotBoundary;target->busy=true;++fastQueued;
                        }
                    }
                }
                fastPairArmed=false;
            } else ++fastSkips;
        }
    }
    const bool key=settings.hotkey&&(GetAsyncKeyState(int(settings.hotkey))&0x8000)!=0;
    const bool edge=key&&!hotkeyDown;hotkeyDown=key;
    bool timer=settings.autoStart&&!autoFired&&now-startTick>=uint64_t(settings.autoStart)*1000;
    if(timer)autoFired=true;
    // Continuous mode re-arms captures for as long as the probe is enabled, so a consumer gets a
    // live face guide instead of one frozen at the last session. The bounded behaviour (capture on
    // hotkey/timer only, up to SkinMaskProbeMaxSessions per process) is unchanged when it is off.
    const bool limited=!settings.continuous&&settings.maxSessions!=0;
    const bool capped=limited&&sessions>=settings.maxSessions;
    const bool idle=!remaining&&!pending;
    const bool repeat=settings.continuous&&idle&&now>=nextCapture&&!writerBusy.load();
    if(capped&&edge)
        LOG_WARN(blog::cat::core,"skin_probe F8 session limit reached="+std::to_string(settings.maxSessions)+"; no new files captured");
    if(idle&&(edge||timer||repeat)&&!capped) {
        remaining=settings.frames;++sessions;nextCapture=now;
        skinmaterial::begin_session(sessions,frame.instance,frame.frame);
        {std::lock_guard drawLock(drawMutex);drawRecords.clear();droppedDraws=0;normalAdmissions.clear();normalDirty=false;normalSnapshotTick=normalSnapshotKey=normalSnapshotDepthKey=0;normalSnapshotPhase=0;
         // The diagnostic capture must not destroy the face accumulator the per-frame fast
         // path is publishing from.
         if(!fastActive.load(std::memory_order_relaxed))reset_face_locked();}
        drawBudget.store(kDrawQueryBudget,std::memory_order_relaxed);
        LOG_INFO(blog::cat::core,"skin_probe capture session="+std::to_string(sessions));
    }
    if(!remaining||pending||writerBusy.load()||now<nextCapture)return;
    const bool fresh=settings.gbuffer&&settings.gbufferFresh;
    if(fresh) {
        bool usable=false;
        {
            std::lock_guard drawLock(drawMutex);
            usable=geometryNormalBeforeClear&&normalPairedDepthBeforeClear&&normalContext.Get()==frame.context&&
                normalSnapshotPhase&&same_bridge_interval(normalSnapshotBoundary,currentBoundary,normalSnapshotInstance,frame.instance);
        }
        if(!usable) {
            if(!freshWaitStart)freshWaitStart=now;
            if(now-freshWaitStart<2000) {
                // Request ONE paired snapshot in the immediately following
                // geometry interval; do not copy every rendered frame.
                pairRequested.store(true);drawBudget.store(kDrawQueryBudget);
                return;
            }
            if(!fastActive.load(std::memory_order_relaxed))pairRequested.store(false);
            LOG_WARN(blog::cat::core,"skin_probe fresh MRT unavailable after bounded wait; exporting admission metadata without reusing stale skin pixels");
        }
    }
    ComPtr<ID3D11Device> device;frame.context->GetDevice(&device);if(!device)return;
    ComPtr<ID3D11On12Device> on12;
    if(SUCCEEDED(device.As(&on12))) {
        active.store(false);LOG_WARN(blog::cat::core,"skin_probe refused D3D11On12: no CPU readback on shared-queue path");return;
    }
    auto p=std::make_unique<Pending>();p->context=frame.context;p->queued=now;p->frame=frame.frame;
    p->instance=frame.instance;p->sequence=++sequence;p->width=frame.width;p->height=frame.height;
    p->outW=frame.outputWidth;p->outH=frame.outputHeight;p->captureBoundary=currentBoundary;p->normalFreshRequested=fresh;p->faceOnly=faceActive.load();
    p->jitterX=std::isfinite(frame.jitterX)?frame.jitterX:0;
    p->jitterY=std::isfinite(frame.jitterY)?frame.jitterY:0;
    p->linear=frame.linear;p->pq=frame.pq;
    D3D11_TEXTURE2D_DESC colorDescription;frame.color->GetDesc(&colorDescription);
    const auto fmt=colorDescription.Format;
    const bool supportedColor=fmt==DXGI_FORMAT_R16G16B16A16_FLOAT || fmt==DXGI_FORMAT_R11G11B10_FLOAT ||
        fmt==DXGI_FORMAT_R10G10B10A2_UNORM || fmt==DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        fmt==DXGI_FORMAT_R8G8B8A8_UNORM || fmt==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        fmt==DXGI_FORMAT_B8G8R8A8_TYPELESS || fmt==DXGI_FORMAT_B8G8R8A8_UNORM || fmt==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if(!supportedColor || !queue_texture(device.Get(),frame.context,frame.color,frame.width,frame.height,p->color)) {
        active.store(false);LOG_WARN(blog::cat::core,"skin_probe source shape/format unsupported; disabled without altering game");return;
    }
    if(frame.depth) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv;frame.depth->GetDesc(&srv);
        // Do not misread a lower mip, array or multisampled view as material IDs.
        if(srv.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D&&srv.Texture2D.MostDetailedMip==0) {
            ComPtr<ID3D11Resource> resource;frame.depth->GetResource(&resource);
            ComPtr<ID3D11Texture2D> depth;if(resource&&SUCCEEDED(resource.As(&depth))) {
                p->fsrDepthKey=reinterpret_cast<uint64_t>(depth.Get());
                queue_texture(device.Get(),frame.context,depth.Get(),frame.width,frame.height,p->depth);
            }
        }
    }
    {
        std::lock_guard drawLock(drawMutex);
        bool paired=false;
        if(settings.gbuffer&&geometryNormalBeforeClear&&normalPairedDepthBeforeClear&&normalContext.Get()==frame.context&&
           geometryNormal&&normalSnapshotKey==reinterpret_cast<uint64_t>(geometryNormal.Get())&&normalSnapshotDepthKey==normalDepthKey&&
           normalSnapshotPhase&&now>=normalSnapshotTick&&now-normalSnapshotTick<1000&&
           (!fresh||same_bridge_interval(normalSnapshotBoundary,currentBoundary,normalSnapshotInstance,frame.instance))) {
            const bool normalQueued=queue_texture(device.Get(),frame.context,geometryNormalBeforeClear.Get(),frame.width,frame.height,p->normal,normalSnapshotDecodeFormat);
            const bool depthQueued=queue_texture(device.Get(),frame.context,normalPairedDepthBeforeClear.Get(),frame.width,frame.height,p->materialDepth);
            paired=normalQueued&&depthQueued;
            if(paired) {
                p->normalKey=normalSnapshotKey;p->normalPixelHash=normalSnapshotPixelHash;p->normalSnapshotAgeMs=now-normalSnapshotTick;
                p->normalDecodeFormat=normalSnapshotDecodeFormat;p->normalResourceFormat=normalSnapshotResourceFormat;p->normalSnapshotPhase=normalSnapshotPhase;
                p->normalSnapshotBoundary=normalSnapshotBoundary;p->normalSnapshotInstance=normalSnapshotInstance;
                p->normalSameInterval=same_bridge_interval(normalSnapshotBoundary,currentBoundary,normalSnapshotInstance,frame.instance);
                p->normalBeforeClear=normalSnapshotPhase!=3;p->normalBeforeColorClear=normalSnapshotBeforeColor;
                p->materialDepthKey=normalSnapshotDepthKey;p->materialBeforeClear=normalSnapshotPhase!=3;p->materialSnapshotAgeMs=now-normalSnapshotTick;
                if(gpuActive.load()&&p->normalSameInterval) {
                    p->gpuRequested=true;skinguide::Candidate guide;
                    bool generated=false;
                    if(p->faceOnly) {
                        p->faceDraws=faceDraws;p->faceShaderHash=faceShaderHash;p->faceStatus=faceStatus;
                        const bool sourceMatch=faceBoundary==normalSnapshotBoundary&&faceInstance==normalSnapshotInstance&&
                            faceTargetKey==normalSnapshotKey&&faceDepthKey==normalSnapshotDepthKey;
                        if(sourceMatch) {
                            generated=skinguide::generate_face(frame.context,geometryNormalBeforeClear.Get(),normalSnapshotDecodeFormat,
                                normalPairedDepthBeforeClear.Get(),settings.gbufferStencilIds,settings.gbufferSelected,faceAccum,guide,&p->gpuStatus);
                            if(generated) {
                                const bool coverage=queue_texture(device.Get(),frame.context,faceAccum.coverage.Get(),frame.width,frame.height,p->faceCoverage);
                                const bool reference=queue_texture(device.Get(),frame.context,faceAccum.reference.Get(),frame.width,frame.height,p->faceReference);
                                if(!coverage||!reference){p->faceCoverage.Reset();p->faceReference.Reset();p->faceStatus="face-reference-readback-unavailable";}
                            }
                        } else p->gpuStatus="face-snapshot-pair-mismatch";
                    } else generated=skinguide::generate(frame.context,geometryNormalBeforeClear.Get(),normalSnapshotDecodeFormat,
                        normalPairedDepthBeforeClear.Get(),settings.gbufferStencilIds,settings.gbufferSelected,guide,&p->gpuStatus);
                    if(generated&& !queue_texture(device.Get(),frame.context,guide.texture.Get(),frame.width,frame.height,p->gpu))p->gpuStatus="generated-but-readback-queue-failed";
                }
            } else {p->normal.Reset();p->materialDepth.Reset();}
        }
        if(!paired&&geometryDepth&&geometryContext.Get()==frame.context) {
            ID3D11Texture2D* source=geometryDepth.Get();p->materialDepthKey=reinterpret_cast<uint64_t>(geometryDepth.Get());
            if(geometryBeforeClear&&geometrySnapshotKey==p->materialDepthKey&&now>=geometrySnapshotTick&&now-geometrySnapshotTick<1000) {
                source=geometryBeforeClear.Get();p->materialBeforeClear=true;p->materialSnapshotAgeMs=now-geometrySnapshotTick;
            }
            queue_texture(device.Get(),frame.context,source,frame.width,frame.height,p->materialDepth);
        }
        p->admissions=normalAdmissions;
    }
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
    if(FAILED(device->CreateQuery(&query,&p->ready)))return;
    frame.context->End(p->ready.Get()); // no Flush, no synchronous waits, no state changes
    {std::lock_guard drawLock(drawMutex);p->draws=std::move(drawRecords);p->traceDropped=droppedDraws;drawRecords.clear();droppedDraws=0;}
    pending=std::move(p);--remaining;nextCapture=now+settings.interval;freshWaitStart=0;
    if(!fastActive.load(std::memory_order_relaxed))pairRequested.store(false);
    // Last capture needs no following draw burst. All tracing stops after its budget.
    drawBudget.store(remaining?kDrawQueryBudget:0,std::memory_order_relaxed);
    if(!remaining){skinmaterial::end_session();if(!fastActive.load(std::memory_order_relaxed)){std::lock_guard drawLock(drawMutex);reset_face_locked();}}
} catch(...) {
    active.store(false);
    LOG_WARN(blog::cat::core,"skin_probe capture exception; disabled without altering NR");
}
}
