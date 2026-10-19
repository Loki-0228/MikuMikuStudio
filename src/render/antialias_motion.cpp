#define NOMINMAX
#include "mikumikustudio/antialias_motion.hpp"
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/deformed_mesh_capture.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/graphics_runtime.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/vertex_capture.hpp"
#include "antialias_motion_cache.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace mikumikustudio::antialias::motion {
#if !MMD_ORIGINAL_PROFILE
using Microsoft::WRL::ComPtr;
namespace {
using Float4 = std::array<float,4>;
void Check(HRESULT hr,const char* message){if(FAILED(hr))throw std::runtime_error(message);}
Float4 Transform(const Float4& p,const D3DMATRIX& m) {
    Float4 result{};
    for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)result[c]+=p[r]*m.m[r][c];
    return result;
}
D3DMATRIX Multiply(const D3DMATRIX& a,const D3DMATRIX& b) {
    D3DMATRIX result{};
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)
        for(unsigned k=0;k<4;++k)result.m[r][c]+=a.m[r][k]*b.m[k][c];
    return result;
}
void Hash(std::uint64_t& value,const void* data,std::size_t size) {
    const auto* bytes=static_cast<const unsigned char*>(data);
    for(std::size_t i=0;i<size;++i)value=(value^bytes[i])*1099511628211ull;
}
template<class Shader>std::vector<DWORD> Code(Shader* shader) {
    if(!shader)return {};
    UINT bytes=0;Check(shader->GetFunction(nullptr,&bytes),"Cannot describe motion source shader");
    std::vector<DWORD> code((bytes+3)/4);
    Check(shader->GetFunction(code.data(),&bytes),"Cannot read motion source shader");return code;
}
// A pixel shader that changes depth no longer shares the captured VS depth.
bool WritesDepth(const std::vector<DWORD>& code) {
    for(std::size_t at=1;at<code.size();) {
        const DWORD instruction=code[at++],opcode=instruction&D3DSI_OPCODE_MASK;
        if(opcode==D3DSIO_END)break;
        if(opcode==D3DSIO_COMMENT){at+=(instruction&D3DSI_COMMENTSIZE_MASK)>>D3DSI_COMMENTSIZE_SHIFT;continue;}
        if(opcode==D3DSIO_TEXDEPTH)return true;
        const std::size_t length=(instruction&D3DSI_INSTLENGTH_MASK)>>D3DSI_INSTLENGTH_SHIFT;
        if(!length)continue;
        if(at+length>code.size())return true;
        const DWORD token=code[at];
        const DWORD type=((token&D3DSP_REGTYPE_MASK)>>D3DSP_REGTYPE_SHIFT)|
            ((token&D3DSP_REGTYPE_MASK2)>>D3DSP_REGTYPE_SHIFT2);
        if((token&0x80000000u)&&type==D3DSPR_DEPTHOUT)return true;
        at+=length;
    }
    return false;
}
struct SavedState {
    IDirect3DDevice9* device;
    ComPtr<IDirect3DStateBlock9> block;
    std::array<ComPtr<IDirect3DSurface9>,4> targets;
    ComPtr<IDirect3DSurface9> depth;
    D3DVIEWPORT9 viewport{};
    explicit SavedState(IDirect3DDevice9* d):device(d) {
        Check(d->CreateStateBlock(D3DSBT_ALL,&block),"Cannot save motion capture state");
        Check(d->GetRenderTarget(0,&targets[0]),"Cannot save motion capture target");
        for(unsigned i=1;i<4;++i)d->GetRenderTarget(i,&targets[i]);
        d->GetDepthStencilSurface(&depth);d->GetViewport(&viewport);
    }
    ~SavedState() {
        for(unsigned i=0;i<4;++i)device->SetTexture(i,nullptr);
        for(unsigned i=0;i<2;++i)device->SetTexture(D3DVERTEXTEXTURESAMPLER0+i,nullptr);
        device->SetDepthStencilSurface(nullptr);
        for(unsigned i=0;i<4;++i)device->SetRenderTarget(i,targets[i].Get());
        device->SetDepthStencilSurface(depth.Get());block->Apply();device->SetViewport(&viewport);
    }
};
struct Vertex {Float4 current,previous;float depth,previousDepth;};
struct Snapshot {
    std::vector<Float4> clip;std::vector<float> depth;
    std::shared_ptr<const vertex_capture::GpuPositions> gpu;
    D3DMATRIX inverseProjection{};
    float jitterX=0,jitterY=0;
    unsigned first=0,count=0;
};
using Key=std::tuple<std::uintptr_t,unsigned,std::uint64_t,unsigned>;
const char kShader[]=R"hlsl(
float4 RasterJitter : register(c0);
row_major float4x4 FixedTransform : register(c0);
float4 FixedVS(float3 position:POSITION0):POSITION0 {return mul(float4(position,1),FixedTransform);}
struct VIn {float4 current:POSITION0;float4 previous:TEXCOORD0;float2 depth:TEXCOORD1;};
struct VOut {float4 position:POSITION0;float4 current:TEXCOORD0;float4 previous:TEXCOORD1;float2 depth:TEXCOORD2;};
VOut VS(VIn v) {
 VOut o;o.position=v.current;o.position.xy+=RasterJitter.xy*v.current.w;
 o.current=v.current;o.previous=v.previous;o.depth=v.depth;return o;
}
sampler2D CurrentPositions : register(s0);
sampler2D PreviousPositions : register(s1);
float4 CurrentMetrics : register(c1); // width, height, first source vertex, 0
float4 PreviousMetrics : register(c2);
row_major float4x4 CurrentInverseProjection : register(c4);
row_major float4x4 PreviousInverseProjection : register(c8);
VOut GpuVS(float4 raster:POSITION0) {
 float2 uv=raster.xy*float2(0.5,-0.5)+0.5;
 float index=floor(uv.x*CurrentMetrics.x)+floor(uv.y*CurrentMetrics.y)*CurrentMetrics.x;
 index+=CurrentMetrics.z-PreviousMetrics.z;
 float row=floor(index/PreviousMetrics.x);
 float2 previousUV=(float2(index-row*PreviousMetrics.x,row)+0.5)/PreviousMetrics.xy;
 float4 current=tex2Dlod(CurrentPositions,float4(uv,0,0));
 float4 previous=tex2Dlod(PreviousPositions,float4(previousUV,0,0));
 VOut o;o.position=current;
 current.xy-=RasterJitter.xy*current.w;previous.xy-=RasterJitter.zw*previous.w;
 o.current=current;o.previous=previous;
 float4 currentView=mul(current,CurrentInverseProjection);
 float4 previousView=mul(previous,PreviousInverseProjection);
 o.depth=float2(abs(currentView.w)>1e-10?currentView.z/currentView.w:0,
               abs(previousView.w)>1e-10?previousView.z/previousView.w:0);
 return o;
}
sampler2D Coverage : register(s0);
float4 ViewportMap : register(c0);
float4 Controls : register(c1);
float4 Motion(VOut v,float alpha) {
 clip(alpha-0.0001);
 float2 current=v.current.xy/v.current.w*float2(0.5,-0.5)+0.5;
 float2 previous=v.previous.xy/max(v.previous.w,0.000001)*float2(0.5,-0.5)+0.5;
 // Depth-writing alpha materials (notably MMD hair) still have tracked
 // geometry. Rejecting all partial coverage left them jittering indefinitely.
 // The resolve still validates depth; non-depth-writing blends remain invalid.
 bool valid=Controls.z>0.5&&v.previous.w>0.000001&&v.depth.y>0;
 float2 velocity=(previous-current)*ViewportMap.xy;
 // Subtracting two captured clip positions introduces subpixel round-off.
 // Do not let that noise drive SMAA's velocity-change rejection on a still mesh.
 if(all(abs(velocity)<Controls.xy/64.0))velocity=0;
 return float4(velocity,max(v.depth.x,0),valid?v.depth.y:0);
}
float4 PS(VOut v,float2 pixel:VPOS):COLOR0 {
 return Motion(v,tex2D(Coverage,(pixel+0.5)*Controls.xy).a);
}
float4 OpaquePS(VOut v):COLOR0 {return Motion(v,1);}
)hlsl";
struct State final:deformed_mesh_capture::DrawObserver {
    MMDApp* app{};
    ComPtr<IDirect3DSurface9> scene,surface,coverageSurface,depth,coverageDepth;
    ComPtr<IDirect3DTexture9> texture,coverage;
    ComPtr<IDirect3DVertexShader9> vertexShader,gpuVertexShader,fixedCaptureShader;
    ComPtr<IDirect3DPixelShader9> pixelShader,opaquePixelShader;
    ComPtr<IDirect3DVertexDeclaration9> declaration,gpuDeclaration;
    D3DSURFACE_DESC description{};
    D3DVIEWPORT9 viewport{};
    D3DMATRIX inverseProjection{};
    float jitterX=0,jitterY=0;
    bool collecting=false,pending=false,failed=false,cleared=false;
    bool gpuAvailable=false;
    unsigned draws=0,validDraws=0;
    unsigned coverageReplays=0,opaqueCoverageSkips=0;
    float maxMotionPixels=0;
    std::map<Key,Snapshot> previous,current;
    std::map<std::tuple<std::uintptr_t,unsigned,std::uint64_t>,unsigned> ordinals;
    detail::VertexCache vertexCache;
    std::string lastError;
    ~State(){deformed_mesh_capture::StopObserving(this);}
    void Fail(const char* message) {
        failed=true;
        if(lastError!=message){
            lastError=message;runtime_log::Write("TAA motion unavailable: %s",message);
            runtime_log::Trace("TAA motion unavailable frame=%d pass=%u: %s",
                app?app->CurrentFrame():-1,app?unsigned(app->ActiveRenderPass()):0,message);
        }
    }
    bool SceneDraw(IDirect3DDevice9* device) {
        if(!collecting||failed)return false;
        ComPtr<IDirect3DSurface9> target;
        return SUCCEEDED(device->GetRenderTarget(0,&target))&&target.Get()==scene.Get();
    }
    void Allocate(IDirect3DDevice9* device,IDirect3DSurface9* target) {
        D3DSURFACE_DESC desc{};Check(target->GetDesc(&desc),"Cannot describe TAA motion target");
        if(texture&&surface&&coverage&&coverageSurface&&depth&&coverageDepth&&vertexShader&&pixelShader&&opaquePixelShader&&declaration&&
           (!gpuAvailable||(gpuVertexShader&&gpuDeclaration&&fixedCaptureShader))&&
           desc.Width==description.Width&&desc.Height==description.Height)return;
        // These are standard D3D9 capabilities on NVIDIA, AMD and Intel. No
        // vendor depth aliases, resolve commands or driver settings are used.
        D3DCAPS9 caps{};D3DDEVICE_CREATION_PARAMETERS creation{};D3DDISPLAYMODE mode{};
        ComPtr<IDirect3D9> factory;
        Check(device->GetDeviceCaps(&caps),"Cannot query motion device capabilities");
        if(caps.VertexShaderVersion<D3DVS_VERSION(3,0)||caps.PixelShaderVersion<D3DPS_VERSION(3,0))
            throw std::runtime_error("Motion capture requires vertex/pixel shader 3.0");
        Check(device->GetCreationParameters(&creation),"Cannot query motion adapter");
        Check(device->GetDirect3D(&factory),"Cannot query motion Direct3D interface");
        Check(factory->GetAdapterDisplayMode(creation.AdapterOrdinal,&mode),"Cannot query motion adapter format");
        for(auto format:{D3DFMT_A32B32G32R32F,D3DFMT_A8R8G8B8}) {
            Check(factory->CheckDeviceFormat(creation.AdapterOrdinal,creation.DeviceType,mode.Format,
                D3DUSAGE_RENDERTARGET,D3DRTYPE_TEXTURE,format),"Motion render-target format is unsupported");
            Check(factory->CheckDepthStencilMatch(creation.AdapterOrdinal,creation.DeviceType,mode.Format,
                format,D3DFMT_D24S8),"Motion render-target/depth pairing is unsupported");
        }
        // VTF is optional in D3D9. Query the actual adapter, never its vendor;
        // devices without RGBA32F vertex sampling retain the compatible path.
        gpuAvailable=!device->GetSoftwareVertexProcessing()&&SUCCEEDED(factory->CheckDeviceFormat(creation.AdapterOrdinal,creation.DeviceType,mode.Format,
            D3DUSAGE_QUERY_VERTEXTEXTURE,D3DRTYPE_TEXTURE,D3DFMT_A32B32G32R32F));
        texture.Reset();surface.Reset();coverage.Reset();coverageSurface.Reset();depth.Reset();coverageDepth.Reset();
        previous.clear();description=desc;
        Check(device->CreateTexture(desc.Width,desc.Height,1,D3DUSAGE_RENDERTARGET,
            D3DFMT_A32B32G32R32F,D3DPOOL_DEFAULT,&texture,nullptr),"RGBA32F motion target unavailable");
        Check(texture->GetSurfaceLevel(0,&surface),"Motion target surface unavailable");
        Check(device->CreateTexture(desc.Width,desc.Height,1,D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&coverage,nullptr),"Motion alpha target unavailable");
        Check(coverage->GetSurfaceLevel(0,&coverageSurface),"Motion alpha surface unavailable");
        for(auto* z:{depth.GetAddressOf(),coverageDepth.GetAddressOf()})Check(device->CreateDepthStencilSurface(desc.Width,desc.Height,
            D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,TRUE,z,nullptr),"Motion depth unavailable");
        if(!vertexShader||!pixelShader||!opaquePixelShader||!declaration||(gpuAvailable&&(!gpuVertexShader||!gpuDeclaration||!fixedCaptureShader))) {
            vertexShader.Reset();pixelShader.Reset();opaquePixelShader.Reset();declaration.Reset();
            gpuVertexShader.Reset();gpuDeclaration.Reset();fixedCaptureShader.Reset();
            ComPtr<ID3DBlob> vs,ps,error;
            const auto compile=[&](const char* entry,const char* profile,ID3DBlob** out) {
                error.Reset();
                if(FAILED(graphics_runtime::Compile(kShader,sizeof(kShader)-1,"TAA motion",nullptr,nullptr,
                    entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,out,&error)))
                    throw std::runtime_error(error?static_cast<const char*>(error->GetBufferPointer()):"Motion shader compilation failed");
            };
            compile("VS","vs_3_0",vs.GetAddressOf());compile("PS","ps_3_0",ps.GetAddressOf());
            Check(device->CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()),&vertexShader),"Motion vertex shader unavailable");
            Check(device->CreatePixelShader(static_cast<const DWORD*>(ps->GetBufferPointer()),&pixelShader),"Motion pixel shader unavailable");
            ps.Reset();compile("OpaquePS","ps_3_0",ps.GetAddressOf());
            Check(device->CreatePixelShader(static_cast<const DWORD*>(ps->GetBufferPointer()),&opaquePixelShader),"Opaque motion shader unavailable");
            const D3DVERTEXELEMENT9 elements[]={
                {0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
                {0,16,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},
                {0,32,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,1},D3DDECL_END()};
            Check(device->CreateVertexDeclaration(elements,&declaration),"Motion vertex declaration unavailable");
            if(gpuAvailable) {
                vs.Reset();compile("GpuVS","vs_3_0",vs.GetAddressOf());
                Check(device->CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()),&gpuVertexShader),"GPU motion shader unavailable");
                const D3DVERTEXELEMENT9 gpuElements[]={
                    {0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
                Check(device->CreateVertexDeclaration(gpuElements,&gpuDeclaration),"GPU motion declaration unavailable");
                vs.Reset();compile("FixedVS","vs_3_0",vs.GetAddressOf());
                Check(device->CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()),&fixedCaptureShader),"Fixed-function GPU capture shader unavailable");
            }
        }
    }
    void Capture(IDirect3DDevice9* device,D3DPRIMITIVETYPE type,INT base,UINT minVertex,UINT vertexCount,
                 UINT firstIndex,UINT primitives,const void* object,unsigned material) {
        if(!primitives||!SceneDraw(device))return;
        bool grid=false;
        if(type<D3DPT_TRIANGLELIST) {
            ComPtr<IDirect3DIndexBuffer9> indices;
            device->GetIndices(&indices);
            grid=type==D3DPT_LINELIST && indices && indices.Get()==app->GroundGridIndices();
            if(!grid)return;
            object=app->GroundGridIndices();material=UINT_MAX;
        }
        D3DVIEWPORT9 drawViewport{};
        Check(device->GetViewport(&drawViewport),"Cannot read motion source viewport");
        if(drawViewport.X!=viewport.X||drawViewport.Y!=viewport.Y||drawViewport.Width!=viewport.Width||
           drawViewport.Height!=viewport.Height||drawViewport.MinZ!=viewport.MinZ||drawViewport.MaxZ!=viewport.MaxZ) {
            Fail("Material uses a different scene viewport");return;
        }
        // Inverted-hull outlines use a separate, already deformed vertex
        // buffer. Track that geometry too, otherwise every moving black edge
        // would lose temporal history despite having a valid source object.
        if(!object&&app->ActiveRenderPass()==AccessoryRenderPass::ModelOutline) {
            object=app->ActiveRenderObject();material=UINT_MAX;
        }
        if(type!=D3DPT_TRIANGLELIST && !grid){Fail("Non-triangle-list material geometry");return;}
        D3DCAPS9 caps{};Check(device->GetDeviceCaps(&caps),"Cannot query motion render-target limits");
        bool extraTarget=false;
        for(unsigned slot=1;slot<std::min(4u,unsigned(caps.NumSimultaneousRTs));++slot) {
            ComPtr<IDirect3DSurface9> extra;
            const auto hr=device->GetRenderTarget(slot,&extra);
            if(hr!=D3DERR_NOTFOUND)Check(hr,"Cannot inspect motion source render targets");
            extraTarget|=bool(extra);
        }
        if(extraTarget && !NativeOutlineMaskDraw(app,device)) {
            Fail("Material writes multiple scene render targets");return;
        }
        ComPtr<IDirect3DIndexBuffer9> indices;
        Check(device->GetIndices(&indices),"Cannot read motion indices");
        if(!indices)throw std::runtime_error("Motion source has no index buffer");
        D3DINDEXBUFFER_DESC desc{};Check(indices->GetDesc(&desc),"Cannot describe motion indices");
        const unsigned indexBytes=desc.Format==D3DFMT_INDEX32?4:2;
        const std::uint64_t indexCount=std::uint64_t(primitives)*(grid?2:3);
        if(indexCount>48000000||(std::uint64_t(firstIndex)+indexCount)*indexBytes>desc.Size)
            throw std::runtime_error("Motion index range is invalid");
        std::vector<unsigned> meshIndices(static_cast<std::size_t>(indexCount));
        unsigned first=UINT_MAX,last=0;void* bytes=nullptr;
        Check(indices->Lock(firstIndex*indexBytes,UINT(indexCount*indexBytes),&bytes,D3DLOCK_READONLY),"Cannot lock motion indices");
        bool validIndices=true;
        for(std::size_t i=0;i<meshIndices.size();++i) {
            const std::int64_t index=std::int64_t(base)+(indexBytes==4?static_cast<DWORD*>(bytes)[i]:static_cast<WORD*>(bytes)[i]);
            if(index<0||index>INT_MAX){validIndices=false;break;}
            meshIndices[i]=unsigned(index);first=std::min(first,unsigned(index));last=std::max(last,unsigned(index));
        }
        indices->Unlock();if(!validIndices||last-first>16000000)throw std::runtime_error("Motion vertex range is invalid");
        const unsigned count=last-first+1;
        for(auto& index:meshIndices)index-=first;
        ComPtr<IDirect3DVertexShader9> vs;ComPtr<IDirect3DPixelShader9> ps;
        device->GetVertexShader(&vs);device->GetPixelShader(&ps);
        const auto vsCode=Code(vs.Get()),psCode=Code(ps.Get());
        if(WritesDepth(psCode)){Fail("Pixel shader replaces geometry depth");return;}
        std::uint64_t signature=14695981039346656037ull;
        Hash(signature,vsCode.data(),vsCode.size()*sizeof(DWORD));
        Hash(signature,psCode.data(),psCode.size()*sizeof(DWORD));
        Hash(signature,meshIndices.data(),meshIndices.size()*sizeof(unsigned));Hash(signature,&first,sizeof(first));
        // Stable source buffers distinguish reloads and topology replacement.
        ComPtr<IDirect3DVertexBuffer9> vb;UINT offset=0,stride=0;
        Check(device->GetStreamSource(0,&vb,&offset,&stride),"Cannot read motion vertex stream");
        const auto identity=reinterpret_cast<std::uintptr_t>(vb.Get());Hash(signature,&identity,sizeof(identity));
        const auto ordinalKey=std::make_tuple(reinterpret_cast<std::uintptr_t>(object),material,signature);
        const Key key=std::tuple_cat(ordinalKey,std::make_tuple(ordinals[ordinalKey]++));
        Snapshot snapshot;snapshot.first=first;snapshot.count=count;
        snapshot.inverseProjection=inverseProjection;snapshot.jitterX=jitterX;snapshot.jitterY=jitterY;
        if(vs&&gpuAvailable) {
            std::string error;
            if(!vertexCache.CaptureGpu(device,vsCode,base,minVertex,vertexCount,first,count,snapshot.gpu,error))
                throw std::runtime_error("GPU vertex shader capture: "+error);
        } else if(vs) {
            std::string error;
            if(!vertexCache.Capture(device,vsCode,base,minVertex,vertexCount,first,count,snapshot.clip,error))
                throw std::runtime_error("Final vertex shader capture: "+error);
        } else {
            ComPtr<IDirect3DVertexDeclaration9> decl;device->GetVertexDeclaration(&decl);
            D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};UINT n=MAXD3DDECLLENGTH+1;
            if(!decl||FAILED(decl->GetDeclaration(elements,&n)))throw std::runtime_error("Fixed motion declaration unavailable");
            const D3DVERTEXELEMENT9* position=nullptr;
            for(UINT i=0;i<n&&elements[i].Stream!=0xff;++i)
                if(elements[i].Usage==D3DDECLUSAGE_POSITION&&elements[i].UsageIndex==0)position=&elements[i];
            if(!position||position->Type!=D3DDECLTYPE_FLOAT3||position->Stream!=0)
                throw std::runtime_error("Unsupported fixed-function motion position");
            if(!vb)throw std::runtime_error("Fixed-function motion vertex buffer is unavailable");
            D3DVERTEXBUFFER_DESC vdesc{};Check(vb->GetDesc(&vdesc),"Cannot describe motion vertices");
            if(std::uint64_t(offset)+std::uint64_t(last)*stride+position->Offset+12>vdesc.Size)
                throw std::runtime_error("Motion vertex buffer bounds exceeded");
            D3DMATRIX world{},view{},projection{};
            device->GetTransform(D3DTS_WORLD,&world);device->GetTransform(D3DTS_VIEW,&view);device->GetTransform(D3DTS_PROJECTION,&projection);
            const auto transform=Multiply(Multiply(world,view),projection);
            if(gpuAvailable) {
                SavedState preserve(device);
                Check(device->SetVertexShader(fixedCaptureShader.Get()),"Bind fixed-function GPU capture");
                Check(device->SetVertexShaderConstantF(0,&transform.m[0][0],4),"Bind fixed-function GPU transform");
                std::string error;
                if(!vertexCache.CaptureGpu(device,Code(fixedCaptureShader.Get()),base,minVertex,vertexCount,first,count,snapshot.gpu,error))
                    throw std::runtime_error("Fixed-function GPU capture: "+error);
            } else {
                snapshot.clip.resize(count);
                Check(vb->Lock(0,0,&bytes,D3DLOCK_READONLY),"Cannot lock deformed motion vertices");
                for(unsigned i=0;i<count;++i) {
                    Float4 point{0,0,0,1};std::memcpy(point.data(),static_cast<const char*>(bytes)+offset+std::size_t(first+i)*stride+position->Offset,12);
                    snapshot.clip[i]=Transform(point,transform);
                }
                vb->Unlock();
            }
        }
        if(!snapshot.gpu)snapshot.depth.resize(count);
        for(unsigned i=0;i<snapshot.clip.size();++i) {
            auto& clip=snapshot.clip[i];clip[0]-=jitterX*clip[3];clip[1]-=jitterY*clip[3];
            const auto view=Transform(clip,inverseProjection);
            snapshot.depth[i]=std::abs(view[3])>1e-10f?view[2]/view[3]:0;
            for(float x:clip)if(!std::isfinite(x))throw std::runtime_error("Non-finite final vertex position");
        }
        const auto old=previous.find(key);
        const bool matching=object&&old!=previous.end()&&old->second.count==count&&old->second.first==first&&
            bool(old->second.gpu)==bool(snapshot.gpu);
        if(matching&&!snapshot.gpu)for(unsigned i=0;i<count;++i) {
            const auto& now=snapshot.clip[i];const auto& before=old->second.clip[i];
            if(now[3]>1e-6f&&before[3]>1e-6f) {
                const float dx=(before[0]/before[3]-now[0]/now[3])*.5f*viewport.Width;
                const float dy=(before[1]/before[3]-now[1]/now[3])*.5f*viewport.Height;
                maxMotionPixels=std::max(maxMotionPixels,std::sqrt(dx*dx+dy*dy));
            }
        }
        std::vector<Vertex> vertices;
        if(!snapshot.gpu) {
            vertices.resize(count);
            for(unsigned i=0;i<count;++i)vertices[i]={snapshot.clip[i],matching?old->second.clip[i]:snapshot.clip[i],
                snapshot.depth[i],matching?old->second.depth[i]:0};
        }
        DWORD blend=0,sourceBlend=0,destinationBlend=0,stencil=0,stencilFunc=0,zfunc=0,zEnabled=0,zWrite=0;
        device->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend);device->GetRenderState(D3DRS_SRCBLEND,&sourceBlend);
        device->GetRenderState(D3DRS_DESTBLEND,&destinationBlend);device->GetRenderState(D3DRS_STENCILENABLE,&stencil);
        device->GetRenderState(D3DRS_STENCILFUNC,&stencilFunc);
        device->GetRenderState(D3DRS_ZFUNC,&zfunc);
        device->GetRenderState(D3DRS_ZENABLE,&zEnabled);device->GetRenderState(D3DRS_ZWRITEENABLE,&zWrite);
        const bool opaqueBlend=!blend||(sourceBlend==D3DBLEND_SRCALPHA&&destinationBlend==D3DBLEND_INVSRCALPHA)||
            (sourceBlend==D3DBLEND_ONE&&destinationBlend==D3DBLEND_ZERO);
        const bool historyAllowed=matching&&opaqueBlend&&zEnabled&&zWrite&&(!stencil||stencilFunc==D3DCMP_ALWAYS)&&
            (zfunc==D3DCMP_LESS||zfunc==D3DCMP_LESSEQUAL);
        DWORD clipPlanes=0,scissor=0,fill=0;device->GetRenderState(D3DRS_CLIPPLANEENABLE,&clipPlanes);
        device->GetRenderState(D3DRS_SCISSORTESTENABLE,&scissor);
        device->GetRenderState(D3DRS_FILLMODE,&fill);
        const bool opaqueCoverage=!ps&&!clipPlanes&&!scissor&&fill==D3DFILL_SOLID&&deformed_mesh_capture::OpaqueCoverage();
        SavedState saved(device);
        device->SetDepthStencilSurface(nullptr);
        for(unsigned i=1;i<4;++i)device->SetRenderTarget(i,nullptr);
        if(!cleared) {
            Check(device->SetRenderTarget(0,surface.Get()),"Cannot bind motion target");
            Check(device->SetDepthStencilSurface(depth.Get()),"Cannot bind motion depth");
            device->SetViewport(&viewport);
            Check(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0,1,0),"Cannot clear motion target");cleared=true;
        }
        device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE,15);device->SetRenderState(D3DRS_STENCILENABLE,FALSE);
        device->SetRenderState(D3DRS_ZENABLE,TRUE);device->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);
        device->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);device->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);
        // A verified opaque native material needs no mask. MME and alpha
        // textures still replay the original shader, including clip/discard.
        if(!opaqueCoverage) {
            device->SetDepthStencilSurface(nullptr);
            Check(device->SetRenderTarget(0,coverageSurface.Get()),"Cannot bind coverage target");
            Check(device->SetDepthStencilSurface(coverageDepth.Get()),"Cannot bind coverage depth");
            device->SetViewport(&viewport);
            Check(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0,1,0),"Cannot clear coverage target");
            Check(device->DrawIndexedPrimitive(type,base,minVertex,vertexCount,firstIndex,primitives),"Coverage replay failed");
            ++coverageReplays;
        } else ++opaqueCoverageSkips;
        device->SetDepthStencilSurface(nullptr);
        Check(device->SetRenderTarget(0,surface.Get()),"Cannot bind motion field");
        Check(device->SetDepthStencilSurface(depth.Get()),"Cannot bind motion field depth");
        device->SetViewport(&viewport);
        device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);device->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE,0);device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,FALSE);
        device->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID);device->SetRenderState(D3DRS_FOGENABLE,FALSE);
        device->SetVertexDeclaration(snapshot.gpu?gpuDeclaration.Get():declaration.Get());
        device->SetVertexShader(snapshot.gpu?gpuVertexShader.Get():vertexShader.Get());
        device->SetPixelShader(opaqueCoverage?opaquePixelShader.Get():pixelShader.Get());
        const auto& prior=matching?old->second:snapshot;
        const float jitter[]={jitterX,jitterY,prior.jitterX,prior.jitterY};device->SetVertexShaderConstantF(0,jitter,1);
        const float controls[]={float(viewport.Width)/description.Width,float(viewport.Height)/description.Height,
            float(viewport.X)/description.Width,float(viewport.Y)/description.Height,
            1.f/description.Width,1.f/description.Height,historyAllowed?1.f:0.f,opaqueCoverage?1.f:0.f};
        device->SetPixelShaderConstantF(0,controls,2);device->SetTexture(0,coverage.Get());
        for(auto sampler:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})device->SetSamplerState(0,sampler,D3DTEXF_POINT);
        device->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE);device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);
        device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);device->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
        if(snapshot.gpu) {
            const auto& now=*snapshot.gpu;const auto& before=*prior.gpu;
            const float metrics[]={float(now.width),float(now.height),float(now.firstVertex),0,
                float(before.width),float(before.height),float(before.firstVertex),0};
            device->SetVertexShaderConstantF(1,metrics,2);
            device->SetVertexShaderConstantF(4,&snapshot.inverseProjection.m[0][0],4);
            device->SetVertexShaderConstantF(8,&prior.inverseProjection.m[0][0],4);
            device->SetTexture(D3DVERTEXTEXTURESAMPLER0,now.texture.Get());
            device->SetTexture(D3DVERTEXTEXTURESAMPLER1,before.texture.Get());
            for(unsigned i=0;i<2;++i) {
                const DWORD sampler=D3DVERTEXTEXTURESAMPLER0+i;
                device->SetSamplerState(sampler,D3DSAMP_MINFILTER,D3DTEXF_POINT);
                device->SetSamplerState(sampler,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
                device->SetSamplerState(sampler,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
                device->SetSamplerState(sampler,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
                device->SetSamplerState(sampler,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
                device->SetSamplerState(sampler,D3DSAMP_SRGBTEXTURE,FALSE);
            }
            Check(device->SetStreamSource(0,now.rasterPositions.Get(),0,16),"Bind GPU motion lookup");
            // Retain the original index buffer. No expanded CPU vertex/index
            // upload is needed to read either frame's captured positions.
            const std::int64_t replayBase=std::int64_t(base)-now.firstVertex;
            if(replayBase<INT_MIN||replayBase>INT_MAX)throw std::runtime_error("GPU motion index base is out of bounds");
            Check(device->DrawIndexedPrimitive(type,INT(replayBase),UINT(std::int64_t(first)-base),
                count,firstIndex,primitives),"GPU motion geometry replay failed");
        } else Check(device->DrawIndexedPrimitiveUP(type,0,count,primitives,meshIndices.data(),D3DFMT_INDEX32,
            vertices.data(),sizeof(Vertex)),"Motion geometry replay failed");
        ++draws;if(historyAllowed)++validDraws;
        if(object)current.emplace(key,std::move(snapshot));
    }
    void Indexed(IDirect3DDevice9* device,D3DPRIMITIVETYPE type,INT base,UINT minVertex,UINT vertexCount,
                 UINT firstIndex,UINT primitives,const void* object,unsigned material) noexcept override {
        try{Capture(device,type,base,minVertex,vertexCount,firstIndex,primitives,object,material);}
        catch(const std::exception& e){Fail(e.what());}catch(...){Fail("Native geometry capture failed");}
    }
    void Unsupported(IDirect3DDevice9* device,D3DPRIMITIVETYPE type,UINT count) noexcept override {
        if(!count||type<D3DPT_TRIANGLELIST||!SceneDraw(device))return;
        // Background/UI fixed-function screen quads carry no 3D history. An
        // external shader screen pass can move every pixel and invalidates it.
        ComPtr<IDirect3DPixelShader9> ps;ComPtr<IDirect3DVertexShader9> vs;
        device->GetPixelShader(&ps);device->GetVertexShader(&vs);
        if(ps||vs)Fail("Untracked non-indexed shader or full-screen effect");
    }
};
std::map<MMDApp*,std::unique_ptr<State>> states;
}
#endif
bool Begin(MMDApp* app,IDirect3DDevice9* device,IDirect3DSurface9* sceneTarget,
           const D3DVIEWPORT9& viewport,const D3DMATRIX& projection,float jitterX,float jitterY) {
#if MMD_ORIGINAL_PROFILE
    return false;
#else
    if(!app||!device||!sceneTarget)return false;
    // Mesh export reconstructs world positions using its unjittered camera.
    // Do not inject viewport sampling jitter into those exported coordinates.
    if(deformed_mesh_capture::Active())return false;
    auto& entry=states[app];if(!entry)entry=std::make_unique<State>();auto& s=*entry;
    if(s.pending||s.viewport.X!=viewport.X||s.viewport.Y!=viewport.Y||
       s.viewport.Width!=viewport.Width||s.viewport.Height!=viewport.Height)
        s.previous.clear();
    s.app=app;s.collecting=false;s.pending=false;s.current.clear();s.ordinals.clear();
    s.vertexCache.Clear();
    s.failed=false;s.cleared=false;s.draws=s.validDraws=0;s.maxMotionPixels=0;s.viewport=viewport;s.jitterX=jitterX;s.jitterY=jitterY;
    s.coverageReplays=s.opaqueCoverageSkips=0;
    try {
        s.Allocate(device,sceneTarget);s.scene=sceneTarget;
        if(!d3dx::Get().Load()||!d3dx::Get().inverse(reinterpret_cast<d3dx::D3DXMATRIXF*>(&s.inverseProjection),nullptr,
            reinterpret_cast<const d3dx::D3DXMATRIXF*>(&projection)))throw std::runtime_error("Motion projection is singular");
        std::string error;if(!deformed_mesh_capture::Observe(*app,&s,error))throw std::runtime_error(error);
        s.collecting=true;s.pending=true;return true;
    } catch(const std::exception& e){s.Fail(e.what());s.previous.clear();return false;}
#endif
}
IDirect3DTexture9* Resolve(MMDApp* app,IDirect3DDevice9* device) {
#if MMD_ORIGINAL_PROFILE
    return nullptr;
#else
    const auto found=states.find(app);if(found==states.end())return nullptr;auto& s=*found->second;
    s.collecting=false;
    if(s.failed)return nullptr;
    if(!s.draws) {
        bool sceneObjects=false;
        for(int i=0;i<kModelSlotCount;++i)sceneObjects|=app->ModelSlot(i)!=nullptr;
        for(int i=0;i<255;++i)sceneObjects|=app->AccessorySlot(i)!=nullptr;
        // An empty native scene has no reprojection samples. A renderer that
        // bypasses the native geometry path must never reuse stale motion.
        if(sceneObjects)return nullptr;
        try {
            SavedState saved(device);device->SetDepthStencilSurface(nullptr);
            for(unsigned i=1;i<4;++i)device->SetRenderTarget(i,nullptr);
            Check(device->SetRenderTarget(0,s.surface.Get()),"Cannot clear empty motion scene");
            Check(device->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0),"Cannot clear empty motion scene");
        }catch(const std::exception& e){s.Fail(e.what());return nullptr;}
    } else if(!s.cleared||s.current.empty())return nullptr;
    return s.texture.Get();
#endif
}
void Commit(MMDApp* app) {
#if !MMD_ORIGINAL_PROFILE
    const auto found=states.find(app);if(found==states.end())return;auto& s=*found->second;
    if(s.pending&&!s.failed){s.previous=std::move(s.current);s.pending=false;s.collecting=false;}
#endif
}
void Cancel(MMDApp* app) {
#if !MMD_ORIGINAL_PROFILE
    const auto found=states.find(app);if(found==states.end())return;auto& s=*found->second;
    if(s.pending){s.previous.clear();s.current.clear();s.pending=false;s.collecting=false;}
#endif
}
void Release(MMDApp* app) {
#if !MMD_ORIGINAL_PROFILE
    states.erase(app);
#endif
}
Stats GetStats(MMDApp* app) {
#if MMD_ORIGINAL_PROFILE
    return {};
#else
    const auto found=states.find(app);if(found==states.end())return {};
    const auto& s=*found->second;return {s.draws,s.validDraws,s.maxMotionPixels,s.vertexCache.captures,s.vertexCache.hits,
        s.vertexCache.gpuCaptures,s.vertexCache.cpuReadbacks,s.coverageReplays,s.opaqueCoverageSkips};
#endif
}
IDirect3DTexture9* GetTexture(MMDApp* app) {
#if MMD_ORIGINAL_PROFILE
    return nullptr;
#else
    const auto found=states.find(app);
    return found==states.end()||found->second->failed?nullptr:found->second->texture.Get();
#endif
}
}
