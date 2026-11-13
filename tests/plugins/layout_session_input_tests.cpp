// Real EDIT controls and public layout sessions over Manager's original
// property-input bridge, EditStore, serializer and SharedHistory. The top-level
// window stays hidden. Logical insertion positions use EM_GETSEL; this is not
// an IME, physical-focus, visible-caret or pixel-rendering acceptance test.
#define NOMINMAX
#include "../../src/plugins/manager.hpp"
#include "../../src/plugins/native_ui.hpp"
#include "../../src/plugins/shared_history.hpp"
#include <mms/layout_session.h>
#include <MinHook.h>
#include <commctrl.h>
#include <imm.h>
#include <array>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace mikumikustudio::plugins;
namespace {
unsigned checks=0;
void Check(bool condition,const char* label){++checks;if(!condition){std::fprintf(stderr,"FAIL %s\n",label);throw std::runtime_error(label);}}
MmsString S(const char* text){return {text,std::strlen(text)};}
HWND focused=nullptr;
unsigned focusRequests=0;
HWND WINAPI LocalFocus(){return focused;}
HWND WINAPI LocalSetFocus(HWND next){
    ++focusRequests;const HWND previous=focused;if(previous==next)return previous;
    focused=next;
    if(IsWindow(previous))SendMessageW(previous,WM_KILLFOCUS,reinterpret_cast<WPARAM>(next),0);
    if(IsWindow(next))SendMessageW(next,WM_SETFOCUS,reinterpret_cast<WPARAM>(previous),0);
    return previous;
}
struct FocusGuards {
    FocusGuards(){Check(MH_Initialize()==MH_OK,"initialize isolated focus hooks");
        Check(MH_CreateHook(reinterpret_cast<void*>(&GetFocus),reinterpret_cast<void*>(&LocalFocus),nullptr)==MH_OK&&
            MH_CreateHook(reinterpret_cast<void*>(&SetFocus),reinterpret_cast<void*>(&LocalSetFocus),nullptr)==MH_OK&&
            MH_EnableHook(reinterpret_cast<void*>(&GetFocus))==MH_OK&&MH_EnableHook(reinterpret_cast<void*>(&SetFocus))==MH_OK,
            "isolate focus without replacing EDIT or host code");}
    ~FocusGuards(){focused=nullptr;MH_Uninitialize();}
};
void Pump(){MSG message{};for(unsigned i=0;i<512&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE);++i){
    Check(message.message!=WM_QUIT,"test message queue remains open");
    if(message.hwnd){DWORD process=0;GetWindowThreadProcessId(message.hwnd,&process);Check(process==GetCurrentProcessId(),"dispatch own process only");}
    TranslateMessage(&message);DispatchMessageW(&message);
}}
struct Window {
    HWND handle=nullptr,sink=nullptr;
    Window(){INITCOMMONCONTROLSEX init{sizeof(init),ICC_WIN95_CLASSES};Check(InitCommonControlsEx(&init),"initialize common controls");
        handle=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,L"STATIC",L"Hidden layout session INPUT fixture",
            WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,0,0,1000,700,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(handle!=nullptr,"create hidden top-level owner");Check(SetMenu(handle,CreateMenu()),"attach fixture menu");
        sink=CreateWindowExW(0,L"BUTTON",L"Logical focus sink",WS_CHILD|WS_VISIBLE,0,600,80,24,handle,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(sink!=nullptr&&!IsWindowVisible(handle),"owner remains hidden");}
    ~Window(){if(IsWindow(handle)){const auto menu=GetMenu(handle);SetMenu(handle,nullptr);if(menu)DestroyMenu(menu);DestroyWindow(handle);}}
};
std::wstring Caption(HWND field){const int size=GetWindowTextLengthW(field);Check(size>=0&&size<4096,"bounded EDIT text");
    std::wstring text(size_t(size)+1,L'\0');Check(GetWindowTextW(field,text.data(),int(text.size()))==size,"read EDIT text");text.resize(size);return text;}
std::array<DWORD,2> Selection(HWND field){std::array<DWORD,2> range{};
    SendMessageW(field,EM_GETSEL,reinterpret_cast<WPARAM>(&range[0]),reinterpret_cast<LPARAM>(&range[1]));return range;}
HWND Field(HWND parent){struct Search{HWND found=nullptr;unsigned count=0;} search;
    EnumChildWindows(parent,[](HWND child,LPARAM context)->BOOL{auto& search=*reinterpret_cast<Search*>(context);wchar_t type[32]{};
        GetClassNameW(child,type,32);if(GetDlgCtrlID(child)==0x7201&&!_wcsicmp(type,L"EDIT")){search.found=child;++search.count;}return TRUE;
    },reinterpret_cast<LPARAM>(&search));Check(search.count==1&&search.found,"find one real property EDIT");return search.found;}
void Enter(HWND field){SendMessageW(field,WM_KEYDOWN,VK_RETURN,1);SendMessageW(field,WM_CHAR,L'\r',1);SendMessageW(field,WM_KEYUP,VK_RETURN,1);}
void Escape(HWND field){SendMessageW(field,WM_KEYDOWN,VK_ESCAPE,1);SendMessageW(field,WM_CHAR,VK_ESCAPE,1);SendMessageW(field,WM_KEYUP,VK_ESCAPE,1);}
void DispatchQueuedBlur(HWND wrapper){
    // Dispatch the actual posted NativePropertyInput focus check only. Do not
    // pump unrelated main-window work which could apply native layout first.
    constexpr UINT checkFocus=WM_APP+0x4b1;
    MSG message{};Check(PeekMessageW(&message,wrapper,checkFocus,checkFocus,PM_REMOVE),"real EDIT blur queued a deferred submission check");
    Check(message.hwnd==wrapper,"queued focus check belongs to this INPUT");
    DispatchMessageW(&message);
}
struct ImmReentry {
    // Only the designated hidden EDIT receives a fixture context. Every other
    // call uses its original trampoline. No keyboard or input-language state
    // is changed. This tests synchronous IMM callback lifetime, not real IME.
    inline static thread_local ImmReentry* active=nullptr;
    inline static decltype(&ImmGetContext) originalGet=nullptr;
    inline static decltype(&ImmNotifyIME) originalNotify=nullptr;
    inline static decltype(&ImmReleaseContext) originalRelease=nullptr;
    HWND field=nullptr;
    std::function<void()> callback;
    std::exception_ptr failure;
    unsigned acquired=0,notified=0,released=0;
    bool armed=true,validRequest=true,aliveAtRelease=true;
    BOOL answer=TRUE;
    std::array<bool,3> installed{};
    static void* Entry(unsigned index){
        if(index==0)return reinterpret_cast<void*>(&ImmGetContext);
        if(index==1)return reinterpret_cast<void*>(&ImmNotifyIME);
        return reinterpret_cast<void*>(&ImmReleaseContext);
    }
    HIMC Context() noexcept {return reinterpret_cast<HIMC>(this);}
    static HIMC WINAPI Get(HWND field){
        if(active&&active->armed&&field==active->field){++active->acquired;return active->Context();}
        return originalGet(field);
    }
    static BOOL WINAPI Notify(HIMC context,DWORD action,DWORD index,DWORD value){
        if(!active||context!=active->Context())return originalNotify(context,action,index,value);
        auto& self=*active;++self.notified;self.armed=false;
        self.validRequest=self.validRequest&&action==NI_COMPOSITIONSTR&&index==CPS_CANCEL&&value==0;
        try{if(self.callback)self.callback();}catch(...){self.failure=std::current_exception();}
        return self.answer;
    }
    static BOOL WINAPI Release(HWND field,HIMC context){
        if(!active||context!=active->Context())return originalRelease(field,context);
        ++active->released;active->validRequest=active->validRequest&&field==active->field;
        active->aliveAtRelease=active->aliveAtRelease&&IsWindow(field);return TRUE;
    }
    ImmReentry(HWND target,std::function<void()> action,BOOL result=TRUE):field(target),callback(std::move(action)),answer(result){
        Check(!active,"one isolated IMM callback fixture at a time");
        try{
            void* hooks[]={reinterpret_cast<void*>(&Get),reinterpret_cast<void*>(&Notify),reinterpret_cast<void*>(&Release)};
            void** originals[]={reinterpret_cast<void**>(&originalGet),reinterpret_cast<void**>(&originalNotify),reinterpret_cast<void**>(&originalRelease)};
            for(unsigned i=0;i<3;++i){Check(MH_CreateHook(Entry(i),hooks[i],originals[i])==MH_OK,"create isolated IMM API hook");installed[i]=true;}
            active=this;
            for(unsigned i=0;i<3;++i)Check(MH_EnableHook(Entry(i))==MH_OK,"enable isolated IMM API hook");
        }catch(...){Close();throw;}
    }
    ~ImmReentry(){Close();}
    void Close() noexcept {
        for(unsigned i=0;i<3;++i)if(installed[i]){MH_DisableHook(Entry(i));MH_RemoveHook(Entry(i));installed[i]=false;}
        if(active==this)active=nullptr;
    }
    void Verify(){
        if(failure)std::rethrow_exception(failure);
        Check(acquired==1&&notified==1&&released==1&&validRequest&&aliveAtRelease,
            "IMM cancellation pairs one acquired context and release while original EDIT remains alive");
    }
};

struct Fixture {
    static constexpr const char* id="org.example.layout-session-input";
    Window window;
    std::unique_ptr<Manager> manager;
    std::unique_ptr<SharedHistory> history;
    std::unique_ptr<NativeUi> native;
    HMODULE module=nullptr;
    int (*Value)()=nullptr;
    uint64_t (*Calls)()=nullptr,(*Candidates)()=nullptr;
    UiClient* owner=nullptr;
    MmsUiApiV1 ui{};MmsUiLayoutApiV1 layout{};MmsUiLayoutSessionApiV1 sessions{};
    MmsHandle input=0,group=0,spacer=0;
    std::vector<MmsHandle> active;
    HWND field=nullptr,wrapper=nullptr;
    bool stopped=false;
    std::string groupId=std::string(id)+".group",inputId=std::string(id)+".input",spaceId=std::string(id)+".spacer";
    Fixture(const fs::path& directory,const fs::path& source){try{
        const auto package=directory/L"PLUGINS"/L"input";Check(fs::create_directories(package),"create new private plugin package");
        const auto dll=package/L"input-edit-probe.dll";Check(fs::copy_file(source,dll,fs::copy_options::none),"copy original edit probe without overwriting");
        std::ofstream manifest(package/L"plugin.json",std::ios::binary);
        manifest << R"({"schemaVersion":1,"id":"org.example.layout-session-input","name":"Isolated INPUT layout session test","version":"1.0.0","kind":"native","abi":{"major":1,"minMinor":0},"entry":"input-edit-probe.dll","architecture":"x64","minWindows":"6.1.7601","capabilities":["ui.command"],"requiresServices":[{"id":"mms.ui","major":1,"minMinor":0},{"id":"mms.ui.layout.session","major":1,"minMinor":0}],"dependencies":[],"nativeDependencies":[],"reloadPolicy":"restart"})";
        manifest.close();Check(bool(manifest),"write fixture-only manifest");
        module=LoadLibraryW(dll.c_str());Check(module!=nullptr,"retain original edit DLL for read-only counters after stop");
        Value=reinterpret_cast<decltype(Value)>(GetProcAddress(module,"GetProbeValue"));
        Calls=reinterpret_cast<decltype(Calls)>(GetProcAddress(module,"GetPropertyStageCalls"));
        Candidates=reinterpret_cast<decltype(Candidates)>(GetProcAddress(module,"GetPropertyCandidateCount"));
        Check(Value&&Calls&&Candidates,"original edit probe exports are present");
        manager=std::make_unique<Manager>(directory/L"data",[](const std::string& text){std::printf("manager: %s\n",text.c_str());});
        history=std::make_unique<SharedHistory>(manager->Storage(),[this]{return manager->InCallback()&&!manager->CanInvokeEdit()?MMS_E_BUSY:MMS_OK;});
        Check(manager->AttachEditHost(*history)==MMS_OK,"attach production SharedHistory to original Manager edit bridge");
        manager->Load(directory/L"PLUGINS");const auto status=manager->Status();Check(status.size()==1&&status[0].stage=="started","load original writable-property plugin");
        auto& registry=manager->Ui();const auto property=registry.properties.at(std::string(id)+".editable");owner=property->owner;
        ui=registry.Api(*owner);layout=registry.LayoutApi(*owner);sessions=registry.LayoutSessionApi(*owner);
        MmsUiElementV1 e{};e.h=Header<MmsUiElementV1>();e.id=Text(groupId);e.ownerPlugin=S(id);e.parentId=S("mms.toolbar.main.extensions");
        e.kind=MMS_UI_GROUP;e.capabilities=MMS_UI_CONTAINER|MMS_UI_HIDE|MMS_UI_LAYOUT|MMS_UI_RESIZE;
        Check(ui.registerElement(ui.context,&e,&group)==MMS_OK,"register ancestor group");
        MmsUiLayoutHintsV1 hints{};hints.h=Header<MmsUiLayoutHintsV1>();hints.flow=MMS_FLOW_ROW;hints.flags=MMS_HINT_EXPLICIT_SPACING;hints.gap[0]=8;
        Check(layout.setDefaultHints(layout.context,Text(groupId),&hints)==MMS_OK,"group uses real row layout");
        e={};e.h=Header<MmsUiElementV1>();e.id=Text(inputId);e.ownerPlugin=S(id);e.parentId=Text(groupId);e.propertyId=property->desc.info.id;
        e.kind=MMS_UI_INPUT;e.capabilities=MMS_UI_MOVE|MMS_UI_HIDE|MMS_UI_RESIZE|MMS_UI_LAYOUT;
        Check(ui.registerElement(ui.context,&e,&input)==MMS_OK,"register INPUT against actual plugin property");
        hints={};hints.h=Header<MmsUiLayoutHintsV1>();hints.order=10;hints.minimum[0]=90;hints.minimum[1]=20;
        hints.preferred[0]=240;hints.preferred[1]=30;hints.maximum[0]=600;hints.maximum[1]=100;
        Check(layout.setDefaultHints(layout.context,Text(inputId),&hints)==MMS_OK,"set INPUT bounds");
        e={};e.h=Header<MmsUiElementV1>();e.id=Text(spaceId);e.ownerPlugin=S(id);e.parentId=Text(groupId);
        e.kind=MMS_UI_SEPARATOR;e.capabilities=MMS_UI_LAYOUT|MMS_UI_RESIZE;
        Check(ui.registerElement(ui.context,&e,&spacer)==MMS_OK,"register ordering sibling");
        hints={};hints.h=Header<MmsUiLayoutHintsV1>();hints.preferred[0]=40;hints.preferred[1]=30;
        Check(layout.setDefaultHints(layout.context,Text(spaceId),&hints)==MMS_OK,"set ordering sibling width");
        native=std::make_unique<NativeUi>(registry,window.handle,[](const std::string& text){std::printf("native: %s\n",text.c_str());});
        Refresh();field=Field(native->Toolbar());wrapper=GetParent(field);Check(wrapper&&Caption(field)==L"0"&&Value()==0,"mount original canonical property value");
        RECT bounds{};Check(GetWindowRect(wrapper,&bounds)&&bounds.right>bounds.left&&bounds.bottom>bounds.top,"initial INPUT has nonempty geometry");
    }catch(...){Cleanup();throw;}}
    ~Fixture(){Cleanup();}
    void Cleanup() noexcept {
        if(manager&&!stopped&&owner){for(auto handle:active){MmsUiLayoutSessionInfoV1 info{};info.h=Header<MmsUiLayoutSessionInfoV1>();
            if(sessions.query(sessions.context,handle,&info)==MMS_OK&&info.state==MMS_LAYOUT_SESSION_ACTIVE)sessions.rollback(sessions.context,handle,info.revision);
            sessions.release(sessions.context,handle);}}
        native.reset();
        if(manager){
            manager->Stop();auto detached=manager->DetachEditHost();
            if(detached==MMS_E_FAILED)detached=manager->DetachEditHost();
            if(detached){manager.release();history.release();return;}
        }
        history.reset();manager.reset();
        if(module){FreeLibrary(module);module=nullptr;}
    }
    void Trace(const char* caseName,const char* phase){
        const bool fieldAlive=IsWindow(field)!=FALSE,wrapperAlive=IsWindow(wrapper)!=FALSE;
        const auto selection=fieldAlive?Selection(field):std::array<DWORD,2>{};
        const auto text=fieldAlive?Utf8(Caption(field)):std::string("<destroyed>");
        const HWND toolbar=native->Toolbar();RECT bounds{},toolbarBounds{};
        if(wrapperAlive)GetWindowRect(wrapper,&bounds);if(IsWindow(toolbar))GetWindowRect(toolbar,&toolbarBounds);
        struct Search{HWND field=nullptr;unsigned count=0;} mounted;
        if(IsWindow(toolbar))EnumChildWindows(toolbar,[](HWND child,LPARAM context)->BOOL{
            auto& found=*reinterpret_cast<Search*>(context);wchar_t type[32]{};GetClassNameW(child,type,32);
            if(GetDlgCtrlID(child)==0x7201&&!_wcsicmp(type,L"EDIT")){found.field=child;++found.count;}return TRUE;
        },reinterpret_cast<LPARAM>(&mounted));
        const auto element=manager->Ui().elements.find(inputId);
        const auto visibility=element==manager->Ui().elements.end()?-1:int(manager->Ui().ControlVisibility(element->second));
        std::printf("INPUT TRACE case=%s phase=%s field=%p alive=%u wrapper=%p alive=%u parent=%p mounted=%p count=%u "
            "text='%s' selection=%lu,%lu fieldStyle=%llx wrapperStyle=%llx bounds=%ld,%ld,%ld,%ld "
            "toolbar=%p style=%llx bounds=%ld,%ld,%ld,%ld visibility=%d focus=%p focusCalls=%u "
            "canonical=%d propertyCalls=%llu candidates=%llu undo=%llu redo=%llu\n",
            caseName,phase,reinterpret_cast<void*>(field),unsigned(fieldAlive),reinterpret_cast<void*>(wrapper),unsigned(wrapperAlive),
            reinterpret_cast<void*>(fieldAlive?GetParent(field):nullptr),reinterpret_cast<void*>(mounted.field),mounted.count,
            text.c_str(),selection[0],selection[1],static_cast<unsigned long long>(fieldAlive?GetWindowLongPtrW(field,GWL_STYLE):0),
            static_cast<unsigned long long>(wrapperAlive?GetWindowLongPtrW(wrapper,GWL_STYLE):0),bounds.left,bounds.top,bounds.right,bounds.bottom,
            reinterpret_cast<void*>(toolbar),static_cast<unsigned long long>(IsWindow(toolbar)?GetWindowLongPtrW(toolbar,GWL_STYLE):0),
            toolbarBounds.left,toolbarBounds.top,toolbarBounds.right,toolbarBounds.bottom,visibility,reinterpret_cast<void*>(GetFocus()),focusRequests,
            Value(),static_cast<unsigned long long>(Calls()),static_cast<unsigned long long>(Candidates()),
            static_cast<unsigned long long>(history->UndoCount()),static_cast<unsigned long long>(history->RedoCount()));
    }
    void Refresh(const char* diagnostic=nullptr){
        if(diagnostic)Trace(diagnostic,"before-native-refresh");native->Refresh();
        if(diagnostic)Trace(diagnostic,"after-native-refresh");native->Layout(10,850,180,true);
        if(diagnostic)Trace(diagnostic,"after-explicit-layout");Pump();
        if(diagnostic)Trace(diagnostic,"after-message-pump");Check(!IsWindowVisible(window.handle),"never show fixture window");
    }
    uint64_t Revision(){uint64_t r=0;Check(history->Revision(r)==MMS_OK,"read actual history revision");return r;}
    void Clean(int value=0,unsigned undo=0,unsigned calls=0){Check(Value()==value&&history->UndoCount()==undo&&!history->RedoCount()&&Calls()==calls&&!Candidates(),"layout does not write property or add history");}
    void Draft(const wchar_t* text=L"37",DWORD begin=0,DWORD end=1){SetFocus(field);Check(GetFocus()==field,"set test-local field focus");
        SendMessageW(field,EM_SETSEL,0,-1);SendMessageW(field,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(text));
        SendMessageW(field,EM_SETSEL,begin,end);Check(Caption(field)==text&&Selection(field)==std::array<DWORD,2>{begin,end},"draft and selection originate in real EDIT messages");}
    void Identity(HWND original,HWND originalWrapper){Check(field==original&&wrapper==originalWrapper&&IsWindow(field)&&GetParent(field)==wrapper&&
        Field(native->Toolbar())==field,"temporary layout preserves actual EDIT and wrapper identities");}
    MmsUiLayoutSessionInfoV1 Info(MmsHandle handle){MmsUiLayoutSessionInfoV1 info{};info.h=Header<MmsUiLayoutSessionInfoV1>();
        Check(sessions.query(sessions.context,handle,&info)==MMS_OK,"query public layout session");return info;}
    void Visibility(MmsHandle session,MmsHandle target,bool visible){
        const auto info=Info(session);MmsUiLayoutSessionValueV1 value{};value.h=Header<MmsUiLayoutSessionValueV1>();
        value.element=target;value.fields=MMS_LAYOUT_SESSION_VISIBLE;value.visible=visible;
        Check(sessions.apply(sessions.context,session,info.revision,{&value,1,sizeof(value),1})==MMS_OK,"update existing public visibility session");
    }
    MmsHandle Apply(MmsHandle target,uint32_t fields,bool visible=false,float width=0,float height=0,int order=0){
        MmsUiLayoutSessionTargetV1 claim{};claim.h=Header<MmsUiLayoutSessionTargetV1>();claim.element=target;claim.fields=fields;
        MmsHandle handle=0;Check(sessions.begin(sessions.context,{&claim,1,sizeof(claim),1},&handle)==MMS_OK&&handle,"begin public INPUT layout session");active.push_back(handle);
        const auto info=Info(handle);MmsUiLayoutSessionValueV1 value{};value.h=Header<MmsUiLayoutSessionValueV1>();value.element=target;value.fields=fields;
        if(fields&MMS_LAYOUT_SESSION_VISIBLE)value.visible=visible;if(fields&MMS_LAYOUT_SESSION_SIZE){value.width=width;value.height=height;}
        if(fields&MMS_LAYOUT_SESSION_ORDER)value.order=order;
        Check(sessions.apply(sessions.context,handle,info.revision,{&value,1,sizeof(value),1})==MMS_OK,"apply public INPUT layout session");return handle;
    }
    void Release(MmsHandle handle){auto info=Info(handle);if(info.state==MMS_LAYOUT_SESSION_ACTIVE)
        Check(sessions.rollback(sessions.context,handle,info.revision)==MMS_OK,"rollback public INPUT layout session");
        Check(sessions.release(sessions.context,handle)==MMS_OK,"release public INPUT layout session");
        active.erase(std::remove(active.begin(),active.end(),handle),active.end());}
    void PersistentVisible(bool visible){MmsHandle tx=0;auto& registry=manager->Ui();
        Check(ui.beginLayout(ui.context,registry.Revision(),&tx)==MMS_OK,"begin persistent user layout");
        MmsLayoutOpV1 op{};op.h=Header<MmsLayoutOpV1>();op.operation=MMS_LAYOUT_VISIBLE;op.elementId=Text(inputId);
        op.expectedGeneration=registry.elements.at(inputId).desc.generation;op.value.visible=visible;
        Check(ui.applyLayout(ui.context,tx,&op)==MMS_OK&&ui.commitLayout(ui.context,tx)==MMS_OK,"commit persistent INPUT visibility");
    }
};

void GeometryDraft(Fixture& f){
    const HWND original=f.field,wrapper=f.wrapper;const auto revision=f.Revision();
    const auto preferences=f.manager->Ui().ExportPreferences();f.Draft(L"37",1,1);RECT before{};Check(GetWindowRect(wrapper,&before),"read initial native geometry");
    const auto session=f.Apply(f.input,MMS_LAYOUT_SESSION_SIZE|MMS_LAYOUT_SESSION_ORDER,false,330,48,-10);f.Refresh();
    RECT changed{};Check(GetWindowRect(wrapper,&changed),"read temporary native geometry");
    Check(changed.right-changed.left!=before.right-before.left&&changed.left!=before.left,"size and order both change actual native geometry");
    f.Identity(original,wrapper);Check(Caption(f.field)==L"37"&&Selection(f.field)==std::array<DWORD,2>{1,1},"size/order preserve draft and logical caret");f.Clean();
    f.Release(session);f.Refresh();RECT after{};Check(GetWindowRect(wrapper,&after)&&EqualRect(&before,&after),"rollback restores original geometry");
    Check(Caption(f.field)==L"37"&&Selection(f.field)==std::array<DWORD,2>{1,1}&&f.Revision()==revision&&preferences==f.manager->Ui().ExportPreferences(),"geometry rollback preserves draft history and preferences");
    Escape(f.field);Check(Caption(f.field)==L"0","explicit Escape still cancels draft");f.Clean();
}
void TemporaryHide(Fixture& f,bool ancestor){
    const HWND original=f.field,wrapper=f.wrapper;const auto revision=f.Revision();const auto preferences=f.manager->Ui().ExportPreferences();
    f.Draft();
    // Queue a genuine EN_KILLFOCUS via EDIT before layout, but do not dispatch
    // its posted submission check until after the temporary hide is installed.
    SetFocus(f.window.sink);
    const auto session=f.Apply(ancestor?f.group:f.input,MMS_LAYOUT_SESSION_VISIBLE);f.Refresh(ancestor?"ancestor-hide":"direct-hide");
    f.Identity(original,wrapper);Check(!(GetWindowLongPtrW(wrapper,GWL_STYLE)&WS_VISIBLE),"temporary layout actually hides wrapper");
    Check(Caption(f.field)==L"37"&&Selection(f.field)==std::array<DWORD,2>{0,1},"temporary hide preserves completed draft and selection");
    Enter(f.field);Pump();f.Clean();Check(Caption(f.field)==L"37","hidden Enter and queued blur cannot submit or discard draft");
    const auto requests=focusRequests;f.Trace(ancestor?"ancestor-restore":"direct-restore","before-rollback");
    f.Release(session);f.Refresh(ancestor?"ancestor-restore":"direct-restore");
    f.Identity(original,wrapper);Check((GetWindowLongPtrW(wrapper,GWL_STYLE)&WS_VISIBLE)&&Caption(f.field)==L"37"&&
        Selection(f.field)==std::array<DWORD,2>{0,1},"temporary restore preserves draft selection and original HWND");
    Check(GetFocus()==f.window.sink&&focusRequests==requests,"restoration does not steal focus");Pump();f.Clean();
    Check(f.Revision()==revision&&preferences==f.manager->Ui().ExportPreferences(),"temporary visibility does not change history or persisted preferences");
    SetFocus(f.field);Check(Caption(f.field)==L"37","refocusing resumes existing draft");Escape(f.field);f.Clean();
    std::printf("PASS INPUT temporary %s hide preserves draft and selection without submission\n",ancestor?"ancestor":"direct");
}
void PersistentHide(Fixture& f){
    f.Draft();const HWND old=f.field;const auto revision=f.Revision();f.PersistentVisible(false);
    f.native->Refresh();f.native->Layout(10,850,180,true);Pump();Check(!IsWindow(old),"persistent hide unmounts and destroys old EDIT");f.Clean();
    f.PersistentVisible(true);f.Refresh();f.field=Field(f.native->Toolbar());f.wrapper=GetParent(f.field);
    Check(Caption(f.field)==L"0"&&f.Revision()==revision,"persistent restore creates canonical input without stale draft or history");
}
// Parent is internal-only in this slice: exercise the same production registry
// and native INPUT objects without claiming a new public C ABI field.
struct InternalParentPreview {
    Fixture& f;MmsHandle handle=0;
    explicit InternalParentPreview(Fixture& value):f(value){
        LayoutPreviewValue claim{};claim.element=f.input;claim.fields=PreviewParent;
        claim.parent=f.manager->Ui().elements.at(String(f.manager->Ui().EffectiveParent(
            f.manager->Ui().elements.at(f.inputId)))).handle;
        Check(f.manager->Ui().BeginLayoutPreview(*f.owner,{claim},handle)==MMS_OK&&handle,
            "begin internal parent preview for existing INPUT");
    }
    ~InternalParentPreview(){if(!handle)return;auto& registry=f.manager->Ui();LayoutPreviewInfo info{};
        if(registry.QueryLayoutPreview(*f.owner,handle,info)==MMS_OK&&info.state==LayoutPreviewState::Active)
            registry.EndLayoutPreview(*f.owner,handle,info.revision,false);
        registry.ReleaseLayoutPreview(*f.owner,handle);}
    void Apply(MmsHandle parent){auto& registry=f.manager->Ui();LayoutPreviewInfo info{};
        Check(registry.QueryLayoutPreview(*f.owner,handle,info)==MMS_OK,"query internal parent preview revision");
        LayoutPreviewValue value{};value.element=f.input;value.fields=PreviewParent;value.parent=parent;
        Check(registry.ApplyLayoutPreview(*f.owner,handle,info.revision,{value})==MMS_OK,
            "apply same-mount INPUT parent through internal preview API");}
    void End(bool commit){auto& registry=f.manager->Ui();LayoutPreviewInfo info{};
        Check(registry.QueryLayoutPreview(*f.owner,handle,info)==MMS_OK&&
            registry.EndLayoutPreview(*f.owner,handle,info.revision,commit)==MMS_OK,
            commit?"commit internal parent using persistent layout semantics":"rollback internal parent preview");
        Check(registry.ReleaseLayoutPreview(*f.owner,handle)==MMS_OK,"release internal parent preview");handle=0;}
};
void ParentDraft(Fixture& f){
    auto& registry=f.manager->Ui();const auto peerId=std::string(Fixture::id)+".peer";
    const auto hiddenId=std::string(Fixture::id)+".hidden-peer";MmsHandle peer=0,hidden=0;
    const auto add=[&](const std::string& id,MmsHandle& out){MmsUiElementV1 e{};e.h=Header<MmsUiElementV1>();
        e.id=Text(id);e.ownerPlugin=S(Fixture::id);e.parentId=S("mms.toolbar.main.extensions");e.kind=MMS_UI_GROUP;
        e.capabilities=MMS_UI_CONTAINER|MMS_UI_HIDE|MMS_UI_LAYOUT|MMS_UI_RESIZE;
        Check(f.ui.registerElement(f.ui.context,&e,&out)==MMS_OK,"register real sibling group in same toolbar mount");
        MmsUiLayoutHintsV1 hints{};hints.h=Header<MmsUiLayoutHintsV1>();hints.flow=MMS_FLOW_ROW;
        hints.minimum[0]=100;hints.minimum[1]=30;hints.preferred[0]=300;hints.preferred[1]=35;
        Check(f.layout.setDefaultHints(f.layout.context,Text(id),&hints)==MMS_OK,"give sibling real adaptive layout hints");};
    const auto visible=[&](const std::string& id,bool show){MmsHandle tx=0;
        Check(f.ui.beginLayout(f.ui.context,registry.Revision(),&tx)==MMS_OK,"begin persistent sibling visibility");
        MmsLayoutOpV1 op{};op.h=Header<MmsLayoutOpV1>();op.operation=MMS_LAYOUT_VISIBLE;op.elementId=Text(id);
        op.expectedGeneration=registry.elements.at(id).desc.generation;op.value.visible=show;
        Check(f.ui.applyLayout(f.ui.context,tx,&op)==MMS_OK&&f.ui.commitLayout(f.ui.context,tx)==MMS_OK,
            "persist sibling visibility independently of temporary parent");};
    const auto parentIs=[&](const std::string& id){const auto parent=registry.EffectiveParent(registry.elements.at(f.inputId));
        return parent.bytes==id.size()&&!std::memcmp(parent.data,id.data(),id.size());};
    add(peerId,peer);add(hiddenId,hidden);visible(hiddenId,false);f.Refresh();
    const auto prefs=registry.ExportPreferences();const auto revision=f.Revision();
    const HWND original=f.field,wrapper=f.wrapper;
    f.Draft(L"37",1,2);
    {
        InternalParentPreview session(f);session.Apply(peer);f.Refresh();f.Identity(original,wrapper);
        Check(parentIs(peerId)&&Caption(original)==L"37"&&Selection(original)==std::array<DWORD,2>{1,2},
            "visible sibling parent preview preserves real HWND text and selected draft range");f.Clean();
        session.End(false);f.Refresh();f.Identity(original,wrapper);
        Check(parentIs(f.groupId)&&Caption(original)==L"37"&&Selection(original)==std::array<DWORD,2>{1,2},
            "parent rollback restores original grouping without rebuilding INPUT");
    }
    Escape(original);f.Draft(L"37",0,1);SetFocus(f.window.sink);
    {
        InternalParentPreview session(f);session.Apply(hidden);
        Check(GetWindowLongPtrW(wrapper,GWL_STYLE)&WS_VISIBLE,"parent preview precedes actual native hide");
        DispatchQueuedBlur(wrapper);Enter(original);f.Clean();
        Check(Caption(original)==L"37"&&Selection(original)==std::array<DWORD,2>{0,1},
            "queued blur and Enter cannot commit draft while effective parent is hidden before refresh");
        f.Refresh();f.Identity(original,wrapper);
        Check(parentIs(hiddenId)&&!(GetWindowLongPtrW(wrapper,GWL_STYLE)&WS_VISIBLE)&&
            Caption(original)==L"37"&&Selection(original)==std::array<DWORD,2>{0,1},
            "temporary move into persistently hidden sibling preserves suspended INPUT draft");
        const auto requests=focusRequests;session.End(false);f.Refresh();f.Identity(original,wrapper);
        Check(parentIs(f.groupId)&&(GetWindowLongPtrW(wrapper,GWL_STYLE)&WS_VISIBLE)&&
            Caption(original)==L"37"&&Selection(original)==std::array<DWORD,2>{0,1}&&
            GetFocus()==f.window.sink&&focusRequests==requests,
            "parent rollback restores draft and range without reclaiming user focus");
    }
    Check(registry.ExportPreferences()==prefs&&f.Revision()==revision,"temporary parent moves leave saved layout and property history untouched");f.Clean();
    {
        InternalParentPreview session(f);session.Apply(peer);f.Refresh();session.End(true);f.Refresh();f.Identity(original,wrapper);
        Check(parentIs(peerId)&&Caption(original)==L"37"&&registry.ExportPreferences()!=prefs,
            "visible parent commit persists grouping while preserving original INPUT draft");f.Clean();
    }
    SetFocus(original);Enter(original);Pump();f.Clean(37,1,1);
    f.Draft(L"99",0,2);SetFocus(f.window.sink);
    {
        InternalParentPreview session(f);session.Apply(hidden);DispatchQueuedBlur(wrapper);f.Refresh();f.Identity(original,wrapper);
        Check(Caption(original)==L"99","second hidden parent preview still preserves unsubmitted draft");f.Clean(37,1,1);
        session.End(true);f.Refresh();
        Check(parentIs(hiddenId)&&!IsWindow(original)&&!IsWindow(wrapper),
            "committed hidden parent uses persistent hide contract and destroys suspended INPUT");f.Clean(37,1,1);
    }
    visible(hiddenId,true);f.Refresh();f.field=Field(f.native->Toolbar());f.wrapper=GetParent(f.field);
    Check(Caption(f.field)==L"37","persistently showing committed parent rebuilds canonical value without abandoned draft");f.Clean(37,1,1);
    std::puts("PASS INPUT internal parent preview: real drafts selection hidden ancestors rollback and persistent commit; no public parent ABI claim");
}
void HiddenBeforeNativeRefresh(Fixture& f,bool ancestor){
    const HWND original=f.field,wrapper=f.wrapper;const auto revision=f.Revision();
    const auto preferences=f.manager->Ui().ExportPreferences();f.Draft();SetFocus(f.window.sink);
    const auto session=f.Apply(ancestor?f.group:f.input,MMS_LAYOUT_SESSION_VISIBLE);
    Check(GetWindowLongPtrW(wrapper,GWL_STYLE)&WS_VISIBLE,"native INPUT has not yet reflected session hide");
    DispatchQueuedBlur(wrapper);f.Clean();
    Check(Caption(f.field)==L"37"&&Selection(f.field)==std::array<DWORD,2>{0,1},"pre-refresh queued blur preserves hidden draft and selection");
    Enter(f.field);f.Clean();
    Check((GetWindowLongPtrW(wrapper,GWL_STYLE)&WS_VISIBLE)&&Caption(f.field)==L"37","pre-refresh Enter neither commits nor cancels hidden draft");
    f.Refresh();f.Identity(original,wrapper);f.Clean();
    Check(!(GetWindowLongPtrW(wrapper,GWL_STYLE)&WS_VISIBLE)&&Caption(f.field)==L"37","native hide preserves draft after asynchronous gap");
    const auto requests=focusRequests;f.Release(session);f.Refresh();f.Identity(original,wrapper);
    Check(Caption(f.field)==L"37"&&Selection(f.field)==std::array<DWORD,2>{0,1}&&
        GetFocus()==f.window.sink&&focusRequests==requests,"gap rollback restores same draft without reclaiming focus");
    Check(f.Revision()==revision&&f.manager->Ui().ExportPreferences()==preferences,"gap handling preserves actual history and settings");f.Clean();
    SetFocus(f.field);Escape(f.field);f.Clean();
    std::printf("PASS INPUT pre-refresh %s hide blocks actual queued blur and Enter\n",ancestor?"ancestor":"direct");
}
void HiddenBaselineSessions(Fixture& f){
    const auto revision=f.Revision();const HWND previous=f.field;f.PersistentVisible(false);f.Refresh();
    Check(!IsWindow(previous),"saved-hidden INPUT is initially unmounted");f.Clean();
    const auto preferences=f.manager->Ui().ExportPreferences();const auto show=f.Apply(f.input,MMS_LAYOUT_SESSION_VISIBLE,true);f.Refresh();
    f.field=Field(f.native->Toolbar());f.wrapper=GetParent(f.field);const HWND original=f.field,wrapper=f.wrapper;
    Check((GetWindowLongPtrW(wrapper,GWL_STYLE)&WS_VISIBLE)&&Caption(f.field)==L"0","session A mounts saved-hidden INPUT with canonical value");
    f.Draft();SetFocus(f.window.sink);
    const auto ancestor=f.Apply(f.group,MMS_LAYOUT_SESSION_VISIBLE);DispatchQueuedBlur(wrapper);f.Clean();f.Refresh();
    f.Identity(original,wrapper);Check(!(GetWindowLongPtrW(wrapper,GWL_STYLE)&WS_VISIBLE)&&Caption(f.field)==L"37"&&
        Selection(f.field)==std::array<DWORD,2>{0,1},"session B ancestor hide preserves session A shown INPUT draft");
    Enter(f.field);Pump();f.Clean();const auto beforeAncestorRestore=focusRequests;f.Release(ancestor);f.Refresh();
    f.Identity(original,wrapper);Check((GetWindowLongPtrW(wrapper,GWL_STYLE)&WS_VISIBLE)&&Caption(f.field)==L"37"&&
        Selection(f.field)==std::array<DWORD,2>{0,1}&&GetFocus()==f.window.sink&&focusRequests==beforeAncestorRestore,
        "ancestor rollback restores saved-hidden INPUT draft without focus");
    // Reuse the same public session. Its false value is still an active
    // temporary hide even though it equals the persistent hidden baseline.
    SetFocus(f.field);SetFocus(f.window.sink);f.Visibility(show,f.input,false);DispatchQueuedBlur(wrapper);f.Clean();
    Enter(f.field);f.Clean();f.Refresh();f.Identity(original,wrapper);
    Check(!(GetWindowLongPtrW(wrapper,GWL_STYLE)&WS_VISIBLE)&&Caption(f.field)==L"37"&&Selection(f.field)==std::array<DWORD,2>{0,1},
        "session A false override retains HWND and draft over hidden baseline");
    const auto beforeOwnRestore=focusRequests;f.Visibility(show,f.input,true);f.Refresh();f.Identity(original,wrapper);
    Check((GetWindowLongPtrW(wrapper,GWL_STYLE)&WS_VISIBLE)&&Caption(f.field)==L"37"&&Selection(f.field)==std::array<DWORD,2>{0,1}&&
        GetFocus()==f.window.sink&&focusRequests==beforeOwnRestore,"session A true restores existing draft without focus");
    f.Clean();Check(f.Revision()==revision&&f.manager->Ui().ExportPreferences()==preferences,"stacked sessions never write history or persistent settings");
    SetFocus(f.field);Enter(f.field);Pump();f.Refresh();f.Clean(37,1,1);
    const auto committedRevision=f.Revision();
    Check(committedRevision==revision+1&&Caption(f.field)==L"37"&&f.manager->Ui().ExportPreferences()==preferences,
        "explicit submission of temporarily shown saved-hidden INPUT publishes one real history entry");
    f.Draft(L"59");f.Release(show);f.Refresh();
    Check(!IsWindow(original),"withdrawing final show restores persistent hide and destroys retained INPUT");f.Clean(37,1,1);
    f.PersistentVisible(true);f.Refresh();f.field=Field(f.native->Toolbar());f.wrapper=GetParent(f.field);
    Check(Caption(f.field)==L"37"&&f.Revision()==committedRevision,"persistent restore uses committed canonical value and discards later draft");
    std::puts("PASS INPUT saved-hidden baseline supports nested sessions, explicit commit and final unmount");
}
void ImmUnregisterDuringCancel(Fixture& f){
    const auto revision=f.Revision();const HWND original=f.field,wrapper=f.wrapper;
    f.Draft();SendMessageW(original,WM_IME_STARTCOMPOSITION,0,0);SetFocus(f.window.sink);
    const auto session=f.Apply(f.input,MMS_LAYOUT_SESSION_VISIBLE);
    MmsResult removed=MMS_E_FAILED;bool aliveInside=false,cleanInside=false;
    {
        ImmReentry imm(original,[&]{
            removed=f.ui.unregisterItem(f.ui.context,f.input);
            f.native->Refresh();
            aliveInside=IsWindow(original)&&IsWindow(wrapper);
            cleanInside=f.Value()==0&&f.Calls()==0&&f.Candidates()==0&&f.history->UndoCount()==0;
        });
        DispatchQueuedBlur(wrapper);imm.Verify();
        Check(removed==MMS_OK&&aliveInside&&cleanInside,"Notify may unregister INPUT but nested Refresh defers its active Impl destruction");
        Check(IsWindow(original)&&IsWindow(wrapper),"INPUT stays alive until outer focus-check message returns");f.Clean();
    }
    const auto info=f.Info(session);
    Check(info.state==MMS_LAYOUT_SESSION_REVOKED&&(info.reasons&MMS_LAYOUT_SESSION_TARGET_CHANGED),"public removal revokes input session through original target identity");
    f.Refresh();Check(!IsWindow(original)&&!IsWindow(wrapper),"later explicit Refresh destroys retired INPUT after callback unwind");
    f.Clean();Check(f.Revision()==revision,"IMM unregister callback did not publish a draft");f.Release(session);
    std::puts("PASS INPUT IMM unregister plus synchronous Refresh defers destruction until outer message returns");
}
void ImmRollbackDuringCancel(Fixture& f){
    const auto revision=f.Revision();const HWND original=f.field,wrapper=f.wrapper;
    f.Draft();SendMessageW(original,WM_IME_STARTCOMPOSITION,0,0);SetFocus(f.window.sink);
    const auto session=f.Apply(f.input,MMS_LAYOUT_SESSION_VISIBLE);
    bool rolledBackInside=false,aliveInside=false;const auto focusBefore=focusRequests;
    {
        ImmReentry imm(original,[&]{
            f.Release(session);
            rolledBackInside=true;
            f.native->Layout(10,850,180,true);
            aliveInside=IsWindow(original)&&IsWindow(wrapper);
        },FALSE); // Failure belongs to the old, now superseded cancellation.
        DispatchQueuedBlur(wrapper);imm.Verify();
        Check(rolledBackInside&&aliveInside&&GetFocus()==f.window.sink&&focusRequests==focusBefore,"Notify may rollback and request deferred layout without taking focus");
        f.Identity(original,wrapper);
        Check(Caption(original)==L"37"&&Selection(original)==std::array<DWORD,2>{0,1},"superseded failed IMM cancellation cannot clear restored draft");f.Clean();
    }
    Check(f.Revision()==revision,"IMM rollback alone does not edit shared history");
    f.Refresh();f.Identity(original,wrapper);
    Check((GetWindowLongPtrW(wrapper,GWL_STYLE)&WS_VISIBLE)&&Caption(original)==L"37"&&
        Selection(original)==std::array<DWORD,2>{0,1}&&GetFocus()==f.window.sink&&focusRequests==focusBefore,
        "explicit refresh after callback restores retained draft without taking focus");
    // End the synthetic composition through the original EDIT subclass before
    // the user explicitly submits. No real candidate window is exercised.
    SetFocus(original);SendMessageW(original,WM_IME_ENDCOMPOSITION,0,0);SendMessageW(original,WM_KEYUP,VK_RETURN,1);
    Enter(original);Pump();f.Refresh();
    f.Clean(37,1,1);Check(Caption(original)==L"37","restored draft remains usable for one explicit original edit transaction");
    std::puts("PASS INPUT IMM rollback supersedes old cancellation and preserves explicit submit");
}
void ImmUnregisterFromOuterLayout(Fixture& f){
    const auto revision=f.Revision();const HWND original=f.field,wrapper=f.wrapper;
    Check(f.manager->Ui().GeometryReady(),"outer-layout IMM case starts with published real geometry");
    f.Draft();SendMessageW(original,WM_IME_STARTCOMPOSITION,0,0);
    const auto session=f.Apply(f.input,MMS_LAYOUT_SESSION_VISIBLE);
    MmsResult removed=MMS_E_FAILED;bool aliveInside=false,cleanInside=false;
    {
        ImmReentry imm(original,[&]{
            removed=f.ui.unregisterItem(f.ui.context,f.input);
            f.native->Refresh();
            aliveInside=IsWindow(original)&&IsWindow(wrapper);
            cleanInside=f.Value()==0&&f.Calls()==0&&!f.Candidates()&&!f.history->UndoCount();
        });
        // Do not call Refresh or dispatch a blur here. The public Layout pass
        // first measures the retained tree and then pre-arms INPUT suspension;
        // Notify removes an ID which that already measured tree still holds.
        f.native->Layout(10,850,180,true);imm.Verify();
        Check(removed==MMS_OK&&aliveInside&&cleanInside,"outer Layout IMM unregister defers nested Refresh destruction");
        Check(IsWindow(original)&&IsWindow(wrapper),"outer Layout safely returns before retired INPUT destruction");
        Check(!f.manager->Ui().GeometryReady(),"outer Layout discards measured tree invalidated by IMM unregister");
        f.Clean();
    }
    MmsUiLayoutSnapshotV1 snapshot{};snapshot.h=Header<MmsUiLayoutSnapshotV1>();
    const auto staleResult=f.layout.acquireSnapshot(f.layout.context,&snapshot);
    if(staleResult==MMS_OK)f.layout.releaseSnapshot(f.layout.context,snapshot.handle);
    Check(staleResult==MMS_E_NOT_READY,"public layout snapshot refuses invalidated outer-layout geometry");
    const auto info=f.Info(session);
    Check(info.state==MMS_LAYOUT_SESSION_REVOKED&&(info.reasons&MMS_LAYOUT_SESSION_TARGET_CHANGED),"outer Layout unregister revokes original session target");
    f.Refresh();Check(!IsWindow(original)&&!IsWindow(wrapper),"explicit Refresh after outer Layout destroys retired INPUT");
    Check(f.manager->Ui().GeometryReady(),"explicit Refresh publishes recovered geometry after outer-layout reentry");
    snapshot={};snapshot.h=Header<MmsUiLayoutSnapshotV1>();
    Check(f.layout.acquireSnapshot(f.layout.context,&snapshot)==MMS_OK,"public layout snapshot becomes available after cleanup");
    bool containsRetired=false;
    for(uint64_t i=0;i<snapshot.items.count;++i){
        const auto& item=*reinterpret_cast<const MmsUiLayoutItemV1*>(static_cast<const unsigned char*>(snapshot.items.data)+i*snapshot.items.stride);
        containsRetired=containsRetired||String(item.elementId)==f.inputId;
    }
    f.layout.releaseSnapshot(f.layout.context,snapshot.handle);
    Check(!containsRetired,"recovered public geometry excludes retired INPUT");
    f.Clean();Check(f.Revision()==revision,"outer-layout cancellation and recovery never commit draft history");f.Release(session);
    std::puts("PASS INPUT outer Layout IMM unregister rejects stale measured tree and recovers on explicit Refresh");
}
void StopRetractionAndSubmit(Fixture& f){
    f.Draft();const HWND old=f.field,wrapper=f.wrapper;const auto session=f.Apply(f.group,MMS_LAYOUT_SESSION_VISIBLE);f.Refresh();
    SetFocus(f.window.sink);Pump();f.Clean();
    // Trusted host lifecycle notification only; owner remains loaded here.
    // This does not write owner.stopping or synthesize input internals.
    f.manager->Ui().RevokeLayoutPreviews(*f.owner);const auto revoked=f.Info(session);
    Check(revoked.state==MMS_LAYOUT_SESSION_REVOKED&&(revoked.reasons&MMS_LAYOUT_SESSION_STOPPING),"host revocation reports STOPPING through public session query");
    f.Refresh();f.Identity(old,wrapper);Check(Caption(f.field)==L"37"&&GetFocus()==f.window.sink,"session-only withdrawal preserves draft without focus or commit");f.Clean();f.Release(session);
    SetFocus(f.field);Enter(f.field);Pump();f.Refresh();f.Clean(37,1,1);Check(Caption(f.field)==L"37","explicit Enter commits once through original property edit bridge");
    const auto undo=std::string(Fixture::id)+".undo";
    Check(f.ui.invoke(f.ui.context,Text(undo),{},{})==MMS_OK,"original plugin undo uses common history");f.Refresh();
    Check(f.Value()==0&&f.history->UndoCount()==0&&f.history->RedoCount()==1&&f.Calls()==1,"undo restores actual serialized property");
    const auto redo=std::string(Fixture::id)+".redo";
    Check(f.ui.invoke(f.ui.context,Text(redo),{},{})==MMS_OK,"original plugin redo uses common history");f.Refresh();f.Clean(37,1,1);
    f.Draft(L"59");const auto stopping=f.Apply(f.input,MMS_LAYOUT_SESSION_VISIBLE);(void)stopping;f.Refresh();
    const auto calls=f.Calls();const auto steps=f.history->UndoCount();
    Check(f.manager->Stop(),"actual owner stop retires active session and property");f.stopped=true;f.active.clear();f.owner=nullptr;
    f.native->Refresh();Pump();Check(!IsWindow(old)&&f.Calls()==calls&&!f.Candidates()&&f.history->UndoCount()==steps,"complete owner stop destroys input without committing retained draft");
}
}

int wmain(int argc,wchar_t** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(argc!=2){std::fputs("usage: layout_session_input_tests <absolute-manager-edit-probe.dll>\n",stderr);return 2;}
    try{
        const fs::path source(argv[1]);Check(source.is_absolute()&&fs::is_regular_file(source),"existing absolute edit probe source");
        wchar_t path[32768]{};Check(GetModuleFileNameW(nullptr,path,32768)>0,"resolve own copied test executable");
        const fs::path executable(path);Check(executable.stem()==L"layout_session_input_tests","run dedicated INPUT test executable only");
        const auto directory=executable.parent_path()/(L"input-session-run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        Check(fs::create_directory(directory),"claim unique fixture directory without deleting existing data");
        std::printf("Evidence directory: %s\n",Utf8(directory.wstring()).c_str());
        FocusGuards focus;
        {
            Fixture fixture(directory/L"ordinary",source);
            GeometryDraft(fixture);TemporaryHide(fixture,false);TemporaryHide(fixture,true);
            HiddenBeforeNativeRefresh(fixture,false);HiddenBeforeNativeRefresh(fixture,true);
            PersistentHide(fixture);StopRetractionAndSubmit(fixture);
        }
        {
            Fixture fixture(directory/L"saved-hidden",source);
            HiddenBaselineSessions(fixture);
        }
        {
            Fixture fixture(directory/L"internal-parent",source);
            ParentDraft(fixture);
        }
        {
            Fixture fixture(directory/L"imm-unregister",source);
            ImmUnregisterDuringCancel(fixture);
        }
        {
            Fixture fixture(directory/L"imm-rollback",source);
            ImmRollbackDuringCancel(fixture);
        }
        {
            Fixture fixture(directory/L"imm-outer-layout",source);
            ImmUnregisterFromOuterLayout(fixture);
        }
        std::printf("layout_session_input_tests PASS: %u checks; logical EDIT state only, no IME or visible-pixel claim\n",checks);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"layout_session_input_tests FAIL: %s\n",error.what());return 1;}
}
