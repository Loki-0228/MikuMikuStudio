#include "mikumikustudio/d3d12_skinning.hpp"
#if defined(MMD_ENABLE_DX12) && MMD_ENABLE_DX12
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/d3d12_skinning.hpp"
#include <Windows.h>
#include "mikumikustudio/graphics_runtime.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <limits>
#include <unordered_set>
#include "d3d12_skinning_shader.hpp"

namespace mikumikustudio::gpu {
using Microsoft::WRL::ComPtr;
namespace {
void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char message[200]; sprintf_s(message,"%s: HRESULT 0x%08lx",operation,hr);
        throw std::runtime_error(message);
    }
}
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; return b;
}
// Lossless LUID packing: the high 32 bits come from HighPart, the low 32 bits
// from LowPart.
std::uint64_t AdapterId(const LUID& luid) {
    return (std::uint64_t(static_cast<std::uint32_t>(luid.HighPart))<<32) |
           static_cast<std::uint32_t>(luid.LowPart);
}
void RequireInput(ID3D12PipelineState* pipeline, const std::vector<Vertex>& vertices,
                  const std::vector<Bone>& bones, const std::vector<Material>& materials) {
    if(!pipeline || vertices.empty() || bones.empty() || materials.empty() || vertices.size()>65535u*64u)
        throw std::runtime_error("Invalid GPU skinning input or dispatch size");
}
struct Candidate {
    ComPtr<IDXGIAdapter1> adapter;
    std::wstring name;
    std::uint64_t id=0;
    bool integrated=false;
    std::wstring hardwareKey;
};
// Hardware D3D12 feature level 11_0 adapters, with software and remote adapters
// excluded. Collected in DXGI high-performance order, then stably partitioned so
// discrete adapters precede integrated ones. The split comes from the D3D12 UMA
// architecture flag, never from vendor names or memory sizes.
std::vector<Candidate> EnumerateCandidates() {
    ComPtr<IDXGIFactory1> factory;
    if(FAILED(graphics_runtime::CreateFactory2(0,IID_PPV_ARGS(&factory))))
        Check(graphics_runtime::CreateFactory1(IID_PPV_ARGS(&factory)),"CreateDXGIFactory1");
    // IDXGIFactory6 (high-performance ordering) is absent on older runtimes; the
    // enumeration then falls back to IDXGIFactory1 order.
    ComPtr<IDXGIFactory6> factory6;
    if(FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory6)))) factory6.Reset();
    std::vector<Candidate> discrete,integrated;
    std::unordered_set<std::uint64_t> seen;
    for(UINT index=0;;++index) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr=factory6
            ? factory6->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                   IID_PPV_ARGS(&adapter))
            : factory->EnumAdapters1(index,adapter.GetAddressOf());
        if(hr==DXGI_ERROR_NOT_FOUND) break;
        Check(hr,"Enumerate DXGI adapter");
        DXGI_ADAPTER_DESC1 desc{};
        if(FAILED(adapter->GetDesc1(&desc))) continue;
        if(desc.Flags&(DXGI_ADAPTER_FLAG_SOFTWARE|DXGI_ADAPTER_FLAG_REMOTE)) continue;
        ComPtr<ID3D12Device> probe;
        if(FAILED(graphics_runtime::CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&probe)))) continue;
        D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
        // Do not mislabel an adapter when the driver cannot report its type.
        if(FAILED(probe->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE,&architecture,
                                             static_cast<UINT>(sizeof(architecture))))) continue;
        Candidate candidate;
        candidate.adapter=adapter;
        candidate.name=desc.Description;
        LARGE_INTEGER driver{};
        adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver);
        candidate.hardwareKey=std::to_wstring(desc.VendorId)+L"/"+std::to_wstring(desc.DeviceId)+L"/"+
            std::to_wstring(desc.SubSysId)+L"/"+std::to_wstring(desc.Revision)+L"/"+
            std::to_wstring(desc.DedicatedVideoMemory)+L"/"+std::to_wstring(driver.QuadPart);
        // Indirect display drivers may alias the same physical compute GPU.
        candidate.id=AdapterId(probe->GetAdapterLuid());
        if(!seen.insert(candidate.id).second) continue;
        candidate.integrated=architecture.UMA!=FALSE;
        (candidate.integrated?integrated:discrete).push_back(std::move(candidate));
    }
    discrete.insert(discrete.end(),std::make_move_iterator(integrated.begin()),
                    std::make_move_iterator(integrated.end()));
    return discrete;
}
}
struct Skinning::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12PipelineState> packPipeline;
    HANDLE event=nullptr;
    std::uint64_t next=0;
    std::string error;
    std::wstring adapter;
    std::uint64_t adapterId=0;
    struct Frame {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Resource> vertices,vertexUpload,bones,materials,output,readback,packed;
        bool packedReady=false;
        std::uint64_t geometryRevision=0;
        bool verticesReady=false;
        std::uint64_t waitFence=0,generation=0;
        bool outputReady=false;
    };
    std::array<Frame,3> frames;
    ~Impl() {Reset();}
    void Wait(std::uint64_t value) {
        if(!value) return;
        const auto complete=fence->GetCompletedValue();
        if(complete==UINT64_MAX) Check(device->GetDeviceRemovedReason(),"device removed");
        if(complete>=value) return;
        Check(fence->SetEventOnCompletion(value,event),"SetEventOnCompletion");
        if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)
            throw std::runtime_error("D3D12 fence timeout (10s)");
    }
    // Releases everything a partial initialisation may have created, so a failed
    // attempt cannot pollute the next Initialize call.
    void Reset() {
        if(fence&&event) {try {for(auto& f:frames) Wait(f.waitFence);} catch(...) {}}
        for(auto& f:frames) {
            f.allocator.Reset(); f.commands.Reset();
            f.vertices.Reset(); f.vertexUpload.Reset(); f.bones.Reset();
            f.materials.Reset(); f.output.Reset(); f.readback.Reset(); f.packed.Reset(); f.packedReady=false;
            f.geometryRevision=0; f.verticesReady=false;
            f.waitFence=0; f.generation=0; f.outputReady=false;
        }
        packPipeline.Reset(); pipeline.Reset(); root.Reset(); fence.Reset(); queue.Reset(); device.Reset();
        if(event) {CloseHandle(event); event=nullptr;}
        next=0; adapterId=0; adapter.clear();
    }
    bool Buffer(ComPtr<ID3D12Resource>& resource,std::uint64_t bytes,D3D12_HEAP_TYPE heap,
                D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
        if(resource && resource->GetDesc().Width>=bytes) return false;
        resource.Reset();
        D3D12_HEAP_PROPERTIES properties{}; properties.Type=heap;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width=(bytes+65535)&~std::uint64_t(65535); desc.Height=1;
        desc.DepthOrArraySize=1; desc.MipLevels=1; desc.SampleDesc.Count=1;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags=flags;
        Check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,state,
            nullptr,IID_PPV_ARGS(&resource)),"CreateCommittedResource"); return true;
    }
    template<class T> void Upload(ComPtr<ID3D12Resource>& resource,const std::vector<T>& data) {
        const auto bytes=data.size()*sizeof(T);
        Buffer(resource,bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped=nullptr; D3D12_RANGE noRead{0,0};
        Check(resource->Map(0,&noRead,&mapped),"Map upload");
        std::memcpy(mapped,data.data(),bytes);
        D3D12_RANGE written{0,bytes}; resource->Unmap(0,&written);
    }
    std::uint64_t Execute(Frame& frame) {
        Check(frame.commands->Close(),"Close commands");
        ID3D12CommandList* lists[]={frame.commands.Get()};
        queue->ExecuteCommandLists(1,lists);
        Check(queue->Signal(fence.Get(),++next),"Signal");
        frame.waitFence=next; return next;
    }
    void PreparePackPipeline() {
        if(packPipeline) return;
        // Integer copies preserve NaNs, signed zero and every output bit.
        static constexpr char source[]=R"(
ByteAddressBuffer source : register(t0);
RWByteAddressBuffer packed : register(u0);
cbuffer Parameters : register(b0) { uint count, bodyBytes; };
[numthreads(64,1,1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if(tid.x>=count) return;
    uint inputOffset=tid.x*112, outputOffset=tid.x*(bodyBytes+16);
    for(uint offset=0;offset<bodyBytes;offset+=16)
        packed.Store4(outputOffset+offset,source.Load4(inputOffset+offset));
    packed.Store4(outputOffset+bodyBytes,source.Load4(inputOffset+96));
})";
        ComPtr<ID3DBlob> shader,errors;
        const auto hr=graphics_runtime::Compile(source,sizeof(source)-1,"skinning_readback_pack",
            nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&errors);
        if(FAILED(hr) && errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        Check(hr,"compile readback packing");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root.Get();
        pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
        Check(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&packPipeline)),"Create packing pipeline");
    }
    // Builds the device and the shared objects on one adapter; throws on failure.
    void Setup(IDXGIAdapter1* selected) {
        Check(graphics_runtime::CreateDevice(selected,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC desc{};
        Check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"CreateCommandQueue");
        Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"CreateFence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event) throw std::runtime_error("CreateEvent failed");
        D3D12_ROOT_PARAMETER parameters[5]{};
        for(unsigned i=0;i<3;++i) { parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[i].Descriptor.ShaderRegister=i; }
        parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[4].Constants.Num32BitValues=7;
        D3D12_ROOT_SIGNATURE_DESC rootDesc{}; rootDesc.NumParameters=5; rootDesc.pParameters=parameters;
        ComPtr<ID3DBlob> rootCode,errors;
        Check(graphics_runtime::SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&rootCode,&errors),"serialize root");
        Check(device->CreateRootSignature(0,rootCode->GetBufferPointer(),rootCode->GetBufferSize(),
            IID_PPV_ARGS(&root)),"CreateRootSignature");
        ComPtr<ID3DBlob> shader;
        HRESULT compile=graphics_runtime::Compile(kSkinningShader,sizeof(kSkinningShader)-1,"d3d12_skinning.hlsl",
            nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&shader,&errors);
        if(FAILED(compile) && errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        Check(compile,"compile compute shader");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature=root.Get();
        pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
        Check(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline)),"CreateComputePipelineState");
        for(auto& f:frames) {
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f.allocator)),"CreateCommandAllocator");
            Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f.allocator.Get(),pipeline.Get(),
                IID_PPV_ARGS(&f.commands)),"CreateCommandList");
            Check(f.commands->Close(),"initial Close");
        }
    }
    // Records the uploads and the skinning dispatch for input that passed
    // RequireInput. On return the output resource holds fresh results in the
    // unordered-access state; returns the vertex count.
    std::uint32_t RecordCompute(Frame& f,const std::vector<Vertex>& vertices,
                                const std::vector<Bone>& bones,const std::vector<Material>& materials,
                                Parameters p) {
        const bool update=!p.geometryRevision||p.geometryRevision!=f.geometryRevision;
        if(update) {
            for(const auto& v:vertices) if(v.type>3 || v.material>=materials.size())
                throw std::runtime_error("Unsupported skinning type or invalid material");
            Upload(f.vertexUpload,vertices);
            if(Buffer(f.vertices,vertices.size()*sizeof(Vertex),D3D12_HEAP_TYPE_DEFAULT,
                        D3D12_RESOURCE_STATE_COPY_DEST)) f.verticesReady=false;
        }
        Upload(f.bones,bones); Upload(f.materials,materials);
        if(Buffer(f.output,vertices.size()*sizeof(Output),D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) f.outputReady=false;
        Check(f.allocator->Reset(),"Reset allocator");
        Check(f.commands->Reset(f.allocator.Get(),pipeline.Get()),"Reset list");
        if(update) {
            if(f.verticesReady) {
                auto b=Transition(f.vertices.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
                f.commands->ResourceBarrier(1,&b);
            }
            f.commands->CopyBufferRegion(f.vertices.Get(),0,f.vertexUpload.Get(),0,vertices.size()*sizeof(Vertex));
            auto b=Transition(f.vertices.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            f.commands->ResourceBarrier(1,&b);
            f.verticesReady=true; f.geometryRevision=p.geometryRevision;
        }
        if(f.outputReady) {
            auto b=Transition(f.output.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            f.commands->ResourceBarrier(1,&b);
        }
        f.commands->SetComputeRootSignature(root.Get());
        f.commands->SetComputeRootShaderResourceView(0,f.vertices->GetGPUVirtualAddress());
        f.commands->SetComputeRootShaderResourceView(1,f.bones->GetGPUVirtualAddress());
        f.commands->SetComputeRootShaderResourceView(2,f.materials->GetGPUVirtualAddress());
        f.commands->SetComputeRootUnorderedAccessView(3,f.output->GetGPUVirtualAddress());
        struct Constants {UINT vertices,bones,materials;float distance;UINT opaque,pmd,perVertex;} constants{
            UINT(vertices.size()),UINT(bones.size()),UINT(materials.size()),p.edgeDistance,p.opaqueEdge?1u:0u,p.pmd?1u:0u,p.perVertex?1u:0u};
        f.commands->SetComputeRoot32BitConstants(4,7,&constants,0);
        f.commands->Dispatch((constants.vertices+63)/64,1,1);
        return constants.vertices;
    }
};
Skinning::Skinning():impl(std::make_unique<Impl>()){}
Skinning::~Skinning()=default;
const char* Skinning::Error() const{return impl->error.c_str();}
const wchar_t* Skinning::AdapterName() const{return impl->adapter.c_str();}
ID3D12Device* Skinning::Device() const{return impl->device.Get();}
ID3D12CommandQueue* Skinning::Queue() const{return impl->queue.Get();}
std::vector<Adapter> EnumerateAdapters() {
    std::vector<Adapter> result;
    for(auto& candidate:EnumerateCandidates()) {
        Adapter adapter;
        adapter.id=candidate.id;
        adapter.name=std::move(candidate.name);
        adapter.integrated=candidate.integrated;
        adapter.hardwareKey=std::move(candidate.hardwareKey);
        result.push_back(std::move(adapter));
    }
    return result;
}
bool Skinning::Initialize(bool debug,std::uint64_t adapterId) {
    auto& s=*impl;
    try {
        if(s.pipeline) {
            if(adapterId && adapterId!=s.adapterId) {
                s.error="Skinning already initialized on a different adapter";
                return false;
            }
            return true;
        }
        s.error.clear();
        // The debug layer has to be on before the first device is created.
        if(debug) {
            ComPtr<ID3D12Debug> layer;
            Check(graphics_runtime::GetDebugInterface(IID_PPV_ARGS(&layer)),"D3D12 debug layer");
            layer->EnableDebugLayer();
        }
        auto candidates=EnumerateCandidates();
        if(candidates.empty()) throw std::runtime_error("No hardware D3D12 adapter");
        bool attempted=false;
        std::string failure;
        for(auto& candidate:candidates) {
            if(adapterId && candidate.id!=adapterId) continue;
            attempted=true;
            s.Reset();
            try {
                s.Setup(candidate.adapter.Get());
            } catch(const std::exception& e) {
                // Automatic mode falls through to the next adapter; an explicit
                // adapterId has no second entry in this loop.
                failure=e.what();
                continue;
            }
            s.adapter=candidate.name;
            s.adapterId=candidate.id;
            return true;
        }
        s.Reset();
        if(!attempted) throw std::runtime_error("Requested GPU adapter id not found");
        throw std::runtime_error(failure.empty()?std::string("No hardware D3D12 adapter"):failure);
    } catch(const std::exception& e) {
        s.Reset();
        s.error=e.what();
        return false;
    }
}
bool Skinning::Submit(const std::vector<Vertex>& vertices,const std::vector<Bone>& bones,
                     const std::vector<Material>& materials,Parameters p,Ticket& ticket) {
    auto& s=*impl; ticket={};
    try {
        RequireInput(s.pipeline.Get(),vertices,bones,materials);
        const unsigned slot=static_cast<unsigned>(s.next%3); auto& f=s.frames[slot];
        s.Wait(f.waitFence);
        const auto count=s.RecordCompute(f,vertices,bones,materials,p);
        auto b=Transition(f.output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        f.commands->ResourceBarrier(1,&b);
        f.generation=s.Execute(f); f.outputReady=true;
        ticket={f.generation,slot,count}; return true;
    } catch(const std::exception& e) {s.error=e.what();return false;}
}
bool Skinning::ReleaseWorkingBuffers() {
    auto& s=*impl;
    try {
        for(auto& f:s.frames) s.Wait(f.waitFence);
        for(auto& f:s.frames) {
            f.vertices.Reset(); f.vertexUpload.Reset(); f.bones.Reset();
            f.materials.Reset(); f.output.Reset(); f.readback.Reset(); f.packed.Reset(); f.packedReady=false;
            f.geometryRevision=0; f.verticesReady=false;
            f.waitFence=0; f.generation=0; f.outputReady=false;
        }
        return true;
    } catch(const std::exception& e) {s.error=e.what();return false;}
}
bool Skinning::SubmitAndReadback(const std::vector<Vertex>& vertices,const std::vector<Bone>& bones,
                                 const std::vector<Material>& materials,Parameters p,
                                 std::vector<Output>& output,Ticket* ticket) {
    auto& s=*impl;
    try {
        if(ticket) *ticket={};
        RequireInput(s.pipeline.Get(),vertices,bones,materials);
        const unsigned slot=static_cast<unsigned>(s.next%3); auto& f=s.frames[slot];
        s.Wait(f.waitFence);
        const auto count=s.RecordCompute(f,vertices,bones,materials,p);
        const auto bytes=std::size_t(count)*sizeof(Output);
        s.Buffer(f.readback,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        // One command list carries the dispatch, the output copy and the state
        // restoration, so only this submission's fence is waited on below.
        auto b=Transition(f.output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        f.commands->ResourceBarrier(1,&b);
        f.commands->CopyBufferRegion(f.readback.Get(),0,f.output.Get(),0,bytes);
        b=Transition(f.output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        f.commands->ResourceBarrier(1,&b);
        f.generation=s.Execute(f); f.outputReady=true;
        if(ticket) *ticket={f.generation,slot,count};
        s.Wait(f.generation);
        output.resize(count);
        void* mapped=nullptr; D3D12_RANGE range{0,bytes};
        Check(f.readback->Map(0,&range,&mapped),"Map readback");
        std::memcpy(output.data(),mapped,bytes);
        D3D12_RANGE noWrite{0,0}; f.readback->Unmap(0,&noWrite);
        return true;
    } catch(const std::exception& e) {s.error=e.what();return false;}
}
bool Skinning::SubmitAndReadbackCompact(const std::vector<Vertex>& vertices,const std::vector<Bone>& bones,
                                        const std::vector<Material>& materials,Parameters p,
                                        unsigned additionalUvCount,std::vector<unsigned char>& output) {
    auto& s=*impl;
    try {
        if(additionalUvCount>4) throw std::runtime_error("Invalid compact UV count");
        RequireInput(s.pipeline.Get(),vertices,bones,materials);
        s.PreparePackPipeline();
        const unsigned slot=static_cast<unsigned>(s.next%3);auto& f=s.frames[slot];
        s.Wait(f.waitFence);
        const auto bodyBytes=32u+16u*additionalUvCount;
        const auto bytes=vertices.size()*(bodyBytes+16u);
        if(s.Buffer(f.packed,bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) f.packedReady=false;
        s.Buffer(f.readback,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        const auto count=s.RecordCompute(f,vertices,bones,materials,p);
        auto b=Transition(f.output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        f.commands->ResourceBarrier(1,&b);
        if(f.packedReady) {
            b=Transition(f.packed.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            f.commands->ResourceBarrier(1,&b);
        }
        f.commands->SetPipelineState(s.packPipeline.Get());
        f.commands->SetComputeRootShaderResourceView(0,f.output->GetGPUVirtualAddress());
        f.commands->SetComputeRootUnorderedAccessView(3,f.packed->GetGPUVirtualAddress());
        const UINT constants[]={count,bodyBytes};
        f.commands->SetComputeRoot32BitConstants(4,2,constants,0);
        f.commands->Dispatch((count+63)/64,1,1);
        b=Transition(f.packed.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        f.commands->ResourceBarrier(1,&b);
        f.commands->CopyBufferRegion(f.readback.Get(),0,f.packed.Get(),0,bytes);
        f.generation=s.Execute(f);f.outputReady=true;f.packedReady=true;
        s.Wait(f.generation);output.resize(bytes);
        void* mapped=nullptr;D3D12_RANGE range{0,bytes};
        Check(f.readback->Map(0,&range,&mapped),"Map compact readback");
        std::memcpy(output.data(),mapped,bytes);
        D3D12_RANGE noWrite{0,0};f.readback->Unmap(0,&noWrite);
        return true;
    } catch(const std::exception& e) {s.error=e.what();return false;}
}
ID3D12Resource* Skinning::OutputResource(Ticket t) const {
    if(t.slot>=3 || !t.fence || impl->frames[t.slot].generation!=t.fence) return nullptr;
    return impl->frames[t.slot].output.Get();
}
bool Skinning::Wait(Ticket t) {
    try {
        if(!OutputResource(t)) throw std::runtime_error("Expired GPU skinning ticket");
        impl->Wait(t.fence);return true;
    } catch(const std::exception& e) {impl->error=e.what();return false;}
}
bool Skinning::RetainForQueuedGraphics(Ticket t) {
    try {
        if(!OutputResource(t)) throw std::runtime_error("Expired GPU skinning ticket");
        Check(impl->queue->Signal(impl->fence.Get(),++impl->next),"graphics Signal");
        impl->frames[t.slot].waitFence=impl->next;return true;
    } catch(const std::exception& e) {impl->error=e.what();return false;}
}
bool Skinning::Readback(Ticket t,std::vector<Output>& output) {
    auto& s=*impl;
    try {
        if(!OutputResource(t)) throw std::runtime_error("Expired GPU skinning ticket");
        auto& f=s.frames[t.slot]; s.Wait(f.waitFence);
        const auto bytes=std::size_t(t.count)*sizeof(Output);
        s.Buffer(f.readback,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        Check(f.allocator->Reset(),"readback Reset allocator");
        Check(f.commands->Reset(f.allocator.Get(),nullptr),"readback Reset list");
        auto b=Transition(f.output.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        f.commands->ResourceBarrier(1,&b);
        f.commands->CopyBufferRegion(f.readback.Get(),0,f.output.Get(),0,bytes);
        b=Transition(f.output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        f.commands->ResourceBarrier(1,&b);
        s.Wait(s.Execute(f)); output.resize(t.count);
        void* mapped=nullptr; D3D12_RANGE range{0,bytes};
        Check(f.readback->Map(0,&range,&mapped),"Map readback");
        std::memcpy(output.data(),mapped,bytes); D3D12_RANGE noWrite{0,0}; f.readback->Unmap(0,&noWrite);
        return true;
    } catch(const std::exception& e) {s.error=e.what();return false;}
}
}
#else
namespace mikumikustudio::gpu {
struct Skinning::Impl { std::string error = "DX12 support is disabled in this build"; };
Skinning::Skinning():impl(std::make_unique<Impl>()){}
Skinning::~Skinning()=default;
const char* Skinning::Error() const{return impl->error.c_str();}
const wchar_t* Skinning::AdapterName() const{return L"";}
ID3D12Device* Skinning::Device() const{return nullptr;}
ID3D12CommandQueue* Skinning::Queue() const{return nullptr;}
std::vector<Adapter> EnumerateAdapters(){return {};}
bool Skinning::Initialize(bool,std::uint64_t){return false;}
bool Skinning::ReleaseWorkingBuffers(){return true;}
bool Skinning::Submit(const std::vector<Vertex>&,const std::vector<Bone>&,
                      const std::vector<Material>&,Parameters,Ticket&){return false;}
bool Skinning::SubmitAndReadback(const std::vector<Vertex>&,const std::vector<Bone>&,
                                 const std::vector<Material>&,Parameters,
                                 std::vector<Output>&,Ticket*){return false;}
bool Skinning::SubmitAndReadbackCompact(const std::vector<Vertex>&,const std::vector<Bone>&,
                                        const std::vector<Material>&,Parameters,unsigned,
                                        std::vector<unsigned char>&){return false;}
bool Skinning::Readback(Ticket,std::vector<Output>&){return false;}
bool Skinning::Wait(Ticket){return false;}
ID3D12Resource* Skinning::OutputResource(Ticket) const{return nullptr;}
bool Skinning::RetainForQueuedGraphics(Ticket){return false;}
}
#endif
