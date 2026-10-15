#include <mms/ui.h>
#include <mms/surface.h>
#include <string>
#include <cstring>
#include <thread>
#include <new>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
template<class T> static MmsHeader H() { return {sizeof(T), 1, 0}; }
static MmsString S(const char* s) { return {s, std::strlen(s)}; }
struct Plugin { MmsHostV1 host; MmsUiApiV1 ui{}; MmsUiSurfaceApiV1 surface{}; std::string id; MmsHandle command = 0, menu = 0, button = 0, factory = 0, panel = 0, panel2 = 0; int stops = 0; };
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
static MmsResult MMS_CALL Invoke(void* context, MmsObjectRef, MmsSpan, MmsHandle) { Log(static_cast<Plugin*>(context), "COMMAND_INVOKED"); return MMS_OK; }
static MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1* info, void** out) {
    *out = nullptr; auto* p = new Plugin{*host}; p->id.assign(info->pluginId.data, size_t(info->pluginId.bytes));
    auto r = host->queryService(host->context, S("mms.ui"), 1, 0, sizeof(p->ui), &p->ui); if (r) { delete p; return r; }
    unsigned char buffer[256], original[256]; std::memset(buffer, 0x5a, sizeof(buffer)); std::memcpy(original, buffer, sizeof(buffer));
    if (host->queryService(host->context, S("mms.ui"), 2, 0, sizeof(buffer), buffer) != MMS_E_VERSION || std::memcmp(buffer, original, sizeof(buffer))) { delete p; return MMS_E_FAILED; }
    if (host->queryService(host->context, S("mms.ui"), 1, 1, sizeof(buffer), buffer) != MMS_E_VERSION || std::memcmp(buffer, original, sizeof(buffer))) { delete p; return MMS_E_FAILED; }
    if (host->queryService(host->context, S("mms.ui"), 1, 0, 8, buffer) != MMS_E_ARGUMENT || std::memcmp(buffer, original, sizeof(buffer))) { delete p; return MMS_E_FAILED; }
    if (host->queryService(host->context, S("mms.scene"), 1, 0, sizeof(buffer), buffer) != MMS_E_NOT_FOUND || std::memcmp(buffer, original, sizeof(buffer))) { delete p; return MMS_E_FAILED; }
    if (host->queryService(host->context, S("mms.ui"), 1, 0, sizeof(buffer), buffer) != MMS_OK || std::memcmp(buffer + sizeof(MmsUiApiV1), original + sizeof(MmsUiApiV1), sizeof(buffer) - sizeof(MmsUiApiV1))) { delete p; return MMS_E_FAILED; }
    Log(p, "NEGOTIATION_PASS");
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
    return MMS_OK;
}
static void MMS_CALL Event(void* context, const MmsEventV1* event) { if (event->kind == MMS_EVENT_UI_CLOSING) Log(static_cast<Plugin*>(context), "UI_CLOSING"); }
static MmsResult MMS_CALL Stop(void* context) {
    auto* p = static_cast<Plugin*>(context); ++p->stops;
    if (p->id.find("busy") != std::string::npos && p->stops < 3) { Log(p, "STOP_BUSY"); return MMS_E_BUSY; }
    for (auto* item : {&p->panel, &p->panel2, &p->button, &p->menu}) if (*item) { auto r = p->ui.unregisterItem(p->ui.context, *item); if (r) return r; *item = 0; }
    if (p->factory) { auto r = p->surface.unregisterSurface(p->surface.context, p->factory); if (r) return r; p->factory = 0; }
    if (p->command) { auto r = p->ui.unregisterItem(p->ui.context, p->command); if (r) return r; p->command = 0; }
    Log(p, "STOP_OK"); return MMS_OK;
}
static void MMS_CALL Destroy(void* context) { auto* p = static_cast<Plugin*>(context); Log(p, "DESTROY"); delete p; }
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t ma, uint32_t mi, uint32_t n, MmsPluginApiV1* out) {
    if (!out || n < sizeof(*out)) return MMS_E_ARGUMENT; if (ma != 1 || mi) return MMS_E_VERSION;
    *out = {H<MmsPluginApiV1>(), Create, Start, Event, Stop, Destroy}; return MMS_OK;
}
