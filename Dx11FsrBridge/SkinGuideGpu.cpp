#include "SkinGuideGpu.h"
#include "SkinGBufferProbeMath.h"
#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <mutex>
#include <cstring>
namespace skinguide {
namespace {
using Microsoft::WRL::ComPtr;
constexpr char kShader[]=R"HLSL(
Texture2D<float4> normalInput : register(t0);
Texture2D<uint2> originalStencil : register(t1);
RWTexture2D<float> guideOutput : register(u0);
cbuffer Selection : register(b0) {
    uint2 extent; uint useStencil; uint padding;
    uint4 selectedIds[2];
};
[numthreads(8,8,1)]
void main(uint3 at : SV_DispatchThreadID) {
    if(any(at.xy>=extent))return;
    float alpha=normalInput.Load(int3(at.xy,0)).a;
    uint id=originalStencil.Load(int3(at.xy,0)).y & 255u;
    uint word=selectedIds[id>>7u][(id>>5u)&3u];
    bool selected=useStencil==0u || (word & (1u<<(id&31u)))!=0u;
    // A2/UNORM/FP formats all normalize the game-produced alpha. Not RGB hue.
    bool candidate=abs(alpha-(1.0/3.0))<=0.015 && selected;
    guideOutput[at.xy]=candidate ? 1.0 : 0.0;
}
)HLSL";
constexpr char kFaceDeltaShader[]=R"HLSL(
Texture2D<float4> beforeInput:register(t0);
Texture2D<float4> afterInput:register(t1);
RWTexture2D<uint> coverage:register(u0);
RWTexture2D<float4> reference:register(u1);
[numthreads(8,8,1)]
void main(uint3 at:SV_DispatchThreadID) {
    uint w,h;coverage.GetDimensions(w,h);if(at.x>=w||at.y>=h)return;
    float4 before=beforeInput.Load(int3(at.xy,0));
    float4 after=afterInput.Load(int3(at.xy,0));
    // This copy interval surrounds ONLY the native face shader draw. A body
    // marker that already existed elsewhere is unchanged and cannot seed face.
    if(abs(after.a-(1.0/3.0))<=0.015 && any(before!=after)) {
        coverage[at.xy]=1u;reference[at.xy]=after;
    }
}
)HLSL";
constexpr char kFaceFinalShader[]=R"HLSL(
Texture2D<float4> normalInput:register(t0);
Texture2D<uint2> originalStencil:register(t1);
Texture2D<uint> faceCoverage:register(t2);
Texture2D<float4> faceReference:register(t3);
RWTexture2D<float> guideOutput:register(u0);
cbuffer Selection:register(b0) {uint2 extent;uint useStencil;uint padding;uint4 selectedIds[2];};
[numthreads(8,8,1)]
void main(uint3 at:SV_DispatchThreadID) {
    if(any(at.xy>=extent))return;
    if(faceCoverage.Load(int3(at.xy,0))==0u){guideOutput[at.xy]=0.0;return;}
    float4 current=normalInput.Load(int3(at.xy,0));float4 face=faceReference.Load(int3(at.xy,0));
    uint id=originalStencil.Load(int3(at.xy,0)).y & 255u;
    bool selected=useStencil==0u || (selectedIds[id>>7u][(id>>5u)&3u] & (1u<<(id&31u)))!=0u;
    // A later hair/body draw which changes the original normal/material pixel
    // invalidates this face sample. Keep unchanged visible face pixels only.
    bool visible=all(abs(current-face)<=0.00001) && abs(current.a-(1.0/3.0))<=0.015;
    guideOutput[at.xy]=selected&&visible ? 1.0 : 0.0;
}
)HLSL";
std::mutex shaderMutex;
std::array<ComPtr<ID3DBlob>,3> compiled;
ComPtr<ID3D11Device> shaderDevice;
std::array<ComPtr<ID3D11ComputeShader>,3> cachedShader;
bool shader_for(ID3D11Device* device,ComPtr<ID3D11ComputeShader>& shader,uint32_t program=0) {
    std::lock_guard lock(shaderMutex);if(program>=3)return false;
    const char* source=program==0?kShader:program==1?kFaceDeltaShader:kFaceFinalShader;
    if(!compiled[program]) {
        ComPtr<ID3DBlob> errors;
        if(FAILED(D3DCompile(source,std::strlen(source),"SkinGuideGpu",nullptr,nullptr,"main","cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&compiled[program],&errors)))return false;
    }
    if(shaderDevice.Get()!=device){for(auto& s:cachedShader)s.Reset();shaderDevice=device;}
    if(!cachedShader[program]) {
        ComPtr<ID3D11ComputeShader> next;
        if(FAILED(device->CreateComputeShader(compiled[program]->GetBufferPointer(),compiled[program]->GetBufferSize(),nullptr,&next)))return false;
        cachedShader[program]=next;
    }
    shader=cachedShader[program];return true;
}
struct RestoreCS {
    ID3D11DeviceContext* context;
    ComPtr<ID3D11ComputeShader> shader;
    std::array<ComPtr<ID3D11ClassInstance>,256> classes;
    UINT classCount=256;
    std::array<ComPtr<ID3D11ShaderResourceView>,4> views;
    ComPtr<ID3D11Buffer> cb;
    std::array<ComPtr<ID3D11UnorderedAccessView>,2> uavs;
    explicit RestoreCS(ID3D11DeviceContext* c):context(c) {
        ID3D11ClassInstance* ci[256]{};context->CSGetShader(&shader,ci,&classCount);
        for(UINT i=0;i<classCount&&i<256;++i)classes[i].Attach(ci[i]);
        ID3D11ShaderResourceView* srv[4]{};context->CSGetShaderResources(0,4,srv);
        for(UINT i=0;i<4;++i)views[i].Attach(srv[i]);
        ID3D11UnorderedAccessView* output[2]{};context->CSGetUnorderedAccessViews(0,2,output);
        for(UINT i=0;i<2;++i)uavs[i].Attach(output[i]);
        context->CSGetConstantBuffers(0,1,&cb);
    }
    ~RestoreCS() noexcept {
        const UINT keep[2]{UINT(-1),UINT(-1)};
        ID3D11UnorderedAccessView* noneUav[2]{};context->CSSetUnorderedAccessViews(0,2,noneUav,keep);
        ID3D11ShaderResourceView* noneSrv[4]{};context->CSSetShaderResources(0,4,noneSrv);
        ID3D11ClassInstance* ci[256]{};for(UINT i=0;i<classCount&&i<256;++i)ci[i]=classes[i].Get();
        context->CSSetShader(shader.Get(),classCount?ci:nullptr,classCount);
        context->CSSetConstantBuffers(0,1,cb.GetAddressOf());
        ID3D11ShaderResourceView* srv[4]{views[0].Get(),views[1].Get(),views[2].Get(),views[3].Get()};context->CSSetShaderResources(0,4,srv);
        // Preserve the caller's hidden APPEND/COUNTER value: do NOT reset it.
        ID3D11UnorderedAccessView* output[2]{uavs[0].Get(),uavs[1].Get()};context->CSSetUnorderedAccessViews(0,2,output,keep);
    }
};
bool readonly_snapshot(const D3D11_TEXTURE2D_DESC& d) {
    return d.Width&&d.Height&&d.Width<=4096&&d.Height<=4096&&d.ArraySize==1&&d.MipLevels==1&&d.SampleDesc.Count==1&&
        (d.BindFlags&D3D11_BIND_SHADER_RESOURCE)&&!(d.BindFlags&(D3D11_BIND_RENDER_TARGET|D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_UNORDERED_ACCESS))&&
        !(d.MiscFlags&(D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX|D3D11_RESOURCE_MISC_SHARED_NTHANDLE));
}
}
bool generate(ID3D11DeviceContext* context,ID3D11Texture2D* normal,uint32_t normalViewFormat,
              ID3D11Texture2D* originalDepth,const std::array<uint8_t,256>& ids,bool select,
              Candidate& result,const char** reason) noexcept {
    result={};if(reason)*reason="not-started";
    auto fail=[&](const char* detail){if(reason)*reason=detail;return false;};
    try {
        if(!context||!normal||!originalDepth||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return fail("invalid-or-deferred-context");
        ComPtr<ID3D11Device> device,normalDevice,depthDevice;context->GetDevice(&device);normal->GetDevice(&normalDevice);originalDepth->GetDevice(&depthDevice);
        if(!device||device.Get()!=normalDevice.Get()||device.Get()!=depthDevice.Get())return fail("device-mismatch");
        ComPtr<ID3D11On12Device> on12;if(SUCCEEDED(device.As(&on12)))return fail("d3d11on12-refused");
        D3D11_TEXTURE2D_DESC nd{},dd{};normal->GetDesc(&nd);originalDepth->GetDesc(&dd);
        if(!readonly_snapshot(nd)||!readonly_snapshot(dd)||nd.Width!=dd.Width||nd.Height!=dd.Height)return fail("inputs-not-private-paired-readonly-snapshots");
        const auto decode=skinprobe::typed_gbuffer_format(nd.Format,normalViewFormat);if(!decode)return fail("unsupported-normal-view");
        DXGI_FORMAT stencilFormat=dd.Format==DXGI_FORMAT_R32G8X24_TYPELESS?DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
            dd.Format==DXGI_FORMAT_R24G8_TYPELESS?DXGI_FORMAT_X24_TYPELESS_G8_UINT:DXGI_FORMAT_UNKNOWN;
        if(stencilFormat==DXGI_FORMAT_UNKNOWN)return fail("unsupported-stencil-storage");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;
        sv.Format=static_cast<DXGI_FORMAT>(decode);ComPtr<ID3D11ShaderResourceView> ns,ds;
        if(FAILED(device->CreateShaderResourceView(normal,&sv,&ns)))return fail("normal-srv-failed");
        sv.Format=stencilFormat;if(FAILED(device->CreateShaderResourceView(originalDepth,&sv,&ds)))return fail("stencil-srv-failed");
        D3D11_TEXTURE2D_DESC out{};out.Width=nd.Width;out.Height=nd.Height;out.MipLevels=out.ArraySize=out.SampleDesc.Count=1;
        out.Format=DXGI_FORMAT_R8_UNORM;out.Usage=D3D11_USAGE_DEFAULT;out.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        Candidate candidate;ComPtr<ID3D11UnorderedAccessView> output;
        if(FAILED(device->CreateTexture2D(&out,nullptr,&candidate.texture))||
           FAILED(device->CreateShaderResourceView(candidate.texture.Get(),nullptr,&candidate.view))||
           FAILED(device->CreateUnorderedAccessView(candidate.texture.Get(),nullptr,&output)))return fail("r8-guide-creation-failed");
        std::array<uint32_t,12> params{};params[0]=nd.Width;params[1]=nd.Height;params[2]=select?1:0;
        for(uint32_t id=0;id<256;++id)if(ids[id])params[4+id/32]|=1u<<(id%32);
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(params);bd.Usage=D3D11_USAGE_IMMUTABLE;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{params.data(),0,0};ComPtr<ID3D11Buffer> cb;
        if(FAILED(device->CreateBuffer(&bd,&initial,&cb)))return fail("selection-cb-failed");
        ComPtr<ID3D11ComputeShader> shader;if(!shader_for(device.Get(),shader))return fail("compute-shader-failed");
        {
            RestoreCS restore(context);
            const UINT keep=UINT(-1);ID3D11ShaderResourceView* inputs[2]{ns.Get(),ds.Get()};
            context->CSSetShader(shader.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,cb.GetAddressOf());
            context->CSSetShaderResources(0,2,inputs);context->CSSetUnorderedAccessViews(0,1,output.GetAddressOf(),&keep);
            context->Dispatch((nd.Width+7)/8,(nd.Height+7)/8,1);
        }
        result=std::move(candidate);if(reason)*reason="generated-r8-candidate";return true;
    } catch(...) {return fail("candidate-exception");}
}
bool accumulate_face_delta(ID3D11DeviceContext* context,ID3D11Texture2D* before,ID3D11Texture2D* after,
    uint32_t normalViewFormat,FaceAccumulation& face,const char** reason) noexcept {
    if(reason)*reason="not-started";auto fail=[&](const char* why){if(reason)*reason=why;return false;};
    try {
        if(!context||!before||!after||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return fail("invalid-face-context");
        ComPtr<ID3D11Device> device,beforeDevice,afterDevice;context->GetDevice(&device);before->GetDevice(&beforeDevice);after->GetDevice(&afterDevice);
        if(!device||device.Get()!=beforeDevice.Get()||device.Get()!=afterDevice.Get())return fail("face-device-mismatch");
        ComPtr<ID3D11On12Device> on12;if(SUCCEEDED(device.As(&on12)))return fail("face-d3d11on12-refused");
        D3D11_TEXTURE2D_DESC bd{},ad{};before->GetDesc(&bd);after->GetDesc(&ad);
        if(!readonly_snapshot(bd)||!readonly_snapshot(ad)||bd.Width!=ad.Width||bd.Height!=ad.Height||
            skinprobe::typed_gbuffer_format(bd.Format,normalViewFormat)!=24||skinprobe::typed_gbuffer_format(ad.Format,normalViewFormat)!=24||
            uint64_t(bd.Width)*bd.Height*28>128ull*1024*1024)return fail("face-input-format-shape-budget-refused");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R10G10B10A2_UNORM;sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;
        ComPtr<ID3D11ShaderResourceView> bs,as; if(FAILED(device->CreateShaderResourceView(before,&sv,&bs))||FAILED(device->CreateShaderResourceView(after,&sv,&as)))return fail("face-input-srv-failed");
        bool initialize=!face.coverage;
        if(face.coverage) {
            D3D11_TEXTURE2D_DESC prior{};face.coverage->GetDesc(&prior);ComPtr<ID3D11Device> owner;face.coverage->GetDevice(&owner);
            if(prior.Width!=bd.Width||prior.Height!=bd.Height||owner.Get()!=device.Get())return fail("face-accumulator-owner-shape-mismatch");
        }
        if(initialize) {
            FaceAccumulation next;D3D11_TEXTURE2D_DESC td{};td.Width=bd.Width;td.Height=bd.Height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
            td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;td.Format=DXGI_FORMAT_R32_UINT;
            if(FAILED(device->CreateTexture2D(&td,nullptr,&next.coverage))||FAILED(device->CreateShaderResourceView(next.coverage.Get(),nullptr,&next.coverageView)))return fail("face-coverage-creation-failed");
            td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
            if(FAILED(device->CreateTexture2D(&td,nullptr,&next.reference))||FAILED(device->CreateShaderResourceView(next.reference.Get(),nullptr,&next.referenceView)))return fail("face-reference-creation-failed");
            face=std::move(next);
        }
        ComPtr<ID3D11UnorderedAccessView> coverage,reference;
        if(FAILED(device->CreateUnorderedAccessView(face.coverage.Get(),nullptr,&coverage))||FAILED(device->CreateUnorderedAccessView(face.reference.Get(),nullptr,&reference)))return fail("face-uav-creation-failed");
        ComPtr<ID3D11ComputeShader> shader;if(!shader_for(device.Get(),shader,1))return fail("face-delta-shader-failed");
        if(initialize){const UINT zero[4]{};context->ClearUnorderedAccessViewUint(coverage.Get(),zero);}
        {
            RestoreCS restore(context);ID3D11ShaderResourceView* input[4]{bs.Get(),as.Get(),nullptr,nullptr};ID3D11UnorderedAccessView* output[2]{coverage.Get(),reference.Get()};const UINT keep[2]{UINT(-1),UINT(-1)};
            ID3D11UnorderedAccessView* none[2]{};context->CSSetUnorderedAccessViews(0,2,none,keep);
            context->CSSetShader(shader.Get(),nullptr,0);context->CSSetShaderResources(0,4,input);context->CSSetUnorderedAccessViews(0,2,output,keep);
            context->Dispatch((bd.Width+7)/8,(bd.Height+7)/8,1);
        }
        if(reason)*reason="face-native-draw-delta-accumulated";return true;
    } catch(...) {return fail("face-delta-exception");}
}
bool generate_face(ID3D11DeviceContext* context,ID3D11Texture2D* normal,uint32_t normalViewFormat,
    ID3D11Texture2D* depth,const std::array<uint8_t,256>& ids,bool select,
    const FaceAccumulation& face,Candidate& result,const char** reason) noexcept {
    result={};if(reason)*reason="not-started";auto fail=[&](const char* why){if(reason)*reason=why;return false;};
    try {
        if(!context||!normal||!depth||!face.coverage||!face.reference||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return fail("face-guide-unavailable");
        ComPtr<ID3D11Device> device,ndev,ddev,cdev,rdev;context->GetDevice(&device);normal->GetDevice(&ndev);depth->GetDevice(&ddev);face.coverage->GetDevice(&cdev);face.reference->GetDevice(&rdev);
        if(!device||device.Get()!=ndev.Get()||device.Get()!=ddev.Get()||device.Get()!=cdev.Get()||device.Get()!=rdev.Get())return fail("face-guide-device-mismatch");
        ComPtr<ID3D11On12Device> on12;if(SUCCEEDED(device.As(&on12)))return fail("face-guide-d3d11on12-refused");
        D3D11_TEXTURE2D_DESC n{},d{},c{},r{};normal->GetDesc(&n);depth->GetDesc(&d);face.coverage->GetDesc(&c);face.reference->GetDesc(&r);
        if(!readonly_snapshot(n)||!readonly_snapshot(d)||skinprobe::typed_gbuffer_format(n.Format,normalViewFormat)!=24||
           n.Width!=d.Width||n.Height!=d.Height||n.Width!=c.Width||n.Height!=c.Height||n.Width!=r.Width||n.Height!=r.Height||
           c.Format!=DXGI_FORMAT_R32_UINT||r.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT)return fail("face-guide-format-shape-refused");
        DXGI_FORMAT stencil=d.Format==DXGI_FORMAT_R32G8X24_TYPELESS?DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
            d.Format==DXGI_FORMAT_R24G8_TYPELESS?DXGI_FORMAT_X24_TYPELESS_G8_UINT:DXGI_FORMAT_UNKNOWN;
        if(stencil==DXGI_FORMAT_UNKNOWN)return fail("face-guide-stencil-refused");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;sv.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
        ComPtr<ID3D11ShaderResourceView> ns,ds;if(FAILED(device->CreateShaderResourceView(normal,&sv,&ns)))return fail("face-final-normal-srv-failed");
        sv.Format=stencil;if(FAILED(device->CreateShaderResourceView(depth,&sv,&ds)))return fail("face-final-stencil-srv-failed");
        D3D11_TEXTURE2D_DESC td{};td.Width=n.Width;td.Height=n.Height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;
        td.Format=DXGI_FORMAT_R8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        Candidate next;ComPtr<ID3D11UnorderedAccessView> output;
        if(FAILED(device->CreateTexture2D(&td,nullptr,&next.texture))||FAILED(device->CreateShaderResourceView(next.texture.Get(),nullptr,&next.view))||FAILED(device->CreateUnorderedAccessView(next.texture.Get(),nullptr,&output)))return fail("face-r8-creation-failed");
        std::array<uint32_t,12> params{};params[0]=n.Width;params[1]=n.Height;params[2]=select?1:0;
        for(uint32_t id=0;id<256;++id)if(ids[id])params[4+id/32]|=1u<<(id%32);
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(params);cb.Usage=D3D11_USAGE_IMMUTABLE;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{params.data(),0,0};ComPtr<ID3D11Buffer> buffer;if(FAILED(device->CreateBuffer(&cb,&initial,&buffer)))return fail("face-selection-cb-failed");
        ComPtr<ID3D11ComputeShader> shader;if(!shader_for(device.Get(),shader,2))return fail("face-final-shader-failed");
        {
            RestoreCS restore(context);ID3D11ShaderResourceView* input[4]{ns.Get(),ds.Get(),face.coverageView.Get(),face.referenceView.Get()};const UINT keep=UINT(-1);
            ID3D11UnorderedAccessView* none[2]{};const UINT keeps[2]{UINT(-1),UINT(-1)};context->CSSetUnorderedAccessViews(0,2,none,keeps);
            context->CSSetShader(shader.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,buffer.GetAddressOf());context->CSSetShaderResources(0,4,input);context->CSSetUnorderedAccessViews(0,1,output.GetAddressOf(),&keep);
            context->Dispatch((n.Width+7)/8,(n.Height+7)/8,1);
        }
        result=std::move(next);if(reason)*reason="generated-face-only-r8-candidate";return true;
    } catch(...) {return fail("face-guide-exception");}
}

}
