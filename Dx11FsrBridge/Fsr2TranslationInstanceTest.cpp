// Exercise production translation dispatch, substituting NGX/FSR hooks only.
#define NOMINMAX
#define DX11FSRBRIDGE_RELEASE_RUNTIME 1
#include "Fsr2TranslationLayer.cpp"
#include <wrl/client.h>
#include <d3d11sdklayers.h>
#include <iostream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D11 failure");}
void require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
static unsigned creates=0,destroys=0;
static FfxErrorCode create_mock(FfxFsr2Context* c,const FfxFsr2ContextDescription*){
 std::memset(c,0,sizeof(*c));c->data[0]=++creates;return FFX_OK;}
static FfxErrorCode destroy_mock(FfxFsr2Context*){++destroys;return FFX_OK;}
static unsigned currentHandle=0;static bool currentReset=false;static void* currentMotion=nullptr;
static FfxErrorCode dispatch_mock(FfxFsr2Context* c,const FfxFsr2DispatchDescription* d){
 currentHandle=c->data[0];currentReset=d->reset;currentMotion=d->motionVectors.resource;return FFX_OK;}
int main(){try{
 ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;
 check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx));
 ComPtr<ID3D11InfoQueue> info;check(dev.As(&info));
 auto srv=[&](unsigned w,unsigned h,DXGI_FORMAT f){D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=f;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> t;check(dev->CreateTexture2D(&d,nullptr,&t));ComPtr<ID3D11ShaderResourceView> v;check(dev->CreateShaderResourceView(t.Get(),nullptr,&v));return v;};
 auto color=srv(32,20,DXGI_FORMAT_R8G8B8A8_UNORM),motion=srv(32,20,DXGI_FORMAT_R10G10B10A2_UNORM),depth=srv(32,20,DXGI_FORMAT_R32_FLOAT);
 D3D11_TEXTURE2D_DESC od{};od.Width=64;od.Height=40;od.MipLevels=od.ArraySize=1;od.Format=DXGI_FORMAT_R8G8B8A8_UNORM;od.SampleDesc.Count=1;od.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
 ComPtr<ID3D11Texture2D> out;check(dev->CreateTexture2D(&od,nullptr,&out));
 g_translation_context_create=create_mock;g_translation_context_dispatch=dispatch_mock;g_translation_context_destroy=destroy_mock;g_hook_entry_detected=true;
 Fsr2TranslationFrame frame;frame.context=ctx.Get();frame.color=color.Get();frame.motion=motion.Get();frame.depth=depth.Get();frame.output=out.Get();frame.render_width=32;frame.render_height=20;frame.output_width=64;frame.output_height=40;
 auto run=[&](unsigned key){frame.instance_key=key;auto result=dispatch_fsr2_translation(frame);require(result.succeeded,result.error.c_str());return result;};
 require(run(101).context_created && currentReset,"first scene history reset");auto a=currentHandle;auto* ma=currentMotion;
 require(run(202).context_created && currentReset,"first character history reset");auto b=currentHandle;auto* mb=currentMotion;
 require(a!=b && ma!=mb,"character and scene share contract/resources");
 for(unsigned i=0;i<20;i++){require(!run(101).context_created && currentHandle==a && !currentReset && currentMotion==ma,"scene context lost");require(!run(202).context_created && currentHandle==b && !currentReset && currentMotion==mb,"character context lost");}
 frame.reset=true;run(202);require(currentReset,"explicit character reset lost");frame.reset=false;run(101);require(!currentReset,"character reset contaminated scene");
 frame.output_width=65;od.Width=65;out.Reset();check(dev->CreateTexture2D(&od,nullptr,&out));frame.output=out.Get();require(run(202).context_created && currentHandle!=b && currentReset,"instance resize not rebuilt");
 frame.output_width=64;od.Width=64;out.Reset();check(dev->CreateTexture2D(&od,nullptr,&out));frame.output=out.Get();require(!run(101).context_created && currentHandle==a,"other instance rebuilt by resize");
 auto sceneHandle=currentHandle;
 frame.depth_inverted=false;
 require(run(202).context_created && currentReset,"depth contract change did not rebuild instance");
 frame.depth_inverted=true;
 require(!run(101).context_created && currentHandle==sceneHandle,"depth contract contaminated other instance");
 frame.motion_vectors_jittered=true;
 require(run(202).context_created && currentReset,"jittered MV contract change did not rebuild");
 frame.motion_vectors_jittered=false;
 require(!run(101).context_created && currentHandle==sceneHandle,"MV contract contaminated other instance");
 for(unsigned k=3;k<=8;k++)run(1000+k);
 frame.instance_key=9000;auto rejected=dispatch_fsr2_translation(frame);require(!rejected.succeeded && g_sessions.size()==8,"unbounded session cache");
 reset_fsr2_translation_context();require(g_sessions.empty() && g_session==nullptr && destroys==creates,"session resources not retired");
 ctx->ClearState();ctx->Flush();unsigned errors=0;
 for(UINT64 i=0;i<info->GetNumStoredMessages();i++){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<char> data(size);auto* m=reinterpret_cast<D3D11_MESSAGE*>(data.data());check(info->GetMessage(i,m,&size));if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::cerr<<m->pDescription<<'\n';errors++;}}
 require(!errors,"debug-layer warning/error");
 std::cout<<"PASS: production per-instance dispatch, separate contexts/textures/history, 40 alternating scene/character frames, instance resize/reset/create-flag isolation, bounded cache and cleanup; zero D3D11 debug warnings/errors\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
