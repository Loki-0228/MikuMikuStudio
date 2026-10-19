#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"
#if defined(MMD_ENABLE_DX12) && MMD_ENABLE_DX12
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/gpu_model.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/model_alpha_pass.hpp"
#include "mikumikustudio/processor_settings.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/graphics_runtime.hpp"
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <numeric>
#include "d3d12_native_shaders.hpp"

namespace mikumikustudio {
namespace {
using Microsoft::WRL::ComPtr;
void Check12(HRESULT hr,const char* name) {
    if(FAILED(hr)) {char text[180];sprintf_s(text,"%s: 0x%08lx",name,hr);throw std::runtime_error(text);}
}
D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER result{};result.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    result.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};return result;
}
ComPtr<ID3DBlob> Compile(const char* source,const char* entry,const char* target) {
    ComPtr<ID3DBlob> code,error;
    HRESULT hr=graphics_runtime::Compile(source,strlen(source),"native",nullptr,nullptr,entry,target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&error);
    if(FAILED(hr)&&error)throw std::runtime_error(static_cast<const char*>(error->GetBufferPointer()));
    Check12(hr,"native shader compile");return code;
}
LRESULT CALLBACK NativeWindow(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_NCHITTEST)return HTTRANSPARENT;
    if(message==WM_ERASEBKGND)return 1;
    return DefWindowProcW(hwnd,message,w,l);
}
struct Native {
    gpu::Skinning skin;
    HWND window=nullptr;
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12DescriptorHeap> rtvs,dsvs,srvs;
    ComPtr<ID3D12RootSignature> drawRoot,sortRoot;
    std::array<ComPtr<ID3D12PipelineState>,12> drawPso;
    ComPtr<ID3D12PipelineState> sortPso;
    ComPtr<ID3D12Fence> fence;
    HANDLE event=nullptr;UINT64 sequence=0;
    unsigned width=0,height=0,rtvStep=0,srvStep=0;
    struct Frame {
        ComPtr<ID3D12Resource> color,depth,sourceIndices,indices,keys,constants,capture;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        UINT64 done=0;bool indicesReady=false;
        std::vector<ComPtr<ID3D12Resource>> uploads;
    };
    std::array<Frame,3> frames;
    struct Texture {ComPtr<IDirect3DTexture9> source;ComPtr<ID3D12Resource> gpu;};
    std::map<IDirect3DTexture9*,Texture> textures;
    struct Draw {
        unsigned first,count,textureSlot;bool alpha,edge,twoSided,additive;
        float diffuse[4],ambient[4],specular[4];unsigned sphereMode;
    };
    GpuModelInput packed,one;
    std::vector<unsigned> indices;
    std::vector<Draw> draws;
    std::vector<unsigned char*> modelIdentity;
    std::vector<std::uint64_t> geometryStamp;
    std::uint64_t geometryRevision=0;
    unsigned lastSlot=0;
    gpu::Ticket lastTicket{};
    float lastDepth[4]{};
    bool captureEnabled=false;
    struct Constants {
        float wvp[16],wv[16],light[4],lightColor[4],diffuse[4],ambient[4],specular[4],camera[4],flags[4];
    };
    ~Native() {
        try {WaitAll();}catch(...){}
        if(window)DestroyWindow(window);
        if(event)CloseHandle(event);
    }
    ID3D12Device* Device(){return skin.Device();}
    void Wait(UINT64 value) {
        if(!value)return;
        if(fence->GetCompletedValue()==UINT64_MAX)Check12(Device()->GetDeviceRemovedReason(),"native device removed");
        if(fence->GetCompletedValue()>=value)return;
        Check12(fence->SetEventOnCompletion(value,event),"native fence event");
        if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)throw std::runtime_error("native graphics timeout");
    }
    void WaitAll(){for(auto& f:frames)Wait(f.done);}
    bool Buffer(ComPtr<ID3D12Resource>& r,UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,
                D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
        if(r&&r->GetDesc().Width>=bytes)return false;
        r.Reset();D3D12_HEAP_PROPERTIES h{};h.Type=heap;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=(bytes+65535)&~UINT64(65535);
        d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;
        Check12(Device()->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"native buffer");return true;
    }
    void Upload(ComPtr<ID3D12Resource>& r,const void* data,size_t bytes) {
        Buffer(r,bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped;D3D12_RANGE range{0,0};Check12(r->Map(0,&range,&mapped),"native upload map");
        memcpy(mapped,data,bytes);D3D12_RANGE written{0,bytes};r->Unmap(0,&written);
    }
    void Root(D3D12_ROOT_PARAMETER* parameters,UINT count,ComPtr<ID3D12RootSignature>& root,
              D3D12_STATIC_SAMPLER_DESC* samplers=nullptr,UINT samplerCount=0) {
        D3D12_ROOT_SIGNATURE_DESC d{};d.NumParameters=count;d.pParameters=parameters;
        d.NumStaticSamplers=samplerCount;d.pStaticSamplers=samplers;
        ComPtr<ID3DBlob> code,error;Check12(graphics_runtime::SerializeRootSignature(&d,D3D_ROOT_SIGNATURE_VERSION_1,&code,&error),"native root serialize");
        Check12(Device()->CreateRootSignature(0,code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&root)),"native root");
    }
    void Initialize(HWND parent,bool validation) {
        captureEnabled=validation;
        // The preview runs on this run's actual main-render adapter (the
        // frozen processor policy).  Without a valid hardware LUID - the main
        // device failed, runs on the software rasterizer, or has no verified
        // LUID - the preview must NOT fall back to the compute default: that
        // would silently pick another GPU.  Throw the explicit reason and let
        // RenderD3D12Native keep the existing D3D9 fallback.
        const processor::ResolvedInfo& renderer=processor::ActualRenderer();
        if(renderer.failed)
            throw std::runtime_error("native preview unavailable: the main render device failed to be created");
        if(renderer.softwareDevice)
            throw std::runtime_error("native preview unavailable: the main render device is the software rasterizer");
        if(renderer.adapterId==0)
            throw std::runtime_error("native preview unavailable: the main render device has no verified LUID");
        if(!skin.Initialize(validation,renderer.adapterId))throw std::runtime_error(skin.Error());
        Check12(Device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"native fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("native event");
        WNDCLASSW wc{};wc.lpfnWndProc=NativeWindow;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"MmdNativeD3D12Viewport";
        RegisterClassW(&wc);
        window=CreateWindowExW(WS_EX_NOACTIVATE,wc.lpszClassName,L"D3D12",WS_CHILD,0,0,1,1,parent,nullptr,wc.hInstance,nullptr);
        if(!window)throw std::runtime_error("native viewport window");
        ComPtr<IDXGIFactory4> factory;Check12(graphics_runtime::CreateFactory2(0,IID_PPV_ARGS(&factory)),"native factory");
        DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=1;sd.Height=1;sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=3;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> first;Check12(factory->CreateSwapChainForHwnd(skin.Queue(),window,&sd,nullptr,nullptr,&first),"native swapchain");
        Check12(first.As(&swap),"native swapchain3");factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=3;
        Check12(Device()->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs)),"native RTV heap");
        hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;Check12(Device()->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsvs)),"native DSV heap");
        hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=30000*3;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check12(Device()->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srvs)),"native SRV heap");
        rtvStep=Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srvStep=Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_ROOT_PARAMETER rp[3]{};rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,1,0,0};
        rp[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[2].DescriptorTable={1,&range};
        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        for(unsigned i=0;i<2;++i){auto& s=samplers[i];s.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            s.AddressU=s.AddressV=s.AddressW=i?D3D12_TEXTURE_ADDRESS_MODE_CLAMP:D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            s.MaxLOD=D3D12_FLOAT32_MAX;s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;s.MaxAnisotropy=1;s.ShaderRegister=i;}
        Root(rp,3,drawRoot,samplers,2);
        auto vs=Compile(kNativeShader,"vs","vs_5_1"),ps=Compile(kNativeShader,"ps","ps_5_1");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=drawRoot.Get();
        p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        p.SampleMask=UINT_MAX;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets=1;p.RTVFormats[0]=DXGI_FORMAT_B8G8R8A8_UNORM;p.DSVFormat=DXGI_FORMAT_D32_FLOAT;p.SampleDesc.Count=1;
        p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.DepthClipEnable=TRUE;
        // The D3D9 model path culls counter-clockwise faces; keep clockwise
        // fronts for model bodies, and cull those fronts for expanded outlines.
        p.RasterizerState.FrontCounterClockwise=FALSE;
        p.DepthStencilState.DepthEnable=TRUE;p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
        p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
        p.DepthStencilState.StencilReadMask=p.DepthStencilState.StencilWriteMask=255;
        p.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
        p.DepthStencilState.BackFace=p.DepthStencilState.FrontFace;
        auto& blend=p.BlendState.RenderTarget[0];blend.BlendEnable=TRUE;blend.SrcBlend=D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOp=D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        for(unsigned i=0;i<12;++i){p.RasterizerState.CullMode=(i%3)==0?D3D12_CULL_MODE_BACK:(i%3)==1?D3D12_CULL_MODE_NONE:D3D12_CULL_MODE_FRONT;
            p.RasterizerState.FillMode=i>=6?D3D12_FILL_MODE_WIREFRAME:D3D12_FILL_MODE_SOLID;
            blend.DestBlend=(i%6)>=3?D3D12_BLEND_ONE:D3D12_BLEND_INV_SRC_ALPHA;
            Check12(Device()->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&drawPso[i])),"native graphics PSO");}
        D3D12_ROOT_PARAMETER sp[5]{};
        for(unsigned i=0;i<2;++i){sp[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;sp[i].Descriptor.ShaderRegister=i;
            sp[i+2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;sp[i+2].Descriptor.ShaderRegister=i;}
        sp[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;sp[4].Constants.Num32BitValues=12;
        Root(sp,5,sortRoot);auto cs=Compile(kSortShader,"main","cs_5_1");
        D3D12_COMPUTE_PIPELINE_STATE_DESC cp{};cp.pRootSignature=sortRoot.Get();cp.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
        Check12(Device()->CreateComputePipelineState(&cp,IID_PPV_ARGS(&sortPso)),"native sort PSO");
        for(auto& f:frames){Check12(Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f.allocator)),"native allocator");
            Check12(Device()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f.allocator.Get(),nullptr,IID_PPV_ARGS(&f.list)),"native commands");
            Check12(f.list->Close(),"native initial close");}
    }
    void Resize(MMDApp* app) {
        const RECT rect=app->ViewportRect();
        const auto size=viewport_render_scale::PreviewSize(app,app->viewportRender.automatic,app->viewportRender.percent);
        const unsigned w=std::max(1u,size.width),h=std::max(1u,size.height);
        SetWindowPos(window,HWND_TOP,rect.left,rect.top,std::max<LONG>(1,rect.right-rect.left),std::max<LONG>(1,rect.bottom-rect.top),SWP_NOACTIVATE|SWP_SHOWWINDOW);
        if(w==width&&h==height)return;WaitAll();
        for(auto& f:frames){f.color.Reset();f.depth.Reset();}
        Check12(swap->ResizeBuffers(3,w,h,DXGI_FORMAT_B8G8R8A8_UNORM,0),"native resize");width=w;height=h;
        for(unsigned i=0;i<3;++i){auto& f=frames[i];Check12(swap->GetBuffer(i,IID_PPV_ARGS(&f.color)),"native backbuffer");
            auto rtv=rtvs->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=i*rtvStep;Device()->CreateRenderTargetView(f.color.Get(),nullptr,rtv);
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
            d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=1;d.MipLevels=1;
            d.Format=DXGI_FORMAT_D32_FLOAT;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            D3D12_CLEAR_VALUE clear{};clear.Format=d.Format;clear.DepthStencil.Depth=1;
            Check12(Device()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,IID_PPV_ARGS(&f.depth)),"native depth");
            auto dsv=dsvs->GetCPUDescriptorHandleForHeapStart();dsv.ptr+=i*Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            Device()->CreateDepthStencilView(f.depth.Get(),nullptr,dsv);}
    }
    IDirect3DTexture9* FindTexture(MMDApp* app,const wchar_t* path,bool* alpha=nullptr) {
        if(!path||!path[0])return nullptr;
        for(auto& entry:app->Renderer()->resourcePool){auto* name=static_cast<const wchar_t*>(entry.heapBuffer);if(!name)break;
            if(wcscmp(name,path)==0){if(alpha){auto tag=reinterpret_cast<const unsigned char*>(&entry.tag)[3];*alpha=(tag&kTextureAlphaKnown)==0||(tag&kTextureUsesAlpha)!=0;}
                return reinterpret_cast<IDirect3DTexture9*>(entry.comObject);}}
        return nullptr;
    }
    ID3D12Resource* TextureResource(IDirect3DTexture9* source,Frame& frame) {
        auto it=textures.find(source);if(it!=textures.end())return it->second.gpu.Get();
        D3DSURFACE_DESC old{};if(source)Check12(source->GetLevelDesc(0,&old),"native source texture");
        else {old.Width=old.Height=1;old.Format=D3DFMT_A8R8G8B8;}
        DXGI_FORMAT format;unsigned bytes=4,block=1;
        switch(old.Format){case D3DFMT_A8R8G8B8:format=DXGI_FORMAT_B8G8R8A8_UNORM;break;
        case D3DFMT_X8R8G8B8:format=DXGI_FORMAT_B8G8R8X8_UNORM;break;
        case D3DFMT_DXT1:format=DXGI_FORMAT_BC1_UNORM;bytes=8;block=4;break;
        case D3DFMT_DXT3:format=DXGI_FORMAT_BC2_UNORM;bytes=16;block=4;break;
        case D3DFMT_DXT5:format=DXGI_FORMAT_BC3_UNORM;bytes=16;block=4;break;
        case D3DFMT_A16B16G16R16F:format=DXGI_FORMAT_R16G16B16A16_FLOAT;bytes=8;break;
        case D3DFMT_A32B32G32R32F:format=DXGI_FORMAT_R32G32B32A32_FLOAT;bytes=16;break;
        default:throw std::runtime_error("Native texture format is not yet supported");}
        Texture t;t.source=source;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=old.Width;d.Height=old.Height;
        d.DepthOrArraySize=1;d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;
        Check12(Device()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&t.gpu)),"native texture");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows;UINT64 rowBytes,total;
        Device()->GetCopyableFootprints(&d,0,1,0,&footprint,&rows,&rowBytes,&total);
        ComPtr<ID3D12Resource> upload;Buffer(upload,total,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        D3DLOCKED_RECT locked{};DWORD white=0xffffffff;
        if(source)Check12(source->LockRect(0,&locked,nullptr,D3DLOCK_READONLY),"native texture read");
        else {locked.pBits=&white;locked.Pitch=4;}
        void* mapped=nullptr;D3D12_RANGE noRead{0,0};HRESULT mapResult=upload->Map(0,&noRead,&mapped);
        if(FAILED(mapResult)){if(source)source->UnlockRect(0);Check12(mapResult,"native texture upload");}
        for(unsigned y=0;y<rows;++y)memcpy(static_cast<char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch,
            static_cast<const char*>(locked.pBits)+y*locked.Pitch,static_cast<size_t>(rowBytes));
        upload->Unmap(0,nullptr);if(source)source->UnlockRect(0);
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=t.gpu.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
        frame.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        auto barrier=Barrier(t.gpu.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);frame.list->ResourceBarrier(1,&barrier);
        frame.uploads.push_back(upload);return textures.emplace(source,std::move(t)).first->second.gpu.Get();
    }
    void Descriptor(IDirect3DTexture9* texture,Frame& f,unsigned slot) {
        auto* r=TextureResource(texture,f);D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=r->GetDesc().Format;
        d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;
        auto handle=srvs->GetCPUDescriptorHandleForHeapStart();handle.ptr+=size_t(slot)*srvStep;Device()->CreateShaderResourceView(r,&d,handle);
    }
    void Gather(MMDApp* app,const float world[16],Frame& f,unsigned frameSlot) {
        packed.bones.clear();packed.materials.clear();indices.clear();draws.clear();
        packed.parameters={0,false,false,true};
        std::vector<unsigned char*> identity;
        std::vector<std::uint64_t> stamp;
        for(int i=0;i<kModelSlotCount;++i)if(auto* model=app->ModelSlot(i)){
            identity.push_back(model);const auto& m=*mdl::Mdl(model);
            stamp.insert(stamp.end(),{std::uint64_t(reinterpret_cast<uintptr_t>(model)),
                std::uint64_t(reinterpret_cast<uintptr_t>(m.pmxVertices)),
                std::uint64_t(reinterpret_cast<uintptr_t>(m.rawVertices)),
                m.vertexCount,m.boneCount,m.materialCount,std::uint64_t(m.comboSelIndex),m.loadComplete});
            for(std::uint32_t j=0;j<m.morphCount;++j){std::uint32_t bits;memcpy(&bits,&mdl::Morphs(model)[j].value,4);stamp.push_back(bits);}
        }
        const bool geometry=stamp!=geometryStamp||!geometryRevision;
        if(geometry){geometryStamp=std::move(stamp);++geometryRevision;packed.vertices.clear();}
        packed.parameters.geometryRevision=geometryRevision;
        if(identity!=modelIdentity){WaitAll();textures.clear();modelIdentity=std::move(identity);}
        unsigned vertexBase=0;
        for(int order=0;order<kModelSlotCount;++order)for(int slot=0;slot<kModelSlotCount;++slot){
            auto* model=app->ModelSlot(slot);if(!model||mdl::Mdl(model)->comboSelIndex!=order)continue;
            auto& m=*mdl::Mdl(model);if(!m.loadComplete)break;
            if(!PrepareGpuModelInput(app,model,world,one,geometry))throw std::runtime_error("Native model has an unsupported skinning type");
            const unsigned base=vertexBase,boneBase=unsigned(packed.bones.size()),materialBase=unsigned(packed.materials.size());
            vertexBase+=m.vertexCount;
            if(geometry)for(auto v:one.vertices){for(auto& b:v.bones)b=b>=0&&unsigned(b)<one.bones.size()?b+boneBase:-1;v.material+=materialBase;packed.vertices.push_back(v);}
            packed.bones.insert(packed.bones.end(),one.bones.begin(),one.bones.end());packed.materials.insert(packed.materials.end(),one.materials.begin(),one.materials.end());
            unsigned first=unsigned(indices.size());const bool pmx=m.physicsMode==2;
            for(unsigned i=0;i<m.indexCount;++i){unsigned index=pmx?mdl::PmxIndices(model)[i]:mdl::Indices(model)[i*2];
                if(index>=m.vertexCount)throw std::runtime_error("Native invalid model index");indices.push_back(base+index);}
            unsigned consumed=0;
            for(unsigned i=0;i<m.materialCount;++i){const auto& mat=mdl::Materials(model)[i];const unsigned count=unsigned(mat.faceVertexCount);
                if(count>m.indexCount-consumed||count%3)throw std::runtime_error("Native invalid material index span");
                if(draws.size()>=10000)throw std::runtime_error("Native material capacity exceeded");
                Draw draw{};draw.first=first+consumed;draw.count=count;draw.textureSlot=frameSlot*30000+unsigned(draws.size())*3;
                draw.alpha=mat.diffuse[3]<1||m.displayState!=0||m.postLoadFlag2!=0;bool alpha=false;
                auto* tex=FindTexture(app,mat.texturePath,&alpha);draw.alpha|=alpha;alpha=false;
                auto* sphere=FindTexture(app,mat.spherePath,&alpha);
                draw.sphereMode=pmx?mat.sphereMode:(!sphere?0:wcsstr(mat.spherePath,L".spa")?2:1);
                if(!sphere)draw.sphereMode=0;
                if(draw.sphereMode)draw.alpha|=alpha;
                IDirect3DTexture9* toon=app->ToonTexture(0);
                if(mat.toonPath[0])toon=FindTexture(app,mat.toonPath);
                else if(mat.toonReference<10)toon=app->ToonTexture(mat.toonReference+1);
                Descriptor(tex,f,draw.textureSlot);Descriptor(sphere,f,draw.textureSlot+1);Descriptor(toon,f,draw.textureSlot+2);
                memcpy(draw.diffuse,mat.diffuse,16);memcpy(draw.ambient,mat.ambient,12);memcpy(draw.specular,mat.specular,12);draw.specular[3]=mat.specularPower;
                if(m.displayState)draw.diffuse[3]=.5f;
                draw.edge=mat.doubleSided!=0&&m.displayState==0;draw.twoSided=(!pmx&&draw.diffuse[3]<1)||(pmx&&(mat.flags&1));draw.additive=m.postLoadFlag2!=0;
                draws.push_back(draw);consumed+=count;}
            break;
        }
    }
    void Render(MMDApp* app,const float world[16]) {
        Resize(app);unsigned slot=swap->GetCurrentBackBufferIndex();auto& f=frames[slot];Wait(f.done);f.uploads.clear();
        Check12(f.allocator->Reset(),"native allocator reset");Check12(f.list->Reset(f.allocator.Get(),nullptr),"native list reset");
        Gather(app,world,f,slot);
        D3DMATRIX view{},projection{},wv{},wvp{};app->Renderer()->device->GetTransform(D3DTS_VIEW,&view);app->Renderer()->device->GetTransform(D3DTS_PROJECTION,&projection);
        auto& api=d3dx::Get();api.Load();api.multiply(reinterpret_cast<d3dx::D3DXMATRIXF*>(&wv),reinterpret_cast<const d3dx::D3DXMATRIXF*>(world),reinterpret_cast<const d3dx::D3DXMATRIXF*>(&view));
        api.multiply(reinterpret_cast<d3dx::D3DXMATRIXF*>(&wvp),reinterpret_cast<const d3dx::D3DXMATRIXF*>(&wv),reinterpret_cast<const d3dx::D3DXMATRIXF*>(&projection));
        gpu::Ticket ticket;const bool geometry=!packed.vertices.empty()&&!indices.empty();
        if(geometry){if(!skin.Submit(packed.vertices,packed.bones,packed.materials,packed.parameters,ticket))throw std::runtime_error(skin.Error());
            Upload(f.sourceIndices,indices.data(),indices.size()*4);
            if(Buffer(f.indices,indices.size()*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))f.indicesReady=false;
            if(f.indicesReady){auto b=Barrier(f.indices.Get(),D3D12_RESOURCE_STATE_INDEX_BUFFER,D3D12_RESOURCE_STATE_COPY_DEST);f.list->ResourceBarrier(1,&b);}
            f.list->CopyBufferRegion(f.indices.Get(),0,f.sourceIndices.Get(),0,indices.size()*4);
            auto b=Barrier(f.indices.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);f.list->ResourceBarrier(1,&b);
            unsigned maxPadded=1;for(auto& d:draws)if(d.alpha){unsigned n=1;while(n<d.count/3)n*=2;maxPadded=std::max(maxPadded,n);}
            Buffer(f.keys,size_t(maxPadded)*8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            f.list->SetPipelineState(sortPso.Get());f.list->SetComputeRootSignature(sortRoot.Get());
            f.list->SetComputeRootShaderResourceView(0,skin.OutputResource(ticket)->GetGPUVirtualAddress());f.list->SetComputeRootShaderResourceView(1,f.sourceIndices->GetGPUVirtualAddress());
            f.list->SetComputeRootUnorderedAccessView(2,f.keys->GetGPUVirtualAddress());f.list->SetComputeRootUnorderedAccessView(3,f.indices->GetGPUVirtualAddress());
            D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
            for(auto& d:draws)if(d.alpha&&d.count){struct Sort {unsigned first,count,padded,mode,k,j,r0,r1;float depth[4];} s{d.first,d.count/3,1,0,0,0,0,0,{wv._13,wv._23,wv._33,wv._43}};
                while(s.padded<s.count)s.padded*=2;
                if(s.padded>65535*64u)throw std::runtime_error("Native transparent material too large");
                auto dispatch=[&]{f.list->SetComputeRoot32BitConstants(4,12,&s,0);f.list->Dispatch((s.padded+63)/64,1,1);f.list->ResourceBarrier(1,&uav);};
                dispatch();s.mode=1;for(s.k=2;s.k<=s.padded;s.k*=2)for(s.j=s.k/2;s.j;s.j/=2)dispatch();s.mode=2;dispatch();}
            b=Barrier(f.indices.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_INDEX_BUFFER);f.list->ResourceBarrier(1,&b);f.indicesReady=true;
        }
        auto b=Barrier(f.color.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);f.list->ResourceBarrier(1,&b);
        auto rtv=rtvs->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=slot*rtvStep;
        auto dsv=dsvs->GetCPUDescriptorHandleForHeapStart();dsv.ptr+=slot*Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        float c=app->state.blackBackgroundEnabled?0.f:1.f;float clear[4]={c,c,c,1};f.list->ClearRenderTargetView(rtv,clear,0,nullptr);
        f.list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);f.list->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
        D3D12_VIEWPORT vp{0,0,float(width),float(height),0,1};D3D12_RECT rect{0,0,LONG(width),LONG(height)};f.list->RSSetViewports(1,&vp);f.list->RSSetScissorRects(1,&rect);
        if(geometry){std::vector<unsigned char> constants(draws.size()*2*256);Constants value{};memcpy(value.wvp,&wvp,64);memcpy(value.wv,&wv,64);
            memcpy(value.light,app->LightDirection(),12);memcpy(value.lightColor,&app->SceneLight().Ambient,12);
            for(unsigned i=0;i<draws.size();++i){auto& d=draws[i];memcpy(value.diffuse,d.diffuse,16);memcpy(value.ambient,d.ambient,16);memcpy(value.specular,d.specular,16);
                value.flags[0]=0;value.flags[1]=float(d.sphereMode);memcpy(constants.data()+i*512,&value,sizeof value);value.flags[0]=1;memcpy(constants.data()+i*512+256,&value,sizeof value);}
            Upload(f.constants,constants.data(),constants.size());ID3D12DescriptorHeap* heaps[]={srvs.Get()};f.list->SetDescriptorHeaps(1,heaps);
            f.list->SetGraphicsRootSignature(drawRoot.Get());f.list->SetGraphicsRootShaderResourceView(0,skin.OutputResource(ticket)->GetGPUVirtualAddress());
            D3D12_INDEX_BUFFER_VIEW ib{f.indices->GetGPUVirtualAddress(),UINT(indices.size()*4),DXGI_FORMAT_R32_UINT};f.list->IASetIndexBuffer(&ib);f.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for(unsigned pass=0;pass<3;++pass)for(unsigned i=0;i<draws.size();++i){auto& d=draws[i];if(!d.count||d.diffuse[3]<=0)continue;
                if(pass<2&&d.alpha!=(pass==1))continue;if(pass==2&&(!d.edge||app->WireframeRenderingEnabled()))continue;
                unsigned pso=pass==2?2:(d.twoSided?1:0)+(d.additive?3:0);
                if(app->WireframeRenderingEnabled())pso+=6;
                f.list->SetPipelineState(drawPso[pso].Get());
                f.list->SetGraphicsRootConstantBufferView(1,f.constants->GetGPUVirtualAddress()+i*512+(pass==2?256:0));
                auto srv=srvs->GetGPUDescriptorHandleForHeapStart();srv.ptr+=size_t(d.textureSlot)*srvStep;f.list->SetGraphicsRootDescriptorTable(2,srv);
                f.list->DrawIndexedInstanced(d.count,1,d.first,0,0);}
        }
        if(captureEnabled){
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows;UINT64 rowBytes,total;
            auto desc=f.color->GetDesc();Device()->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowBytes,&total);
            Buffer(f.capture,total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            b=Barrier(f.color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);f.list->ResourceBarrier(1,&b);
            D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=f.color.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=f.capture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
            f.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            b=Barrier(f.color.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
        }else b=Barrier(f.color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
        f.list->ResourceBarrier(1,&b);
        Check12(f.list->Close(),"native close");ID3D12CommandList* lists[]={f.list.Get()};skin.Queue()->ExecuteCommandLists(1,lists);
        Check12(skin.Queue()->Signal(fence.Get(),++sequence),"native signal");f.done=sequence;
        if(geometry&&!skin.RetainForQueuedGraphics(ticket))throw std::runtime_error(skin.Error());
        lastSlot=slot;lastTicket=ticket;
        lastDepth[0]=wv._13;lastDepth[1]=wv._23;lastDepth[2]=wv._33;lastDepth[3]=wv._43;
        Check12(swap->Present(0,0),"native present");
    }
    void Validate(const wchar_t* path) {
        WaitAll();auto& f=frames[lastSlot];
        std::vector<gpu::Output> vertices;
        if(lastTicket.fence&&!skin.Readback(lastTicket,vertices))throw std::runtime_error(skin.Error());
        Check12(f.allocator->Reset(),"validation allocator");Check12(f.list->Reset(f.allocator.Get(),nullptr),"validation list");
        ComPtr<ID3D12Resource> readIndices;
        if(!indices.empty()){
            Buffer(readIndices,indices.size()*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            auto b=Barrier(f.indices.Get(),D3D12_RESOURCE_STATE_INDEX_BUFFER,D3D12_RESOURCE_STATE_COPY_SOURCE);f.list->ResourceBarrier(1,&b);
            f.list->CopyBufferRegion(readIndices.Get(),0,f.indices.Get(),0,indices.size()*4);
            b=Barrier(f.indices.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_INDEX_BUFFER);f.list->ResourceBarrier(1,&b);
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows;UINT64 rowBytes,total;
        auto desc=f.color->GetDesc();Device()->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowBytes,&total);
        Check12(f.list->Close(),"validation close");ID3D12CommandList* lists[]={f.list.Get()};skin.Queue()->ExecuteCommandLists(1,lists);
        Check12(skin.Queue()->Signal(fence.Get(),++sequence),"validation signal");f.done=sequence;WaitAll();
        if(readIndices){
            void* mapped;D3D12_RANGE range{0,indices.size()*4};Check12(readIndices->Map(0,&range,&mapped),"validation index map");
            std::vector<unsigned> actual(indices.size());memcpy(actual.data(),mapped,indices.size()*4);readIndices->Unmap(0,nullptr);
            for(const auto& d:draws){
                std::vector<unsigned> order(d.count/3);std::iota(order.begin(),order.end(),0);
                std::vector<float> depths(order.size());
                if(d.alpha){for(unsigned i=0;i<order.size();++i)for(unsigned n=0;n<3;++n){const auto& p=vertices[indices[d.first+3*i+n]].position;
                    depths[i]+=(p[0]*lastDepth[0]+p[1]*lastDepth[1]+p[2]*lastDepth[2]+lastDepth[3])*(1.f/3.f);}
                    std::stable_sort(order.begin(),order.end(),[&](unsigned a,unsigned b){return depths[a]>depths[b];});}
                for(unsigned i=0;i<order.size();++i)for(unsigned n=0;n<3;++n)
                    if(actual[d.first+3*i+n]!=indices[d.first+3*order[i]+n]){
                        char error[256];sprintf_s(error,"GPU stable sort mismatch material first=%u triangle=%u component=%u actual=%u expected=%u depth=%.9g",d.first,i,n,actual[d.first+3*i+n],indices[d.first+3*order[i]+n],depths[order[i]]);
                        throw std::runtime_error(error);
                    }
            }
        }
        if(path){
            if(!f.capture)throw std::runtime_error("capture requires validation mode");
            FILE* file=nullptr;if(_wfopen_s(&file,path,L"wb")||!file)throw std::runtime_error("capture file open");
            BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+width*height*4;
            BITMAPINFOHEADER info{};info.biSize=sizeof info;info.biWidth=width;info.biHeight=-LONG(height);info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
            void* mapped;D3D12_RANGE range{0,size_t(total)};HRESULT hr=f.capture->Map(0,&range,&mapped);
            if(FAILED(hr)){fclose(file);Check12(hr,"capture map");}
            bool ok=fwrite(&header,sizeof header,1,file)==1&&fwrite(&info,sizeof info,1,file)==1;
            for(unsigned y=0;y<height;++y)ok=(fwrite(static_cast<char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch,width*4,1,file)==1)&&ok;
            f.capture->Unmap(0,nullptr);fclose(file);if(!ok)throw std::runtime_error("capture write");
        }
        ComPtr<ID3D12InfoQueue> info;
        if(SUCCEEDED(Device()->QueryInterface(IID_PPV_ARGS(&info))))
            for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<char> bytes(size);
                auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());Check12(info->GetMessage(i,message,&size),"debug message");
                if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)throw std::runtime_error(message->pDescription);}
    }
};
bool enabled=false;
bool validationEnabled=false;
std::unique_ptr<Native> native;
std::string status="disabled";
}
void EnableD3D12Native(bool value,bool validation){enabled=value;validationEnabled=validation;if(!value)ShutdownD3D12Native();status=value?"requested":"disabled";}
bool ValidateD3D12Native(const wchar_t* path){
    if(!native)return false;
    try{native->Validate(path);return true;}catch(const std::exception& e){status=e.what();return false;}
}
const char* D3D12NativeStatus(){return status.c_str();}
bool WaitD3D12Native(){
    if(!native)return false;
    try{native->WaitAll();return true;}
    catch(const std::exception& e){status=e.what();return false;}
}
void ShutdownD3D12Native(){native.reset();}
bool RenderD3D12Native(MMDApp* app,const float world[16]) {
    if(!enabled)return false;
    try {
        if(GetModuleHandleW(L"MMEffect.dll"))throw std::runtime_error("D3D12 native requires a directory without the MME proxy");
        if(app->RecordingWindow()||app->FloatingWindow()||app->FullscreenMode())throw std::runtime_error("Native recording/floating/fullscreen is not available");
        for(int i=0;i<255;++i)if(app->AccessorySlot(i))throw std::runtime_error("Native accessory rendering is not yet available");
        if(!native){native=std::make_unique<Native>();native->Initialize(app->MainWindow(),validationEnabled);}
        native->Render(app,world);status="D3D12 native active";return true;
    }catch(const std::exception& e){status=e.what();runtime_log::Write("D3D12 native fallback: %s",status.c_str());native.reset();enabled=false;return false;}
}
}
#else
namespace mikumikustudio {
namespace {
const char* status = "DX12 features are disabled in this build";
}
void EnableD3D12Native(bool enabled,bool) {
    status=enabled?"DX12 features are disabled in this build":"disabled";
}
bool RenderD3D12Native(MMDApp*,const float[16]) {return false;}
void ShutdownD3D12Native() {}
bool ValidateD3D12Native(const wchar_t*) {return false;}
const char* D3D12NativeStatus() {return status;}
bool WaitD3D12Native() {return false;}
}
#endif
