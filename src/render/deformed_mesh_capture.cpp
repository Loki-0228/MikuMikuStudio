#define NOMINMAX
#include "mikudancestudio/deformed_mesh_capture.hpp"
#include "mikudancestudio/vertex_capture.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/native_mesh.hpp"
#include "mikudancestudio/d3dx_dyn.hpp"
#include <d3d9.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <MinHook.h>

namespace mikudancestudio::deformed_mesh_capture {
using Microsoft::WRL::ComPtr;
namespace {
using DrawIndexed = HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
using Draw = HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT);
using DrawUp = HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,const void*,UINT);
using DrawIndexedUp = HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT,UINT,const void*,D3DFORMAT,const void*,UINT);
// Retain forwarding addresses if teardown cannot disable a native detour.
// An inactive hook must still forward draws.
DrawIndexed fallbackIndexed{};
Draw fallbackDraw{};
DrawUp fallbackUp{};
DrawIndexedUp fallbackIndexedUp{};
struct State {
    MMDApp& app;
    bool selectedOnly;
    bool collecting=false,busy=false,camera=false;
    DWORD thread=GetCurrentThreadId();
    std::vector<Mesh> meshes;
    std::string error;
    d3dx::D3DXMATRIXF inverse{};
    State(MMDApp& a,bool selected):app(a),selectedOnly(selected){}
};
struct Hooks {
    std::array<void*,4> targets{};
    unsigned installed=0;
    HMODULE library{};
    HWND window{};
    ComPtr<IDirect3D9> factory;
    ComPtr<IDirect3DDevice9> probe;
    explicit Hooks(MMDApp& app);
    void Unhook() noexcept;
    ~Hooks(){Unhook();probe.Reset();factory.Reset();if(window)DestroyWindow(window);if(library)FreeLibrary(library);}
};
State* active=nullptr;
std::unique_ptr<Hooks> hooks;
struct ObserverRegistration { DrawObserver* value=nullptr;DWORD thread=0; };
std::array<ObserverRegistration,2> observers{};
thread_local bool observing=false;
thread_local unsigned observerSilence=0;
thread_local const void* sourceObject=nullptr;
thread_local unsigned sourceMaterial=0;
thread_local bool sourceOpaqueCoverage=false;
void Check(HRESULT hr,const char* operation) {if(FAILED(hr))throw std::runtime_error(operation);}
std::array<float,4> Transform(const std::array<float,4>& p,const d3dx::D3DXMATRIXF& matrix) {
    const auto* m=reinterpret_cast<const float*>(&matrix);std::array<float,4> out{};
    for(int c=0;c<4;++c)for(int r=0;r<4;++r)out[c]+=p[r]*m[r*4+c];return out;
}
std::string ObjectName(State& s) {
    const auto* original=native_mesh::SourceObject(sourceObject);
    for(int i=0;i<kModelSlotCount;++i)if(s.app.ModelSlot(i)==original) {
        if(s.selectedOnly&&s.app.SelectedModel()!=original)return {};
        return "model_"+std::to_string(i)+"_material_"+std::to_string(sourceMaterial);
    }
    if(!s.selectedOnly)for(int i=0;i<255;++i)if(s.app.AccessorySlot(i)==original)
        return "accessory_"+std::to_string(i)+"_material_"+std::to_string(sourceMaterial);
    return {};
}
struct LockedBuffer {
    ComPtr<IDirect3DVertexBuffer9> buffer;void* bytes{};
    ~LockedBuffer(){if(bytes)buffer->Unlock();}
};
bool MeshInput(IDirect3DDevice9* device) {
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    if(FAILED(device->GetVertexDeclaration(&declaration))||!declaration)return false;
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};UINT count=MAXD3DDECLLENGTH+1;
    if(FAILED(declaration->GetDeclaration(elements,&count)))return false;
    bool position=false,normal=false;
    for(UINT i=0;i<count&&elements[i].Stream!=0xff;++i){
        position|=elements[i].Usage==D3DDECLUSAGE_POSITION&&elements[i].UsageIndex==0;
        normal|=elements[i].Usage==D3DDECLUSAGE_NORMAL&&elements[i].UsageIndex==0;
    }
    return position&&normal;
}
void Capture(State& s,IDirect3DDevice9* device,D3DPRIMITIVETYPE type,INT base,UINT start,UINT primitives) {
    const auto name=ObjectName(s);if(name.empty()||!primitives)return;
    // MME may draw axes/guide lines inside a material callback. These are not
    // polygon surfaces and cannot become an Alembic PolyMesh.
    if(type==D3DPT_LINELIST||type==D3DPT_LINESTRIP||type==D3DPT_POINTLIST)return;
    if(!MeshInput(device))return; // screen quads and buffered UI draws inside MME
    if(type!=D3DPT_TRIANGLELIST)throw std::runtime_error("Unsupported effect primitive type "+std::to_string(type)+" count "+std::to_string(primitives));
    if(!s.camera)throw std::runtime_error("The export camera transform is unavailable");
    ComPtr<IDirect3DIndexBuffer9> ib;Check(device->GetIndices(&ib),"Cannot read the rendered mesh indices");
    if(!ib)throw std::runtime_error("The effect did not bind an index buffer");
    D3DINDEXBUFFER_DESC indexDesc{};Check(ib->GetDesc(&indexDesc),"Cannot describe the rendered mesh indices");
    const unsigned indexSize=indexDesc.Format==D3DFMT_INDEX32?4:2;
    const uint64_t indexCount=uint64_t(primitives)*3;
    if(indexCount>100000000|| (uint64_t(start)+indexCount)*indexSize>indexDesc.Size)throw std::runtime_error("Invalid rendered mesh index range");
    std::vector<unsigned> indices(size_t(indexCount),0);unsigned first=UINT_MAX,last=0;
    void* data=nullptr;Check(ib->Lock(start*indexSize,UINT(indexCount*indexSize),&data,D3DLOCK_READONLY),"Cannot lock the rendered mesh indices");
    for(size_t i=0;i<indices.size();++i) {
        const int64_t value=int64_t(base)+(indexSize==4?static_cast<uint32_t*>(data)[i]:static_cast<uint16_t*>(data)[i]);
        if(value<0||value>INT32_MAX){ib->Unlock();throw std::runtime_error("Invalid rendered vertex index");}
        indices[i]=unsigned(value);first=std::min(first,indices[i]);last=std::max(last,indices[i]);
    }
    ib->Unlock();const unsigned count=last-first+1;
    if(count>16000000)throw std::runtime_error("The rendered mesh exceeds the vertex capture limit");
    ComPtr<IDirect3DVertexDeclaration9> declaration;Check(device->GetVertexDeclaration(&declaration),"Cannot read the rendered vertex declaration");
    if(!declaration)throw std::runtime_error("The effect did not bind a vertex declaration");
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH+1]{};UINT elementCount=MAXD3DDECLLENGTH+1;
    Check(declaration->GetDeclaration(elements,&elementCount),"Cannot describe the rendered vertices");
    const D3DVERTEXELEMENT9* pos=nullptr;const D3DVERTEXELEMENT9* uv=nullptr;
    for(unsigned i=0;i<elementCount&&elements[i].Stream!=0xff;++i) {
        if(elements[i].Usage==D3DDECLUSAGE_POSITION&&elements[i].UsageIndex==0)pos=&elements[i];
        if(elements[i].Usage==D3DDECLUSAGE_TEXCOORD&&elements[i].UsageIndex==0)uv=&elements[i];
    }
    std::array<LockedBuffer,16> inputs;std::array<UINT,16> offsets{},strides{},sizes{};
    const auto read=[&](const D3DVERTEXELEMENT9& element,unsigned index,float* out,unsigned dimensions) {
        if(element.Stream>=inputs.size()||element.Type>D3DDECLTYPE_FLOAT4||unsigned(element.Type)+1<dimensions)
            throw std::runtime_error("Unsupported rendered vertex attribute format");
        auto& input=inputs[element.Stream];
        if(!input.buffer) {
            Check(device->GetStreamSource(element.Stream,&input.buffer,&offsets[element.Stream],&strides[element.Stream]),"Cannot read the vertex input stream");
            if(!input.buffer)throw std::runtime_error("Missing vertex input stream");
            D3DVERTEXBUFFER_DESC d{};Check(input.buffer->GetDesc(&d),"Cannot describe the vertex input stream");
            sizes[element.Stream]=d.Size;
            Check(input.buffer->Lock(0,0,&input.bytes,D3DLOCK_READONLY),"Cannot read the rendered vertex buffer");
        }
        if(uint64_t(offsets[element.Stream])+uint64_t(index)*strides[element.Stream]+element.Offset+dimensions*4>sizes[element.Stream])
            throw std::runtime_error("Invalid rendered vertex buffer range");
        memcpy(out,static_cast<const char*>(input.bytes)+offsets[element.Stream]+size_t(index)*strides[element.Stream]+element.Offset,dimensions*4);
    };
    // Snapshot UVs before replaying the shader; unlock streams before GPU use.
    std::vector<std::array<float,2>> uvs(count);
    if(uv)for(unsigned i=0;i<count;++i)read(*uv,first+i,uvs[i].data(),2);
    ComPtr<IDirect3DVertexShader9> shader;Check(device->GetVertexShader(&shader),"Cannot read the active vertex shader");
    std::vector<std::array<float,4>> clip(count);
    uint64_t shaderHash=0;
    if(shader) {
        UINT size=0;Check(shader->GetFunction(nullptr,&size),"Cannot describe the active vertex shader");
        std::vector<unsigned char> code(size);Check(shader->GetFunction(code.data(),&size),"Cannot read the active vertex shader");
        shaderHash=1469598103934665603ull;for(auto b:code){shaderHash^=b;shaderHash*=1099511628211ull;}
        for(auto& input:inputs)if(input.bytes){input.buffer->Unlock();input.bytes=nullptr;}
        std::string message;
        if(!vertex_capture::Capture(device,first,count,clip,message))throw std::runtime_error("Effect vertex capture failed: "+message);
    } else {
        if(!pos)throw std::runtime_error("The fixed-function mesh has no position attribute");
        d3dx::D3DXMATRIXF world{},view{},projection{},wv{},wvp{};
        Check(device->GetTransform(D3DTS_WORLD,reinterpret_cast<D3DMATRIX*>(&world)),"Cannot read the mesh transform");
        device->GetTransform(D3DTS_VIEW,reinterpret_cast<D3DMATRIX*>(&view));device->GetTransform(D3DTS_PROJECTION,reinterpret_cast<D3DMATRIX*>(&projection));
        auto& dx=d3dx::Get();dx.multiply(&wv,&world,&view);dx.multiply(&wvp,&wv,&projection);
        for(unsigned i=0;i<count;++i){std::array<float,4> p{0,0,0,1};read(*pos,first+i,p.data(),3);clip[i]=Transform(p,wvp);}
    }
    Mesh mesh;const auto prefix=name+"_shader_"+std::to_string(shaderHash)+"_pass_";
    std::map<unsigned,int32_t> remap;
    for(unsigned index:indices) {
        auto inserted=remap.emplace(index,int32_t(remap.size()));
        if(inserted.second) {
            const auto p=Transform(clip[index-first],s.inverse);
            if(!std::isfinite(p[3])||std::abs(p[3])<1e-10f)throw std::runtime_error("The effect produced a vertex at infinity");
            std::array<float,3> world{p[0]/p[3],p[1]/p[3],-p[2]/p[3]};
            for(float v:world)if(!std::isfinite(v))throw std::runtime_error("The effect produced a non-finite vertex");
            mesh.positions.push_back(world);auto t=uvs[index-first];t[1]=1-t[1];mesh.uvs.push_back(t);
        }
        mesh.indices.push_back(inserted.first->second);
    }
    for(size_t i=0;i<mesh.indices.size();i+=3)std::swap(mesh.indices[i+1],mesh.indices[i+2]);
    unsigned ordinal=0;
    for(const auto& previous:s.meshes)if(previous.name.rfind(prefix,0)==0) {
        // Native alpha/outline preparation can repeat the same surface. Keep
        // distinct deformations, but do not duplicate an identical pass.
        if(previous.indices==mesh.indices&&previous.positions==mesh.positions&&previous.uvs==mesh.uvs)return;
        ++ordinal;
    }
    mesh.name=prefix+std::to_string(ordinal);s.meshes.push_back(std::move(mesh));
}
HRESULT WINAPI Hook(IDirect3DDevice9* device,D3DPRIMITIVETYPE type,INT base,UINT min,UINT count,UINT start,UINT primitives) {
    if(observing||observerSilence)return fallbackIndexed(device,type,base,min,count,start,primitives);
    observing=true;
    for(const auto& observer:observers)if(observer.value&&observer.thread==GetCurrentThreadId())
        observer.value->Indexed(device,type,base,min,count,start,primitives,sourceObject,sourceMaterial);
    auto* s=active;
    if(s&&s->collecting&&!s->busy&&s->thread==GetCurrentThreadId()&&sourceObject&&s->error.empty()) {
        s->busy=true;
        try{Capture(*s,device,type,base,start,primitives);}catch(const std::exception& e){s->error=e.what();}catch(...){s->error="Geometry capture failed";}
        s->busy=false;
    }
    observing=false;
    return fallbackIndexed(device,type,base,min,count,start,primitives);
}
void UnsupportedDraw(IDirect3DDevice9* device,D3DPRIMITIVETYPE type,UINT count) {
    if(observing||observerSilence)return;
    observing=true;
    for(const auto& observer:observers)if(observer.value&&observer.thread==GetCurrentThreadId())
        observer.value->Unsupported(device,type,count);
    observing=false;
    auto* s=active;
    if(!s||!count||type<D3DPT_TRIANGLELIST||!s->collecting||s->busy||s->thread!=GetCurrentThreadId()||!sourceObject||!s->error.empty())return;
    try{if(!ObjectName(*s).empty()&&MeshInput(device))s->error="The effect uses a non-indexed or user-memory draw that cannot be exported";}
    catch(const std::exception& e){s->error=e.what();}
}
HRESULT WINAPI HookDraw(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT start,UINT count){UnsupportedDraw(d,t,count);return fallbackDraw(d,t,start,count);}
HRESULT WINAPI HookUp(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT count,const void* p,UINT stride){UnsupportedDraw(d,t,count);return fallbackUp(d,t,count,p,stride);}
HRESULT WINAPI HookIndexedUp(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT min,UINT vertices,UINT count,const void* indices,D3DFORMAT format,const void* p,UINT stride){UnsupportedDraw(d,t,count);return fallbackIndexedUp(d,t,min,vertices,count,indices,format,p,stride);}
Hooks::Hooks(MMDApp& app) {
        // MME also wraps Surface::GetDevice. Resolve native implementation
        // addresses from a private device created by the system runtime.
        wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
        library=LoadLibraryW((std::wstring(system)+L"\\d3d9.dll").c_str());
        if(!library)throw std::runtime_error("Cannot load the system Direct3D runtime");
        auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(library,"Direct3DCreate9"));
        factory.Attach(create?create(D3D_SDK_VERSION):nullptr);
        if(!factory)throw std::runtime_error("Cannot initialize geometry capture");
        window=CreateWindowExW(0,L"STATIC",L"Geometry export",WS_POPUP,0,0,8,8,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        D3DDEVICE_CREATION_PARAMETERS cp{};Check(app.Renderer()->device->GetCreationParameters(&cp),"Cannot read the renderer device settings");
        D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;pp.BackBufferWidth=pp.BackBufferHeight=8;pp.BackBufferFormat=D3DFMT_X8R8G8B8;
        Check(factory->CreateDevice(cp.AdapterOrdinal,cp.DeviceType,window,cp.BehaviorFlags,&pp,&probe),"Cannot initialize native geometry capture");
        auto table=*reinterpret_cast<void***>(probe.Get());
        const auto initialized=MH_Initialize();
        if(initialized!=MH_OK&&initialized!=MH_ERROR_ALREADY_INITIALIZED)throw std::runtime_error("Cannot initialize geometry draw capture");
        void* hooks[]={reinterpret_cast<void*>(HookDraw),reinterpret_cast<void*>(Hook),reinterpret_cast<void*>(HookUp),reinterpret_cast<void*>(HookIndexedUp)};
        void** originals[]={reinterpret_cast<void**>(&fallbackDraw),reinterpret_cast<void**>(&fallbackIndexed),reinterpret_cast<void**>(&fallbackUp),reinterpret_cast<void**>(&fallbackIndexedUp)};
        try {
            // The runtime rewrites device vtable slots as state changes.
            // Detour native code instead, after MME has selected its shader.
            for(;installed<4;++installed){
                targets[installed]=table[81+installed];
                const auto status=MH_CreateHook(targets[installed],hooks[installed],originals[installed]);
                if(status!=MH_OK)throw std::runtime_error(std::string("Cannot capture native draw: ")+MH_StatusToString(status));
            }
            for(auto target:targets)MH_QueueEnableHook(target);
            if(MH_ApplyQueued()!=MH_OK)throw std::runtime_error("Cannot enable native draw capture");
        }
        catch(...){Unhook();throw;}
}
void Hooks::Unhook() noexcept {
        for(unsigned i=0;i<installed;++i)MH_QueueDisableHook(targets[i]);
        if(MH_ApplyQueued()==MH_OK)for(unsigned i=0;i<installed;++i)MH_RemoveHook(targets[i]);
        installed=0;
}
void DropUnusedHooks(){
    const bool registered=std::any_of(observers.begin(),observers.end(),[](const auto& entry){return entry.value!=nullptr;});
    // A callback may unregister itself. Never free the trampoline while its
    // native hook is still on the stack; the next outside teardown can drop it.
    if(!active&&!registered&&!observing)hooks.reset();
}
}
struct Session::Impl:State {
    Impl(MMDApp& app,bool selected):State(app,selected) {
        if(active)throw std::runtime_error("A geometry export is already running");
        if(!hooks)hooks=std::make_unique<Hooks>(app);
        active=this;
    }
    ~Impl(){if(active==this)active=nullptr;DropUnusedHooks();}
};
bool Observe(MMDApp& app,DrawObserver* value,std::string& error) {
    error.clear();
    if(!value){error="The native draw observer is null";return false;}
    for(auto& registered:observers)if(registered.value==value) {
        registered.thread=GetCurrentThreadId();return true;
    }
    const auto free=std::find_if(observers.begin(),observers.end(),[](const auto& entry){return entry.value==nullptr;});
    if(free==observers.end()){error="Two native draw observers are already active";return false;}
    try{if(!hooks)hooks=std::make_unique<Hooks>(app);}
    catch(const std::exception& e){error=e.what();return false;}
    *free={value,GetCurrentThreadId()};return true;
}
void StopObserving(DrawObserver* value){
    for(auto& registered:observers)if(registered.value==value)registered={};
    DropUnusedHooks();
}
ObserverSilenceScope::ObserverSilenceScope():previous_(observerSilence){++observerSilence;}
ObserverSilenceScope::~ObserverSilenceScope(){observerSilence=previous_;}
Session::Session(MMDApp& app,bool selectedOnly):impl_(new Impl(app,selectedOnly)){}
Session::~Session()=default;
void Session::BeginFrame(){impl_->meshes.clear();impl_->camera=false;impl_->collecting=true;}
const std::vector<Mesh>& Session::EndFrame(){impl_->collecting=false;return impl_->meshes;}
const std::string& Session::Error()const{return impl_->error;}
MaterialScope::MaterialScope(const void* object,unsigned material,bool enabled,bool opaqueCoverage)
    :oldObject_(sourceObject),oldMaterial_(sourceMaterial),oldOpaqueCoverage_(sourceOpaqueCoverage) {
    sourceObject=enabled?object:nullptr;sourceMaterial=material;sourceOpaqueCoverage=enabled&&opaqueCoverage;
}
MaterialScope::~MaterialScope(){sourceObject=oldObject_;sourceMaterial=oldMaterial_;sourceOpaqueCoverage=oldOpaqueCoverage_;}
bool OpaqueCoverage(){return sourceOpaqueCoverage;}
bool Active(){return active!=nullptr;}
void SetCamera(IDirect3DDevice9* device,const float world[16]) {
    if(!active||!active->collecting)return;
    d3dx::D3DXMATRIXF view{},projection{},wv{},wvp{};auto& dx=d3dx::Get();
    device->GetTransform(D3DTS_VIEW,reinterpret_cast<D3DMATRIX*>(&view));device->GetTransform(D3DTS_PROJECTION,reinterpret_cast<D3DMATRIX*>(&projection));
    dx.multiply(&wv,reinterpret_cast<const d3dx::D3DXMATRIXF*>(world),&view);dx.multiply(&wvp,&wv,&projection);
    active->camera=dx.inverse(&active->inverse,nullptr,&wvp)!=nullptr;
}
}
