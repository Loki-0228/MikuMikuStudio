#include "mikudancestudio/local_data_path.hpp"
#include "mikudancestudio/antialias.hpp"
#include "mikudancestudio/antialias_motion.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/graphics_runtime.hpp"
#include "mikudancestudio/runtime_log.hpp"
#include "mikudancestudio/build_profile.hpp"
#include "mikudancestudio/pmm_project_settings.hpp"
#include "antialias_shader.hpp"
#include "outline_coverage_shader.hpp"
#include "mikudancestudio/model.hpp"
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

namespace mikudancestudio::antialias {
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
    ComPtr<IDirect3DVertexDeclaration9> outlineDeclaration;
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
    WritePrivateProfileStringW(L"Antialias",L"Enabled",settings.enabled?L"1":L"0",path.c_str());
}
void ReadUserPreferences(Settings& settings) {
    const auto path=StorePath();
    const auto algorithm=GetPrivateProfileIntW(L"Antialias",L"Algorithm",0,path.c_str());
    settings.algorithm=algorithm<unsigned(Algorithm::Count)?Algorithm(algorithm):Algorithm::MmdOriginal;
    settings.enabled=GetPrivateProfileIntW(L"Antialias",L"Enabled",1,path.c_str())!=0;
    const auto last=Algorithm(GetPrivateProfileIntW(L"Antialias",L"LastSmaaAlgorithm",unsigned(Algorithm::Smaa),path.c_str()));
    settings.lastSmaaAlgorithm=Smaa(last)?last:Algorithm::Smaa;
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
    const char* source=index>=2?kSmaaShader:kShader;
    const auto sourceSize=index>=2?sizeof(kSmaaShader)-1:sizeof(kShader)-1;
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
    auto temporal=algorithm==Algorithm::Taa?RequestShader(1):nullptr;
    auto edges=useSmaa?RequestShader(shaderBase):nullptr;
    auto weights=useSmaa?RequestShader(shaderBase+1):nullptr;
    auto resolve=useSmaa?RequestShader(shaderBase+2):nullptr;
    auto smaaTemporal=SmaaTemporal(algorithm)?RequestShader(shaderBase+3):nullptr;
    // The same preset's unjittered neighborhood shader is the motion fallback.
    auto smaaFallback=SmaaTemporal(algorithm)?RequestShader(shaderBase-8+2):nullptr;
    for(const auto& shader:{fxaa,temporal,edges,weights,resolve,smaaTemporal,smaaFallback})if(shader) {
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
void Release(MMDApp* app){motion::Release(app);targets.erase(app);}
bool DrawOutline(MMDApp* app,unsigned char* model,unsigned firstIndex,
                 unsigned indexCount,const float* uniformColor) {
    if(build_profile::original || !app || !model || !PostProcess(app->antialiasing))return false;
    const auto found=targets.find(app);if(found==targets.end())return false;
    auto& t=found->second;auto* device=app->Renderer()->device;
    ComPtr<IDirect3DSurface9> target;
    if(FAILED(device->GetRenderTarget(0,&target)) || target.Get()!=t.color.Get())return false;
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
    const auto additional=mdl::Mdl(model)->pmxAdditionalUvCount;
    const UINT stride=sizeof(mdl::SkinnedVertexBase)+(additional>=1 && additional<=4?16*additional:0);
    HRESULT hr=device->SetStreamSource(1,body,0,stride);
    if(SUCCEEDED(hr))hr=device->SetVertexDeclaration(t.outlineDeclaration.Get());
    if(SUCCEEDED(hr))hr=device->SetVertexShader(t.outlineVertex.Get());
    if(SUCCEEDED(hr))hr=device->SetPixelShader(t.outlinePixel.Get());
    if(SUCCEEDED(hr))hr=device->SetVertexShaderConstantF(0,constants,7);
    const DWORD coverageStates[]={TRUE,D3DBLEND_SRCALPHA,D3DBLEND_INVSRCALPHA,D3DBLENDOP_ADD,
        TRUE,D3DCMP_GREATER,0};
    for(unsigned i=0;i<_countof(states);++i)if(SUCCEEDED(hr))hr=device->SetRenderState(states[i],coverageStates[i]);
    if(SUCCEEDED(hr))hr=device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,mdl::Mdl(model)->vertexCount,firstIndex,indexCount/3);
    device->SetVertexShader(oldVertex.Get());device->SetPixelShader(oldPixel.Get());
    device->SetVertexDeclaration(oldDeclaration.Get());device->SetStreamSource(1,oldStream.Get(),oldOffset,oldStride);
    device->SetVertexShaderConstantF(0,oldConstants,7);
    for(unsigned i=0;i<_countof(states);++i)device->SetRenderState(states[i],values[i]);
    return SUCCEEDED(hr);
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
    ~Impl(){if(bindingsChanged)Restore();if(temporal)motion::Cancel(app);}
};
FrameScope::FrameScope(MMDApp* app) {
    if(build_profile::original)return;
    Initialize(app);ApplyRenderState(app);
    if(!PostProcess(app->antialiasing))return;
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
    auto* device=native.Get();ComPtr<IDirect3DStateBlock9> saved;
    if(FAILED(device->CreateStateBlock(D3DSBT_ALL,&saved))){p.Restore();return false;}
    p.Restore();
    HRESULT hr=device->BeginScene();
    ComPtr<IDirect3DSurface9> motionSurface;
    bool historyWritten=false;
    if(SUCCEEDED(hr)) {
        auto* vectors=p.temporal?motion::Resolve(p.app,device):nullptr;
        FullscreenState(device);
        const auto& v=p.viewport;const auto& desc=t.description;
        const float constants[]={1.f/desc.Width,1.f/desc.Height,vectors?float(std::min(t.samples,31u))/(std::min(t.samples,31u)+1):0,0,
            (v.X+.5f)/desc.Width,(v.Y+.5f)/desc.Height,(v.X+v.Width-.5f)/desc.Width,(v.Y+v.Height-.5f)/desc.Height,
            p.jitterX,p.jitterY,.002f,.002f,
            t.historyJitterX,t.historyJitterY,0,0};
        device->SetPixelShaderConstantF(0,constants,4);
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
            if(SUCCEEDED(hr))hr=Draw(device,t.scene.Get(),t.smaa[0].Get(),v,desc.Width,desc.Height);
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
            if(SUCCEEDED(hr))hr=Draw(device,t.scene.Get(),neighborhood,v,desc.Width,desc.Height);
            for(unsigned sampler=0;sampler<3;++sampler)device->SetTexture(sampler,nullptr);
            if(vectors && SUCCEEDED(hr)) {
                device->SetRenderTarget(0,p.previous.Get());
                device->SetTexture(1,t.history[t.historyIndex].Get());device->SetTexture(2,vectors);
                device->SetTexture(3,t.historyDepth.Get());device->SetTexture(4,t.scene.Get());
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
                Draw(device,t.scene.Get(),nullptr,v,desc.Width,desc.Height);
                p.app->antialiasing.enabled=false;RefreshMenu(p.app);
            }
        } else if(vectors) {
            const unsigned next=1-t.historyIndex;
            device->SetDepthStencilSurface(nullptr);device->SetTexture(1,t.history[t.historyIndex].Get());
            device->SetTexture(2,vectors);device->SetTexture(3,t.historyDepth.Get());
            hr=device->SetRenderTarget(0,t.historySurface[next].Get());
            if(SUCCEEDED(hr))hr=Draw(device,t.scene.Get(),t.temporal.Get(),v,desc.Width,desc.Height);
            device->SetTexture(1,nullptr);device->SetTexture(2,nullptr);device->SetTexture(3,nullptr);
            device->SetRenderTarget(0,p.previous.Get());
            if(SUCCEEDED(hr))hr=Draw(device,t.history[next].Get(),nullptr,v,desc.Width,desc.Height);
            if(SUCCEEDED(hr) && SUCCEEDED(vectors->GetSurfaceLevel(0,&motionSurface))) {
                t.historyIndex=next;historyWritten=true;
            }
        } else {
            t.samples=0;motion::Cancel(p.app);
            device->SetDepthStencilSurface(nullptr);device->SetRenderTarget(0,p.previous.Get());
            hr=Draw(device,t.scene.Get(),t.fxaa.Get(),v,desc.Width,desc.Height);
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
        t.samples=0;t.temporalUnavailable=true;motion::Cancel(p.app);
    } else if(SUCCEEDED(hr) && !Temporal(t.algorithm)) {
        t.samples=1;
    }
    saved->Apply();
    // State blocks exclude the target/depth bindings and may restore jitter.
    device->SetDepthStencilSurface(nullptr);device->SetRenderTarget(0,p.previous.Get());device->SetDepthStencilSurface(p.depth.Get());
    device->SetViewport(&p.viewport);device->SetTransform(D3DTS_PROJECTION,&p.projection);ApplyRenderState(p.app);
    if(FAILED(hr))runtime_log::Write("AA resolve failed hr=0x%08lX",hr);
    p.completed=SUCCEEDED(hr);
    return SUCCEEDED(hr);
}
}
