// Real hidden native hosts and registered surface factories. PANEL is not
// enabled in the layout-session API by these prerequisite failure tests.
#include "../../src/plugins/native_ui.hpp"
#include <commctrl.h>
#include <cstdio>
#include <stdexcept>

using namespace mikumikustudio::plugins;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)){++failures;std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);} } while(false)
MmsString S(const char* text){return {text,std::strlen(text)};}
enum class Fault {None,ThrowBefore,ThrowAfter,Ignore,Reveal};
struct Factory {
    HWND child=nullptr,edit=nullptr;
    unsigned creates=0,destroys=0,resizes=0,shows=0;
    bool failCreate=false;
    MmsResult createError=MMS_E_FAILED;
    Fault resize=Fault::None,hide=Fault::None,show=Fault::None;
    std::string calls;
    MmsUiSurfaceDescV1 Descriptor(){
        MmsUiSurfaceDescV1 desc{};desc.h=Header<MmsUiSurfaceDescV1>();desc.id=S("example.surfacefailure.factory");desc.context=this;
        desc.minimumWidth=20;desc.minimumHeight=20;desc.preferredWidth=160;desc.preferredHeight=80;
        desc.createView=[](void* p,const MmsUiMountV1* mount,void** out,uintptr_t* native)->MmsResult{
            auto& f=*static_cast<Factory*>(p);++f.creates;*out=nullptr;*native=0;
            if(f.failCreate)return f.createError;
            f.child=CreateWindowExW(WS_EX_CONTROLPARENT,L"STATIC",L"Failure recovery surface",WS_CHILD,
                mount->bounds.x,mount->bounds.y,mount->bounds.width,mount->bounds.height,
                reinterpret_cast<HWND>(mount->parentWindow),nullptr,GetModuleHandleW(nullptr),nullptr);
            if(!f.child)return MMS_E_FAILED;
            f.edit=CreateWindowExW(0,L"EDIT",L"Unsubmitted surface text",WS_CHILD|WS_VISIBLE,
                2,2,130,22,f.child,nullptr,GetModuleHandleW(nullptr),nullptr);
            if(!f.edit){DestroyWindow(f.child);f.child=nullptr;return MMS_E_FAILED;}
            *out=&f;*native=reinterpret_cast<uintptr_t>(f.child);return MMS_OK;
        };
        desc.resizeView=[](void* p,const MmsUiRectV1* rect,uint32_t){
            auto& f=*static_cast<Factory*>(p);++f.resizes;f.calls+='R';
            if(f.resize==Fault::ThrowBefore)throw std::runtime_error("resize before mutation");
            if(f.resize!=Fault::Ignore)CHECK(MoveWindow(f.child,rect->x,rect->y,rect->width,rect->height,FALSE));
            if(f.resize==Fault::Reveal)ShowWindow(f.child,SW_SHOWNA);
            if(f.resize==Fault::ThrowAfter)throw std::runtime_error("resize after mutation");
        };
        desc.showView=[](void* p,uint32_t visible){
            auto& f=*static_cast<Factory*>(p);++f.shows;f.calls+=visible?'S':'H';
            const auto mode=visible?f.show:f.hide;
            if(mode==Fault::ThrowBefore)throw std::runtime_error("visibility before mutation");
            if(mode!=Fault::Ignore)ShowWindow(f.child,visible?SW_SHOWNA:SW_HIDE);
            if(mode==Fault::ThrowAfter)throw std::runtime_error("visibility after mutation");
        };
        desc.focusView=[](void*)->MmsResult{return MMS_OK;};
        desc.destroyView=[](void* p){auto& f=*static_cast<Factory*>(p);++f.destroys;DestroyWindow(f.child);f.child=f.edit=nullptr;};
        return desc;
    }
    bool Visible()const{return child&&(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;}
};
struct Fixture {
    UiRegistry registry;
    UiClient owner{&registry,"example.surfacefailure"};
    UiRefreshQueue& queue=registry.Refresh();RefreshClient refreshClient{&queue,owner.id};
    MmsUiApiV1 ui=registry.Api(owner);MmsUiSurfaceApiV1 surfaces=registry.SurfaceApi(owner);
    MmsUiRefreshApiV1 refresh=queue.Api(refreshClient);
    Factory factory;
    HWND host=nullptr;
    MmsHandle surface=0,panel=0;
    std::unique_ptr<NativeUi> native;
    explicit Fixture(bool mount=true,bool failCreate=false){
        host=CreateWindowExW(0,L"STATIC",L"MMS surface contract hidden host",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
            0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);CHECK(host!=nullptr);
        auto desc=factory.Descriptor();factory.failCreate=failCreate;
        CHECK(surfaces.registerSurface(surfaces.context,&desc,&surface)==MMS_OK);
        MmsUiElementV1 element{};element.h=Header<MmsUiElementV1>();element.id=S("example.surfacefailure.panel");
        element.ownerPlugin=Text(owner.id);element.parentId=S("mms.workspace.tabs.extensions");element.label=S("Surface recovery");
        element.surfaceId=desc.id;element.kind=MMS_UI_PANEL;element.capabilities=MMS_UI_HIDE|MMS_UI_RESIZE|MMS_UI_LAYOUT;
        CHECK(ui.registerElement(ui.context,&element,&panel)==MMS_OK);
        if(mount){native=std::make_unique<NativeUi>(registry,host,Log{});native->Refresh();native->Layout(0,400,260,true);}
    }
    ~Fixture(){native.reset();queue.Retire(refreshClient);registry.Retire(owner);if(host)DestroyWindow(host);}
    UiRegistry::SurfacePresentation View(){UiRegistry::SurfacePresentation view;CHECK(registry.ExistingSurface("example.surfacefailure.panel",view));return view;}
    void Identity(const UiRegistry::SurfacePresentation& before){const auto now=View();CHECK(now.window==before.window&&now.instance==before.instance&&factory.creates==1&&!factory.destroys);
        wchar_t text[80]{};GetWindowTextW(factory.edit,text,80);CHECK(std::wstring(text)==L"Unsubmitted surface text");CHECK(!IsWindowVisible(host));}
    void Ready(bool expected){CHECK(registry.GeometryReady()==expected);MmsUiRefreshTargetV1 target{};target.h=Header<MmsUiRefreshTargetV1>();
        const auto result=refresh.getTarget(refresh.context,panel,&target);
        if(!expected){
            // Direct ShowSurface has no NativeUi publication. A mounted view
            // retains its published identity, but is suspended, not removed.
            if(!native)CHECK(result==MMS_E_NOT_READY);
            else CHECK(result==MMS_OK&&!target.ready);
        }}
    void Persist(bool visible){MmsHandle tx=0;CHECK(ui.beginLayout(ui.context,registry.Revision(),&tx)==MMS_OK);
        MmsLayoutOpV1 op{};op.h=Header<MmsLayoutOpV1>();op.elementId=S("example.surfacefailure.panel");
        op.expectedGeneration=registry.elements.at("example.surfacefailure.panel").desc.generation;op.operation=MMS_LAYOUT_VISIBLE;op.value.visible=visible;
        CHECK(ui.applyLayout(ui.context,tx,&op)==MMS_OK&&ui.commitLayout(ui.context,tx)==MMS_OK);}
    void Size(float width,float height){MmsHandle tx=0;CHECK(ui.beginLayout(ui.context,registry.Revision(),&tx)==MMS_OK);
        MmsLayoutOpV1 op{};op.h=Header<MmsLayoutOpV1>();op.elementId=S("example.surfacefailure.panel");
        op.expectedGeneration=registry.elements.at("example.surfacefailure.panel").desc.generation;op.operation=MMS_LAYOUT_SIZE;op.value.size={width,height};
        CHECK(ui.applyLayout(ui.context,tx,&op)==MMS_OK&&ui.commitLayout(ui.context,tx)==MMS_OK);}
    void Refresh(){native->Refresh();}
    void Layout(){native->Layout(0,400,260,true);}
    void Recover(){factory.resize=factory.hide=factory.show=Fault::None;Refresh();Layout();Ready(true);}
};
void HiddenResizeOrdering(){
    Fixture f(false);MmsUiRectV1 initial{0,0,160,80},changed{2,3,240,120};
    CHECK(f.registry.ShowSurface("example.surfacefailure.panel",f.host,initial,96,true)==MMS_OK);
    const auto identity=f.View();f.factory.calls.clear();f.factory.resize=Fault::ThrowBefore;
    CHECK(f.registry.ShowSurface("example.surfacefailure.panel",f.host,changed,144,false)==MMS_E_FAILED);
    CHECK(f.factory.calls=="HR"&&!f.factory.Visible()&&!f.View().visible);f.Identity(identity);f.Ready(false);
    f.factory.resize=Fault::None;
    CHECK(f.registry.ShowSurface("example.surfacefailure.panel",f.host,changed,144,false)==MMS_OK);
    CHECK(!f.factory.Visible()&&f.View().dpi==144&&f.View().bounds.width==240);f.Identity(identity);
    changed.width=260;f.factory.resize=Fault::Reveal;
    CHECK(f.registry.ShowSurface("example.surfacefailure.panel",f.host,changed,144,false)==MMS_E_FAILED);
    CHECK(f.factory.Visible());f.Ready(false); // A violating callback is reported, never silently treated as hidden.
    f.factory.resize=Fault::None;
    CHECK(f.registry.ShowSurface("example.surfacefailure.panel",f.host,changed,144,false)==MMS_OK&&!f.factory.Visible());
    CHECK(f.registry.ShowSurface("example.surfacefailure.panel",f.host,changed,144,true)==MMS_OK&&f.factory.Visible());f.Identity(identity);
}
void NativeVisibilityFailures(){
    for(const auto mode:{Fault::ThrowBefore,Fault::ThrowAfter,Fault::Ignore}){
        Fixture f;const auto identity=f.View();f.Ready(true);CHECK(f.factory.Visible());
        f.factory.hide=mode;f.Persist(false);f.Refresh();f.Ready(false);
        CHECK(f.factory.Visible()==(mode!=Fault::ThrowAfter));f.Identity(identity);
        f.Layout();f.Ready(false);f.Identity(identity);
        f.Recover();CHECK(!f.factory.Visible()&&!f.View().visible);f.Identity(identity);
        f.factory.show=mode;f.Persist(true);f.Refresh();f.Ready(false);
        CHECK(f.factory.Visible()==(mode==Fault::ThrowAfter));f.Identity(identity);
        f.Layout();f.Ready(false);f.Recover();CHECK(f.factory.Visible()&&f.View().visible);f.Identity(identity);
    }
}
void NativeResizeFailures(){
    for(const auto mode:{Fault::ThrowBefore,Fault::ThrowAfter,Fault::Ignore}){
        Fixture f;const auto identity=f.View();f.factory.resize=mode;f.Size(550,140);
        f.Refresh();f.Ready(false);f.Identity(identity);
        f.Layout();f.Ready(false);f.Identity(identity);
        f.Recover();CHECK(f.View().bounds.width>=550&&f.factory.Visible());f.Identity(identity);
    }
}
void InitialFailurePlaceholder(){
    Fixture f(true,true);CHECK(f.factory.creates>0&&!f.factory.child&&!f.factory.destroys);
    // The established first-create error placeholder remains a valid ordinary
    // workspace layout. It does not claim a successful surface presentation.
    CHECK(f.registry.GeometryReady());
    f.factory.createError=MMS_E_STALE;f.Refresh();f.Layout();
    CHECK(f.registry.GeometryReady());UiRegistry::SurfacePresentation absent;
    CHECK(!f.registry.ExistingSurface("example.surfacefailure.panel",absent));
    MmsUiRefreshTargetV1 target{};target.h=Header<MmsUiRefreshTargetV1>();
    CHECK(f.refresh.getTarget(f.refresh.context,f.panel,&target)!=MMS_OK);
    f.factory.failCreate=false;f.Refresh();f.Layout();CHECK(f.registry.GeometryReady()&&f.factory.Visible());
    const auto creates=f.factory.creates;const auto view=f.View();f.Refresh();f.Layout();
    CHECK(f.factory.creates==creates&&f.View().instance==view.instance&&!f.factory.destroys);
}
}
int main(){
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_TAB_CLASSES|ICC_BAR_CLASSES};
    if(!InitCommonControlsEx(&controls))return 2;
    try{HiddenResizeOrdering();NativeVisibilityFailures();NativeResizeFailures();InitialFailurePlaceholder();}
    catch(...){CHECK(false);}
    std::printf("layout_session_surface_tests: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
