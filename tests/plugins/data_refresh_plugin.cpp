// Test-only real plugin. It negotiates public services and never accesses
// manager/registry internals, project data, files or a renderer.
#include "data_refresh_probe.h"
#include <mms/cpp/data_refresh.hpp>
#include <mms/cpp/state_content.hpp>
#include <mms/surface.h>
#include <mms/edit.h>
#include <mms/layout_session.h>
#include <mms/cpp/layout_session.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <stdexcept>
#include <thread>

namespace {
using namespace mms::sdk;
struct Plugin {
    MmsHostV1 host{};MmsUiApiV1 ui{};MmsUiSurfaceApiV1 surface{};MmsEditApiV1 edit{};
    RefreshServices refresh;DataServices data;DataSource source;DataBinding binding;
    StateContentServices stateContent;
    // Retain an SDK session if explicit failure cleanup cannot release it.
    // Its destructor deliberately does not call the host.
    LayoutSession sdkLayoutSession;
    RefreshTicket ticket;DataDisplay held;
    std::string id;MmsHandle command=0,menu=0,button=0,panel=0,factory=0;
    HINSTANCE module=nullptr;bool registeredClass=false;
    DataRefreshProbePrepareHookFn prepareHook=nullptr;void* prepareHookContext=nullptr;
    DataRefreshProbeState state{};
    DataRefreshProbeLayoutState layoutState{};
    DataRefreshProbeLayoutHookFn layoutHook=nullptr;void* layoutHookContext=nullptr;
    uint32_t layoutVisible=1;
};
Plugin* current=nullptr;
constexpr wchar_t kPanelClass[]=L"MMS.DataRefresh.Probe.View.V1";
MmsString S(const char* text){return {text,std::strlen(text)};}
MmsString S(const std::string& text){return {text.data(),text.size()};}
void Log(Plugin& p,const char* text){if(p.host.log)p.host.log(p.host.context,1,S(text));}
int DisplayInt(const DataDisplay& display,int fallback=-1234567){const auto bytes=display.bytes();int result=fallback;
    if(bytes.bytes==sizeof(result)&&bytes.data)std::memcpy(&result,bytes.data,sizeof(result));return result;}
MmsResult Request(Plugin& p,std::uint32_t kinds,RefreshTicket& out){
    if(kinds&MMS_UI_REFRESH_DATA){if(!p.state.sourceRevision)return MMS_E_NOT_READY;
        return p.binding.Refresh(p.refresh,p.state.target,p.state.sourceRevision,out,kinds,401);}
    MmsUiRefreshRequestV1 request{};request.h=Header<MmsUiRefreshRequestV1>();request.target=p.state.target;
    request.kinds=kinds;request.scope=MMS_UI_REFRESH_SELF;request.coalesceKey=402;
    return p.refresh.Request(request,out);
}
MmsResult AttemptRefresh(Plugin& p,std::uint32_t kinds){RefreshTicket ticket;const auto result=Request(p,kinds,ticket);
    // Clean an unexpected acceptance as well. The test still sees MMS_OK and
    // fails, but the probing callback cannot leave an unrelated lease behind.
    if(!result)ticket.CancelAndRelease();return result;}
void Denied(Plugin& p,MmsResult& mutation,MmsResult& data,MmsResult& state){
    mutation=p.ui.unregisterItem(p.ui.context,p.menu);
    data=AttemptRefresh(p,MMS_UI_REFRESH_DATA);state=AttemptRefresh(p,MMS_UI_REFRESH_STATE);
}
void AttemptEdit(Plugin& p,DataRefreshProbeEditState& state){
    ++state.calls;state.originalOutput=0x123456789abcdef0ull;state.beginOutput=state.originalOutput;
    if(!p.state.editAvailable){state.begin=state.undo=state.redo=MMS_E_NOT_FOUND;return;}
    state.begin=p.edit.begin(p.edit.context,S("DATA read-only edit probe"),p.state.editExpectedRevision,&state.beginOutput);
    // An unexpected acceptance remains a test failure, but must not leave a
    // new pending edit merely because the probe intentionally challenged it.
    if(!state.begin)p.edit.cancel(p.edit.context,state.beginOutput);
    state.undo=p.edit.undo(p.edit.context);state.redo=p.edit.redo(p.edit.context);
}
struct Task{Plugin* owner;unsigned slot;bool child;bool normal;};
void MMS_CALL TaskRun(void*);
void MMS_CALL TaskCancel(void*);
MmsResult Enqueue(Plugin& p,unsigned slot,bool child,bool cancelNow,MmsResult* cancellation=nullptr,bool normal=false){
    auto task=std::make_unique<Task>(Task{&p,slot,child,normal});MmsHandle handle=0;
    const auto result=p.host.enqueueUi(p.host.context,TaskRun,TaskCancel,task.get(),&handle);
    if(result)return result;task.release();
    if(cancelNow){const auto cancelled=p.host.cancelUi(p.host.context,handle);if(cancellation)*cancellation=cancelled;}
    return MMS_OK;
}
void ExecuteTask(void* context,bool cancelled){std::unique_ptr<Task> task(static_cast<Task*>(context));auto& p=*task->owner;
    const unsigned index=task->slot;auto& state=p.state.tasks[index];++state.calls;
    AttemptEdit(p,state.edit);
    if(task->normal){
        if(!task->child){state.mutate=p.ui.unregisterItem(p.ui.context,p.menu);if(!state.mutate)p.menu=0;}
        else{const auto id=p.id+".normal-child-menu",command=p.id+".command";
            MmsUiElementV1 element{};element.h=Header<MmsUiElementV1>();element.id=S(id);element.ownerPlugin=S(p.id);
            element.parentId=S("mms.menu.plugins");element.label=S("Normal child");element.commandId=S(command);element.kind=MMS_UI_MENU_ITEM;
            MmsHandle handle=0;state.mutate=p.ui.registerElement(p.ui.context,&element,&handle);
            if(!state.mutate)state.mutate=p.ui.unregisterItem(p.ui.context,handle);}
        state.dataRequest=AttemptRefresh(p,MMS_UI_REFRESH_DATA);state.stateRequest=AttemptRefresh(p,MMS_UI_REFRESH_STATE);
    }else Denied(p,state.mutate,state.dataRequest,state.stateRequest);
    if(!task->child)state.enqueueChild=Enqueue(p,index==1?3u:2u,true,false,nullptr,task->normal);
    Log(p,cancelled?"DATA_PROBE_TASK_CANCEL":"DATA_PROBE_TASK_RUN");
}
void MMS_CALL TaskRun(void* context){ExecuteTask(context,false);}
void MMS_CALL TaskCancel(void* context){ExecuteTask(context,true);}
struct LayoutTask{Plugin* owner;unsigned slot;bool child;bool normal;};
void MMS_CALL LayoutTaskRun(void*);
void MMS_CALL LayoutTaskCancel(void*);
MmsResult EnqueueLayout(Plugin& p,unsigned slot,bool child,bool cancelNow,bool normal=false,MmsResult* cancellation=nullptr){
    auto task=std::make_unique<LayoutTask>(LayoutTask{&p,slot,child,normal});MmsHandle handle=0;
    const auto result=p.host.enqueueUi(p.host.context,LayoutTaskRun,LayoutTaskCancel,task.get(),&handle);
    if(result)return result;task.release();
    if(cancelNow){const auto cancelled=p.host.cancelUi(p.host.context,handle);if(cancellation)*cancellation=cancelled;}
    return MMS_OK;
}
void ExecuteLayoutTask(void* context,bool cancelled){
    std::unique_ptr<LayoutTask> task(static_cast<LayoutTask*>(context));auto& p=*task->owner;
    auto& state=p.layoutState.tasks[task->slot];++state.calls;AttemptEdit(p,state.edit);
    if(task->normal&&task->child){
        const auto id=p.id+".layout-normal-child",command=p.id+".command";
        MmsUiElementV1 element{};element.h=Header<MmsUiElementV1>();element.id=S(id);element.ownerPlugin=S(p.id);
        element.parentId=S("mms.menu.plugins");element.label=S("Layout child");element.commandId=S(command);element.kind=MMS_UI_MENU_ITEM;
        MmsHandle handle=0;state.mutate=p.ui.registerElement(p.ui.context,&element,&handle);
        if(!state.mutate)state.mutate=p.ui.unregisterItem(p.ui.context,handle);
    }else{
        state.mutate=p.ui.unregisterItem(p.ui.context,p.menu);if(!state.mutate&&task->normal)p.menu=0;
    }
    state.dataRequest=AttemptRefresh(p,MMS_UI_REFRESH_DATA);state.stateRequest=AttemptRefresh(p,MMS_UI_REFRESH_STATE);
    state.layoutRequest=AttemptRefresh(p,MMS_UI_REFRESH_LAYOUT);
    if(!task->child)state.enqueueChild=EnqueueLayout(p,task->slot==1?3u:2u,true,false,task->normal);
    Log(p,cancelled?"LAYOUT_PROBE_TASK_CANCEL":"LAYOUT_PROBE_TASK_RUN");
}
void MMS_CALL LayoutTaskRun(void* context){ExecuteLayoutTask(context,false);}
void MMS_CALL LayoutTaskCancel(void* context){ExecuteLayoutTask(context,true);}
MmsResult MMS_CALL Prepare(void* context,const MmsUiDataPrepareV1* input){auto& p=*static_cast<Plugin*>(context);auto& s=p.state;
    ++s.prepareCount;s.prepareSequence=++s.sequence;s.preparedRevision=input->sourceRevision;
    AttemptEdit(p,s.edit);
    s.prepareWrite=s.secondWrite=MMS_E_NOT_READY;
    if(input->sourceBytes.bytes!=sizeof(int)||!input->sourceBytes.data)return MMS_E_ARGUMENT;
    int source=0;std::memcpy(&source,input->sourceBytes.data,sizeof(source));
    const auto doubled=std::int64_t(source)*2;
    if(doubled<(std::numeric_limits<int>::min)()||doubled>(std::numeric_limits<int>::max)())return MMS_E_ARGUMENT;
    const int value=int(doubled);s.preparedValue=value;
    MmsUiDataSourceStateV1 sourceState{};s.sourceRead=p.source.State(sourceState);
    if(!s.sourceRead)s.sourceReadRevision=sourceState.revision;
    DataDisplay previous;s.oldDisplayRead=p.binding.Acquire(previous);
    if(!s.oldDisplayRead)s.previousDisplayValue=DisplayInt(previous);
    Denied(p,s.directMutation,s.recursiveData,s.recursiveState);
    if(s.flags&DATA_PROBE_ENQUEUE){s.enqueueRun=Enqueue(p,0,false,false);
        s.enqueueCancel=Enqueue(p,1,false,true,&s.selfCancel);}
    if(p.prepareHook)p.prepareHook(p.prepareHookContext);
    if(!(s.flags&DATA_PROBE_NO_WRITE))s.prepareWrite=p.data.WritePrepared(input->builder,{&value,sizeof(value)});
    if(s.flags&DATA_PROBE_DOUBLE_WRITE)s.secondWrite=p.data.WritePrepared(input->builder,{&value,sizeof(value)});
    if(s.flags&DATA_PROBE_THROW_MEMORY)throw std::bad_alloc();
    if(s.flags&DATA_PROBE_FAIL_PREPARE)return MMS_E_FAILED;
    return MMS_OK;
}
struct View{Plugin* owner;HWND window;};
LRESULT CALLBACK PanelProc(HWND window,UINT message,WPARAM wp,LPARAM lp){
    auto* view=reinterpret_cast<View*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){view=static_cast<View*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        view->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(view));}
    if(view&&message==WM_PAINT){auto& p=*view->owner;PAINTSTRUCT paint{};HDC dc=BeginPaint(window,&paint);
        DataDisplay display;const auto result=p.binding.Acquire(display);const int value=result?-1234567:DisplayInt(display);
        ++p.state.paintCount;p.state.paintValue=value;p.state.paintSequence=++p.state.sequence;
        if(!result)p.state.displayRevision=display.value().displayRevision;
        RECT client{};GetClientRect(window,&client);const unsigned color=static_cast<unsigned>(value);
        HBRUSH brush=CreateSolidBrush(RGB(32+(color&63),64+((color>>6)&63),96+((color>>12)&63)));
        FillRect(dc,&client,brush?brush:static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));if(brush)DeleteObject(brush);
        wchar_t text[96]{};swprintf_s(text,L"DATA display = %d",value);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
        TextOutW(dc,6,6,text,int(std::wcslen(text)));EndPaint(window,&paint);return 0;
    }
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_NCDESTROY)SetWindowLongPtrW(window,GWLP_USERDATA,0);
    return DefWindowProcW(window,message,wp,lp);
}
MmsResult MMS_CALL CreateView(void* context,const MmsUiMountV1* mount,void** out,uintptr_t* native){
    if(!mount||!out||!native)return MMS_E_ARGUMENT;*out=nullptr;*native=0;auto& p=*static_cast<Plugin*>(context);
    if(!p.registeredClass){WNDCLASSW wc{};wc.hInstance=p.module;wc.lpfnWndProc=PanelProc;wc.lpszClassName=kPanelClass;
        if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return MMS_E_FAILED;p.registeredClass=true;}
    auto view=std::make_unique<View>(View{&p,nullptr});
    HWND window=CreateWindowExW(0,kPanelClass,L"DATA probe",WS_CHILD,
        mount->bounds.x,mount->bounds.y,int(mount->bounds.width),int(mount->bounds.height),reinterpret_cast<HWND>(mount->parentWindow),nullptr,p.module,view.get());
    if(!window)return MMS_E_FAILED;*native=reinterpret_cast<uintptr_t>(window);*out=view.release();
    p.state.panelWindow=*native;++p.state.panelCreated;return MMS_OK;
}
void MMS_CALL ResizeView(void* context,const MmsUiRectV1* bounds,uint32_t){auto& view=*static_cast<View*>(context);auto& p=*view.owner;
    if(p.layoutState.flags&LAYOUT_PROBE_ACTIVE){auto& s=p.layoutState;++s.resizeCount;AttemptEdit(p,s.edit);
        Denied(p,s.directMutation,s.recursiveData,s.recursiveState);s.recursiveLayout=AttemptRefresh(p,MMS_UI_REFRESH_LAYOUT);
        if(s.flags&LAYOUT_PROBE_ENQUEUE){s.enqueueRun=EnqueueLayout(p,0,false,false);
            s.enqueueCancel=EnqueueLayout(p,1,false,true,false,&s.selfCancel);}
        if(p.layoutHook)p.layoutHook(p.layoutHookContext);
        if(s.flags&LAYOUT_PROBE_THROW)throw std::runtime_error("controlled real DLL layout resize failure");
    }
    MoveWindow(view.window,bounds->x,bounds->y,int(bounds->width),int(bounds->height),TRUE);}
void MMS_CALL ShowView(void* context,uint32_t visible){ShowWindow(static_cast<View*>(context)->window,visible?SW_SHOWNA:SW_HIDE);}
MmsResult MMS_CALL FocusView(void* context){SetFocus(static_cast<View*>(context)->window);return MMS_OK;}
void MMS_CALL DestroyView(void* context){std::unique_ptr<View> view(static_cast<View*>(context));auto& p=*view->owner;
    DestroyWindow(view->window);if(p.state.panelWindow==reinterpret_cast<uintptr_t>(view->window))p.state.panelWindow=0;
    ++p.state.panelDestroyed;Log(p,"DATA_PROBE_VIEW_DESTROY");}
MmsResult MMS_CALL State(void* context,MmsObjectRef,MmsCommandStateV1* out){auto& p=*static_cast<Plugin*>(context);
    if(!out)return MMS_E_ARGUMENT;*out={Header<MmsCommandStateV1>(),1,0,p.layoutVisible,0,{}};
    ++p.state.stateCount;p.state.stateSequence=++p.state.sequence;
    if(p.binding){DataDisplay display;if(!p.binding.Acquire(display)){const int value=DisplayInt(display);
        p.state.stateValue=value;p.state.displayRevision=display.value().displayRevision;
        out->enabled=value>=0;out->checked=(static_cast<unsigned>(value/2)&1);}}
    return MMS_OK;
}
MmsResult MMS_CALL Invoke(void*,MmsObjectRef,MmsSpan,MmsHandle){return MMS_OK;}
MmsResult MMS_CALL Create(const MmsHostV1* host,const MmsCreateInfoV1* info,void** out){
    if(!host||!info||!out)return MMS_E_ARGUMENT;*out=nullptr;if(current)return MMS_E_BUSY;
    auto p=std::make_unique<Plugin>();p->host=*host;p->id.assign(info->pluginId.data,std::size_t(info->pluginId.bytes));
    p->state.size=sizeof(DataRefreshProbeState);p->state.paintValue=p->state.stateValue=p->state.previousDisplayValue=-1234567;
    MmsResult result=host->queryService(host->context,Text("mms.ui"),1,0,sizeof(p->ui),&p->ui);if(result)return result;
    if((result=host->queryService(host->context,Text("mms.ui.surface"),1,0,sizeof(p->surface),&p->surface)))return result;
    if((result=RefreshServices::Query(*host,p->refresh)))return result;
    if((result=DataServices::Query(*host,p->data)))return result;
    if((result=StateContentServices::Query(*host,p->stateContent)))return result;
    MmsUiStateContentCapabilitiesV1 contentCaps{};
    if((result=p->stateContent.Capabilities(contentCaps)))return result;
    if(contentCaps.maxBytesPerField!=4096||contentCaps.maxProvidersPerPlugin!=128)return MMS_E_VERSION;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&PanelProc),&p->module))return MMS_E_FAILED;
    current=p.get();*out=p.release();return MMS_OK;
}
MmsResult RegisterElement(Plugin& p,MmsHandle& handle,const char* suffix,const char* parent,uint32_t kind,const std::string& command,const std::string& factory={}){
    const auto id=p.id+suffix;MmsUiElementV1 element{};element.h=Header<MmsUiElementV1>();element.id=S(id);element.ownerPlugin=S(p.id);
    element.parentId=S(parent);element.label=S("DATA refresh probe");element.kind=kind;element.capabilities=MMS_UI_HIDE|MMS_UI_MOVE;
    element.commandId=S(command);element.surfaceId=S(factory);return p.ui.registerElement(p.ui.context,&element,&handle);
}
MmsResult MMS_CALL Start(void* context){auto& p=*static_cast<Plugin*>(context);const auto id=p.id+".command";
    p.state.editQuery=p.host.queryService(p.host.context,Text("mms.edit"),1,0,sizeof(p.edit),&p.edit);
    if(p.state.editQuery!=MMS_OK&&p.state.editQuery!=MMS_E_NOT_FOUND)return p.state.editQuery;
    if(!p.state.editQuery){
        if(p.edit.h.size<sizeof(p.edit)||p.edit.h.major!=1||!p.edit.begin||!p.edit.cancel||!p.edit.undo||!p.edit.redo)return MMS_E_VERSION;
        p.state.editAvailable=1;
    }
    // This private probe starts against the fixture's fresh common journal.
    // The attached-host case checks that this is its real current revision.
    p.state.editExpectedRevision=1;
    MmsCommandDescV1 command{};command.info.h=Header<MmsCommandDescV1>();command.info.id=S(id);command.info.ownerPlugin=S(p.id);
    command.info.label=S("DATA test");command.context=&p;command.getState=State;command.invoke=Invoke;
    MmsResult result=p.ui.registerCommand(p.ui.context,&command,&p.command);if(result)return result;
    if((result=RegisterElement(p,p.menu,".menu","mms.menu.plugins",MMS_UI_MENU_ITEM,id)))return result;
    if((result=RegisterElement(p,p.button,".button","mms.toolbar.main.extensions",MMS_UI_BUTTON,id)))return result;
    const auto factory=p.id+".factory";MmsUiSurfaceDescV1 surface{};surface.h=Header<MmsUiSurfaceDescV1>();surface.id=S(factory);surface.context=&p;
    surface.minimumWidth=80;surface.minimumHeight=24;surface.preferredWidth=240;surface.preferredHeight=64;
    surface.createView=CreateView;surface.resizeView=ResizeView;surface.showView=ShowView;surface.focusView=FocusView;surface.destroyView=DestroyView;
    if((result=p.surface.registerSurface(p.surface.context,&surface,&p.factory)))return result;
    if((result=RegisterElement(p,p.panel,".panel","mms.workspace.tabs.extensions",MMS_UI_PANEL,{},factory)))return result;
    if((result=p.data.RegisterSource(S(p.id+".data"),sizeof(int),p.source)))return result;
    Log(p,"DATA_PROBE_START");return MMS_OK;
}
void MMS_CALL Event(void*,const MmsEventV1*){}
bool Retired(MmsResult result){return result==MMS_E_STALE||result==MMS_E_NOT_FOUND||result==MMS_E_NOT_READY;}
MmsResult MMS_CALL Stop(void* context){auto& p=*static_cast<Plugin*>(context);++p.state.stopCount;
    if(p.sdkLayoutSession){
        MmsUiLayoutSessionInfoV1 info{};
        if(const auto result=p.sdkLayoutSession.Query(info))return result;
        if(const auto result=p.sdkLayoutSession.Close())return result;
    }
    if(p.ticket||p.held){p.state.stopHeldTicket=bool(p.ticket);p.state.stopHeldDisplay=bool(p.held);
        Log(p,"DATA_PROBE_STOP_HELD");return MMS_OK;}
    if(auto result=p.binding.Release())if(!Retired(result))return result;
    if(auto result=p.source.Release())if(!Retired(result))return result;
    for(auto* item:{&p.panel,&p.button,&p.menu})if(*item){const auto result=p.ui.unregisterItem(p.ui.context,*item);
        if(result&&!Retired(result))return result;*item=0;}
    if(p.factory){const auto result=p.surface.unregisterSurface(p.surface.context,p.factory);if(result&&!Retired(result))return result;p.factory=0;}
    if(p.command){const auto result=p.ui.unregisterItem(p.ui.context,p.command);if(result&&!Retired(result))return result;p.command=0;}
    Log(p,"DATA_PROBE_STOP_OK");return MMS_OK;
}
void MMS_CALL Destroy(void* context){auto* p=static_cast<Plugin*>(context);Log(*p,"DATA_PROBE_DESTROY");
    if(p->registeredClass)UnregisterClassW(kPanelClass,p->module);if(current==p)current=nullptr;delete p;}
}
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major,uint32_t minor,uint32_t bytes,MmsPluginApiV1* out){
    if(!out||bytes<sizeof(*out))return MMS_E_ARGUMENT;if(major!=1||minor)return MMS_E_VERSION;
    *out={mms::sdk::Header<MmsPluginApiV1>(),Create,Start,Event,Stop,Destroy};return MMS_OK;
}
extern "C" MmsResult MMS_CALL ProbeBegin(uint32_t kind){return mms::sdk::detail::Call([&]()->MmsResult{
    auto* p=current;if(!p)return MMS_E_NOT_READY;if(kind!=DATA_PROBE_PANEL&&kind!=DATA_PROBE_BUTTON)return MMS_E_ARGUMENT;
    if(p->binding||p->ticket||p->held)return MMS_E_BUSY;MmsUiRefreshTargetV1 target{};
    if(const auto result=p->refresh.Target(kind==DATA_PROBE_PANEL?p->panel:p->button,target))return result;
    if(const auto result=p->data.Bind(target.target,p->source,sizeof(int),p->binding,Prepare,p))return result;
    p->state.targetKind=kind;p->state.target=target.target;return MMS_OK;});}
extern "C" MmsResult MMS_CALL ProbePublish(int32_t value){return mms::sdk::detail::Call([&]()->MmsResult{
    auto* p=current;if(!p||!p->source)return MMS_E_NOT_READY;MmsUiDataSourceStateV1 state{};MmsResult result=MMS_E_FAILED;
    std::thread worker([&]{int bytes=value;result=p->source.Publish({&bytes,sizeof(bytes)},p->state.sourceRevision,state);bytes=-9876543;});worker.join();
    p->state.publishResult=result;if(!result){p->state.sourceValue=value;p->state.sourceRevision=state.revision;}return result;});}
extern "C" MmsResult MMS_CALL ProbeRequest(uint32_t kinds,uint32_t flags){return mms::sdk::detail::Call([&]()->MmsResult{
    auto* p=current;if(!p||!p->binding)return MMS_E_NOT_READY;if(p->ticket)return MMS_E_BUSY;if(flags&~31u)return MMS_E_ARGUMENT;
    p->state.flags=flags;MmsResult result=MMS_E_FAILED;std::thread worker([&]{result=Request(*p,kinds,p->ticket);});worker.join();return result;});}
extern "C" MmsResult MMS_CALL ProbeQuery(DataRefreshProbeState* out){return mms::sdk::detail::Call([&]()->MmsResult{
    if(!out||out->size!=sizeof(*out))return MMS_E_ARGUMENT;auto* p=current;if(!p)return MMS_E_NOT_READY;auto result=p->state;
    result.menu=p->menu;result.button=p->button;result.panel=p->panel;result.source=p->source.get();result.binding=p->binding.get();result.ticket=p->ticket.get();
    if(p->held){result.heldLease=p->held.value().lease;result.heldValue=DisplayInt(p->held);}
    result.ticketQueryResult=p->ticket?p->ticket.Query(result.ticketState):MMS_E_NOT_READY;*out=result;return MMS_OK;});}
extern "C" MmsResult MMS_CALL ProbeReleaseTicket(void){return current?current->ticket.Release():MMS_E_NOT_READY;}
extern "C" MmsResult MMS_CALL ProbeAcquireHold(void){return current?current->binding.Acquire(current->held):MMS_E_NOT_READY;}
extern "C" MmsResult MMS_CALL ProbeReleaseHold(void){return mms::sdk::detail::Call([&]()->MmsResult{
    auto* p=current;if(!p)return MMS_E_NOT_READY;MmsResult result=MMS_E_FAILED;
    std::thread worker([&]{result=p->held.Release();});worker.join();p->state.heldReleaseResult=result;return result;});}
extern "C" MmsResult MMS_CALL ProbeRemoveTarget(void){return mms::sdk::detail::Call([&]()->MmsResult{
    auto* p=current;if(!p)return MMS_E_NOT_READY;auto& item=p->state.targetKind==DATA_PROBE_BUTTON?p->button:p->panel;
    if(!item)return MMS_E_NOT_READY;const auto result=p->ui.unregisterItem(p->ui.context,item);if(!result)item=0;return result;});}
extern "C" MmsResult MMS_CALL ProbeSetPrepareHook(DataRefreshProbePrepareHookFn callback,void* context){
    if(!current)return MMS_E_NOT_READY;current->prepareHook=callback;current->prepareHookContext=context;return MMS_OK;}
extern "C" MmsResult MMS_CALL ProbeEnqueueNormal(void){return mms::sdk::detail::Call([&]()->MmsResult{
    auto* p=current;if(!p||!p->binding||!p->state.sourceRevision)return MMS_E_NOT_READY;
    // This independent worker also proves DATA/STATE requests are not rejected
    // merely because another owner's prepare is currently on the UI thread.
    if(const auto result=AttemptRefresh(*p,MMS_UI_REFRESH_DATA))return result;
    if(const auto result=AttemptRefresh(*p,MMS_UI_REFRESH_STATE))return result;
    return Enqueue(*p,0,false,false,nullptr,true);});}
extern "C" MmsResult MMS_CALL ProbeLayoutConfigure(uint32_t flags,DataRefreshProbeLayoutHookFn hook,void* context){
    if(!current)return MMS_E_NOT_READY;if(flags&~7u)return MMS_E_ARGUMENT;
    current->layoutState={};current->layoutState.size=sizeof(DataRefreshProbeLayoutState);current->layoutState.flags=flags;
    current->layoutHook=hook;current->layoutHookContext=context;return MMS_OK;
}
extern "C" MmsResult MMS_CALL ProbeLayoutQuery(DataRefreshProbeLayoutState* out){
    if(!out||out->size!=sizeof(*out))return MMS_E_ARGUMENT;if(!current)return MMS_E_NOT_READY;
    *out=current->layoutState;out->size=sizeof(*out);return MMS_OK;
}
extern "C" MmsResult MMS_CALL ProbeEnqueueLayoutNormal(void){return mms::sdk::detail::Call([&]()->MmsResult{
    auto* p=current;if(!p||!p->binding||!p->state.sourceRevision)return MMS_E_NOT_READY;
    return EnqueueLayout(*p,0,false,false,true);});}
extern "C" MmsResult MMS_CALL ProbeLayoutSetVisible(uint32_t visible){
    if(!current)return MMS_E_NOT_READY;if(visible>1)return MMS_E_ARGUMENT;current->layoutVisible=visible;return MMS_OK;
}
// Trusted host tests only. This is not a plugin ABI service or SDK API.
extern "C" void* MMS_CALL ProbeUiContext(){return current?current->ui.context:nullptr;}
// Negotiate in the loaded DLL. The copied table keeps the actual plugin's
// owner context, and every operation still enters the public host C ABI.
extern "C" MmsResult MMS_CALL ProbeLayoutSessionApi(MmsUiLayoutSessionApiV1* out){
    if(!out||out->h.size!=sizeof(*out)||out->h.major!=1||out->h.minor)return MMS_E_ARGUMENT;
    if(!current)return MMS_E_NOT_READY;
    MmsUiLayoutSessionApiV1 table{};
    const auto result=current->host.queryService(current->host.context,S("mms.ui.layout.session"),1,0,sizeof(table),&table);
    if(result)return result;*out=table;return MMS_OK;
}
extern "C" MmsResult MMS_CALL ProbeLayoutSessionRoundtrip(uint32_t* steps){
    return mms::sdk::detail::Call([&]()->MmsResult{
        if(!steps)return MMS_E_ARGUMENT;*steps=0;if(!current)return MMS_E_NOT_READY;
        MmsUiLayoutSessionApiV1 api{};api.h=Header<MmsUiLayoutSessionApiV1>();
        if(const auto result=ProbeLayoutSessionApi(&api))return result;
        MmsUiLayoutSessionTargetV1 target{};target.h=Header<MmsUiLayoutSessionTargetV1>();
        target.element=current->button;target.fields=MMS_LAYOUT_SESSION_VISIBLE;
        MmsHandle handle=0;
        if(const auto result=api.begin(api.context,{&target,1,sizeof(target),1},&handle))return result;
        *steps|=1;
        struct Cleanup{MmsUiLayoutSessionApiV1& api;MmsHandle& handle;~Cleanup(){if(!handle)return;
            MmsUiLayoutSessionInfoV1 info{};info.h=Header<MmsUiLayoutSessionInfoV1>();
            if(api.query(api.context,handle,&info)==MMS_OK&&info.state==MMS_LAYOUT_SESSION_ACTIVE)
                api.rollback(api.context,handle,info.revision);
            api.release(api.context,handle);}} cleanup{api,handle};
        MmsUiLayoutSessionValueV1 value{};value.h=Header<MmsUiLayoutSessionValueV1>();
        value.element=target.element;value.fields=target.fields; // Hidden; unused fields stay zero.
        MmsUiLayoutSessionInfoV1 info{};info.h=Header<MmsUiLayoutSessionInfoV1>();
        if(const auto result=api.query(api.context,handle,&info))return result;
        if(info.state!=MMS_LAYOUT_SESSION_ACTIVE)return MMS_E_FAILED;
        if(const auto result=api.apply(api.context,handle,info.revision,{&value,1,sizeof(value),1}))return result;
        *steps|=2;
        if(const auto result=api.query(api.context,handle,&info))return result;
        if(info.state!=MMS_LAYOUT_SESSION_ACTIVE||info.activeTargetCount!=1||info.targetCount!=1||info.reasons)return MMS_E_FAILED;
        *steps|=4;
        MmsUiLayoutSessionTargetV1 claim{};claim.h=Header<MmsUiLayoutSessionTargetV1>();
        if(const auto result=api.queryClaim(api.context,handle,info.revision,0,&claim))return result;
        if(claim.element!=target.element||claim.fields!=target.fields||claim.reserved)return MMS_E_FAILED;
        *steps|=8;
        if(const auto result=api.rollback(api.context,handle,info.revision))return result;
        *steps|=16;
        if(const auto result=api.query(api.context,handle,&info))return result;
        if(info.state!=MMS_LAYOUT_SESSION_ROLLED_BACK||info.activeTargetCount)return MMS_E_FAILED;
        *steps|=32;
        if(const auto result=api.queryClaim(api.context,handle,info.revision,0,&claim))return result;
        if(claim.element!=target.element||claim.fields||claim.reserved)return MMS_E_FAILED;
        *steps|=64;
        if(const auto result=api.release(api.context,handle))return result;
        handle=0;*steps|=128;Log(*current,"LAYOUT_SESSION_CABI_ROUNDTRIP_PASS steps=255");return MMS_OK;
    });
}
extern "C" MmsResult MMS_CALL ProbeLayoutWorkspaceVersion(){
    return mms::sdk::detail::Call([&]()->MmsResult{
        if(!current)return MMS_E_NOT_READY;
        mms::sdk::LayoutSessionServices services;
        if(const auto result=mms::sdk::LayoutSessionServices::Query(current->host,services,1))return result;
        MmsUiLayoutSessionCapsV1 caps{};
        if(const auto result=services.Capabilities(caps))return result;
        if(services.api().h.minor!=1 || caps.mounts!=31 || caps.h.minor)return MMS_E_FAILED;
        MmsUiLayoutSessionApiV1 legacy{};
        const auto result=current->host.queryService(current->host.context,S("mms.ui.layout.session"),1,0,sizeof(legacy),&legacy);
        if(result)return result;
        return legacy.context==services.api().context && legacy.h.minor==0 ? MMS_OK : MMS_E_FAILED;
    });
}
extern "C" MmsResult MMS_CALL ProbeLayoutSessionSdkRoundtrip(uint32_t* steps){
    return mms::sdk::detail::Call([&]()->MmsResult{
        if(!steps)return MMS_E_ARGUMENT;*steps=0;if(!current)return MMS_E_NOT_READY;
        auto& p=*current;auto& session=p.sdkLayoutSession;
        if(session)return MMS_E_BUSY; // Never overwrite a retained owner.
        LayoutSessionServices services;
        if(const auto result=LayoutSessionServices::Query(p.host,services))return result;
        *steps|=1;
        if(const auto result=services.BeginVisibility(p.button,session))return result;
        *steps|=2;
        const auto original=session.get();
        const auto fail=[&](MmsResult originalResult)->MmsResult{
            // Explicitly query before Close. On either failure the same member
            // keeps the original handle for a later UI-thread Stop attempt.
            MmsUiLayoutSessionInfoV1 remaining{};
            if(session.Query(remaining)!=MMS_OK || session.Close()!=MMS_OK)
                Log(p,"LAYOUT_SESSION_SDK_CLEANUP_RETAINED");
            return originalResult;
        };
        MmsUiLayoutSessionInfoV1 info{};
        if(const auto result=session.Query(info))return fail(result);
        if(!original||info.state!=MMS_LAYOUT_SESSION_ACTIVE||info.targetCount!=1||info.activeTargetCount!=1)
            return fail(MMS_E_FAILED);
        *steps|=4;
        const auto hidden=LayoutSessionVisibility(p.button,false);
        if(const auto result=session.Apply(&hidden,1))return fail(result);
        if(session.get()!=original||session.revision())return fail(MMS_E_FAILED);
        *steps|=8;
        if(const auto result=session.Query(info))return fail(result);
        if(info.state!=MMS_LAYOUT_SESSION_ACTIVE||session.get()!=original||!session.revision())return fail(MMS_E_FAILED);
        *steps|=16;
        if(const auto result=session.Close())return fail(result);
        *steps|=32;
        if(session||session.get()||session.revision())return MMS_E_FAILED;
        *steps|=64;
        // No native refresh occurs inside this roundtrip. Geometry is exercised
        // separately by the actual-host public C ABI takeover checks.
        Log(p,"LAYOUT_SESSION_SDK_ROUNDTRIP_PASS steps=127");return MMS_OK;
    });
}

// Private test observer: called from this exported test driver, not from a
// plugin callback. The host independently checks real HWNDs and geometry.
using LayoutParentObserver=MmsResult(MMS_CALL*)(void*,uint32_t,MmsHandle,MmsHandle,MmsHandle);
static LayoutParentObserver parentObserver=nullptr;
static void* parentObserverContext=nullptr;
extern "C" void MMS_CALL ProbeSetLayoutParentObserver(LayoutParentObserver observer,void* context){
    parentObserver=observer;parentObserverContext=context;
}
extern "C" MmsResult MMS_CALL ProbeLayoutParentRoundtrip(uint32_t* steps){
    return mms::sdk::detail::Call([&]()->MmsResult{
        if(!steps)return MMS_E_ARGUMENT;*steps=0;
        if(!current||!parentObserver)return MMS_E_NOT_READY;
        auto& p=*current;if(p.sdkLayoutSession)return MMS_E_BUSY;
        MmsUiLayoutSessionApiV1_2 api{};
        if(const auto r=p.host.queryService(p.host.context,S("mms.ui.layout.session"),1,2,sizeof(api),&api))return r;
        if(api.h.size!=sizeof(api)||api.h.major!=1||api.h.minor!=2||!api.queryCapabilitiesEx||
            !api.queryTargetEx||!api.beginEx||!api.applyEx||!api.queryClaimEx)return MMS_E_FAILED;
        MmsUiLayoutSessionApiV1 old[2]{};
        for(unsigned minor=0;minor!=2;++minor){
            struct Guarded{MmsUiLayoutSessionApiV1 table;unsigned char tail[40];} guarded{};
            std::memset(guarded.tail,0x5a,sizeof(guarded.tail));
            if(const auto r=p.host.queryService(p.host.context,S("mms.ui.layout.session"),1,minor,sizeof(guarded.table),&guarded.table))return r;
            for(auto byte:guarded.tail)if(byte!=0x5a)return MMS_E_FAILED;
            old[minor]=guarded.table;
            MmsUiLayoutSessionCapsV1 caps{};caps.h=Header<MmsUiLayoutSessionCapsV1>();
            if(old[minor].h.size!=88||old[minor].h.minor!=minor||old[minor].context!=api.context||
                old[minor].queryCapabilities(old[minor].context,&caps)||caps.fields!=7||caps.mounts!=(minor?31u:15u))return MMS_E_FAILED;
        }
        MmsUiLayoutSessionApiV1_2 shortTable;std::memset(&shortTable,0x6b,sizeof(shortTable));
        const auto sentinel=shortTable;
        for(uint32_t bytes:{88u,127u})if(p.host.queryService(p.host.context,S("mms.ui.layout.session"),1,2,bytes,&shortTable)!=MMS_E_ARGUMENT||
            std::memcmp(&shortTable,&sentinel,sizeof(shortTable)))return MMS_E_FAILED;
        if(p.host.queryService(p.host.context,S("mms.ui.layout.session"),1,3,sizeof(shortTable),&shortTable)!=MMS_E_VERSION||
            std::memcmp(&shortTable,&sentinel,sizeof(shortTable))||
            p.host.queryService(p.host.context,S("mms.ui.layout.session"),2,2,sizeof(shortTable),&shortTable)!=MMS_E_VERSION||
            std::memcmp(&shortTable,&sentinel,sizeof(shortTable)))return MMS_E_FAILED;
        MmsUiLayoutSessionCapsV1 caps{};caps.h=Header<MmsUiLayoutSessionCapsV1>();
        if(api.queryCapabilities(api.context,&caps)||caps.fields!=7||caps.mounts!=31||
            api.queryCapabilitiesEx(api.context,&caps)||caps.fields!=15||caps.mounts!=31)return MMS_E_FAILED;
        *steps|=1;
        MmsUiLayoutApiV1_1 history{};
        if(const auto r=p.host.queryService(p.host.context,S("mms.ui.layout"),1,1,sizeof(history),&history))return r;
        MmsHandle groups[2]{},button=0,session=0;
        struct Cleanup{Plugin& p;MmsUiLayoutSessionApiV1_2& api;MmsHandle* groups;MmsHandle& button;MmsHandle& session;
            ~Cleanup(){if(p.sdkLayoutSession){MmsUiLayoutSessionInfoV1 info{};if(!p.sdkLayoutSession.Query(info))p.sdkLayoutSession.Close();}
                if(session){MmsUiLayoutSessionInfoV1 info{};info.h=Header<MmsUiLayoutSessionInfoV1>();
                    if(!api.query(api.context,session,&info)&&info.state==MMS_LAYOUT_SESSION_ACTIVE)api.rollback(api.context,session,info.revision);
                    api.release(api.context,session);}
                if(button)p.ui.unregisterItem(p.ui.context,button);
                for(int i=1;i>=0;--i)if(groups[i])p.ui.unregisterItem(p.ui.context,groups[i]);}
        } cleanup{p,api,groups,button,session};
        const std::string ids[]{p.id+".parent.group.a",p.id+".parent.group.b",p.id+".parent.button"};
        const auto command=p.id+".command";
        for(unsigned i=0;i!=3;++i){
            MmsUiElementV1 e{};e.h=Header<MmsUiElementV1>();e.id=S(ids[i]);e.ownerPlugin=S(p.id);
            e.parentId=i==2?S(ids[0]):S("mms.toolbar.main.extensions");e.label=S("Parent roundtrip button");
            e.kind=i==2?MMS_UI_BUTTON:MMS_UI_GROUP;
            e.capabilities=MMS_UI_MOVE|MMS_UI_HIDE|MMS_UI_LAYOUT|MMS_UI_RESIZE|(i==2?0:MMS_UI_CONTAINER);
            if(i==2)e.commandId=S(command);
            if(const auto r=p.ui.registerElement(p.ui.context,&e,i==2?&button:&groups[i]))return r;
            if(i<2){MmsUiLayoutHintsV1 hints{};hints.h=Header<MmsUiLayoutHintsV1>();
                hints.flow=MMS_FLOW_ROW;hints.minimum[0]=160;hints.minimum[1]=38;
                hints.padding[0]=i?47.0f:5.0f;hints.padding[1]=3;hints.padding[2]=5;hints.padding[3]=3;
                if(const auto r=history.setDefaultHints(history.context,e.id,&hints))return r;}
        }
        const auto observe=[&](uint32_t phase){return parentObserver(parentObserverContext,phase,button,groups[0],groups[1]);};
        if(const auto r=observe(0))return r;
        MmsUiLayoutSessionTargetInfoV1 targetInfo{};targetInfo.h=Header<MmsUiLayoutSessionTargetInfoV1>();
        if(api.queryTargetEx(api.context,button,&targetInfo)||!(targetInfo.fields&MMS_LAYOUT_SESSION_PARENT)||targetInfo.mount!=1)return MMS_E_FAILED;
        MmsUiLayoutSessionTargetV1_2 target{};target.h={sizeof(target),1,2};target.element=button;
        target.fields=MMS_LAYOUT_SESSION_PARENT;target.parent=groups[1];
        MmsUiLayoutSessionValueV1_2 value{};value.h={sizeof(value),1,2};value.element=button;value.fields=target.fields;value.parent=target.parent;
        MmsUiLayoutSessionInfoV1 info{};info.h=Header<MmsUiLayoutSessionInfoV1>();
        const auto beginApply=[&]()->MmsResult{
            if(const auto r=api.beginEx(api.context,{&target,1,sizeof(target),2},&session))return r;
            if(const auto r=old[0].query(old[0].context,session,&info))return r;
            return api.applyEx(api.context,session,info.revision,{&value,1,sizeof(value),2});};
        if(const auto r=beginApply())return r;
        if(const auto r=api.query(api.context,session,&info))return r;
        MmsUiLayoutSessionTargetV1 claim{};claim.h=Header<MmsUiLayoutSessionTargetV1>();claim.element=991;
        const auto originalClaim=claim;
        if(old[1].queryClaim(old[1].context,session,info.revision,0,&claim)!=MMS_E_UNSUPPORTED||
            std::memcmp(&claim,&originalClaim,sizeof(claim)))return MMS_E_FAILED;
        MmsUiLayoutSessionValueV1 omitted{};omitted.h=Header<MmsUiLayoutSessionValueV1>();omitted.element=button;omitted.fields=MMS_LAYOUT_SESSION_VISIBLE;
        if(old[0].apply(old[0].context,session,info.revision,{&omitted,1,sizeof(omitted),1})!=MMS_E_ARGUMENT)return MMS_E_FAILED;
        if(const auto r=observe(1))return r;
        if(const auto r=old[0].rollback(old[0].context,session,info.revision))return r;
        if(const auto r=api.query(api.context,session,&info))return r;
        MmsUiLayoutSessionTargetV1_2 exClaim{};exClaim.h={sizeof(exClaim),1,2};
        if(api.queryClaimEx(api.context,session,info.revision,0,&exClaim)||exClaim.element!=button||exClaim.fields||exClaim.parent)return MMS_E_FAILED;
        if(const auto r=old[1].release(old[1].context,session))return r;session=0;
        if(const auto r=observe(2))return r;*steps|=2;
        MmsUiLayoutHistoryInfoV1 before{},after{};before.h=after.h=Header<MmsUiLayoutHistoryInfoV1>();
        if(const auto r=history.queryHistory(history.context,&before))return r;
        if(const auto r=beginApply())return r;
        if(const auto r=api.query(api.context,session,&info))return r;
        if(const auto r=old[1].commit(old[1].context,session,info.revision))return r;
        if(const auto r=old[0].release(old[0].context,session))return r;session=0;
        if(history.queryHistory(history.context,&after)||after.undoDepth!=before.undoDepth+1)return MMS_E_FAILED;
        if(const auto r=observe(3))return r;
        if(const auto r=history.undo(history.context))return r;
        if(const auto r=observe(4))return r;
        if(const auto r=history.redo(history.context))return r;
        if(const auto r=observe(5))return r;
        if(const auto r=history.undo(history.context))return r;
        if(const auto r=observe(6))return r;*steps|=4;
        LayoutSessionServices sdk;
        if(const auto r=LayoutSessionServices::Query(p.host,sdk,2))return r;
        auto& owned=p.sdkLayoutSession;
        if(const auto r=sdk.BeginParent(button,groups[1],owned))return r;
        if(const auto r=owned.Query(info))return r;
        const auto sdkValue=LayoutSessionParent(button,groups[1]);
        if(const auto r=owned.Apply(&sdkValue,1))return r;
        if(const auto r=owned.Query(info))return r;
        if(const auto r=owned.QueryClaim(0,exClaim))return r;
        if(exClaim.element!=button||exClaim.fields!=MMS_LAYOUT_SESSION_PARENT||exClaim.parent!=groups[1])return MMS_E_FAILED;
        if(const auto r=observe(7))return r;
        if(const auto r=owned.Close())return r;
        if(const auto r=observe(8))return r;*steps|=8;
        if(const auto r=p.ui.unregisterItem(p.ui.context,button))return r;button=0;
        for(int i=1;i>=0;--i){if(const auto r=p.ui.unregisterItem(p.ui.context,groups[i]))return r;groups[i]=0;}
        *steps|=16;return MMS_OK;
    });
}
