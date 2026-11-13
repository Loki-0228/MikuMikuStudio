// Real hidden native windows and three independent registered factories.
// Tab selection uses TCM_SETCURSEL + TCN_SELCHANGE, not physical mouse input.
// This fixture verifies presentation and layout history; it owns no MMD scene.
#include "../../src/plugins/native_ui.hpp"
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace mikumikustudio::plugins;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)){++failures;std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);} } while(false)
MmsString S(const char* text){return {text,std::strlen(text)};}
template<class T>MmsSpan Span(const T* values,size_t count){return {values,count,sizeof(T),1};}
MmsUiLayoutSessionTargetV1 Target(MmsHandle element,uint32_t fields){return {Header<MmsUiLayoutSessionTargetV1>(),element,fields,0};}
MmsUiLayoutSessionValueV1 Value(MmsHandle element,uint32_t fields,bool visible=false,float width=0,float height=0,int order=0){
    return {Header<MmsUiLayoutSessionValueV1>(),element,fields,(fields&1u)?uint32_t(visible):0u,
        (fields&2u)?width:0.f,(fields&2u)?height:0.f,(fields&4u)?order:0,0};
}
struct Factory {
    HWND child=nullptr,edit=nullptr;
    unsigned creates=0,destroys=0,focuses=0,resizes=0;
    bool failResize=false;
    MmsUiSurfaceDescV1 Descriptor(const char* id){
        MmsUiSurfaceDescV1 d{};d.h=Header<MmsUiSurfaceDescV1>();d.id=S(id);d.context=this;
        d.minimumWidth=d.minimumHeight=20;d.preferredWidth=160;d.preferredHeight=80;
        d.createView=[](void* p,const MmsUiMountV1* mount,void** out,uintptr_t* window)->MmsResult{
            auto& f=*static_cast<Factory*>(p);++f.creates;*out=nullptr;*window=0;
            f.child=CreateWindowExW(WS_EX_CONTROLPARENT,L"STATIC",L"Independent panel surface",WS_CHILD,
                mount->bounds.x,mount->bounds.y,mount->bounds.width,mount->bounds.height,
                reinterpret_cast<HWND>(mount->parentWindow),nullptr,GetModuleHandleW(nullptr),nullptr);
            if(!f.child)return MMS_E_FAILED;
            f.edit=CreateWindowExW(0,L"EDIT",L"Unsubmitted panel draft",WS_CHILD|WS_VISIBLE,
                2,2,140,22,f.child,nullptr,GetModuleHandleW(nullptr),nullptr);
            if(!f.edit){DestroyWindow(f.child);f.child=nullptr;return MMS_E_FAILED;}
            SendMessageW(f.edit,EM_SETMODIFY,TRUE,0);*out=&f;*window=reinterpret_cast<uintptr_t>(f.child);return MMS_OK;
        };
        d.resizeView=[](void* p,const MmsUiRectV1* bounds,uint32_t){
            auto& f=*static_cast<Factory*>(p);++f.resizes;
            if(f.failResize)throw std::runtime_error("panel resize refused");
            if(!MoveWindow(f.child,bounds->x,bounds->y,bounds->width,bounds->height,FALSE))throw std::runtime_error("panel move failed");
        };
        d.showView=[](void* p,uint32_t visible){auto& f=*static_cast<Factory*>(p);ShowWindow(f.child,visible?SW_SHOWNA:SW_HIDE);};
        d.focusView=[](void* p)->MmsResult{++static_cast<Factory*>(p)->focuses;return MMS_OK;};
        d.destroyView=[](void* p){auto& f=*static_cast<Factory*>(p);++f.destroys;DestroyWindow(f.child);f.child=f.edit=nullptr;};
        return d;
    }
    bool Visible()const{return child&&(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;}
};
struct Fixture {
    UiRegistry registry;
    UiClient owner{&registry,"example.panelpreview"};
    MmsUiApiV1 ui=registry.Api(owner);
    MmsUiSurfaceApiV1 surfaces=registry.SurfaceApi(owner);
    MmsUiLayoutSessionApiV1 api=registry.LayoutSessionApi(owner,1);
    MmsUiLayoutApiV1_1 history=registry.LayoutHistoryApi(owner);
    std::array<Factory,3> factories;
    std::array<MmsHandle,3> panel{},surface{};
    std::array<int32_t,3> initialOrder{};
    const std::array<const char*,3> ids{"example.panelpreview.a","example.panelpreview.b","example.panelpreview.c"};
    const std::array<const char*,3> surfaceIds{"example.panelpreview.factory-a","example.panelpreview.factory-b","example.panelpreview.factory-c"};
    const std::array<const char*,3> labels{"Panel A","Panel B","Panel C"};
    HWND host=nullptr,tabs=nullptr;
    MmsHandle toolbar=0;
    std::unique_ptr<NativeUi> native;
    Fixture(){
        host=CreateWindowExW(0,L"STATIC",L"Panel preview hidden host",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
            0,0,900,650,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        CHECK(host!=nullptr);if(!host)throw std::runtime_error("host");
        for(size_t i=0;i<3;++i){auto d=factories[i].Descriptor(surfaceIds[i]);CHECK(surfaces.registerSurface(surfaces.context,&d,&surface[i])==MMS_OK);Register(i);}
        MmsCommandDescV1 command{};command.info.h=Header<MmsCommandDescV1>();command.info.id=S("example.panelpreview.command");command.info.ownerPlugin=Text(owner.id);
        command.getState=[](void*,MmsObjectRef,MmsCommandStateV1* state)->MmsResult{state->enabled=state->visible=1;return MMS_OK;};
        command.invoke=[](void*,MmsObjectRef,MmsSpan,MmsHandle)->MmsResult{return MMS_OK;};MmsHandle registered=0;
        CHECK(ui.registerCommand(ui.context,&command,&registered)==MMS_OK);
        MmsUiElementV1 button{};button.h=Header<MmsUiElementV1>();button.id=S("example.panelpreview.toolbar");button.ownerPlugin=Text(owner.id);
        button.parentId=S("mms.toolbar.main.extensions");button.commandId=command.info.id;button.label=S("Toolbar sentinel");
        button.kind=MMS_UI_BUTTON;button.capabilities=MMS_UI_HIDE|MMS_UI_LAYOUT;
        CHECK(ui.registerElement(ui.context,&button,&toolbar)==MMS_OK);
        native=std::make_unique<NativeUi>(registry,host,Log{});Refresh();
        EnumChildWindows(host,[](HWND w,LPARAM p)->BOOL{wchar_t type[80]{};GetClassNameW(w,type,80);
            if(!std::wcscmp(type,WC_TABCONTROLW))*reinterpret_cast<HWND*>(p)=w;return TRUE;},reinterpret_cast<LPARAM>(&tabs));
        CHECK(tabs&&TabCtrl_GetItemCount(tabs)==3);if(!tabs)throw std::runtime_error("tabs");
        CHECK(Selected()==L"Panel A");CHECK(registry.GeometryReady());
        // The host creates a surface only when it first becomes active. Warm
        // all three through the real tab notification before retaining views.
        CHECK(factories[0].creates==1&&!factories[1].creates&&!factories[2].creates);
        Select(L"Panel B");Refresh();Select(L"Panel C");Refresh();Select(L"Panel A");Refresh();
        CHECK(Selected()==L"Panel A"&&registry.GeometryReady());
        for(size_t i=0;i<3;++i){CHECK(factories[i].creates==1);initialOrder[i]=registry.elements.at(ids[i]).effectiveHints.order;}
    }
    ~Fixture(){native.reset();registry.Retire(owner);if(host)DestroyWindow(host);}
    void Register(size_t i){MmsUiElementV1 e{};e.h=Header<MmsUiElementV1>();e.id=S(ids[i]);e.ownerPlugin=Text(owner.id);
        e.parentId=S("mms.workspace.tabs.extensions");e.surfaceId=S(surfaceIds[i]);e.label=S(labels[i]);
        e.kind=MMS_UI_PANEL;e.capabilities=MMS_UI_HIDE|MMS_UI_RESIZE|MMS_UI_LAYOUT;
        CHECK(ui.registerElement(ui.context,&e,&panel[i])==MMS_OK);}
    void Refresh(){native->Refresh();native->Layout(0,420,300,true);}
    std::wstring Label(int index){wchar_t text[80]{};TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=text;item.cchTextMax=80;
        CHECK(SendMessageW(tabs,TCM_GETITEMW,index,reinterpret_cast<LPARAM>(&item)));return text;}
    std::wstring Selected(){return Label(TabCtrl_GetCurSel(tabs));}
    void Select(const wchar_t* label){int index=-1;for(int i=0;i<TabCtrl_GetItemCount(tabs);++i)if(Label(i)==label)index=i;
        CHECK(index>=0);if(index<0)return;TabCtrl_SetCurSel(tabs,index);
        NMHDR notification{tabs,static_cast<UINT_PTR>(GetDlgCtrlID(tabs)),TCN_SELCHANGE};
        SendMessageW(GetParent(tabs),WM_NOTIFY,notification.idFrom,reinterpret_cast<LPARAM>(&notification));}
    UiRegistry::SurfacePresentation View(size_t i){UiRegistry::SurfacePresentation v;CHECK(registry.ExistingSurface(ids[i],v));return v;}
    auto Views(){return std::array<UiRegistry::SurfacePresentation,3>{View(0),View(1),View(2)};}
    void Identity(const std::array<UiRegistry::SurfacePresentation,3>& old){for(size_t i=0;i<3;++i){auto now=View(i);
        CHECK(now.window==old[i].window&&now.instance==old[i].instance&&factories[i].creates==1&&!factories[i].destroys);
        wchar_t text[80]{};GetWindowTextW(factories[i].edit,text,80);CHECK(std::wstring(text)==L"Unsubmitted panel draft");
        CHECK(SendMessageW(factories[i].edit,EM_GETMODIFY,0,0));}CHECK(!IsWindowVisible(host));}
    unsigned Focuses()const{unsigned n=0;for(const auto& f:factories)n+=f.focuses;return n;}
    MmsUiLayoutHistoryInfoV1 History(){MmsUiLayoutHistoryInfoV1 h{};h.h=Header<MmsUiLayoutHistoryInfoV1>();CHECK(history.queryHistory(history.context,&h)==MMS_OK);return h;}
    MmsHandle Begin(std::initializer_list<MmsUiLayoutSessionTargetV1> values){MmsHandle h=0;CHECK(api.begin(api.context,Span(values.begin(),values.size()),&h)==MMS_OK);return h;}
    MmsUiLayoutSessionInfoV1 Info(MmsHandle h){MmsUiLayoutSessionInfoV1 info{};info.h=Header<MmsUiLayoutSessionInfoV1>();CHECK(api.query(api.context,h,&info)==MMS_OK);return info;}
    void Apply(MmsHandle h,std::initializer_list<MmsUiLayoutSessionValueV1> values){CHECK(api.apply(api.context,h,Info(h).revision,Span(values.begin(),values.size()))==MMS_OK);}
    MmsUiLayoutSessionTargetV1 Claim(MmsHandle h,uint32_t index){auto c=Target(0,0);CHECK(api.queryClaim(api.context,h,Info(h).revision,index,&c)==MMS_OK);return c;}
    void Rollback(MmsHandle h){CHECK(api.rollback(api.context,h,Info(h).revision)==MMS_OK);CHECK(api.release(api.context,h)==MMS_OK);Refresh();}
    void Persist(size_t index,bool visible){MmsHandle tx=0;CHECK(ui.beginLayout(ui.context,registry.Revision(),&tx)==MMS_OK);
        MmsLayoutOpV1 op{};op.h=Header<MmsLayoutOpV1>();op.elementId=S(ids[index]);op.expectedGeneration=registry.elements.at(ids[index]).desc.generation;
        op.operation=MMS_LAYOUT_VISIBLE;op.value.visible=visible;CHECK(ui.applyLayout(ui.context,tx,&op)==MMS_OK);CHECK(ui.commitLayout(ui.context,tx)==MMS_OK);Refresh();}
};
void CapabilityAndCombination(){
    Fixture f;auto old=f.Views();const auto history=f.History();const auto focus=f.Focuses();
    MmsUiLayoutSessionCapsV1 caps{};caps.h=Header<MmsUiLayoutSessionCapsV1>();
    CHECK(f.api.h.major==1&&f.api.h.minor==1);CHECK(f.api.queryCapabilities(f.api.context,&caps)==MMS_OK&&(caps.mounts&16u));
    auto legacy=f.registry.LayoutSessionApi(f.owner);MmsUiLayoutSessionTargetInfoV1 target{};target.h=Header<MmsUiLayoutSessionTargetInfoV1>();
    CHECK(legacy.queryTarget(legacy.context,f.panel[0],&target)==MMS_E_UNSUPPORTED);
    CHECK(f.api.queryTarget(f.api.context,f.panel[0],&target)==MMS_OK&&target.mount==16&&target.fields==7);
    const auto h=f.Begin({Target(f.panel[0],7),Target(f.panel[1],4)});
    const auto lastOrder=*std::max_element(f.initialOrder.begin(),f.initialOrder.end())+100;
    f.Apply(h,{Value(f.panel[0],7,true,620,240,lastOrder),Value(f.panel[1],4,false,0,0,-10)});f.Refresh();
    CHECK(f.Label(0)==L"Panel B"&&f.Label(1)==L"Panel C"&&f.Label(2)==L"Panel A"&&f.Selected()==L"Panel A");
    CHECK(f.View(0).bounds.width>=620&&f.View(0).bounds.height>=240);f.Identity(old);
    CHECK(f.registry.elements.at(f.ids[0]).width==0&&f.registry.elements.at(f.ids[0]).effectiveHints.order==f.initialOrder[0]);
    CHECK(f.History().undoDepth==history.undoDepth&&f.History().preferencesRevision==history.preferencesRevision);
    f.Rollback(h);CHECK(f.Selected()==L"Panel A"&&f.Label(0)==L"Panel A");f.Identity(old);CHECK(f.Focuses()==focus);
    CHECK(f.History().undoDepth==history.undoDepth&&f.History().preferencesRevision==history.preferencesRevision);
}
void HiddenActiveRestores(){
    Fixture f;const auto old=f.Views();const auto focus=f.Focuses();const auto history=f.History();
    const auto h=f.Begin({Target(f.panel[0],1)});f.Apply(h,{Value(f.panel[0],1,false)});f.Refresh();
    CHECK(f.Selected()==L"Panel B"&&!f.factories[0].Visible()&&f.factories[1].Visible()&&TabCtrl_GetItemCount(f.tabs)==2);f.Identity(old);
    f.Rollback(h);CHECK(f.Selected()==L"Panel A"&&f.factories[0].Visible()&&!f.factories[1].Visible());f.Identity(old);
    CHECK(f.Focuses()==focus&&f.History().undoDepth==history.undoDepth&&f.History().preferencesRevision==history.preferencesRevision);
}
void UserSelectionTakesOnlyWorkspace(){
    Fixture f;const auto old=f.Views();const auto history=f.History();
    const auto h=f.Begin({Target(f.panel[0],1),Target(f.panel[2],4),Target(f.toolbar,1)});
    f.Apply(h,{Value(f.panel[0],1,false),Value(f.panel[2],4,false,0,0,-30),Value(f.toolbar,1,false)});f.Refresh();
    CHECK(f.Label(0)==L"Panel C"&&TabCtrl_GetItemCount(f.tabs)==2);
    const auto cFocusBefore=f.factories[2].focuses;
    f.Select(L"Panel C");f.Refresh();
    CHECK(f.Selected()==L"Panel C"&&f.Label(0)==L"Panel A"&&f.Label(2)==L"Panel C");
    CHECK(!f.Claim(h,0).fields&&!f.Claim(h,1).fields&&f.Claim(h,2).fields==1);
    CHECK(f.Info(h).state==MMS_LAYOUT_SESSION_ACTIVE&&(f.Info(h).reasons&MMS_LAYOUT_SESSION_HOST_INTERACTION));
    CHECK(!f.registry.EffectiveLayout(f.registry.elements.at("example.panelpreview.toolbar")).visible);
    const auto focus=f.Focuses();CHECK(f.factories[2].focuses==cFocusBefore+1);f.Rollback(h);
    CHECK(f.Selected()==L"Panel C"&&f.factories[2].Visible()&&!f.factories[0].Visible()&&f.Focuses()==focus);f.Identity(old);
    CHECK(f.History().undoDepth==history.undoDepth&&f.History().preferencesRevision==history.preferencesRevision);
}
void PersistentAndReplacement(){
    {
        Fixture f;const auto old=f.Views();const auto focus=f.Focuses();
        const auto h=f.Begin({Target(f.panel[0],6)});f.Apply(h,{Value(f.panel[0],6,false,610,230,15)});f.Refresh();
        CHECK(f.History().undoDepth==0);CHECK(f.api.commit(f.api.context,h,f.Info(h).revision)==MMS_OK);
        CHECK(f.Info(h).state==MMS_LAYOUT_SESSION_COMMITTED&&f.History().undoDepth==1);
        CHECK(f.api.release(f.api.context,h)==MMS_OK);f.Refresh();
        CHECK(f.registry.elements.at(f.ids[0]).width==610&&f.registry.elements.at(f.ids[0]).effectiveHints.order==15&&f.Selected()==L"Panel A");
        CHECK(f.history.undo(f.history.context)==MMS_OK);f.Refresh();
        CHECK(f.registry.elements.at(f.ids[0]).width==0&&f.registry.elements.at(f.ids[0]).effectiveHints.order==f.initialOrder[0]&&f.Label(0)==L"Panel A"&&f.Selected()==L"Panel A");
        f.Identity(old);CHECK(f.Focuses()==focus);
    }
    {
        Fixture f;const auto h=f.Begin({Target(f.panel[0],1)});f.Apply(h,{Value(f.panel[0],1,false)});f.Refresh();
        f.Persist(0,false);CHECK(f.Info(h).state==MMS_LAYOUT_SESSION_REVOKED&&(f.Info(h).reasons&MMS_LAYOUT_SESSION_PERSISTENT_EDIT));
        CHECK(f.api.release(f.api.context,h)==MMS_OK);f.Refresh();CHECK(f.Selected()==L"Panel B"&&!f.factories[0].Visible());
        CHECK(f.History().undoDepth==1);
    }
    {
        Fixture f;const auto h=f.Begin({Target(f.panel[0],1)});f.Apply(h,{Value(f.panel[0],1,false)});f.Refresh();
        const auto retired=f.panel[0];CHECK(f.ui.unregisterItem(f.ui.context,retired)==MMS_OK);f.Refresh();f.Register(0);f.Refresh();
        UiRegistry::SurfacePresentation absent;
        CHECK(f.panel[0]!=retired&&!f.registry.ExistingSurface(f.ids[0],absent)&&f.factories[0].creates==1&&f.factories[0].destroys==1);
        CHECK(f.Info(h).state==MMS_LAYOUT_SESSION_REVOKED&&(f.Info(h).reasons&MMS_LAYOUT_SESSION_TARGET_CHANGED));
        CHECK(f.api.release(f.api.context,h)==MMS_OK);f.Refresh();CHECK(f.Selected()==L"Panel B"&&!f.factories[0].Visible());
        // Factory context and HWND addresses can legally be reused. Verify
        // registration retirement and actual destroy/create, not pointer inequality.
        f.Select(L"Panel A");f.Refresh();CHECK(f.registry.ExistingSurface(f.ids[0],absent));
        CHECK(f.factories[0].creates==2&&f.factories[0].destroys==1&&f.factories[0].Visible()&&f.Selected()==L"Panel A");
    }
}
void SamePageAndTemporaryShowIntent(){
    {
        Fixture f;const auto old=f.Views();const auto history=f.History();
        const auto h=f.Begin({Target(f.panel[0],1)});f.Apply(h,{Value(f.panel[0],1,false)});f.Refresh();
        CHECK(f.Selected()==L"Panel B");f.Select(L"Panel B");f.Refresh();
        CHECK(f.Info(h).state==MMS_LAYOUT_SESSION_REVOKED&&(f.Info(h).reasons&MMS_LAYOUT_SESSION_HOST_INTERACTION));
        CHECK(f.Selected()==L"Panel B"&&TabCtrl_GetItemCount(f.tabs)==3&&!f.factories[0].Visible());
        const auto focus=f.Focuses();CHECK(f.api.release(f.api.context,h)==MMS_OK);f.Refresh();
        CHECK(f.Selected()==L"Panel B"&&f.Focuses()==focus);f.Identity(old);
        CHECK(f.History().undoDepth==history.undoDepth&&f.History().preferencesRevision==history.preferencesRevision);
    }
    {
        Fixture f;f.Persist(0,false);const auto old=f.Views();const auto history=f.History();
        CHECK(f.Selected()==L"Panel B"&&TabCtrl_GetItemCount(f.tabs)==2);
        const auto h=f.Begin({Target(f.panel[0],1)});f.Apply(h,{Value(f.panel[0],1,true)});f.Refresh();
        CHECK(f.Selected()==L"Panel B"&&TabCtrl_GetItemCount(f.tabs)==3);
        const auto focus=f.Focuses();f.Select(L"Panel A");f.Refresh();
        // Taking over visibility removes a temporary-only tab. Do not focus
        // or reactivate that persistently hidden page after its removal.
        CHECK(f.Info(h).state==MMS_LAYOUT_SESSION_REVOKED&&(f.Info(h).reasons&MMS_LAYOUT_SESSION_HOST_INTERACTION));
        CHECK(f.Selected()==L"Panel B"&&TabCtrl_GetItemCount(f.tabs)==2&&!f.factories[0].Visible()&&f.Focuses()==focus);
        CHECK(f.api.release(f.api.context,h)==MMS_OK);f.Refresh();CHECK(f.Selected()==L"Panel B");f.Identity(old);
        CHECK(f.History().undoDepth==history.undoDepth&&f.History().preferencesRevision==history.preferencesRevision);
    }
}
void StopAndResizeFailure(){
    {
        Fixture f;const auto h=f.Begin({Target(f.panel[0],1)});f.Apply(h,{Value(f.panel[0],1,false)});f.Refresh();
        const auto focus=f.Focuses();f.owner.stopping=true;f.registry.RevokeLayoutPreviews(f.owner);f.Refresh();
        CHECK(f.Info(h).state==MMS_LAYOUT_SESSION_REVOKED&&(f.Info(h).reasons&MMS_LAYOUT_SESSION_STOPPING));
        CHECK(f.registry.EffectiveLayout(f.registry.elements.at(f.ids[0])).visible&&f.Focuses()==focus&&f.Selected()==L"Panel B");
        const auto t=Target(f.panel[0],1);MmsHandle untouched=99;CHECK(f.api.begin(f.api.context,Span(&t,1),&untouched)==MMS_E_NOT_READY&&untouched==99);
        CHECK(f.api.release(f.api.context,h)==MMS_OK);f.Refresh();CHECK(f.Focuses()==focus);
    }
    {
        Fixture f;const auto old=f.Views();const auto focus=f.Focuses();const auto history=f.History();
        const auto h=f.Begin({Target(f.panel[0],3)});f.factories[0].failResize=true;
        f.Apply(h,{Value(f.panel[0],3,false,650,250)});f.Refresh();
        CHECK(!f.registry.GeometryReady()&&!f.factories[0].Visible());f.Identity(old);CHECK(f.Focuses()==focus);
        CHECK(f.api.rollback(f.api.context,h,f.Info(h).revision)==MMS_OK);CHECK(f.api.release(f.api.context,h)==MMS_OK);
        f.factories[0].failResize=false;f.Refresh();CHECK(f.registry.GeometryReady()&&f.Selected()==L"Panel A"&&f.factories[0].Visible());
        f.Identity(old);CHECK(f.Focuses()==focus&&f.History().undoDepth==history.undoDepth&&f.History().preferencesRevision==history.preferencesRevision);
    }
}
}
int main(){
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_TAB_CLASSES|ICC_BAR_CLASSES};if(!InitCommonControlsEx(&controls))return 2;
    try{CapabilityAndCombination();HiddenActiveRestores();UserSelectionTakesOnlyWorkspace();PersistentAndReplacement();SamePageAndTemporaryShowIntent();StopAndResizeFailure();}
    catch(const std::exception& error){std::fprintf(stderr,"exception: %s\n",error.what());CHECK(false);}catch(...){CHECK(false);}
    std::printf("layout_session_panel_tests: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
