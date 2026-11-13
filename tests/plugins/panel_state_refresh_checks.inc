// Include after manager_refresh_checks.inc. Uses the real registered surface,
// NativeUi tab control and original refresh/provider services.
#include <mms/refresh_state.h>
#include <cstdio>
namespace panel_state_refresh_checks {
struct Fixture {
    manager_refresh_checks::Window window;
    UiRegistry registry;
    UiClient owner{&registry,"example.panelstate"};
    UiRefreshQueue& queue=registry.Refresh();
    RefreshClient client{&queue,owner.id};
    MmsUiApiV1 ui=registry.Api(owner);
    MmsUiRefreshApiV1 refresh=queue.Api(client);
    MmsUiRefreshStateContentApiV1 content=registry.StateContentApi(owner);
    struct View {
        Fixture* f; unsigned index; HWND window=nullptr,edit=nullptr;
        unsigned creates=0,destroys=0,shows=0,resizes=0,buttonMessages=0,throwShowAfter=0,throwResizeAfter=0;
        bool throwRestoreBefore=false;
    } views[2]{{this,0},{this,1}};
    std::array<MmsHandle,2> panels{},surfaces{},providers{};
    std::array<std::string,2> ids{{owner.id+".a",owner.id+".b"}};
    MmsHandle command=0;
    MmsCommandDescV1 commandDesc{};
    std::unique_ptr<NativeUi> native;
    HWND workspace=nullptr,tabs=nullptr;
    unsigned stateReads=0,prepares=0;
    uint32_t enabled=1,visible=1,checked=0;
    std::string label="Updated panel",description="Panel description",reason="Panel unavailable";
    std::function<MmsResult(MmsHandle)> onPrepare;
    static LRESULT CALLBACK Observe(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR p) {
        auto& v=*reinterpret_cast<View*>(p);
        if(m==BM_GETCHECK||m==BM_SETCHECK)++v.buttonMessages;
        if(m==WM_NCDESTROY)RemoveWindowSubclass(h,Observe,id);
        return DefSubclassProc(h,m,w,l);
    }
    explicit Fixture(bool mountBoth=true) {
        commandDesc.info.h=Header<MmsCommandDescV1>();commandDesc.info.id=S("example.panelstate.command");
        commandDesc.info.ownerPlugin=Text(owner.id);commandDesc.info.label=S("Panel command");commandDesc.info.description=S("Registered description");
        commandDesc.context=this;
        commandDesc.getState=[](void* p,MmsObjectRef,MmsCommandStateV1* out)->MmsResult {
            auto& x=*static_cast<Fixture*>(p);++x.stateReads;
            *out={Header<MmsCommandStateV1>(),x.enabled,x.checked,x.visible,0,Text(x.reason)};return MMS_OK;
        };
        commandDesc.invoke=[](void*,MmsObjectRef,MmsSpan,MmsHandle)->MmsResult{return MMS_OK;};
        CHECK(ui.registerCommand(ui.context,&commandDesc,&command)==MMS_OK);
        auto api=registry.SurfaceApi(owner);
        for(unsigned i=0;i<2;++i) {
            const auto surfaceId=ids[i]+".surface";
            MmsUiSurfaceDescV1 s{};s.h=Header<MmsUiSurfaceDescV1>();s.id=Text(surfaceId);s.context=&views[i];
            s.minimumWidth=40;s.minimumHeight=24;s.preferredWidth=160;s.preferredHeight=80;
            s.createView=[](void* p,const MmsUiMountV1* mount,void** context,uintptr_t* child)->MmsResult {
                auto& v=*static_cast<View*>(p);++v.creates;
                v.window=CreateWindowExW(WS_EX_CONTROLPARENT,L"STATIC",L"Plugin-owned view",WS_CHILD,
                    mount->bounds.x,mount->bounds.y,mount->bounds.width,mount->bounds.height,
                    reinterpret_cast<HWND>(mount->parentWindow),nullptr,GetModuleHandleW(nullptr),nullptr);
                if(!v.window)return MMS_E_FAILED;
                v.edit=CreateWindowExW(0,L"EDIT",L"Unsubmitted plugin text",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                    2,2,120,24,v.window,nullptr,GetModuleHandleW(nullptr),nullptr);
                if(!v.edit){DestroyWindow(v.window);v.window=nullptr;return MMS_E_FAILED;}
                CHECK(SetWindowSubclass(v.window,Observe,0x50535441,reinterpret_cast<DWORD_PTR>(&v))!=FALSE);
                *context=&v;*child=reinterpret_cast<uintptr_t>(v.window);return MMS_OK;
            };
            s.resizeView=[](void* p,const MmsUiRectV1* r,uint32_t){
                auto& v=*static_cast<View*>(p);++v.resizes;CHECK(MoveWindow(v.window,r->x,r->y,r->width,r->height,FALSE));
                if(v.throwResizeAfter){--v.throwResizeAfter;throw std::runtime_error("surface moved before failure");}
            };
            s.showView=[](void* p,uint32_t show){
                auto& v=*static_cast<View*>(p);++v.shows;
                if(show&&v.throwRestoreBefore){v.throwRestoreBefore=false;throw std::runtime_error("surface restore refused");}
                ShowWindow(v.window,show?SW_SHOWNA:SW_HIDE);
                if(v.throwShowAfter){--v.throwShowAfter;throw std::runtime_error("surface shown before failure");}
            };
            s.focusView=[](void* p)->MmsResult{SetFocus(static_cast<View*>(p)->edit);return MMS_OK;};
            s.destroyView=[](void* p){auto& v=*static_cast<View*>(p);++v.destroys;DestroyWindow(v.window);v.window=v.edit=nullptr;};
            CHECK(api.registerSurface(api.context,&s,&surfaces[i])==MMS_OK);
            MmsUiElementV1 e{};e.h=Header<MmsUiElementV1>();e.id=Text(ids[i]);e.ownerPlugin=Text(owner.id);
            e.parentId=S("mms.workspace.tabs.extensions");e.label=i?S("Panel B"):S("Panel A");e.surfaceId=Text(surfaceId);
            e.kind=MMS_UI_PANEL;e.capabilities=MMS_UI_LAYOUT|MMS_UI_HIDE|MMS_UI_RESIZE;
            if(!i)e.commandId=commandDesc.info.id;
            CHECK(ui.registerElement(ui.context,&e,&panels[i])==MMS_OK);
        }
        native=std::make_unique<NativeUi>(registry,window.value,Log{});native->Refresh();CHECK(native->Layout(0,400,300,true)>0);
        workspace=FindWindowExW(window.value,nullptr,L"MMS.PluginAbi.Workspace.V1",nullptr);
        tabs=FindWindowExW(workspace,nullptr,WC_TABCONTROLW,nullptr);CHECK(workspace&&tabs&&views[0].window);
        if(mountBoth){Select(1);Select(0);CHECK(views[1].window);}
        onPrepare=[this](MmsHandle builder){
            MmsUiStateContentV1 c{Header<MmsUiStateContentV1>(),7,0,Text(label),Text(description),Text(reason)};
            return content.writePrepared(content.context,builder,&c);
        };
    }
    ~Fixture(){native.reset();queue.Retire(client);registry.Retire(owner);}
    void Select(int row){TabCtrl_SetCurSel(tabs,row);NMHDR n{tabs,0,TCN_SELCHANGE};SendMessageW(workspace,WM_NOTIFY,0,reinterpret_cast<LPARAM>(&n));}
    MmsUiRefreshTargetV1 Target(unsigned i=0){MmsUiRefreshTargetV1 t{};t.h=Header<MmsUiRefreshTargetV1>();CHECK(refresh.getTarget(refresh.context,panels[i],&t)==MMS_OK);return t;}
    MmsUiStateContentDescV1 Desc(unsigned i=0){
        MmsUiStateContentDescV1 d{};d.h=Header<MmsUiStateContentDescV1>();d.target=Target(i).target;d.context=this;d.fields=7;
        d.prepare=[](void* p,const MmsUiTargetRefV1* target,MmsHandle builder)->MmsResult{
            auto& x=*static_cast<Fixture*>(p);++x.prepares;
            CHECK(x.ui.unregisterItem(x.ui.context,x.panels[1])==MMS_E_BUSY);
            CHECK(x.queue.ReadOnlyCause(x.client));
            MmsUiRefreshRequestV1 nested{};nested.h=Header<MmsUiRefreshRequestV1>();nested.target=*target;
            nested.scope=MMS_UI_REFRESH_SELF;nested.kinds=MMS_UI_REFRESH_STATE;MmsHandle refused=0;
            CHECK(x.refresh.request(x.refresh.context,&nested,&refused)==MMS_E_BUSY&&!refused);
            return x.onPrepare(builder);
        };return d;
    }
    void Register(unsigned i=0){const auto d=Desc(i);CHECK(content.registerProvider(content.context,&d,&providers[i])==MMS_OK);}
    MmsHandle Submit(uint32_t kinds=MMS_UI_REFRESH_STATE,unsigned i=0,bool worker=false){
        MmsUiRefreshRequestV1 r{};r.h=Header<MmsUiRefreshRequestV1>();r.target=Target(i).target;r.kinds=kinds;r.scope=MMS_UI_REFRESH_SELF;
        MmsHandle t=0;MmsResult result=MMS_E_FAILED;
        if(worker){std::thread job([&]{result=refresh.request(refresh.context,&r,&t);});job.join();}
        else result=refresh.request(refresh.context,&r,&t);
        CHECK(result==MMS_OK&&t);return t;
    }
    MmsUiRefreshStateV1 Result(MmsHandle t){MmsUiRefreshStateV1 s{};s.h=Header<MmsUiRefreshStateV1>();CHECK(refresh.query(refresh.context,t,&s)==MMS_OK);return s;}
    void Done(MmsHandle t,uint32_t kinds=MMS_UI_REFRESH_STATE){
        const auto s=Result(t);
        if(s.status!=MMS_UI_REFRESH_COMPLETED||s.result!=MMS_OK||s.completedKinds!=kinds)
            std::fprintf(stderr,"panel refresh failed: status=%u result=%d completed=%u expected=%u visible=%u enabled=%u prepares=%u label=%s revision=%llu\n",
                unsigned(s.status),int(s.result),unsigned(s.completedKinds),unsigned(kinds),unsigned(visible),unsigned(enabled),prepares,label.c_str(),
                static_cast<unsigned long long>(registry.Revision()));
        CHECK(s.status==MMS_UI_REFRESH_COMPLETED&&s.result==MMS_OK&&s.completedKinds==kinds);CHECK(refresh.release(refresh.context,t)==MMS_OK);
    }
    std::wstring Tab(int row)const{wchar_t text[8194]{};TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=text;item.cchTextMax=8194;CHECK(SendMessageW(tabs,TCM_GETITEMW,row,reinterpret_cast<LPARAM>(&item)));return text;}
    static std::wstring TextOf(HWND w){wchar_t text[256]{};GetWindowTextW(w,text,256);return text;}
    void Identity(const std::array<UiRegistry::SurfacePresentation,2>& before){
        for(unsigned i=0;i<2;++i){UiRegistry::SurfacePresentation v;CHECK(registry.ExistingSurface(ids[i],v));CHECK(v.window==before[i].window&&v.instance==before[i].instance&&views[i].creates==1&&!views[i].destroys);CHECK(TextOf(views[i].edit)==L"Unsubmitted plugin text");CHECK(!views[i].buttonMessages);}
    }
    std::array<UiRegistry::SurfacePresentation,2> Identity(){std::array<UiRegistry::SurfacePresentation,2> a{};for(unsigned i=0;i<2;++i)CHECK(registry.ExistingSurface(ids[i],a[i]));return a;}
    std::wstring Help(){
        struct Search{HWND owner,found=nullptr;} search{window.value};
        EnumThreadWindows(GetCurrentThreadId(),[](HWND w,LPARAM p)->BOOL{auto& s=*reinterpret_cast<Search*>(p);wchar_t name[64]{};GetClassNameW(w,name,64);if(GetWindow(w,GW_OWNER)==s.owner&&!std::wcscmp(name,TOOLTIPS_CLASSW)){s.found=w;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&search));
        CHECK(search.found);wchar_t text[8194]{};TOOLINFOW t{};t.cbSize=TTTOOLINFOW_V1_SIZE;t.hwnd=workspace;t.uId=UINT_PTR(panels[0]);t.lpszText=text;
        CHECK(SendMessageW(search.found,TTM_GETTOOLINFOW,0,reinterpret_cast<LPARAM>(&t)));t.lpszText=text;
        SendMessageW(search.found,TTM_GETTEXTW,8194,reinterpret_cast<LPARAM>(&t));return text;
    }
};
static void TabSelectionThenStateLayout(){
    Fixture x;x.Register();const auto identity=x.Identity();
    x.Select(1);x.Select(0);const auto revision=x.registry.Revision();
    auto t=x.Submit(MMS_UI_REFRESH_STATE|MMS_UI_REFRESH_LAYOUT);x.queue.Drain();x.Done(t,12);
    CHECK(x.registry.Revision()==revision&&TabCtrl_GetCurSel(x.tabs)==0&&x.Tab(0)==L"Updated panel");x.Identity(identity);
    x.visible=0;t=x.Submit(MMS_UI_REFRESH_STATE|MMS_UI_REFRESH_LAYOUT);x.queue.Drain();x.Done(t,12);
    CHECK(x.registry.Revision()==revision&&TabCtrl_GetItemCount(x.tabs)==1&&x.Tab(0)==L"Panel B");x.Identity(identity);
}
static void ContentAndAvailability(){
    Fixture x;const auto identity=x.Identity();const auto target=x.Target();const auto revision=x.registry.Revision();x.Register();
    auto t=x.Submit(MMS_UI_REFRESH_STATE|MMS_UI_REFRESH_REPAINT,0,true);x.queue.Drain();x.Done(t,5);
    CHECK(x.Tab(0)==L"Updated panel"&&x.Help()==L"Panel description"&&x.registry.Revision()==revision);
    x.Identity(identity);CHECK(x.Target().target.instanceGeneration==target.target.instanceGeneration);
    {
        MmsUiRefreshRequestV1 subtree{};subtree.h=Header<MmsUiRefreshRequestV1>();subtree.target=x.Target().target;
        subtree.scope=MMS_UI_REFRESH_SUBTREE;subtree.kinds=MMS_UI_REFRESH_STATE;
        MmsHandle a=0,b=0;const auto before=x.prepares;
        CHECK(x.refresh.request(x.refresh.context,&subtree,&a)==MMS_OK&&x.refresh.request(x.refresh.context,&subtree,&b)==MMS_OK);
        x.queue.Drain();x.Done(a);x.Done(b);CHECK(x.prepares==before+1);
    }
    const auto reads=x.stateReads;auto baseline=x.Submit(MMS_UI_REFRESH_STATE,1);x.queue.Drain();x.Done(baseline);
    CHECK(x.stateReads==reads&&x.Tab(1)==L"Panel B"&&IsWindowEnabled(x.views[1].window));
    x.Register(1);baseline=x.Submit(MMS_UI_REFRESH_STATE,1);x.queue.Drain();x.Done(baseline);
    CHECK(x.stateReads==reads&&x.Tab(1)==L"Updated panel");
    CHECK(x.content.unregisterProvider(x.content.context,x.providers[1])==MMS_OK);x.providers[1]=0;
    baseline=x.Submit(MMS_UI_REFRESH_STATE,1);x.queue.Drain();x.Done(baseline);CHECK(x.Tab(1)==L"Panel B"&&x.stateReads==reads);
    SetFocus(x.views[0].edit);CHECK(GetFocus()==x.views[0].edit);x.enabled=0;x.checked=1;
    t=x.Submit();x.queue.Drain();x.Done(t);CHECK(!IsWindowEnabled(x.views[0].window)&&GetFocus()!=x.views[0].edit);
    CHECK(x.Help()==L"Panel description\nPanel unavailable"&&TabCtrl_GetCurSel(x.tabs)==0);x.Identity(identity);
    x.enabled=1;t=x.Submit();x.queue.Drain();x.Done(t);CHECK(IsWindowEnabled(x.views[0].window));
    x.visible=0;t=x.Submit();x.queue.Drain();CHECK(x.Result(t).result==MMS_E_UNSUPPORTED);
    CHECK(IsWindowVisible(x.views[0].window)&&TabCtrl_GetItemCount(x.tabs)==2);
    CHECK(x.refresh.release(x.refresh.context,t)==MMS_OK);
    t=x.Submit(MMS_UI_REFRESH_STATE|MMS_UI_REFRESH_LAYOUT);x.queue.Drain();x.Done(t,12);
    CHECK(TabCtrl_GetItemCount(x.tabs)==1&&x.Tab(0)==L"Panel B"&&!IsWindowVisible(x.views[0].window)&&IsWindowVisible(x.views[1].window));x.Identity(identity);
    x.visible=1;t=x.Submit(12);x.queue.Drain();x.Done(t,12);CHECK(TabCtrl_GetItemCount(x.tabs)==2&&x.Tab(0)==L"Updated panel");
    x.Select(0);x.native->Layout(0,300,240,true);x.Identity(identity);CHECK(x.Tab(0)==L"Updated panel"&&x.Help()==L"Panel description");
}
struct CaptionFault {
    bool fired=false,truncate=false;
    Fixture* expireAfterWrite=nullptr;
    static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR p){
        auto& f=*reinterpret_cast<CaptionFault*>(p);
        if(m==TCM_SETITEMW&&!f.fired){
            f.fired=true;
            if(f.expireAfterWrite){const auto result=DefSubclassProc(h,m,w,l);f.expireAfterWrite->queue.AdvanceProjectEpoch();return result;}
            if(!f.truncate)return FALSE;auto item=*reinterpret_cast<TCITEMW*>(l);item.pszText=const_cast<wchar_t*>(L"truncated");return DefSubclassProc(h,m,w,reinterpret_cast<LPARAM>(&item));
        }
        return DefSubclassProc(h,m,w,l);
    }
};
static void FailuresAndIdentity(){
    Fixture x;const auto identity=x.Identity();x.Register();auto t=x.Submit();x.queue.Drain();x.Done(t);const auto prior=x.Tab(0);
    for(bool truncate:{false,true}){
        CaptionFault f{false,truncate};CHECK(SetWindowSubclass(x.tabs,CaptionFault::Proc,0x50535446,reinterpret_cast<DWORD_PTR>(&f)));
        x.label=truncate?"Truncation candidate":"Rejected candidate";t=x.Submit();x.queue.Drain();CHECK(f.fired&&x.Result(t).result==MMS_E_FAILED&&x.Tab(0)==prior);
        CHECK(RemoveWindowSubclass(x.tabs,CaptionFault::Proc,0x50535446));CHECK(x.refresh.release(x.refresh.context,t)==MMS_OK);x.native->Layout(0,400,300,true);x.Identity(identity);
    }
    const auto old=x.Submit(),paint=x.Submit(MMS_UI_REFRESH_REPAINT);auto d=x.Desc();CHECK(x.content.updateProvider(x.content.context,x.providers[0],&d)==MMS_OK);
    CHECK(x.Result(old).result==MMS_E_STALE);x.queue.Drain();x.Done(paint,MMS_UI_REFRESH_REPAINT);CHECK(x.refresh.release(x.refresh.context,old)==MMS_OK);
    x.onPrepare=[](MmsHandle)->MmsResult{throw std::runtime_error("panel content failure");};t=x.Submit();x.queue.Drain();CHECK(x.Result(t).result==MMS_E_FAILED&&x.Tab(0)==prior&&!x.registry.InCallback());CHECK(x.refresh.release(x.refresh.context,t)==MMS_OK);
    x.native->Layout(0,400,300,true);
    x.onPrepare=[](MmsHandle)->MmsResult{throw std::bad_alloc();};t=x.Submit();x.queue.Drain();CHECK(x.Result(t).result==MMS_E_MEMORY&&x.Tab(0)==prior&&!x.registry.InCallback());CHECK(x.refresh.release(x.refresh.context,t)==MMS_OK);
    x.onPrepare=[&](MmsHandle b){MmsUiStateContentV1 bad{Header<MmsUiStateContentV1>(),MMS_UI_STATE_LABEL,0,{"\xff",1},{},{}};return x.content.writePrepared(x.content.context,b,&bad);};
    t=x.Submit();x.queue.Drain();CHECK(x.Result(t).result==MMS_E_ARGUMENT&&x.Tab(0)==prior);CHECK(x.refresh.release(x.refresh.context,t)==MMS_OK);
    x.onPrepare=[&](MmsHandle b){MmsUiStateContentV1 c{Header<MmsUiStateContentV1>(),1,0,S("Expired"),{}, {}};const auto r=x.content.writePrepared(x.content.context,b,&c);x.queue.AdvanceProjectEpoch();return r;};
    t=x.Submit();x.queue.Drain();CHECK(x.Result(t).result==MMS_E_STALE&&x.Tab(0)==prior);CHECK(x.refresh.release(x.refresh.context,t)==MMS_OK);x.Identity(identity);
}
static void NativeEpochAndLayoutRecovery(){
    Fixture x;const auto identity=x.Identity();x.Register();auto t=x.Submit();x.queue.Drain();x.Done(t);
    const auto retained=x.Tab(0),help=x.Help();CaptionFault fault;fault.expireAfterWrite=&x;
    CHECK(SetWindowSubclass(x.tabs,CaptionFault::Proc,0x50535446,reinterpret_cast<DWORD_PTR>(&fault)));
    x.label="Native caption written by expired ticket";x.description="Unpublished description";
    t=x.Submit();x.queue.Drain();CHECK(fault.fired&&x.Result(t).result==MMS_E_STALE&&x.Tab(0)==L"Native caption written by expired ticket");
    CHECK(RemoveWindowSubclass(x.tabs,CaptionFault::Proc,0x50535446));CHECK(x.refresh.release(x.refresh.context,t)==MMS_OK);
    CHECK(!x.Target().ready);x.native->Layout(0,400,300,true);
    CHECK(x.Target().ready&&x.Tab(0)==retained&&x.Help()==help);x.Identity(identity);
    // A fresh provider token uses the new epoch and the same original surface.
    auto desc=x.Desc();CHECK(x.content.updateProvider(x.content.context,x.providers[0],&desc)==MMS_OK);
    t=x.Submit();x.queue.Drain();x.Done(t);CHECK(x.Tab(0)==L"Native caption written by expired ticket"&&x.Help()==L"Unpublished description");x.Identity(identity);
}
static bool ActualBounds(HWND child,const MmsUiRectV1& expected){
    RECT r{};if(!GetWindowRect(child,&r))return false;SetLastError(ERROR_SUCCESS);
    if(!MapWindowPoints(nullptr,GetParent(child),reinterpret_cast<POINT*>(&r),2)&&GetLastError()!=ERROR_SUCCESS)return false;
    return r.left==expected.x&&r.top==expected.y&&int64_t(r.right)-r.left==expected.width&&int64_t(r.bottom)-r.top==expected.height;
}
static void SurfaceFailureCompensation(){
    Fixture x;const auto identity=x.Identity();x.Register();auto t=x.Submit();x.queue.Drain();x.Done(t);const auto caption=x.Tab(0);
    // Rejection before visibility publication must preserve both logical and
    // native visibility. No late map allocation is needed to publish a panel.
    CaptionFault reject;CHECK(SetWindowSubclass(x.tabs,CaptionFault::Proc,0x50535446,reinterpret_cast<DWORD_PTR>(&reject)));
    x.visible=0;x.label="Rejected hidden panel";t=x.Submit(12);x.queue.Drain();CHECK(reject.fired&&x.Result(t).result==MMS_E_FAILED&&x.Tab(0)==caption);
    CHECK(RemoveWindowSubclass(x.tabs,CaptionFault::Proc,0x50535446));CHECK(x.refresh.release(x.refresh.context,t)==MMS_OK);
    x.native->Layout(0,400,300,true);CHECK(x.Target().ready&&TabCtrl_GetItemCount(x.tabs)==2&&IsWindowVisible(x.views[0].window));x.Identity(identity);
    x.label="Updated panel";
    for(bool refuseRestore:{false,true}){
        x.views[0].throwShowAfter=1;x.views[0].throwRestoreBefore=refuseRestore;const auto shows=x.views[0].shows;
        t=x.Submit(12);x.queue.Drain();CHECK(x.Result(t).result==MMS_E_FAILED&&x.views[0].shows==shows+2);
        UiRegistry::SurfacePresentation view;CHECK(x.registry.ExistingSurface(x.ids[0],view)&&view.visible);
        CHECK(bool(GetWindowLongPtrW(view.window,GWL_STYLE)&WS_VISIBLE)==!refuseRestore&&!x.Target().ready);
        CHECK(x.refresh.release(x.refresh.context,t)==MMS_OK);x.native->Layout(0,400,300,true);
        CHECK(x.Target().ready&&IsWindowVisible(view.window)&&x.Tab(0)==caption&&TabCtrl_GetItemCount(x.tabs)==2);x.Identity(identity);
    }
    // Metadata was not updated after this callback moved the real child and
    // threw. Identical desired bounds must still invoke a repair on layout.
    UiRegistry::SurfacePresentation before;CHECK(x.registry.ExistingSurface(x.ids[0],before));auto moved=before.bounds;moved.x+=7;moved.width+=11;
    x.views[0].throwResizeAfter=1;const auto resizes=x.views[0].resizes;
    CHECK(x.registry.ShowSurface(x.ids[0],GetParent(before.window),moved,before.dpi,true)==MMS_E_FAILED);
    CHECK(ActualBounds(before.window,moved)&&!x.Target().ready);
    UiRegistry::SurfacePresentation metadata;CHECK(x.registry.ExistingSurface(x.ids[0],metadata)&&!std::memcmp(&metadata.bounds,&before.bounds,sizeof(before.bounds)));
    x.native->Layout(0,400,300,true);CHECK(x.Target().ready&&ActualBounds(before.window,before.bounds)&&x.views[0].resizes==resizes+2);x.Identity(identity);
}
static void UnmountedAndRetirement(){
    {
        Fixture x(false);MmsUiRefreshTargetV1 target{};target.h=Header<MmsUiRefreshTargetV1>();CHECK(x.refresh.getTarget(x.refresh.context,x.panels[1],&target)==MMS_E_NOT_READY&&!x.views[1].creates);
        x.Select(1);CHECK(x.Target(1).supportedKinds&MMS_UI_REFRESH_STATE);x.Select(0);x.Register();
        auto d=x.Desc();d.fields=MMS_UI_STATE_ENABLED;MmsHandle bad=0;CHECK(x.content.registerProvider(x.content.context,&d,&bad)==MMS_E_UNSUPPORTED&&!bad);
        auto t=x.Submit();const auto calls=x.prepares;x.owner.stopping=true;x.queue.Drain();CHECK(x.Result(t).result!=MMS_OK&&x.prepares==calls);CHECK(x.refresh.release(x.refresh.context,t)==MMS_OK);
    }
    {
        Fixture x;x.Register();const auto original=x.Target();const auto t=x.Submit();const auto calls=x.prepares;
        x.queue.Retire(x.client);x.registry.Retire(x.owner);x.native->Refresh();x.queue.Drain();
        CHECK(x.Result(t).status==MMS_UI_REFRESH_CANCELLED&&x.prepares==calls&&!IsWindow(x.views[0].window));
        CHECK(original.target.element==x.panels[0]&&TabCtrl_GetItemCount(x.tabs)==0);
    }
    {
        Fixture x;x.Register();const auto original=x.Target();const auto t=x.Submit();const auto calls=x.prepares;
        auto replacement=x.registry.elements.at(x.ids[0]).desc;replacement.label=S("Replacement panel");
        CHECK(x.ui.updateElement(x.ui.context,x.panels[0],&replacement)==MMS_OK);
        CHECK(x.Result(t).result==MMS_E_STALE&&x.prepares==calls);CHECK(x.refresh.release(x.refresh.context,t)==MMS_OK);
        x.native->Refresh();CHECK(x.Target().target.element==original.target.element&&
            x.Target().target.instanceGeneration!=original.target.instanceGeneration&&x.Tab(0)==L"Replacement panel");
        CHECK(x.views[0].creates==2&&x.views[0].destroys==1&&!x.views[0].buttonMessages);
    }
}
}
static void PanelStateRefreshChecks(){
    panel_state_refresh_checks::TabSelectionThenStateLayout();
    panel_state_refresh_checks::ContentAndAvailability();panel_state_refresh_checks::FailuresAndIdentity();
    panel_state_refresh_checks::NativeEpochAndLayoutRecovery();panel_state_refresh_checks::SurfaceFailureCompensation();
    panel_state_refresh_checks::UnmountedAndRetirement();
}
