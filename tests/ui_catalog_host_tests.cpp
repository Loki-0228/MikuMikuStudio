// MAINTENANCE: regression-keep. Real hidden MMS + original C ABI fixture DLL.
// The runner must copy this EXE/dependencies into a fresh private scratch.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/plugin_storage.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "../src/plugins/history_runtime.hpp"
#include <mms/ui_catalog.h>
#include <MinHook.h>
#include <commctrl.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mikumikustudio;
namespace fs=std::filesystem;
namespace {
unsigned checks=0;
const char* stage="setup";
void Require(bool condition,const char* message) {
    ++checks;if(!condition){std::fprintf(stderr,"FAIL [%s] %s\n",stage,message);throw std::runtime_error(message);}
}
template<class T> constexpr MmsHeader H(){return {sizeof(T),1,0};}
std::string Text(MmsString value) {
    Require(!value.bytes||value.data,"nonempty ABI string has readable leased storage");
    return value.bytes?std::string(value.data,size_t(value.bytes)):std::string{};
}
bool Is(MmsString value,const char* literal){return value.bytes==std::strlen(literal)&&(!value.bytes||!std::memcmp(value.data,literal,size_t(value.bytes)));}
bool OwnTopLevel(HWND window){DWORD process=0;GetWindowThreadProcessId(window,&process);return process==GetCurrentProcessId()&&!GetParent(window);}
decltype(&ShowWindow) realShow=nullptr;
decltype(&SetWindowPos) realPosition=nullptr;
BOOL WINAPI Hidden(HWND window,int command){return realShow(window,OwnTopLevel(window)?SW_HIDE:command);}
BOOL WINAPI HiddenPosition(HWND window,HWND after,int x,int y,int width,int height,UINT flags){
    if(OwnTopLevel(window))flags=(flags&~SWP_SHOWWINDOW)|SWP_NOACTIVATE;
    return realPosition(window,after,x,y,width,height,flags);
}
int WINAPI UnexpectedA(HWND,LPCSTR text,LPCSTR,UINT){std::fprintf(stderr,"FAIL unexpected fixture dialog: %s\n",text);std::exit(2);}
int WINAPI UnexpectedW(HWND,LPCWSTR text,LPCWSTR,UINT){std::fwprintf(stderr,L"FAIL unexpected fixture dialog: %ls\n",text);std::exit(2);}
void Hook(void* target,void* replacement,void** original=nullptr){
    Require(MH_CreateHook(target,replacement,original)==MH_OK,"create own hidden-host guard");
    Require(MH_EnableHook(target)==MH_OK,"enable own hidden-host guard");
}
fs::path PreparePackage(const fs::path& source){
    Require(source.is_absolute()&&fs::is_regular_file(source),"probe DLL is an existing absolute path");
    wchar_t module[32768]{};const auto length=GetModuleFileNameW(nullptr,module,32768);
    Require(length&&length<32768,"read this dedicated fixture executable path");
    const fs::path executable(module),directory=executable.parent_path();
    Require(executable.stem()==L"ui_catalog_host_tests"&&directory.filename().wstring().rfind(L"mms-ui-catalog-host-",0)==0,
        "run only a copied dedicated EXE in a fresh mms-ui-catalog-host-* scratch directory");
    const auto plugins=directory/L"PLUGINS",data=directory/L"data";
    Require(!fs::exists(plugins)&&!fs::exists(data),"refuse existing plugin packages or data before making any fixture claim");
    const auto lock=directory/L"ui-catalog-owner.lock";
    HANDLE claim=CreateFileW(lock.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    Require(claim!=INVALID_HANDLE_VALUE,"refuse reused fixture scratch directory");CloseHandle(claim);
    Require(fs::create_directory(plugins)&&fs::create_directory(data),"exclusively create own PLUGINS and data roots");
    const auto package=plugins/L"ui-catalog-probe";
    Require(fs::create_directory(package)&&fs::copy_file(source,package/L"ui_catalog_probe.dll",fs::copy_options::none),
        "copy only the original probe into its newly owned package");
    std::ofstream manifest(package/L"plugin.json",std::ios::binary|std::ios::out);
    Require(bool(manifest),"create the exclusively owned probe manifest");
    manifest<<R"({"schemaVersion":1,"id":"org.mms.test.uicatalog","name":"UI catalog real host fixture","version":"1.0.0","kind":"native","abi":{"major":1,"minMinor":0},"entry":"ui_catalog_probe.dll","architecture":"x64","minWindows":"6.1.7601","capabilities":["ui.command","ui.element"],"requiresServices":[{"id":"mms.ui","major":1,"minMinor":0},{"id":"mms.ui.catalog","major":1,"minMinor":0},{"id":"mms.ui.refresh","major":1,"minMinor":0}],"dependencies":[],"nativeDependencies":[],"reloadPolicy":"restart"})";
    manifest.close();Require(bool(manifest),"finish own probe manifest");
    Require(SetCurrentDirectoryW(directory.c_str())!=FALSE,"keep legacy relative I/O inside the copied fixture directory");
    return directory;
}
void PumpOwn(){
    MSG message{};unsigned dispatched=0;
    while(dispatched++<512&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
        Require(message.message!=WM_QUIT,"keep own host alive during verification");
        if(!message.hwnd)continue;
        DWORD process=0;GetWindowThreadProcessId(message.hwnd,&process);
        Require(process==GetCurrentProcessId(),"dispatch only this fixture process's window messages");
        TranslateMessage(&message);DispatchMessageW(&message);
    }
}
void Settle(MMDApp& app){for(unsigned n=0;n<4;++n){PumpOwn();plugins::Refresh(&app);}PumpOwn();}

struct ProbeState {
    uint32_t size,readyEvents,stateCalls,invokes,acquires,resolves,releases;
    MmsResult editGate;
    MmsHandle command,button,menu;
    uint32_t startupScopes,startupEntries;
};
struct AccessResults {
    uint32_t size;
    MmsResult refreshGet,refreshRequest,layoutBegin,layoutApply,layoutCommit,unregisterNative;
    uint32_t reserved;
};
static_assert(sizeof(ProbeState)==64&&sizeof(AccessResults)==32,"original DLL test protocols");
struct Probe {
    MmsResult(MMS_CALL* state)(ProbeState*)=nullptr;
    MmsResult(MMS_CALL* capabilities)(MmsUiCatalogCapabilitiesV1*)=nullptr;
    MmsResult(MMS_CALL* acquire)(uint32_t,MmsUiCatalogSnapshotV1*)=nullptr;
    MmsResult(MMS_CALL* resolve)(const MmsUiTargetRefV1*,MmsUiCatalogEntryV1*,MmsHandle*)=nullptr;
    MmsResult(MMS_CALL* release)(MmsHandle)=nullptr;
    MmsResult(MMS_CALL* readOnly)(const MmsUiCatalogEntryV1*,AccessResults*)=nullptr;
    Probe(){
        const auto module=GetModuleHandleW(L"ui_catalog_probe.dll");Require(module!=nullptr,"production Manager loaded the original probe DLL");
#define PROBE_EXPORT(member,name) member=reinterpret_cast<decltype(member)>(GetProcAddress(module,name));Require(member!=nullptr,"resolve original C ABI probe export " name)
        PROBE_EXPORT(state,"UiCatalogProbeState");PROBE_EXPORT(capabilities,"UiCatalogProbeCapabilities");
        PROBE_EXPORT(acquire,"UiCatalogProbeAcquire");PROBE_EXPORT(resolve,"UiCatalogProbeResolve");
        PROBE_EXPORT(release,"UiCatalogProbeRelease");PROBE_EXPORT(readOnly,"UiCatalogProbeReadOnly");
#undef PROBE_EXPORT
    }
    ProbeState State()const{ProbeState out{};out.size=sizeof(out);Require(state(&out)==MMS_OK,"read original DLL counters and registered handles");return out;}
};
constexpr uint32_t kAllScopes=MMS_UI_CATALOG_REGISTRY|MMS_UI_CATALOG_MAIN_MENU|MMS_UI_CATALOG_MAIN_CONTROLS;
struct Snapshot {
    const Probe& probe;MmsUiCatalogSnapshotV1 value{};
    explicit Snapshot(const Probe& p):probe(p){value.h=H<MmsUiCatalogSnapshotV1>();Require(probe.acquire(kAllScopes,&value)==MMS_OK,"acquire public catalog through actual loaded DLL");}
    ~Snapshot(){
        if(value.handle&&probe.release(value.handle)!=MMS_OK){
            std::fprintf(stderr,"FAIL [%s] release actual catalog snapshot before fixture shutdown\n",stage);
            std::exit(2);
        }
    }
    Snapshot(const Snapshot&)=delete;
};
template<class T> const T& At(MmsSpan span,uint64_t index){
    Require(index<span.count&&span.data&&span.stride>=sizeof(T)&&span.recordVersion==1,"leased span has the negotiated record type and bounded index");
    return *reinterpret_cast<const T*>(static_cast<const unsigned char*>(span.data)+size_t(index*span.stride));
}
const MmsUiCatalogEntryV1& Entry(const MmsUiCatalogSnapshotV1& snapshot,const char* id){
    const MmsUiCatalogEntryV1* found=nullptr;
    for(uint64_t i=0;i<snapshot.elements.count;++i){const auto& entry=At<MmsUiCatalogEntryV1>(snapshot.elements,i);
        if(Is(entry.id,id)){Require(!found,"catalog IDs never duplicate");found=&entry;}}
    Require(found!=nullptr,"required actual host element is present in the catalog");return *found;
}
const MmsUiCatalogEntryV1& Label(const MmsUiCatalogSnapshotV1& snapshot,const char* label){
    const MmsUiCatalogEntryV1* found=nullptr;
    for(uint64_t i=0;i<snapshot.elements.count;++i){const auto& entry=At<MmsUiCatalogEntryV1>(snapshot.elements,i);
        if(Is(entry.label,label)){Require(!found,"fixture label appears exactly once");found=&entry;}}
    Require(found!=nullptr,"actual materialized native menu label is discoverable");return *found;
}
std::string SnapshotBytes(const MmsUiCatalogSnapshotV1& snapshot){
    std::string result;
    const auto scalar=[&](const auto& value){result.append(reinterpret_cast<const char*>(&value),sizeof(value));};
    const auto string=[&](MmsString value){scalar(value.bytes);result+=Text(value);};
    scalar(snapshot.revision);scalar(snapshot.registryRevision);scalar(snapshot.projectEpoch);
    scalar(snapshot.requestedScopes);scalar(snapshot.availableScopes);scalar(snapshot.coverageFlags);
    for(uint64_t i=0;i<snapshot.elements.count;++i){const auto& e=At<MmsUiCatalogEntryV1>(snapshot.elements,i);
        scalar(e.element);string(e.id);string(e.ownerPlugin);string(e.parentId);string(e.label);string(e.commandId);string(e.propertyId);
        scalar(e.descriptionGeneration);scalar(e.target);scalar(e.bounds);scalar(e.kind);scalar(e.origin);scalar(e.scope);
        scalar(e.knownFields);scalar(e.state);scalar(e.role);scalar(e.ordinal);}
    for(uint64_t i=0;i<snapshot.registrations.count;++i){const auto& r=At<MmsUiCatalogRegistrationV1>(snapshot.registrations,i);
        scalar(r.handle);string(r.id);string(r.ownerPlugin);string(r.label);scalar(r.kind);scalar(r.flags);}
    return result;
}

struct MessageCounts{uint64_t commands=0,menu=0,popup=0,draw=0,measure=0;};
struct Messages {
    HWND window;MessageCounts counts{};
    explicit Messages(HWND value):window(value){Require(SetWindowSubclass(window,Procedure,0x55494341,reinterpret_cast<DWORD_PTR>(this))!=FALSE,"observe only the owned main window");}
    ~Messages(){if(IsWindow(window))RemoveWindowSubclass(window,Procedure,0x55494341);}
    static LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR data){
        auto& self=*reinterpret_cast<Messages*>(data);
        switch(message){case WM_COMMAND:++self.counts.commands;break;case WM_INITMENU:++self.counts.menu;break;
        case WM_INITMENUPOPUP:++self.counts.popup;break;case WM_DRAWITEM:++self.counts.draw;break;case WM_MEASUREITEM:++self.counts.measure;break;}
        return DefSubclassProc(window,message,wParam,lParam);
    }
};
struct Invariant {
    MMDApp& app;const Probe& probe;Messages& messages;
    int frame;uint8_t modified;uint64_t sceneRevision,sceneGeneration;
    HWND focus,capture;ProbeState before;MessageCounts counts;
    plugins::SharedHistory* history;uint64_t revision=0,retained=0;size_t undo=0,redo=0;
    Invariant(MMDApp& a,const Probe& p,Messages& m):app(a),probe(p),messages(m),frame(a.CurrentFrame()),modified(a.state.sceneModified),
        sceneRevision(a.SceneRevision()),sceneGeneration(a.SceneGeneration()),focus(GetFocus()),capture(GetCapture()),before(p.State()),counts(m.counts),history(plugins::NativeHistory(&a)){
        if(history){Require(history->Revision(revision)==MMS_OK,"capture actual common history revision");retained=history->RetainedBytes();undo=history->UndoCount();redo=history->RedoCount();}
    }
    void Check(){
        const auto after=probe.State();
        Require(app.CurrentFrame()==frame&&app.state.sceneModified==modified&&app.SceneRevision()==sceneRevision&&app.SceneGeneration()==sceneGeneration,
            "catalog reads and rejected native writes preserve playhead, scene data and scene revisions");
        Require(GetFocus()==focus&&GetCapture()==capture,"catalog operations neither move focus nor capture input");
        Require(after.stateCalls==before.stateCalls&&after.invokes==before.invokes,"catalog reads invoke no command providers or commands");
        Require(!std::memcmp(&counts,&messages.counts,sizeof(counts)),"catalog calls send no command, popup initialization, measure or draw messages");
        Require(plugins::NativeHistory(&app)==history,"catalog calls preserve actual journal identity");
        if(history){uint64_t now=0;Require(history->Revision(now)==MMS_OK&&now==revision&&history->RetainedBytes()==retained&&history->UndoCount()==undo&&history->RedoCount()==redo,
            "catalog operations add no undo/redo steps or retained history payload");}
    }
};
void Stale(const Probe& probe,const MmsUiTargetRefV1& ref){
    MmsUiCatalogEntryV1 output;std::memset(&output,0x5a,sizeof(output));output.h=H<MmsUiCatalogEntryV1>();
    const auto untouched=output;MmsHandle lease=0x9595959595959595ull;
    Require(probe.resolve(&ref,&output,&lease)==MMS_E_STALE,"retired native instance or project epoch rejects the old target reference");
    Require(!std::memcmp(&output,&untouched,sizeof(output))&&lease==0x9595959595959595ull,"stale resolve leaves both caller outputs unchanged");
}
MmsUiCatalogEntryV1 Resolve(const Probe& probe,const MmsUiTargetRefV1& ref){
    MmsUiCatalogEntryV1 out{};out.h=H<MmsUiCatalogEntryV1>();MmsHandle lease=0;
    Require(probe.resolve(&ref,&out,&lease)==MMS_OK&&lease&&out.element==ref.element,"resolve the exact original current element through the public ABI");
    // Copy only scalars for the caller. Leased strings must be consumed before
    // release; the checks needing strings occur here.
    Require(out.target.element==ref.element&&out.target.instanceGeneration==ref.instanceGeneration&&out.target.projectEpoch==ref.projectEpoch,
        "resolve preserves original handle, presentation identity and project epoch");
    Require(probe.release(lease)==MMS_OK,"release the exact owner-scoped resolve lease");
    out.id={};out.ownerPlugin={};out.parentId={};out.label={};out.commandId={};out.propertyId={};return out;
}
unsigned FindMenu(HMENU menu,const wchar_t* label){
    unsigned result=0;
    for(int i=0;menu&&i<GetMenuItemCount(menu);++i){wchar_t text[256]{};GetMenuStringW(menu,UINT(i),text,256,MF_BYPOSITION);
        if(!std::wcscmp(text,label)){const auto id=GetMenuItemID(menu,i);Require(id&&id!=UINT(-1)&&!result,"one actual plugin command menu ID");result=id;}
        if(const auto child=GetSubMenu(menu,i)){const auto nested=FindMenu(child,label);if(nested){Require(!result,"no duplicate plugin command label");result=nested;}}}
    return result;
}
struct NativeMenu {
    HWND window;HMENU root,popup=nullptr;
    explicit NativeMenu(HWND main):window(main),root(GetMenu(main)){
        Require(root!=nullptr,"actual MMS main menu exists before fixture mutation");popup=CreatePopupMenu();Require(popup!=nullptr,"create own unopened native submenu");
        Require(AppendMenuW(popup,MF_STRING,0x7d10,L"Catalog native leaf \x03a9")&&AppendMenuW(popup,MF_SEPARATOR,0,nullptr)&&
            AppendMenuW(popup,MF_OWNERDRAW,0x7d11,reinterpret_cast<LPCWSTR>(UINT_PTR(0x1234))),"create native string, separator and unknown owner-draw metadata");
        Require(AppendMenuW(root,MF_POPUP,reinterpret_cast<UINT_PTR>(popup),L"Catalog fixture native root")!=FALSE,"append only the fixture's native submenu to its own main window");
    }
    ~NativeMenu(){
        if(!popup)return;
        if(IsMenu(root))for(int i=GetMenuItemCount(root)-1;i>=0;--i)if(GetSubMenu(root,i)==popup){RemoveMenu(root,UINT(i),MF_BYPOSITION);break;}
        if(IsMenu(popup))DestroyMenu(popup);
    }
};

#if !MMD_ORIGINAL_PROFILE
void Run(MMDApp& app){
    Settle(app);Probe probe;const auto state=probe.State();
    std::printf("Catalog startup: scopes=%u entries=%u edit-query=%d\n",state.startupScopes,state.startupEntries,state.editGate);
    Require(state.command&&state.button&&state.menu&&state.editGate==MMS_E_NOT_FOUND,"ordinary runtime registers original probe objects while keeping public editing closed");
    Require((state.startupScopes&(MMS_UI_CATALOG_MAIN_MENU|MMS_UI_CATALOG_MAIN_CONTROLS))==(MMS_UI_CATALOG_MAIN_MENU|MMS_UI_CATALOG_MAIN_CONTROLS)&&state.startupEntries>20,
        "the real loaded DLL could read the first host-owned catalog before registering its own elements");
    Require(!IsWindowVisible(app.MainWindow()),"actual host stays hidden");
    NativeMenu native(app.MainWindow());Settle(app);Messages messages(app.MainWindow());
    stage="real-manager-catalog-and-read-only-query";
    Invariant first(app,probe,messages);
    MmsUiCatalogCapabilitiesV1 caps{};caps.h=H<MmsUiCatalogCapabilitiesV1>();
    Require(probe.capabilities(&caps)==MMS_OK&&(caps.supportedScopes&kAllScopes)==kAllScopes&&caps.maxEntries&&caps.maxLeasesPerPlugin&&caps.maxSnapshotBytes,
        "actual service advertises bounded catalog scopes and lease limits");
    Snapshot initial(probe);const auto initialBytes=SnapshotBytes(initial.value);
    Require((initial.value.availableScopes&kAllScopes)==kAllScopes&&initial.value.projectEpoch&&
        (initial.value.coverageFlags&(MMS_UI_CATALOG_REGISTRY_COMPLETE|MMS_UI_CATALOG_MATERIALIZED_MENU|MMS_UI_CATALOG_DECLARED_CONTROLS|MMS_UI_CATALOG_INCOMPLETE_NATIVE))==
            (MMS_UI_CATALOG_REGISTRY_COMPLETE|MMS_UI_CATALOG_MATERIALIZED_MENU|MMS_UI_CATALOG_DECLARED_CONTROLS|MMS_UI_CATALOG_INCOMPLETE_NATIVE),
        "production publication honestly covers registry, current main menu and declared controls only");
    const auto& slider=Entry(initial.value,"mms.catalog.main.control.455");
    Require(slider.origin==MMS_UI_CATALOG_NATIVE_OBSERVATION&&Is(slider.ownerPlugin,"mms")&&slider.role==MMS_UI_CATALOG_ROLE_SLIDER&&
        (slider.knownFields&(MMS_UI_CATALOG_BOUNDS|MMS_UI_CATALOG_PRESENTATION|MMS_UI_CATALOG_ENABLED|MMS_UI_CATALOG_VISIBLE|MMS_UI_CATALOG_LABEL))==
            (MMS_UI_CATALOG_BOUNDS|MMS_UI_CATALOG_PRESENTATION|MMS_UI_CATALOG_ENABLED|MMS_UI_CATALOG_VISIBLE|MMS_UI_CATALOG_LABEL)&&
        Is(slider.label,"Light red")&&slider.target.instanceGeneration&&slider.target.element==slider.element,
        "actual original light slider has semantic metadata, known bounds and one native instance");
    const auto& button=Entry(initial.value,"org.mms.test.uicatalog.button");const auto& menu=Entry(initial.value,"org.mms.test.uicatalog.menu");
    Require(button.element==state.button&&menu.element==state.menu&&button.origin==MMS_UI_CATALOG_REGISTERED&&menu.origin==MMS_UI_CATALOG_REGISTERED&&
        button.target.element==state.button&&menu.target.element==state.menu&&button.target.instanceGeneration&&menu.target.instanceGeneration,
        "toolbar and menu presentations reuse their original public registration handles");
    std::set<MmsHandle> handles;unsigned actualNativeMenuRows=0,originalButtonCount=0,originalMenuCount=0,commandCount=0;
    for(uint64_t i=0;i<initial.value.elements.count;++i){const auto& e=At<MmsUiCatalogEntryV1>(initial.value.elements,i);
        Require(e.element&&handles.insert(e.element).second,"merged catalog never duplicates a handle for native aliases or registered objects");
        if(e.element==state.button)++originalButtonCount;if(e.element==state.menu)++originalMenuCount;
        if(e.origin==MMS_UI_CATALOG_NATIVE_OBSERVATION&&(e.scope&MMS_UI_CATALOG_MAIN_MENU)){
            ++actualNativeMenuRows;Require(!(e.knownFields&MMS_UI_CATALOG_BOUNDS),"native menus never invent client rectangles");}}
    for(uint64_t i=0;i<initial.value.registrations.count;++i){const auto& r=At<MmsUiCatalogRegistrationV1>(initial.value.registrations,i);
        if(r.handle==state.command){++commandCount;Require(r.kind==MMS_UI_CATALOG_COMMAND&&Is(r.id,"org.mms.test.uicatalog.invoke")&&Is(r.ownerPlugin,"org.mms.test.uicatalog"),
            "command descriptions retain the actual original registered command identity");}}
    Require(originalButtonCount==1&&originalMenuCount==1&&commandCount==1&&actualNativeMenuRows>20,"real base menu and each original fixture registration are present exactly once");
    const auto& leaf=Label(initial.value,u8"Catalog native leaf \u03a9");const auto& nativeRoot=Label(initial.value,"Catalog fixture native root");
    Require(leaf.origin==MMS_UI_CATALOG_NATIVE_OBSERVATION&&leaf.target.instanceGeneration&&!(leaf.knownFields&MMS_UI_CATALOG_BOUNDS),"actual added native menu is observed without fake bounds");
    bool unknownOwnerDraw=false,knownSeparator=false;
    const auto parent=Text(nativeRoot.id);
    for(uint64_t i=0;i<initial.value.elements.count;++i){const auto& e=At<MmsUiCatalogEntryV1>(initial.value.elements,i);
        if(Text(e.parentId)!=parent)continue;
        if(e.ordinal==1)knownSeparator=e.role==MMS_UI_CATALOG_ROLE_SEPARATOR;
        if(e.ordinal==2)unknownOwnerDraw=!(e.knownFields&MMS_UI_CATALOG_LABEL)&&e.label.bytes==0&&!(e.knownFields&MMS_UI_CATALOG_BOUNDS);}
    Require(unknownOwnerDraw&&knownSeparator,"unknown owner-draw label and known separator remain explicitly distinguishable");
    Resolve(probe,slider.target);Resolve(probe,button.target);Resolve(probe,menu.target);Resolve(probe,leaf.target);
    AccessResults access{};access.size=sizeof(access);
    Require(probe.readOnly(&slider,&access)==MMS_OK&&access.refreshGet!=MMS_OK&&access.refreshRequest!=MMS_OK&&access.unregisterNative!=MMS_OK&&
        access.layoutBegin==MMS_OK&&(access.layoutApply!=MMS_OK||access.layoutCommit!=MMS_OK),
        "native discovery grants no refresh, unregister or layout mutation authority");
    first.Check();

    stage="real-plugin-command-and-menu-metadata";
    const auto commandId=FindMenu(GetMenu(app.MainWindow()),L"Catalog fixture plugin menu");
    Require(commandId>=0xa000&&commandId<=0xbfff,"actual adapter maps the registered plugin menu into its ordinary command range");
    const auto beforeInvoke=probe.State();SendMessageW(app.MainWindow(),WM_COMMAND,WPARAM(commandId),0);
    Require(probe.State().invokes==beforeInvoke.invokes+1,"ordinary MMS command dispatch invokes the original DLL callback");
    Settle(app);
    Require(ModifyMenuW(native.popup,0,MF_BYPOSITION|MF_STRING,0x7d10,L"Catalog native renamed \x03a9")!=FALSE,"change only text of own native menu entry");
    Settle(app);
    {
        Invariant read(app,probe,messages);Snapshot changed(probe);
        const auto& renamed=Label(changed.value,u8"Catalog native renamed \u03a9");
        Require(renamed.element==leaf.element&&renamed.target.instanceGeneration==leaf.target.instanceGeneration&&changed.value.revision!=initial.value.revision,
            "native text-only publication updates metadata without inventing another live element or instance");
        MmsUiCatalogEntryV1 resolved{};resolved.h=H<MmsUiCatalogEntryV1>();MmsHandle lease=0;
        Require(probe.resolve(&leaf.target,&resolved,&lease)==MMS_OK&&Is(resolved.label,u8"Catalog native renamed \u03a9"),"old live native ref resolves to latest text-only metadata");
        Require(probe.release(lease)==MMS_OK,"release text-update resolve lease");
        Require(SnapshotBytes(initial.value)==initialBytes&&Is(leaf.label,u8"Catalog native leaf \u03a9"),"old lease keeps immutable original text after metadata publication");read.Check();
    }

    stage="real-menu-structure-retirement";
    Require(AppendMenuW(native.popup,MF_STRING,0x7d12,L"Catalog structural addition")!=FALSE,"append a structural row only to own native submenu");
    Settle(app);
    {
        Invariant read(app,probe,messages);Stale(probe,leaf.target);Snapshot changed(probe);
        const auto& renamed=Label(changed.value,u8"Catalog native renamed \u03a9");
        Require(renamed.element!=leaf.element&&renamed.target.instanceGeneration!=leaf.target.instanceGeneration,"structural native menu replacement gets a fresh observation identity");
        Require(SnapshotBytes(initial.value)==initialBytes,"old entire catalog lease remains readable after native menu observation retirement");read.Check();
    }

    stage="actual-control-destruction-and-recreation";
    HWND oldControl=GetDlgItem(app.MainWindow(),455);Require(oldControl&&IsWindow(oldControl),"actual original 455 control exists");
    wchar_t controlClass[128]{};Require(GetClassNameW(oldControl,controlClass,128)>0,"read owned control class before replacing it");
    RECT bounds{};Require(GetWindowRect(oldControl,&bounds)!=FALSE,"read actual owned control bounds");
    SetLastError(ERROR_SUCCESS);
    Require(MapWindowPoints(nullptr,app.MainWindow(),reinterpret_cast<POINT*>(&bounds),2)||GetLastError()==ERROR_SUCCESS,"map actual original slider rectangle into its own main client");
    const auto style=DWORD(GetWindowLongPtrW(oldControl,GWL_STYLE)),extended=DWORD(GetWindowLongPtrW(oldControl,GWL_EXSTYLE));
    const auto minimum=SendMessageW(oldControl,TBM_GETRANGEMIN,0,0),maximum=SendMessageW(oldControl,TBM_GETRANGEMAX,0,0),position=SendMessageW(oldControl,TBM_GETPOS,0,0);
    Require(DestroyWindow(oldControl)!=FALSE,"destroy only the fixture's real original light slider");
    {Invariant read(app,probe,messages);Stale(probe,slider.target);Require(SnapshotBytes(initial.value)==initialBytes,"destroyed control does not revoke old immutable snapshot memory");read.Check();}
    HWND replacement=CreateWindowExW(extended,controlClass,L"",style,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,
        app.MainWindow(),reinterpret_cast<HMENU>(INT_PTR(455)),GetModuleHandleW(nullptr),nullptr);
    Require(replacement&&GetDlgItem(app.MainWindow(),455)==replacement,"recreate a real child of the same native class and ID under the owned host");
    SendMessageW(replacement,TBM_SETRANGEMIN,FALSE,minimum);SendMessageW(replacement,TBM_SETRANGEMAX,FALSE,maximum);SendMessageW(replacement,TBM_SETPOS,FALSE,position);
    Settle(app);
    {
        Invariant read(app,probe,messages);Snapshot changed(probe);const auto& replacementEntry=Entry(changed.value,"mms.catalog.main.control.455");
        Require(replacementEntry.target.instanceGeneration&&replacementEntry.target.instanceGeneration!=slider.target.instanceGeneration,"recreated actual child has a new instance even if USER32 reuses the HWND value");
        Stale(probe,slider.target);Resolve(probe,replacementEntry.target);Require(SnapshotBytes(initial.value)==initialBytes,"old lease survives control replacement");read.Check();
    }

    stage="production-project-epoch-retirement";
    Snapshot beforeReset(probe);const auto beforeResetBytes=SnapshotBytes(beforeReset.value);
    const auto prior=Entry(beforeReset.value,"org.mms.test.uicatalog.button").target;
    Require(plugins::ResetPluginProject(&app),"use existing production project reset boundary only on the owned empty fixture");
    Settle(app);
    {
        Invariant read(app,probe,messages);Stale(probe,prior);Snapshot afterReset(probe);
        const auto& newButton=Entry(afterReset.value,"org.mms.test.uicatalog.button");
        Require(afterReset.value.projectEpoch!=beforeReset.value.projectEpoch&&newButton.element==state.button&&newButton.target.projectEpoch==afterReset.value.projectEpoch,
            "project reset advances the original refresh epoch without replacing plugin registration identity");
        Resolve(probe,newButton.target);
        Require(SnapshotBytes(beforeReset.value)==beforeResetBytes&&SnapshotBytes(initial.value)==initialBytes,"old leases remain byte-stable across the actual project epoch change");
        Require(probe.State().editGate==MMS_E_NOT_FOUND,"ordinary editing service stays closed throughout discovery lifecycle checks");read.Check();
    }
    Require(!IsWindowVisible(app.MainWindow()),"host remains hidden after all lifecycle operations");
}
#endif
}

int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(argc!=2){std::fprintf(stderr,"usage: ui_catalog_host_tests <absolute-ui_catalog_probe.dll>\n");return 2;}
#if MMD_ORIGINAL_PROFILE
    std::puts("SKIP actual UI catalog host: original product deliberately closes plugin runtime");return 0;
#else
    SetProcessDPIAware();auto app=std::make_unique<MMDApp>();bool initialized=false,hooks=false;
    try{
        const auto directory=PreparePackage(argv[1]);
        Require(MH_Initialize()==MH_OK,"initialize own hidden-host guard hooks");hooks=true;
        Hook(reinterpret_cast<void*>(&ShowWindow),reinterpret_cast<void*>(Hidden),reinterpret_cast<void**>(&realShow));
        Hook(reinterpret_cast<void*>(&SetWindowPos),reinterpret_cast<void*>(HiddenPosition),reinterpret_cast<void**>(&realPosition));
        Hook(reinterpret_cast<void*>(&MessageBoxA),reinterpret_cast<void*>(UnexpectedA));Hook(reinterpret_cast<void*>(&MessageBoxW),reinterpret_cast<void*>(UnexpectedW));
        g_Block=app.get();std::memset(&app->state,0,sizeof(app->state));app->InitDefaults();
        Require(InitMainWindowAndD3D(app.get(),GetModuleHandleW(nullptr),SW_HIDE),"initialize real hidden MMS window, D3D9 and production plugin runtime");initialized=true;
        Run(*app);
        ShutdownCleanup(app.get());initialized=false;g_Block=nullptr;MH_Uninitialize();hooks=false;
        Require(GetModuleHandleW(L"ui_catalog_probe.dll")==nullptr,"production shutdown unloads the original DLL after every catalog lease was released");
        Require(fs::is_directory(directory/L"data")&&fs::is_regular_file(directory/L"PLUGINS/ui-catalog-probe/ui_catalog_probe.dll"),"fixture data and sole original package stay in the owned executable directory");
        std::printf("PASS actual UI catalog host: %u checks\n",checks);return 0;
    }catch(const std::exception& error){
        std::fprintf(stderr,"FAIL actual UI catalog host [%s]: %s\n",stage,error.what());
        if(initialized)ShutdownCleanup(app.get());g_Block=nullptr;if(hooks)MH_Uninitialize();return 1;
    }
#endif
}
