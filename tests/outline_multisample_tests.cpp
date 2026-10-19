// MAINTENANCE: regression-keep; hardware MSAA native-outline coverage and scene restoration.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../src/render/outline_multisample.hpp"
#include "mikumikustudio/deformed_mesh_capture.hpp"
#include "mikumikustudio/graphics_runtime.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <wrl/client.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace mikumikustudio;
using Microsoft::WRL::ComPtr;
namespace sampled = mikumikustudio::antialias::outline_multisample;
namespace {
constexpr unsigned kSide=128;
unsigned checks=0;
void Check(bool ok,const char* message) {
    ++checks;
    if(!ok) { std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1); }
}
void Hr(HRESULT result,const char* message) {
    if(FAILED(result))std::fprintf(stderr,"HRESULT=0x%08lX\n",result);
    Check(SUCCEEDED(result),message);
}
decltype(&graphics_runtime::Compile) originalCompile=nullptr;
unsigned rejectedCompiles=0;
HRESULT RejectNativeCompile(const void* source,SIZE_T size,const char* name,
    const D3D_SHADER_MACRO* defines,ID3DInclude* include,const char* entry,
    const char* target,UINT flags1,UINT flags2,ID3DBlob** code,ID3DBlob** error) {
    if(name && entry && std::strcmp(name,"native outline MSAA")==0 && std::strcmp(entry,"outlineVertex")==0) {
        ++rejectedCompiles;
        if(code)*code=nullptr;
        if(error)*error=nullptr;
        return E_OUTOFMEMORY;
    }
    return originalCompile(source,size,name,defines,include,entry,target,flags1,flags2,code,error);
}
struct CompileFailure {
    void* target=reinterpret_cast<void*>(&graphics_runtime::Compile);
    CompileFailure() {
        const auto initialized=MH_Initialize();
        Check(initialized==MH_OK || initialized==MH_ERROR_ALREADY_INITIALIZED,"initialize process-local compile fault");
        rejectedCompiles=0;
        Check(MH_CreateHook(target,reinterpret_cast<void*>(&RejectNativeCompile),reinterpret_cast<void**>(&originalCompile))==MH_OK,
            "install process-local native outline compile fault");
        Check(MH_EnableHook(target)==MH_OK,"enable native outline compile fault");
    }
    ~CompileFailure() {
        // Keep the module's independent native draw observers installed.
        Check(MH_DisableHook(target)==MH_OK,"disable native outline compile fault");
        Check(MH_RemoveHook(target)==MH_OK,"remove native outline compile fault");
    }
};
D3DMATRIX Identity() { D3DMATRIX m{};m._11=m._22=m._33=m._44=1;return m; }
struct Parameters {
    float topWidth=.25f,bottomWidth=.25f,slope=.25f,phase=.17f;
    unsigned normals=0,displacement=0,additionalUv=0,occlusion=0;
    bool oblique=false,transparent=false,uniform=false,vertexColors=false,inset=false;
    DWORD color=0xff000000;
};
struct SavedBindings {
    ComPtr<IDirect3DSurface9> color,depth,extra[3];
    ComPtr<IDirect3DVertexShader9> vertex;
    ComPtr<IDirect3DPixelShader9> pixel;
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    ComPtr<IDirect3DVertexBuffer9> stream;
    ComPtr<IDirect3DIndexBuffer9> indices;
    ComPtr<IDirect3DBaseTexture9> textures[4];
    D3DVIEWPORT9 viewport{};
    D3DMATRIX transforms[3]{};
    RECT scissor{};
    UINT offset=0,stride=0;
    DWORD fvf=0;
    float vsConstants[32]{},psConstants[32]{};
    static constexpr D3DRENDERSTATETYPE renderStates[]={
        D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ZFUNC,D3DRS_CULLMODE,
        D3DRS_ALPHABLENDENABLE,D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP,
        D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_SRCBLENDALPHA,D3DRS_DESTBLENDALPHA,D3DRS_BLENDOPALPHA,
        D3DRS_ALPHATESTENABLE,D3DRS_ALPHAFUNC,D3DRS_ALPHAREF,D3DRS_COLORWRITEENABLE,
        D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,D3DRS_STENCILENABLE,
        D3DRS_MULTISAMPLEMASK,D3DRS_MULTISAMPLEANTIALIAS};
    static constexpr D3DSAMPLERSTATETYPE samplerStates[]={
        D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,
        D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_SRGBTEXTURE};
    DWORD render[_countof(renderStates)]{},samplers[2][_countof(samplerStates)]{},extraWrites[3]{};
    unsigned extraCount=0;
    explicit SavedBindings(IDirect3DDevice9* device) {
        Hr(device->GetRenderTarget(0,&color),"capture main target");Hr(device->GetDepthStencilSurface(&depth),"capture depth");
        D3DCAPS9 caps{};Hr(device->GetDeviceCaps(&caps),"capture caps");extraCount=std::min(3u,caps.NumSimultaneousRTs?unsigned(caps.NumSimultaneousRTs-1):0u);
        for(unsigned i=0;i<extraCount;++i) {
            const auto result=device->GetRenderTarget(i+1,&extra[i]);Check(result==S_OK || result==D3DERR_NOTFOUND,"capture optional target");
            Hr(device->GetRenderState(D3DRENDERSTATETYPE(D3DRS_COLORWRITEENABLE1+i),&extraWrites[i]),"capture extra write mask");
        }
        Hr(device->GetVertexShader(&vertex),"capture vertex shader");Hr(device->GetPixelShader(&pixel),"capture pixel shader");
        Hr(device->GetVertexDeclaration(&declaration),"capture declaration");Hr(device->GetFVF(&fvf),"capture FVF");
        Hr(device->GetStreamSource(0,&stream,&offset,&stride),"capture source stream");Hr(device->GetIndices(&indices),"capture indices");
        Hr(device->GetViewport(&viewport),"capture viewport");Hr(device->GetScissorRect(&scissor),"capture scissor");
        const D3DTRANSFORMSTATETYPE kinds[]={D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION};
        for(unsigned i=0;i<3;++i)Hr(device->GetTransform(kinds[i],&transforms[i]),"capture transforms");
        Hr(device->GetVertexShaderConstantF(0,vsConstants,8),"capture VS constants");Hr(device->GetPixelShaderConstantF(0,psConstants,8),"capture PS constants");
        for(unsigned i=0;i<4;++i)Hr(device->GetTexture(i,&textures[i]),"capture texture");
        for(unsigned i=0;i<_countof(renderStates);++i)Hr(device->GetRenderState(renderStates[i],&render[i]),"capture render state");
        for(unsigned s=0;s<2;++s)for(unsigned i=0;i<_countof(samplerStates);++i)
            Hr(device->GetSamplerState(s,samplerStates[i],&samplers[s][i]),"capture sampler state");
    }
    void Compare(IDirect3DDevice9* device) const {
        const SavedBindings after(device);
        Check(color==after.color && depth==after.depth,"Finish restores outer color/depth bindings");
        for(unsigned i=0;i<extraCount;++i)Check(extra[i]==after.extra[i] && extraWrites[i]==after.extraWrites[i],"Finish restores extra MRTs and write masks");
        Check(vertex==after.vertex && pixel==after.pixel && declaration==after.declaration && fvf==after.fvf,"Finish restores non-null shaders and declaration");
        Check(stream==after.stream && offset==after.offset && stride==after.stride && indices==after.indices,"Finish restores vertex/index sources");
        Check(std::memcmp(&viewport,&after.viewport,sizeof(viewport))==0 && std::memcmp(&scissor,&after.scissor,sizeof(scissor))==0,"Finish restores viewport and scissor");
        Check(std::memcmp(transforms,after.transforms,sizeof(transforms))==0,"Finish restores transforms");
        Check(std::memcmp(vsConstants,after.vsConstants,sizeof(vsConstants))==0 && std::memcmp(psConstants,after.psConstants,sizeof(psConstants))==0,"Finish restores VS/PS constants");
        for(unsigned i=0;i<4;++i)Check(textures[i]==after.textures[i],"Finish restores texture bindings");
        Check(std::memcmp(render,after.render,sizeof(render))==0 && std::memcmp(samplers,after.samplers,sizeof(samplers))==0,"Finish restores blend/depth/filter state");
    }
};
struct Fixture {
    HWND window=nullptr;
    ComPtr<IDirect3D9> d3d;
    ComPtr<IDirect3DDevice9> device;
    ComPtr<IDirect3DSurface9> mainColor,mainDepth,readback;
    ComPtr<IDirect3DVertexBuffer9> body,edge,foreground;
    ComPtr<IDirect3DIndexBuffer9> indices;
    ComPtr<IDirect3DTexture9> sentinelTexture;
    ComPtr<IDirect3DVertexShader9> sentinelVertex;
    ComPtr<IDirect3DPixelShader9> sentinelPixel;
    struct Targets { ComPtr<IDirect3DSurface9> color,depth,resolved; };
    std::map<unsigned,Targets> references;
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    std::unique_ptr<D3DRenderer> renderer=std::make_unique<D3DRenderer>();
    std::unique_ptr<mdl::ModelRecord> model=std::make_unique<mdl::ModelRecord>();
    Fixture() {
        window=CreateWindowW(L"STATIC",L"Native outline multisample regression",WS_POPUP,0,0,kSide,kSide,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(window!=nullptr,"create hidden GPU test window");d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));Check(bool(d3d),"native D3D9");
        D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow=window;pp.BackBufferWidth=pp.BackBufferHeight=kSide;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
        pp.EnableAutoDepthStencil=TRUE;pp.AutoDepthStencilFormat=D3DFMT_D24S8;
        Hr(d3d->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&device),"create real HAL device");
        Hr(device->GetRenderTarget(0,&mainColor),"acquire main scene");Hr(device->GetDepthStencilSurface(&mainDepth),"acquire main depth");
        app->state=MMDAppState{};std::memset(renderer.get(),0,sizeof(*renderer));app->Renderer()=renderer.get();
        renderer->device=device.Get();renderer->d3d9=d3d.Get();renderer->presentParameters=pp;
        model->vertexCount=4;model->loadComplete=1;model->edgeScale=1;
        Hr(device->CreateVertexBuffer(4*96,0,0,D3DPOOL_MANAGED,&body,nullptr),"allocate body vertices");
        Hr(device->CreateVertexBuffer(4*sizeof(mdl::EdgeVertex),0,0,D3DPOOL_MANAGED,&edge,nullptr),"allocate original hull");
        Hr(device->CreateVertexBuffer(4*sizeof(mdl::SkinnedVertexBase),0,0,D3DPOOL_MANAGED,&foreground,nullptr),"allocate foreground occluder");
        Hr(device->CreateIndexBuffer(6*sizeof(WORD),0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&indices,nullptr),"allocate indices");
        model->vertexBuffer=body.Get();model->vertexBuffer2=edge.Get();model->indexBuffer=indices.Get();
        const WORD faces[]={0,1,2,2,1,3};void* data=nullptr;Hr(indices->Lock(0,0,&data,0),"map indices");std::memcpy(data,faces,sizeof(faces));indices->Unlock();
        Hr(device->CreateOffscreenPlainSurface(kSide,kSide,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr),"allocate GPU readback");
        Hr(device->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&sentinelTexture,nullptr),"allocate sentinel texture");
        static constexpr char sentinel[]=R"hlsl(
float4 markerColor : register(c7);
float4 testVertex(float4 position:POSITION0):POSITION0 { return position; }
float4 testPixel():COLOR0 { return markerColor; }
)hlsl";
        ComPtr<ID3DBlob> vs,ps,error;
        Hr(graphics_runtime::Compile(sentinel,sizeof(sentinel)-1,"outline-msaa-sentinel",nullptr,nullptr,"testVertex","vs_3_0",0,0,&vs,&error),"compile sentinel VS");
        Hr(graphics_runtime::Compile(sentinel,sizeof(sentinel)-1,"outline-msaa-sentinel",nullptr,nullptr,"testPixel","ps_3_0",0,0,&ps,&error),"compile sentinel PS");
        Hr(device->CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()),&sentinelVertex),"create sentinel VS");
        Hr(device->CreatePixelShader(static_cast<const DWORD*>(ps->GetBufferPointer()),&sentinelPixel),"create sentinel PS");
    }
    ~Fixture() { sampled::Cancel(app.get());sampled::Release(app.get());if(window)DestroyWindow(window); }
    unsigned char* Raw() { return reinterpret_cast<unsigned char*>(model.get()); }
    D3DVIEWPORT9 Viewport(const Parameters& p) { return p.inset?D3DVIEWPORT9{9,7,110,111,.05f,.95f}:D3DVIEWPORT9{0,0,kSide,kSide,0,1}; }
    void Geometry(const Parameters& p) {
        mdl::SkinnedVertexBase vertices[4]{};mdl::EdgeVertex outlines[4]{};mdl::SkinnedVertexBase occluder[4]{};
        const float halfHeight=std::min(46.f,40.f/std::max(1.f,std::abs(p.slope)));
        for(unsigned i=0;i<4;++i) {
            const float y=64.f+((i&2)?halfHeight:-halfHeight);
            const float x=(i&1)?64.f+p.slope*(y-64)+p.phase:8.f;
            auto& v=vertices[i];v.position[0]=2*x/kSide-1;v.position[1]=1-2*y/kSide;
            v.position[2]=p.oblique?((i&2)?.44f:.36f):.4f;
            if(p.normals==1)v.normal[1]=1; // Deliberately tangent to edge-body displacement.
            else if(p.normals==2)v.normal[0]=-1; // Opposite projected extrusion.
            else if(p.normals==3) { v.normal[i%3]=i&1?-1.f:1.f; }
            else v.normal[0]=1;
            const float width=(i&2)?p.bottomWidth:p.topWidth;
            const float dx=p.displacement?width*((i&2)?1.f:.2f):width;
            const float dy=p.displacement?width*((i&2)?.1f:.97f):0;
            outlines[i].position[0]=v.position[0]+2*dx/kSide;outlines[i].position[1]=v.position[1]-2*dy/kSide;
            outlines[i].position[2]=v.position[2]+.1f;
            outlines[i].diffuse=p.vertexColors?(i&2?0xc0e04020:0x602060d0):p.color;
            const float fx=(i&1)?127.f:1.f;
            const float fy=(i&2)?(p.occlusion==2?127.f:82.f):(p.occlusion==2?1.f:46.f);
            occluder[i].position[0]=2*fx/kSide-1;occluder[i].position[1]=1-2*fy/kSide;occluder[i].position[2]=.1f;occluder[i].normal[2]=1;
        }
        model->pmxAdditionalUvCount=static_cast<std::uint8_t>(p.additionalUv);
        const unsigned stride=sizeof(mdl::SkinnedVertexBase)+16*p.additionalUv;void* data=nullptr;
        Hr(body->Lock(0,0,&data,0),"upload deformed body");std::memset(data,0,4*96);
        for(unsigned i=0;i<4;++i)std::memcpy(static_cast<BYTE*>(data)+i*stride,&vertices[i],sizeof(vertices[i]));body->Unlock();
        Hr(edge->Lock(0,0,&data,0),"upload original hull");std::memcpy(data,outlines,sizeof(outlines));edge->Unlock();
        Hr(foreground->Lock(0,0,&data,0),"upload foreground");std::memcpy(data,occluder,sizeof(occluder));foreground->Unlock();
    }
    void States(const Parameters& p,bool multisampled) {
        auto world=Identity(),view=Identity(),projection=Identity();
        if(p.oblique) {
            world._41=.027f;world._42=-.031f;
            const float angle=.09f;view._11=view._22=std::cos(angle);view._12=std::sin(angle);view._21=-std::sin(angle);
            projection._21=.07f;projection._12=-.08f;projection._31=.16f;projection._32=.10f;
            projection._14=.12f;projection._24=-.10f;projection._34=.30f;
        }
        Hr(device->SetTransform(D3DTS_WORLD,&world),"set world");Hr(device->SetTransform(D3DTS_VIEW,&view),"set view");Hr(device->SetTransform(D3DTS_PROJECTION,&projection),"set oblique projection");
        Hr(device->SetVertexShader(nullptr),"fixed body VS");Hr(device->SetPixelShader(nullptr),"fixed body PS");
        for(unsigned i=0;i<4;++i)device->SetTexture(i,nullptr);
        for(auto state:{D3DRS_LIGHTING,D3DRS_FOGENABLE,D3DRS_ALPHATESTENABLE,D3DRS_STENCILENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_SRGBWRITEENABLE})device->SetRenderState(state,FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE,0);device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
        device->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID);device->SetRenderState(D3DRS_ZENABLE,TRUE);device->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);
        device->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESS);device->SetRenderState(D3DRS_COLORWRITEENABLE,15);
        device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,multisampled);device->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffff);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,TRUE);device->SetRenderState(D3DRS_BLENDOPALPHA,D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLENDALPHA,D3DBLEND_ONE);device->SetRenderState(D3DRS_DESTBLENDALPHA,D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_TEXTUREFACTOR,0xffffffff);
        device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TFACTOR);
        device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TFACTOR);
        device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);device->SetIndices(indices.Get());
        const auto viewport=Viewport(p);Hr(device->SetViewport(&viewport),"set test viewport");
    }
    void DrawBody(const Parameters& p,bool colorWrites) {
        device->SetFVF(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1);device->SetRenderState(D3DRS_COLORWRITEENABLE,colorWrites?15:0);
        device->SetStreamSource(0,body.Get(),0,32+16*p.additionalUv);
        app->ActiveRenderObject()=model.get();app->ActiveRenderPass()=AccessoryRenderPass::FixedFunction;
        { deformed_mesh_capture::MaterialScope material(model.get(),0,true,true);
          Hr(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2),"draw actual body geometry / depth replay source"); }
        if(p.occlusion) {
            device->SetStreamSource(0,foreground.Get(),0,sizeof(mdl::SkinnedVertexBase));
            deformed_mesh_capture::MaterialScope material(model.get(),1,true,true);
            Hr(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2),"draw actual foreground occluder");
        }
        device->SetRenderState(D3DRS_COLORWRITEENABLE,15);
    }
    void OutlineState(const Parameters& p) {
        app->ActiveRenderObject()=model.get();app->ActiveRenderPass()=AccessoryRenderPass::ModelOutline;
        device->SetFVF(D3DFVF_XYZ|D3DFVF_DIFFUSE);device->SetStreamSource(0,edge.Get(),0,sizeof(mdl::EdgeVertex));
        device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
        device->SetRenderState(D3DRS_TEXTUREFACTOR,p.color);
        const DWORD argument=p.uniform?D3DTA_TFACTOR:D3DTA_DIFFUSE;
        device->SetTextureStageState(0,D3DTSS_COLORARG1,argument);device->SetTextureStageState(0,D3DTSS_ALPHAARG1,argument);
    }
    void Sentinel(const Parameters& p) {
        Hr(device->SetVertexShader(sentinelVertex.Get()),"bind non-null outer VS");Hr(device->SetPixelShader(sentinelPixel.Get()),"bind non-null outer PS");
        float vertexConstants[32],pixelConstants[32];for(unsigned i=0;i<32;++i) {vertexConstants[i]=(i+3)*.03125f;pixelConstants[i]=(i+1)*.015625f;}
        pixelConstants[28]=0;pixelConstants[29]=1;pixelConstants[30]=0;pixelConstants[31]=1;
        Hr(device->SetVertexShaderConstantF(0,vertexConstants,8),"bind sentinel VS constants");Hr(device->SetPixelShaderConstantF(0,pixelConstants,8),"bind sentinel PS constants");
        for(unsigned i=0;i<4;++i)Hr(device->SetTexture(i,sentinelTexture.Get()),"bind sentinel texture");
        for(unsigned i=0;i<2;++i) {device->SetSamplerState(i,D3DSAMP_MINFILTER,D3DTEXF_POINT);device->SetSamplerState(i,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
            device->SetSamplerState(i,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);device->SetSamplerState(i,D3DSAMP_ADDRESSV,D3DTADDRESS_MIRROR);}
        const auto v=Viewport(p);const RECT rect{LONG(v.X),LONG(v.Y),LONG(v.X+v.Width),LONG(v.Y+v.Height)};
        Hr(device->SetScissorRect(&rect),"bind outer scissor");device->SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE);
    }
    std::vector<DWORD> Read(IDirect3DSurface9* surface) {
        Hr(device->GetRenderTargetData(surface,readback.Get()),"read actual GPU pixels");D3DLOCKED_RECT lock{};Hr(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"map readback");
        std::vector<DWORD> image(kSide*kSide);
        for(unsigned y=0;y<kSide;++y)std::memcpy(image.data()+y*kSide,static_cast<BYTE*>(lock.pBits)+y*lock.Pitch,kSide*4);
        readback->UnlockRect();return image;
    }
    struct Output { std::vector<DWORD> layer,scene;unsigned samples=0;sampled::Stats stats; };
    Output Actual(const Parameters& p,bool soften=false) {
        sampled::Cancel(app.get());Geometry(p);device->SetDepthStencilSurface(nullptr);device->SetRenderTarget(0,mainColor.Get());device->SetDepthStencilSurface(mainDepth.Get());
        const D3DVIEWPORT9 full{0,0,kSide,kSide,0,1};device->SetViewport(&full);
        Hr(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,p.transparent?0:0xffffffff,1,0),"clear entire outer scene");States(p,false);
        const auto viewport=Viewport(p);Check(sampled::Begin(app.get(),device.Get(),mainColor.Get(),viewport),"multisample module genuinely begins; fallback is not a pass");
        sampled::SetSoftening(app.get(),soften);
        Check(!sampled::Failed(app.get()),"Begin does not poison frame");Hr(device->BeginScene(),"begin outer scene");DrawBody(p,!p.transparent);OutlineState(p);
        const float color[]={float((p.color>>16)&255)/255,float((p.color>>8)&255)/255,float(p.color&255)/255,float(p.color>>24)/255};
        Check(sampled::Draw(app.get(),Raw(),0,6,p.uniform?color:nullptr),"module handles native outline");Check(!sampled::Failed(app.get()),"native outline draw does not fall back or fail");
        Sentinel(p);const SavedBindings before(device.Get());Check(sampled::Finish(app.get()),"Finish resolves and composites inside outer BeginScene");before.Compare(device.Get());
        // A real draw and matching EndScene must still succeed after Finish's
        // internal native EndScene/resolve/BeginScene sequence.
        struct MarkerVertex { float x,y,z;DWORD color; };
        const MarkerVertex triangle[]={{-.98f,-.98f,0,0xffffffff},{-.75f,-.98f,0,0xffffffff},{-.98f,-.75f,0,0xffffffff}};
        Hr(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,triangle,sizeof(triangle[0])),"continue real drawing after Finish");Hr(device->EndScene(),"outer scene remains open after Finish");
        Output output;output.stats=sampled::GetStats(app.get());output.samples=output.stats.sampleCount;
        Check(output.samples==8 || output.samples==4,"use real MSAA8 or supported MSAA4");
        Check(output.stats.depthReplays>=1u+(p.occlusion?1u:0u) && output.stats.outlineDraws>=1 && output.stats.composites>=1,"stats prove real depth replay, outline raster and composite");
        Check(output.stats.observedNativeDevice && !output.stats.observing && !output.stats.frameFailed && output.stats.fallbackReason.empty(),"module used native device and completed without hidden fallback");
        Check(!sampled::Failed(app.get()),"completed frame remains valid");
        auto* texture=sampled::GetTexture(app.get());Check(texture!=nullptr,"publish actual resolved premultiplied layer");ComPtr<IDirect3DSurface9> layer;
        Hr(texture->GetSurfaceLevel(0,&layer),"get resolved layer");output.layer=Read(layer.Get());output.scene=Read(mainColor.Get());
        const unsigned markerX=viewport.X+unsigned(viewport.Width*.04f),markerY=viewport.Y+unsigned(viewport.Height*.95f);
        Check((output.scene[markerY*kSide+markerX]&0x00ffffff)==0x0000ff00,"continued draw retains sentinel shader/constants and reaches scene");
        return output;
    }
    std::vector<DWORD> Reference(const Parameters& p,unsigned samples) {
        sampled::Cancel(app.get());auto& target=references[samples];
        if(!target.color) {
            Hr(device->CreateRenderTarget(kSide,kSide,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_TYPE(samples),0,FALSE,&target.color,nullptr),"allocate independent real MSAA reference");
            Hr(device->CreateDepthStencilSurface(kSide,kSide,D3DFMT_D24S8,D3DMULTISAMPLE_TYPE(samples),0,TRUE,&target.depth,nullptr),"allocate matching reference depth");
            Hr(device->CreateRenderTarget(kSide,kSide,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target.resolved,nullptr),"allocate reference resolve");
        }
        device->SetDepthStencilSurface(nullptr);device->SetRenderTarget(0,target.color.Get());device->SetDepthStencilSurface(target.depth.Get());
        const D3DVIEWPORT9 full{0,0,kSide,kSide,0,1};device->SetViewport(&full);
        Hr(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL,0,1,0),"clear entire independent reference");States(p,true);Hr(device->BeginScene(),"begin independent reference");
        DrawBody(p,false);OutlineState(p);
        // This reference uses no module shader, width minimum, mask gate or
        // postfilter: the authored edge buffer is rasterized as-is by MSAA.
        Hr(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2),"rasterize original reference hull");Hr(device->EndScene(),"end independent reference");
        device->SetDepthStencilSurface(nullptr);device->SetRenderTarget(0,mainColor.Get());device->SetDepthStencilSurface(mainDepth.Get());
        Hr(device->StretchRect(target.color.Get(),nullptr,target.resolved.Get(),nullptr,D3DTEXF_NONE),"resolve actual independent MSAA reference");
        return Read(target.resolved.Get());
    }
};
struct Error { unsigned maximum=0,missing=0,referencePixels=0;double mae=0,rmse=0; };
Error Compare(const std::vector<DWORD>& actual,const std::vector<DWORD>& reference) {
    Error result;double sum=0,squares=0;
    for(unsigned i=0;i<actual.size();++i) {
        const unsigned a=actual[i]>>24,r=reference[i]>>24;result.referencePixels+=r>0;result.missing+=r>=8 && a<2;
        for(unsigned shift:{0u,8u,16u,24u}) {const int delta=int((actual[i]>>shift)&255)-int((reference[i]>>shift)&255);
            result.maximum=std::max(result.maximum,unsigned(std::abs(delta)));sum+=std::abs(delta);squares+=delta*delta;}
    }
    result.mae=sum/(actual.size()*4*255.);result.rmse=std::sqrt(squares/(actual.size()*4))/(255.);return result;
}
void CheckComposite(const Parameters& p,const Fixture::Output& output,const std::vector<DWORD>& layer) {
    const auto v=p.inset?D3DVIEWPORT9{9,7,110,111,.05f,.95f}:D3DVIEWPORT9{0,0,kSide,kSide,0,1};
    unsigned maximum=0;
    for(unsigned y=v.Y;y<v.Y+v.Height;++y)for(unsigned x=v.X;x<v.X+v.Width;++x) {
        // The intentional post-Finish sentinel draw occupies this corner.
        if(x<v.X+v.Width*.14f && y>v.Y+v.Height*.86f)continue;
        const auto actual=output.scene[y*kSide+x],reference=layer[y*kSide+x];
        for(unsigned shift:{0u,8u,16u,24u}) {
            const int expected=shift==24?(p.transparent?int(reference>>24):255):
                std::clamp(int((reference>>shift)&255)+(p.transparent?0:255-int(reference>>24)),0,255);
            maximum=std::max(maximum,unsigned(std::abs(int((actual>>shift)&255)-expected)));
        }
    }
    Check(maximum<=2,"raw layer composites unchanged over opaque and transparent scenes");
}
void Save(const std::filesystem::path& file,const std::vector<DWORD>& pixels) {
    std::ofstream out(file,std::ios::binary);out<<"P7\nWIDTH "<<kSide<<"\nHEIGHT "<<kSide<<"\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    for(auto pixel:pixels) {const char rgba[]={char(pixel>>16),char(pixel>>8),char(pixel),char(pixel>>24)};out.write(rgba,4);}
}
std::vector<DWORD> TransparentSceneLayer(const Parameters& p,const Fixture::Output& output) {
    Check(p.transparent && !p.inset,"softening fixture uses a full transparent viewport");
    auto pixels=output.scene;
    // Ignore only the unrelated continuation triangle whose color was checked
    // by Actual(). The real line is near x=64, clear of this corner.
    for(unsigned y=0;y<kSide;++y)for(unsigned x=0;x<kSide;++x)
        if(x<kSide*.14f && y>kSide*.86f)pixels[y*kSide+x]=0;
    return pixels;
}
struct SofteningResult {
    unsigned changed=0,supportPixels=0,rawPeak=0,softPeak=0;
    std::int64_t rawTotal[4]{},softTotal[4]{};
    double relativePeakDrop=0;
};
SofteningResult AnalyzeSoftening(const std::vector<DWORD>& raw,const std::vector<DWORD>& soft) {
    SofteningResult result;
    for(unsigned y=0;y<kSide;++y)for(unsigned x=0;x<kSide;++x) {
        const unsigned at=y*kSide+x;const auto original=raw[at],filtered=soft[at];
        result.changed+=original!=filtered;
        bool support=false;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx) {
            const unsigned xx=unsigned(std::clamp(int(x)+dx,0,int(kSide)-1));
            const unsigned yy=unsigned(std::clamp(int(y)+dy,0,int(kSide)-1));
            support=support || (raw[yy*kSide+xx]>>24)!=0;
        }
        result.supportPixels+=support;
        Check((original>>24)==0 || (filtered>>24)!=0,"softening retains every originally covered native pixel");
        if(!support)Check(original==filtered,"softening leaves empty line neighborhoods exact");
        for(unsigned channel=0;channel<4;++channel) {
            const unsigned shift=8*channel;
            result.rawTotal[channel]+=(original>>shift)&255;
            result.softTotal[channel]+=(filtered>>shift)&255;
            if(channel<3)Check(((filtered>>shift)&255)<=(filtered>>24)+1,"filtered transparent color remains premultiplied");
        }
        if(y>=32 && y<96 && x>=58 && x<74) {
            result.rawPeak=std::max(result.rawPeak,unsigned(original>>24));
            result.softPeak=std::max(result.softPeak,unsigned(filtered>>24));
        }
    }
    // The pair-symmetric filter is flux-conserving before the RGBA8 write.
    // Each affected output can round by at most half an LSB; this bound is
    // derived from its actual finite support, not an arbitrary percentage.
    const double roundingBound=result.supportPixels*.5+2;
    for(unsigned channel=0;channel<4;++channel) {
        const auto delta=result.softTotal[channel]-result.rawTotal[channel];
        Check(std::abs(double(delta))<=roundingBound,"whole-layer channel mass stays within accumulated RGBA8 rounding bound");
        // These rows are translation-invariant and away from line endpoints.
        // Summing a whole row leaves only 8-bit rounding, so two byte values
        // per channel is a stricter check than the whole-image bound.
        for(unsigned y=32;y<96;++y) {
            std::int64_t rowDelta=0;
            for(unsigned x=0;x<kSide;++x)rowDelta+=int((soft[y*kSide+x]>>(8*channel))&255)-int((raw[y*kSide+x]>>(8*channel))&255);
            Check(std::abs(rowDelta)<=2,"interior row channel mass changes by at most two RGBA8 byte values");
        }
    }
    Check(result.changed>0 && result.rawPeak>0 && result.softPeak>0 && result.softPeak<result.rawPeak,"enabled filter genuinely softens the native line profile");
    result.relativePeakDrop=double(result.rawPeak-result.softPeak)/result.rawPeak;
    return result;
}
void CheckAutomaticSoftening(Fixture& fixture,const std::filesystem::path& artifacts) {
    std::array<SofteningResult,2> profiles;
    unsigned index=0;
    for(float width:{.25f,2.f}) {
        Parameters p;p.transparent=true;p.color=0xc0e0c0a0;p.phase=.25f;p.slope=0;p.topWidth=p.bottomWidth=width;
        const auto raw=fixture.Actual(p),soft=fixture.Actual(p,true),repeat=fixture.Actual(p,true);
        Check(raw.layer==soft.layer && soft.layer==repeat.layer,"automatic softening never changes the original MSAA layer");
        const auto rawScene=TransparentSceneLayer(p,raw),softScene=TransparentSceneLayer(p,soft),repeatedScene=TransparentSceneLayer(p,repeat);
        Check(rawScene==raw.layer,"disabled softening composites the exact raw transparent layer");
        Check(softScene==repeatedScene,"stationary repeated softening is pixel-identical without temporal history");
        profiles[index]=AnalyzeSoftening(rawScene,softScene);
        const auto& profile=profiles[index];
        std::printf("SOFT width=%.2f changed=%u support=%u alpha_peak=%u->%u relative_peak_drop=%.6f mass_delta_BGRA=%lld/%lld/%lld/%lld rounding_bound=%.1f static_repeat=exact\n",
            width,profile.changed,profile.supportPixels,profile.rawPeak,profile.softPeak,profile.relativePeakDrop,
            static_cast<long long>(profile.softTotal[0]-profile.rawTotal[0]),static_cast<long long>(profile.softTotal[1]-profile.rawTotal[1]),
            static_cast<long long>(profile.softTotal[2]-profile.rawTotal[2]),static_cast<long long>(profile.softTotal[3]-profile.rawTotal[3]),profile.supportPixels*.5+2);
        if(!artifacts.empty()) {
            Save(artifacts/("soft-"+std::to_string(index)+"-raw.pam"),rawScene);
            Save(artifacts/("soft-"+std::to_string(index)+"-filtered.pam"),softScene);
        }
        ++index;
    }
    Check(profiles[0].relativePeakDrop<profiles[1].relativePeakDrop,"thin native lines receive less relative peak reduction than thicker lines");
    std::puts("PASS softening cases=2 raw-layer invariant, premultiplied mass, preserved coverage, adaptive strength and static repeat");
}
void CheckResizeFailureRecovery(Fixture& fixture) {
    Parameters p;p.transparent=true;p.color=0xc0e0c0a0;p.topWidth=.25f;p.bottomWidth=1.75f;p.slope=.5f;
    const auto before=fixture.Actual(p);
    constexpr unsigned width=kSide+32,height=kSide+16;
    ComPtr<IDirect3DSurface9> color,depth;
    Hr(fixture.device->CreateRenderTarget(width,height,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&color,nullptr),
        "allocate resized main scene for rebuild failure");
    Hr(fixture.device->CreateDepthStencilSurface(width,height,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,TRUE,&depth,nullptr),
        "allocate resized main depth for rebuild failure");
    Hr(fixture.device->SetDepthStencilSurface(nullptr),"detach depth before resize failure");
    Hr(fixture.device->SetRenderTarget(0,color.Get()),"bind resized main scene");
    Hr(fixture.device->SetDepthStencilSurface(depth.Get()),"bind resized main depth");
    const D3DVIEWPORT9 viewport{0,0,width,height,0,1};
    Hr(fixture.device->SetViewport(&viewport),"bind resized main viewport");
    const SavedBindings bindings(fixture.device.Get());
    {
        CompileFailure fault;
        // Ensure compiles only after allocating the new MSAA color/depth and
        // resolve texture. Leave that partial set in place, then return to the
        // old successful dimensions without calling Release.
        Check(!sampled::Begin(fixture.app.get(),fixture.device.Get(),color.Get(),viewport),
            "resized outline Begin reports injected shader compilation failure");
        Check(rejectedCompiles==1,"fault reaches native shader compilation after resized allocations");
        Check(sampled::GetStats(fixture.app.get()).fallbackReason=="Compile native outline shader",
            "resized allocation fails for the injected compiler fault");
        Check(sampled::GetTexture(fixture.app.get())==nullptr,"failed rebuild does not republish the previous line layer");
        bindings.Compare(fixture.device.Get());
    }
    sampled::Cancel(fixture.app.get());
    const auto after=fixture.Actual(p);
    Check(after.layer==before.layer && after.scene==before.scene,
        "return to original dimensions rebuilds complete resources and reproduces exact layer/scene pixels");
    Check(after.stats.fallbackReason.empty() && !after.stats.frameFailed,
        "successful rebuild clears the earlier failure instead of silently falling back");
    std::puts("PASS resize-recovery A-success -> B-allocated/compiler-failure -> A-exact-render, without Release");
}
std::vector<Parameters> Cases() {
    std::vector<Parameters> cases;
    for(float width:{.125f,.25f,.75f,2.f})for(float slope:{0.f,.08f,.25f,.5f,1.f,-1.f})for(float phase:{.17f,.61f}) {
        Parameters p;p.topWidth=p.bottomWidth=width;p.slope=slope;p.phase=phase;cases.push_back(p);
    }
    for(unsigned normals=0;normals<4;++normals)for(float width:{.125f,.5f,1.75f}) {
        Parameters p;p.normals=normals;p.topWidth=p.bottomWidth=width;p.slope=1;cases.push_back(p);
    }
    for(bool oblique:{false,true})for(unsigned displacement:{0u,1u})for(bool reverse:{false,true}) {
        Parameters p;p.topWidth=reverse?2.f:.125f;p.bottomWidth=reverse?.125f:2.f;p.normals=3;
        p.displacement=displacement;p.oblique=oblique;p.slope=-1;cases.push_back(p);
    }
    for(unsigned uv=1;uv<=4;++uv) {Parameters p;p.additionalUv=uv;p.oblique=true;p.normals=1;p.slope=1;cases.push_back(p);}
    for(bool transparent:{false,true})for(bool uniform:{false,true}) {
        Parameters p;p.transparent=transparent;p.uniform=uniform;p.color=0x80e02060;p.topWidth=.25f;p.bottomWidth=1.75f;p.slope=1;cases.push_back(p);
    }
    for(bool uniform:{false,true}) {Parameters p;p.transparent=true;p.vertexColors=true;p.uniform=uniform;p.topWidth=.3f;p.bottomWidth=1.5f;p.color=0xc020d080;cases.push_back(p);}
    for(unsigned occlusion:{1u,2u})for(bool transparent:{false,true}) {Parameters p;p.occlusion=occlusion;p.transparent=transparent;p.topWidth=p.bottomWidth=2;p.slope=.5f;cases.push_back(p);}
    Parameters inset;inset.inset=true;inset.oblique=true;inset.normals=3;inset.topWidth=.125f;inset.bottomWidth=2;cases.push_back(inset);
    return cases;
}
}
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);Check(argc<=2,"usage: outline_multisample_tests [artifact-directory]");
    Fixture fixture;const auto cases=Cases();unsigned index=0,totalMissing=0,maxDifference=0;double maximumRmse=0;
    for(const auto& p:cases) {
        const auto actual=fixture.Actual(p);const auto reference=fixture.Reference(p,actual.samples);const auto error=Compare(actual.layer,reference);
        if(argc==2) {const std::filesystem::path directory=argv[1];std::filesystem::create_directories(directory);
            Save(directory/(std::to_string(index)+"-module.pam"),actual.layer);Save(directory/(std::to_string(index)+"-msaa-reference.pam"),reference);}
        std::printf("CASE %u MSAA%u widths=%.3f/%.3f slope=%.2f normal=%u displacement=%u oblique=%u transparent=%u max_byte=%u MAE=%.9f RMSE=%.9f missing=%u depth=%u outlines=%u composites=%u\n",
            index,actual.samples,p.topWidth,p.bottomWidth,p.slope,p.normals,p.displacement,p.oblique,p.transparent,error.maximum,error.mae,error.rmse,error.missing,
            actual.stats.depthReplays,actual.stats.outlineDraws,actual.stats.composites);
        Check(error.maximum<=2 && error.mae<.00025,"identity resolved layer matches real MSAA per pixel, not just total ink");
        CheckComposite(p,actual,reference);
        Check(error.missing==0,"no visible reference-covered pixels disappear");
        if(p.occlusion==2)Check(error.referencePixels==0,"real foreground geometry fully occludes reference and module hulls");
        if(p.transparent)for(auto pixel:actual.layer)for(unsigned shift:{0u,8u,16u})Check(((pixel>>shift)&255)<=(pixel>>24)+1,"transparent colored layer remains premultiplied");
        totalMissing+=error.missing;maxDifference=std::max(maxDifference,error.maximum);maximumRmse=std::max(maximumRmse,error.rmse);++index;
    }
    CheckAutomaticSoftening(fixture,argc==2?std::filesystem::path(argv[1]):std::filesystem::path{});
    CheckResizeFailureRecovery(fixture);
    std::printf("PASS cases=%zu checks=%u max_RGBA_byte_difference=%u max_pixel_RMSE=%.9f missing_reference_pixels=%u; real MSAA depth/outline/composite and open-scene state verified\n",
        cases.size(),checks,maxDifference,maximumRmse,totalMissing);
    return 0;
}
