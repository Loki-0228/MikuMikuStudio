#include "mikumikustudio/local_data_path.hpp"
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/antialias_motion.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/graphics_runtime.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/pmm_project_settings.hpp"
#include "mikumikustudio/transparent_capture.hpp"
#include "antialias_shader.hpp"
#include "fxaa_shader.hpp"
#include "outline_coverage_shader.hpp"
#include "outline_multisample.hpp"
#include "mikumikustudio/model.hpp"
#include "smaa_shader.hpp"
#include "smaa_lookup.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include <unordered_map>

namespace mikumikustudio::antialias {
using Microsoft::WRL::ComPtr;
namespace {
struct Target {
    ComPtr<IDirect3DTexture9> scene, history[2];
    ComPtr<IDirect3DSurface9> color, depth, historySurface[2];
    ComPtr<IDirect3DTexture9> historyDepth;
    ComPtr<IDirect3DSurface9> historyDepthSurface;
    ComPtr<IDirect3DPixelShader9> fxaa, temporal, smaa[4];
    ComPtr<IDirect3DVertexShader9> outlineVertex;
    ComPtr<IDirect3DPixelShader9> outlinePixel;
    ComPtr<IDirect3DPixelShader9> outlineMaskPixel;
    ComPtr<IDirect3DVertexDeclaration9> outlineDeclaration;
    ComPtr<IDirect3DTexture9> outlineMask;
    ComPtr<IDirect3DSurface9> outlineMaskSurface;
    ComPtr<IDirect3DPixelShader9> nativeFilterPixel;
    ComPtr<IDirect3DTexture9> filteredScene;
    ComPtr<IDirect3DSurface9> filteredSceneSurface;
    unsigned outlineMrtCount=0;
    bool outlineMaskValid=false,outlineMaskWritten=false;
    bool outlineStateFailed=false;
    bool outlineAttempted=false;
    ComPtr<IDirect3DTexture9> edges, weights, areaLookup, searchLookup;
    ComPtr<IDirect3DSurface9> edgeSurface, weightSurface;
    D3DSURFACE_DESC description{};
    D3DFORMAT depthFormat=D3DFMT_UNKNOWN;
    unsigned samples=0, historyIndex=0;
    float historyJitterX=0, historyJitterY=0;
    Algorithm algorithm=Algorithm::MmdOriginal;
    D3DVIEWPORT9 historyViewport{};
    int frame=0;
    bool recording=false, temporalUnavailable=false;
};
std::unordered_map<MMDApp*,Target> targets;
void DisableOutlineMask(Target& target,const char* stage,HRESULT result) {
    runtime_log::Write("AA native outline mask disabled at %s hr=0x%08lX; using spatial fallback",stage,result);
    target.outlineMaskValid=false;target.outlineMaskWritten=false;
    target.outlineMask.Reset();target.outlineMaskSurface.Reset();target.outlineMaskPixel.Reset();
    target.nativeFilterPixel.Reset();target.filteredScene.Reset();target.filteredSceneSurface.Reset();
}
// State blocks exclude render targets. Detach MME's optional MRTs before the
// fullscreen pass, then restore them even if a query/binding step fails.
struct ExtraRenderTargetScope {
    IDirect3DDevice9* device;
    ComPtr<IDirect3DSurface9> color,depth,extra[3];
    D3DVIEWPORT9 viewport;
    unsigned count=0;
    bool pending=false;
    ExtraRenderTargetScope(IDirect3DDevice9* value,IDirect3DSurface9* output,
                           IDirect3DSurface9* outputDepth,const D3DVIEWPORT9& view):
        device(value),color(output),depth(outputDepth),viewport(view) {}
    HRESULT CaptureAndUnbind() {
        D3DCAPS9 caps{};
        HRESULT hr=device->GetDeviceCaps(&caps);if(FAILED(hr))return hr;
        count=std::min(3u,caps.NumSimultaneousRTs>0?unsigned(caps.NumSimultaneousRTs-1):0u);
        for(unsigned i=0;i<count;++i) {
            hr=device->GetRenderTarget(i+1,&extra[i]);
            if(hr!=D3DERR_NOTFOUND && FAILED(hr))return hr;
        }
        pending=true;
        for(unsigned i=0;i<count;++i)if(FAILED(hr=device->SetRenderTarget(i+1,nullptr)))return hr;
        return S_OK;
    }
    HRESULT Restore() {
        if(!pending)return S_OK;pending=false;
        HRESULT result=S_OK;
        const auto remember=[&](HRESULT hr) {if(SUCCEEDED(result) && FAILED(hr))result=hr;};
        // First remove intermediates whose size/depth may differ from the
        // original output, then restore all original bindings and viewport.
        remember(device->SetDepthStencilSurface(nullptr));
        for(unsigned i=0;i<count;++i)remember(device->SetRenderTarget(i+1,nullptr));
        remember(device->SetRenderTarget(0,color.Get()));
        remember(device->SetDepthStencilSurface(depth.Get()));
        for(unsigned i=0;i<count;++i)remember(device->SetRenderTarget(i+1,extra[i].Get()));
        remember(device->SetViewport(&viewport));
        if(FAILED(result))runtime_log::Write("AA extra render target restoration failed hr=0x%08lX",result);
        return result;
    }
    ~ExtraRenderTargetScope(){Restore();}
};
bool Smaa(Algorithm algorithm) {
    return algorithm==Algorithm::Smaa || algorithm==Algorithm::SmaaStandard ||
        algorithm==Algorithm::SmaaT2xStandard || algorithm==Algorithm::SmaaT2xEnhanced;
}
bool SmaaTemporal(Algorithm algorithm) {
    return algorithm==Algorithm::SmaaT2xStandard || algorithm==Algorithm::SmaaT2xEnhanced;
}
bool Temporal(Algorithm algorithm) {return algorithm==Algorithm::Taa || SmaaTemporal(algorithm);}
unsigned SmaaVariant(Algorithm algorithm) {
    return (SmaaTemporal(algorithm)?2u:0u)+
        (algorithm==Algorithm::Smaa || algorithm==Algorithm::SmaaT2xEnhanced?1u:0u);
}
std::filesystem::path StorePath() {
    wchar_t path[32768]{};
    DWORD n=GetEnvironmentVariableW(L"MMS_ANTIALIAS_CONFIG",path,32768);
    if(n && n<32768)return path;
    return local_data::File(L"antialias.ini");
}
void Save(MMDApp* app) {
    auto path=StorePath();if(path.empty())return;
    std::error_code error;std::filesystem::create_directories(path.parent_path(),error);
    const auto& settings=app->antialiasing;
    WritePrivateProfileStringW(L"Antialias",L"Algorithm",std::to_wstring(unsigned(settings.algorithm)).c_str(),path.c_str());
    WritePrivateProfileStringW(L"Antialias",L"LastSmaaAlgorithm",std::to_wstring(unsigned(settings.lastSmaaAlgorithm)).c_str(),path.c_str());
    WritePrivateProfileStringW(L"Antialias",L"OutlineMode",std::to_wstring(unsigned(settings.outlineMode)).c_str(),path.c_str());
    WritePrivateProfileStringW(L"Antialias",L"Enabled",settings.enabled?L"1":L"0",path.c_str());
}
void ReadUserPreferences(Settings& settings) {
    const auto path=StorePath();
    const auto algorithm=GetPrivateProfileIntW(L"Antialias",L"Algorithm",0,path.c_str());
    settings.algorithm=algorithm<unsigned(Algorithm::Count)?Algorithm(algorithm):Algorithm::MmdOriginal;
    settings.enabled=GetPrivateProfileIntW(L"Antialias",L"Enabled",1,path.c_str())!=0;
    const auto last=Algorithm(GetPrivateProfileIntW(L"Antialias",L"LastSmaaAlgorithm",unsigned(Algorithm::Smaa),path.c_str()));
    settings.lastSmaaAlgorithm=Smaa(last)?last:Algorithm::Smaa;
    // The independent MSAA outline experiment is retired. Native MSAA remains
    // an AA algorithm; post-process outlines now use the direct soft profile.
    settings.outlineMode=OutlineMode::FxaaAdaptive;
}
void ResolveUnsupported(MMDApp* app) {
    if(!app->Renderer() || !app->Renderer()->device)return;
    auto& settings=app->antialiasing;
    if(!settings.enabled || settings.algorithm==Algorithm::MmdOriginal || Supported(app,settings.algorithm))return;
    if(SmaaTemporal(settings.algorithm))settings.algorithm=settings.algorithm==Algorithm::SmaaT2xStandard?
        Algorithm::SmaaStandard:Algorithm::Smaa;
    if(!Supported(app,settings.algorithm))settings.algorithm=Algorithm::Fxaa;
    if(!Supported(app,settings.algorithm)){settings.algorithm=Algorithm::MmdOriginal;settings.enabled=false;}
}
bool PostProcess(const Settings& settings) {
    return settings.enabled && (settings.algorithm==Algorithm::Fxaa || settings.algorithm==Algorithm::Taa || Smaa(settings.algorithm));
}
unsigned Samples(MMDApp* app,Algorithm algorithm) {
    switch(algorithm) {
    case Algorithm::Msaa2:return 2;
    case Algorithm::Msaa4:return 4;
    case Algorithm::Msaa8:return 8;
    default:return app->antialiasing.originalSamples;
    }
}
struct ShaderCompilation {
    std::vector<DWORD> bytecode;
    HANDLE completed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<bool> done{false};
    HRESULT result=E_FAIL;
    ~ShaderCompilation(){if(completed)CloseHandle(completed);}
};
// The UI owns this cache. Workers own only CPU bytecode, never a device or app.
// Retain it across resize/device reset instead of recompiling both algorithms.
std::shared_ptr<ShaderCompilation> shaders[18];
std::shared_ptr<ShaderCompilation> RequestShader(unsigned index) {
    if(shaders[index])return shaders[index];
    auto work=std::make_shared<ShaderCompilation>();shaders[index]=work;
    static constexpr const char* entries[]={"fxaa","temporal","smaaEdges","smaaWeights","smaaResolve","smaaTemporal"};
    const char* entry=entries[index<2?index:2+(index-2)%4];
    const auto variant=index>=2?(index-2)/4:0;
    const char* source=index>=2?kSmaaShader:index==0?kFxaaShader:kShader;
    const auto sourceSize=index>=2?sizeof(kSmaaShader)-1:index==0?sizeof(kFxaaShader)-1:sizeof(kShader)-1;
    runtime_log::Write("AA compile queued %s",entry);
    try {
        if(!work->completed)throw std::bad_alloc();
        std::thread([work,entry,source,sourceSize,variant] {
            const auto start=GetTickCount64();
            try {
                ComPtr<ID3DBlob> code,error;
                const D3D_SHADER_MACRO macros[]={{"MMS_SMAA_ENHANCED",variant&1?"1":"0"},
                    {"MMS_SMAA_TEMPORAL",variant&2?"1":"0"},{nullptr,nullptr}};
                work->result=graphics_runtime::Compile(source,sourceSize,"antialias",macros,nullptr,
                    entry,"ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
                if(SUCCEEDED(work->result) && code) {
                    const auto* words=static_cast<const DWORD*>(code->GetBufferPointer());
                    work->bytecode.assign(words,words+code->GetBufferSize()/sizeof(DWORD));
                } else {
                    if(SUCCEEDED(work->result))work->result=E_FAIL;
                    runtime_log::Write("AA compile %s hr=0x%08lX %s",entry,work->result,
                        error?static_cast<const char*>(error->GetBufferPointer()):"");
                }
            }catch(...){work->result=E_OUTOFMEMORY;}
            runtime_log::Write("AA compile finished %s ms=%llu",entry,GetTickCount64()-start);
            work->done.store(true,std::memory_order_release);
            SetEvent(work->completed);
        }).detach();
    }catch(...) {
        work->result=E_OUTOFMEMORY;work->done.store(true,std::memory_order_release);
        if(work->completed)SetEvent(work->completed);
    }
    return work;
}
HRESULT Ensure(MMDApp* app) {
    const auto algorithm=app->antialiasing.algorithm;
    const bool useSmaa=Smaa(algorithm),useTemporal=Temporal(algorithm);
    const unsigned shaderBase=2+4*SmaaVariant(algorithm);
    auto fxaa=useSmaa?nullptr:RequestShader(0);
    auto nativeFilter=app->antialiasing.outlineMode==OutlineMode::FxaaAdaptive?
        RequestShader(0):nullptr;
    auto temporal=algorithm==Algorithm::Taa?RequestShader(1):nullptr;
    auto edges=useSmaa?RequestShader(shaderBase):nullptr;
    auto weights=useSmaa?RequestShader(shaderBase+1):nullptr;
    auto resolve=useSmaa?RequestShader(shaderBase+2):nullptr;
    auto smaaTemporal=SmaaTemporal(algorithm)?RequestShader(shaderBase+3):nullptr;
    // The same preset's unjittered neighborhood shader is the motion fallback.
    auto smaaFallback=SmaaTemporal(algorithm)?RequestShader(shaderBase-8+2):nullptr;
    for(const auto& shader:{fxaa,nativeFilter,temporal,edges,weights,resolve,smaaTemporal,smaaFallback})if(shader) {
        // Export waits for the requested output; preview never waits for HLSL.
        if(app->RecordingWindow() && !shader->done.load(std::memory_order_acquire))
            WaitForSingleObject(shader->completed,INFINITE);
        if(!shader->done.load(std::memory_order_acquire))return S_FALSE;
        if(FAILED(shader->result))return shader->result;
    }
    auto* device=app->Renderer()->device;
    ComPtr<IDirect3DSurface9> color,depth;
    HRESULT hr=device->GetRenderTarget(0,&color);if(FAILED(hr))return hr;
    if(FAILED(hr=device->GetDepthStencilSurface(&depth)))return hr;
    D3DSURFACE_DESC desc{},dd{};
    if(FAILED(hr=color->GetDesc(&desc)) || FAILED(hr=depth->GetDesc(&dd)))return hr;
    auto found=targets.find(app);
    if(found!=targets.end() && found->second.description.Width==desc.Width &&
       found->second.description.Height==desc.Height && found->second.description.Format==desc.Format &&
       found->second.depthFormat==dd.Format &&
       found->second.algorithm==algorithm &&
       (useSmaa?bool(found->second.smaa[2]):bool(found->second.fxaa)) &&
       (!useTemporal || found->second.history[0]))return S_OK;
    Target next;next.description=desc;next.depthFormat=dd.Format;next.algorithm=algorithm;
    const auto texture=[&](ComPtr<IDirect3DTexture9>& tex,ComPtr<IDirect3DSurface9>& surface,D3DFORMAT format) {
        HRESULT result=device->CreateTexture(desc.Width,desc.Height,1,D3DUSAGE_RENDERTARGET,
            format,D3DPOOL_DEFAULT,&tex,nullptr);
        return FAILED(result)?result:tex->GetSurfaceLevel(0,&surface);
    };
    if(FAILED(hr=texture(next.scene,next.color,desc.Format)))return hr;
    if(app->antialiasing.outlineMode==OutlineMode::FxaaAdaptive) {
        D3DCAPS9 caps{};
        const HRESULT capability=device->GetDeviceCaps(&caps);
        if(FAILED(capability))DisableOutlineMask(next,"device capabilities",capability);
        else if(caps.NumSimultaneousRTs<2 || !(caps.PrimitiveMiscCaps&D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING))
            DisableOutlineMask(next,"MRT alpha blending unsupported",D3DERR_NOTAVAILABLE);
        else {
            const HRESULT mask=texture(next.outlineMask,next.outlineMaskSurface,desc.Format);
            if(FAILED(mask))DisableOutlineMask(next,"allocation",mask);
            else next.outlineMrtCount=std::min(4u,unsigned(caps.NumSimultaneousRTs));
        }
        if(nativeFilter && next.outlineMask) {
            HRESULT filter=texture(next.filteredScene,next.filteredSceneSurface,D3DFMT_A8R8G8B8);
            if(SUCCEEDED(filter))filter=device->CreatePixelShader(nativeFilter->bytecode.data(),&next.nativeFilterPixel);
            if(FAILED(filter))DisableOutlineMask(next,"native-only filter allocation",filter);
        }
    }
    if(useTemporal) {
        for(int i=0;i<2;++i)if(FAILED(hr=texture(next.history[i],next.historySurface[i],
            useSmaa?D3DFMT_A8R8G8B8:desc.Format)))return hr;
        if(FAILED(hr=device->CreateTexture(desc.Width,desc.Height,1,D3DUSAGE_RENDERTARGET,
            D3DFMT_A32B32G32R32F,D3DPOOL_DEFAULT,&next.historyDepth,nullptr)) ||
           FAILED(hr=next.historyDepth->GetSurfaceLevel(0,&next.historyDepthSurface)))return hr;
    }
    if(useSmaa) {
        const auto intermediate=[&](ComPtr<IDirect3DTexture9>& tex,ComPtr<IDirect3DSurface9>& surface) {
            HRESULT result=device->CreateTexture(desc.Width,desc.Height,1,D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&tex,nullptr);
            return FAILED(result)?result:tex->GetSurfaceLevel(0,&surface);
        };
        if(FAILED(hr=intermediate(next.edges,next.edgeSurface)) ||
           FAILED(hr=intermediate(next.weights,next.weightSurface)) ||
           FAILED(hr=CreateSmaaLookups(device,next.areaLookup,next.searchLookup)))return hr;
        const std::shared_ptr<ShaderCompilation> programs[]={edges,weights,resolve};
        for(unsigned i=0;i<3;++i)if(FAILED(hr=device->CreatePixelShader(programs[i]->bytecode.data(),&next.smaa[i])))return hr;
        if(smaaTemporal && FAILED(hr=device->CreatePixelShader(smaaTemporal->bytecode.data(),&next.smaa[3])))return hr;
        if(smaaFallback && FAILED(hr=device->CreatePixelShader(smaaFallback->bytecode.data(),&next.fxaa)))return hr;
    }
    if(FAILED(hr=device->CreateDepthStencilSurface(desc.Width,desc.Height,dd.Format,
        D3DMULTISAMPLE_NONE,0,TRUE,&next.depth,nullptr)))return hr;
    if(fxaa && FAILED(hr=device->CreatePixelShader(fxaa->bytecode.data(),&next.fxaa)))return hr;
    if(temporal && FAILED(hr=device->CreatePixelShader(temporal->bytecode.data(),&next.temporal)))return hr;
    motion::Release(app);targets[app]=std::move(next);return S_OK;
}
float Halton(unsigned index,unsigned base) {
    float result=0,fraction=1;
    while(index){fraction/=base;result+=fraction*(index%base);index/=base;}
    return result;
}
void FullscreenState(IDirect3DDevice9* device) {
    device->SetVertexShader(nullptr);device->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
    for(auto state:{D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_STENCILENABLE,D3DRS_ALPHABLENDENABLE,
        D3DRS_ALPHATESTENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_FOGENABLE,D3DRS_LIGHTING,
        D3DRS_CLIPPLANEENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_MULTISAMPLEANTIALIAS})device->SetRenderState(state,FALSE);
    device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,15);
    for(unsigned sampler=0;sampler<6;++sampler) {
        device->SetSamplerState(sampler,D3DSAMP_MINFILTER,sampler<2?D3DTEXF_LINEAR:D3DTEXF_POINT);
        device->SetSamplerState(sampler,D3DSAMP_MAGFILTER,sampler<2?D3DTEXF_LINEAR:D3DTEXF_POINT);
        device->SetSamplerState(sampler,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
        device->SetSamplerState(sampler,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
        device->SetSamplerState(sampler,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
        device->SetSamplerState(sampler,D3DSAMP_SRGBTEXTURE,FALSE);
    }
}
HRESULT Draw(IDirect3DDevice9* device,IDirect3DTexture9* source,IDirect3DPixelShader9* shader,
             const D3DVIEWPORT9& viewport,unsigned width,unsigned height) {
    struct Vertex{float x,y,z,w,u,v;};
    const float l=float(viewport.X),t=float(viewport.Y),r=l+viewport.Width,b=t+viewport.Height;
    const Vertex quad[]={{l-.5f,t-.5f,0,1,l/width,t/height},{r-.5f,t-.5f,0,1,r/width,t/height},
        {l-.5f,b-.5f,0,1,l/width,b/height},{r-.5f,b-.5f,0,1,r/width,b/height}};
    device->SetViewport(&viewport);device->SetTexture(0,source);device->SetPixelShader(shader);
    if(!shader) {
        device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
        device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
        device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
        device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
        device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE);
        device->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0);
        device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    }
    return device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex));
}
}

void Initialize(MMDApp* app) {
    if(build_profile::original || !app || app->antialiasing.initialized || !app->Renderer() || !app->Renderer()->device)return;
    auto& s=app->antialiasing;const auto& pp=app->Renderer()->presentParameters;
    s.initialized=true;s.originalSamples=unsigned(pp.MultiSampleType);s.originalQuality=pp.MultiSampleQuality;
    if(!s.projectOverride)ReadUserPreferences(s);
    // Startup keeps the existing device configuration. Explicit sample counts
    // are applied lazily by the first frame, after UI/device creation finishes.
    ResolveUnsupported(app);
    RefreshMenu(app);ApplyRenderState(app);
}
void ApplyProjectSettings(MMDApp* app,bool enabled,Algorithm algorithm,Algorithm lastSmaa) {
    if(!app || build_profile::original)return;
    Initialize(app);Release(app);
    auto& settings=app->antialiasing;settings.projectOverride=true;
    settings.enabled=enabled;settings.algorithm=algorithm<Algorithm::Count?algorithm:Algorithm::MmdOriginal;
    settings.lastSmaaAlgorithm=Smaa(lastSmaa)?lastSmaa:Algorithm::Smaa;
    ResolveUnsupported(app);RefreshMenu(app);ApplyRenderState(app);
}
void ReloadUserSettings(MMDApp* app) {
    if(!app || build_profile::original)return;
    Release(app);app->antialiasing.projectOverride=false;
    ReadUserPreferences(app->antialiasing);ResolveUnsupported(app);
    RefreshMenu(app);ApplyRenderState(app);
}
bool Supported(MMDApp* app,Algorithm algorithm) {
    if(!app || !app->Renderer() || !app->Renderer()->device || algorithm>=Algorithm::Count)return false;
    auto* r=app->Renderer();
    if(algorithm==Algorithm::Fxaa || algorithm==Algorithm::Taa || Smaa(algorithm)) {
        D3DCAPS9 caps{};
        if(FAILED(r->device->GetDeviceCaps(&caps)) || caps.PixelShaderVersion<D3DPS_VERSION(3,0))return false;
        if(algorithm==Algorithm::Fxaa)return true;
        D3DDEVICE_CREATION_PARAMETERS created{};D3DDISPLAYMODE display{};
        if(FAILED(r->device->GetCreationParameters(&created)) ||
           FAILED(r->d3d9->GetAdapterDisplayMode(created.AdapterOrdinal,&display)))return false;
        const auto formatSupported=[&](D3DFORMAT format,DWORD usage) {
            return SUCCEEDED(r->d3d9->CheckDeviceFormat(created.AdapterOrdinal,created.DeviceType,
                display.Format,usage,D3DRTYPE_TEXTURE,format));
        };
        const bool temporalSupported=caps.VertexShaderVersion>=D3DVS_VERSION(3,0) &&
            formatSupported(D3DFMT_A32B32G32R32F,D3DUSAGE_RENDERTARGET) &&
            SUCCEEDED(r->d3d9->CheckDepthStencilMatch(created.AdapterOrdinal,created.DeviceType,
                display.Format,D3DFMT_A32B32G32R32F,r->presentParameters.AutoDepthStencilFormat));
        if(algorithm==Algorithm::Taa)return temporalSupported;
        if(SmaaTemporal(algorithm) && !temporalSupported)return false;
        // Vendor-neutral RGBA8 tables and targets; no NVAPI dependency.
        return caps.MaxTextureWidth>=AREATEX_WIDTH && caps.MaxTextureHeight>=AREATEX_HEIGHT &&
            (!(caps.TextureCaps&D3DPTEXTURECAPS_POW2) || (caps.TextureCaps&D3DPTEXTURECAPS_NONPOW2CONDITIONAL)) &&
            (caps.TextureFilterCaps&D3DPTFILTERCAPS_MINFLINEAR) && (caps.TextureFilterCaps&D3DPTFILTERCAPS_MAGFLINEAR) &&
            formatSupported(D3DFMT_A8R8G8B8,D3DUSAGE_RENDERTARGET|D3DUSAGE_QUERY_FILTER);
    }
    unsigned samples=Samples(app,algorithm);if(!samples)return false;
    D3DDEVICE_CREATION_PARAMETERS created{};
    if(FAILED(r->device->GetCreationParameters(&created)))return false;
    const auto& pp=r->presentParameters;
    DWORD color=0,depth=0;
    return SUCCEEDED(r->d3d9->CheckDeviceMultiSampleType(created.AdapterOrdinal,created.DeviceType,
        pp.BackBufferFormat,pp.Windowed,D3DMULTISAMPLE_TYPE(samples),&color)) && color &&
        SUCCEEDED(r->d3d9->CheckDeviceMultiSampleType(created.AdapterOrdinal,created.DeviceType,
        pp.AutoDepthStencilFormat,pp.Windowed,D3DMULTISAMPLE_TYPE(samples),&depth)) && depth;
}
void ApplyRenderState(MMDApp* app) {
    if(build_profile::original || !app || !app->Renderer() || !app->Renderer()->device)return;
    app->Renderer()->device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,
        app->antialiasing.enabled && !PostProcess(app->antialiasing));
}
void Release(MMDApp* app){outline_multisample::Release(app);motion::Release(app);targets.erase(app);}
bool DrawOutline(MMDApp* app,unsigned char* model,unsigned firstIndex,
                 unsigned indexCount,const float* uniformColor) {
    if(build_profile::original || !app || !model || !PostProcess(app->antialiasing))return false;
    const auto found=targets.find(app);if(found==targets.end())return false;
    auto& t=found->second;
    // This pass supplies its own native coverage shaders. Submitting through
    // MME's proxy lets its material draw callback replace that mask shader,
    // so motion capture would see an external shader writing our bound MRT.
    // Use the scene's runtime owner and restore its actual state afterwards,
    // as the independent multisample outline pass does.
    ComPtr<IDirect3DDevice9> native;
    if(FAILED(t.color->GetDevice(&native)))return false;
    auto* device=native.Get();
    // The frame will be rejected by Complete. Returning handled prevents the
    // caller from issuing a second legacy outline draw into broken bindings.
    if(t.outlineStateFailed)return true;
    ComPtr<IDirect3DSurface9> target;
    if(FAILED(device->GetRenderTarget(0,&target)) || target.Get()!=t.color.Get())return false;
    if(outline_multisample::Active(app))
        return outline_multisample::Draw(app,model,firstIndex,indexCount,uniformColor);
    auto* body=mdl::ResourceAs<IDirect3DVertexBuffer9>(mdl::Mdl(model)->vertexBuffer);
    if(!body)return false;
    if(!t.outlineAttempted) {
        t.outlineAttempted=true;
        ComPtr<ID3DBlob> vs,ps,error;
        HRESULT hr=graphics_runtime::Compile(kOutlineCoverageShader,sizeof(kOutlineCoverageShader)-1,
            "outline coverage",nullptr,nullptr,"outlineVertex","vs_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error);
        if(SUCCEEDED(hr))hr=graphics_runtime::Compile(kOutlineCoverageShader,sizeof(kOutlineCoverageShader)-1,
            "outline coverage",nullptr,nullptr,"outlinePixel","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error);
        const D3DVERTEXELEMENT9 elements[]={
            {0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
            {0,12,D3DDECLTYPE_D3DCOLOR,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0},
            {1,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,1},
            {1,12,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_NORMAL,0},D3DDECL_END()};
        if(SUCCEEDED(hr))hr=device->CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()),&t.outlineVertex);
        if(SUCCEEDED(hr))hr=device->CreatePixelShader(static_cast<const DWORD*>(ps->GetBufferPointer()),&t.outlinePixel);
        if(SUCCEEDED(hr))hr=device->CreateVertexDeclaration(elements,&t.outlineDeclaration);
        if(FAILED(hr)) {
            t.outlineVertex.Reset();t.outlinePixel.Reset();t.outlineDeclaration.Reset();
            runtime_log::Write("Outline coverage unavailable hr=%08lX: %s",hr,error?static_cast<const char*>(error->GetBufferPointer()):"");
        }
        if(t.outlineDeclaration && t.outlineMask) {
            ComPtr<ID3DBlob> mask,error;
            hr=graphics_runtime::Compile(kOutlineCoverageShader,sizeof(kOutlineCoverageShader)-1,
                "outline mask coverage",nullptr,nullptr,"outlineMaskPixel","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&mask,&error);
            if(SUCCEEDED(hr))hr=device->CreatePixelShader(static_cast<const DWORD*>(mask->GetBufferPointer()),&t.outlineMaskPixel);
            if(FAILED(hr)) {
                runtime_log::Write("FXAA native outline mask shader: %s",error?static_cast<const char*>(error->GetBufferPointer()):"");
                DisableOutlineMask(t,"shader creation",hr);
            }
        }
    }
    if(!t.outlineDeclaration)return false;
    D3DMATRIX world{},view{},projection{};D3DVIEWPORT9 viewport{};
    if(FAILED(device->GetTransform(D3DTS_WORLD,&world)) || FAILED(device->GetTransform(D3DTS_VIEW,&view)) ||
       FAILED(device->GetTransform(D3DTS_PROJECTION,&projection)) || FAILED(device->GetViewport(&viewport)))return false;
    const auto multiply=[](const D3DMATRIX& a,const D3DMATRIX& b) {
        D3DMATRIX result{};
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)
            result.m[i][j]+=a.m[i][k]*b.m[k][j];
        return result;
    };
    const auto transform=multiply(multiply(world,view),projection);
    float constants[28]{};std::memcpy(constants,&transform,sizeof(transform));
    constants[16]=viewport.Width*.5f;constants[17]=viewport.Height*.5f;
    if(uniformColor){std::memcpy(constants+20,uniformColor,16);constants[24]=1;}
    // Restore the surrounding effect pass and the second vertex stream too;
    // motion capture and subsequent accessory draws observe these bindings.
    ComPtr<IDirect3DVertexShader9> oldVertex;ComPtr<IDirect3DPixelShader9> oldPixel;
    ComPtr<IDirect3DVertexDeclaration9> oldDeclaration;ComPtr<IDirect3DVertexBuffer9> oldStream;
    UINT oldOffset=0,oldStride=0;float oldConstants[28]{};
    constexpr D3DRENDERSTATETYPE states[]={D3DRS_ALPHABLENDENABLE,D3DRS_SRCBLEND,D3DRS_DESTBLEND,
        D3DRS_BLENDOP,D3DRS_ALPHATESTENABLE,D3DRS_ALPHAFUNC,D3DRS_ALPHAREF};
    DWORD values[_countof(states)]{};
    if(FAILED(device->GetVertexShader(&oldVertex)) || FAILED(device->GetPixelShader(&oldPixel)) ||
       FAILED(device->GetVertexDeclaration(&oldDeclaration)) ||
       FAILED(device->GetStreamSource(1,&oldStream,&oldOffset,&oldStride)) ||
       FAILED(device->GetVertexShaderConstantF(0,oldConstants,7)))return false;
    for(unsigned i=0;i<_countof(states);++i)if(FAILED(device->GetRenderState(states[i],&values[i])))return false;
    // Only native outlines on the main AA scene can mark this mask. Preserve
    // an effect's existing MRT instead of intercepting its secondary output.
    ComPtr<IDirect3DSurface9> oldExtra;
    DWORD oldExtraWrites=0;
    bool maskBound=false;
    if(t.outlineMaskValid && t.outlineMaskPixel) {
        HRESULT mask=S_OK;
        bool externalTarget=false;
        for(unsigned i=1;i<t.outlineMrtCount;++i) {
            ComPtr<IDirect3DSurface9> external;
            mask=device->GetRenderTarget(i,&external);
            if(mask==D3DERR_NOTFOUND)mask=S_OK;
            if(FAILED(mask))break;
            if(i==1)oldExtra=external;
            if(external){externalTarget=true;break;}
        }
        if(SUCCEEDED(mask) && externalTarget) {
            DisableOutlineMask(t,"effect already uses additional targets",D3DERR_NOTAVAILABLE);
        } else {
            if(SUCCEEDED(mask))mask=device->GetRenderState(D3DRS_COLORWRITEENABLE1,&oldExtraWrites);
            if(SUCCEEDED(mask))mask=device->SetRenderTarget(1,t.outlineMaskSurface.Get());
            if(SUCCEEDED(mask)) {
                maskBound=true;
                // SetRenderTarget may reset viewport state. Keep inset/export
                // viewports identical to the surrounding native pass.
                mask=device->SetViewport(&viewport);
                if(SUCCEEDED(mask))mask=device->SetRenderState(D3DRS_COLORWRITEENABLE1,15);
            }
            if(FAILED(mask)) {
                HRESULT restore=S_OK;
                if(maskBound) {
                    // A failed write-mask/viewport setup may leave RT1 bound.
                    // Retry restoration before permitting the ordinary draw.
                    for(unsigned attempt=0;attempt<2;++attempt) {
                        const HRESULT restored=device->SetRenderTarget(1,oldExtra.Get());
                        const HRESULT writes=device->SetRenderState(D3DRS_COLORWRITEENABLE1,oldExtraWrites);
                        const HRESULT view=device->SetViewport(&viewport);
                        restore=FAILED(restored)?restored:FAILED(writes)?writes:view;
                        if(SUCCEEDED(restore))break;
                        runtime_log::Write("FXAA outline mask setup restore failed attempt=%u target=0x%08lX writes=0x%08lX viewport=0x%08lX",
                            attempt+1,restored,writes,view);
                    }
                }
                maskBound=false;DisableOutlineMask(t,"secondary target binding",mask);
                if(FAILED(restore)){t.outlineStateFailed=true;return true;}
            }
        }
    }
    const auto additional=mdl::Mdl(model)->pmxAdditionalUvCount;
    const UINT stride=sizeof(mdl::SkinnedVertexBase)+(additional>=1 && additional<=4?16*additional:0);
    HRESULT hr=device->SetStreamSource(1,body,0,stride);
    if(SUCCEEDED(hr))hr=device->SetVertexDeclaration(t.outlineDeclaration.Get());
    if(SUCCEEDED(hr))hr=device->SetVertexShader(t.outlineVertex.Get());
    if(SUCCEEDED(hr))hr=device->SetPixelShader(maskBound?t.outlineMaskPixel.Get():t.outlinePixel.Get());
    if(SUCCEEDED(hr))hr=device->SetVertexShaderConstantF(0,constants,7);
    const DWORD coverageStates[]={TRUE,D3DBLEND_SRCALPHA,D3DBLEND_INVSRCALPHA,D3DBLENDOP_ADD,
        TRUE,D3DCMP_GREATER,0};
    for(unsigned i=0;i<_countof(states);++i)if(SUCCEEDED(hr))hr=device->SetRenderState(states[i],coverageStates[i]);
    if(SUCCEEDED(hr))hr=device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,mdl::Mdl(model)->vertexCount,firstIndex,indexCount/3);
    if(maskBound && SUCCEEDED(hr))t.outlineMaskWritten=true;
    device->SetVertexShader(oldVertex.Get());device->SetPixelShader(oldPixel.Get());
    device->SetVertexDeclaration(oldDeclaration.Get());device->SetStreamSource(1,oldStream.Get(),oldOffset,oldStride);
    device->SetVertexShaderConstantF(0,oldConstants,7);
    for(unsigned i=0;i<_countof(states);++i)device->SetRenderState(states[i],values[i]);
    if(maskBound) {
        const HRESULT restored=device->SetRenderTarget(1,oldExtra.Get());
        const HRESULT writes=device->SetRenderState(D3DRS_COLORWRITEENABLE1,oldExtraWrites);
        const HRESULT view=device->SetViewport(&viewport);
        if(FAILED(restored) || FAILED(writes) || FAILED(view)) {
            runtime_log::Write("FXAA outline mask restore failed target=0x%08lX writes=0x%08lX viewport=0x%08lX",restored,writes,view);
            const HRESULT restore=FAILED(restored)?restored:FAILED(writes)?writes:view;
            DisableOutlineMask(t,"secondary target restoration",restore);
            t.outlineStateFailed=true;
        } else if(FAILED(hr))DisableOutlineMask(t,"outline drawing",hr);
    }
    return t.outlineStateFailed || SUCCEEDED(hr);
}
void FinishOutlines(MMDApp* app) {
    if(build_profile::original || !app)return;
    if(!outline_multisample::Finish(app)) {
        const auto found=targets.find(app);
        if(found!=targets.end())found->second.outlineStateFailed=true;
    }
}
bool NativeOutlineMaskDraw(MMDApp* app,IDirect3DDevice9* device) {
    if(build_profile::original || !app || !device ||
       app->ActiveRenderPass()!=AccessoryRenderPass::ModelOutline ||
       app->antialiasing.outlineMode!=OutlineMode::FxaaAdaptive || !PostProcess(app->antialiasing))return false;
    const auto found=targets.find(app);if(found==targets.end())return false;
    const auto& target=found->second;
    if(!target.outlineMaskValid || !target.outlineMaskSurface || !target.outlineMaskPixel || target.outlineStateFailed)return false;
    ComPtr<IDirect3DSurface9> color,mask;
    if(FAILED(device->GetRenderTarget(0,&color)) || color.Get()!=target.color.Get() ||
       FAILED(device->GetRenderTarget(1,&mask)) || mask.Get()!=target.outlineMaskSurface.Get()) {
        runtime_log::Trace("AA native mask gate rejected frame=%d targets=%p/%p expected=%p/%p",
            app->CurrentFrame(),color.Get(),mask.Get(),target.color.Get(),target.outlineMaskSurface.Get());
        return false;
    }
    // Matching the exact shader excludes an effect that happens to reuse the
    // auxiliary surface outside our own native outline submission.
    ComPtr<IDirect3DPixelShader9> pixel;
    if(FAILED(device->GetPixelShader(&pixel)) || pixel.Get()!=target.outlineMaskPixel.Get()) {
        runtime_log::Trace("AA native mask gate rejected frame=%d pixel=%p expected=%p",
            app->CurrentFrame(),pixel.Get(),target.outlineMaskPixel.Get());
        return false;
    }
    for(unsigned slot=2;slot<target.outlineMrtCount;++slot) {
        ComPtr<IDirect3DSurface9> extra;
        const auto hr=device->GetRenderTarget(slot,&extra);
        if((FAILED(hr) && hr!=D3DERR_NOTFOUND) || extra) {
            runtime_log::Trace("AA native mask gate rejected frame=%d external_slot=%u target=%p hr=0x%08lX",
                app->CurrentFrame(),slot,extra.Get(),hr);
            return false;
        }
    }
    return true;
}
bool Initializing(MMDApp* app) {
    if(build_profile::original || !app || !PostProcess(app->antialiasing))return false;
    const auto found=targets.find(app);
    return found==targets.end() || found->second.algorithm!=app->antialiasing.algorithm ||
        (Temporal(app->antialiasing.algorithm) && !found->second.history[0]) ||
        (Smaa(app->antialiasing.algorithm) && !found->second.smaa[2]);
}
unsigned AccumulatedSamples(MMDApp* app) {
    const auto found=targets.find(app);return found==targets.end()?0:found->second.samples;
}
unsigned RequiredSamples(MMDApp* app) {
    if(!app || !app->antialiasing.enabled)return 1;
    const auto found=targets.find(app);
    if(found!=targets.end() && found->second.temporalUnavailable)return 1;
    return app->antialiasing.algorithm==Algorithm::Taa?8:SmaaTemporal(app->antialiasing.algorithm)?2:1;
}
void ResetHistory(MMDApp* app) {
    if(app && (app->antialiasing.outlineMode!=OutlineMode::Multisample || !PostProcess(app->antialiasing)))
        outline_multisample::Release(app);
    motion::Release(app);
    if(auto found=targets.find(app);found!=targets.end()) {
        found->second.samples=0;found->second.temporalUnavailable=false;
    }
}
static bool SelectInternal(MMDApp* app,Algorithm algorithm,std::wstring& error,bool userChange) {
    Initialize(app);error.clear();
    if(algorithm>=Algorithm::Count || (app->antialiasing.enabled && !Supported(app,algorithm))) {
        error=app->EnglishUI()==2?L"当前设备不支持此抗锯齿算法。":app->EnglishUI()==1?
            L"This device does not support the selected anti-aliasing mode.":L"このデバイスは、選択したアンチエイリアス方式に対応していません。";return false;
    }
    const auto before=app->antialiasing;
    app->antialiasing.algorithm=algorithm;
    auto* r=app->Renderer();const auto previous=r->presentParameters;
    const unsigned samples=Samples(app,algorithm);
    const unsigned quality=algorithm==Algorithm::MmdOriginal?before.originalQuality:0;
    bool ok=true;
    if(algorithm<=Algorithm::Msaa8 && (samples!=unsigned(previous.MultiSampleType) || quality!=previous.MultiSampleQuality)) {
        r->presentParameters.MultiSampleType=D3DMULTISAMPLE_TYPE(samples);r->presentParameters.MultiSampleQuality=quality;
        ok=PostDeviceReset(app);
    }
    if(ok && PostProcess(app->antialiasing))ok=SUCCEEDED(Ensure(app));
    if(!ok) {
        app->antialiasing=before;r->presentParameters=previous;Release(app);PostDeviceReset(app);
        error=app->EnglishUI()==2?L"无法启用此抗锯齿算法，已恢复之前的设置。":app->EnglishUI()==1?
            L"Could not enable the selected anti-aliasing mode. The previous settings have been restored.":
            L"選択したアンチエイリアス方式を有効にできなかったため、以前の設定に戻しました。";
    } else {
        if(!PostProcess(app->antialiasing))Release(app);
        else ResetHistory(app);
        if(userChange) {
            if(Smaa(algorithm))app->antialiasing.lastSmaaAlgorithm=algorithm;
            Save(app);pmm_settings::AntialiasChanged(*app);
        }
    }
    ApplyRenderState(app);RefreshMenu(app);PostViewRefresh(app);return ok;
}
bool Select(MMDApp* app,Algorithm algorithm,std::wstring& error) {return SelectInternal(app,algorithm,error,true);}
bool SelectOutlineMode(MMDApp* app,OutlineMode mode,std::wstring& error) {
    error.clear();
    if(!app || build_profile::original)return false;
    Initialize(app);
    if(unsigned(mode)>unsigned(OutlineMode::Multisample)) {
        error=app->EnglishUI()==2?L"无效的描边抗锯齿策略。":app->EnglishUI()==1?
            L"Invalid outline anti-aliasing strategy.":L"無効な輪郭線アンチエイリアス方式です。";
        return false;
    }
    if(app->antialiasing.outlineMode==mode){RefreshMenu(app);return true;}
    app->antialiasing.outlineMode=mode;
    // This preference is independent of PMM/VMD and of the selected scene AA
    // algorithm or its enabled state. Do not mark a project change for it.
    Release(app);ResetHistory(app);Save(app);
    RefreshMenu(app);PostViewRefresh(app);
    return true;
}
const wchar_t* DialogTitle(MMDApp* app) {
    return app->EnglishUI()==2?L"抗锯齿":app->EnglishUI()==1?L"Anti-aliasing":L"アンチエイリアス";
}
bool Toggle(MMDApp* app,std::wstring& error) {
    Initialize(app);const bool was=app->antialiasing.enabled;
    app->antialiasing.enabled=!was;
    if(!Select(app,app->antialiasing.algorithm,error)) {
        app->antialiasing.enabled=was;ApplyRenderState(app);RefreshMenu(app);return false;
    }
    return true;
}
void PrepareFrame(MMDApp* app) {
    if(build_profile::original)return;
    Initialize(app);
    // A completed worker cannot post to a potentially destroyed window. Keep
    // polling on the UI thread until the requested frame resources exist.
    if(Initializing(app))app->state.messageSeen=1;
    // Repaint a paused frame (including projects containing only frame zero)
    // until it has both T2x phases. Rendering never advances the timeline.
    if(RequiredSamples(app)>1 && AccumulatedSamples(app)<RequiredSamples(app))app->state.messageSeen=1;
    const auto& s=app->antialiasing;const auto& pp=app->Renderer()->presentParameters;
    if(s.algorithm<=Algorithm::Msaa8 && (Samples(app,s.algorithm)!=unsigned(pp.MultiSampleType) ||
        (s.algorithm==Algorithm::MmdOriginal?s.originalQuality:0)!=pp.MultiSampleQuality)) {
        std::wstring error;
        if(!SelectInternal(app,s.algorithm,error,false))runtime_log::Write("AA restore preference failed");
    }
}

struct FrameScope::Impl {
    MMDApp* app;
    IDirect3DDevice9* device;
    ComPtr<IDirect3DSurface9> previous,depth;
    D3DVIEWPORT9 viewport{};
    D3DMATRIX projection{};
    IDirect3DSurface9* previousSlot=nullptr;
    IDirect3DSurface9* depthSlot=nullptr;
    bool ready=false,restored=false,temporal=false,bindingsChanged=false,completed=false;
    float jitterX=0,jitterY=0;
    explicit Impl(MMDApp* value):app(value),device(value->Renderer()->device) {}
    void Restore() {
        if(restored)return;restored=true;
        device->SetDepthStencilSurface(nullptr);device->SetRenderTarget(0,previous.Get());
        device->SetDepthStencilSurface(depth.Get());device->SetViewport(&viewport);
        device->SetTransform(D3DTS_PROJECTION,&projection);
        app->Renderer()->backbufferSurface=previousSlot;app->Renderer()->depthStencilSurface=depthSlot;
        ApplyRenderState(app);
    }
    ~Impl(){outline_multisample::Cancel(app);if(bindingsChanged)Restore();if(temporal)motion::Cancel(app);}
};
FrameScope::FrameScope(MMDApp* app) {
    if(build_profile::original)return;
    Initialize(app);ApplyRenderState(app);
    if(app->antialiasing.outlineMode!=OutlineMode::Multisample || !PostProcess(app->antialiasing))
        outline_multisample::Release(app);
    if(!PostProcess(app->antialiasing))return;
    if(auto previous=targets.find(app);previous!=targets.end())previous->second.outlineStateFailed=false;
    impl_=std::make_unique<Impl>(app);auto& p=*impl_;
    const HRESULT allocation=Ensure(app);
    if(allocation==S_FALSE){impl_.reset();return;}
    if(FAILED(allocation)) {
        // A stored preference can outlive a compiler/GPU change. Keep drawing
        // the scene and uncheck AA instead of leaving a stale viewport forever.
        runtime_log::Write("AA unavailable hr=0x%08lX",allocation);
        impl_.reset();app->antialiasing.enabled=false;ApplyRenderState(app);RefreshMenu(app);
        const wchar_t* message=app->EnglishUI()==2?L"无法创建抗锯齿画面，已暂时关闭抗锯齿。":
            app->EnglishUI()==0?L"アンチエイリアスを初期化できないため、一時的に無効にしました。":
            L"Anti-aliasing could not be initialized and has been temporarily disabled.";
        MessageBoxW(app->MainWindow(),message,DialogTitle(app),MB_OK|MB_ICONERROR);
        return;
    }
    auto& t=targets.at(app);
    t.outlineMaskValid=false;t.outlineMaskWritten=false;
    if(t.outlineMaskSurface) {
        // FrameScope precedes the scene's Clear/BeginScene. ColorFill clears
        // just this unbound auxiliary target without changing color/depth or
        // viewport state, including when the scene uses an inset viewport.
        const HRESULT cleared=p.device->ColorFill(t.outlineMaskSurface.Get(),nullptr,0);
        if(SUCCEEDED(cleared))t.outlineMaskValid=true;
        else DisableOutlineMask(t,"frame clear",cleared);
    }
    if(FAILED(p.device->GetRenderTarget(0,&p.previous)) || FAILED(p.device->GetDepthStencilSurface(&p.depth)) ||
       FAILED(p.device->GetViewport(&p.viewport)) || FAILED(p.device->GetTransform(D3DTS_PROJECTION,&p.projection)))return;
    p.previousSlot=app->Renderer()->backbufferSurface;p.depthSlot=app->Renderer()->depthStencilSurface;
    p.bindingsChanged=true;
    p.temporal=Temporal(app->antialiasing.algorithm);
    if(p.temporal) {
        // Camera and deformed-geometry motion are reprojected. Only a timeline
        // discontinuity or a change of output domain discards the whole history.
        const auto delta=static_cast<long long>(app->CurrentFrame())-t.frame;
        if(t.samples && (delta<0 || (delta>8 && !app->PlaybackActive() && !app->RecordingWindow()) ||
            t.recording!=(app->RecordingWindow()!=nullptr) || std::memcmp(&t.historyViewport,&p.viewport,sizeof(p.viewport))!=0)) {
            t.samples=0;motion::Release(app);
        }
        t.historyViewport=p.viewport;
        t.frame=app->CurrentFrame();t.recording=app->RecordingWindow()!=nullptr;
        const bool twoPhase=SmaaTemporal(app->antialiasing.algorithm);
        const unsigned sample=t.samples%8+1;
        // Official T2x pattern, expressed in a bottom-to-top projection axis.
        const float phase=t.samples%2?-.25f:.25f;
        const float dx=2*(twoPhase?phase:Halton(sample,2)-.5f)/p.viewport.Width;
        const float dy=2*(twoPhase?-phase:-(Halton(sample,3)-.5f))/p.viewport.Height;
        p.temporal=motion::Begin(app,p.device,t.color.Get(),p.viewport,p.projection,dx,dy);
        t.temporalUnavailable=!p.temporal;
        if(!p.temporal)t.samples=0;
        else {
            p.jitterX=dx*p.viewport.Width/(2*t.description.Width);
            p.jitterY=-dy*p.viewport.Height/(2*t.description.Height);
            auto projection=p.projection;
            for(int i=0;i<4;++i){projection.m[i][0]+=dx*projection.m[i][3];projection.m[i][1]+=dy*projection.m[i][3];}
            p.device->SetTransform(D3DTS_PROJECTION,&projection);
        }
    }
    p.device->SetDepthStencilSurface(nullptr);
    p.ready=SUCCEEDED(p.device->SetRenderTarget(0,t.color.Get())) &&
        SUCCEEDED(p.device->SetDepthStencilSurface(t.depth.Get())) && SUCCEEDED(p.device->SetViewport(&p.viewport));
    app->Renderer()->backbufferSurface=t.color.Get();app->Renderer()->depthStencilSurface=t.depth.Get();
    if(p.ready && app->antialiasing.outlineMode==OutlineMode::Multisample) {
        ComPtr<IDirect3DDevice9> native;
        if(SUCCEEDED(t.color->GetDevice(&native)) && outline_multisample::Begin(app,native.Get(),t.color.Get(),p.viewport))
            outline_multisample::SetSoftening(app,true);
    }
}
FrameScope::~FrameScope()=default;
bool FrameScope::Ready()const{return !impl_ || impl_->ready;}
bool FrameScope::Succeeded()const{return !impl_ || impl_->completed;}
bool FrameScope::Complete() {
    if(!impl_ || !impl_->ready)return false;
    auto& p=*impl_;auto& t=targets.at(p.app);
    // Native ownership avoids invoking MME's frame callbacks a second time.
    ComPtr<IDirect3DDevice9> native;
    if(FAILED(t.color->GetDevice(&native))){p.Restore();return false;}
    auto* device=native.Get();
    if(t.outlineStateFailed || outline_multisample::Failed(p.app)) {
        // Mask acquisition only occurred when every extra target was unbound.
        // Do not capture a leaked mask target as an original external MRT.
        for(unsigned i=1;i<t.outlineMrtCount;++i) {
            const HRESULT detached=device->SetRenderTarget(i,nullptr);
            if(FAILED(detached))runtime_log::Write("AA failed-frame extra target detach failed slot=%u hr=0x%08lX",i,detached);
        }
        p.Restore();t.samples=0;motion::Cancel(p.app);
        runtime_log::Write("AA frame rejected after outline state restoration failure; resolve canceled");
        return false;
    }
    ComPtr<IDirect3DStateBlock9> saved;
    if(FAILED(device->CreateStateBlock(D3DSBT_ALL,&saved))){p.Restore();return false;}
    ExtraRenderTargetScope extraTargets(device,p.previous.Get(),p.depth.Get(),p.viewport);
    const HRESULT detached=extraTargets.CaptureAndUnbind();
    if(FAILED(detached)) {
        extraTargets.Restore();p.Restore();
        runtime_log::Write("AA extra render target capture/detach failed hr=0x%08lX",detached);
        return false;
    }
    p.Restore();
    HRESULT hr=device->BeginScene();
    ComPtr<IDirect3DSurface9> motionSurface;
    bool historyWritten=false;
    if(SUCCEEDED(hr)) {
        const auto& v=p.viewport;const auto& desc=t.description;
        float constants[]={1.f/desc.Width,1.f/desc.Height,0,
            TransparentExportActive(p.app)?1.f:0.f,
            (v.X+.5f)/desc.Width,(v.Y+.5f)/desc.Height,(v.X+v.Width-.5f)/desc.Width,(v.Y+v.Height-.5f)/desc.Height,
            p.jitterX,p.jitterY,.002f,.002f,
            t.historyJitterX,t.historyJitterY,0,0};
        IDirect3DTexture9* currentScene=t.scene.Get();
        FullscreenState(device);
        bool nativePrefiltered=false;
        if(p.app->antialiasing.outlineMode==OutlineMode::FxaaAdaptive &&
           t.nativeFilterPixel && t.filteredScene && t.outlineMaskValid && t.outlineMaskWritten) {
            // Resolve the continuous, width-weighted native hull first. All
            // post-process algorithms, including FXAA, then see the same
            // softened scene instead of bypassing their edge search on lines.
            // Keep scene identity unchanged for draw observation.
            const float noJitter[]={0,0,0,0};
            const float lineOnly[]={1,0,1,0};
            device->SetDepthStencilSurface(nullptr);
            device->SetTexture(0,nullptr);device->SetTexture(1,nullptr);
            hr=device->SetRenderTarget(0,t.filteredSceneSurface.Get());
            if(SUCCEEDED(hr))hr=device->SetPixelShaderConstantF(0,constants,4);
            if(SUCCEEDED(hr))hr=device->SetPixelShaderConstantF(2,noJitter,1);
            if(SUCCEEDED(hr))hr=device->SetPixelShaderConstantF(4,lineOnly,1);
            if(SUCCEEDED(hr))hr=device->SetTexture(1,t.outlineMask.Get());
            if(SUCCEEDED(hr))hr=Draw(device,currentScene,t.nativeFilterPixel.Get(),v,desc.Width,desc.Height);
            const auto remember=[&](HRESULT result){if(SUCCEEDED(hr))hr=result;};
            remember(device->SetTexture(0,nullptr));remember(device->SetTexture(1,nullptr));
            remember(device->SetRenderTarget(0,p.previous.Get()));
            remember(device->SetViewport(&v));
            remember(device->SetPixelShaderConstantF(4,noJitter,1));
            if(SUCCEEDED(hr)) {currentScene=t.filteredScene.Get();nativePrefiltered=true;}
            else {
                runtime_log::Write("AA native-line prefilter failed hr=0x%08lX",hr);
                runtime_log::Trace("AA native-line prefilter failed frame=%d algorithm=%u hr=0x%08lX",
                    p.app->CurrentFrame(),unsigned(t.algorithm),hr);
            }
        }
        if(SUCCEEDED(hr)) {
        auto* vectors=p.temporal?motion::Resolve(p.app,device):nullptr;
        FullscreenState(device);
        constants[2]=vectors?float(std::min(t.samples,31u))/(std::min(t.samples,31u)+1):0;
        device->SetPixelShaderConstantF(0,constants,4);
        const float noMask[]={0,0,0,0};
        device->SetPixelShaderConstantF(4,noMask,1);
        if(Smaa(p.app->antialiasing.algorithm)) {
            const float metrics[]={1.f/desc.Width,1.f/desc.Height,float(desc.Width),float(desc.Height)};
            device->SetPixelShaderConstantF(0,metrics,1);
            const float phase=vectors?float(t.samples%2+1):0;
            const float temporalConstants[]={phase,phase,phase,0,
                vectors && t.samples?1.f:0,.002f,.002f,0,
                vectors?0:p.jitterX,vectors?0:p.jitterY,0,0};
            device->SetPixelShaderConstantF(2,temporalConstants,3);
            const float sampleOffsets[]={p.jitterX,p.jitterY,t.historyJitterX,t.historyJitterY};
            device->SetPixelShaderConstantF(5,sampleOffsets,1);
            for(unsigned i=0;i<3;++i) {
                device->SetSamplerState(i,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
                device->SetSamplerState(i,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
            }
            device->SetDepthStencilSurface(nullptr);
            device->SetTexture(0,nullptr);device->SetTexture(1,nullptr);device->SetTexture(2,nullptr);
            // Clear whole targets, including outside inset scene viewports.
            const D3DVIEWPORT9 full={0,0,desc.Width,desc.Height,0,1};
            hr=device->SetRenderTarget(0,t.edgeSurface.Get());device->SetViewport(&full);
            if(SUCCEEDED(hr))hr=device->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0);
            if(SUCCEEDED(hr))hr=Draw(device,currentScene,t.smaa[0].Get(),v,desc.Width,desc.Height);
            device->SetTexture(0,nullptr);
            if(SUCCEEDED(hr))hr=device->SetRenderTarget(0,t.weightSurface.Get());device->SetViewport(&full);
            if(SUCCEEDED(hr))hr=device->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0);
            device->SetTexture(1,t.areaLookup.Get());device->SetTexture(2,t.searchLookup.Get());
            if(SUCCEEDED(hr))hr=Draw(device,t.edges.Get(),t.smaa[1].Get(),v,desc.Width,desc.Height);
            device->SetTexture(0,nullptr);device->SetTexture(1,nullptr);device->SetTexture(2,nullptr);
            const unsigned next=1-t.historyIndex;
            device->SetRenderTarget(0,vectors?t.historySurface[next].Get():p.previous.Get());
            device->SetTexture(1,t.weights.Get());device->SetTexture(2,vectors);
            // The non-temporal neighborhood shader preserves alpha if motion
            // is unavailable, with the same standard/enhanced spatial preset.
            auto* neighborhood=!vectors && SmaaTemporal(t.algorithm)?t.fxaa.Get():t.smaa[2].Get();
            if(SUCCEEDED(hr))hr=Draw(device,currentScene,neighborhood,v,desc.Width,desc.Height);
            for(unsigned sampler=0;sampler<3;++sampler)device->SetTexture(sampler,nullptr);
            if(vectors && SUCCEEDED(hr)) {
                device->SetRenderTarget(0,p.previous.Get());
                device->SetTexture(1,t.history[t.historyIndex].Get());device->SetTexture(2,vectors);
                device->SetTexture(3,t.historyDepth.Get());device->SetTexture(4,currentScene);
                device->SetTexture(5,t.weights.Get());
                for(unsigned sampler=0;sampler<6;++sampler) {
                    const auto filter=sampler==2 || sampler==3?D3DTEXF_POINT:D3DTEXF_LINEAR;
                    device->SetSamplerState(sampler,D3DSAMP_MINFILTER,filter);
                    device->SetSamplerState(sampler,D3DSAMP_MAGFILTER,filter);
                }
                hr=Draw(device,t.history[next].Get(),t.smaa[3].Get(),v,desc.Width,desc.Height);
                for(unsigned sampler=0;sampler<6;++sampler)device->SetTexture(sampler,nullptr);
                if(SUCCEEDED(hr) && SUCCEEDED(vectors->GetSurfaceLevel(0,&motionSurface))) {
                    t.historyIndex=next;historyWritten=true;
                }
            }
            if(FAILED(hr)) {
                Draw(device,currentScene,nullptr,v,desc.Width,desc.Height);
                p.app->antialiasing.enabled=false;RefreshMenu(p.app);
            }
        } else if(vectors) {
            const unsigned next=1-t.historyIndex;
            device->SetDepthStencilSurface(nullptr);device->SetTexture(1,t.history[t.historyIndex].Get());
            device->SetTexture(2,vectors);device->SetTexture(3,t.historyDepth.Get());
            hr=device->SetRenderTarget(0,t.historySurface[next].Get());
            if(SUCCEEDED(hr))hr=Draw(device,currentScene,t.temporal.Get(),v,desc.Width,desc.Height);
            device->SetTexture(1,nullptr);device->SetTexture(2,nullptr);device->SetTexture(3,nullptr);
            device->SetRenderTarget(0,p.previous.Get());
            if(SUCCEEDED(hr))hr=Draw(device,t.history[next].Get(),nullptr,v,desc.Width,desc.Height);
            if(SUCCEEDED(hr) && SUCCEEDED(vectors->GetSurfaceLevel(0,&motionSurface))) {
                t.historyIndex=next;historyWritten=true;
            }
        } else {
            t.samples=0;motion::Cancel(p.app);
            device->SetDepthStencilSurface(nullptr);device->SetRenderTarget(0,p.previous.Get());
            auto* nativeLines=t.algorithm==Algorithm::Fxaa?outline_multisample::GetTexture(p.app):nullptr;
            const bool mask=!nativePrefiltered && !nativeLines && t.algorithm==Algorithm::Fxaa &&
                t.outlineMaskValid && t.outlineMaskWritten && t.outlineMask;
            const float maskConstants[]={mask?1.f:0.f,nativeLines?1.f:0.f,0,0};
            hr=device->SetTexture(1,nativeLines?nativeLines:mask?t.outlineMask.Get():nullptr);
            if(FAILED(hr) && (mask || nativeLines)) {
                DisableOutlineMask(t,"resolve texture binding",hr);
                const float noMask[]={0,0,0,0};
                hr=device->SetTexture(1,nullptr);
                if(SUCCEEDED(hr))hr=device->SetPixelShaderConstantF(4,noMask,1);
            } else if(SUCCEEDED(hr))hr=device->SetPixelShaderConstantF(4,maskConstants,1);
            if(SUCCEEDED(hr))hr=Draw(device,currentScene,t.fxaa.Get(),v,desc.Width,desc.Height);
            const HRESULT unbound=device->SetTexture(1,nullptr);
            if(SUCCEEDED(hr))hr=unbound;
        }
        }
        const auto end=device->EndScene();if(SUCCEEDED(hr))hr=end;
    }
    // D3D9 requires StretchRect outside BeginScene. Publish geometry and depth
    // history together only after the color resolve and this copy both succeed.
    if(SUCCEEDED(hr) && historyWritten && SUCCEEDED(device->StretchRect(motionSurface.Get(),nullptr,
        t.historyDepthSurface.Get(),nullptr,D3DTEXF_NONE))) {
        motion::Commit(p.app);++t.samples;
        t.historyJitterX=p.jitterX;t.historyJitterY=p.jitterY;
    } else if(p.temporal) {
        runtime_log::Trace("AA temporal resolve canceled frame=%d algorithm=%u outline=%u samples=%u hr=0x%08lX history=%u motion_surface=%u mask=%u/%u",
            p.app->CurrentFrame(),unsigned(t.algorithm),unsigned(p.app->antialiasing.outlineMode),t.samples,hr,
            historyWritten?1u:0u,motionSurface?1u:0u,t.outlineMaskValid?1u:0u,t.outlineMaskWritten?1u:0u);
        t.samples=0;t.temporalUnavailable=true;motion::Cancel(p.app);
    } else if(SUCCEEDED(hr) && !Temporal(t.algorithm)) {
        t.samples=1;
    }
    saved->Apply();
    // State blocks exclude the target/depth bindings and may restore jitter.
    const HRESULT restoredTargets=extraTargets.Restore();
    if(SUCCEEDED(hr))hr=restoredTargets;
    device->SetTransform(D3DTS_PROJECTION,&p.projection);ApplyRenderState(p.app);
    if(FAILED(hr))runtime_log::Write("AA resolve failed hr=0x%08lX",hr);
    p.completed=SUCCEEDED(hr);
    return SUCCEEDED(hr);
}
}
