#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/transparent_peeling.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include <array>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <cstdlib>
namespace mikumikustudio {
namespace {
using namespace d3dx;
template<class T> void Release(T*& p){if(p){p->Release();p=nullptr;}}
constexpr const char* kTechniques[]={"BufferShadowTec","BShadowTextureTec","BShadowSphiaTec","BShadowSphiaTextureTec","BShadowTexCd2Tec","BShadowTextureTexCd2Tec"};
const char* kShader=
#include "transparent_peeling_shader.inc"
;
std::vector<DWORD> ShaderCode(IDirect3DPixelShader9* shader){
 UINT bytes=0;if(!shader || FAILED(shader->GetFunction(nullptr,&bytes)))return {};
 std::vector<DWORD> code(bytes/4);if(FAILED(shader->GetFunction(code.data(),&bytes)))code.clear();return code;
}
std::vector<DWORD> ShaderCode(IDirect3DVertexShader9* shader){
 UINT bytes=0;if(!shader || FAILED(shader->GetFunction(nullptr,&bytes)))return {};
 std::vector<DWORD> code(bytes/4);if(FAILED(shader->GetFunction(code.data(),&bytes)))code.clear();return code;
}
HRESULT DrawViewportQuad(IDirect3DDevice9* d,const D3DVIEWPORT9& viewport,const D3DSURFACE_DESC& target){
 struct Quad {float x,y,z,w,u,v;};
 const float x=float(viewport.X)-.5f,y=float(viewport.Y)-.5f,w=float(viewport.Width),h=float(viewport.Height);
 const float u0=float(viewport.X)/target.Width,v0=float(viewport.Y)/target.Height,u1=float(viewport.X+viewport.Width)/target.Width,v1=float(viewport.Y+viewport.Height)/target.Height;
 const Quad quad[]={{x,y,0,1,u0,v0},{x+w,y,0,1,u1,v0},{x,y+h,0,1,u0,v1},{x+w,y+h,0,1,u1,v1}};
 d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
 return d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Quad));
}
struct Program {
 IDirect3DDevice9* device=nullptr;void* effect=nullptr;
 std::array<std::vector<DWORD>,6> ps,vs;
 ~Program(){if(effect)static_cast<IUnknown*>(effect)->Release();}
 bool Load(IDirect3DDevice9* d){
  device=d;D3DCAPS9 caps{};if(FAILED(d->GetDeviceCaps(&caps)) || caps.PixelShaderVersion<D3DPS_VERSION(3,0) || caps.NumSimultaneousRTs<2)return false;
  // Full builds embed the complete peeling effect separately. Resource 118
  // may be D3DX bytecode: trimming its zeros and appending HLSL corrupts it.
  auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(120),MAKEINTRESOURCEW(10));
  const bool legacySource=resource==nullptr;
  if(legacySource)resource=FindResourceW(nullptr,MAKEINTRESOURCEW(118),MAKEINTRESOURCEW(10));
  const DWORD size=resource?SizeofResource(nullptr,resource):0;
  const auto memory=resource?LoadResource(nullptr,resource):nullptr;
  const auto* data=memory?static_cast<const char*>(LockResource(memory)):nullptr;
  if(!data || !size)return false;
  std::string source(data,size);
  if(legacySource){
   const unsigned char compiledHeader[]={0x01,0x09,0xff,0xfe};
   if(size>=sizeof compiledHeader && !std::memcmp(data,compiledHeader,sizeof compiledHeader)){
    runtime_log::Trace("alpha-peel unavailable: compiled default effect has no peeling resource");return false;
   }
   while(!source.empty() && source.back()==0)source.pop_back();source+=kShader;
  }
  using Create=HRESULT(WINAPI*)(IDirect3DDevice9*,LPCVOID,UINT,const void*,void*,DWORD,void*,void**,void**);
  const auto create=reinterpret_cast<Create>(GetProcAddress(d3dx::Get().module,"D3DXCreateEffect"));
  if(!create)return false;
  void* errors=nullptr;
  const HRESULT hr=create(d,source.data(),UINT(source.size()),nullptr,nullptr,0,nullptr,&effect,&errors);
  if(errors){using Text=void*(WINAPI*)(void*);auto get=EffectMethod<Text,3>(errors);runtime_log::Trace("alpha-peel compile: %s",static_cast<const char*>(get(errors)));static_cast<IUnknown*>(errors)->Release();}
  if(FAILED(hr)){runtime_log::Trace("alpha-peel compile failed 0x%08x",unsigned(hr));return false;}
  for(const auto* technique:kTechniques){
   const std::string name=std::string("Mds_")+technique;
   if(FAILED(FxSetTechnique(effect,name.c_str())))return false;
  }
  for(unsigned i=0;i<6;++i){
   FxSetTechnique(effect,kTechniques[i]);UINT count=0;
   if(FAILED(FxBegin(effect,&count)))return false;
   if(FAILED(FxBeginPass(effect,0))){FxEnd(effect);return false;}
   IDirect3DPixelShader9* pixel=nullptr;IDirect3DVertexShader9* vertex=nullptr;
   d->GetPixelShader(&pixel);d->GetVertexShader(&vertex);ps[i]=ShaderCode(pixel);vs[i]=ShaderCode(vertex);
   Release(pixel);Release(vertex);FxEndPass(effect);FxEnd(effect);
  }
  return true;
 }
 int Match(IDirect3DDevice9* d){
  IDirect3DPixelShader9* pixel=nullptr;IDirect3DVertexShader9* vertex=nullptr;
  d->GetPixelShader(&pixel);d->GetVertexShader(&vertex);
  const auto p=ShaderCode(pixel),v=ShaderCode(vertex);Release(pixel);Release(vertex);
  if(p.empty() || v.empty())return -1;
  for(int i=0;i<6;++i)if(p==ps[i] && v==vs[i])return i;
  return -1;
 }
};
thread_local std::unique_ptr<Program> program;
void CopyParameters(void* from,void* to){
 using GetFloats=HRESULT(WINAPI*)(void*,const char*,float*,UINT);
 using GetMatrix=HRESULT(WINAPI*)(void*,const char*,D3DXMATRIXF*);
 using GetBool=HRESULT(WINAPI*)(void*,const char*,BOOL*);
 const char* vectors[]={"EgColor","ToonColor","LightDir","SpcColor","Place","DifColor","TexCAdd","TexCMul","SphCAdd","SphCMul","MatDifColor","MatAmbColor","MatEmsColor","MatSpcColor","MdsShadowMapSize"};
 for(const auto* name:vectors){float v[4];if(SUCCEEDED((EffectMethod<GetFloats,33>(from)(from,name,v,4))))FxSetFloatArray(to,name,v,4);}
 for(const auto* name:{"matLightViewProj","matWorldViewProj","matWorld","matRotate","matWRotate"}){
  D3DXMATRIXF m;if(SUCCEEDED((EffectMethod<GetMatrix,39>(from)(from,name,&m))))FxSetMatrix(to,name,&m);
 }
 for(const auto* name:{"parthf","spadd","transp"}){BOOL b=FALSE;if(SUCCEEDED((EffectMethod<GetBool,23>(from)(from,name,&b))))FxSetInt(to,name,b);}
}
}
struct TransparentPeeling::Impl {
 IDirect3DDevice9* d=nullptr;Program* program=nullptr;
 IDirect3DStateBlock9* saved=nullptr;IDirect3DSurface9* original=nullptr;IDirect3DSurface9* originalDepth=nullptr;
 IDirect3DSurface9* oldExtra[3]{};
 IDirect3DTexture9* color=nullptr;IDirect3DTexture9* depths[2]{};
 IDirect3DSurface9* colorSurface=nullptr;IDirect3DSurface9* depthSurfaces[2]{};IDirect3DSurface9* z=nullptr;
 IDirect3DTexture9* accumColor[2]{};IDirect3DTexture9* accumDepth[2]{};
 IDirect3DSurface9* accumColorSurface[2]{};IDirect3DSurface9* accumDepthSurface[2]{};
 IDirect3DSurface9* maskTarget=nullptr;IDirect3DTexture9* maskTexture=nullptr;IDirect3DSurface9* maskSurface=nullptr;
 IDirect3DQuery9* query=nullptr;D3DVIEWPORT9 viewport{};D3DSURFACE_DESC target{};
 unsigned layer=0,accumGroup=0;bool active=false,targets=false;
 bool maskPath=false,accumulated=false,committed=false;
 ~Impl(){
  if(d){d->SetTexture(12,nullptr);d->SetTexture(13,nullptr);d->SetTexture(14,nullptr);d->SetTexture(15,nullptr);d->SetRenderTarget(1,nullptr);d->SetDepthStencilSurface(nullptr);
   if(original)d->SetRenderTarget(0,original);if(originalDepth)d->SetDepthStencilSurface(originalDepth);
   for(unsigned i=0;i<3;++i)d->SetRenderTarget(i+1,oldExtra[i]);
   if(saved)saved->Apply();d->SetViewport(&viewport);
  }
  Release(query);Release(z);Release(maskSurface);Release(maskTexture);Release(maskTarget);
  for(auto*& p:accumColorSurface)Release(p);for(auto*& p:accumDepthSurface)Release(p);
  for(auto*& p:accumColor)Release(p);for(auto*& p:accumDepth)Release(p);
  Release(colorSurface);Release(color);for(auto*& p:depthSurfaces)Release(p);for(auto*& p:depths)Release(p);
  Release(original);Release(originalDepth);for(auto*& p:oldExtra)Release(p);Release(saved);
 }
 bool Prepare(){
  if(targets)return true;
  if(!originalDepth)return false;
  for(auto* surface:oldExtra)if(surface)return false;
  // Capability gate.  The accumulation is a manual blend: every private pass
  // writes its colour with blending disabled, so no float post-pixel-shader
  // blending and no MRT blending capability is required.  Everything checked
  // here is checked before a single draw, so an unsupported device leaves the
  // original render target and depth buffer untouched.
  D3DCAPS9 caps{};
  if(FAILED(d->GetDeviceCaps(&caps)) || caps.PixelShaderVersion<D3DPS_VERSION(3,0) || caps.NumSimultaneousRTs<2){
   runtime_log::Trace("alpha-peel unavailable: pixel shader 3.0 and two simultaneous render targets are required");return false;
  }
  D3DSURFACE_DESC depth{};
  if(FAILED(originalDepth->GetDesc(&depth)))return false;
  IDirect3D9* d3d9=nullptr;
  if(FAILED(d->GetDirect3D(&d3d9)) || !d3d9)return false;
  D3DDEVICE_CREATION_PARAMETERS creation{};
  if(FAILED(d->GetCreationParameters(&creation)))creation.AdapterOrdinal=0;
  D3DDISPLAYMODE mode{};
  D3DFORMAT adapterFormat=target.Format;
  if(SUCCEEDED(d3d9->GetAdapterDisplayMode(creation.AdapterOrdinal,&mode)))adapterFormat=mode.Format;
  const bool floatTarget=SUCCEEDED(d3d9->CheckDeviceFormat(creation.AdapterOrdinal,caps.DeviceType,adapterFormat,
   D3DUSAGE_RENDERTARGET,D3DRTYPE_TEXTURE,D3DFMT_A32B32G32R32F));
  if(!floatTarget){
   runtime_log::Trace("alpha-peel unavailable: A32B32G32R32F render targets are not supported");d3d9->Release();return false;
  }
  // A depth-stencil surface and every bound render target must share one
  // multisample type, and A32B32G32R32F cannot be multisampled, so the float
  // accumulation can never be bound beside a multisampled opaque depth.  That
  // depth is still needed as an occluder, so when it is multisampled the layer's
  // occlusion is captured first as a per-sample coverage mask in a multisampled
  // A16B16G16R16F target of the same sample type.  Only that coverage is
  // quantised (exactly, for power-of-two sample counts); the layer colour and
  // the whole accumulation stay FP32.  With a single-sampled opaque depth the
  // mask is unused and the accumulation depth-tests against that depth directly.
  maskPath=depth.MultiSampleType!=D3DMULTISAMPLE_NONE;
  if(maskPath){
   DWORD qualityLevels=0;
   if(FAILED(d3d9->CheckDeviceFormat(creation.AdapterOrdinal,caps.DeviceType,adapterFormat,
        D3DUSAGE_RENDERTARGET,D3DRTYPE_TEXTURE,D3DFMT_A16B16G16R16F)) ||
      FAILED(d3d9->CheckDeviceMultiSampleType(creation.AdapterOrdinal,caps.DeviceType,D3DFMT_A16B16G16R16F,TRUE,
        depth.MultiSampleType,&qualityLevels)) || qualityLevels==0 || depth.MultiSampleQuality>=qualityLevels){
    runtime_log::Trace("alpha-peel unavailable: A16B16G16R16F does not support the original depth multisample type");d3d9->Release();return false;
   }
   const DWORD quality=depth.MultiSampleQuality;
   if(FAILED(d->CreateRenderTarget(target.Width,target.Height,D3DFMT_A16B16G16R16F,depth.MultiSampleType,quality,FALSE,&maskTarget,nullptr)) ||
      FAILED(d->CreateTexture(target.Width,target.Height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&maskTexture,nullptr)) ||
      FAILED(maskTexture->GetSurfaceLevel(0,&maskSurface))){
    runtime_log::Trace("alpha-peel unavailable: multisampled coverage mask targets could not be created");d3d9->Release();return false;
   }
  }
  d3d9->Release();
  if(!program || !program->effect)return false;
  FxSetInt(program->effect,"MdsMasked",maskPath?1:0);
  if(FAILED(d->CreateTexture(target.Width,target.Height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A32B32G32R32F,D3DPOOL_DEFAULT,&color,nullptr)))return false;
  if(FAILED(color->GetSurfaceLevel(0,&colorSurface)))return false;
  for(unsigned i=0;i<2;++i){
   if(FAILED(d->CreateTexture(target.Width,target.Height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A32B32G32R32F,D3DPOOL_DEFAULT,&depths[i],nullptr)))return false;
   if(FAILED(depths[i]->GetSurfaceLevel(0,&depthSurfaces[i])))return false;
  }
  for(unsigned i=0;i<2;++i){
   if(FAILED(d->CreateTexture(target.Width,target.Height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A32B32G32R32F,D3DPOOL_DEFAULT,&accumColor[i],nullptr)) ||
      FAILED(accumColor[i]->GetSurfaceLevel(0,&accumColorSurface[i])))return false;
   if(FAILED(d->CreateTexture(target.Width,target.Height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A32B32G32R32F,D3DPOOL_DEFAULT,&accumDepth[i],nullptr)) ||
      FAILED(accumDepth[i]->GetSurfaceLevel(0,&accumDepthSurface[i])))return false;
  }
  if(FAILED(d->CreateDepthStencilSurface(target.Width,target.Height,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,TRUE,&z,nullptr)) || FAILED(d->CreateQuery(D3DQUERYTYPE_OCCLUSION,&query)))return false;
  runtime_log::Trace("alpha-peel targets: msaa=%u mask=%u",unsigned(depth.MultiSampleType),maskPath?1:0);
  targets=true;return true;
 }
};
TransparentPeeling::TransparentPeeling(IDirect3DDevice9* device):impl(std::make_unique<Impl>()){
 auto& p=*impl;p.d=device;if(!device)return;
 device->GetViewport(&p.viewport);device->GetRenderTarget(0,&p.original);device->GetDepthStencilSurface(&p.originalDepth);
 for(unsigned i=0;i<3;++i)device->GetRenderTarget(i+1,&p.oldExtra[i]);
 if(!p.original || FAILED(p.original->GetDesc(&p.target)) || FAILED(device->CreateStateBlock(D3DSBT_ALL,&p.saved)))return;
 if(!program || program->device!=device){program=std::make_unique<Program>();if(!program->Load(device)){program.reset();return;}}
 p.program=program.get();p.saved->Apply();
}
TransparentPeeling::~TransparentPeeling()=default;
bool TransparentPeeling::Ready()const{return impl->program!=nullptr;}
bool TransparentPeeling::NativePass(){return Ready() && impl->program->Match(impl->d)>=0;}
bool TransparentPeeling::PrepareTargets(){return Ready() && impl->Prepare();}
bool TransparentPeeling::BeginLayer(unsigned layer){
 auto& p=*impl;auto* d=p.d;if(!p.targets)return false;p.layer=layer;
 d->SetTexture(12,nullptr);d->SetTexture(13,nullptr);d->SetTexture(14,nullptr);d->SetTexture(15,nullptr);d->SetDepthStencilSurface(nullptr);
 if(layer==0){
  // Zero both ping-pong groups before the first layer.  Each target is cleared
  // on its own so the result does not depend on how Clear treats MRT.
  d->SetRenderTarget(1,nullptr);d->SetRenderTarget(2,nullptr);d->SetRenderTarget(3,nullptr);
  for(unsigned i=0;i<2;++i){
   if(FAILED(d->SetRenderTarget(0,p.accumColorSurface[i])) || FAILED(d->Clear(0,nullptr,D3DCLEAR_TARGET,0,0,0)))return false;
   if(FAILED(d->SetRenderTarget(0,p.accumDepthSurface[i])) || FAILED(d->Clear(0,nullptr,D3DCLEAR_TARGET,0,0,0)))return false;
  }
  p.accumGroup=0;p.accumulated=false;p.committed=false;
 }
 if(FAILED(d->SetRenderTarget(0,p.colorSurface)) || FAILED(d->SetRenderTarget(1,p.depthSurfaces[layer%2])) || FAILED(d->SetDepthStencilSurface(p.z)))return false;
 d->SetViewport(&p.viewport);d->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
 d->SetRenderState(D3DRS_COLORWRITEENABLE,15);d->SetRenderState(D3DRS_COLORWRITEENABLE1,15);
 if(FAILED(d->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0,0,0)))return false;
 // Refresh the mask-path switch every layer so the shared effect instance can
 // never keep a stale value from another device or another peel.
 FxSetInt(p.program->effect,"MdsMasked",p.maskPath?1:0);
 return SUCCEEDED(p.query->Issue(D3DISSUE_BEGIN));
}
bool TransparentPeeling::BeginMaterial(void* originalEffect){
 auto& p=*impl;const int match=p.program->Match(p.d);if(match<0)return false;
 auto* fx=p.program->effect;CopyParameters(originalEffect,fx);
 const float size[]={float(p.target.Width),float(p.target.Height),1.0f/p.target.Width,1.0f/p.target.Height};
 FxSetFloatArray(fx,"MdsTarget",size,4);FxSetInt(fx,"MdsFirst",p.layer==0);
 const std::string technique=std::string("Mds_")+kTechniques[match];FxSetTechnique(fx,technique.c_str());
 UINT count=0;if(FAILED(FxBegin(fx,&count)))return false;
 if(FAILED(FxBeginPass(fx,0))){FxEnd(fx);return false;}
 p.active=true;auto* d=p.d;
 d->SetTexture(15,p.layer? p.depths[(p.layer-1)%2]:nullptr);
 d->SetRenderState(D3DRS_ZENABLE,TRUE);d->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);d->SetRenderState(D3DRS_ZFUNC,D3DCMP_GREATEREQUAL);
 d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);d->SetRenderState(D3DRS_STENCILENABLE,FALSE);
 d->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);d->SetRenderState(D3DRS_COLORWRITEENABLE,15);d->SetRenderState(D3DRS_COLORWRITEENABLE1,15);
 return true;
}
void TransparentPeeling::EndMaterial(){auto& p=*impl;if(p.active){FxEndPass(p.program->effect);FxEnd(p.program->effect);p.active=false;}}
int TransparentPeeling::EndLayer(){
 auto& p=*impl;auto* d=p.d;if(FAILED(p.query->Issue(D3DISSUE_END)))return -1;
 DWORD pixels=0;HRESULT hr=S_FALSE;const ULONGLONG start=GetTickCount64();
 while((hr=p.query->GetData(&pixels,sizeof(pixels),D3DGETDATA_FLUSH))==S_FALSE){if(GetTickCount64()-start>10000)return -1;Sleep(0);}
 if(FAILED(hr))return -1;if(!pixels)return 0;
 if(const char* prefix=std::getenv("MIKUMIKUSTUDIO_PEEL_DUMP")) {
  if(p.layer<3 || p.layer==8 || p.layer==127 || p.layer==1023) {
   IDirect3DSurface9* surface=nullptr;
   if(SUCCEEDED(d->CreateOffscreenPlainSurface(p.target.Width,p.target.Height,D3DFMT_A32B32G32R32F,D3DPOOL_SYSTEMMEM,&surface,nullptr)) && SUCCEEDED(d->GetRenderTargetData(p.depthSurfaces[p.layer%2],surface))) {
    D3DLOCKED_RECT lock{};
    if(SUCCEEDED(surface->LockRect(&lock,nullptr,D3DLOCK_READONLY))) {
     std::ofstream file(std::string(prefix)+"-"+std::to_string(p.layer)+".bin",std::ios::binary);
     file.write(reinterpret_cast<const char*>(&p.target.Width),4);file.write(reinterpret_cast<const char*>(&p.target.Height),4);
     for(UINT row=0;row<p.target.Height;++row)file.write(static_cast<const char*>(lock.pBits)+row*lock.Pitch,p.target.Width*16);
     surface->UnlockRect();
    }
   }
   Release(surface);
  }
 }

 // Fold the layer into the private float accumulation.  Float render targets
 // cannot be blended on every device, so the previous group is copied first and
 // the pixel shader then blends by hand with blending disabled.  Pixels this
 // layer drops - no fragment, or behind the opaque depth - keep the copied
 // colour and nearest depth.  The original render target and depth buffer are
 // not touched here: the whole peel stays private until Commit().
 const unsigned previous=p.accumGroup,next=previous^1;
 d->SetRenderTarget(1,nullptr);d->SetRenderTarget(2,nullptr);d->SetRenderTarget(3,nullptr);
 if(p.maskPath){
  // The opaque depth is multisampled, so it cannot be bound beside the
  // single-sampled float accumulation.  Capture its occlusion first as a
  // per-sample coverage mask: clear the mask (the depth buffer itself stays
  // untouched), then draw one full-viewport quad whose oDepth is this layer's
  // depth and let the per-sample LESSEQUAL test drop the samples that sit
  // behind opaque geometry.  Resolving the mask averages those samples into
  // the covered fraction of each pixel.
  if(FAILED(d->SetDepthStencilSurface(p.originalDepth)) || FAILED(d->SetRenderTarget(0,p.maskTarget)))return -1;
  d->SetViewport(&p.viewport);d->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
  // The mask pass has to read the sample-level depth test, so full sample
  // coverage and every sample enabled are set explicitly instead of inheriting
  // whatever the selection pass left behind.
  d->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,TRUE);d->SetRenderState(D3DRS_MULTISAMPLEMASK,0xFFFFFFFFu);
  d->SetRenderState(D3DRS_ZENABLE,TRUE);d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);d->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);
  d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);d->SetRenderState(D3DRS_STENCILENABLE,FALSE);
  d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);d->SetRenderState(D3DRS_LIGHTING,FALSE);d->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);
  d->SetRenderState(D3DRS_COLORWRITEENABLE,15);
  bool maskOk=true;
  if(FAILED(d->Clear(0,nullptr,D3DCLEAR_TARGET,0,0,0)))maskOk=false;
  auto* fx=p.program->effect;UINT maskCount=0;
  if(maskOk){
   FxSetTechnique(fx,"MdsMask");
   if(FAILED(FxBegin(fx,&maskCount)))maskOk=false;
   else{
    if(FAILED(FxBeginPass(fx,0))){FxEnd(fx);maskOk=false;}
    else{
     d->SetTexture(15,p.depths[p.layer%2]);
     maskOk=SUCCEEDED(DrawViewportQuad(d,p.viewport,p.target));
     FxEndPass(fx);FxEnd(fx);
    }
   }
  }
  // Both surfaces involved in the resolve have to be free of render target and
  // depth bindings, and a null render target 0 is an invalid call: switch
  // render target 0 to the layer colour surface, which takes part in no
  // resolve, after dropping the depth buffer.
  d->SetDepthStencilSurface(nullptr);
  if(FAILED(d->SetRenderTarget(0,p.colorSurface)))return -1;
  if(!maskOk)return -1;
  if(FAILED(d->StretchRect(p.maskTarget,nullptr,p.maskSurface,nullptr,D3DTEXF_NONE)))return -1;
 }
 // Copy the previous group and blend this layer into the next one.  With a mask
 // the occlusion is already folded into the coverage, so these passes bind no
 // depth buffer at all; with a single-sampled opaque depth they keep
 // depth-testing against it directly.
 if(p.maskPath)d->SetDepthStencilSurface(nullptr);
 else if(FAILED(d->SetDepthStencilSurface(p.originalDepth))){d->SetDepthStencilSurface(nullptr);return -1;}
 if(FAILED(d->SetRenderTarget(0,p.accumColorSurface[next])) || FAILED(d->SetRenderTarget(1,p.accumDepthSurface[next])))return -1;
 d->SetViewport(&p.viewport);
 d->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
 d->SetRenderState(D3DRS_ZENABLE,FALSE);d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
 d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);d->SetRenderState(D3DRS_STENCILENABLE,FALSE);
 d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);d->SetRenderState(D3DRS_LIGHTING,FALSE);d->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);
 d->SetRenderState(D3DRS_COLORWRITEENABLE,15);d->SetRenderState(D3DRS_COLORWRITEENABLE1,15);
 auto* fx=p.program->effect;UINT count=0;
 FxSetTechnique(fx,"MdsAccumCopy");
 if(FAILED(FxBegin(fx,&count)))return -1;
 if(FAILED(FxBeginPass(fx,0))){FxEnd(fx);return -1;}
 d->SetTexture(14,p.accumColor[previous]);d->SetTexture(15,p.accumDepth[previous]);
 hr=DrawViewportQuad(d,p.viewport,p.target);
 FxEndPass(fx);FxEnd(fx);
 if(FAILED(hr))return -1;
 FxSetTechnique(fx,"MdsAccumulate");
 if(FAILED(FxBegin(fx,&count)))return -1;
 if(FAILED(FxBeginPass(fx,0))){FxEnd(fx);return -1;}
 if(!p.maskPath){d->SetRenderState(D3DRS_ZENABLE,TRUE);d->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);}
 d->SetTexture(12,p.maskPath? p.maskTexture : nullptr);
 d->SetTexture(13,p.accumColor[previous]);d->SetTexture(14,p.color);d->SetTexture(15,p.depths[p.layer%2]);
 hr=DrawViewportQuad(d,p.viewport,p.target);
 FxEndPass(fx);FxEnd(fx);
 if(FAILED(hr))return -1;
 p.accumGroup=next;
 p.accumulated=true;
 return 1;
}
bool TransparentPeeling::Commit(){
 auto& p=*impl;auto* d=p.d;
 if(!p.targets || !p.accumulated || p.committed)return false;
 // Restore the original render target and depth buffer and blend the final
 // accumulation in once: the source is already premultiplied, so
 // ONE / INVSRCALPHA reproduces the layer-by-layer source-over chain exactly,
 // without the 8-bit round trip every layer used to cost.  This blend runs on
 // the original LDR target, which every device can blend.
 if(FAILED(d->SetRenderTarget(1,nullptr)) || FAILED(d->SetRenderTarget(2,nullptr)) || FAILED(d->SetRenderTarget(3,nullptr)))return false;
 d->SetDepthStencilSurface(nullptr);
 if(FAILED(d->SetRenderTarget(0,p.original)) || FAILED(d->SetDepthStencilSurface(p.originalDepth)))return false;
 d->SetViewport(&p.viewport);
 d->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
 d->SetRenderState(D3DRS_ZENABLE,TRUE);d->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);d->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);
 d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ONE);d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
 d->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
 d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);d->SetRenderState(D3DRS_STENCILENABLE,FALSE);d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);d->SetRenderState(D3DRS_LIGHTING,FALSE);
 d->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);d->SetRenderState(D3DRS_COLORWRITEENABLE,15);
 auto* fx=p.program->effect;FxSetTechnique(fx,"MdsCommit");UINT count=0;
 if(FAILED(FxBegin(fx,&count)))return false;
 if(FAILED(FxBeginPass(fx,0))){FxEnd(fx);return false;}
 d->SetTexture(14,p.accumColor[p.accumGroup]);d->SetTexture(15,p.accumDepth[p.accumGroup]);
 const HRESULT hr=DrawViewportQuad(d,p.viewport,p.target);
 FxEndPass(fx);FxEnd(fx);
 if(FAILED(hr))return false;
 p.committed=true;
 return true;
}
void ClearTransparentPeelingCache(){program.reset();}
} // namespace mikumikustudio
