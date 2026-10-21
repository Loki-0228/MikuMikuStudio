// Include after manager_refresh_checks.inc and state_content_checks.inc.
// These tests use USER32 menus and the original NativeUi/refresh services.
#include <MinHook.h>
#include <atomic>
#include <exception>

namespace menu_state_refresh_checks {
struct Item {
    HMENU parent=nullptr,submenu=nullptr; unsigned id=0; ULONG_PTR cookie=0;
    UINT state=0; std::wstring caption;
};
static Item Read(HMENU menu,unsigned position) {
    wchar_t text[8194]{};MENUITEMINFOW info{sizeof(info)};
    info.fMask=MIIM_DATA|MIIM_ID|MIIM_SUBMENU|MIIM_STATE|MIIM_STRING;
    info.dwTypeData=text;info.cch=unsigned(std::size(text));
    CHECK(GetMenuItemInfoW(menu,position,TRUE,&info));
    return {menu,info.hSubMenu,info.wID,info.dwItemData,info.fState,text};
}
static int Position(HMENU menu,ULONG_PTR cookie) {
    for(int i=0;i<GetMenuItemCount(menu);++i)if(Read(menu,unsigned(i)).cookie==cookie)return i;
    return -1;
}
struct Fixture {
    manager_refresh_checks::Window window;
    UiRegistry registry;
    UiClient owner{&registry,"example.menustate"},foreign{&registry,"example.menuforeign"};
    UiRefreshQueue& queue=registry.Refresh();RefreshClient client{&queue,owner.id};
    MmsUiApiV1 ui=registry.Api(owner);MmsUiRefreshApiV1 refresh=queue.Api(client);
    MmsUiRefreshStateContentApiV1 content=registry.StateContentApi(owner);
    StorageStore storage;SharedHistory history{storage};
    std::unique_ptr<NativeUi> native;
    std::array<std::string,4> ids{{owner.id+".menu",owner.id+".first",owner.id+".nested",owner.id+".last"}};
    std::array<MmsHandle,4> elements{},providers{};
    std::array<MmsHandle,3> commands{};
    std::array<uint32_t,3> enabled{{1,1,1}},visible{{1,1,1}},checked{};
    std::array<unsigned,3> reads{},calls{};
    std::function<void(unsigned)> duringState;
    struct Context {Fixture* self;unsigned index;} contexts[3]{{this,0},{this,1},{this,2}};
    unsigned prepares=0,propertyReads=0;
    MmsHandle property=0,input=0,button=0;
    std::vector<MmsHandle> tickets;
    std::string label="Dynamic menu label";
    std::function<MmsResult(MmsHandle)> onPrepare;
    std::atomic<unsigned> wakeSignals{0};unsigned wakeDispatches=0,retryDispatches=0;
    unsigned entered=0,exited=0,closeMessages=0;
    bool blockReplayedClose=false;
    bool inspectPosted=false;std::function<void()> inspect;
    bool inspectCompleted=false;
    DWORD inspectPostError=ERROR_SUCCESS;
    unsigned inspectDeliveries=0;
    struct LoopEvent {UINT message;WPARAM wp;LPARAM lp;DWORD elapsed;};
    std::array<LoopEvent,96> loopTrace{};
    size_t loopTraceCount=0,loopTraceDropped=0;
    DWORD loopStarted=0;
    std::exception_ptr loopError;bool timedOut=false;
    static constexpr UINT wakeMessage=WM_APP+0x6c1,inspectMessage=WM_APP+0x6c2;
    static constexpr UINT_PTR retryTimer=0x6c310,deadlineTimer=0x6c311;
    explicit Fixture() {
        for(unsigned i=0;i<3;++i) {
            const auto id=owner.id+".command"+std::to_string(i);
            MmsCommandDescV1 c{};c.info.h=Header<MmsCommandDescV1>();c.info.id=Text(id);
            c.info.ownerPlugin=Text(owner.id);c.info.label=S("Menu command");c.context=&contexts[i];
            c.getState=[](void* p,MmsObjectRef,MmsCommandStateV1* out)->MmsResult {
                auto& c=*static_cast<Context*>(p);auto& x=*c.self;++x.reads[c.index];
                *out={Header<MmsCommandStateV1>(),x.enabled[c.index],x.checked[c.index],x.visible[c.index],0,{}};if(x.duringState)x.duringState(c.index);return MMS_OK;
            };
            c.invoke=[](void* p,MmsObjectRef,MmsSpan,MmsHandle)->MmsResult{auto& c=*static_cast<Context*>(p);++c.self->calls[c.index];return MMS_OK;};
            CHECK(ui.registerCommand(ui.context,&c,&commands[i])==MMS_OK);
        }
        for(unsigned i=0;i<4;++i) {
            MmsUiElementV1 e{};e.h=Header<MmsUiElementV1>();e.id=Text(ids[i]);e.ownerPlugin=Text(owner.id);
            e.parentId=i==0?S("mms.menu.plugins"):Text(ids[i==3?2:0]);
            e.label=i==0?S("Root menu"):i==1?S("First item"):i==2?S("Nested menu"):S("Last item");
            e.kind=(i==0||i==2)?MMS_UI_MENU:MMS_UI_MENU_ITEM;e.capabilities=MMS_UI_HIDE;
            if(e.kind==MMS_UI_MENU)e.capabilities|=MMS_UI_CONTAINER;
            const auto commandId=owner.id+".command"+std::to_string(i==3?2:i);
            if(i!=2)e.commandId=Text(commandId);
            CHECK(ui.registerElement(ui.context,&e,&elements[i])==MMS_OK);
        }
        // A real INPUT and BUTTON prove menu deferral does not stall unrelated
        // native consumers. The property is deliberately readonly/no edit host.
        MmsPropertyDescV1 p{};p.info.h=Header<MmsPropertyDescV1>();p.info.id=S("example.menustate.value");
        p.info.ownerPlugin=Text(owner.id);p.info.label=S("Menu peer input");p.info.valueType=MMS_VALUE_INT;
        p.info.flags=MMS_PROPERTY_READ_ONLY;p.context=this;
        p.read=[](void* p,MmsObjectRef,MmsValue* out,MmsHandle* lease)->MmsResult {auto& x=*static_cast<Fixture*>(p);++x.propertyReads;*out={};out->type=MMS_VALUE_INT;out->value.signedValue=42;*lease=1;return MMS_OK;};
        p.releaseValue=[](void*,MmsHandle){};
        CHECK(ui.registerProperty(ui.context,&p,&property)==MMS_OK);
        MmsUiElementV1 e{};e.h=Header<MmsUiElementV1>();e.id=S("example.menustate.input");e.ownerPlugin=Text(owner.id);
        e.parentId=S("mms.toolbar.main.extensions");e.propertyId=p.info.id;e.kind=MMS_UI_INPUT;e.capabilities=MMS_UI_LAYOUT|MMS_UI_HIDE;
        CHECK(ui.registerElement(ui.context,&e,&input)==MMS_OK);
        e.id=S("example.menustate.button");e.propertyId={};e.commandId=S("example.menustate.command1");e.label=S("Peer button");e.kind=MMS_UI_BUTTON;
        CHECK(ui.registerElement(ui.context,&e,&button)==MMS_OK);
        native=std::make_unique<NativeUi>(registry,window.value,Log{});native->Refresh();CHECK(native->Layout(0,500,200,true)>0);
        CHECK(SetWindowSubclass(window.value,Observe,0x6c340,reinterpret_cast<DWORD_PTR>(this)));
        queue.SetWake([this]{++wakeSignals;return PostMessageW(window.value,wakeMessage,0,0)!=FALSE;});
        onPrepare=[this](MmsHandle builder){const auto local=label;MmsUiStateContentV1 c{Header<MmsUiStateContentV1>(),MMS_UI_STATE_LABEL,0,Text(local),{}, {}};return content.writePrepared(content.context,builder,&c);};
    }
    ~Fixture() {
        queue.SetWake({});KillTimer(window.value,retryTimer);KillTimer(window.value,deadlineTimer);
        RemoveWindowSubclass(window.value,Observe,0x6c340);
        native.reset();queue.Retire(client);registry.Retire(owner);registry.Retire(foreign);
    }
    static LRESULT CALLBACK Observe(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
        auto& x=*reinterpret_cast<Fixture*>(data);
        if(x.inspect&&(m==WM_ENTERMENULOOP||m==WM_EXITMENULOOP||m==WM_INITMENU||m==WM_INITMENUPOPUP||
            m==WM_UNINITMENUPOPUP||m==WM_MENUSELECT||m==WM_ENTERIDLE||m==WM_CANCELMODE||m==WM_CAPTURECHANGED||
            m==WM_CLOSE||m==inspectMessage||m==wakeMessage||m==WM_TIMER)) {
            if(x.loopTraceCount<x.loopTrace.size())x.loopTrace[x.loopTraceCount++]={m,w,l,GetTickCount()-x.loopStarted};
            else ++x.loopTraceDropped;
        }
        if(m==WM_ENTERMENULOOP)++x.entered;
        if(m==WM_EXITMENULOOP)++x.exited;
        if(m==WM_CLOSE){++x.closeMessages;if(x.blockReplayedClose&&x.entered==x.exited)return 0;return DefSubclassProc(h,m,w,l);}
        if(m==WM_INITMENUPOPUP&&x.inspect&&!x.inspectPosted){
            SetLastError(ERROR_SUCCESS);x.inspectPosted=PostMessageW(h,inspectMessage,0,0)!=FALSE;
            x.inspectPostError=x.inspectPosted?ERROR_SUCCESS:GetLastError();
        }
        if(m==wakeMessage||(m==WM_TIMER&&w==retryTimer)) {
            KillTimer(h,retryTimer);if(m==wakeMessage)++x.wakeDispatches;else ++x.retryDispatches;
            try {x.queue.Drain();if(x.queue.Pending())SetTimer(h,retryTimer,16,nullptr);}catch(...){x.loopError=std::current_exception();EndMenu();}
            return 0;
        }
        if(m==inspectMessage) {
            ++x.inspectDeliveries;
            try {if(x.inspect){x.inspect();x.inspectCompleted=true;}}catch(...){x.loopError=std::current_exception();}
            EndMenu();return 0;
        }
        if(m==WM_TIMER&&w==deadlineTimer){x.timedOut=true;EndMenu();return 0;}
        return DefSubclassProc(h,m,w,l);
    }
    HMENU PluginMenu()const{const auto main=GetMenu(window.value);CHECK(main&&GetMenuItemCount(main)==1);const auto p=GetSubMenu(main,0);CHECK(p);return p;}
    Item At(unsigned i)const {
        const auto root=Read(PluginMenu(),0);
        if(i==0)return root;
        const auto first=Read(root.submenu,0),nested=Read(root.submenu,1);
        if(i==1)return first;if(i==2)return nested;return Read(nested.submenu,0);
    }
    MmsUiRefreshTargetV1 NativeTarget(MmsHandle e){MmsUiRefreshTargetV1 t{};t.h=Header<MmsUiRefreshTargetV1>();CHECK(refresh.getTarget(refresh.context,e,&t)==MMS_OK);return t;}
    MmsUiRefreshTargetV1 Target(unsigned i=1){return NativeTarget(elements[i]);}
    MmsUiRefreshRequestV1 Request(unsigned i=1,uint32_t scope=MMS_UI_REFRESH_SELF,uint32_t kinds=MMS_UI_REFRESH_STATE){MmsUiRefreshRequestV1 r{};r.h=Header<MmsUiRefreshRequestV1>();r.target=Target(i).target;r.scope=scope;r.kinds=kinds;return r;}
    MmsHandle Submit(const MmsUiRefreshRequestV1& request,bool worker=false){MmsHandle t=0;MmsResult result=MMS_E_FAILED;if(worker){std::thread job([&]{result=refresh.request(refresh.context,&request,&t);});job.join();}else result=refresh.request(refresh.context,&request,&t);CHECK(result==MMS_OK&&t);tickets.push_back(t);return t;}
    MmsUiRefreshStateV1 State(MmsHandle t){MmsUiRefreshStateV1 s{};s.h=Header<MmsUiRefreshStateV1>();CHECK(refresh.query(refresh.context,t,&s)==MMS_OK);return s;}
    void Done(MmsHandle t){const auto s=State(t);CHECK(s.status==MMS_UI_REFRESH_COMPLETED&&s.result==MMS_OK&&s.completedKinds==MMS_UI_REFRESH_STATE&&s.geometryRevision==0&&!s.sourceRevision);}
    MmsUiStateContentDescV1 Desc(unsigned i=1){MmsUiStateContentDescV1 d{};d.h=Header<MmsUiStateContentDescV1>();d.target=Target(i).target;d.context=this;d.fields=MMS_UI_STATE_LABEL;
        d.prepare=[](void* p,const MmsUiTargetRefV1* target,MmsHandle builder)->MmsResult{auto& x=*static_cast<Fixture*>(p);++x.prepares;
            CHECK(x.queue.ReadOnlyCause(x.client));MmsHandle refused=0;auto r=x.Request();r.target=*target;
            CHECK(x.refresh.request(x.refresh.context,&r,&refused)==MMS_E_BUSY&&!refused);
            CHECK(x.ui.unregisterItem(x.ui.context,x.elements[3])==MMS_E_BUSY);
            CHECK(x.ui.invoke(x.ui.context,S("example.menustate.command1"),{},{})==MMS_E_BUSY);
            return x.onPrepare(builder);};return d;}
    void Register(unsigned i=1){const auto d=Desc(i);CHECK(content.registerProvider(content.context,&d,&providers[i])==MMS_OK&&providers[i]);}
    void Identity(const Item& old,unsigned i=1){const auto current=At(i);CHECK(current.parent==old.parent&&current.submenu==old.submenu&&current.id==old.id&&current.cookie==old.cookie);}
    void Loop(const std::function<void()>& body) {
        inspect=body;inspectPosted=false;loopError={};timedOut=false;
        inspectCompleted=false;inspectPostError=ERROR_SUCCESS;inspectDeliveries=0;
        loopTraceCount=loopTraceDropped=0;loopStarted=GetTickCount();
        CHECK(SetTimer(window.value,deadlineTimer,3000,nullptr));
        const auto beforeEnter=entered,beforeExit=exited;
        const HWND foreground=GetForegroundWindow(),focused=GetFocus(),capture=GetCapture();
        const auto menu=PluginMenu();const auto itemCount=GetMenuItemCount(menu);
        // Keep the production menubar notification path: NONOTIFY suppresses
        // ENTER/INIT/EXIT as well as the command notification on this USER32.
        // RETURNCMD avoids dispatching a command; the fixture ends the real
        // loop with EndMenu, without synthetic pointer/keyboard input.
        SetLastError(ERROR_SUCCESS);
        const auto result=TrackPopupMenuEx(menu,TPM_LEFTALIGN|TPM_TOPALIGN|TPM_RETURNCMD,12,12,window.value,nullptr);
        const auto error=GetLastError();
        const bool complete=inspectPosted&&inspectCompleted&&!timedOut&&entered==beforeEnter+1&&exited==beforeExit+1;
        std::cerr<<"MENU_STATE_LOOP result="<<result<<" error="<<error<<" entered="<<entered-beforeEnter<<" exited="<<exited-beforeExit
            <<" inspect_posted="<<inspectPosted<<" post_error="<<inspectPostError<<" deliveries="<<inspectDeliveries
            <<" completed="<<inspectCompleted<<" timeout="<<timedOut<<" exception="<<bool(loopError)
            <<" owner="<<window.value<<" owner_alive="<<IsWindow(window.value)<<" owner_visible="<<IsWindowVisible(window.value)
            <<" style="<<GetWindowLongPtrW(window.value,GWL_STYLE)<<" exstyle="<<GetWindowLongPtrW(window.value,GWL_EXSTYLE)
            <<" menu="<<menu<<" menu_alive="<<IsMenu(menu)<<" items="<<itemCount
            <<" foreground="<<foreground<<"/"<<GetForegroundWindow()<<" focus="<<focused<<"/"<<GetFocus()
            <<" capture="<<capture<<"/"<<GetCapture()<<" trace_dropped="<<loopTraceDropped<<'\n';
        if(!complete||loopError)for(size_t i=0;i<loopTraceCount;++i){const auto& e=loopTrace[i];std::cerr<<"MENU_STATE_EVENT ms="<<e.elapsed<<" message="<<e.message<<" wp="<<e.wp<<" lp="<<e.lp<<'\n';}
        KillTimer(window.value,deadlineTimer);inspect={};
        if(loopError)std::rethrow_exception(loopError);
        CHECK(inspectPosted&&inspectCompleted&&!timedOut&&entered==beforeEnter+1&&exited==beforeExit+1);
    }
};
static void TargetsAndContent() {
    Fixture x;const auto original=x.At(1),root=x.At(0),nested=x.At(2),last=x.At(3);const auto target=x.Target();
    CHECK(target.ready&&target.supportedKinds==MMS_UI_REFRESH_STATE&&target.supportedScopes==3&&target.visible&&target.width==0&&target.height==0&&!target.geometryRevision);
    for(const auto kind:{MMS_UI_REFRESH_REPAINT,MMS_UI_REFRESH_DATA,MMS_UI_REFRESH_LAYOUT,MMS_UI_REFRESH_STATE|MMS_UI_REFRESH_LAYOUT}) {
        auto r=x.Request(1,MMS_UI_REFRESH_SELF,kind);MmsHandle denied=0;CHECK(x.refresh.request(x.refresh.context,&r,&denied)==MMS_E_UNSUPPORTED&&!denied);
    }
    auto d=x.Desc();d.fields=MMS_UI_STATE_DESCRIPTION;MmsHandle denied=0;CHECK(x.content.registerProvider(x.content.context,&d,&denied)==MMS_E_UNSUPPORTED&&!denied);
    d.fields=MMS_UI_STATE_DISABLED_REASON;CHECK(x.content.registerProvider(x.content.context,&d,&denied)==MMS_E_UNSUPPORTED&&!denied);
    const auto revision=x.registry.Revision(),preferences=x.registry.PreferencesRevision();const auto reads=x.propertyReads;
    x.Register();x.label="菜单・動的ラベル";const auto first=x.Submit(x.Request(),true),second=x.Submit(x.Request(),true);
    x.queue.Drain();x.Done(first);x.Done(second);CHECK(x.prepares==1&&x.At(1).caption==L"菜单・動的ラベル");
    x.Identity(original);x.Identity(root,0);x.Identity(nested,2);x.Identity(last,3);
    CHECK(x.registry.Revision()==revision&&x.registry.PreferencesRevision()==preferences&&x.propertyReads==reads&&x.calls[1]==0&&x.history.UndoCount()==0&&x.history.RedoCount()==0);
    CHECK(x.Target().target.instanceGeneration==target.target.instanceGeneration);
    x.enabled[1]=0;x.checked[1]=1;auto t=x.Submit(x.Request());x.queue.Drain();x.Done(t);CHECK((x.At(1).state&MFS_DISABLED)&&(x.At(1).state&MFS_CHECKED));
    CHECK(x.native->Command(original.id)&&x.calls[1]==0);
    x.enabled[1]=1;t=x.Submit(x.Request());x.queue.Drain();x.Done(t);CHECK(!(x.At(1).state&MFS_DISABLED));
    CHECK(x.native->Command(original.id)&&x.calls[1]==1);x.Identity(original);
    // A commandless MENU keeps the metadata baseline and still consumes LABEL.
    x.Register(2);t=x.Submit(x.Request(2));x.queue.Drain();x.Done(t);CHECK(x.At(2).caption==L"菜单・動的ラベル");
    CHECK(x.content.unregisterProvider(x.content.context,x.providers[1])==MMS_OK);x.providers[1]=0;
    t=x.Submit(x.Request());x.queue.Drain();x.Done(t);CHECK(x.At(1).caption==L"First item");x.Identity(original);
    x.native->Refresh();x.Identity(original);x.Identity(root,0);x.Identity(nested,2);x.Identity(last,3);
}
static void SubtreeVisibilityAndLoop() {
    Fixture x;x.Register();const auto root=x.At(0),first=x.At(1),nested=x.At(2),last=x.At(3);
    auto r=x.Request(0,MMS_UI_REFRESH_SUBTREE);const auto a=x.Submit(r),b=x.Submit(r,true);x.queue.Drain();x.Done(a);x.Done(b);CHECK(x.prepares==1);
    x.visible[0]=0;auto t=x.Submit(x.Request(0));x.queue.Drain();x.Done(t);
    CHECK(GetMenuItemCount(x.PluginMenu())==0&&IsMenu(root.submenu)&&IsMenu(nested.submenu)&&!x.Target(0).visible&&x.Target(0).ready);
    CHECK(x.native->Command(first.id)&&!x.calls[1]);
    x.visible[0]=1;t=x.Submit(x.Request(0));x.queue.Drain();x.Done(t);x.Identity(root,0);x.Identity(first);x.Identity(nested,2);x.Identity(last,3);
    // An open, real root-menu loop delays only menu consumption. Accepted
    // tickets keep their status and are woken once at WM_EXITMENULOOP.
    x.label="After native loop";MmsHandle pending=0,cancelled=0;
    unsigned beforeExitWake=0;
    x.Loop([&] {
        CHECK(x.entered>x.exited);const auto before=x.prepares;const auto wakes=x.wakeSignals.load();
        pending=x.Submit(x.Request(),true);cancelled=x.Submit(x.Request(),true);
        for(unsigned i=0;i<30;++i)x.queue.Drain();
        CHECK(x.State(pending).status==MMS_UI_REFRESH_PENDING&&x.prepares==before&&!x.queue.Pending()&&x.wakeSignals==wakes);
        CHECK(x.refresh.cancel(x.refresh.context,cancelled)==MMS_OK&&x.State(cancelled).status==MMS_UI_REFRESH_CANCELLED);
        auto peer=x.NativeTarget(x.button);MmsUiRefreshRequestV1 p{};p.h=Header<MmsUiRefreshRequestV1>();p.target=peer.target;p.kinds=MMS_UI_REFRESH_STATE;p.scope=MMS_UI_REFRESH_SELF;
        const auto button=x.Submit(p,true);CHECK(x.wakeSignals==wakes+1);x.queue.Drain();
        CHECK(x.State(button).status==MMS_UI_REFRESH_COMPLETED&&x.State(pending).status==MMS_UI_REFRESH_PENDING);
        p.target=x.NativeTarget(x.input).target;p.kinds=MMS_UI_REFRESH_DATA;const auto beforeReads=x.propertyReads;
        const auto input=x.Submit(p,true);x.queue.Drain();CHECK(x.State(input).status==MMS_UI_REFRESH_COMPLETED&&x.propertyReads==beforeReads+1);
        CHECK(!x.queue.Pending()&&x.At(1).caption!=L"After native loop");beforeExitWake=x.wakeSignals;
    });
    CHECK(x.wakeSignals==beforeExitWake+1);x.queue.Drain();x.Done(pending);CHECK(x.At(1).caption==L"After native loop");
    manager_refresh_checks::Pump(35);CHECK(x.retryDispatches==0);x.Identity(first);x.Identity(root,0);
}
static void Invalidations() {
    Fixture x;x.Register();const auto old=x.At(1);const auto oldTarget=x.Target();
    auto t=x.Submit(x.Request());auto desc=x.Desc();CHECK(x.content.updateProvider(x.content.context,x.providers[1],&desc)==MMS_OK);
    x.queue.Drain();CHECK(x.State(t).result==MMS_E_STALE&&x.At(1).caption==L"First item");
    t=x.Submit(x.Request());x.queue.AdvanceProjectEpoch();x.queue.Drain();CHECK(x.State(t).result==MMS_E_STALE);
    CHECK(x.content.unregisterProvider(x.content.context,x.providers[1])==MMS_OK);x.providers[1]=0;x.Register();
    const auto before=x.prepares;
    const auto replacementBefore=x.Target();auto oldRequest=x.Request();oldRequest.target=replacementBefore.target;
    x.Loop([&]{
        t=x.Submit(oldRequest);
        const auto replacement=x.registry.elements.at(x.ids[1]).desc;
        CHECK(x.ui.updateElement(x.ui.context,x.elements[1],&replacement)==MMS_OK);
        CHECK(x.State(t).result==MMS_E_STALE);CHECK(x.native->Command(old.id)&&x.calls[1]==0);
        x.native->Refresh();CHECK(Position(old.parent,old.cookie)>=0&&x.prepares==before);
    });
    x.native->Refresh();const auto replaced=x.At(1);
    std::cerr<<"MENU_STATE_REPLACEMENT old_id="<<old.id<<" new_id="<<replaced.id
        <<" old_cookie="<<old.cookie<<" new_cookie="<<replaced.cookie<<'\n';
    CHECK(replaced.id!=old.id&&replaced.cookie);
    // The cookie identifies the live MenuState by address; allocation may reuse it.
    // Verify the actual native item and the independent presentation identity.
    const int position=Position(replaced.parent,replaced.cookie);CHECK(position>=0);
    MENUITEMINFOW actual{sizeof(actual)};actual.fMask=MIIM_ID|MIIM_DATA|MIIM_SUBMENU|MIIM_STATE;
    CHECK(GetMenuItemInfoW(replaced.parent,unsigned(position),TRUE,&actual));
    CHECK(actual.wID==replaced.id&&actual.dwItemData==replaced.cookie&&actual.hSubMenu==replaced.submenu&&actual.fState==replaced.state);
    const auto replacementAfter=x.Target();
    CHECK(replacementAfter.target.element==replacementBefore.target.element&&
        replacementAfter.target.projectEpoch==replacementBefore.target.projectEpoch);
    CHECK(replacementAfter.target.instanceGeneration!=replacementBefore.target.instanceGeneration);
    CHECK(replacementAfter.target.instanceGeneration!=oldTarget.target.instanceGeneration);
    MmsHandle refused=UINT64_C(0x76123456789abcde);
    CHECK(x.refresh.request(x.refresh.context,&oldRequest,&refused)==MMS_E_STALE&&refused==UINT64_C(0x76123456789abcde));
    CHECK(x.State(t).result==MMS_E_STALE&&x.prepares==before&&x.At(1).caption==replaced.caption);
    auto currentRequest=oldRequest;currentRequest.target=replacementAfter.target;
    const auto current=x.Submit(currentRequest);x.queue.Drain();x.Done(current);
    const auto calls=x.calls;
    CHECK(!x.native->Command(old.id)&&x.calls==calls);
    CHECK(x.native->Command(replaced.id)&&x.calls[0]==calls[0]&&x.calls[1]==calls[1]+1&&x.calls[2]==calls[2]);
    t=x.Submit(x.Request());x.queue.Retire(x.client);x.queue.Drain();CHECK(x.State(t).status==MMS_UI_REFRESH_CANCELLED&&x.State(t).result==MMS_E_CANCELLED);
}
static void NativeAncestorsAndLifetime() {
    Fixture x;const auto root=x.At(0),leaf=x.At(1);const auto oldCalls=x.calls[1];
    CHECK(RemoveMenu(root.parent,0,MF_BYPOSITION));CHECK(IsMenu(root.submenu));
    CHECK(x.native->Command(leaf.id)&&x.calls[1]==oldCalls);
    x.native->Refresh();x.Identity(root,0);x.Identity(leaf);
    CHECK(EnableMenuItem(root.parent,0,MF_BYPOSITION|MF_DISABLED)!=UINT(-1));
    CHECK(x.native->Command(leaf.id)&&x.calls[1]==oldCalls);x.native->Refresh();
    MENUITEMINFOW id{sizeof(id)};id.fMask=MIIM_ID;id.wID=root.id+0x100;
    CHECK(SetMenuItemInfoW(root.parent,0,TRUE,&id));CHECK(x.native->Command(leaf.id)&&x.calls[1]==oldCalls);
    id.wID=root.id;CHECK(SetMenuItemInfoW(root.parent,0,TRUE,&id));x.native->Refresh();
    x.duringState=[&](unsigned index){if(index==0)CHECK(RemoveMenu(root.parent,0,MF_BYPOSITION));};
    CHECK(x.native->Command(leaf.id)&&x.calls[1]==oldCalls);x.duringState={};x.native->Refresh();x.Identity(root,0);
    CHECK(x.native->Command(leaf.id)&&x.calls[1]==oldCalls+1);
    x.blockReplayedClose=true;
    x.Loop([&]{SendMessageW(x.window.value,WM_CLOSE,0,0);CHECK(IsWindow(x.window.value)&&x.closeMessages==1);});
    manager_refresh_checks::Pump(20);CHECK(x.closeMessages==2&&IsWindow(x.window.value));
}
static void ForeignCookieOwnership() {
    Fixture x;const auto root=x.At(0),nested=x.At(2);const auto plugins=x.PluginMenu();
    // Simulate another native owner taking over an otherwise still-valid menu.
    // Cleanup must forget this raw handle instead of destroying that owner.
    CHECK(RemoveMenu(root.parent,0,MF_BYPOSITION));
    CHECK(RemoveMenu(root.submenu,1,MF_BYPOSITION));
    MENUINFO owner{sizeof(owner)};owner.fMask=MIM_MENUDATA;owner.dwMenuData=0x6cffff;
    CHECK(SetMenuInfo(root.submenu,&owner)&&SetMenuInfo(nested.submenu,&owner));
    while(GetMenuItemCount(root.submenu)>0)CHECK(RemoveMenu(root.submenu,0,MF_BYPOSITION));
    while(GetMenuItemCount(nested.submenu)>0)CHECK(RemoveMenu(nested.submenu,0,MF_BYPOSITION));
    CHECK(AppendMenuW(root.submenu,MF_STRING,0x6123,L"Foreign retained root"));
    CHECK(AppendMenuW(nested.submenu,MF_STRING,0x6124,L"Foreign retained nested"));
    CHECK(RemoveMenu(GetMenu(x.window.value),0,MF_BYPOSITION));
    owner.dwMenuData=0x6cffee;CHECK(SetMenuInfo(plugins,&owner));
    CHECK(AppendMenuW(plugins,MF_STRING,0x6125,L"Foreign plugin popup"));
    x.native->Refresh();
    CHECK(IsMenu(root.submenu)&&IsMenu(nested.submenu)&&IsMenu(plugins));
    CHECK(GetMenuItemCount(root.submenu)==1&&Read(root.submenu,0).caption==L"Foreign retained root");
    CHECK(GetMenuItemCount(nested.submenu)==1&&Read(nested.submenu,0).caption==L"Foreign retained nested");
    CHECK(GetMenuItemCount(plugins)==1&&Read(plugins,0).caption==L"Foreign plugin popup");
    CHECK(DestroyMenu(root.submenu)&&DestroyMenu(nested.submenu)&&DestroyMenu(plugins));
}
static void OwnershipAndUnload() {
    {
        Fixture x;auto other=x.registry.Api(x.foreign);MmsHandle command=0,element=0;
        MmsCommandDescV1 c{};c.info.h=Header<MmsCommandDescV1>();c.info.id=S("example.menuforeign.command");c.info.ownerPlugin=Text(x.foreign.id);
        c.getState=[](void*,MmsObjectRef,MmsCommandStateV1* out)->MmsResult{*out={Header<MmsCommandStateV1>(),1,0,1,0,{}};return MMS_OK;};
        c.invoke=[](void*,MmsObjectRef,MmsSpan,MmsHandle)->MmsResult{return MMS_OK;};CHECK(other.registerCommand(other.context,&c,&command)==MMS_OK);
        MmsUiElementV1 e{};e.h=Header<MmsUiElementV1>();e.id=S("example.menuforeign.child");e.ownerPlugin=Text(x.foreign.id);
        e.parentId=Text(x.ids[0]);e.commandId=c.info.id;e.label=S("Foreign child");e.kind=MMS_UI_MENU_ITEM;
        CHECK(other.registerElement(other.context,&e,&element)==MMS_OK);x.native->Refresh();
        auto r=x.Request(0,MMS_UI_REFRESH_SUBTREE);MmsHandle rejected=0;CHECK(x.refresh.request(x.refresh.context,&r,&rejected)==MMS_E_ARGUMENT&&!rejected);
        MmsUiRefreshTargetV1 target{};target.h=Header<MmsUiRefreshTargetV1>();CHECK(x.refresh.getTarget(x.refresh.context,element,&target)==MMS_E_ARGUMENT);
        CHECK(other.unregisterItem(other.context,element)==MMS_OK);x.native->Refresh();
        const auto t=x.Submit(x.Request(0,MMS_UI_REFRESH_SUBTREE));x.queue.Drain();x.Done(t);
    }
    {
        Fixture x;x.Register();const auto old=x.At(1),root=x.At(0);MmsHandle pending=0;
        x.Loop([&] {
            pending=x.Submit(x.Request(),true);x.queue.Retire(x.client);x.registry.Retire(x.owner);
            CHECK(x.State(pending).status==MMS_UI_REFRESH_CANCELLED);
            CHECK(x.native->Command(old.id)&&!x.calls[1]);x.native->Refresh();
            CHECK(IsMenu(root.submenu)&&Position(old.parent,old.cookie)>=0);
        });
        x.native->Refresh();CHECK(!IsMenu(root.submenu)&&GetMenuItemCount(GetMenu(x.window.value))==0);
    }
}

struct NativeFault {
    using Set=BOOL(WINAPI*)(HMENU,UINT,BOOL,LPCMENUITEMINFOW);
    using Insert=BOOL(WINAPI*)(HMENU,UINT,BOOL,LPCMENUITEMINFOW);
    using Get=BOOL(WINAPI*)(HMENU,UINT,BOOL,LPMENUITEMINFOW);
    using SetInfo=BOOL(WINAPI*)(HMENU,LPCMENUINFO);
    inline static Set realSet=nullptr;inline static Insert realInsert=nullptr;
    inline static Get realGet=nullptr;
    inline static SetInfo realSetInfo=nullptr;
    inline static bool failCreation=false;
    inline static unsigned creationSkip=0;
    inline static HMENU failedCreation=nullptr;
    inline static NativeFault* active=nullptr;
    Fixture& fixture;HMENU menu;ULONG_PTR cookie;bool armed=true,after=false,truncate=false,epoch=false,insert=false;
    unsigned hits=0;
    bool readback=false;unsigned readSkip=0;std::wstring readCaption;
    NativeFault(Fixture& x,const Item& item):fixture(x),menu(item.parent),cookie(item.cookie){CHECK(!active);active=this;}
    ~NativeFault(){active=nullptr;}
    bool Match(HMENU m,LPCMENUITEMINFOW i)const noexcept{return armed&&m==menu&&i&&(i->fMask&MIIM_DATA)&&i->dwItemData==cookie;}
    static BOOL WINAPI SetItem(HMENU m,UINT item,BOOL position,LPCMENUITEMINFOW info) {
        auto* f=active;if(!f||f->insert||f->readback||!f->Match(m,info))return realSet(m,item,position,info);
        f->armed=false;++f->hits;
        if(!f->after&&!f->truncate&&!f->epoch)return FALSE;
        auto changed=*info;std::wstring text;
        if(f->truncate&&(info->fMask&MIIM_STRING)){text=info->dwTypeData;text+=L"corrupt";changed.dwTypeData=text.data();}
        const auto result=realSet(m,item,position,&changed);
        if(f->epoch)f->fixture.queue.AdvanceProjectEpoch();
        return f->after?FALSE:result;
    }
    static BOOL WINAPI InsertItem(HMENU m,UINT item,BOOL position,LPCMENUITEMINFOW info) {
        auto* f=active;if(!f||!f->insert||!f->Match(m,info))return realInsert(m,item,position,info);
        f->armed=false;++f->hits;const auto result=realInsert(m,item,position,info);return f->after?FALSE:result;
    }
    static BOOL WINAPI GetItem(HMENU m,UINT item,BOOL position,LPMENUITEMINFOW info) {
        const auto result=realGet(m,item,position,info);auto* f=active;
        if(result&&f&&f->armed&&f->readback&&m==f->menu&&(info->fMask&MIIM_STRING)&&info->dwTypeData&&
            f->readCaption==info->dwTypeData){if(f->readSkip)--f->readSkip;else{f->armed=false;++f->hits;f->fixture.queue.AdvanceProjectEpoch();}}
        return result;
    }
    static BOOL WINAPI SetInfoValue(HMENU m,LPCMENUINFO info) {
        if(failCreation&&info&&(info->fMask&MIM_MENUDATA)&&info->dwMenuData&&GetMenuItemCount(m)==0) {
            if(creationSkip)--creationSkip;
            else {failCreation=false;failedCreation=m;return FALSE;}
        }
        return realSetInfo(m,info);
    }
};
struct Hooks {
    bool own=false;
    Hooks(){const auto init=MH_Initialize();CHECK(init==MH_OK||init==MH_ERROR_ALREADY_INITIALIZED);own=init==MH_OK;
        CHECK(MH_CreateHook(reinterpret_cast<void*>(&SetMenuItemInfoW),reinterpret_cast<void*>(&NativeFault::SetItem),reinterpret_cast<void**>(&NativeFault::realSet))==MH_OK);
        CHECK(MH_CreateHook(reinterpret_cast<void*>(&InsertMenuItemW),reinterpret_cast<void*>(&NativeFault::InsertItem),reinterpret_cast<void**>(&NativeFault::realInsert))==MH_OK);
        CHECK(MH_CreateHook(reinterpret_cast<void*>(&GetMenuItemInfoW),reinterpret_cast<void*>(&NativeFault::GetItem),reinterpret_cast<void**>(&NativeFault::realGet))==MH_OK);
        CHECK(MH_CreateHook(reinterpret_cast<void*>(&SetMenuInfo),reinterpret_cast<void*>(&NativeFault::SetInfoValue),reinterpret_cast<void**>(&NativeFault::realSetInfo))==MH_OK);
        CHECK(MH_EnableHook(reinterpret_cast<void*>(&SetMenuItemInfoW))==MH_OK&&MH_EnableHook(reinterpret_cast<void*>(&InsertMenuItemW))==MH_OK&&MH_EnableHook(reinterpret_cast<void*>(&GetMenuItemInfoW))==MH_OK&&MH_EnableHook(reinterpret_cast<void*>(&SetMenuInfo))==MH_OK);}
    ~Hooks(){MH_DisableHook(reinterpret_cast<void*>(&SetMenuItemInfoW));MH_DisableHook(reinterpret_cast<void*>(&InsertMenuItemW));MH_DisableHook(reinterpret_cast<void*>(&GetMenuItemInfoW));MH_DisableHook(reinterpret_cast<void*>(&SetMenuInfo));MH_RemoveHook(reinterpret_cast<void*>(&SetMenuItemInfoW));MH_RemoveHook(reinterpret_cast<void*>(&InsertMenuItemW));MH_RemoveHook(reinterpret_cast<void*>(&GetMenuItemInfoW));MH_RemoveHook(reinterpret_cast<void*>(&SetMenuInfo));if(own)MH_Uninitialize();}
};
static void FailureAndRepair() {
    Hooks hooks;
    for(unsigned skip:{0u,1u}) {
        NativeFault::creationSkip=skip;NativeFault::failedCreation=nullptr;NativeFault::failCreation=true;
        CHECK(Throws([]{Fixture rejected;}));NativeFault::failCreation=false;
        CHECK(NativeFault::failedCreation&&!IsMenu(NativeFault::failedCreation));
    }
    for(unsigned mode=0;mode<4;++mode) {
        Fixture x;x.Register();const auto before=x.At(1);const auto target=x.Target();x.label="Native publication candidate";
        MmsHandle t=0;
        {
            NativeFault fault(x,before);fault.after=mode==1;fault.truncate=mode==2;fault.epoch=mode==3;
            t=x.Submit(x.Request());x.queue.Drain();CHECK(fault.hits==1);
            const auto state=x.State(t);CHECK(state.status==MMS_UI_REFRESH_FAILED&&state.result==(mode==3?MMS_E_STALE:MMS_E_FAILED));
        }
        CHECK(x.At(1).caption==before.caption);x.Identity(before);CHECK(!x.Target().ready);
        // Ordinary reconciliation repairs retained content without replacing
        // the native submenu/item or registration instance.
        x.native->Refresh();x.Identity(before);CHECK(x.Target().ready&&x.At(1).caption==before.caption);
        if(mode==3){CHECK(x.content.unregisterProvider(x.content.context,x.providers[1])==MMS_OK);x.providers[1]=0;x.Register();}
        t=x.Submit(x.Request());x.queue.Drain();x.Done(t);CHECK(x.At(1).caption==L"Native publication candidate");x.Identity(before);
        CHECK(x.Target().target.instanceGeneration==target.target.instanceGeneration);
    }
    Fixture x;x.Register();const auto before=x.At(1);x.visible[1]=0;auto t=x.Submit(x.Request());x.queue.Drain();x.Done(t);
    CHECK(Position(before.parent,before.cookie)<0&&x.Target().ready&&!x.Target().visible);
    x.visible[1]=1;
    {NativeFault fault(x,before);fault.insert=fault.after=true;t=x.Submit(x.Request());x.queue.Drain();CHECK(fault.hits==1&&x.State(t).result==MMS_E_FAILED);}
    CHECK(Position(before.parent,before.cookie)<0);x.native->Refresh();x.Identity(before);
    t=x.Submit(x.Request());x.queue.Drain();x.Done(t);x.Identity(before);
    for(bool noop:{false,true}) {
        Fixture y;y.Register();const auto original=y.At(1);y.label=noop?"First item":"Candidate readback epoch";
        {NativeFault fault(y,original);fault.readback=true;fault.readSkip=noop?0:1;fault.readCaption=noop?L"First item":L"Candidate readback epoch";
            const auto ticket=y.Submit(y.Request());y.queue.Drain();CHECK(fault.hits==1&&y.State(ticket).result==MMS_E_STALE);}
        CHECK(y.At(1).caption==original.caption);y.native->Refresh();y.Identity(original);CHECK(y.At(1).caption==original.caption);
    }
}
}
static void MenuStateRefreshChecks() {
    using namespace menu_state_refresh_checks;
    TargetsAndContent();SubtreeVisibilityAndLoop();Invalidations();NativeAncestorsAndLifetime();ForeignCookieOwnership();OwnershipAndUnload();FailureAndRepair();
}
