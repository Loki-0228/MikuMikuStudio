// Native mesh capture and Alembic cache playback. No plugin loader or SDK.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/d3dx_mesh.hpp"
#include "mikumikustudio/mesh_bake_cache.hpp"
#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/text_encoding.hpp"
extern "C" float* ExpGetAcsWorldMat(float*,int);
namespace mikumikustudio {float ModelOutlineDistance(MMDApp*,unsigned char*,const float[16]);}
namespace mikumikustudio::native_mesh {
namespace {
using Microsoft::WRL::ComPtr;
MMDApp* host=nullptr;
bool bakeCapture=false,bakedPlaybackBlocked=false,restoreBakedEffects=false;
uint64_t generation=0;
std::vector<deformed_mesh_capture::Mesh> bakeMeshes;
std::string bakeError;
std::vector<std::filesystem::path> effectSnapshots;
std::unordered_map<const void*,uint64_t> generations;
void Log(void*,unsigned,const char* text){runtime_log::Write("Native mesh: %s",text?text:"");}
void Redraw(void*){if(host&&host->MainWindow())InvalidateRect(host->MainWindow(),nullptr,FALSE);}
struct Snapshot {mesh_data::Object object;mesh_data::Mesh mesh;
 mesh_data::Object View(){object.mesh=mesh.View();return object;}};
void Check(HRESULT hr,const char* action){if(FAILED(hr))throw std::runtime_error(action);}
std::string Utf8(const wchar_t* text) {
    if(!text||!*text)return {};
    const int n=WideCharToMultiByte(CP_UTF8,0,text,-1,nullptr,0,nullptr,nullptr);
    std::string s(size_t(n),0);WideCharToMultiByte(CP_UTF8,0,text,-1,s.data(),n,nullptr,nullptr);s.pop_back();return s;
}
std::wstring Wide(const char* text) {
    if(!text||!*text)return {};
    const int n=MultiByteToWideChar(CP_UTF8,0,text,-1,nullptr,0);
    std::wstring s(size_t(n),0);MultiByteToWideChar(CP_UTF8,0,text,-1,s.data(),n);s.pop_back();return s;
}
struct Proxy {
    const void* source=nullptr;
    uint64_t generation=0;
    bool active=false,accessory=false;
    std::unique_ptr<mdl::ModelRecord> model;
    std::vector<std::unique_ptr<mdl::ModelRecord>> retiredModels;
    std::unique_ptr<mdl::AccessoryRecord> acs;
    std::vector<std::unique_ptr<mdl::AccessoryRecord>> retiredAccessories;
    std::vector<mdl::ModelMaterialRecord> materials;
    std::vector<mdl::MaterialMorphPool> morphBase,morphAdd,morphMul;
    std::vector<D3DMATERIAL9> acsMaterials;
    std::vector<std::array<char,2048>> acsPaths;
    std::vector<uint8_t> acsTypes;
    std::vector<uint32_t> indices;
    std::vector<std::wstring> names,namesEn;
    std::vector<std::string> keys;
    std::set<std::wstring> texturePaths;
    ComPtr<IDirect3DVertexBuffer9> vb,edge;
    ComPtr<IDirect3DIndexBuffer9> ib;
    std::vector<ComPtr<IUnknown>> registrationAnchors;
    void* mesh=nullptr;
    uint32_t vertexCapacity=0,triangleCapacity=0;
    ~Proxy(){if(mesh)static_cast<IUnknown*>(mesh)->Release();}
};
std::unordered_map<const void*,std::unique_ptr<Proxy>> proxies;
uint64_t Generation(const void* p){auto& g=generations[p];if(!g)g=++generation;return g;}
void ReadVertices(IDirect3DVertexBuffer9* vb,uint32_t count,uint32_t stride,std::vector<mesh_data::Vertex>& out) {
    D3DVERTEXBUFFER_DESC desc{};Check(vb->GetDesc(&desc),"Get vertex buffer description");
    if(uint64_t(count)*stride>desc.Size||stride<32||stride>96)throw std::runtime_error("Invalid source vertex range");
    void* data=nullptr;Check(vb->Lock(0,0,&data,D3DLOCK_READONLY),"Read source vertices");
    out.resize(count);
    for(uint32_t i=0;i<count;++i){auto& v=out[i];memcpy(v.position,static_cast<char*>(data)+size_t(i)*stride,stride);v.sourceVertex=i;v.edgeScale=1;}
    vb->Unlock();
}
void ReadTriangles(IDirect3DIndexBuffer9* ib,uint32_t count,std::vector<mesh_data::Triangle>& out) {
    D3DINDEXBUFFER_DESC d{};Check(ib->GetDesc(&d),"Get index buffer description");
    const uint32_t stride=d.Format==D3DFMT_INDEX32?4:2;
    if(count%3||uint64_t(count)*stride>d.Size)throw std::runtime_error("Invalid source index range");
    void* data=nullptr;Check(ib->Lock(0,0,&data,D3DLOCK_READONLY),"Read source indices");out.resize(count/3);
    for(uint32_t i=0;i<count;++i)out[i/3].vertex[i%3]=stride==4?static_cast<uint32_t*>(data)[i]:static_cast<uint16_t*>(data)[i];
    ib->Unlock();
}
std::unique_ptr<Snapshot> ModelSnapshot(unsigned char* source) {
 auto& m=*mdl::Mdl(source);auto s=std::make_unique<Snapshot>();auto& o=s->object;
 o.id=reinterpret_cast<uintptr_t>(source);o.generation=Generation(source);o.visible=m.loadComplete!=0;mesh_data::Identity(o.world);
 const bool pmx=m.physicsMode==2;const unsigned uv=pmx?m.pmxAdditionalUvCount:0;
 if(uv>4)throw std::runtime_error("Invalid native additional UV count");
 ReadVertices(static_cast<IDirect3DVertexBuffer9*>(m.vertexBuffer),m.vertexCount,32+16*uv,s->mesh.vertices);
 for(unsigned i=0;i<m.vertexCount;++i){auto& v=s->mesh.vertices[i];if(pmx){v.edgeScale=m.pmxVertices[i].edgeScale;v.flags=m.pmxVertices[i].hasPmxEdgeData?0:mesh_data::kEdgeDisabled;}else {v.edgeScale=m.rawVertices[i].edgeDisabled?0.f:1.f;v.flags=m.rawVertices[i].edgeDisabled?mesh_data::kEdgeDisabled:0;}}
 if(m.indexCount%3||!m.indices)throw std::runtime_error("Invalid native topology");
 s->mesh.triangles.resize(m.indexCount/3);
 for(unsigned i=0;i<m.indexCount;++i)s->mesh.triangles[i/3].vertex[i%3]=pmx?static_cast<const uint32_t*>(m.indices)[i]:static_cast<const uint16_t*>(m.indices)[2*i];
 s->mesh.materials.resize(m.materialCount);unsigned first=0;
 for(unsigned i=0;i<m.materialCount;++i){const auto& a=static_cast<const mdl::ModelMaterialRecord*>(m.materials)[i];auto& b=s->mesh.materials[i];
 b.key="source:"+std::to_string(i);b.name=b.key;b.nameEn=b.name;b.value.sourceMaterial=i;
 memcpy(b.value.diffuse,a.diffuse,16);memcpy(b.value.ambient,a.ambient,12);memcpy(b.value.specular,a.specular,12);b.value.power=a.specularPower;
 memcpy(b.value.edgeColor,a.edgeColor,16);b.value.edgeSize=a.edgeSize;b.value.flags=pmx?a.flags:(a.doubleSided?1u:0u);b.value.sphereMode=a.sphereMode;
 b.texture.path=a.texturePath;b.sphere.path=a.spherePath;b.toon.path=a.toonPath;
 if(a.faceVertexCount<0||a.faceVertexCount%3||uint64_t(first)+a.faceVertexCount/3>s->mesh.triangles.size())throw std::runtime_error("Invalid native material range");
 for(unsigned j=0;j<unsigned(a.faceVertexCount)/3;++j)s->mesh.triangles[first++].material=i;
 }
 return s;
}
std::unique_ptr<Snapshot> AccessorySnapshot(mdl::AccessoryRecord* a) {
 auto s=std::make_unique<Snapshot>();auto& o=s->object;o.kind=1;o.visible=a->visible;o.id=reinterpret_cast<uintptr_t>(a);o.generation=Generation(a);
 mesh_data::Identity(o.world);int ordinal=0;for(int i=0;i<255;++i){if(!host->AccessorySlot(i))continue;if(host->AccessorySlot(i)==a){ExpGetAcsWorldMat(o.world,ordinal);break;}++ordinal;}
    void* clone=nullptr;Check(d3dx::CloneMeshFvf(a->mesh,0x220|0x1,D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1,host->Renderer()->device,&clone),"Clone accessory snapshot");
    ComPtr<IUnknown> owner;owner.Attach(static_cast<IUnknown*>(clone));
    const auto vertices=d3dx::MeshMethod<DWORD(WINAPI*)(void*),5>(clone)(clone);
    const auto faces=d3dx::MeshMethod<DWORD(WINAPI*)(void*),4>(clone)(clone);
    ComPtr<IDirect3DVertexBuffer9> vb;ComPtr<IDirect3DIndexBuffer9> ib;Check(d3dx::MeshVertexBuffer(clone,&vb),"Accessory VB");Check(d3dx::MeshIndexBuffer(clone,&ib),"Accessory IB");
    ReadVertices(vb.Get(),vertices,32,s->mesh.vertices);
    ReadTriangles(ib.Get(),faces*3,s->mesh.triangles);
    if(true){DWORD* attributes=nullptr;Check(d3dx::MeshMethod<HRESULT(WINAPI*)(void*,DWORD,DWORD**),24>(clone)(clone,D3DLOCK_READONLY,&attributes),"Accessory attributes");
        for(uint32_t i=0;i<faces;++i)s->mesh.triangles[i].material=attributes[i];d3dx::MeshMethod<HRESULT(WINAPI*)(void*),25>(clone)(clone);}
    {s->mesh.materials.resize(a->materialCount);
        for(uint32_t i=0;i<a->materialCount;++i){auto& b=s->mesh.materials[i];const auto& m=static_cast<const D3DMATERIAL9*>(a->materials)[i];b.key="source:"+std::to_string(i);b.name="subset"+std::to_string(i);b.value.sourceMaterial=i;
            memcpy(b.value.diffuse,&m.Diffuse,16);memcpy(b.value.ambient,&m.Ambient,12);memcpy(b.value.specular,&m.Specular,12);b.value.power=m.Power;
            if(a->texturePaths){const auto* p=reinterpret_cast<const wchar_t*>(a->texturePaths[i]);b.texture.path=p;b.sphere.path=p+512;}
        }
    }
    return s;
}
class RegistrationSceneState {
    IDirect3DDevice9* device;
    ComPtr<IDirect3DStateBlock9> state;
    std::array<ComPtr<IDirect3DSurface9>,4> targets;
    ComPtr<IDirect3DSurface9> originalDepth,sceneDepth;
    D3DVIEWPORT9 viewport{};
    DWORD targetCount=0;
public:
    explicit RegistrationSceneState(IDirect3DDevice9* d):device(d){
        D3DCAPS9 caps{};Check(device->GetDeviceCaps(&caps),"MME registration device caps");
        targetCount=std::min<DWORD>(DWORD(targets.size()),caps.NumSimultaneousRTs);
        Check(device->CreateStateBlock(D3DSBT_ALL,&state),"MME registration state capture");
        Check(device->GetViewport(&viewport),"MME registration viewport capture");
        for(DWORD i=0;i<targetCount;++i){
            const HRESULT hr=device->GetRenderTarget(i,&targets[i]);
            if(i==0||hr!=D3DERR_NOTFOUND)Check(hr,"MME registration render target capture");
        }
        const HRESULT hr=device->GetDepthStencilSurface(&originalDepth);
        if(hr!=D3DERR_NOTFOUND)Check(hr,"MME registration depth capture");
        sceneDepth=originalDepth;
        if(!sceneDepth&&host&&host->Renderer())sceneDepth=host->Renderer()->depthStencilSurface;
        if(!sceneDepth)throw std::runtime_error("MME registration requires an available scene depth surface");
        D3DSURFACE_DESC color{},depth{};
        Check(targets[0]->GetDesc(&color),"MME registration color description");
        Check(sceneDepth->GetDesc(&depth),"MME registration depth description");
        if(depth.Width<color.Width||depth.Height<color.Height||depth.MultiSampleType!=color.MultiSampleType||depth.MultiSampleQuality!=color.MultiSampleQuality){
            sceneDepth.Reset();
            Check(device->CreateDepthStencilSurface(color.Width,color.Height,depth.Format,color.MultiSampleType,color.MultiSampleQuality,TRUE,&sceneDepth,nullptr),"MME registration compatible depth allocation");
        }
    }
    ~RegistrationSceneState(){
        device->SetDepthStencilSurface(nullptr);
        for(DWORD i=1;i<targetCount;++i)device->SetRenderTarget(i,nullptr);
        device->SetRenderTarget(0,targets[0].Get());
        for(DWORD i=1;i<targetCount;++i)device->SetRenderTarget(i,targets[i].Get());
        device->SetDepthStencilSurface(originalDepth.Get());
        state->Apply();device->SetViewport(&viewport);
    }
    void Prepare(){
        Check(device->SetDepthStencilSurface(nullptr),"MME registration unbind depth");
        for(DWORD i=1;i<targetCount;++i)Check(device->SetRenderTarget(i,nullptr),"MME registration unbind MRT");
        Check(device->SetRenderTarget(0,targets[0].Get()),"MME registration bind color");
        for(DWORD i=1;i<targetCount;++i)Check(device->SetRenderTarget(i,targets[i].Get()),"MME registration restore MRT");
        Check(device->SetDepthStencilSurface(sceneDepth.Get()),"MME registration bind depth");
        Check(state->Apply(),"MME registration restore state");
        Check(device->SetViewport(&viewport),"MME registration restore viewport");
    }
};
void DrainEffectRegistration(IDirect3DDevice9* device){
    RegistrationSceneState state(device);
    struct RestorePass {
        int count=host->state.renderPassCount,pass=host->state.activeRenderPass;
        void* object=host->state.activeRenderObject;
        ~RestorePass(){host->state.renderPassCount=count;host->state.activeRenderPass=pass;host->state.activeRenderObject=object;}
    } restorePass;
    D3DMATRIX world{};Check(device->GetTransform(D3DTS_WORLD,&world),"MME registration world transform");
    for(int i=0;i<4;++i){
        state.Prepare();
        Check(device->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,
            host->state.blackBackgroundEnabled?D3DCOLOR_XRGB(0,0,0):D3DCOLOR_XRGB(255,255,255),1.f,0),"MME registration Clear");
        host->state.renderPassCount=1;
        BeginEffectObjectRegistration();
        Check(device->BeginScene(),"MME registration BeginScene");
        // MME may request several passes through ExpSetRenderRepeatCount.
        // An empty BeginScene/EndScene skips its offscreen-to-main transition;
        // postprocessing then dereferences an uninitialized depth surface.
        // Consume the same pass contract as RenderFrameScene, without advancing
        // animation, physics or Native mesh evaluation, and without presenting a frame.
        RenderShadowMap(host,reinterpret_cast<const float*>(&world));
        while(host->state.renderPassCount>0){
            --host->state.renderPassCount;
            RenderModelsEffect(host,reinterpret_cast<const float*>(&world));
        }
        Check(device->EndScene(),"MME registration EndScene");
    }
}
std::filesystem::path PreserveEffectAssignments(IDirect3DDevice9* device){
    if(!GetModuleHandleW(L"MMEffect.dll"))return {};
    const UINT state=GetMenuState(GetMenu(host->MainWindow()),40022,MF_BYCOMMAND);
    if(state==UINT(-1))throw std::runtime_error("Native mesh cannot preserve the current MME assignments: autosave command unavailable");
    wchar_t temporary[MAX_PATH+1]{};if(!GetTempPathW(MAX_PATH,temporary))throw std::runtime_error("Native mesh temporary directory unavailable");
    std::filesystem::path directoryPath;
    for(uint32_t attempt=0;;++attempt){directoryPath=std::filesystem::path(temporary)/(L"MMS-Mesh-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(attempt));
        if(CreateDirectoryW(directoryPath.c_str(),nullptr))break;if(GetLastError()!=ERROR_ALREADY_EXISTS)throw std::runtime_error("Native mesh cannot create an MME assignment snapshot directory");}
    const auto pmm=directoryPath/L"assignments.pmm";const auto emm=directoryPath/L"assignments.emm";
    {std::ofstream placeholder(pmm,std::ios::binary);placeholder<<"Native mesh assignment transfer\n";if(!placeholder)throw std::runtime_error("Native mesh cannot create an MME assignment snapshot");}
    // Use the host's existing, publicly observed PMM file notification and
    // resource-registration path. No MMEffect private exports or offsets.
    // Only this new temporary EMM is written; the user's EMM is never replaced.
    const bool enable=(state&MF_CHECKED)==0;
    if(enable)SendMessageW(host->MainWindow(),WM_COMMAND,40022,0);
    struct Restore{bool enabled;~Restore(){if(enabled)SendMessageW(host->MainWindow(),WM_COMMAND,40022,0);}}restore{enable};
    NotifyEffectFileSave(pmm.c_str());
    DrainEffectRegistration(device);
    if(GetFileAttributesW(emm.c_str())==INVALID_FILE_ATTRIBUTES)throw std::runtime_error("MME did not save its assignment snapshot; material replacement was skipped");
    effectSnapshots.push_back(directoryPath);return pmm;
}
void FinishRegistration(IDirect3DDevice9* device,const std::filesystem::path& saved){
    if(!saved.empty()){
        NotifyEffectFileOpen(saved.c_str());
        DrainEffectRegistration(device);
    }
    // Old record identities must remain alive until MME has consumed the new
    // object table. They are then released, not retained for every hot reload.
    for(auto& item:proxies){auto& proxy=*item.second;
        if(proxy.accessory&&!proxy.registrationAnchors.empty()){
            auto current=std::move(proxy.registrationAnchors.back());proxy.registrationAnchors.clear();proxy.registrationAnchors.push_back(std::move(current));
        }else if(!proxy.accessory)proxy.registrationAnchors.clear();
        proxy.retiredModels.clear();proxy.retiredAccessories.clear();
    }
}
}

void Initialize(MMDApp* app){host=app;}
void ClearScene(){mesh_bake_cache::Clear();proxies.clear();generations.clear();restoreBakedEffects=false;bakedPlaybackBlocked=false;}
void Shutdown(){ClearScene();host=nullptr;}
void Forget(const void* object){mesh_bake_cache::Forget(object);proxies.erase(object);generations.erase(object);}
void ProjectOpened(MMDApp* app,const wchar_t*){Initialize(app);restoreBakedEffects=!mesh_bake_cache::Empty();bakedPlaybackBlocked=false;}
void DeviceLost(){proxies.clear();}
bool DisableMeshEffects(MMDApp* app,const std::vector<const void*>& objects,std::string& error){
    std::filesystem::path saved;
    bool mmeChanged=false;
    try{
        if(GetModuleHandleW(L"MMEffect.dll")){
            saved=PreserveEffectAssignments(app->Renderer()->device);
            auto original=saved;original.replace_extension(L".emm");
            const auto edited=saved.parent_path()/L"baked.emm",project=saved.parent_path()/L"baked.pmm";
            std::filesystem::copy_file(original,edited,std::filesystem::copy_options::overwrite_existing);
            {std::ofstream placeholder(project);placeholder<<"Baked effect assignments";}
            const auto readSection=[&](const wchar_t* section){
                for(size_t size=4096;size<=4*1024*1024;size*=2){std::vector<wchar_t> data(size);
                    const DWORD count=section?GetPrivateProfileSectionW(section,data.data(),DWORD(size),edited.c_str()):GetPrivateProfileSectionNamesW(data.data(),DWORD(size),edited.c_str());
                    if(count<size-2)return data;}
                throw std::runtime_error("MME assignments exceed the supported size");
            };
            auto entries=readSection(L"Object"),sections=readSection(nullptr);
            std::set<std::wstring> disabled;std::set<const void*> matched;
            const auto samePath=[](const std::wstring& value,const wchar_t* path){std::error_code ec;return std::filesystem::equivalent(value,path,ec)&&!ec;};
            for(auto* entry=entries.data();*entry;entry+=wcslen(entry)+1){
                std::wstring line(entry);const auto equals=line.find(L'=');if(equals==std::wstring::npos)continue;
                auto key=line.substr(0,equals),value=line.substr(equals+1);while(!key.empty()&&iswspace(key.back()))key.pop_back();while(!value.empty()&&iswspace(value.front()))value.erase(value.begin());
                if(value.size()>1&&value.front()==L'"'&&value.back()==L'"')value=value.substr(1,value.size()-2);
                const void* source=nullptr;
                if(key.rfind(L"Pmd",0)==0)for(int i=0;i<kModelSlotCount;++i){auto* p=app->ModelSlot(i);if(p&&!matched.count(p)&&samePath(value,mdl::Mdl(p)->path)){source=p;break;}}
                if(key.rfind(L"Acs",0)==0)for(int i=0;i<255;++i){auto* p=app->AccessorySlot(i);if(p&&!matched.count(p)&&samePath(value,p->sourcePath)){source=p;break;}}
                if(source){matched.insert(source);if(std::find(objects.begin(),objects.end(),source)!=objects.end())disabled.insert(key);}
            }
            for(const auto* object:objects)if(!matched.count(object))throw std::runtime_error("Cannot match a baked object to its MME assignments");
            for(auto* section=sections.data();*section;section+=wcslen(section)+1){
                if(wcsncmp(section,L"Effect",6))continue;
                auto assignments=readSection(section);
                for(auto* entry=assignments.data();*entry;entry+=wcslen(entry)+1){std::wstring key(entry);key=key.substr(0,key.find(L'='));while(!key.empty()&&iswspace(key.back()))key.pop_back();
                    for(const auto& object:disabled)if(key==object||(key.rfind(object+L"[",0)==0&&key.back()==L']')){if(!WritePrivateProfileStringW(section,key.c_str(),L"none",edited.c_str()))throw std::runtime_error("Cannot disable baked mesh MME assignments");break;}}
            }
            for(const auto& key:disabled)if(!WritePrivateProfileStringW(L"Effect",key.c_str(),L"none",edited.c_str()))throw std::runtime_error("Cannot write disabled MME assignments");
            // The documented cache-flush form returns zero even on success.
            WritePrivateProfileStringW(nullptr,nullptr,nullptr,edited.c_str());
            mmeChanged=true;
            FinishRegistration(app->Renderer()->device,project);
        }
        bakedPlaybackBlocked=false;return true;
    }catch(const std::exception& e){
        error=e.what();
        if(mmeChanged)try{FinishRegistration(app->Renderer()->device,saved);}catch(...){error+="; restoring the original MME assignments also failed";app->SceneModified()=1;}
        return false;
    }
}
bool KeyBakedEffects(MMDApp*,const std::vector<const void*>&,int,int,std::string& error){error.clear();return true;}
void ClearMeshBakeObjects(MMDApp* app,const std::vector<const void*>& objects){
    if(!app)return;
    try{
        bool changed=false;for(const auto* source:objects)changed|=source&&mesh_bake_cache::Has(source);
        if(!changed)return;
        auto* device=app->Renderer()?app->Renderer()->device:nullptr;
        const auto saved=device?PreserveEffectAssignments(device):std::filesystem::path{};
        for(const auto* source:objects)if(source&&mesh_bake_cache::Has(source)){mesh_bake_cache::Forget(source);proxies.erase(source);}
        if(device)FinishRegistration(device,saved);
        app->SceneModified()=1;Redraw(nullptr);
    }catch(const std::exception& e){Log(nullptr,2,e.what());}
}
void ClearMeshBake(MMDApp* app,bool selectedOnly){
    std::vector<const void*> objects;
    for(int i=0;i<kModelSlotCount;++i)if(auto* source=app->ModelSlot(i))if(!selectedOnly||source==app->SelectedModel())objects.push_back(source);
    if(!selectedOnly)for(int i=0;i<255;++i)if(auto* source=app->AccessorySlot(i))objects.push_back(source);
    ClearMeshBakeObjects(app,objects);
}
unsigned char* RenderModel(unsigned char* source){
    // Ordered render passes revisit empty slots for every possible draw order.
    // Neither an empty slot nor a scene without cache proxies needs hashing.
    if(!source||proxies.empty())return source;
    auto it=proxies.find(source);
    return it!=proxies.end()&&it->second->active&&it->second->model
        ?reinterpret_cast<unsigned char*>(it->second->model.get()):source;
}
mdl::AccessoryRecord* RenderAccessory(mdl::AccessoryRecord* source){
    if(!source||proxies.empty())return source;
    auto it=proxies.find(source);
    return it!=proxies.end()&&it->second->active&&it->second->accessory
        ?it->second->acs.get():source;
}
const void* SourceObject(const void* p){for(const auto& x:proxies)if(x.second->model.get()==p||x.second->acs.get()==p)return x.first;return p;}
const wchar_t* MaterialName(uint32_t id,int material,bool english){for(auto& x:proxies){auto& p=*x.second;if(!p.active)continue;const auto address=p.accessory?reinterpret_cast<uintptr_t>(p.acs.get()):reinterpret_cast<uintptr_t>(p.model.get());
    if(uint32_t(address)==id&&material>=0&&size_t(material)<p.names.size()&&!p.names[material].empty())return (english&&!p.namesEn[material].empty()?p.namesEn:p.names)[material].c_str();}return nullptr;}
const std::int32_t* Indices32(unsigned char* model){for(auto& x:proxies)if(x.second->model.get()==reinterpret_cast<mdl::ModelRecord*>(model))return reinterpret_cast<const int32_t*>(x.second->indices.data());return mdl::Mdl(model)->physicsMode==2?mdl::PmxIndices(model):nullptr;}
void* Texture(const wchar_t*){return nullptr;}
bool TextureProperties(const wchar_t*,float*,bool*){return false;}
void* AccessoryMesh(void* accessory){for(auto& x:proxies)if(x.second->active&&x.second->acs.get()==accessory)return x.second->mesh;return mdl::Accessory(accessory)->mesh;}
uint32_t Color(const float* c,bool opaque=false){auto cv=[](float f){return uint32_t(std::clamp(f,0.f,1.f)*255.f);};return ((opaque?255:cv(c[3]))<<24)|(cv(c[0])<<16)|(cv(c[1])<<8)|cv(c[2]);}
std::wstring PrepareTexture(const mesh_data::TextureView& t,uint64_t,unsigned,int){return t.path?t.path:L"";}
template<size_t N>void PathCopy(wchar_t(&to)[N],const std::wstring& from){if(from.size()>=N)throw std::runtime_error("Native mesh texture path exceeds host material capacity");wcscpy_s(to,from.c_str());}
void Upload(Proxy& p,const mesh_data::ObjectOutput& out,const mesh_data::Object& input,const float* world) {
    const auto& mesh=out.mesh;mesh_data::Validate(mesh);
    const auto sourceMaterials=p.accessory?static_cast<const mdl::AccessoryRecord*>(p.source)->materialCount:static_cast<const mdl::ModelRecord*>(p.source)->materialCount;
    if(mesh.materialCount<sourceMaterials)throw std::runtime_error("Native mesh cannot remove source material slots");
    std::vector<std::string> keys;std::vector<uint32_t> counts(mesh.materialCount),vertexMaterial(mesh.vertexCount);
    for(uint32_t i=0;i<mesh.materialCount;++i){const auto& a=mesh.materials[i];keys.emplace_back(a.key);
        if(i<sourceMaterials&&(a.sourceMaterial!=int32_t(i)||keys.back()!="source:"+std::to_string(i)))throw std::runtime_error("Native mesh changed a source material identity");
        if(std::count(keys.begin(),keys.end(),keys.back())!=1)throw std::runtime_error("Duplicate Native mesh material identity");}
    p.indices.clear();p.indices.reserve(size_t(mesh.triangleCount)*3);
    for(uint32_t material=0;material<mesh.materialCount;++material)for(uint32_t t=0;t<mesh.triangleCount;++t)if(mesh.triangles[t].material==material){
        for(auto v:mesh.triangles[t].vertex){p.indices.push_back(v);vertexMaterial[v]=material;}counts[material]+=3;}
    const bool structureChanged=p.keys!=keys;
    p.names.resize(mesh.materialCount);p.namesEn.resize(mesh.materialCount);
    std::vector<std::array<std::wstring,3>> paths(mesh.materialCount);
    for(uint32_t i=0;i<mesh.materialCount;++i){const auto& a=mesh.materials[i];p.names[i]=i<sourceMaterials?L"":Wide(a.name);p.namesEn[i]=i<sourceMaterials?L"":Wide(a.nameEn);
        paths[i]={PrepareTexture(a.texture,out.objectId,i,0),PrepareTexture(a.sphere,out.objectId,i,1),PrepareTexture(a.toon,out.objectId,i,2)};}
    auto* device=host->Renderer()->device;
    const uint32_t vc=std::max(3u,std::max(mesh.vertexCount,mesh.vertexCapacity)),tc=std::max(1u,std::max(mesh.triangleCount,mesh.triangleCapacity));
    if(vc>16u*1024*1024||tc>16u*1024*1024)throw std::runtime_error("Native mesh capacity exceeds host limit");
    if(!p.accessory){auto& source=*static_cast<const mdl::ModelRecord*>(p.source);
        if(p.model&&(structureChanged||p.vertexCapacity<vc||p.triangleCapacity<tc))p.retiredModels.push_back(std::move(p.model));
        if(!p.model)p.model=std::make_unique<mdl::ModelRecord>();
        auto& m=*p.model;const auto oldIb=m.indexBuffer;m=source;m.indexBuffer=oldIb;
        const bool pmx=source.physicsMode==2;const uint32_t stride=32+(pmx?source.pmxAdditionalUvCount*16:0);
        if(!p.ib||p.vertexCapacity<vc||p.triangleCapacity<tc||structureChanged){
            // Pass the stable model record's resource slot to the host's hooked
            // CreateIndexBuffer. MMHack uses this documented host layout.
            if(p.ib){ComPtr<IUnknown> anchor;p.ib.As(&anchor);p.registrationAnchors.push_back(std::move(anchor));}
            p.vb.Reset();p.edge.Reset();p.ib.Reset();m.indexBuffer=nullptr;
            Check(device->CreateVertexBuffer(vc*stride,0,0,D3DPOOL_MANAGED,&p.vb,nullptr),"Native mesh main VB");
            Check(device->CreateVertexBuffer(vc*16,0,D3DFVF_XYZ|D3DFVF_DIFFUSE,D3DPOOL_MANAGED,&p.edge,nullptr),"Native mesh edge VB");
            Check(device->CreateIndexBuffer(tc*12,0,D3DFMT_INDEX32,D3DPOOL_MANAGED,reinterpret_cast<IDirect3DIndexBuffer9**>(&m.indexBuffer),nullptr),"Native mesh model registration");
            p.ib.Attach(static_cast<IDirect3DIndexBuffer9*>(m.indexBuffer));p.vertexCapacity=vc;p.triangleCapacity=tc;
        }
        p.materials.resize(mesh.materialCount);for(uint32_t i=0;i<mesh.materialCount;++i){auto& b=p.materials[i];const auto& a=mesh.materials[i];
            if(i<sourceMaterials)b=static_cast<const mdl::ModelMaterialRecord*>(source.materials)[i];else b={};
            memcpy(b.diffuse,a.diffuse,16);memcpy(b.diffuseMirror,a.diffuse,16);memcpy(b.ambient,a.ambient,12);memcpy(b.specular,a.specular,12);b.specularPower=a.power;
            memcpy(b.edgeColor,a.edgeColor,16);b.edgeSize=a.edgeSize;b.flags=uint8_t(a.flags);b.sphereMode=uint8_t(a.sphereMode);b.doubleSided=(a.flags&1)!=0;b.faceVertexCount=out.hidden?0:counts[i];
            PathCopy(b.texturePath,paths[i][0]);PathCopy(b.spherePath,paths[i][1]);PathCopy(b.toonPath,paths[i][2]);}
        m.vertexCount=mesh.vertexCount;m.indexCount=out.hidden?0:mesh.triangleCount*3;m.vertexBuffer=p.vb.Get();m.vertexBuffer2=p.edge.Get();m.indexBuffer=p.ib.Get();m.indices=p.indices.data();m.materials=p.materials.data();m.materialCount=mesh.materialCount;m.pmxVertexIndexSize=4;
        if(pmx){
            // PMX shading indexes these arrays by the render material number.
            // Borrowing the original arrays would read past their end as soon
            // as a mesh pass appends a material.
            p.morphBase.resize(mesh.materialCount);p.morphAdd.resize(mesh.materialCount);p.morphMul.resize(mesh.materialCount);
            for(uint32_t i=0;i<mesh.materialCount;++i){
                p.morphBase[i]={};p.morphAdd[i]={};p.morphMul[i]={};
                auto& mul=p.morphMul[i].channels;
                for(float& v:mul.textureTint)v=1;for(float& v:mul.sphereTint)v=1;for(float& v:mul.toonTint)v=1;
                if(i<sourceMaterials){
                    if(source.materialMorphPools.base)p.morphBase[i]=source.materialMorphPools.base[i];
                    if(source.materialMorphPools.additive)p.morphAdd[i]=source.materialMorphPools.additive[i];
                    if(source.materialMorphPools.multiplicative)p.morphMul[i]=source.materialMorphPools.multiplicative[i];
                }
            }
            m.materialMorphPools={p.morphBase.data(),p.morphAdd.data(),p.morphMul.data()};
        }
        void* data=nullptr;Check(p.vb->Lock(0,0,&data,0),"Native mesh vertex upload");for(uint32_t i=0;i<mesh.vertexCount;++i)memcpy(static_cast<char*>(data)+size_t(i)*stride,mesh.vertices[i].position,stride);p.vb->Unlock();
        std::vector<mdl::EdgeVertex> sourceEdges(source.vertexCount);
        if(source.vertexBuffer2){void* original=nullptr;auto* buffer=static_cast<IDirect3DVertexBuffer9*>(source.vertexBuffer2);Check(buffer->Lock(0,0,&original,D3DLOCK_READONLY),"Native mesh source outline");memcpy(sourceEdges.data(),original,sourceEdges.size()*sizeof(mdl::EdgeVertex));buffer->Unlock();}
        Check(p.edge->Lock(0,0,&data,0),"Native mesh outline upload");auto* edge=static_cast<mdl::EdgeVertex*>(data);const float distance=ModelOutlineDistance(host,reinterpret_cast<unsigned char*>(const_cast<void*>(p.source)),world);
        for(uint32_t i=0;i<mesh.vertexCount;++i){const auto& v=mesh.vertices[i];const auto& mat=mesh.materials[vertexMaterial[i]];
            if(source.vertexBuffer2&&v.sourceVertex<input.mesh.vertexCount&&v.sourceVertex<source.vertexCount){const auto& before=input.mesh.vertices[v.sourceVertex];const auto oldMaterial=pmx?source.pmxVertices[v.sourceVertex].materialIndex:0;
                if(!memcmp(v.position,before.position,24)&&v.edgeScale==before.edgeScale&&v.flags==before.flags&&(!pmx||(oldMaterial>=0&&uint32_t(oldMaterial)<input.mesh.materialCount&&!memcmp(mat.edgeColor,input.mesh.materials[oldMaterial].edgeColor,16)&&mat.edgeSize==input.mesh.materials[oldMaterial].edgeSize))){edge[i]=sourceEdges[v.sourceVertex];continue;}}
            for(int c=0;c<3;++c){if(v.flags&mesh_data::kEdgeDisabled)edge[i].position[c]=v.position[c]-v.normal[c]*.005f;
                else if(pmx){float amount=distance*v.normal[c];amount*=v.edgeScale;amount*=mat.edgeSize;edge[i].position[c]=v.position[c]+amount;}
                else edge[i].position[c]=v.position[c]+v.normal[c]*distance*v.edgeScale;}
            edge[i].diffuse=pmx?Color(mat.edgeColor,host->state.selfShadowEnabled!=0):0xff000000;}
        p.edge->Unlock();Check(p.ib->Lock(0,0,&data,0),"Native mesh index upload");if(!p.indices.empty())memcpy(data,p.indices.data(),p.indices.size()*4);p.ib->Unlock();
    }else{auto& a=*static_cast<const mdl::AccessoryRecord*>(p.source);
        if(structureChanged&&p.acs)p.retiredAccessories.push_back(std::move(p.acs));
        if(!p.acs)p.acs=std::make_unique<mdl::AccessoryRecord>();
        auto& acs=*p.acs;acs=a;acs.mesh=p.mesh;
        if(!p.mesh||p.vertexCapacity!=mesh.vertexCount||p.triangleCapacity!=mesh.triangleCount||structureChanged){
            auto& dx=d3dx::Get();if(!dx.Load())throw std::runtime_error("D3DX mesh unavailable");
            if(p.registrationAnchors.empty()||structureChanged){
                // Match the existing accessory resource registration entry point.
                void* adjacency=nullptr;void* materials=nullptr;DWORD n=0;acs.mesh=nullptr;
                p.active=true;acs.materialCount=mesh.materialCount;
                const auto hr=dx.loadMeshFromXW(a.sourcePath,0x220,device,&adjacency,&materials,nullptr,&n,&acs.mesh);
                p.active=false;
                if(adjacency)static_cast<IUnknown*>(adjacency)->Release();if(materials)static_cast<IUnknown*>(materials)->Release();Check(hr,"Native mesh accessory registration");
                ComPtr<IUnknown> anchor;anchor.Attach(static_cast<IUnknown*>(acs.mesh));p.registrationAnchors.push_back(std::move(anchor));}
            using Create=HRESULT(WINAPI*)(DWORD,DWORD,DWORD,DWORD,IDirect3DDevice9*,void**);
            auto create=reinterpret_cast<Create>(GetProcAddress(dx.module,"D3DXCreateMeshFVF"));if(!create)throw std::runtime_error("D3DXCreateMeshFVF unavailable");
            void* next=nullptr;Check(create(std::max(1u,mesh.triangleCount),std::max(3u,mesh.vertexCount),0x221,D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1,device,&next),"Native mesh accessory mesh");
            if(p.mesh)static_cast<IUnknown*>(p.mesh)->Release();p.mesh=next;p.vertexCapacity=mesh.vertexCount;p.triangleCapacity=mesh.triangleCount;}
        acs.mesh=p.registrationAnchors.back().Get();acs.materialCount=mesh.materialCount;acs.visible=out.hidden?0:a.visible;
        p.acsMaterials.resize(mesh.materialCount);p.acsPaths.resize(mesh.materialCount);p.acsTypes.resize(mesh.materialCount);
        for(uint32_t i=0;i<mesh.materialCount;++i){const auto& v=mesh.materials[i];auto& m=p.acsMaterials[i];
            // Preserve accessory-only values (emission and unused colour alpha
            // components) when the Native mesh material still refers to a source slot.
            m=i<sourceMaterials?static_cast<const D3DMATERIAL9*>(a.materials)[i]:D3DMATERIAL9{};
            memcpy(&m.Diffuse,v.diffuse,16);memcpy(&m.Ambient,v.ambient,12);memcpy(&m.Specular,v.specular,12);m.Power=v.power;
            auto* path=reinterpret_cast<wchar_t*>(p.acsPaths[i].data());if(paths[i][0].size()>=512||paths[i][1].size()>=512)throw std::runtime_error("Native mesh accessory texture path too long");wcscpy_s(path,512,paths[i][0].c_str());wcscpy_s(path+512,512,paths[i][1].c_str());p.acsTypes[i]=i<sourceMaterials&&a.textureTypes?a.textureTypes[i]:0;}
        acs.materials=p.acsMaterials.data();acs.texturePaths=reinterpret_cast<char(*)[2048]>(p.acsPaths.data());acs.textureTypes=p.acsTypes.data();
        ComPtr<IDirect3DVertexBuffer9> vb;ComPtr<IDirect3DIndexBuffer9> ib;Check(d3dx::MeshVertexBuffer(p.mesh,&vb),"Native mesh accessory VB");Check(d3dx::MeshIndexBuffer(p.mesh,&ib),"Native mesh accessory IB");
        void* data=nullptr;Check(vb->Lock(0,0,&data,0),"Native mesh accessory vertex upload");for(uint32_t i=0;i<mesh.vertexCount;++i)memcpy(static_cast<char*>(data)+i*32,mesh.vertices[i].position,32);vb->Unlock();
        Check(ib->Lock(0,0,&data,0),"Native mesh accessory index upload");if(!p.indices.empty())memcpy(data,p.indices.data(),p.indices.size()*4);else memset(data,0,12);ib->Unlock();
        DWORD* attrs=nullptr;Check(d3dx::MeshMethod<HRESULT(WINAPI*)(void*,DWORD,DWORD**),24>(p.mesh)(p.mesh,0,&attrs),"Native mesh accessory groups");uint32_t k=0;for(uint32_t i=0;i<mesh.materialCount;++i)for(uint32_t j=0;j<counts[i]/3;++j)attrs[k++]=i;if(!k)attrs[0]=0;d3dx::MeshMethod<HRESULT(WINAPI*)(void*),25>(p.mesh)(p.mesh);
        if(!mesh.triangleCount)acs.visible=0;
    }
    p.keys=std::move(keys);p.active=true;
}
void ApplyBakedMesh(Snapshot& snapshot,double seconds) {
    const auto* source=reinterpret_cast<const void*>(uintptr_t(snapshot.object.id));
    if(bakedPlaybackBlocked||!mesh_bake_cache::ActiveAt(source,seconds))return;
    // Layer-specific archives need the removed plugin and cannot play as a native cache.
    mesh_bake_cache::EntryInfo info;
    if(mesh_bake_cache::Info(source,info)&&info.cutoffLayer!=mesh_bake_cache::kNoCutoffLayer)throw std::runtime_error("Layer-specific mesh caches are unavailable in MMS");
    std::vector<deformed_mesh_capture::Mesh> samples;std::string error;
    if(!mesh_bake_cache::Sample(source,seconds,samples,error))throw std::runtime_error(error);
    auto& mesh=snapshot.mesh;
    const bool patch=std::any_of(samples.begin(),samples.end(),[](const auto& m){return m.vertexPatch;});
    // Validate every source row before changing this snapshot. Missing maps in
    // old complete archives are unknown, not an implied identity mapping.
    for(const auto& sample:samples){
        if(patch&&!sample.vertexPatch)throw std::runtime_error("Invalid source-vertex cache patch");
        if((patch||!sample.sourceVertices.empty())&&sample.sourceVertices.size()!=sample.positions.size())
            throw std::runtime_error("Invalid source-vertex map in baked mesh");
        for(const auto index:sample.sourceVertices)
            if(index!=UINT32_MAX&&index>=mesh.vertices.size())
                throw std::runtime_error("Baked vertex no longer exists in the source model");
    }
    d3dx::D3DXMATRIXF inverse{};
    if(!d3dx::Get().inverse(&inverse,nullptr,reinterpret_cast<const d3dx::D3DXMATRIXF*>(snapshot.object.world)))throw std::runtime_error("Baked accessory has a singular transform");
    if(patch){
        // Source IDs are checked before any write. A malformed sample is discarded
        // with this private snapshot; the live source records remain read-only.
        for(const auto& sample:samples){
            for(size_t i=0;i<sample.positions.size();++i){
                const auto index=sample.sourceVertices[i];if(index==UINT32_MAX)continue;
                const auto& p=sample.positions[i];float point[3]{p[0],p[1],-p[2]},transformed[4]{};
                d3dx::Get().vec3Transform(transformed,point,&inverse);
                if(!std::isfinite(transformed[3])||std::abs(transformed[3])<1e-8f)throw std::runtime_error("Invalid baked vertex transform");
                for(int c=0;c<3;++c)mesh.vertices[index].position[c]=transformed[c]/transformed[3];
                if(sample.normals.size()==sample.positions.size()){
                    const auto& n=sample.normals[i];const float sourceNormal[3]{n[0],n[1],-n[2]};float length=0;
                    // Normals transform with transpose(world), inverse of the
                    // inverse-transpose used while capturing object -> world.
                    for(int r=0;r<3;++r){auto& value=mesh.vertices[index].normal[r];value=0;for(int c=0;c<3;++c)value+=snapshot.object.world[r*4+c]*sourceNormal[c];length+=value*value;}
                    if(length>1e-16f)for(auto& value:mesh.vertices[index].normal)value/=std::sqrt(length);
                }
            }
        }
        mesh.revision=++generation;return;
    }
    // Allocate once for the final mesh, before moving its source attributes.
    // MeshView and triangle indices use uint32_t even on a 64-bit host.
    std::vector<mesh_data::Vertex> vertices;
    const size_t maxVertices=std::min(vertices.max_size(),size_t(UINT32_MAX));
    size_t vertexCount=0;
    for(const auto& sample:samples){
        if(sample.positions.size()>maxVertices-vertexCount)
            throw std::runtime_error("Baked mesh vertex count is out of range");
        vertexCount+=sample.positions.size();
    }
    vertices.reserve(vertexCount);
    // Keep the source attributes without copying the entire vertex table.
    const auto sourceVertices=std::move(mesh.vertices);
    mesh.vertices=std::move(vertices);mesh.triangles.clear();
    for(const auto& sample:samples){
        const auto marker=sample.name.find("_material_");
        const unsigned material=unsigned(std::stoul(sample.name.substr(marker+10)));
        if(material>=65536)throw std::runtime_error("Baked material index is out of range");
        while(mesh.materials.size()<=material){mesh_data::Material m;m.key="baked:"+std::to_string(mesh.materials.size());m.name=m.key;m.nameEn=m.name;m.value.sourceMaterial=-1;m.value.diffuse[0]=m.value.diffuse[1]=m.value.diffuse[2]=m.value.diffuse[3]=1;m.value.flags=1;mesh.materials.push_back(std::move(m));}
        const auto base=uint32_t(mesh.vertices.size());
        for(size_t i=0;i<sample.positions.size();++i){mesh_data::Vertex v{};
            v.sourceVertex=sample.sourceVertices.empty()?UINT32_MAX:sample.sourceVertices[i];
            if(v.sourceVertex!=UINT32_MAX){
                const auto& original=sourceVertices[v.sourceVertex];
                std::copy_n(original.additionalUv,16,v.additionalUv);
                v.edgeScale=original.edgeScale;v.flags=original.flags;
            }
            // Generated vertices and legacy archives without a map retain the
            // existing zero additional UVs, enabled edge and unit edge scale.
            const auto& p=sample.positions[i];float point[3]{p[0],p[1],-p[2]},transformed[4]{};
            d3dx::Get().vec3Transform(transformed,point,&inverse);
            if(!std::isfinite(transformed[3])||std::abs(transformed[3])<1e-8f)throw std::runtime_error("Invalid baked vertex transform");
            for(int c=0;c<3;++c)v.position[c]=transformed[c]/transformed[3];
            if(sample.normals.size()==sample.positions.size()){
                const auto& n=sample.normals[i];const float worldNormal[3]{n[0],n[1],-n[2]};
                for(int r=0;r<3;++r)for(int c=0;c<3;++c)v.normal[r]+=snapshot.object.world[r*4+c]*worldNormal[c];
            }
            if(i<sample.uvs.size()){v.uv[0]=sample.uvs[i][0];v.uv[1]=1-sample.uvs[i][1];}mesh.vertices.push_back(v);}
        for(size_t i=0;i<sample.indices.size();i+=3){mesh_data::Triangle triangle{{base+unsigned(sample.indices[i]),base+unsigned(sample.indices[i+2]),base+unsigned(sample.indices[i+1])},material};mesh.triangles.push_back(triangle);
            auto* a=mesh.vertices[triangle.vertex[0]].position;auto* b=mesh.vertices[triangle.vertex[1]].position;auto* c=mesh.vertices[triangle.vertex[2]].position;
            float u[3]{b[0]-a[0],b[1]-a[1],b[2]-a[2]},v[3]{c[0]-a[0],c[1]-a[1],c[2]-a[2]},n[3]{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
            if(sample.normals.size()!=sample.positions.size())for(auto index:triangle.vertex)for(int k=0;k<3;++k)mesh.vertices[index].normal[k]+=n[k];}
    }
    for(auto& vertex:mesh.vertices){auto* n=vertex.normal;const float length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);if(length>0)for(int c=0;c<3;++c)n[c]/=length;}
    mesh.revision=++generation;
}

void Evaluate(MMDApp* app,const float world[16]){
 Initialize(app);bakeMeshes.clear();bakeError.clear();
 if(mesh_bake_cache::Empty()&&!bakeCapture){for(auto& p:proxies)p.second->active=false;return;}
 try{
  const double seconds=app->PlaybackActive()?app->PlaybackCursorSeconds():app->CurrentFrame()/30.0;
  auto* device=app->Renderer()->device;
  std::vector<const void*> bakedObjects;
  for(int i=0;i<kModelSlotCount;++i)if(auto* m=app->ModelSlot(i))if(mesh_bake_cache::Has(m)&&!mesh_bake_cache::RangeLimited(m))bakedObjects.push_back(m);
  for(int i=0;i<255;++i)if(auto* a=app->AccessorySlot(i))if(mesh_bake_cache::Has(a)&&!mesh_bake_cache::RangeLimited(a))bakedObjects.push_back(a);
  if(restoreBakedEffects){std::string error;if(!DisableMeshEffects(app,bakedObjects,error))throw std::runtime_error(error);restoreBakedEffects=false;}
  std::vector<std::unique_ptr<Snapshot>> owned;
  for(int i=0;i<kModelSlotCount;++i)if(auto* m=app->ModelSlot(i))if((bakeCapture||mesh_bake_cache::ActiveAt(m,seconds))&&mdl::Mdl(m)->vertexBuffer&&mdl::Mdl(m)->indexBuffer)owned.push_back(ModelSnapshot(m));
  for(int i=0;i<255;++i)if(auto* a=app->AccessorySlot(i))if((bakeCapture||mesh_bake_cache::ActiveAt(a,seconds))&&a->mesh)owned.push_back(AccessorySnapshot(a));
  // Preserve MME assignments only when the registered object/resource table
  // changes. A cached vertex update must not re-save/reload MME every frame.
  std::vector<mesh_data::Mesh> originals;originals.reserve(owned.size());
  bool registrationChanged=false;
  for(const auto& entry:proxies)
   if(entry.second->active!=mesh_bake_cache::ActiveAt(entry.first,seconds))registrationChanged=true;
  for(auto& snapshot:owned){
   originals.push_back(snapshot->mesh);ApplyBakedMesh(*snapshot,seconds);
   const auto* source=reinterpret_cast<const void*>(uintptr_t(snapshot->object.id));
   if(!mesh_bake_cache::ActiveAt(source,seconds))continue;
   const auto found=proxies.find(source);
   if(found==proxies.end()){registrationChanged=true;continue;}
   const auto& proxy=*found->second;const auto mesh=snapshot->mesh.View();
   std::vector<std::string> keys;
   for(uint32_t i=0;i<mesh.materialCount;++i)keys.emplace_back(mesh.materials[i].key);
   const bool resources=proxy.accessory
    ?proxy.vertexCapacity!=mesh.vertexCount||proxy.triangleCapacity!=mesh.triangleCount
    :proxy.vertexCapacity<std::max(3u,mesh.vertexCount)||proxy.triangleCapacity<std::max(1u,mesh.triangleCount);
   registrationChanged=registrationChanged||!proxy.active||proxy.keys!=keys||resources;
  }
  const auto savedAssignments=registrationChanged?PreserveEffectAssignments(device):std::filesystem::path{};
  for(auto& p:proxies)p.second->active=false;
  size_t snapshotIndex=0;
  for(auto& snapshot:owned){
   auto original=snapshot->View();original.mesh=originals[snapshotIndex++].View();
   const auto input=snapshot->View();
   const auto* source=reinterpret_cast<const void*>(uintptr_t(input.id));
   if(mesh_bake_cache::ActiveAt(source,seconds)){
    auto& proxy=proxies[source];if(!proxy){proxy=std::make_unique<Proxy>();proxy->source=source;proxy->generation=input.generation;proxy->accessory=input.kind==1;}
    mesh_data::ObjectOutput baked;baked.objectId=input.id;baked.mesh=input.mesh;baked.hidden=input.mesh.triangleCount==0;Upload(*proxy,baked,original,world);
   }
   if(!bakeCapture||!input.visible)continue;
   const auto* mesh=&input.mesh;
                std::string prefix;for(int i=0;i<kModelSlotCount;++i)if(app->ModelSlot(i)==source)prefix="model_"+std::to_string(i);
                for(int i=0;i<255;++i)if(app->AccessorySlot(i)==source)prefix="accessory_"+std::to_string(i);
                if(prefix.empty())throw std::runtime_error("Baked object was removed during sampling");
                d3dx::D3DXMATRIXF inverseWorld{};
                if(!d3dx::Get().inverse(&inverseWorld,nullptr,reinterpret_cast<const d3dx::D3DXMATRIXF*>(input.world)))throw std::runtime_error("Cannot bake a singular object transform");
                const float* inverseValues=reinterpret_cast<const float*>(&inverseWorld);
                std::map<uint32_t,deformed_mesh_capture::Mesh> parts;
                std::map<uint32_t,std::map<uint32_t,int32_t>> remaps;
                for(uint32_t t=0;t<mesh->triangleCount;++t){const auto& triangle=mesh->triangles[t];auto& part=parts[triangle.material];auto& remap=remaps[triangle.material];
                    part.name=prefix+"_material_"+std::to_string(triangle.material)+"_shader_0_pass_0";
                    const uint32_t order[]{triangle.vertex[0],triangle.vertex[2],triangle.vertex[1]};
                    for(const auto v:order){if(v>=mesh->vertexCount)throw std::runtime_error("Invalid native mesh vertex index");auto inserted=remap.emplace(v,int32_t(remap.size()));
                        if(inserted.second){const auto& vertex=mesh->vertices[v];float p[4]{};d3dx::Get().vec3Transform(p,vertex.position,reinterpret_cast<const d3dx::D3DXMATRIXF*>(input.world));
                            if(!std::isfinite(p[3])||std::abs(p[3])<1e-8f)throw std::runtime_error("Invalid native mesh transform");
                            part.positions.push_back({p[0]/p[3],p[1]/p[3],-p[2]/p[3]});part.uvs.push_back({vertex.uv[0],1-vertex.uv[1]});part.sourceVertices.push_back(vertex.sourceVertex);
                            std::array<float,3> normal{};for(int r=0;r<3;++r)for(int c=0;c<3;++c)normal[r]+=inverseValues[r*4+c]*vertex.normal[c];
                            const float length=std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);if(length>1e-8f)for(auto& n:normal)n/=length;normal[2]=-normal[2];part.normals.push_back(normal);}
                        part.indices.push_back(inserted.first->second);
                    }
                }
                for(auto& part:parts)bakeMeshes.push_back(std::move(part.second));

  }
  FinishRegistration(device,savedAssignments);
 }catch(const std::exception& e){bakeError=e.what();for(auto& p:proxies)p.second->active=false;Log(nullptr,2,e.what());}
}
void EffectiveWeights(std::uint32_t weightType,const int bones[4],const float weights[4],
                      int outBones[4],float outWeights[4]) noexcept {
    for(int i=0;i<4;++i){outBones[i]=bones[i];outWeights[i]=weights[i];}
    if(weightType==0){
        // BDEF1: one bone, weight 1; the unused slots are cleared so the sum
        // is exactly 1 and no stale bone index survives.
        outWeights[0]=1.f;outWeights[1]=outWeights[2]=outWeights[3]=0.f;
        outBones[1]=outBones[2]=outBones[3]=-1;
    }else if(weightType==1||weightType==3){
        // BDEF2 / SDEF: the native record keeps one unique weight, the second
        // is implicit 1-w.  Filled exactly, never re-balanced.
        outWeights[1]=1.f-outWeights[0];
        outWeights[2]=outWeights[3]=0.f;
        outBones[2]=outBones[3]=-1;
    }
    // BDEF4 keeps its four native values as they are.
}
void SetBakeCapture(bool enabled){bakeCapture=enabled;bakeMeshes.clear();bakeError.clear();}
bool ReadBakeCapture(std::vector<deformed_mesh_capture::Mesh>& meshes,std::string& error){if(!bakeCapture||!bakeError.empty()){error=bakeError.empty()?"Mesh capture is inactive":bakeError;return false;}meshes=bakeMeshes;error.clear();return true;}

LayerBakeSupport LayerBakeState(){return LayerBakeSupport::NoPlugin;}
bool BakeLayerList(std::vector<uint32_t>& layers,std::string& error){layers.clear();error="Layer baking is unavailable in MMS";return false;}
struct LayerSampling::Impl {};
LayerSampling::LayerSampling()=default;
LayerSampling::~LayerSampling()=default;
bool LayerSampling::Begin(uint32_t,std::string& error){error="Layer baking is unavailable in MMS";return false;}
void LayerSampling::End()noexcept{}
bool LayerSampling::Active()const noexcept{return false;}
uint32_t LayerSampling::Layer()const noexcept{return 0;}
uint64_t LayerSampling::Generation()const noexcept{return 0;}
bool LayerSampling::Query(LayerOutput&,std::string& error,LayerOutput*)const {error="Layer baking is unavailable in MMS";return false;}
LayerBakeTransaction::LayerBakeTransaction()=default;
LayerBakeTransaction::~LayerBakeTransaction()=default;
bool LayerBakeTransaction::Begin(std::string& error){error="Layer baking is unavailable in MMS";return false;}
bool LayerBakeTransaction::StageRemoval(const std::vector<uint64_t>&,uint32_t,std::string& error){error="Layer baking is unavailable in MMS";return false;}
bool LayerBakeTransaction::Commit(std::string& error){error="Layer baking is unavailable in MMS";return false;}
void LayerBakeTransaction::Cancel()noexcept{}
bool LayerBakeTransaction::Open()const noexcept{return false;}
uint64_t LayerBakeTransaction::Generation()const noexcept{return 0;}
}
