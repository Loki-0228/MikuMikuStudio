// Real UiRegistry, public session service, NativeUi layout/dispatch and HMENU.
// Only this executable's TrackPopupMenuEx import is replaced: no menu is shown
// and no mouse/keyboard/focus input is sent. The owner is transparent, offscreen
// and nonactivating. This is not a visual or physical-input acceptance test.
#include "../../src/plugins/native_ui.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include <commctrl.h>

extern "C" decltype(&TrackPopupMenuEx) __imp_TrackPopupMenuEx;
using namespace mikumikustudio::plugins;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);throw std::runtime_error(#x);}}while(false)
MmsString S(const char* text){return {text,std::strlen(text)};}
template<class T> MmsSpan Span(const T* data,size_t count=1){return {data,uint64_t(count),sizeof(T),1};}

struct Window {
    HWND handle=nullptr;
    unsigned activations=0,focusRequests=0;
    static LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
        auto* self=reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_NCCREATE){self=static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(self && msg==WM_ACTIVATE && LOWORD(wp)!=WA_INACTIVE)++self->activations;
        if(self && msg==WM_SETFOCUS)++self->focusRequests;
        if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
        if(msg==WM_NCHITTEST)return HTTRANSPARENT;
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
    Window(){
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES};CHECK(InitCommonControlsEx(&controls));
        WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=Proc;cls.lpszClassName=L"MMS.SessionOverflow.Test";
        CHECK(RegisterClassW(&cls));
        handle=CreateWindowExW(WS_EX_LAYERED|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT,cls.lpszClassName,L"MMS invisible overflow fixture",
            WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,GetSystemMetrics(SM_XVIRTUALSCREEN)-1600,GetSystemMetrics(SM_YVIRTUALSCREEN)-1000,
            900,600,nullptr,nullptr,cls.hInstance,this);
        CHECK(handle);
        CHECK(SetLayeredWindowAttributes(handle,0,0,LWA_ALPHA));
        const auto menu=CreateMenu();CHECK(menu);if(!SetMenu(handle,menu)){DestroyMenu(menu);CHECK(false);}
        ShowWindow(handle,SW_SHOWNOACTIVATE);Invisible();
    }
    ~Window(){if(handle){const auto menu=GetMenu(handle);SetMenu(handle,nullptr);if(menu)DestroyMenu(menu);DestroyWindow(handle);}UnregisterClassW(L"MMS.SessionOverflow.Test",GetModuleHandleW(nullptr));}
    void Invisible() const {
        CHECK(IsWindow(handle));BYTE alpha=255;DWORD flags=0;COLORREF color=0;
        CHECK(GetLayeredWindowAttributes(handle,&color,&alpha,&flags) && (flags&LWA_ALPHA) && !alpha);
        CHECK(!activations && !focusRequests);
        const auto focus=GetFocus();CHECK(focus!=handle && (!focus || !IsChild(handle,focus)));
        RECT rect{},intersection{},desktop{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};
        desktop.right=desktop.left+GetSystemMetrics(SM_CXVIRTUALSCREEN);desktop.bottom=desktop.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);
        CHECK(GetWindowRect(handle,&rect) && !IntersectRect(&intersection,&rect,&desktop));
    }
};
UINT Dpi(HWND window){
    using Function=UINT(WINAPI*)(HWND);const auto query=reinterpret_cast<Function>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
    if(query){const auto value=query(window);if(value)return value;}
    const auto dc=GetDC(window);CHECK(dc);const int value=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(window,dc);CHECK(value>0);return UINT(value);
}
std::wstring Caption(HWND window){wchar_t text[128]{};GetWindowTextW(window,text,128);return text;}
struct Fixture {
    Window window;
    UiRegistry registry;
    UiClient owner{&registry,"example.sessionoverflow"};
    MmsUiApiV1 ui=registry.Api(owner);
    MmsUiLayoutApiV1 layout=registry.LayoutApi(owner);
    MmsUiLayoutApiV1_1 history=registry.LayoutHistoryApi(owner);
    MmsUiLayoutSessionApiV1 session=registry.LayoutSessionApi(owner);
    std::unique_ptr<NativeUi> native;
    std::array<MmsHandle,3> elements{},commands{};
    std::array<HWND,3> controls{};
    std::array<unsigned,3> calls{};
    std::array<std::string,3> ids{},commandIds{};
    struct Context{Fixture* fixture;size_t index;}contexts[3]{{this,0},{this,1},{this,2}};
    std::string preferences;
    uint64_t preferencesRevision=0;
    uint32_t undo=0,redo=0;
    int width=190;
    Fixture(){
        for(size_t i=0;i<3;++i){
            ids[i]=owner.id+".button"+std::to_string(i);commandIds[i]=ids[i]+".command";
            const auto label="Command "+std::to_string(i);
            MmsCommandDescV1 command{};command.info.h=Header<MmsCommandDescV1>();command.info.id=Text(commandIds[i]);
            command.info.ownerPlugin=Text(owner.id);command.info.label=Text(label);command.context=&contexts[i];
            command.getState=[](void*,MmsObjectRef,MmsCommandStateV1* out)->MmsResult{*out={Header<MmsCommandStateV1>(),1,0,1,0,{}};return MMS_OK;};
            command.invoke=[](void* context,MmsObjectRef,MmsSpan args,MmsHandle edit)->MmsResult{
                auto& c=*static_cast<Context*>(context);CHECK(!args.count && !edit);++c.fixture->calls[c.index];return MMS_OK;
            };
            CHECK(ui.registerCommand(ui.context,&command,&commands[i])==MMS_OK);
            MmsUiElementV1 element{};element.h=Header<MmsUiElementV1>();element.id=Text(ids[i]);element.ownerPlugin=Text(owner.id);
            element.parentId=S("mms.toolbar.main.extensions");element.commandId=Text(commandIds[i]);element.label=Text(label);
            element.kind=MMS_UI_BUTTON;element.capabilities=MMS_UI_LAYOUT|MMS_UI_HIDE|MMS_UI_MOVE|MMS_UI_RESIZE;
            CHECK(ui.registerElement(ui.context,&element,&elements[i])==MMS_OK);
            MmsUiLayoutHintsV1 hints{};hints.h=Header<MmsUiLayoutHintsV1>();hints.minimum[0]=10;hints.minimum[1]=16;
            hints.preferred[0]=90;hints.preferred[1]=24;hints.maximum[0]=600;hints.maximum[1]=120;
            hints.overflow=MMS_UI_OVERFLOW_MENU;hints.order=int32_t(i);
            CHECK(layout.setDefaultHints(layout.context,element.id,&hints)==MMS_OK);
        }
        native=std::make_unique<NativeUi>(registry,window.handle,Log{});native->Refresh();Arrange(190);
        for(auto child=GetWindow(native->Toolbar(),GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))
            for(size_t i=0;i<3;++i)if(Caption(child)==L"Command "+std::to_wstring(i))controls[i]=child;
        for(auto control:controls)CHECK(control);
        preferences=registry.ExportPreferences();preferencesRevision=registry.PreferencesRevision();
        MmsUiLayoutHistoryInfoV1 info{};info.h=Header<MmsUiLayoutHistoryInfoV1>();CHECK(history.queryHistory(history.context,&info)==MMS_OK);undo=info.undoDepth;redo=info.redoDepth;
        Identity();
    }
    ~Fixture(){registry.RevokeLayoutPreviews(owner);native.reset();registry.Retire(owner);}
    void Arrange(int dipWidth){width=dipWidth;native->Layout(0,MulDiv(width,int(Dpi(window.handle)),96)+GetSystemMetrics(SM_CXVSCROLL),MulDiv(40,int(Dpi(window.handle)),96),true);}
    void Refresh(){native->Refresh();Arrange(width);}
    HWND More() const {
        for(auto child=GetWindow(native->Toolbar(),GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))if(Caption(child)==L"更多")return child;
        return nullptr;
    }
    unsigned NativeId(size_t index) const {return unsigned(GetDlgCtrlID(controls[index]));}
    bool Shown(size_t index) const {return (GetWindowLongPtrW(controls[index],GWL_STYLE)&WS_VISIBLE)!=0;}
    void Identity(){
        window.Invisible();
        for(size_t i=0;i<3;++i){CHECK(IsWindow(controls[i]) && GetParent(controls[i])==native->Toolbar());
            CHECK(registry.elements.at(ids[i]).handle==elements[i] && registry.commands.at(commandIds[i])->handle==commands[i]);}
        CHECK(preferences==registry.ExportPreferences() && preferencesRevision==registry.PreferencesRevision());
        MmsUiLayoutHistoryInfoV1 info{};info.h=Header<MmsUiLayoutHistoryInfoV1>();CHECK(history.queryHistory(history.context,&info)==MMS_OK && info.undoDepth==undo && info.redoDepth==redo);
    }
    MmsUiLayoutItemV1 Geometry(size_t index){
        MmsUiLayoutSnapshotV1 snapshot{};snapshot.h=Header<MmsUiLayoutSnapshotV1>();CHECK(layout.acquireSnapshot(layout.context,&snapshot)==MMS_OK);
        struct Lease{MmsUiLayoutApiV1 api;MmsHandle handle;~Lease(){api.releaseSnapshot(api.context,handle);}}lease{layout,snapshot.handle};
        for(uint64_t i=0;i<snapshot.items.count;++i){const auto& item=*reinterpret_cast<const MmsUiLayoutItemV1*>(static_cast<const unsigned char*>(snapshot.items.data)+i*snapshot.items.stride);
            if(String(item.elementId)==ids[index]){auto copy=item;copy.elementId={};return copy;}}
        CHECK(false);return {};
    }
    MmsUiLayoutSessionInfoV1 Info(MmsHandle handle){MmsUiLayoutSessionInfoV1 out{};out.h=Header<MmsUiLayoutSessionInfoV1>();CHECK(session.query(session.context,handle,&out)==MMS_OK);return out;}
    MmsHandle Begin(size_t index,uint32_t fields){
        MmsUiLayoutSessionTargetV1 target{Header<MmsUiLayoutSessionTargetV1>(),elements[index],fields,0};MmsHandle handle=0;
        CHECK(session.begin(session.context,Span(&target),&handle)==MMS_OK && handle);return handle;
    }
    void Apply(MmsHandle handle,size_t index,uint32_t fields,bool visible=false,float size=0,int32_t order=0){
        MmsUiLayoutSessionValueV1 value{Header<MmsUiLayoutSessionValueV1>(),elements[index],fields,uint32_t(visible),size,size?24.f:0.f,order,0};
        CHECK(session.apply(session.context,handle,Info(handle).revision,Span(&value))==MMS_OK);
    }
    void Rollback(MmsHandle handle){CHECK(session.rollback(session.context,handle,Info(handle).revision)==MMS_OK);CHECK(Info(handle).state==MMS_LAYOUT_SESSION_ROLLED_BACK);CHECK(session.release(session.context,handle)==MMS_OK);}
};

struct MenuEntry{unsigned id;std::wstring label;};
std::vector<MenuEntry> Entries(HMENU menu){
    CHECK(IsMenu(menu));const int count=GetMenuItemCount(menu);CHECK(count>=0);std::vector<MenuEntry> out;
    for(int i=0;i<count;++i){wchar_t text[128]{};MENUITEMINFOW info{sizeof(info)};info.fMask=MIIM_ID|MIIM_STRING|MIIM_FTYPE|MIIM_SUBMENU;info.dwTypeData=text;info.cch=128;
        CHECK(GetMenuItemInfoW(menu,UINT(i),TRUE,&info) && !(info.fType&MFT_SEPARATOR) && !info.hSubMenu);out.push_back({info.wID,text});}
    return out;
}
void Expect(HMENU menu,Fixture& f,std::initializer_list<size_t> indices){
    const auto entries=Entries(menu);CHECK(entries.size()==indices.size());size_t n=0;
    for(const auto i:indices){CHECK(entries[n].id==f.NativeId(i) && entries[n].label==L"Command "+std::to_wstring(i));++n;}
}
struct Popup {
    Fixture& fixture;
    std::function<unsigned(HMENU)> callback;
    std::exception_ptr error;
    HMENU seen=nullptr;
    unsigned visited=0;
    inline static thread_local Popup* active=nullptr;
    static BOOL WINAPI Track(HMENU menu,UINT flags,int,int,HWND owner,LPTPMPARAMS){
        // This replacement is installed only around the owning fixture's
        // synchronous click. An unexpected call fails instead of showing UI.
        if(!active){++failures;return 0;}
        auto& self=*active;++self.visited;self.seen=menu;
        try{CHECK(owner==self.fixture.native->Toolbar() && (flags&TPM_RETURNCMD));CHECK(self.fixture.native->PopupActive());
            self.fixture.window.Invisible();return BOOL(self.callback(menu));}
        catch(...){self.error=std::current_exception();return 0;}
    }
    static bool Write(decltype(&TrackPopupMenuEx) value){
        DWORD old=0,ignored=0;if(!VirtualProtect(&__imp_TrackPopupMenuEx,sizeof(value),PAGE_READWRITE,&old))return false;
        __imp_TrackPopupMenuEx=value;return VirtualProtect(&__imp_TrackPopupMenuEx,sizeof(value),old,&ignored)!=FALSE;
    }
    Popup(Fixture& f,std::function<unsigned(HMENU)> action):fixture(f),callback(std::move(action)){}
    void Run(){
        CHECK(!active && !fixture.native->PopupActive());const auto more=fixture.More();CHECK(more && IsWindowVisible(more));
        struct ImportGuard {
            decltype(&TrackPopupMenuEx) original=__imp_TrackPopupMenuEx;
            ~ImportGuard(){if(!Popup::Write(original))++failures;Popup::active=nullptr;}
        }guard;
        active=this;CHECK(Write(Track));
        SendMessageW(fixture.native->Toolbar(),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(more),BN_CLICKED),reinterpret_cast<LPARAM>(more));
        CHECK(visited==1 && !fixture.native->PopupActive());if(error)std::rethrow_exception(error);
        CHECK(seen && !IsMenu(seen));fixture.Identity();
    }
};

void SizeAndOrder(){
    Fixture f;CHECK(f.Shown(0) && !f.Shown(1) && !f.Shown(2));
    Popup(f,[&](HMENU menu){Expect(menu,f,{1,2});return 0u;}).Run();
    const auto size=f.Begin(0,MMS_LAYOUT_SESSION_SIZE);f.Apply(size,0,MMS_LAYOUT_SESSION_SIZE,false,180);f.Arrange(190);
    CHECK(!f.Shown(0) && !f.Shown(1) && !f.Shown(2));CHECK(f.Geometry(0).overflowed && !f.Geometry(0).visible);
    Popup(f,[&](HMENU menu){Expect(menu,f,{0,1,2});return f.NativeId(0);}).Run();CHECK((f.calls==std::array<unsigned,3>{1,0,0}));
    f.Rollback(size);f.Refresh();CHECK(f.Shown(0) && !f.Shown(1) && !f.Shown(2));
    Popup(f,[&](HMENU menu){Expect(menu,f,{1,2});return f.NativeId(2);}).Run();CHECK((f.calls==std::array<unsigned,3>{1,0,1}));
    f.Arrange(60);const auto order=f.Begin(2,MMS_LAYOUT_SESSION_ORDER);f.Apply(order,2,MMS_LAYOUT_SESSION_ORDER,false,0,-10);f.Arrange(60);
    CHECK(f.Geometry(2).effective.order==-10);
    Popup(f,[&](HMENU menu){Expect(menu,f,{2,0,1});return f.NativeId(2);}).Run();CHECK((f.calls==std::array<unsigned,3>{1,0,2}));
    f.Rollback(order);f.Refresh();Popup(f,[&](HMENU menu){Expect(menu,f,{0,1,2});return 0u;}).Run();
}
void OpenPopupMutation(){
    Fixture f;f.Arrange(60);const auto order=f.Begin(2,MMS_LAYOUT_SESSION_ORDER);
    Popup(f,[&](HMENU menu){Expect(menu,f,{0,1,2});const auto selected=f.NativeId(0);
        f.Apply(order,2,MMS_LAYOUT_SESSION_ORDER,false,0,-10);f.Arrange(60);
        CHECK(f.native->PopupActive());return selected;}).Run();
    CHECK((f.calls==std::array<unsigned,3>{0,0,0})); // Changed layout invalidates the old menu choice.
    Popup(f,[&](HMENU menu){Expect(menu,f,{2,0,1});return f.NativeId(2);}).Run();CHECK((f.calls==std::array<unsigned,3>{0,0,1}));
    f.Rollback(order);f.Refresh();f.Arrange(110);
    const auto size=f.Begin(0,MMS_LAYOUT_SESSION_SIZE);
    Popup(f,[&](HMENU menu){Expect(menu,f,{0,1,2});const auto selected=f.NativeId(0);
        f.Apply(size,0,MMS_LAYOUT_SESSION_SIZE,false,60);f.Arrange(110);return selected;}).Run();
    CHECK((f.calls==std::array<unsigned,3>{0,0,1}));CHECK(f.Shown(0) && !f.Geometry(0).overflowed);
    Popup(f,[&](HMENU menu){Expect(menu,f,{1,2});return f.NativeId(1);}).Run();CHECK((f.calls==std::array<unsigned,3>{0,1,1}));
    f.Rollback(size);f.Refresh();
}
void HideRollbackAndStop(){
    Fixture f;f.Arrange(60);const auto hidden=f.Begin(1,MMS_LAYOUT_SESSION_VISIBLE);
    Popup(f,[&](HMENU menu){Expect(menu,f,{0,1,2});const auto selected=f.NativeId(1);
        f.Apply(hidden,1,MMS_LAYOUT_SESSION_VISIBLE);f.Refresh();return selected;}).Run();
    CHECK((f.calls==std::array<unsigned,3>{0,0,0}));CHECK(!f.Geometry(1).visible && !f.Geometry(1).overflowed);
    Popup(f,[&](HMENU menu){Expect(menu,f,{0,2});return f.NativeId(2);}).Run();CHECK((f.calls==std::array<unsigned,3>{0,0,1}));
    Popup(f,[&](HMENU menu){Expect(menu,f,{0,2});const auto selected=f.NativeId(0);f.Rollback(hidden);f.Refresh();return selected;}).Run();
    CHECK((f.calls==std::array<unsigned,3>{0,0,1}));
    Popup(f,[&](HMENU menu){Expect(menu,f,{0,1,2});return f.NativeId(1);}).Run();CHECK((f.calls==std::array<unsigned,3>{0,1,1}));
    const auto stopping=f.Begin(2,MMS_LAYOUT_SESSION_ORDER);f.Apply(stopping,2,MMS_LAYOUT_SESSION_ORDER,false,0,-10);f.Refresh();
    Popup(f,[&](HMENU menu){Expect(menu,f,{2,0,1});const auto selected=f.NativeId(2);
        f.owner.stopping=true;f.registry.RevokeLayoutPreviews(f.owner);f.Refresh();
        CHECK(f.Info(stopping).state==MMS_LAYOUT_SESSION_REVOKED && (f.Info(stopping).reasons&MMS_LAYOUT_SESSION_STOPPING));
        // A stopping provider can retain disabled presentations until Retire.
        // They must never remain callable through a stale popup selection.
        for(const auto& item:Entries(menu)){
            const auto state=GetMenuState(menu,item.id,MF_BYCOMMAND);
            CHECK(state!=UINT(-1) && (state&(MF_DISABLED|MF_GRAYED)));
        }
        return selected;}).Run();
    CHECK((f.calls==std::array<unsigned,3>{0,1,1}));CHECK(f.session.release(f.session.context,stopping)==MMS_OK);
    for(size_t i=0;i<3;++i){f.native->Command(f.NativeId(i));SendMessageW(f.native->Toolbar(),WM_COMMAND,f.NativeId(i),0);}
    CHECK((f.calls==std::array<unsigned,3>{0,1,1}));
    const std::array<unsigned,3> oldIds{f.NativeId(0),f.NativeId(1),f.NativeId(2)};
    f.registry.Retire(f.owner);f.Refresh();
    for(size_t i=0;i<3;++i){CHECK(!IsWindow(f.controls[i]));CHECK(!f.native->Command(oldIds[i]));}
    CHECK(!IsWindowVisible(f.More()) && (f.calls==std::array<unsigned,3>{0,1,1}));f.window.Invisible();
}
}
int main(){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{SizeAndOrder();OpenPopupMutation();HideRollbackAndStop();}
    catch(const std::exception& error){std::fprintf(stderr,"layout_session_overflow_tests: %s (%u checks, %u failures)\n",error.what(),checks,failures);return 1;}
    if(failures)return 1;
    std::printf("layout_session_overflow_tests: %u checks passed; real native layout/menu/dispatch, simulated popup return only; no visual acceptance\n",checks);return 0;
}
