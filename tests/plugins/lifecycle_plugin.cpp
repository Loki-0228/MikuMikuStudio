#include <mms/ui.h>
#include <mms/surface.h>
#include <mms/scene.h>
#include <mms/storage.h>
#include <mms/cpp/refresh.hpp>
#include <string>
#include <cstring>
#include <thread>
#include <new>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
template<class T> static MmsHeader H() { return {sizeof(T), 1, 0}; }
static MmsString S(const char* s) { return {s, std::strlen(s)}; }
struct Plugin { MmsHostV1 host; MmsUiApiV1 ui{}; MmsUiSurfaceApiV1 surface{}; MmsSceneApiV1 scene{};
    mms::sdk::RefreshServices refresh; mms::sdk::RefreshTicket refreshTicket;
    MmsHandle snapshot=0; std::string id; MmsHandle command = 0, menu = 0, button = 0, factory = 0, panel = 0, panel2 = 0, serializer = 0; int stops = 0, projectValue = 0, candidateValue = 0; };
static Plugin* refreshProbe = nullptr;
static void Log(Plugin* p, const char* s) {
    p->host.log(p->host.context, 1, S(s));
    wchar_t path[32768]{}; auto n = GetEnvironmentVariableW(L"MMS_ABI_PROBE_LOG", path, 32768);
    if (n && n < 32768) {
        HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) { DWORD written; WriteFile(file, s, DWORD(std::strlen(s)), &written, nullptr); WriteFile(file, "\n", 1, &written, nullptr); CloseHandle(file); }
    }
}
struct Task { Plugin* plugin; MmsHandle handle = 0; bool orphan = false; };
struct PanelView { Plugin* plugin; HWND window; };
static MmsResult MMS_CALL CreatePanel(void* p, const MmsUiMountV1* m, void** view, uintptr_t* native) {
    *view = nullptr; *native = 0;
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"ABI custom panel", WS_CHILD | WS_CLIPCHILDREN,
        m->bounds.x, m->bounds.y, m->bounds.width, m->bounds.height, reinterpret_cast<HWND>(m->parentWindow), nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return MMS_E_FAILED;
    HWND input = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"中文输入测试", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 8, 8, 180, 24, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!input) { DestroyWindow(window); return MMS_E_FAILED; }
    auto* v = new(std::nothrow) PanelView{static_cast<Plugin*>(p), window}; if (!v) { DestroyWindow(window); return MMS_E_MEMORY; }
    SendMessageW(input, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
    *view = v; *native = reinterpret_cast<uintptr_t>(window); Log(v->plugin, "SURFACE_CREATE"); return MMS_OK;
}
static void MMS_CALL ResizePanel(void* p, const MmsUiRectV1* b, uint32_t) { auto* v = static_cast<PanelView*>(p); MoveWindow(v->window, b->x, b->y, b->width, b->height, TRUE); Log(v->plugin, "SURFACE_RESIZE"); }
static void MMS_CALL ShowPanel(void* p, uint32_t show) { auto* v = static_cast<PanelView*>(p); ShowWindow(v->window, show ? SW_SHOWNA : SW_HIDE); Log(v->plugin, show ? "SURFACE_SHOW" : "SURFACE_HIDE"); }
static MmsResult MMS_CALL FocusPanel(void* p) { auto* v = static_cast<PanelView*>(p); SetFocus(GetWindow(v->window, GW_CHILD)); Log(v->plugin, "SURFACE_FOCUS"); return MMS_OK; }
static void MMS_CALL DestroyPanel(void* p) { auto* v = static_cast<PanelView*>(p); Log(v->plugin, "SURFACE_DESTROY"); DestroyWindow(v->window); delete v; }
static void MMS_CALL Run(void* context) {
    auto* task = static_cast<Task*>(context); auto* p = task->plugin;
    if (p->host.cancelUi(p->host.context, task->handle) == MMS_E_BUSY) Log(p, "RUNNING_CANCEL_BUSY");
    Log(p, "TASK_RUN"); if (task->orphan) delete p; delete task;
}
static void MMS_CALL Cancel(void* context) { auto* task = static_cast<Task*>(context); Log(task->plugin, "TASK_CANCEL"); if (task->orphan) delete task->plugin; delete task; }
static MmsResult MMS_CALL State(void*, MmsObjectRef, MmsCommandStateV1* out) { *out = {H<MmsCommandStateV1>(), 1, 0, 1, 0, {}}; return MMS_OK; }
static MmsResult ReadScene(Plugin* p, bool retain) {
    auto r=p->host.queryService(p->host.context,S("mms.scene"),1,0,sizeof(p->scene),&p->scene); if(r) return r;
    MmsSceneQueryV1 query{};query.h=H<MmsSceneQueryV1>();query.flags=MMS_SCENE_CURRENT_TIME;query.channels=MMS_CHANNEL_BONES;
    MmsSnapshotV1 snapshot{};r=p->scene.acquire(p->scene.context,&query,&snapshot);if(r) return r;
    MmsSpan objects{};r=p->scene.objects(p->scene.context,snapshot.handle,&objects);
    if(retain && !r) p->snapshot=snapshot.handle;else p->scene.release(p->scene.context,snapshot.handle);
    if(!r) Log(p,"SCENE_READ_PASS");return r;
}
static MmsResult HideLayout(Plugin* p) {
        MmsUiSnapshotV1 snapshot{};snapshot.h=H<MmsUiSnapshotV1>();
        auto result=p->ui.acquireSnapshot(p->ui.context,0,&snapshot);if(result)return result;
        const auto id=p->id+".button";uint64_t generation=0;const auto revision=snapshot.revision;
        const auto* elements=static_cast<const MmsUiElementV1*>(snapshot.elements.data);
        for(size_t i=0;i<snapshot.elements.count;++i)if(std::string(elements[i].id.data,size_t(elements[i].id.bytes))==id)generation=elements[i].generation;
        p->ui.releaseSnapshot(p->ui.context,snapshot.handle);if(!generation)return MMS_E_NOT_FOUND;
        MmsHandle tx=0;result=p->ui.beginLayout(p->ui.context,revision,&tx);if(result)return result;
        MmsLayoutOpV1 op{};op.h=H<MmsLayoutOpV1>();op.operation=MMS_LAYOUT_VISIBLE;
        op.elementId={id.data(),id.size()};op.expectedGeneration=generation;op.value.visible=0;
        result=p->ui.applyLayout(p->ui.context,tx,&op);
        if(result){p->ui.cancelLayout(p->ui.context,tx);return result;}
        result=p->ui.commitLayout(p->ui.context,tx);if(!result)Log(p,"LAYOUT_HIDDEN");return result;
}
static void MMS_CALL ApplyLayout(void* context) {
    auto* p=static_cast<Plugin*>(context);
    if(HideLayout(p)!=MMS_OK)Log(p,"LAYOUT_FAILED");
}
static void MMS_CALL CancelLayout(void*) {}
static MmsResult QueueRefresh(Plugin* p) {
    if (p->refreshTicket) return MMS_E_BUSY;
    MmsUiRefreshTargetV1 target{}; target.h = H<MmsUiRefreshTargetV1>();
    auto result = p->refresh.Target(p->button, target);
    if (result) return result;
    Log(p, "REFRESH_TARGET_IN_COMMAND");
    std::thread worker([&] {
        MmsUiDirtyRectV1 rect{3, 4, 11, 9};
        result = p->refresh.RepaintRect(target, rect, p->refreshTicket, 17);
        if (!result) {
            MmsUiRefreshStateV1 state{}; state.h = H<MmsUiRefreshStateV1>();
            result = p->refreshTicket.Query(state);
            if (!result && state.status != MMS_UI_REFRESH_PENDING) result = MMS_E_FAILED;
        }
    });
    worker.join();
    Log(p, result ? "REFRESH_WORKER_FAILED" : "REFRESH_WORKER_PENDING");
    if (!result && p->id.find("auto-release") != std::string::npos) {
        result = p->refreshTicket.Release();
        Log(p, result ? "REFRESH_RELEASE_FAILED" : "REFRESH_RELEASED");
    }
    return result;
}
static MmsResult MMS_CALL Invoke(void* context, MmsObjectRef, MmsSpan, MmsHandle) {
    auto* p=static_cast<Plugin*>(context);Log(p,"COMMAND_INVOKED");
    if (p->id.find("gui-refresh") != std::string::npos) return QueueRefresh(p);
    if(p->id.find("gui-layout")!=std::string::npos) {
        MmsHandle task=0;return p->host.enqueueUi(p->host.context,ApplyLayout,CancelLayout,p,&task);
    }
    if(p->serializer)++p->projectValue;
    return p->id.find("scene")!=std::string::npos?ReadScene(p,false):MMS_OK;
}
static MmsResult RegisterSerializer(Plugin* p){
    MmsSerializerV1 s{H<MmsSerializerV1>(),p};
    s.prepareLoad=[](void* context,MmsBytes bytes,uint32_t schema,MmsHandle* out)->MmsResult{
        auto* p=static_cast<Plugin*>(context);if(schema && (schema!=1 || bytes.bytes!=sizeof(int)))return MMS_E_VERSION;
        p->candidateValue=0;if(schema)std::memcpy(&p->candidateValue,bytes.data,sizeof(int));*out=1;Log(p,"SERIALIZER_PREPARE");return MMS_OK;
    };
    s.commitLoad=[](void* context,MmsHandle){auto* p=static_cast<Plugin*>(context);p->projectValue=p->candidateValue;Log(p,"SERIALIZER_COMMIT");};
    s.cancelLoad=[](void* context,MmsHandle){Log(static_cast<Plugin*>(context),"SERIALIZER_CANCEL");};
    s.save=[](void* context,MmsBytes* bytes,uint32_t* schema,MmsHandle* out)->MmsResult{
        auto* p=static_cast<Plugin*>(context);wchar_t fail[8]{};if(GetEnvironmentVariableW(L"MMS_TEST_SERIALIZER_FAIL",fail,8))return MMS_E_FAILED;
        *bytes={&p->projectValue,sizeof(int)};*schema=1;*out=2;Log(p,"SERIALIZER_SAVE");return MMS_OK;
    };
    s.releaseBytes=[](void* context,MmsHandle){Log(static_cast<Plugin*>(context),"SERIALIZER_RELEASE");};
    return p->host.registerProvider(p->host.context,S("mms.serializer"),{p->id.data(),p->id.size()},&s,sizeof(s),&p->serializer);
}
static MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1* info, void** out) {
    *out = nullptr; auto* p = new Plugin{*host}; p->id.assign(info->pluginId.data, size_t(info->pluginId.bytes));
    auto r = host->queryService(host->context, S("mms.ui"), 1, 0, sizeof(p->ui), &p->ui); if (r) { delete p; return r; }
    unsigned char buffer[256], original[256]; std::memset(buffer, 0x5a, sizeof(buffer)); std::memcpy(original, buffer, sizeof(buffer));
    if (host->queryService(host->context, S("mms.ui"), 2, 0, sizeof(buffer), buffer) != MMS_E_VERSION || std::memcmp(buffer, original, sizeof(buffer))) { delete p; return MMS_E_FAILED; }
    if (host->queryService(host->context, S("mms.ui"), 1, 1, sizeof(buffer), buffer) != MMS_E_VERSION || std::memcmp(buffer, original, sizeof(buffer))) { delete p; return MMS_E_FAILED; }
    if (host->queryService(host->context, S("mms.ui"), 1, 0, 8, buffer) != MMS_E_ARGUMENT || std::memcmp(buffer, original, sizeof(buffer))) { delete p; return MMS_E_FAILED; }
    if (host->queryService(host->context, S("mms.test.unavailable"), 1, 0, sizeof(buffer), buffer) != MMS_E_NOT_FOUND || std::memcmp(buffer, original, sizeof(buffer))) { delete p; return MMS_E_FAILED; }
    if (host->queryService(host->context, S("mms.ui"), 1, 0, sizeof(buffer), buffer) != MMS_OK || std::memcmp(buffer + sizeof(MmsUiApiV1), original + sizeof(MmsUiApiV1), sizeof(buffer) - sizeof(MmsUiApiV1))) { delete p; return MMS_E_FAILED; }
    Log(p, "NEGOTIATION_PASS");
    if (p->id.find("gui-refresh") != std::string::npos) {
        r = mms::sdk::RefreshServices::Query(*host, p->refresh);
        if (r) { delete p; return r; }
        MmsUiRefreshCapabilitiesV1 capabilities{}; capabilities.h = H<MmsUiRefreshCapabilitiesV1>();
        r = p->refresh.Capabilities(capabilities);
        if (r || capabilities.supportedKinds != MMS_UI_REFRESH_REPAINT || capabilities.supportedScopes != MMS_UI_REFRESH_SELF) { delete p; return r ? r : MMS_E_FAILED; }
        if (host->queryService(host->context, S("mms.edit"), 1, 0, sizeof(buffer), buffer) != MMS_E_NOT_FOUND) { delete p; return MMS_E_FAILED; }
        Log(p, "REFRESH_SERVICE_PASS");
    }
    if (p->id.find("failcreate") != std::string::npos) {
        auto* task = new Task{p, 0, true}; r = host->enqueueUi(host->context, Run, Cancel, task, &task->handle);
        if (r) { delete task; delete p; return r; }
        Log(p, "CREATE_TASK_ACCEPTED"); return MMS_E_FAILED;
    }
    *out = p; return MMS_OK;
}
static MmsResult MMS_CALL Start(void* context) {
    auto* p = static_cast<Plugin*>(context); MmsCommandDescV1 command{}; command.info.h = H<MmsCommandDescV1>();
    const auto id = p->id + ".command"; command.info.id = {id.data(), id.size()}; command.info.ownerPlugin = {p->id.data(), p->id.size()}; command.info.label = S("Lifecycle test");
    command.context = p; command.getState = State; command.invoke = Invoke;
    auto r = p->ui.registerCommand(p->ui.context, &command, &p->command); if (r) return r;
    if(p->id.find("storage")!=std::string::npos){r=RegisterSerializer(p);if(r)return r;}
    if(p->id.find("scenelease")!=std::string::npos) {r=ReadScene(p,true);if(r)return r;}
    if (p->id.find("failstart") != std::string::npos) return MMS_E_FAILED;
    if (p->id.find("gui") != std::string::npos) {
        MmsUiElementV1 e{}; e.h = H<MmsUiElementV1>(); const auto itemId = p->id + ".menu";
        e.id = {itemId.data(), itemId.size()}; e.ownerPlugin = command.info.ownerPlugin; e.label = S("ABI GUI probe");
        e.parentId = S("mms.menu.plugins"); e.commandId = command.info.id; e.kind = MMS_UI_MENU_ITEM;
        e.capabilities = MMS_UI_HIDE | MMS_UI_MOVE; r = p->ui.registerElement(p->ui.context, &e, &p->menu); if (r) return r;
        const auto buttonId = p->id + ".button"; e.id = {buttonId.data(), buttonId.size()}; e.parentId = S("mms.toolbar.main.extensions"); e.kind = MMS_UI_BUTTON;
        r = p->ui.registerElement(p->ui.context, &e, &p->button); if (r) return r; Log(p, "GUI_REGISTERED");
    }
    if (p->id.find("queue") != std::string::npos) {
        auto* run = new Task{p}; auto* cancel = new Task{p};
        std::thread worker([&] {
            r = p->host.enqueueUi(p->host.context, Run, Cancel, run, &run->handle);
            if (!r) r = p->host.enqueueUi(p->host.context, Run, Cancel, cancel, &cancel->handle);
            if (!r) r = p->host.cancelUi(p->host.context, cancel->handle);
        }); worker.join(); if (r) return r;
    }
    if (p->id.find("panel") != std::string::npos) {
        r = p->host.queryService(p->host.context, S("mms.ui.surface"), 1, 0, sizeof(p->surface), &p->surface); if (r) return r;
        const auto factory = p->id + ".factory", panel = p->id + ".panel", panel2 = p->id + ".panel2";
        MmsUiSurfaceDescV1 d{}; d.h = H<MmsUiSurfaceDescV1>(); d.id = {factory.data(), factory.size()}; d.context = p; d.flags = MMS_SURFACE_MULTIPLE_VIEWS;
        d.minimumWidth = 200; d.minimumHeight = 48; d.preferredWidth = 320; d.preferredHeight = 100;
        d.createView = CreatePanel; d.resizeView = ResizePanel; d.showView = ShowPanel; d.focusView = FocusPanel; d.destroyView = DestroyPanel;
        r = p->surface.registerSurface(p->surface.context, &d, &p->factory); if (r) return r;
        MmsUiElementV1 e{}; e.h = H<MmsUiElementV1>(); e.id = {panel.data(), panel.size()}; e.ownerPlugin = {p->id.data(), p->id.size()}; e.label = S("自定义面板一");
        e.parentId = S("mms.workspace.tabs.extensions"); e.surfaceId = d.id; e.kind = MMS_UI_PANEL; e.capabilities = MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_RESIZE;
        r = p->ui.registerElement(p->ui.context, &e, &p->panel); if (r) return r;
        e.id = {panel2.data(), panel2.size()}; e.label = S("自定义面板二");
        r = p->ui.registerElement(p->ui.context, &e, &p->panel2); if (r) return r;
        Log(p, "PANELS_REGISTERED");
    }
    if (p->id.find("gui-refresh") != std::string::npos) refreshProbe = p;
    return MMS_OK;
}
static void MMS_CALL Event(void* context, const MmsEventV1* event) {
    auto* p=static_cast<Plugin*>(context);
    if(event->kind==MMS_EVENT_UI_CLOSING)Log(p,"UI_CLOSING");
    if(event->kind==MMS_EVENT_UI_CLOSING && p->id.find("gui-layout")!=std::string::npos) {
        wchar_t enabled[8]{};
        if(GetEnvironmentVariableW(L"MMS_UI_LAYOUT_ON_CLOSE",enabled,8) && HideLayout(p)!=MMS_OK)Log(p,"LAYOUT_CLOSE_FAILED");
    }
    if(p->serializer && (event->kind==MMS_EVENT_PROJECT_OPENED || event->kind==MMS_EVENT_PROJECT_SAVED)){
        if(event->payloadVersion!=1 || event->payload.bytes!=sizeof(MmsProjectEventV1)){Log(p,"PROJECT_PAYLOAD_FAIL");return;}
        auto& project=*static_cast<const MmsProjectEventV1*>(event->payload.data);
        if(!(project.projectId.lo||project.projectId.hi) || !project.path.bytes){Log(p,"PROJECT_PAYLOAD_FAIL");return;}
        MmsStorageApiV1 api{};MmsBytes bytes{};uint32_t schema=0;MmsHandle lease=0;
        if(p->host.queryService(p->host.context,S("mms.storage"),1,0,sizeof(api),&api) || api.readSaved(api.context,&bytes,&schema,&lease)){Log(p,"SAVED_READ_FAIL");return;}
        const bool same=schema==1 && bytes.bytes==sizeof(int) && !std::memcmp(bytes.data,&p->projectValue,sizeof(int));
        api.releaseSaved(api.context,lease);
        Log(p,same?(event->kind==MMS_EVENT_PROJECT_OPENED?"PROJECT_OPENED_PASS":"PROJECT_SAVED_PASS"):"SAVED_BYTES_FAIL");
        if(ReadScene(p,false))Log(p,"PROJECT_SCENE_FAIL");
    }
}
static MmsResult MMS_CALL Stop(void* context) {
    auto* p = static_cast<Plugin*>(context); ++p->stops;
    if (p->refreshTicket) {
        // Deliberately return success. The host's outstanding-ticket guard,
        // rather than a plugin BUSY result, must retain this module/context.
        Log(p, "STOP_REFRESH_TICKET_RETAINED"); return MMS_OK;
    }
    if(p->snapshot && p->stops>1) {
        std::thread worker([&] {MmsSpan objects{};
            if(p->scene.objects(p->scene.context,p->snapshot,&objects)==MMS_OK)Log(p,"STOP_SCENE_READ_PASS");
            p->scene.release(p->scene.context,p->snapshot);p->snapshot=0;
        });worker.join();
    }
    if (p->id.find("busy") != std::string::npos && p->stops < 3) { Log(p, "STOP_BUSY"); return MMS_E_BUSY; }
    if(p->serializer){auto r=p->host.unregisterProvider(p->host.context,p->serializer);if(r)return r;p->serializer=0;}
    for (auto* item : {&p->panel, &p->panel2, &p->button, &p->menu}) if (*item) { auto r = p->ui.unregisterItem(p->ui.context, *item); if (r) return r; *item = 0; }
    if (p->factory) { auto r = p->surface.unregisterSurface(p->surface.context, p->factory); if (r) return r; p->factory = 0; }
    if (p->command) { auto r = p->ui.unregisterItem(p->ui.context, p->command); if (r) return r; p->command = 0; }
    Log(p, "STOP_OK"); return MMS_OK;
}
static void MMS_CALL Destroy(void* context) { auto* p = static_cast<Plugin*>(context); Log(p, "DESTROY"); if (refreshProbe == p) refreshProbe = nullptr; delete p; }
extern "C" __declspec(dllexport) MmsResult MMS_CALL RefreshProbeState(MmsUiRefreshStateV1* out) {
    if (!refreshProbe || !refreshProbe->refreshTicket) return MMS_E_NOT_READY;
    if (!out) return MMS_E_ARGUMENT;
    return refreshProbe->refreshTicket.Query(*out);
}
extern "C" __declspec(dllexport) MmsResult MMS_CALL RefreshProbeRelease() {
    if (!refreshProbe || !refreshProbe->refreshTicket) return MMS_E_NOT_READY;
    return refreshProbe->refreshTicket.Release();
}
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t ma, uint32_t mi, uint32_t n, MmsPluginApiV1* out) {
    if (!out || n < sizeof(*out)) return MMS_E_ARGUMENT; if (ma != 1 || mi) return MMS_E_VERSION;
    *out = {H<MmsPluginApiV1>(), Create, Start, Event, Stop, Destroy}; return MMS_OK;
}
