// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// Real ModelLoadPMD / LoadPMX, a test-owned hidden Win32 window and an actual
// D3D9 device. This does not claim full InitMainWindow, rendering or GPU timing
// acceptance. No global input is generated and no native loader is replaced.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include "../src/model/model_geometry_cache.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_load_ui.hpp"
#include "mikumikustudio/path_workspace.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "model_geometry_reference.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <fcntl.h>
#include <io.h>
#include <share.h>
using namespace mikumikustudio;
namespace gc=model_geometry_cache;
namespace ref=geometry_test_reference;
namespace fs=std::filesystem;
namespace {
unsigned checks=0,serial=0;
const char* stage="setup";
fs::path root;
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL [%s] %s win32=%lu\n",stage,why,GetLastError());std::exit(1);}}
void Write(const fs::path& path,const ref::Bytes& bytes){std::ofstream out(path,std::ios::binary|std::ios::trunc);Check(bool(out),"open private model fixture");
    out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));out.close();Check(bool(out),"write complete private model fixture");}
struct Environment {
    std::wstring previous;bool present=false;
    Environment(){const auto n=GetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE",nullptr,0);if(n){previous.resize(n);GetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE",previous.data(),n);previous.resize(n-1);present=true;}Set(false);}
    void Set(bool disabled){Check(SetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE",disabled?L"1":nullptr)!=FALSE,"set only process-local cache mode");}
    ~Environment(){SetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE",present?previous.c_str():nullptr);}
};
struct DirectoryRestore {
    fs::path previous=fs::current_path();
    ~DirectoryRestore(){Check(SetCurrentDirectoryW(previous.c_str())!=FALSE,"restore cwd changed by the ordinary PMD loader");}
};
LRESULT CALLBACK UnexpectedDialog(int code,WPARAM wp,LPARAM lp) {
    if(code>=0) {
        const auto& message=*reinterpret_cast<const CWPRETSTRUCT*>(lp);
        if(message.message==WM_INITDIALOG) {
            wchar_t cls[40]{};GetClassNameW(message.hwnd,cls,40);
            if(!std::wcscmp(cls,L"#32770")) {
                wchar_t title[256]{},text[2048]{};GetWindowTextW(message.hwnd,title,256);GetDlgItemTextW(message.hwnd,65535,text,2048);
                std::fwprintf(stderr,L"Unexpected same-thread loader dialog: %ls / %ls\n",title,text);
                Check(false,"unexpected native modal dialog during an isolated model load");
            }
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
struct Host {
    HWND window=nullptr;HHOOK hook=nullptr;
    std::unique_ptr<D3DRenderer> renderer=std::make_unique<D3DRenderer>();
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    MMDApp* previousApp=g_Block;
    Host() {
        window=CreateWindowExW(0,L"STATIC",L"MMS geometry cache isolated loader",WS_OVERLAPPEDWINDOW,
            0,0,96,96,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(window&&GetWindowThreadProcessId(window,nullptr)==GetCurrentThreadId()&&!IsWindowVisible(window),
            "create a test-owned hidden window on the inherited desktop");
        renderer->hwnd=window;renderer->d3d9=Direct3DCreate9(D3D_SDK_VERSION);Check(renderer->d3d9!=nullptr,"create actual D3D9 runtime");
        D3DPRESENT_PARAMETERS p{};p.Windowed=TRUE;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.hDeviceWindow=window;
        p.BackBufferWidth=p.BackBufferHeight=64;p.BackBufferFormat=D3DFMT_UNKNOWN;p.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
        const DWORD flags=D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE;
        HRESULT hr=renderer->d3d9->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,flags,&p,&renderer->device);
        if(FAILED(hr))hr=renderer->d3d9->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_REF,window,flags,&p,&renderer->device);
        Check(SUCCEEDED(hr)&&renderer->device,"create a real device; unavailable D3D9 is a failed prerequisite, never a skipped pass");
        std::memset(&app->state,0,sizeof app->state);app->Hwnd()=window;app->HInstance()=GetModuleHandleW(nullptr);app->Renderer()=renderer.get();app->EnglishUI()=1;g_Block=app.get();
        hook=SetWindowsHookExW(WH_CALLWNDPROCRET,UnexpectedDialog,nullptr,GetCurrentThreadId());Check(hook!=nullptr,"guard only this test thread against unexpected native dialogs");
    }
    ~Host() {
        if(hook)UnhookWindowsHookEx(hook);app->Renderer()=nullptr;g_Block=previousApp;
        // These fixtures have no external texture names. Still release any
        // renderer-owned resource should the native path add one in future.
        for(auto& entry:renderer->resourcePool){if(entry.comObject)entry.comObject->Release();if(entry.heapBuffer)std::free(entry.heapBuffer);entry={};}
        if(renderer->device)renderer->device->Release();if(renderer->d3d9)renderer->d3d9->Release();
        renderer->device=nullptr;renderer->d3d9=nullptr;Check(DestroyWindow(window)!=FALSE,"destroy only the test-owned hidden host window");
    }
};
struct Model {
    std::unique_ptr<mdl::ModelRecord> value=std::make_unique<mdl::ModelRecord>();
    Model(){ModelInitDefaults(Raw());}
    ~Model(){ModelDispose(Raw());}
    unsigned char* Raw()const{return reinterpret_cast<unsigned char*>(value.get());}
};
enum class Route { PmdEntry,DirectPmx };
bool Load(Host& host,Model& object,const fs::path& path,Route route) {
    Check(path.native().size()<256,"real loader path satisfies its original fixed-width field");
    DirectoryRestore cwd;PathResolutionWorkspace paths{};object.value->hwnd=host.window;
    wcscpy_s(object.value->path,std::size(object.value->path),path.c_str());
    const auto directory=path.parent_path().native()+L"\\";
    wcscpy_s(object.value->modelDirectory,std::size(object.value->modelDirectory),directory.c_str());
    model_load_ui::DeferredRefresh deferred(host.window);
    if(route==Route::PmdEntry)return ModelLoadPMD(object.Raw(),host.window,path.c_str(),host.renderer.get(),0,0,1,nullptr,paths);
    int fd=-1;Check(!_wsopen_s(&fd,path.c_str(),_O_RDONLY|_O_BINARY,_SH_DENYNO,0)&&fd>=0,"open a real direct PMX loader input");
    Check(_lseeki64(fd,3,SEEK_SET)==3,"direct PMX entry receives the original post-magic file position");
    return LoadPMX(object.Raw(),host.renderer.get(),0,1,paths,fd); // Production reader owns fd.
}
ref::Bytes VertexBuffer(void* storage) {
    auto* buffer=static_cast<IDirect3DVertexBuffer9*>(storage);Check(buffer!=nullptr,"native loader returned a real vertex buffer");
    D3DVERTEXBUFFER_DESC desc{};Check(SUCCEEDED(buffer->GetDesc(&desc))&&desc.Size,"query actual vertex-buffer storage");
    // The native edge buffer is WRITEONLY. READONLY conflicts with that usage;
    // use an ordinary non-discard lock for its device-specific byte witness.
    const DWORD lockFlags=(desc.Usage&D3DUSAGE_WRITEONLY)?0:D3DLOCK_READONLY;
    void* data=nullptr;Check(SUCCEEDED(buffer->Lock(0,0,&data,lockFlags))&&data,"lock real native vertex-buffer bytes with usage-compatible flags");
    const auto* p=static_cast<const unsigned char*>(data);ref::Bytes result(p,p+desc.Size);Check(SUCCEEDED(buffer->Unlock()),"unlock real native vertex buffer");return result;
}
ref::Bytes IndexBuffer(void* storage) {
    auto* buffer=static_cast<IDirect3DIndexBuffer9*>(storage);Check(buffer!=nullptr,"native loader returned a real index buffer");
    D3DINDEXBUFFER_DESC desc{};Check(SUCCEEDED(buffer->GetDesc(&desc))&&desc.Size,"query actual index-buffer storage");
    void* data=nullptr;Check(SUCCEEDED(buffer->Lock(0,0,&data,D3DLOCK_READONLY))&&data,"lock real native index-buffer bytes");
    const auto* p=static_cast<const unsigned char*>(data);ref::Bytes result(p,p+desc.Size);Check(SUCCEEDED(buffer->Unlock()),"unlock real native index buffer");return result;
}
struct Snapshot {ref::Bytes cpu,main,edge,indices;std::uint32_t vertices=0,indexCount=0;};
Snapshot Capture(const Model& object,const ref::Fixture& fixture) {
    const auto& model=*object.value;Snapshot out;out.vertices=model.vertexCount;out.indexCount=model.indexCount;
    Check(model.loadComplete&&model.vertexCount==fixture.vertices&&model.indexCount==fixture.indices&&model.materialCount==1&&model.boneCount==1,
        "real loader completed geometry and subsequent material/bone sections");
    const auto append=[&](const void* source,std::size_t bytes){const auto* p=static_cast<const unsigned char*>(source);out.cpu.insert(out.cpu.end(),p,p+bytes);};
    if(fixture.pmd) {
        Check(model.rawVertices&&model.indices,"real PMD working arrays exist");append(model.rawVertices,fixture.vertices*sizeof(mdl::PmdVertex));
        const auto* indices=static_cast<const std::uint16_t*>(model.indices);
        for(std::uint32_t i=0;i<fixture.indices;++i){Check(indices[i*2+1]==0,"actual PMD loader keeps the four-byte CPU index stride");append(indices+i*2,4);}
    } else {
        Check(model.pmxVertices&&model.indices,"real PMX working arrays exist");
        for(std::uint32_t i=0;i<fixture.vertices;++i){const auto fields=ref::ParsedFields(model.pmxVertices[i],fixture.uv);out.cpu.insert(out.cpu.end(),fields.begin(),fields.end());}
        append(model.indices,fixture.indices*sizeof(std::int32_t));
    }
    out.main=VertexBuffer(model.vertexBuffer);out.edge=VertexBuffer(model.vertexBuffer2);out.indices=IndexBuffer(model.indexBuffer);return out;
}
void Equal(const Snapshot& a,const Snapshot& b) {
    Check(a.vertices==b.vertices&&a.indexCount==b.indexCount&&a.cpu==b.cpu,"actual parser CPU geometry matches cold/warm/disabled paths");
    Check(a.main==b.main&&a.edge==b.edge&&a.indices==b.indices,"actual D3D9 main/edge/index buffer contents match all cache modes");
}
Snapshot Actual(Host& host,const fs::path& path,const ref::Fixture& fixture,Route route) {
    Model model;Check(Load(host,model,path,route),"the actual production loader accepts the complete private fixture");return Capture(model,fixture);
}
FILETIME LastWrite(const fs::path& path){WIN32_FILE_ATTRIBUTE_DATA info{};Check(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info)!=FALSE,"read own source mtime");return info.ftLastWriteTime;}
void RestoreWrite(const fs::path& path,FILETIME time){const auto file=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    Check(file!=INVALID_HANDLE_VALUE&&SetFileTime(file,nullptr,nullptr,&time),"restore own fixture mtime after same-sized mutation");CloseHandle(file);}
void FullRoundTrip(Host& host,Environment& environment,ref::Fixture fixture,Route route,const char* label) {
    stage=label;std::printf("STAGE %s\n",label);const auto path=root/(L"loader-"+std::to_wstring(++serial)+(fixture.pmd?L".pmd":L".pmx"));Write(path,fixture.bytes);
    environment.Set(false);const auto initial=gc::GetProcessStatistics();const auto cold=Actual(host,path,fixture,route);const auto first=gc::GetProcessStatistics();
    const auto warm=Actual(host,path,fixture,route);const auto second=gc::GetProcessStatistics();Equal(cold,warm);
    if(build_profile::original)Check(first.hits==initial.hits&&first.stores==initial.stores&&second.hits==first.hits,"original production profile keeps its process geometry cache disabled");
    else Check(first.misses==initial.misses+1&&first.stores==initial.stores+1&&second.hits==first.hits+1,
        "actual full first load publishes exactly one miss and second actual load hits");
    environment.Set(true);const auto disabled=Actual(host,path,fixture,route);const auto third=gc::GetProcessStatistics();Equal(cold,disabled);
    Check(third.hits==second.hits&&third.stores==second.stores,"disabled real parser neither hits nor publishes a snapshot");environment.Set(false);
    const auto oldTime=LastWrite(path);const float changed=42.25f;std::memcpy(fixture.bytes.data()+fixture.vertexStart,&changed,4);Write(path,fixture.bytes);RestoreWrite(path,oldTime);
    const auto changedSnapshot=Actual(host,path,fixture,route);const auto fourth=gc::GetProcessStatistics();
    Check(changedSnapshot.cpu!=cold.cpu&&changedSnapshot.main!=cold.main,"real CPU and GPU arrays both reflect the completed source mutation");
    if(!build_profile::original)Check(fourth.hits==third.hits&&fourth.stores==third.stores+1&&fourth.invalidations>third.invalidations,
        "same-size same-mtime source edit invalidates an actual cached full import");
    const auto changedWarm=Actual(host,path,fixture,route);Equal(changedSnapshot,changedWarm);
    Check(!IsWindowVisible(host.window),"real loader validation never displays the test-owned host");std::printf("PASS %s\n",label);
}
void LaterFailureDoesNotPublish(Host& host,Environment& environment) {
    stage="actual-PMX-late-section-failure";std::printf("STAGE %s\n",stage);
    const auto fixture=ref::Pmx(3,2,2,2,true);const auto path=root/L"late-failure.pmx";
    auto broken=fixture.bytes;broken.resize(static_cast<std::size_t>(fixture.indexEnd));
    ref::Put(broken,1u);ref::Put(broken,3u);broken.insert(broken.end(),{0,0,0}); // Odd UTF16 texture name after geometry.
    Write(path,broken);environment.Set(false);const auto before=gc::GetProcessStatistics();
    {
        Model model;Check(!Load(host,model,path,Route::DirectPmx),"actual PMX loader rejects malformed later text after preparing real geometry");
        Check(model.value->vertexBuffer&&model.value->indexBuffer,"failure happened after both actual GPU geometry resources were created");
    }
    const auto failed=gc::GetProcessStatistics();Check(failed.stores==before.stores&&!failed.pendingBytes,"failed complete native import releases pending geometry without storing");
    Write(path,fixture.bytes);const auto restored=Actual(host,path,fixture,Route::DirectPmx);const auto after=gc::GetProcessStatistics();
    if(!build_profile::original)Check(after.hits==failed.hits&&after.stores==failed.stores+1,"first successful restored full import still misses and stores only now");
    Equal(restored,Actual(host,path,fixture,Route::DirectPmx));std::printf("PASS %s\n",stage);
}
}
int wmain(int argc,wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);Check(argc==2,"provide a private evidence directory");
    root=fs::absolute(argv[1])/(L"host-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));root.make_preferred();
    Check(root.native().size()+48<256&&fs::create_directories(root),"create fresh bounded native-loader fixture directory");
    Environment environment;Host host;
    FullRoundTrip(host,environment,ref::Pmd(3,true),Route::PmdEntry,"real-PMD-entry");
    FullRoundTrip(host,environment,ref::Pmx(3,0,1,1,true),Route::PmdEntry,"real-PMX-via-PMD-entry");
    FullRoundTrip(host,environment,ref::Pmx(3,2,2,2,true),Route::DirectPmx,"real-direct-PMX-two-UV");
    FullRoundTrip(host,environment,ref::Pmx(3,4,4,4,true),Route::DirectPmx,"real-direct-PMX-four-UV-wide-index");
    LaterFailureDoesNotPublish(host,environment);
    std::printf("PASS actual hidden Win32/D3D9 loader geometry cache: %u checks profile=%s\n",checks,build_profile::original?"original-disabled":"full-cache");return 0;
}
