#include "SkinMaterialEvidence.h"
#include "SkinMaterialEvidenceMath.h"
#include "SkinMaskProbeMath.h"
#include "BridgeLogger.h"
#include <Windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <unordered_map>
#include <set>
#include <memory>
#include <cstring>
#include <deque>

namespace skinmaterial {
namespace {
using Microsoft::WRL::ComPtr;
constexpr size_t kTextureLimit=24,kPendingLimit=8,kAssociationLimit=512;
constexpr uint64_t kSessionByteLimit=64ull*1024*1024,kShaderByteLimit=16ull*1024*1024;
struct Settings {
    bool enabled=false,selectStencil=false,family=false;
    std::array<uint8_t,256> refs{};
    std::filesystem::path directory;
};
struct Texture {
    uint64_t key=0,queued=0,contentHash=0;
    uint32_t width=0,height=0,format=0,resourceFormat=0,mip=0;
    std::string file,status="queued";
    ComPtr<ID3D11Texture2D> source; // Keep identities alive until the session closes (no pointer recycling).
};
struct Association {
    uint32_t texture=0,slot=0,calls=1;
    skinprobe::DrawIdentity draw;
};
struct Pending {
    size_t index=0;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Query> query;
};
struct Job {
    std::filesystem::path directory;
    std::string file,manifest;
    std::vector<uint8_t> bytes;
    std::vector<std::pair<uint64_t,std::vector<uint8_t>>> shaders;
    bool finalize=false;
};
std::atomic_bool active{false},writerBusy{false},familyActive{false},analysisBusy{false};
std::atomic_bool writerSuccess{false};
std::mutex stateMutex;
Settings settings;
std::unordered_map<uint64_t,std::vector<uint8_t>> shaderCache;
struct AnalysisJob {uint64_t hash=0;std::vector<uint8_t> bytes;};
std::deque<AnalysisJob> analysisQueue;
std::unordered_map<uint64_t,skinfamily::Result> analysisMemo;
uint32_t analysisRequests=0;
bool analysisCollecting=false;
uint64_t shaderBytes=0,sessionBytes=0,sessionStart=0,instance=0,frame=0;
uint32_t session=0,droppedAssociations=0;
bool collecting=false,closing=false,finalized=true;
std::string stem;
std::vector<Texture> textures;
std::vector<Association> associations;
std::vector<Pending> pending;
size_t writingIndex=SIZE_MAX;
bool writingFinal=false;
ComPtr<ID3D11DeviceContext> owner;
std::string hex(uint64_t value) {std::ostringstream s;s<<"0x"<<std::hex<<std::setw(16)<<std::setfill('0')<<value;return s.str();}
bool write(const std::filesystem::path& path,const void* bytes,size_t size) {
    std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);
    std::ofstream s(path,std::ios::binary|std::ios::trunc);
    if(!s)return false;s.write(static_cast<const char*>(bytes),static_cast<std::streamsize>(size));return s.good();
}
std::string manifest() {
    std::ostringstream s;
    s<<R"({"schema":1,"classification":"unverified-material-texture-evidence","consumer":"none","pid":)"<<GetCurrentProcessId()
     <<",\"session\":"<<session<<",\"startInstance\":"<<instance<<",\"startFrame\":"<<frame
     <<",\"startTickMs\":"<<sessionStart<<",\"nrModified\":false,\"textureLimit\":"<<kTextureLimit
     <<",\"byteLimit\":"<<kSessionByteLimit<<",\"shaderCacheBytes\":"<<shaderBytes
     <<",\"droppedAssociations\":"<<droppedAssociations<<",\"stencilSelector\":[";
    bool comma=false;for(unsigned i=0;i<256;++i)if(settings.refs[i]){if(comma)s<<',';comma=true;s<<i;}
    s<<"],\"textures\":[";
    for(size_t i=0;i<textures.size();++i) {
        if(i)s<<',';const auto& t=textures[i];
        s<<"{\"index\":"<<i<<",\"key\":\""<<hex(t.key)<<"\",\"width\":"<<t.width<<",\"height\":"<<t.height
         <<",\"ddsFormat\":"<<t.format<<",\"resourceFormat\":"<<t.resourceFormat<<",\"sourceMip\":"<<t.mip
         <<",\"queuedTickMs\":"<<t.queued<<",\"file\":\""<<t.file<<"\",\"status\":\""<<t.status
         <<"\",\"contentHash\":\""<<hex(t.contentHash)<<"\",\"role\":\"unknown\"}";
    }
    s<<"],\"associations\":[";
    for(size_t i=0;i<associations.size();++i) {
        if(i)s<<',';const auto& a=associations[i];const auto& d=a.draw;
        s<<"{\"texture\":"<<a.texture<<",\"slot\":"<<a.slot<<",\"calls\":"<<a.calls<<",\"ps\":\""<<hex(d.pixelHash)
         <<"\",\"vs\":\""<<hex(d.vertexHash)<<"\",\"rt\":\""<<hex(d.renderTarget)<<"\",\"dsv\":\""<<hex(d.depthTarget)
         <<"\",\"stencilRef\":"<<d.stencilRef<<",\"stencilEnabled\":"<<d.stencilEnabled<<",\"writeMask\":"<<d.writeMask
         <<",\"passOp\":"<<d.passOp<<",\"width\":"<<d.width<<",\"height\":"<<d.height<<"}";
    }
    s<<"],\"shaders\":[";std::set<uint64_t> hashes;for(const auto& a:associations)if(a.draw.pixelHash)hashes.insert(a.draw.pixelHash);
    comma=false;for(auto h:hashes) {
        if(comma)s<<',';comma=true;const auto found=shaderCache.find(h);
        s<<"{\"ps\":\""<<hex(h)<<"\",\"cached\":"<<(found!=shaderCache.end()?"true":"false")
         <<",\"alphaFamilyKind\":"<<(analysisMemo.contains(h)?uint32_t(analysisMemo.at(h).kind):0)
         <<",\"bytecodeFile\":\"ps_"<<hex(h).substr(2)<<".dxbc\",\"assemblyFile\":\"ps_"<<hex(h).substr(2)<<".asm.txt\"}";
    }
    s<<"]}\n";return s.str();
}
bool start_writer(std::shared_ptr<Job> job) {
    // The worker owns only CPU byte arrays. Never carry a D3D reference across this boundary.
    HMODULE pinned=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&poll),&pinned))return false;
    writerBusy.store(true,std::memory_order_release);
    try {
        std::thread([job] {
            bool ok=false;
            try {
                if(!job->finalize)ok=write(job->directory/job->file,job->bytes.data(),job->bytes.size());
                else {
                    ok=true;
                    for(const auto& [hash,bytecode]:job->shaders) {
                        const auto name="ps_"+hex(hash).substr(2);
                        ok=write(job->directory/(name+".dxbc"),bytecode.data(),bytecode.size())&&ok;
                        ComPtr<ID3DBlob> disassembly;
                        HRESULT result=D3DDisassemble(bytecode.data(),bytecode.size(),0,nullptr,&disassembly);
                        if(SUCCEEDED(result)&&disassembly) {
                            ok=write(job->directory/(name+".asm.txt"),disassembly->GetBufferPointer(),disassembly->GetBufferSize())&&ok;
                        } else {
                            const auto error="Disassembly unavailable; HRESULT="+hex(uint32_t(result))+"\n";
                            write(job->directory/(name+".asm-error.txt"),error.data(),error.size());ok=false;
                        }
                    }
                    ok=write(job->directory/"materials.json",job->manifest.data(),job->manifest.size())&&ok;
                    LOG_INFO(blog::cat::core,"skin_material output="+job->directory.string()+" complete="+std::to_string(ok)+" semantic_skin=unverified consumer=none");
                }
            } catch(...) {ok=false;LOG_WARN(blog::cat::core,"skin_material CPU export failed; no NR changes");}
            writerSuccess.store(ok,std::memory_order_release);writerBusy.store(false,std::memory_order_release);
        }).detach();return true;
    } catch(...) {writerBusy.store(false,std::memory_order_release);return false;}
}
bool start_analysis_locked() {
    if(analysisBusy.load())return true;
    HMODULE pinned=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&alpha_family),&pinned))return false;
    analysisBusy.store(true);
    try {
        std::thread([] {
          try {
            while(true) {
                AnalysisJob job;
                {
                    std::lock_guard lock(stateMutex);
                    if(analysisQueue.empty()){analysisBusy.store(false);return;}
                    job=std::move(analysisQueue.front());analysisQueue.pop_front();
                }
                skinfamily::Result result;result.reason="shader-disassembly-unavailable";
                try {
                    ComPtr<ID3DBlob> assembly;
                    if(SUCCEEDED(D3DDisassemble(job.bytes.data(),job.bytes.size(),0,nullptr,&assembly))&&assembly)
                        result=skinfamily::classify(std::string_view(static_cast<const char*>(assembly->GetBufferPointer()),assembly->GetBufferSize()));
                } catch(...) {result.kind=skinfamily::Kind::Rejected;result.reason="shader-analysis-exception";}
                {
                    std::lock_guard lock(stateMutex);
                    const auto found=analysisMemo.find(job.hash);if(found!=analysisMemo.end())found->second=result;
                }
                LOG_INFO(blog::cat::core,"skin_material alpha-family ps="+hex(job.hash)+" kind="+std::to_string(uint32_t(result.kind))+
                    " bucket_line="+std::to_string(result.bucketLine)+" class_line="+std::to_string(result.classLine)+" output_line="+std::to_string(result.outputLine)+" reason="+result.reason);
            }
          } catch(...) {
            // A diagnostic CPU thread must never terminate the game on an
            // allocation/logger/disassembler exception. Don't allocate/log in
            // this fallback, and never leave queued entries stuck as Pending.
            try {
                std::lock_guard lock(stateMutex);
                for(auto& [hash,value]:analysisMemo)if(value.kind==skinfamily::Kind::Pending)
                    value={skinfamily::Kind::Rejected,0,0,0,0,"shader-analysis-worker-exception"};
                analysisQueue.clear();analysisBusy.store(false);
            } catch(...) {familyActive.store(false);analysisBusy.store(false);}
          }
        }).detach();return true;
    } catch(...) {analysisBusy.store(false);return false;}
}
void associate(size_t index,uint32_t slot,const skinprobe::DrawIdentity& d) {
    for(auto& a:associations)if(a.texture==index&&a.slot==slot&&a.draw==d){++a.calls;return;}
    if(associations.size()>=kAssociationLimit){++droppedAssociations;return;}
    associations.push_back({static_cast<uint32_t>(index),slot,1,d});
}
}
bool enabled(){return active.load(std::memory_order_acquire);}
bool family_enabled(){return familyActive.load(std::memory_order_acquire);}
skinfamily::Result alpha_family(uint64_t hash,uint32_t stencilRef,bool request) try {
    if(!family_enabled()||!hash)return {skinfamily::Kind::Unknown,0,0,0,0,"family-analysis-disabled"};
    std::lock_guard lock(stateMutex);
    const auto found=analysisMemo.find(hash);if(found!=analysisMemo.end())return found->second;
    if(!request||!analysisCollecting||stencilRef>255||
        (settings.selectStencil?!settings.refs[stencilRef]:(stencilRef!=133&&stencilRef!=165)))
        return {skinfamily::Kind::Unknown,0,0,0,0,"outside-bounded-character-trace"};
    const auto source=shaderCache.find(hash);if(source==shaderCache.end())return {skinfamily::Kind::Unknown,0,0,0,0,"original-bytecode-not-cached"};
    if(analysisRequests>=16||analysisMemo.size()>=256||analysisQueue.size()>=8)
        return {skinfamily::Kind::Unknown,0,0,0,0,"shader-analysis-budget-exhausted"};
    skinfamily::Result pendingResult{skinfamily::Kind::Pending,0,0,0,0,"shader-family-analysis-pending"};
    analysisMemo.emplace(hash,pendingResult);analysisQueue.push_back({hash,source->second});++analysisRequests;
    if(!start_analysis_locked()) {
        analysisMemo[hash]={skinfamily::Kind::Rejected,0,0,0,0,"shader-analysis-worker-unavailable"};
        analysisQueue.clear();return analysisMemo[hash];
    }
    return pendingResult;
} catch(...) {return {skinfamily::Kind::Unknown,0,0,0,0,"shader-analysis-request-failed"};}
void configure(const wchar_t* iniPath) try {
    if(!iniPath)return;std::lock_guard lock(stateMutex);
    if(writerBusy.load()||analysisBusy.load()||writingIndex!=SIZE_MAX||writingFinal)return; // Don't reset a live exporter.
    Settings next;
    next.family=GetPrivateProfileIntW(L"Dx11FsrBridge",L"SkinMaskProbe",0,iniPath)!=0&&
        GetPrivateProfileIntW(L"Dx11FsrBridge",L"SkinMaskProbeGBufferShaderFamily",0,iniPath)!=0;
    next.enabled=GetPrivateProfileIntW(L"Dx11FsrBridge",L"SkinMaskProbe",0,iniPath)!=0&&
        GetPrivateProfileIntW(L"Dx11FsrBridge",L"SkinMaskProbeMaterialEvidence",0,iniPath)!=0;
    wchar_t path[2048]{};GetPrivateProfileStringW(L"Dx11FsrBridge",L"SkinMaskProbeDir",L"",path,2048,iniPath);
    next.directory=path[0]?std::filesystem::path(path):std::filesystem::path(iniPath).parent_path()/L"fsr2dump"/L"skinmask";
    if(next.directory.is_relative())next.directory=std::filesystem::path(iniPath).parent_path()/next.directory;
    wchar_t refs[1024]{};GetPrivateProfileStringW(L"Dx11FsrBridge",L"SkinMaskProbeMaterialStencilRefs",L"",refs,1024,iniPath);
    bool valid=false;next.refs=skinprobe::parse_stencil_ids(refs,&valid);
    next.selectStencil=std::any_of(next.refs.begin(),next.refs.end(),[](auto x){return x!=0;});
    if(!valid){next.enabled=false;next.family=false;}
    settings=next;active.store(next.enabled,std::memory_order_release);familyActive.store(next.family,std::memory_order_release);
    analysisQueue.clear();analysisMemo.clear();analysisRequests=0;analysisCollecting=false;
    pending.clear();textures.clear();associations.clear();shaderCache.clear();shaderBytes=0;
    owner.Reset();collecting=closing=false;finalized=true;writingIndex=SIZE_MAX;writingFinal=false;
    if(next.family)LOG_INFO(blog::cat::core,"skin_material bounded shader-family analysis enabled; original PS alpha/material/output structure only; NR unchanged");
    if(next.enabled)LOG_INFO(blog::cat::core,"skin_material evidence enabled: bounded DDS + original PS bytecode; material roles unknown; NR unchanged");
} catch(...) {active.store(false);familyActive.store(false);}
void register_pixel_shader(uint64_t hash,const void* bytecode,size_t size) try {
    if((!enabled()&&!family_enabled())||!hash||!bytecode||!size||size>1024*1024)return;
    std::lock_guard lock(stateMutex);
    if(shaderCache.contains(hash)||shaderCache.size()>=2048||shaderBytes+size>kShaderByteLimit)return;
    const auto* first=static_cast<const uint8_t*>(bytecode);
    shaderCache.emplace(hash,std::vector<uint8_t>(first,first+size));shaderBytes+=size;
} catch(...) {LOG_WARN(blog::cat::core,"skin_material shader cache allocation skipped");}
void begin_session(uint32_t number,uint64_t captureInstance,uint64_t captureFrame) try {
    if(!enabled()&&!family_enabled())return;std::lock_guard lock(stateMutex);
    analysisRequests=0;analysisCollecting=true;
    if(!enabled())return;
    if(!finalized||writerBusy.load()){LOG_WARN(blog::cat::core,"skin_material previous session still exporting; skip new material session");return;}
    session=number;instance=captureInstance;frame=captureFrame;sessionStart=GetTickCount64();
    stem="p"+std::to_string(GetCurrentProcessId())+"_s"+std::to_string(session)+"_t"+std::to_string(sessionStart)+"_materials";
    textures.clear();associations.clear();pending.clear();owner.Reset();sessionBytes=0;droppedAssociations=0;
    collecting=true;closing=false;finalized=false;
} catch(...) {active.store(false);}
void end_session(){if(!enabled()&&!family_enabled())return;std::lock_guard lock(stateMutex);analysisCollecting=false;if(enabled()){collecting=false;closing=!finalized;}}
void observe_draw(ID3D11DeviceContext* context,const skinprobe::DrawIdentity& identity,
                  ID3D11ShaderResourceView* const* views,uint32_t count) try {
    if(!enabled()||!context||!views||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    std::lock_guard lock(stateMutex);if(!collecting)return;
    if(settings.selectStencil&&(!identity.stencilEnabled||identity.stencilRef>255||!settings.refs[identity.stencilRef]))return;
    if(owner&&owner.Get()!=context)return;
    ComPtr<ID3D11Device> device;context->GetDevice(&device);if(!device)return;
    if(!owner){ComPtr<ID3D11On12Device> on12;if(SUCCEEDED(device.As(&on12)))return;owner=context;}
    uint32_t queuedThisDraw=0;
    for(uint32_t slot=0;slot<std::min(count,4u);++slot) {
        if(!views[slot])continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};views[slot]->GetDesc(&vd);
        if(vd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D)continue;
        ComPtr<ID3D11Resource> resource;views[slot]->GetResource(&resource);
        ComPtr<ID3D11Texture2D> source;if(!resource||FAILED(resource.As(&source)))continue;
        D3D11_TEXTURE2D_DESC td{};source->GetDesc(&td);
        if(td.ArraySize!=1||td.SampleDesc.Count!=1||vd.Texture2D.MostDetailedMip>=td.MipLevels||
           (td.BindFlags&(D3D11_BIND_RENDER_TARGET|D3D11_BIND_DEPTH_STENCIL))||
           (td.MiscFlags&(D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX|D3D11_RESOURCE_MISC_SHARED_NTHANDLE)))continue;
        const auto mip=vd.Texture2D.MostDetailedMip;
        const auto w=std::max(1u,td.Width>>mip),h=std::max(1u,td.Height>>mip);
        const auto format=typed_format(td.Format,vd.Format);const auto layout=texture_layout(format,w,h);
        if(!layout.rowBytes)continue;
        const auto key=reinterpret_cast<uint64_t>(source.Get());size_t index=0;
        for(;index<textures.size();++index)if(textures[index].key==key&&textures[index].mip==mip&&textures[index].format==format)break;
        if(index<textures.size()){associate(index,slot,identity);continue;}
        const uint64_t bytes=uint64_t(layout.rowBytes)*layout.rows;
        if(textures.size()>=kTextureLimit||pending.size()>=kPendingLimit||queuedThisDraw>=2||sessionBytes+bytes>kSessionByteLimit)continue;
        Pending p;p.index=textures.size();p.context=context;
        auto staging=td;staging.Width=w;staging.Height=h;staging.MipLevels=staging.ArraySize=1;
        staging.Usage=D3D11_USAGE_STAGING;staging.BindFlags=0;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;staging.MiscFlags=0;
        if(FAILED(device->CreateTexture2D(&staging,nullptr,&p.staging)))continue;
        D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};if(FAILED(device->CreateQuery(&query,&p.query)))continue;
        Texture t;t.key=key;t.width=w;t.height=h;t.format=format;t.resourceFormat=td.Format;t.mip=mip;t.source=source;t.queued=GetTickCount64();
        t.file="t"+std::to_string(textures.size())+"_"+hex(key).substr(2)+"_m"+std::to_string(mip)+".dds";
        // Allocate bookkeeping BEFORE issuing the copy. Neither call alters bound pipeline state.
        textures.push_back(std::move(t));pending.push_back(std::move(p));
        context->CopySubresourceRegion(pending.back().staging.Get(),0,0,0,0,source.Get(),mip,nullptr);
        context->End(pending.back().query.Get());sessionBytes+=bytes;++queuedThisDraw;associate(textures.size()-1,slot,identity);
    }
} catch(...) {LOG_WARN(blog::cat::core,"skin_material draw capture skipped; no rendering changes");}
void poll(ID3D11DeviceContext* context) try {
    if(!enabled()||!context||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    std::lock_guard lock(stateMutex);if(finalized||(owner&&owner.Get()!=context))return;
    if(writerBusy.load(std::memory_order_acquire))return;
    if(writingIndex!=SIZE_MAX){textures[writingIndex].status=writerSuccess.load()?"saved":"write-failed";writingIndex=SIZE_MAX;}
    if(writingFinal) {
        writingFinal=false;finalized=true;closing=collecting=false;owner.Reset();
        for(auto& t:textures)t.source.Reset();return;
    }
    // Drain at most one texture per frame. Expire stalled queries without Flush or a blocking Map.
    for(size_t i=0;i<pending.size();) {
        auto& p=pending[i];auto& t=textures[p.index];
        if(GetTickCount64()-t.queued>5000){t.status="expired";pending.erase(pending.begin()+i);continue;}
        BOOL done=FALSE;HRESULT ready=context->GetData(p.query.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(FAILED(ready)){t.status="query-failed";pending.erase(pending.begin()+i);continue;}
        if(ready!=S_OK||!done){++i;continue;}
        const auto layout=texture_layout(t.format,t.width,t.height);
        auto job=std::make_shared<Job>();job->directory=settings.directory/stem;job->file=t.file;
        job->bytes=dds_header(t.format,t.width,t.height);const auto header=job->bytes.size();
        job->bytes.resize(header+size_t(layout.rowBytes)*layout.rows); // Allocations happen before Map.
        D3D11_MAPPED_SUBRESOURCE map{};
        const HRESULT mapped=context->Map(p.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
        if(mapped==DXGI_ERROR_WAS_STILL_DRAWING){++i;continue;}
        if(FAILED(mapped)){t.status="map-failed";pending.erase(pending.begin()+i);continue;}
        // Texture2D DepthPitch is not used; a mapped row has at least RowPitch bytes.
        const bool valid=map.pData&&valid_copy(layout.rowBytes,layout.rows,map.RowPitch,uint64_t(map.RowPitch)*layout.rows);
        if(valid)for(uint32_t row=0;row<layout.rows;++row)std::memcpy(job->bytes.data()+header+size_t(row)*layout.rowBytes,
            static_cast<const uint8_t*>(map.pData)+size_t(row)*map.RowPitch,layout.rowBytes);
        context->Unmap(p.staging.Get(),0);
        if(!valid){t.status="invalid-map-layout";pending.erase(pending.begin()+i);continue;}
        t.contentHash=fnv64(job->bytes.data()+header,job->bytes.size()-header);writingIndex=p.index;
        pending.erase(pending.begin()+i);
        if(!start_writer(job)){t.status="writer-unavailable";writingIndex=SIZE_MAX;}
        return;
    }
    if(closing&&pending.empty()) {
        auto job=std::make_shared<Job>();job->directory=settings.directory/stem;job->finalize=true;job->manifest=manifest();
        std::set<uint64_t> hashes;for(const auto& a:associations)if(a.draw.pixelHash)hashes.insert(a.draw.pixelHash);
        for(auto hash:hashes){auto found=shaderCache.find(hash);if(found!=shaderCache.end())job->shaders.emplace_back(hash,found->second);}
        writingFinal=start_writer(job);if(!writingFinal){finalized=true;owner.Reset();for(auto& t:textures)t.source.Reset();}
    }
} catch(...) {active.store(false);LOG_WARN(blog::cat::core,"skin_material exporter disabled after exception; NR untouched");}
}
