#include "mikumikustudio/d3d12_cloth.hpp"
#if defined(MMD_ENABLE_DX12) && MMD_ENABLE_DX12
#define NOMINMAX
#include "mikumikustudio/d3d12_skinning.hpp"
#include "mikumikustudio/graphics_runtime.hpp"
#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace mikumikustudio::gpu {
using Microsoft::WRL::ComPtr;
namespace {
const char shaderSource[] =
#include "d3d12_cloth_shader.inc"
;
void Check(HRESULT result,const char* where) {
    if(FAILED(result)) throw std::runtime_error(std::string(where)+": "+std::to_string(unsigned(result)));
}
D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};return b;
}
}
struct Cloth::Impl {
    Skinning context;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    std::array<ComPtr<ID3D12Resource>,5> uploads;
    std::array<ComPtr<ID3D12Resource>,3> inputs;
    bool inputsReady[3]{};
    ComPtr<ID3D12Resource> nodes,rigids,readback;
    HANDLE event=nullptr;
    std::uint64_t serial=0;
    bool nodesReady=false,rigidsReady=false;
    std::string error;
    ~Impl() {try{Wait();}catch(...){}if(event)CloseHandle(event);}
    void Wait() {
        if(!serial || !fence) return;
        const auto completed=fence->GetCompletedValue();
        if(completed==UINT64_MAX) throw std::runtime_error("cloth device removed");
        if(completed>=serial) return;
        Check(fence->SetEventOnCompletion(serial,event),"cloth fence event");
        if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0) throw std::runtime_error("cloth GPU timeout");
    }
    bool Buffer(ComPtr<ID3D12Resource>& resource,std::size_t size,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,bool uav=false) {
        if(resource && resource->GetDesc().Width>=size) return false;
        resource.Reset();
        D3D12_HEAP_PROPERTIES properties{};properties.Type=heap;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width=(size+65535)&~std::uint64_t(65535);desc.Height=1;desc.DepthOrArraySize=1;
        desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
        Check(context.Device()->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)),"cloth buffer");
        return true;
    }
    template<class T> void Upload(unsigned slot,const std::vector<T>& values) {
        const auto size=values.size()*sizeof(T);
        Buffer(uploads[slot],size?size:sizeof(T),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* data=nullptr;D3D12_RANGE noRead{0,0};
        Check(uploads[slot]->Map(0,&noRead,&data),"cloth upload");
        if(size)std::memcpy(data,values.data(),size);
        D3D12_RANGE written{0,size};uploads[slot]->Unmap(0,&written);
    }
};
Cloth::Cloth():impl(std::make_unique<Impl>()){}
Cloth::~Cloth()=default;
const char* Cloth::Error() const{return impl->error.c_str();}
const wchar_t* Cloth::AdapterName() const{return impl->context.AdapterName();}
bool Cloth::Initialize(std::uint64_t adapter) {
    auto& s=*impl;
    if(s.pipeline) return true;
    try {
        if(!s.context.Initialize(false,adapter)) throw std::runtime_error(s.context.Error());
        auto* device=s.context.Device();
        D3D12_ROOT_PARAMETER parameters[6]{};
        for(unsigned i=0;i<3;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[i].Descriptor.ShaderRegister=i;}
        for(unsigned i=3;i<5;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[i].Descriptor.ShaderRegister=i-3;}
        parameters[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[5].Constants.Num32BitValues=8;
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=6;desc.pParameters=parameters;
        ComPtr<ID3DBlob> code,errors;
        Check(graphics_runtime::SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&code,&errors),"cloth root signature");
        Check(device->CreateRootSignature(0,code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&s.root)),"cloth root");
        code.Reset();errors.Reset();
        const auto result=graphics_runtime::Compile(shaderSource,sizeof(shaderSource)-1,"d3d12_cloth",nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&errors);
        if(FAILED(result)&&errors)throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        Check(result,"cloth shader");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=s.root.Get();pso.CS={code->GetBufferPointer(),code->GetBufferSize()};
        Check(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&s.pipeline)),"cloth pipeline");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator)),"cloth allocator");
        Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),s.pipeline.Get(),IID_PPV_ARGS(&s.commands)),"cloth commands");
        Check(s.commands->Close(),"cloth initial close");
        Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.fence)),"cloth fence");
        s.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!s.event)throw std::runtime_error("cloth event");
        return true;
    }catch(const std::exception& e){s.error=e.what();s.pipeline.Reset();return false;}
}
bool Cloth::Step(std::vector<ClothNode>& nodes,const std::vector<ClothLink>& links,
    std::vector<ClothRigid>& rigids,const std::vector<ClothCoupling>& couplings,
    const std::vector<ClothGroup>& groups,const std::vector<ClothPass>& passes,ClothParameters parameters) {
    auto& s=*impl;
    try {
        if(!s.pipeline || nodes.empty()) throw std::runtime_error("cloth solver not initialized");
        s.Wait();
        s.Upload(0,links);s.Upload(1,couplings);s.Upload(2,groups);s.Upload(3,nodes);s.Upload(4,rigids);
        const auto nodeBytes=nodes.size()*sizeof(ClothNode),rigidBytes=rigids.size()*sizeof(ClothRigid);
        if(s.Buffer(s.nodes,nodeBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,true))s.nodesReady=false;
        if(s.Buffer(s.rigids,rigidBytes?rigidBytes:sizeof(ClothRigid),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,true))s.rigidsReady=false;
        s.Buffer(s.readback,nodeBytes+rigidBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        Check(s.allocator->Reset(),"cloth allocator reset");Check(s.commands->Reset(s.allocator.Get(),s.pipeline.Get()),"cloth list reset");
        auto transition=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){auto barrier=Barrier(r,a,b);s.commands->ResourceBarrier(1,&barrier);};
        const std::size_t inputBytes[]={links.size()*sizeof(ClothLink),couplings.size()*sizeof(ClothCoupling),groups.size()*sizeof(ClothGroup)};
        for(unsigned i=0;i<3;++i) {
            if(s.Buffer(s.inputs[i],inputBytes[i]?inputBytes[i]:16,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST))s.inputsReady[i]=false;
            if(s.inputsReady[i])transition(s.inputs[i].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            if(inputBytes[i])s.commands->CopyBufferRegion(s.inputs[i].Get(),0,s.uploads[i].Get(),0,inputBytes[i]);
            transition(s.inputs[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            s.inputsReady[i]=true;
        }
        if(s.nodesReady)transition(s.nodes.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        if(s.rigidsReady)transition(s.rigids.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        s.commands->CopyBufferRegion(s.nodes.Get(),0,s.uploads[3].Get(),0,nodeBytes);
        if(rigidBytes)s.commands->CopyBufferRegion(s.rigids.Get(),0,s.uploads[4].Get(),0,rigidBytes);
        transition(s.nodes.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(s.rigids.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        s.commands->SetComputeRootSignature(s.root.Get());
        for(unsigned i=0;i<3;++i)s.commands->SetComputeRootShaderResourceView(i,s.inputs[i]->GetGPUVirtualAddress());
        s.commands->SetComputeRootUnorderedAccessView(3,s.nodes->GetGPUVirtualAddress());
        s.commands->SetComputeRootUnorderedAccessView(4,s.rigids->GetGPUVirtualAddress());
        struct Constants{unsigned stage,first,count,pad;ClothParameters p;} constants{0,0,0,0,parameters};
        for(const auto& pass:passes) {
            if(!pass.count)continue;
            if(pass.count>65535u*64u)throw std::runtime_error("cloth dispatch too large");
            constants.stage=pass.stage;constants.first=pass.first;constants.count=pass.count;
            s.commands->SetComputeRoot32BitConstants(5,8,&constants,0);
            s.commands->Dispatch((pass.count+63)/64,1,1);
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
            s.commands->ResourceBarrier(1,&barrier);
        }
        transition(s.nodes.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(s.rigids.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        s.commands->CopyBufferRegion(s.readback.Get(),0,s.nodes.Get(),0,nodeBytes);
        if(rigidBytes)s.commands->CopyBufferRegion(s.readback.Get(),nodeBytes,s.rigids.Get(),0,rigidBytes);
        Check(s.commands->Close(),"cloth close");ID3D12CommandList* lists[]={s.commands.Get()};s.context.Queue()->ExecuteCommandLists(1,lists);
        Check(s.context.Queue()->Signal(s.fence.Get(),++s.serial),"cloth signal");s.nodesReady=s.rigidsReady=true;s.Wait();
        void* data=nullptr;D3D12_RANGE read{0,nodeBytes+rigidBytes};Check(s.readback->Map(0,&read,&data),"cloth readback");
        std::memcpy(nodes.data(),data,nodeBytes);if(rigidBytes)std::memcpy(rigids.data(),static_cast<char*>(data)+nodeBytes,rigidBytes);
        D3D12_RANGE noWrite{0,0};s.readback->Unmap(0,&noWrite);return true;
    }catch(const std::exception& e){s.error=e.what();return false;}
}
}
#else
namespace mikumikustudio::gpu {
struct Cloth::Impl {};
Cloth::Cloth():impl(std::make_unique<Impl>()){} Cloth::~Cloth()=default;
bool Cloth::Initialize(std::uint64_t){return false;}
bool Cloth::Step(std::vector<ClothNode>&,const std::vector<ClothLink>&,std::vector<ClothRigid>&,
    const std::vector<ClothCoupling>&,const std::vector<ClothGroup>&,const std::vector<ClothPass>&,ClothParameters){return false;}
const char* Cloth::Error()const{return "DX12 disabled in this build";}
const wchar_t* Cloth::AdapterName()const{return L"";}
}
#endif
