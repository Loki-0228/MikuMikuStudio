#include "outline_multisample.hpp"
#include "outline_multisample_shader.hpp"
#include "mikudancestudio/build_profile.hpp"
#include "mikudancestudio/deformed_mesh_capture.hpp"
#include "mikudancestudio/graphics_runtime.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/runtime_log.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace mikudancestudio::antialias::outline_multisample {
using Microsoft::WRL::ComPtr;
namespace {
void Check(HRESULT hr,const char* operation) {
    if(FAILED(hr)) {
        char text[240]{};std::snprintf(text,sizeof text,"%s (0x%08lX)",operation,hr);
        throw std::runtime_error(text);
    }
}
struct Busy {
    bool& value;bool before;
    explicit Busy(bool& flag):value(flag),before(flag){value=true;}
    ~Busy(){value=before;}
};
struct Preserve {
    IDirect3DDevice9* device;
    bool* poisoned;
    ComPtr<IDirect3DStateBlock9> state;
    ComPtr<IDirect3DSurface9> target[4],depth;
    D3DVIEWPORT9 viewport{};
    unsigned count=0;
    bool restored=false;
    Preserve(IDirect3DDevice9* d,bool& failed):device(d),poisoned(&failed) {
        D3DCAPS9 caps{};Check(d->GetDeviceCaps(&caps),"Query capture device capabilities");
        count=std::min(4u,unsigned(caps.NumSimultaneousRTs));
        for(unsigned i=0;i<count;++i) {
            const HRESULT hr=d->GetRenderTarget(i,&target[i]);
            if(i==0 || hr!=D3DERR_NOTFOUND)Check(hr,"Capture render target");
        }
        const HRESULT z=d->GetDepthStencilSurface(&depth);
        if(z!=D3DERR_NOTFOUND)Check(z,"Capture depth target");
        Check(d->GetViewport(&viewport),"Capture viewport");
        Check(d->CreateStateBlock(D3DSBT_ALL,&state),"Capture full render state");
    }
    bool Extra()const {
        for(unsigned i=1;i<count;++i)if(target[i])return true;
        return false;
    }
    void Bind(IDirect3DSurface9* color,IDirect3DSurface9* z,const D3DVIEWPORT9& view) {
        Check(device->SetDepthStencilSurface(nullptr),"Unbind capture depth");
        for(unsigned i=1;i<count;++i)Check(device->SetRenderTarget(i,nullptr),"Unbind extra capture target");
        Check(device->SetRenderTarget(0,color),"Bind outline target");
        Check(device->SetDepthStencilSurface(z),"Bind outline depth");
        Check(device->SetViewport(&view),"Restore outline viewport");
    }
    HRESULT Restore()noexcept {
        if(restored)return S_OK;restored=true;
        HRESULT first=S_OK;
        const auto record=[&](HRESULT hr){if(SUCCEEDED(first)&&FAILED(hr))first=hr;};
        record(device->SetDepthStencilSurface(nullptr));
        for(unsigned i=1;i<count;++i)record(device->SetRenderTarget(i,nullptr));
        if(count)record(device->SetRenderTarget(0,target[0].Get()));
        record(device->SetDepthStencilSurface(depth.Get()));
        for(unsigned i=1;i<count;++i)record(device->SetRenderTarget(i,target[i].Get()));
        if(state)record(state->Apply());
        record(device->SetViewport(&viewport));
        if(FAILED(first)) {
            *poisoned=true;
            runtime_log::Write("Native outline MSAA state restoration failed hr=0x%08lX",first);
        }
        return first;
    }
    ~Preserve(){Restore();}
};
bool SameViewport(const D3DVIEWPORT9& a,const D3DVIEWPORT9& b) {
    return a.X==b.X&&a.Y==b.Y&&a.Width==b.Width&&a.Height==b.Height&&a.MinZ==b.MinZ&&a.MaxZ==b.MaxZ;
}
DWORD RenderState(IDirect3DDevice9* device,D3DRENDERSTATETYPE type) {
    DWORD value=0;Check(device->GetRenderState(type,&value),"Read source render state");return value;
}
bool IndependentStencil(IDirect3DDevice9* device) {
    if(!RenderState(device,D3DRS_STENCILENABLE))return true;
    // Native MMD leaves stencil enabled with ALWAYS/REPLACE for its model
    // pass. It writes the mask but does not depend on earlier stencil data.
    return RenderState(device,D3DRS_STENCILFUNC)==D3DCMP_ALWAYS &&
        (!RenderState(device,D3DRS_TWOSIDEDSTENCILMODE) ||
         RenderState(device,D3DRS_CCW_STENCILFUNC)==D3DCMP_ALWAYS);
}
bool GeometryInput(IDirect3DDevice9* device) {
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    Check(device->GetVertexDeclaration(&declaration),"Read source declaration");
    if(!declaration)return false;
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};UINT count=MAXD3DDECLLENGTH+1;
    Check(declaration->GetDeclaration(elements,&count),"Describe source declaration");
    bool position=false,normal=false;
    for(unsigned i=0;i<count&&elements[i].Stream!=0xff;++i) {
        if(elements[i].Usage==D3DDECLUSAGE_POSITIONT)return false;
        if(elements[i].Usage==D3DDECLUSAGE_POSITION&&elements[i].UsageIndex==0)
            position=elements[i].Type==D3DDECLTYPE_FLOAT3;
        if(elements[i].Usage==D3DDECLUSAGE_NORMAL&&elements[i].UsageIndex==0)normal=true;
    }
    return position&&normal;
}
std::vector<DWORD> PixelCode(IDirect3DDevice9* device) {
    ComPtr<IDirect3DPixelShader9> shader;
    Check(device->GetPixelShader(&shader),"Read source pixel shader");
    if(!shader)return {};
    UINT bytes=0;Check(shader->GetFunction(nullptr,&bytes),"Size source pixel shader");
    if(bytes<8 || bytes>1024*1024 || bytes%4)throw std::runtime_error("Invalid source pixel shader size");
    std::vector<DWORD> code(bytes/4);Check(shader->GetFunction(code.data(),&bytes),"Read source pixel shader bytecode");
    return code;
}
bool SafePixelShader(const std::vector<DWORD>& code,std::string& reason) {
    if(code.empty())return true;
    if(D3DSHADER_VERSION_MAJOR(code[0])<2){reason="Shader model 1 pixel coverage is not supported";return false;}
    for(std::size_t cursor=1;cursor<code.size();) {
        const DWORD instruction=code[cursor],opcode=instruction&D3DSI_OPCODE_MASK;
        if(opcode==D3DSIO_END)return true;
        if(opcode==D3DSIO_COMMENT) {
            const std::size_t length=(instruction&D3DSI_COMMENTSIZE_MASK)>>D3DSI_COMMENTSIZE_SHIFT;
            if(length>=code.size()-cursor){reason="Truncated pixel shader comment";return false;}
            cursor+=length+1;continue;
        }
        const std::size_t length=(instruction&D3DSI_INSTLENGTH_MASK)>>D3DSI_INSTLENGTH_SHIFT;
        if(length>=code.size()-cursor){reason="Truncated pixel shader instruction";return false;}
        // DEF/DEFI/DEFB contain immediate literal words, not registers. DCL's
        // first operand is a semantic token. Never interpret their bit patterns
        // as VPOS/DEPTHOUT registers.
        const std::size_t firstParameter=opcode==D3DSIO_DCL?2u:1u;
        const std::size_t lastParameter=(opcode==D3DSIO_DEF||opcode==D3DSIO_DEFI||opcode==D3DSIO_DEFB)
            ?std::min(std::size_t(1),length):length;
        for(std::size_t i=firstParameter;i<=lastParameter;++i) {
            const DWORD parameter=code[cursor+i];
            if(!(parameter&0x80000000u))continue;
            const DWORD type=((parameter&D3DSP_REGTYPE_MASK)>>D3DSP_REGTYPE_SHIFT)|
                ((parameter&D3DSP_REGTYPE_MASK2)>>D3DSP_REGTYPE_SHIFT2);
            if(type==D3DSPR_DEPTHOUT){reason="Pixel shader overrides geometry depth";return false;}
            if(type==D3DSPR_MISCTYPE){reason="Pixel shader uses VPOS/VFACE screen-sample inputs";return false;}
            if(type==D3DSPR_COLOROUT && (parameter&D3DSP_REGNUM_MASK)!=0) {
                reason="Pixel shader writes multiple color outputs";return false;
            }
        }
        cursor+=length+1;
    }
    reason="Pixel shader has no END token";return false;
}
D3DMATRIX Multiply(const D3DMATRIX& a,const D3DMATRIX& b) {
    D3DMATRIX result{};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)
        result.m[i][j]+=a.m[i][k]*b.m[k][j];
    return result;
}
struct State final:deformed_mesh_capture::DrawObserver {
    MMDApp* app;
    ComPtr<IDirect3DDevice9> device;
    ComPtr<IDirect3DSurface9> scene,color,depth,resolvedSurface;
    ComPtr<IDirect3DTexture9> resolved;
    ComPtr<IDirect3DVertexShader9> vertexShader;
    ComPtr<IDirect3DPixelShader9> pixelShader,compositeShader;
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    D3DSURFACE_DESC description{};
    D3DFORMAT depthFormat=D3DFMT_UNKNOWN;
    D3DVIEWPORT9 viewport{};
    Stats stats;
    bool busy=false,collecting=false,disabled=false,handled=false,published=false,finished=false,softening=false;
    bool registered=false;
    explicit State(MMDApp* value):app(value){}
    ~State(){Stop();}
    void Pause() {
        collecting=false;
        stats.observing=false;
    }
    void Stop() {
        Pause();
        if(registered){deformed_mesh_capture::StopObserving(this);registered=false;}
    }
    void Reject(const std::string& reason,bool hard=false) {
        if(stats.fallbackReason.empty()) {
            stats.fallbackReason=reason;
            runtime_log::Write("Native outline MSAA fallback: %s (handled=%u replays=%u)",reason.c_str(),handled?1u:0u,stats.depthReplays);
        }
        disabled=true;collecting=false;published=false;
        stats.frameFailed=stats.frameFailed||handled||hard;
    }
    bool SceneDraw(IDirect3DDevice9* source) {
        ComPtr<IDirect3DSurface9> current;
        Check(source->GetRenderTarget(0,&current),"Read observed scene target");
        return current.Get()==scene.Get();
    }
    void Ensure(IDirect3DDevice9* source,IDirect3DSurface9* mainScene) {
        D3DSURFACE_DESC desc{},zdesc{};
        Check(mainScene->GetDesc(&desc),"Describe main outline scene");
        ComPtr<IDirect3DSurface9> originalDepth;
        Check(source->GetDepthStencilSurface(&originalDepth),"Read main outline depth");
        Check(originalDepth->GetDesc(&zdesc),"Describe main outline depth");
        if(desc.MultiSampleType!=D3DMULTISAMPLE_NONE)throw std::runtime_error("Main scene must remain single-sample");
        if(device.Get()==source&&color&&depth&&resolved&&resolvedSurface&&vertexShader&&pixelShader&&compositeShader&&declaration&&
           description.Width==desc.Width&&description.Height==desc.Height&&depthFormat==zdesc.Format) {
            scene=mainScene;stats.sampleCount=unsigned(description.MultiSampleType);return;
        }
        // A failed resize may leave only some of the new resources alive.
        // Invalidate the previous dimensions before rebuilding so returning
        // to that size cannot publish a partial allocation as a ready layer.
        description={};depthFormat=D3DFMT_UNKNOWN;
        device=source;scene=mainScene;color.Reset();depth.Reset();resolved.Reset();resolvedSurface.Reset();
        vertexShader.Reset();pixelShader.Reset();compositeShader.Reset();declaration.Reset();
        D3DCAPS9 caps{};Check(device->GetDeviceCaps(&caps),"Query outline MSAA capabilities");
        if(caps.VertexShaderVersion<D3DVS_VERSION(3,0)||caps.PixelShaderVersion<D3DPS_VERSION(3,0))
            throw std::runtime_error("Native outline MSAA requires shader model 3");
        D3DDEVICE_CREATION_PARAMETERS cp{};Check(device->GetCreationParameters(&cp),"Query outline adapter");
        ComPtr<IDirect3D9> factory;Check(device->GetDirect3D(&factory),"Query outline D3D interface");
        ComPtr<IDirect3DSwapChain9> swap;Check(device->GetSwapChain(0,&swap),"Query windowed state");
        D3DPRESENT_PARAMETERS pp{};Check(swap->GetPresentParameters(&pp),"Query source presentation");
        D3DDISPLAYMODE mode{};Check(factory->GetAdapterDisplayMode(cp.AdapterOrdinal,&mode),"Query adapter display format");
        Check(factory->CheckDepthStencilMatch(cp.AdapterOrdinal,cp.DeviceType,mode.Format,D3DFMT_A8R8G8B8,zdesc.Format),"Pair outline color/depth");
        unsigned samples=0;
        for(unsigned requested:{8u,4u}) {
            DWORD colorLevels=0,depthLevels=0;
            const auto type=D3DMULTISAMPLE_TYPE(requested);
            if(FAILED(factory->CheckDeviceMultiSampleType(cp.AdapterOrdinal,cp.DeviceType,D3DFMT_A8R8G8B8,pp.Windowed,type,&colorLevels))||!colorLevels||
               FAILED(factory->CheckDeviceMultiSampleType(cp.AdapterOrdinal,cp.DeviceType,zdesc.Format,pp.Windowed,type,&depthLevels))||!depthLevels)continue;
            if(FAILED(device->CreateRenderTarget(desc.Width,desc.Height,D3DFMT_A8R8G8B8,type,0,FALSE,&color,nullptr)))continue;
            if(FAILED(device->CreateDepthStencilSurface(desc.Width,desc.Height,zdesc.Format,type,0,TRUE,&depth,nullptr))){color.Reset();continue;}
            samples=requested;break;
        }
        if(!samples)throw std::runtime_error("No matching native outline MSAA8/MSAA4 color and depth targets");
        Check(device->CreateTexture(desc.Width,desc.Height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&resolved,nullptr),"Allocate resolved outline layer");
        Check(resolved->GetSurfaceLevel(0,&resolvedSurface),"Read resolved outline surface");
        const auto compile=[&](const char* entry,const char* profile,ID3DBlob** output) {
            ComPtr<ID3DBlob> error;
            const HRESULT hr=graphics_runtime::Compile(kShader,sizeof(kShader)-1,"native outline MSAA",nullptr,nullptr,
                entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,output,&error);
            if(FAILED(hr))throw std::runtime_error(error?static_cast<const char*>(error->GetBufferPointer()):"Compile native outline shader");
        };
        ComPtr<ID3DBlob> code;compile("outlineVertex","vs_3_0",&code);
        Check(device->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()),&vertexShader),"Create original outline vertex shader");
        code.Reset();compile("outlinePixel","ps_3_0",&code);
        Check(device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),&pixelShader),"Create original outline pixel shader");
        code.Reset();compile("composite","ps_3_0",&code);
        Check(device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),&compositeShader),"Create outline composite shader");
        const D3DVERTEXELEMENT9 elements[]={
            {0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
            {0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0},D3DDECL_END()};
        Check(device->CreateVertexDeclaration(elements,&declaration),"Create original outline declaration");
        description=desc;description.MultiSampleType=D3DMULTISAMPLE_TYPE(samples);depthFormat=zdesc.Format;
        stats.sampleCount=samples;
        runtime_log::Write("Native outline coverage ready MSAA%u dimensions=%ux%u; main scene remains single-sample",samples,desc.Width,desc.Height);
    }
    void Begin(IDirect3DDevice9* source,IDirect3DSurface9* mainScene,const D3DVIEWPORT9& view) {
        Pause();stats={};disabled=false;handled=false;published=false;finished=false;softening=false;busy=false;viewport=view;
        Ensure(source,mainScene);
        Preserve preserve(device.Get(),stats.frameFailed);
        if(preserve.Extra())throw std::runtime_error("An external MRT is active at outline collection start");
        const D3DVIEWPORT9 full{0,0,description.Width,description.Height,0,1};
        preserve.Bind(color.Get(),depth.Get(),full);
        DWORD clear=D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER;
        if(depthFormat==D3DFMT_D24S8||depthFormat==D3DFMT_D24FS8)clear|=D3DCLEAR_STENCIL;
        Check(device->Clear(0,nullptr,clear,0,1,0),"Clear native multisample outline layer before BeginScene");
        Check(preserve.Restore(),"Restore scene after outline layer clear");
        if(!registered) {
            std::string error;
            if(!deformed_mesh_capture::Observe(*app,this,error))throw std::runtime_error("Native outline observer: "+error);
            registered=true;
        }
        // Keep the native detours/probe alive between FXAA frames. Finish and
        // Cancel pause callbacks; Release/AA algorithm changes own unhooking.
        collecting=true;stats.observing=true;
    }
    void Capture(IDirect3DDevice9* source,D3DPRIMITIVETYPE type,INT base,UINT minVertex,UINT vertices,UINT firstIndex,UINT primitives) {
        if(!collecting||disabled||busy||!primitives||!SceneDraw(source))return;
        if(type<D3DPT_TRIANGLELIST)return; // helpers/ground grid are not polygon occluders
        if(RenderState(source,D3DRS_ZENABLE)==D3DZB_FALSE||!RenderState(source,D3DRS_ZWRITEENABLE))return;
        if(RenderState(source,D3DRS_ZENABLE)!=D3DZB_TRUE)throw std::runtime_error("W-buffer depth replay is unsupported");
        if(!IndependentStencil(source))throw std::runtime_error("Stencil-dependent body coverage cannot be mirrored");
        if(!GeometryInput(source))throw std::runtime_error("Screen/transformed or non-mesh depth-writing draw");
        D3DVIEWPORT9 actual{};Check(source->GetViewport(&actual),"Read observed body viewport");
        if(!SameViewport(actual,viewport))throw std::runtime_error("Body depth draw uses a different viewport");
        std::string unsafe;
        if(!SafePixelShader(PixelCode(source),unsafe))throw std::runtime_error(unsafe);
        Preserve preserve(source,stats.frameFailed);
        if(preserve.Extra())throw std::runtime_error("Body coverage uses external MRT outputs");
        // The hook sees the runtime's actual native device and final MME
        // shaders, not the application's d3d9 proxy. It must own these targets.
        if(source!=device.Get())throw std::runtime_error("Observed native device differs from outline target owner");
        stats.observedNativeDevice=true;
        Busy replay(busy);
        preserve.Bind(color.Get(),depth.Get(),actual);
        Check(source->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,TRUE),"Enable body depth coverage samples");
        Check(source->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffffu),"Enable all body depth coverage samples");
        Check(source->SetRenderState(D3DRS_COLORWRITEENABLE,15),"Enable zeroing covered outline samples");
        Check(source->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE),"Enable body color cancellation");
        Check(source->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD),"Body color cancellation operation");
        Check(source->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ZERO),"Cancel body source color");
        Check(source->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_ZERO),"Clear earlier occluded outlines");
        Check(source->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,TRUE),"Enable body alpha cancellation");
        Check(source->SetRenderState(D3DRS_BLENDOPALPHA,D3DBLENDOP_ADD),"Body alpha cancellation operation");
        Check(source->SetRenderState(D3DRS_SRCBLENDALPHA,D3DBLEND_ZERO),"Cancel body source alpha");
        Check(source->SetRenderState(D3DRS_DESTBLENDALPHA,D3DBLEND_ZERO),"Cancel old outline alpha");
        Check(source->DrawIndexedPrimitive(type,base,minVertex,vertices,firstIndex,primitives),"Replay final native/MME body depth");
        ++stats.depthReplays;
        Check(preserve.Restore(),"Restore original body draw after depth replay");
    }
    void Indexed(IDirect3DDevice9* source,D3DPRIMITIVETYPE type,INT base,UINT minVertex,UINT vertices,
                 UINT firstIndex,UINT primitives,const void* object,unsigned)noexcept override {
        // MaterialScope identifies native body/accessory submissions. Shadow
        // and selection passes may share the same target/depth but are not
        // polygon occluders for this line layer.
        if(!object)return;
        try{Capture(source,type,base,minVertex,vertices,firstIndex,primitives);}
        catch(const std::exception& e){Reject(e.what());}
        catch(...){Reject("Unexpected native outline depth replay failure",true);}
    }
    void Unsupported(IDirect3DDevice9* source,D3DPRIMITIVETYPE type,UINT primitives)noexcept override {
        try {
            if(!collecting||disabled||busy||!primitives||type<D3DPT_TRIANGLELIST||!SceneDraw(source))return;
            if(RenderState(source,D3DRS_ZENABLE)!=D3DZB_FALSE&&RenderState(source,D3DRS_ZWRITEENABLE))
                Reject("Non-indexed/user-memory depth-writing draw cannot be replayed");
        }catch(const std::exception& e){Reject(e.what());}catch(...){Reject("Unexpected outline observer failure",true);}
    }
    bool Draw(unsigned char* raw,unsigned firstIndex,unsigned indexCount,const float* uniformColor) {
        if(stats.frameFailed)return true;
        if(!collecting||disabled||!raw||!indexCount)return false;
        if(!SceneDraw(device.Get()))return false;
        if(!stats.observedNativeDevice){Reject("No matching runtime body draws reached the native observer");return false;}
        Busy owned(busy);
        try {
            Preserve preserve(device.Get(),stats.frameFailed);
            if(preserve.Extra())throw std::runtime_error("Native outline stage has external MRT outputs");
            if(!SameViewport(preserve.viewport,viewport))throw std::runtime_error("Native outline uses a different viewport");
            if(!IndependentStencil(device.Get()))throw std::runtime_error("Stencil-dependent native outline cannot be mirrored");
            if(RenderState(device.Get(),D3DRS_SRGBWRITEENABLE))throw std::runtime_error("sRGB native outline accumulation requires a separate linear layer");
            if(RenderState(device.Get(),D3DRS_ZENABLE)!=D3DZB_TRUE)throw std::runtime_error("Native outline requires ordinary enabled depth");
            auto* record=mdl::Mdl(raw);
            auto* vb=mdl::ResourceAs<IDirect3DVertexBuffer9>(record->vertexBuffer2);
            auto* ib=mdl::ResourceAs<IDirect3DIndexBuffer9>(record->indexBuffer);
            if(!vb||!ib||indexCount%3)throw std::runtime_error("Native outline geometry is unavailable");
            D3DMATRIX world{},view{},projection{};
            Check(device->GetTransform(D3DTS_WORLD,&world),"Read outline world transform");
            Check(device->GetTransform(D3DTS_VIEW,&view),"Read outline view transform");
            Check(device->GetTransform(D3DTS_PROJECTION,&projection),"Read outline projection");
            const auto transform=Multiply(Multiply(world,view),projection);
            float constants[24]{};std::memcpy(constants,&transform,sizeof transform);
            if(uniformColor){std::memcpy(constants+16,uniformColor,16);constants[20]=1;}
            Check(device->SetStreamSource(0,vb,0,sizeof(mdl::EdgeVertex)),"Bind original outline vertices");
            Check(device->SetIndices(ib),"Bind original outline indices");
            Check(device->SetVertexDeclaration(declaration.Get()),"Bind original outline declaration");
            Check(device->SetVertexShader(vertexShader.Get()),"Bind original outline vertex shader");
            Check(device->SetPixelShader(pixelShader.Get()),"Bind original outline pixel shader");
            Check(device->SetVertexShaderConstantF(0,constants,6),"Set original outline transform/color");
            preserve.Bind(color.Get(),depth.Get(),viewport);
            Check(device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,TRUE),"Enable native outline MSAA");
            Check(device->SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffffu),"Enable all native outline samples");
            Check(device->SetRenderState(D3DRS_COLORWRITEENABLE,15),"Write outline RGBA");
            Check(device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE),"Enable premultiplied outline accumulation");
            Check(device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD),"Outline accumulation operation");
            Check(device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA),"Premultiply native outline color");
            Check(device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA),"Blend native outline color");
            Check(device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,TRUE),"Accumulate outline coverage alpha");
            Check(device->SetRenderState(D3DRS_BLENDOPALPHA,D3DBLENDOP_ADD),"Outline alpha operation");
            Check(device->SetRenderState(D3DRS_SRCBLENDALPHA,D3DBLEND_ONE),"Preserve authored outline alpha");
            Check(device->SetRenderState(D3DRS_DESTBLENDALPHA,D3DBLEND_INVSRCALPHA),"Accumulate outline coverage");
            Check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,record->vertexCount,firstIndex,indexCount/3),"Draw original MSAA outline hull");
            // Once a line submission is accepted, a later failure must reject
            // the frame. Legacy redraw would duplicate a partially drawn line.
            handled=true;++stats.outlineDraws;
            preserve.Bind(scene.Get(),preserve.depth.Get(),viewport);
            Check(device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,FALSE),"Retain single-sample main scene");
            Check(device->SetRenderState(D3DRS_COLORWRITEENABLE,0),"Main outline keeps depth only");
            Check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,record->vertexCount,firstIndex,indexCount/3),"Preserve original outline main-scene depth");
            Check(preserve.Restore(),"Restore caller after native outline draw");
            return true;
        }catch(const std::exception& e){Reject(e.what());}catch(...){Reject("Unexpected native outline draw failure",true);}
        return handled||stats.frameFailed;
    }
    bool Finish() {
        if(finished)return !stats.frameFailed;
        finished=true;
        Pause();
        if(stats.frameFailed)return false;
        if(disabled||!stats.outlineDraws)return true;
        Busy owned(busy);
        try {
            Preserve preserve(device.Get(),stats.frameFailed);
            if(preserve.Extra())throw std::runtime_error("External MRT active at native outline composite");
            if(!SceneDraw(device.Get()))throw std::runtime_error("Native outline composite target changed before Finish");
            // End/Begin the actual runtime device, never the MME wrapper. MME
            // still observes one logical application scene and post-processes
            // the completed line layer only after this function has returned.
            Check(device->EndScene(),"Split native scene for outline resolve");
            const HRESULT resolve=device->StretchRect(color.Get(),nullptr,resolvedSurface.Get(),nullptr,D3DTEXF_NONE);
            HRESULT resumed=device->BeginScene();
            if(FAILED(resumed)) {
                runtime_log::Write("Native outline scene resume failed hr=0x%08lX; retrying once",resumed);
                resumed=device->BeginScene();
            }
            // Resume is attempted even if resolve failed, so the outer
            // renderer can still execute its matching EndScene on failure.
            Check(resumed,"Resume native scene after outline resolve");
            Check(resolve,"Resolve native multisample outline layer");
            preserve.Bind(scene.Get(),preserve.depth.Get(),viewport);
            Check(device->SetVertexShader(nullptr),"Bind outline composite vertex mode");
            Check(device->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1),"Bind outline composite quad format");
            Check(device->SetPixelShader(compositeShader.Get()),"Bind outline composite pixel shader");
            for(auto state:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_STENCILENABLE,D3DRS_ALPHATESTENABLE,
                D3DRS_FOGENABLE,D3DRS_LIGHTING,D3DRS_SRGBWRITEENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_CLIPPLANEENABLE,
                D3DRS_MULTISAMPLEANTIALIAS})Check(device->SetRenderState(state,FALSE),"Disable composite geometry states");
            Check(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"Disable composite culling");
            Check(device->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID),"Set composite fill");
            Check(device->SetRenderState(D3DRS_COLORWRITEENABLE,15),"Composite RGBA");
            Check(device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE),"Enable premultiplied composite");
            Check(device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD),"Composite blend operation");
            Check(device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ONE),"Preserve premultiplied layer");
            Check(device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA),"Composite over scene");
            Check(device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,TRUE),"Composite coverage alpha");
            Check(device->SetRenderState(D3DRS_BLENDOPALPHA,D3DBLENDOP_ADD),"Composite alpha operation");
            Check(device->SetRenderState(D3DRS_SRCBLENDALPHA,D3DBLEND_ONE),"Preserve layer alpha");
            Check(device->SetRenderState(D3DRS_DESTBLENDALPHA,D3DBLEND_INVSRCALPHA),"Composite destination alpha");
            Check(device->SetTexture(0,resolved.Get()),"Bind resolved line layer");
            for(auto setting:{D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})Check(device->SetSamplerState(0,setting,D3DTEXF_LINEAR),"Bilinear outline resolve sampling");
            Check(device->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE),"Disable line mipmapping");
            Check(device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE),"Preserve line layer color space");
            for(auto setting:{D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV})Check(device->SetSamplerState(0,setting,D3DTADDRESS_CLAMP),"Clamp line layer sampler");
            const float w=float(description.Width),h=float(description.Height);
            const float constants[]={1/w,1/h,0,0,(viewport.X+.5f)/w,(viewport.Y+.5f)/h,
                (viewport.X+viewport.Width-.5f)/w,(viewport.Y+viewport.Height-.5f)/h,softening?1.f:0.f,0,0,0};
            Check(device->SetPixelShaderConstantF(0,constants,3),"Bind line composite constants");
            struct Quad {float x,y,z,w,u,v;};
            const float l=float(viewport.X),t=float(viewport.Y),r=l+viewport.Width,b=t+viewport.Height;
            const Quad quad[]={{l-.5f,t-.5f,0,1,l/w,t/h},{r-.5f,t-.5f,0,1,r/w,t/h},
                {l-.5f,b-.5f,0,1,l/w,b/h},{r-.5f,b-.5f,0,1,r/w,b/h}};
            {
                // Motion capture shares the live draw hook with this module.
                // Only the synthetic screen composition is hidden; the main
                // depth-only outline draw remains visible to motion capture.
                deformed_mesh_capture::ObserverSilenceScope silence;
                Check(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Quad)),"Composite original MSAA outline coverage");
            }
            Check(preserve.Restore(),"Restore outer scene after line composite");
            ++stats.composites;published=true;
            return true;
        }catch(const std::exception& e){Reject(e.what(),true);}catch(...){Reject("Unexpected native outline composite failure",true);}
        return false;
    }
};
std::unordered_map<MMDApp*,std::unique_ptr<State>> states;
State* Find(MMDApp* app) {const auto it=states.find(app);return it==states.end()?nullptr:it->second.get();}
}
bool Begin(MMDApp* app,IDirect3DDevice9* device,IDirect3DSurface9* scene,const D3DVIEWPORT9& viewport) {
    if(build_profile::original||!app||!device||!scene)return false;
    auto& pointer=states[app];if(!pointer)pointer=std::make_unique<State>(app);
    try{pointer->Begin(device,scene,viewport);return true;}
    catch(const std::exception& e){pointer->Reject(e.what());pointer->Pause();}
    catch(...){pointer->Reject("Unexpected native outline initialization failure",true);pointer->Pause();}
    return false;
}
bool Draw(MMDApp* app,unsigned char* model,unsigned firstIndex,unsigned indexCount,const float* color) {
    auto* state=Find(app);return state&&state->Draw(model,firstIndex,indexCount,color);
}
bool Finish(MMDApp* app) {auto* state=Find(app);return !state||state->Finish();}
bool Active(MMDApp* app){auto* state=Find(app);return state&&state->collecting&&!state->disabled;}
bool Failed(MMDApp* app){auto* state=Find(app);return state&&state->stats.frameFailed;}
Stats GetStats(MMDApp* app){auto* state=Find(app);return state?state->stats:Stats{};}
IDirect3DTexture9* GetTexture(MMDApp* app){auto* state=Find(app);return state&&state->published?state->resolved.Get():nullptr;}
void SetSoftening(MMDApp* app,bool enabled){if(auto* state=Find(app))state->softening=enabled;}
void Cancel(MMDApp* app){if(auto* state=Find(app)) {
    if(!state->finished){state->stats.frameFailed=state->stats.frameFailed||state->handled;state->published=false;state->finished=true;}
    state->Pause();
}}
void Release(MMDApp* app){states.erase(app);}
}
