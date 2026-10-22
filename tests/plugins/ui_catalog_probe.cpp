// Original integration fixture. All discovery/identity calls cross negotiated
// public C ABI tables; this DLL never receives a registry, HWND or host pointer.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mms/ui_catalog.h>
#include <mms/layout.h>
#include <mms/edit.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {
template<class T> constexpr MmsHeader H() { return {sizeof(T),1,0}; }
MmsString S(const char* text) { return {text,std::strlen(text)}; }
MmsString S(const std::string& text) { return {text.data(),text.size()}; }
template<class F> MmsResult Guard(F&& f) noexcept {
    try { return f(); } catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (...) { return MMS_E_FAILED; }
}
// Duplicated in the host fixture; contains no private host pointers.
struct ProbeState {
    uint32_t size,readyEvents,stateCalls,invokes,acquires,resolves,releases;
    MmsResult editGate;
    MmsHandle command,button,menu;
    uint32_t startupScopes,startupEntries;
};
static_assert(sizeof(ProbeState)==64,"catalog probe protocol");
struct AccessResults {
    uint32_t size;
    MmsResult refreshGet,refreshRequest,layoutBegin,layoutApply,layoutCommit,unregisterNative;
    uint32_t reserved;
};
static_assert(sizeof(AccessResults)==32,"catalog access protocol");
struct Plugin {
    MmsHostV1 host{}; MmsUiApiV1 ui{}; MmsUiCatalogApiV1 catalog{};
    MmsUiRefreshApiV1 refresh{};
    std::string id,commandId,buttonId,menuId;
    DWORD thread=GetCurrentThreadId();
    ProbeState state{sizeof(ProbeState),0,0,0,0,0,0,MMS_E_FAILED,0,0,0,0,0};
    std::vector<MmsHandle> leases;
};
Plugin* current=nullptr;
MmsResult Ready() {
    if(!current)return MMS_E_NOT_READY;
    return GetCurrentThreadId()==current->thread?MMS_OK:MMS_E_THREAD;
}
MmsResult MMS_CALL CommandState(void* context,MmsObjectRef,MmsCommandStateV1* out) {
    if(!out)return MMS_E_ARGUMENT;
    ++static_cast<Plugin*>(context)->state.stateCalls;
    *out={H<MmsCommandStateV1>(),1,0,1,0,{}};return MMS_OK;
}
MmsResult MMS_CALL Invoke(void* context,MmsObjectRef,MmsSpan,MmsHandle) {
    ++static_cast<Plugin*>(context)->state.invokes;return MMS_OK;
}
MmsResult MMS_CALL Create(const MmsHostV1* host,const MmsCreateInfoV1* info,void** out) {
    if(!host||!info||!out||!host->queryService||current)return MMS_E_ARGUMENT;
    return Guard([&]() -> MmsResult {
        auto p=std::make_unique<Plugin>();p->host=*host;
        p->id.assign(info->pluginId.data,size_t(info->pluginId.bytes));
        p->commandId=p->id+".invoke";p->buttonId=p->id+".button";p->menuId=p->id+".menu";
        p->leases.reserve(16);
        auto r=host->queryService(host->context,S("mms.ui"),1,0,sizeof(p->ui),&p->ui);if(r)return r;
        r=host->queryService(host->context,S("mms.ui.catalog"),1,0,sizeof(p->catalog),&p->catalog);if(r)return r;
        r=host->queryService(host->context,S("mms.ui.refresh"),1,0,sizeof(p->refresh),&p->refresh);if(r)return r;
        MmsEditApiV1 edit{};
        p->state.editGate=host->queryService(host->context,S("mms.edit"),1,0,sizeof(edit),&edit);
        // Validate that the host published its own UI before Manager starts
        // loading plugins. Registered fixture presentations are checked only
        // after Start and the host's ordinary safe presentation boundary.
        MmsUiCatalogSnapshotV1 initial{};initial.h=H<MmsUiCatalogSnapshotV1>();
        r=p->catalog.acquireSnapshot(p->catalog.context,MMS_UI_CATALOG_MAIN_MENU|MMS_UI_CATALOG_MAIN_CONTROLS,&initial);if(r)return r;
        p->state.startupScopes=initial.availableScopes;
        p->state.startupEntries=static_cast<uint32_t>(initial.elements.count);
        r=p->catalog.releaseSnapshot(p->catalog.context,initial.handle);if(r)return r;
        *out=p.get();current=p.release();return MMS_OK;
    });
}
MmsResult MMS_CALL Start(void* context) {
    return Guard([&]() -> MmsResult {
        auto& p=*static_cast<Plugin*>(context);
        MmsCommandDescV1 command{};command.info.h=H<MmsCommandDescV1>();
        command.info.id=S(p.commandId);command.info.ownerPlugin=S(p.id);
        command.info.label=S("Catalog fixture command");command.info.flags=MMS_COMMAND_INVOKE_ONLY;
        command.context=&p;command.getState=CommandState;command.invoke=Invoke;
        auto r=p.ui.registerCommand(p.ui.context,&command,&p.state.command);if(r)return r;
        MmsUiElementV1 button{};button.h=H<MmsUiElementV1>();
        button.id=S(p.buttonId);button.ownerPlugin=S(p.id);button.parentId=S("mms.toolbar.main.extensions");
        button.label=S("Catalog fixture toolbar button");button.commandId=S(p.commandId);
        button.kind=MMS_UI_BUTTON;button.capabilities=MMS_UI_LAYOUT|MMS_UI_MOVE|MMS_UI_HIDE|MMS_UI_RESIZE;
        r=p.ui.registerElement(p.ui.context,&button,&p.state.button);if(r)return r;
        MmsUiElementV1 menu{};menu.h=H<MmsUiElementV1>();
        menu.id=S(p.menuId);menu.ownerPlugin=S(p.id);menu.parentId=S("mms.menu.plugins");
        menu.label=S("Catalog fixture plugin menu");menu.commandId=S(p.commandId);menu.kind=MMS_UI_MENU_ITEM;
        return p.ui.registerElement(p.ui.context,&menu,&p.state.menu);
    });
}
void MMS_CALL Event(void* context,const MmsEventV1* event) {
    if(event&&event->kind==MMS_EVENT_UI_READY)++static_cast<Plugin*>(context)->state.readyEvents;
}
MmsResult MMS_CALL Stop(void* context) {
    return Guard([&]() -> MmsResult {
        auto& p=*static_cast<Plugin*>(context);
        for(auto handle:p.leases){const auto r=p.catalog.releaseSnapshot(p.catalog.context,handle);if(r)return r;}
        p.leases.clear();
        for(auto* handle:{&p.state.menu,&p.state.button,&p.state.command}){
            if(!*handle)continue;const auto r=p.ui.unregisterItem(p.ui.context,*handle);
            if(r&&r!=MMS_E_NOT_FOUND&&r!=MMS_E_STALE)return r;*handle=0;
        }
        return MMS_OK;
    });
}
void MMS_CALL Destroy(void* context) {auto* p=static_cast<Plugin*>(context);if(current==p)current=nullptr;delete p;}
MmsResult TrackLease(Plugin& p,MmsHandle handle) {
    try {p.leases.push_back(handle);return MMS_OK;}
    catch(...){p.catalog.releaseSnapshot(p.catalog.context,handle);return MMS_E_MEMORY;}
}
}

extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major,uint32_t minor,uint32_t bytes,MmsPluginApiV1* out) {
    if(!out||bytes<sizeof(*out))return MMS_E_ARGUMENT;
    if(major!=1||minor)return MMS_E_VERSION;
    *out={H<MmsPluginApiV1>(),Create,Start,Event,Stop,Destroy};return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL UiCatalogProbeState(ProbeState* out) {
    if(const auto r=Ready())return r;if(!out||out->size!=sizeof(*out))return MMS_E_ARGUMENT;
    *out=current->state;return MMS_OK;
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL UiCatalogProbeCapabilities(MmsUiCatalogCapabilitiesV1* out) {
    if(const auto r=Ready())return r;
    return Guard([&]{return current->catalog.getCapabilities(current->catalog.context,out);});
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL UiCatalogProbeAcquire(uint32_t scopes,MmsUiCatalogSnapshotV1* out) {
    if(const auto r=Ready())return r;if(!out)return MMS_E_ARGUMENT;
    return Guard([&]() -> MmsResult {
        MmsUiCatalogSnapshotV1 snapshot{};snapshot.h=out->h;
        auto& p=*current;const auto r=p.catalog.acquireSnapshot(p.catalog.context,scopes,&snapshot);if(r)return r;
        if(const auto tracked=TrackLease(p,snapshot.handle))return tracked;
        ++p.state.acquires;*out=snapshot;return MMS_OK;
    });
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL UiCatalogProbeResolve(const MmsUiTargetRefV1* expected,MmsUiCatalogEntryV1* out,MmsHandle* lease) {
    if(const auto r=Ready())return r;if(!out||!lease)return MMS_E_ARGUMENT;
    return Guard([&]() -> MmsResult {
        MmsUiCatalogEntryV1 entry{};entry.h=out->h;MmsHandle handle=0;
        auto& p=*current;const auto r=p.catalog.resolveElement(p.catalog.context,expected,&entry,&handle);if(r)return r;
        if(const auto tracked=TrackLease(p,handle))return tracked;
        ++p.state.resolves;*out=entry;*lease=handle;return MMS_OK;
    });
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL UiCatalogProbeRelease(MmsHandle lease) {
    if(const auto r=Ready())return r;
    return Guard([&]() -> MmsResult {
        auto& p=*current;const auto r=p.catalog.releaseSnapshot(p.catalog.context,lease);if(r)return r;
        const auto found=std::find(p.leases.begin(),p.leases.end(),lease);
        if(found!=p.leases.end())p.leases.erase(found);++p.state.releases;return MMS_OK;
    });
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL UiCatalogProbeReadOnly(const MmsUiCatalogEntryV1* native,AccessResults* out) {
    if(const auto r=Ready())return r;if(!native||!out||out->size!=sizeof(*out))return MMS_E_ARGUMENT;
    return Guard([&]() -> MmsResult {
        auto& p=*current;AccessResults result{sizeof(AccessResults),MMS_E_FAILED,MMS_E_FAILED,MMS_E_FAILED,MMS_E_FAILED,MMS_E_FAILED,MMS_E_FAILED,0};
        MmsUiRefreshTargetV1 target{};target.h=H<MmsUiRefreshTargetV1>();
        result.refreshGet=p.refresh.getTarget(p.refresh.context,native->element,&target);
        MmsUiRefreshRequestV1 request{};request.h=H<MmsUiRefreshRequestV1>();request.target=native->target;
        request.kinds=MMS_UI_REFRESH_REPAINT;request.scope=MMS_UI_REFRESH_SELF;
        MmsHandle ticket=0;result.refreshRequest=p.refresh.request(p.refresh.context,&request,&ticket);
        if(result.refreshRequest==MMS_OK){p.refresh.cancel(p.refresh.context,ticket);p.refresh.release(p.refresh.context,ticket);}
        MmsUiSnapshotV1 snapshot{};snapshot.h=H<MmsUiSnapshotV1>();
        const auto acquired=p.ui.acquireSnapshot(p.ui.context,0,&snapshot);if(acquired)return acquired;
        const auto revision=snapshot.revision;p.ui.releaseSnapshot(p.ui.context,snapshot.handle);
        MmsHandle transaction=0;result.layoutBegin=p.ui.beginLayout(p.ui.context,revision,&transaction);
        if(result.layoutBegin==MMS_OK){
            MmsLayoutOpV1 op{};op.h=H<MmsLayoutOpV1>();op.operation=MMS_LAYOUT_VISIBLE;
            op.elementId=native->id;op.expectedGeneration=native->descriptionGeneration;op.value.visible=0;
            result.layoutApply=p.ui.applyLayout(p.ui.context,transaction,&op);
            if(result.layoutApply==MMS_OK)result.layoutCommit=p.ui.commitLayout(p.ui.context,transaction);
            else p.ui.cancelLayout(p.ui.context,transaction);
            if(result.layoutCommit!=MMS_OK)p.ui.cancelLayout(p.ui.context,transaction);
        }
        result.unregisterNative=p.ui.unregisterItem(p.ui.context,native->element);
        *out=result;return MMS_OK;
    });
}
