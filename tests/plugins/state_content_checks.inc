// Include after manager_refresh_checks.inc. Real NativeUi/USER32 consumers;
// no private refresh queue or substitute button renderer.
#include <mms/cpp/state_content.hpp>
#include "ui_refresh_binding.hpp"
#include <array>
#include <cstdio>

namespace state_content_checks {
namespace sdk = mms::sdk;
struct Fixture {
    manager_refresh_checks::Window window;
    UiRegistry registry;
    UiClient owner{&registry,"example.content"}, foreign{&registry,"example.foreign"};
    UiRefreshQueue& queue = registry.Refresh();
    RefreshClient client{&queue,owner.id};
    MmsUiApiV1 ui = registry.Api(owner);
    MmsUiRefreshApiV1 refresh = queue.Api(client);
    MmsUiRefreshStateContentApiV1 content = registry.StateContentApi(owner);
    MmsUiRefreshStateContentApiV1 other = registry.StateContentApi(foreign);
    MmsUiLayoutApiV1 layout = registry.LayoutApi(owner);
    std::unique_ptr<NativeUi> native;
    std::array<MmsHandle,2> commands{}, buttons{};
    std::array<HWND,2> controls{};
    std::array<unsigned,2> stateReads{};
    struct Command { Fixture* fixture; unsigned index; } contexts[2]{{this,0},{this,1}};
    std::vector<MmsHandle> tickets;
    MmsHandle provider=0, builder=0;
    unsigned prepares=0;
    uint32_t enabled=1, visible=1, checked=0;
    std::string reason="Legacy disabled reason", label="Updated caption", description="Updated description", disabled="Updated reason";
    std::function<MmsResult(MmsHandle)> prepare;
    Fixture() {
        for(unsigned i=0;i<2;++i) {
            const auto id=owner.id+(i?".peer":".button"), commandId=id+".command";
            MmsCommandDescV1 command{}; command.info.h=Header<MmsCommandDescV1>();
            command.info.id=Text(commandId);command.info.ownerPlugin=Text(owner.id);command.info.label=S("Command");
            command.info.description=S("Registered description");command.context=&contexts[i];
            command.getState=[](void* p,MmsObjectRef,MmsCommandStateV1* out)->MmsResult{
                auto& c=*static_cast<Command*>(p);auto& x=*c.fixture;++x.stateReads[c.index];
                *out={Header<MmsCommandStateV1>(),c.index?1:x.enabled,c.index?0:x.checked,c.index?1:x.visible,0,Text(x.reason)};return MMS_OK;
            };
            command.invoke=[](void*,MmsObjectRef,MmsSpan,MmsHandle)->MmsResult{return MMS_OK;};
            CHECK(ui.registerCommand(ui.context,&command,&commands[i])==MMS_OK);
            MmsUiElementV1 element{};element.h=Header<MmsUiElementV1>();element.id=Text(id);element.ownerPlugin=Text(owner.id);
            element.parentId=S("mms.toolbar.main.extensions");element.label=i?S("Peer"):S("Original");element.commandId=Text(commandId);
            element.kind=MMS_UI_BUTTON;element.capabilities=MMS_UI_LAYOUT|MMS_UI_HIDE|MMS_UI_MOVE;
            CHECK(ui.registerElement(ui.context,&element,&buttons[i])==MMS_OK);
        }
        native=std::make_unique<NativeUi>(registry,window.value,Log{});native->Refresh();CHECK(native->Layout(0,250,800,true)>0);
        for(auto child=GetWindow(native->Toolbar(),GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
            const auto text=Caption(child);if(text==L"Original")controls[0]=child;if(text==L"Peer")controls[1]=child;
        }
        CHECK(controls[0]&&controls[1]);
        prepare=[this](MmsHandle handle){
            // The source buffer dies before prepare returns. writePrepared must
            // already own both UTF-8 conversion and tooltip storage.
            std::string localLabel=label,localDescription=description,localDisabled=disabled;
            MmsUiStateContentV1 value{Header<MmsUiStateContentV1>(),7,0,Text(localLabel),Text(localDescription),Text(localDisabled)};
            const auto result=content.writePrepared(content.context,handle,&value);
            localLabel.assign(localLabel.size(),'x');localDescription.clear();localDisabled.clear();return result;
        };
    }
    ~Fixture(){
        if(provider)content.unregisterProvider(content.context,provider);
        for(auto ticket:tickets){refresh.cancel(refresh.context,ticket);refresh.release(refresh.context,ticket);}
        native.reset();queue.Retire(client);registry.Retire(owner);
    }
    static std::wstring Caption(HWND hwnd){wchar_t text[8194]{};GetWindowTextW(hwnd,text,8194);return text;}
    MmsUiRefreshTargetV1 Target(unsigned index=0){MmsUiRefreshTargetV1 t{};t.h=Header<MmsUiRefreshTargetV1>();CHECK(refresh.getTarget(refresh.context,buttons[index],&t)==MMS_OK);return t;}
    MmsUiStateContentDescV1 Desc(){
        MmsUiStateContentDescV1 d{};d.h=Header<MmsUiStateContentDescV1>();d.target=Target().target;d.context=this;d.fields=7;
        d.prepare=[](void* p,const MmsUiTargetRefV1* target,MmsHandle builder)->MmsResult{
            auto& x=*static_cast<Fixture*>(p);CHECK(target&&target->element==x.buttons[0]);++x.prepares;x.builder=builder;return x.prepare(builder);
        };return d;
    }
    void Register(){const auto d=Desc();CHECK(content.registerProvider(content.context,&d,&provider)==MMS_OK&&provider);}
    MmsUiRefreshRequestV1 Request(uint32_t kinds=MMS_UI_REFRESH_STATE|MMS_UI_REFRESH_REPAINT){
        MmsUiRefreshRequestV1 r{};r.h=Header<MmsUiRefreshRequestV1>();r.target=Target().target;r.kinds=kinds;r.scope=MMS_UI_REFRESH_SELF;return r;
    }
    MmsHandle Submit(const MmsUiRefreshRequestV1& request,bool worker=false){
        MmsHandle ticket=0;MmsResult result=MMS_E_FAILED;
        if(worker){std::thread job([&]{result=refresh.request(refresh.context,&request,&ticket);});job.join();}
        else result=refresh.request(refresh.context,&request,&ticket);
        CHECK(result==MMS_OK&&ticket);tickets.push_back(ticket);return ticket;
    }
    MmsUiRefreshStateV1 State(MmsHandle ticket){MmsUiRefreshStateV1 s{};s.h=Header<MmsUiRefreshStateV1>();CHECK(refresh.query(refresh.context,ticket,&s)==MMS_OK);return s;}
    void Complete(MmsHandle ticket,uint32_t kinds){const auto s=State(ticket);CHECK(s.status==MMS_UI_REFRESH_COMPLETED&&s.result==MMS_OK&&s.completedKinds==kinds);}
    HWND Tooltip(){
        struct Search{HWND owner,found=nullptr;} search{window.value};
        EnumThreadWindows(GetCurrentThreadId(),[](HWND hwnd,LPARAM p)->BOOL{
            auto& s=*reinterpret_cast<Search*>(p);wchar_t name[64]{};GetClassNameW(hwnd,name,64);
            if(GetWindow(hwnd,GW_OWNER)==s.owner&&!std::wcscmp(name,TOOLTIPS_CLASSW)){s.found=hwnd;return FALSE;}return TRUE;
        },reinterpret_cast<LPARAM>(&search));CHECK(search.found);return search.found;
    }
    std::wstring Help(bool parentRectangle=false){
        wchar_t text[8194]{};TOOLINFOW tool{};tool.cbSize=TTTOOLINFOW_V1_SIZE;tool.hwnd=native->Toolbar();
        tool.uId=parentRectangle?UINT_PTR(buttons[0]):reinterpret_cast<UINT_PTR>(controls[0]);tool.lpszText=text;
        const auto tooltip=Tooltip();CHECK(SendMessageW(tooltip,TTM_GETTOOLINFOW,0,reinterpret_cast<LPARAM>(&tool)));
        tool.lpszText=text;SendMessageW(tooltip,TTM_GETTEXTW,8194,reinterpret_cast<LPARAM>(&tool));return text;
    }
    void HitDisabled(const char* phase){
        CHECK(!IsWindowEnabled(controls[0])&&IsWindowVisible(controls[0]));
        RECT client{};CHECK(GetClientRect(controls[0],&client)&&client.right>0&&client.bottom>0);
        POINT center{client.right/2,client.bottom/2};SetLastError(ERROR_SUCCESS);
        const auto mapped=MapWindowPoints(controls[0],native->Toolbar(),&center,1);
        CHECK(mapped||GetLastError()==ERROR_SUCCESS);
        wchar_t text[8194]{};TTHITTESTINFOW hit{};hit.hwnd=native->Toolbar();hit.pt=center;
        hit.ti.cbSize=TTTOOLINFOW_V1_SIZE;hit.ti.lpszText=text;
        const auto found=SendMessageW(Tooltip(),TTM_HITTESTW,0,reinterpret_cast<LPARAM>(&hit));
        std::printf("STATE_CONTENT_DISABLED_HIT phase=%s found=%lld point=%ld,%ld id=%llu expected=%llu flags=%u rect=%ld,%ld,%ld,%ld\n",
            phase,static_cast<long long>(found),center.x,center.y,static_cast<unsigned long long>(hit.ti.uId),
            static_cast<unsigned long long>(buttons[0]),hit.ti.uFlags,hit.ti.rect.left,hit.ti.rect.top,hit.ti.rect.right,hit.ti.rect.bottom);
        CHECK(found&&hit.ti.hwnd==native->Toolbar()&&hit.ti.uId==UINT_PTR(buttons[0])&&!(hit.ti.uFlags&TTF_IDISHWND));
        CHECK(PtInRect(&hit.ti.rect,center));
    }
    RECT Bounds(){RECT r{};CHECK(GetWindowRect(controls[0],&r));return r;}
    MmsHostV1 Host(){MmsHostV1 host{};host.h=Header<MmsHostV1>();host.context=this;
        host.queryService=[](void* p,MmsString name,uint32_t ma,uint32_t mi,uint32_t bytes,void* out)->MmsResult{
            auto& x=*static_cast<Fixture*>(p);const auto id=String(name);
            if(id=="mms.ui.refresh")return Table(ma,mi,bytes,out,x.refresh);
            if(id=="mms.ui.refresh.state")return Table(ma,mi,bytes,out,x.content);return MMS_E_NOT_FOUND;
        };return host;
    }
};
static void ToolInfoPrefixCompatibility(){
    Fixture x;const HWND tooltip=x.Tooltip();
    struct Version{DWORD size,major,minor,build,platform;} version{sizeof(Version),0,0,0,0};
    const auto module=reinterpret_cast<HMODULE>(GetClassLongPtrW(tooltip,GCLP_HMODULE));
    using GetVersion=HRESULT(CALLBACK*)(Version*);
    const auto query=reinterpret_cast<GetVersion>(GetProcAddress(module,"DllGetVersion"));
    CHECK(module&&query&&SUCCEEDED(query(&version))&&version.major>=5);
    wchar_t path[MAX_PATH]{};CHECK(GetModuleFileNameW(module,path,MAX_PATH));
    TOOLINFOW tool{};tool.cbSize=TTTOOLINFOW_V1_SIZE;tool.hwnd=x.native->Toolbar();tool.uId=0x5c0fffff;
    tool.rect={-500,-500,-400,-400};tool.lpszText=const_cast<wchar_t*>(L"中文说明・無効の理由");
    CHECK(SendMessageW(tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool)));
    wchar_t text[128]{};auto read=tool;read.lpszText=text;
    CHECK(SendMessageW(tooltip,TTM_GETTOOLINFOW,0,reinterpret_cast<LPARAM>(&read)));
    read.lpszText=text;SendMessageW(tooltip,TTM_GETTEXTW,128,reinterpret_cast<LPARAM>(&read));
    CHECK(std::wstring(text)==L"中文说明・無効の理由");
    auto oversized=tool;oversized.cbSize=sizeof(oversized);++oversized.uId;
    const bool accepted=SendMessageW(tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&oversized))!=0;
    if(accepted)SendMessageW(tooltip,TTM_DELTOOLW,0,reinterpret_cast<LPARAM>(&oversized));
    if(version.major==5)CHECK(!accepted);
    SendMessageW(tooltip,TTM_DELTOOLW,0,reinterpret_cast<LPARAM>(&tool));
    std::printf("STATE_CONTENT_TOOLTIP version=%lu.%lu prefix=%u sizeof=%zu full_size_accepted=%u unicode=verified module=%ls\n",
        version.major,version.minor,unsigned(TTTOOLINFOW_V1_SIZE),sizeof(TOOLINFOW),unsigned(accepted),path);
}
static void NativeContentAndLayout(){
    Fixture x;const auto handle=x.controls[0];const auto revision=x.registry.Revision(),preferences=x.registry.PreferencesRevision();
    MmsUiStateContentCapabilitiesV1 caps{};caps.h=Header<MmsUiStateContentCapabilitiesV1>();
    CHECK(x.content.getCapabilities(x.content.context,&caps)==MMS_OK&&caps.supportedFields==63&&caps.maxBytesPerField==4096);
    auto legacy=x.Submit(x.Request());x.enabled=0;x.queue.Drain();x.Complete(legacy,5);
    CHECK(x.Help()==L"Registered description\nLegacy disabled reason"&&x.Help(true)==x.Help());
    x.HitDisabled("legacy");
    x.Register();x.checked=1;const auto reads=x.stateReads;auto ticket=x.Submit(x.Request(),true);
    CHECK(x.prepares==0&&Fixture::Caption(handle)==L"Original");x.queue.Drain();x.Complete(ticket,5);
    CHECK(x.controls[0]==handle&&Fixture::Caption(handle)==Wide(x.label)&&!IsWindowEnabled(handle)&&SendMessageW(handle,BM_GETCHECK,0,0)==BST_CHECKED);
    CHECK(x.Help()==L"Updated description\nUpdated reason"&&x.Help(true)==x.Help());
    x.HitDisabled("provider");
    CHECK(x.stateReads[0]==reads[0]+1&&x.stateReads[1]==reads[1]&&x.prepares==1);
    const auto before=x.Bounds();x.label="A substantially longer button caption";x.enabled=1;
    auto layout=x.Submit(x.Request(MMS_UI_REFRESH_STATE|MMS_UI_REFRESH_LAYOUT|MMS_UI_REFRESH_REPAINT));x.queue.Drain();x.Complete(layout,13);
    const auto after=x.Bounds();CHECK(after.right-after.left>before.right-before.left&&x.Help()==Wide(x.description));
    x.enabled=0;auto disabledAfterLayout=x.Submit(x.Request());x.queue.Drain();x.Complete(disabledAfterLayout,5);
    x.HitDisabled("after-layout-and-disable");
    const auto prepares=x.prepares;const auto cachedReads=x.stateReads;
    auto pure=x.Submit(x.Request(MMS_UI_REFRESH_LAYOUT));x.queue.Drain();x.Complete(pure,8);
    CHECK(x.prepares==prepares&&x.stateReads==cachedReads&&Fixture::Caption(handle)==Wide(x.label));
    CHECK(x.registry.Revision()==revision&&x.registry.PreferencesRevision()==preferences);
    x.visible=0;auto hide=x.Submit(x.Request(13));x.queue.Drain();x.Complete(hide,13);const auto collapsed=x.Target();CHECK(!collapsed.ready);
    x.visible=1;x.label="Restored";auto restore=x.Submit(x.Request(13));x.queue.Drain();x.Complete(restore,13);
    CHECK(x.Target().target.instanceGeneration==collapsed.target.instanceGeneration&&x.Target().ready&&Fixture::Caption(handle)==L"Restored");
}
static void ReplacementAndFailures(){
    Fixture x;x.Register();auto first=x.Submit(x.Request());x.queue.Drain();x.Complete(first,5);
    auto obsolete=x.Submit(x.Request()),repaint=x.Submit(x.Request(MMS_UI_REFRESH_REPAINT));
    auto desc=x.Desc();CHECK(x.content.updateProvider(x.content.context,x.provider,&desc)==MMS_OK);
    CHECK(x.State(obsolete).result==MMS_E_STALE&&x.State(repaint).status==MMS_UI_REFRESH_PENDING);x.queue.Drain();x.Complete(repaint,1);
    const auto oldLabel=Fixture::Caption(x.controls[0]),oldHelp=x.Help();
    for(unsigned failure=0;failure<7;++failure){
        x.prepare=[&](MmsHandle builder)->MmsResult{
            if(failure==0)return MMS_E_FAILED;if(failure==1)return MMS_OK;
            if(failure==2)throw std::runtime_error("controlled provider exception");
            if(failure==6)throw std::bad_alloc();
            std::string bytes=failure==3?std::string(4097,'a'):failure==4?std::string("\xc3\x28",2):"new";
            MmsUiStateContentV1 value{Header<MmsUiStateContentV1>(),1,0,Text(bytes),{}, {}};
            auto result=x.content.writePrepared(x.content.context,builder,&value);
            if(failure==5){CHECK(result==MMS_OK);CHECK(x.content.writePrepared(x.content.context,builder,&value)==MMS_E_CONFLICT);return MMS_OK;}
            CHECK(result==MMS_E_ARGUMENT);return MMS_OK;
        };
        auto ticket=x.Submit(x.Request(13));x.queue.Drain();const auto state=x.State(ticket);
        CHECK(state.status==MMS_UI_REFRESH_FAILED&&!state.completedKinds&&Fixture::Caption(x.controls[0])==oldLabel&&x.Help()==oldHelp);
        if(failure==6)CHECK(state.result==MMS_E_MEMORY);
    }
    MmsUiStateContentV1 value{};value.h=Header<MmsUiStateContentV1>();CHECK(x.content.writePrepared(x.content.context,x.builder,&value)==MMS_E_NOT_FOUND);
    const auto old=x.Submit(x.Request());CHECK(x.content.unregisterProvider(x.content.context,x.provider)==MMS_OK);x.provider=0;
    CHECK(x.State(old).result==MMS_E_STALE);auto reset=x.Submit(x.Request());x.queue.Drain();x.Complete(reset,5);
    CHECK(Fixture::Caption(x.controls[0])==L"Original"&&x.Help()==L"Registered description");
}
static void UpdatedReasonSurvivesNormalRefresh(){
    Fixture x;x.Register();x.disabled="First reason";
    auto first=x.Submit(x.Request());x.queue.Drain();x.Complete(first,5);CHECK(x.Help()==Wide(x.description));
    x.disabled="Latest reason";auto changed=x.Submit(x.Request());x.queue.Drain();x.Complete(changed,5);
    CHECK(x.Help()==Wide(x.description));const auto prepares=x.prepares;
    x.enabled=0;x.native->Refresh();
    CHECK(x.prepares==prepares&&x.Help()==Wide(x.description+"\nLatest reason")&&x.Help(true)==x.Help());
}
static void CaptionFailureAndProviderEpoch(){
    Fixture x;x.Register();auto original=x.Submit(x.Request());x.queue.Drain();x.Complete(original,5);
    const auto prior=Fixture::Caption(x.controls[0]),help=x.Help();
    struct Intercept{unsigned mode=0,writes=0;} intercept;
    const auto filter=[](HWND hwnd,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)->LRESULT{
        auto& state=*reinterpret_cast<Intercept*>(data);
        if(message==WM_SETTEXT&&state.mode){++state.writes;if(state.mode==1)return FALSE;
            if(state.mode==2){
                // The API wrapper may report success regardless of our return.
                // Forward genuinely different text so readback must reject it.
                DefSubclassProc(hwnd,message,wp,reinterpret_cast<LPARAM>(L"Partial caption"));return TRUE;
            }return TRUE;}
        return DefSubclassProc(hwnd,message,wp,lp);
    };
    CHECK(SetWindowSubclass(x.controls[0],filter,0x5c01,reinterpret_cast<DWORD_PTR>(&intercept)));
    x.label="Rejected caption";
    for(unsigned mode:{1u,3u,2u}){
        intercept.mode=mode;auto ticket=x.Submit(x.Request(13));x.queue.Drain();const auto observed=x.State(ticket);
        const auto actual=Fixture::Caption(x.controls[0]);const auto ready=x.Target().ready;
        std::printf("STATE_CONTENT_CAPTION mode=%u status=%u result=%d completed=%u writes=%u prepares=%u ready=%u actual=%ls prior=%ls desired=%ls\n",
            mode,observed.status,observed.result,observed.completedKinds,intercept.writes,x.prepares,ready,
            actual.c_str(),prior.c_str(),Wide(x.label).c_str());
        CHECK(observed.result==MMS_E_FAILED&&!observed.completedKinds);
        if(mode!=2){
            CHECK(Fixture::Caption(x.controls[0])==prior&&x.Help()==help);
            const auto reads=x.prepares;auto layout=x.Submit(x.Request(8));x.queue.Drain();x.Complete(layout,8);CHECK(x.prepares==reads);
        }else{
            CHECK(!x.Target().ready&&Fixture::Caption(x.controls[0])==L"Partial caption");
            MmsUiLayoutSnapshotV1 snapshot{};snapshot.h=Header<MmsUiLayoutSnapshotV1>();snapshot.handle=777;
            CHECK(x.layout.acquireSnapshot(x.layout.context,&snapshot)==MMS_E_NOT_READY&&snapshot.handle==777);
            // Still rejecting writes must not republish mismatched geometry.
            x.native->Layout(0,250,800,true);CHECK(!x.Target().ready);
        }
        intercept.mode=0;if(mode==2){
            const auto prepares=x.prepares;x.native->Layout(0,250,800,true);
            CHECK(x.Target().ready&&Fixture::Caption(x.controls[0])==prior&&x.prepares==prepares);
        }
    }
    CHECK(intercept.writes==4);CHECK(RemoveWindowSubclass(x.controls[0],filter,0x5c01));
    x.prepare=[&](MmsHandle builder)->MmsResult{
        MmsUiStateContentV1 value{Header<MmsUiStateContentV1>(),1,0,S("Stale project caption"),{}, {}};
        const auto result=x.content.writePrepared(x.content.context,builder,&value);x.queue.AdvanceProjectEpoch();return result;
    };
    auto stale=x.Submit(x.Request());x.queue.Drain();CHECK(x.State(stale).result==MMS_E_STALE&&Fixture::Caption(x.controls[0])==prior&&x.Help()==help);
}
static void DataIsolationAndPartialKinds(){
    Fixture x;x.Register();auto data=x.queue.Bindings().Api(x.client);MmsHandle source=0,binding=0;
    MmsUiDataSourceDescV1 desc{Header<MmsUiDataSourceDescV1>(),S("content-source"),64,0,0};
    CHECK(data.registerSource(data.context,&desc,&source)==MMS_OK);
    int number=42;MmsUiDataSourcePublishV1 publish{Header<MmsUiDataSourcePublishV1>(),source,0,{&number,sizeof(number)},0,0};
    MmsUiDataSourceStateV1 sourceState{};sourceState.h=Header<MmsUiDataSourceStateV1>();CHECK(data.publishSource(data.context,&publish,&sourceState)==MMS_OK);
    MmsUiDataBindingDescV1 bind{};bind.h=Header<MmsUiDataBindingDescV1>();bind.target=x.Target().target;bind.source=source;bind.maxDisplayBytes=64;
    CHECK(data.bind(data.context,&bind,&binding)==MMS_OK);
    auto dataRequest=x.Request(MMS_UI_REFRESH_DATA);dataRequest.binding=binding;dataRequest.sourceRevision=sourceState.revision;
    auto pureData=x.Submit(dataRequest);auto state=x.Submit(x.Request());auto repaint=x.Submit(x.Request(1));
    auto provider=x.Desc();CHECK(x.content.updateProvider(x.content.context,x.provider,&provider)==MMS_OK);
    CHECK(x.State(state).result==MMS_E_STALE&&x.State(pureData).status==MMS_UI_REFRESH_PENDING&&x.State(repaint).status==MMS_UI_REFRESH_PENDING);
    x.queue.Drain();x.Complete(pureData,2);x.Complete(repaint,1);
    x.label="A different long label changes the measured button width";auto request=x.Request(15);request.binding=binding;request.sourceRevision=sourceState.revision;
    const auto target=x.Target();MmsUiDirtyRectV1 rect{0,0,4,4};request.expectedGeometryRevision=target.geometryRevision;
    request.dirtyRects={&rect,1,sizeof(rect),1};auto partial=x.Submit(request);x.queue.Drain();const auto result=x.State(partial);
    CHECK(result.status==MMS_UI_REFRESH_FAILED&&result.result==MMS_E_STALE&&result.completedKinds==14&&result.sourceRevision==sourceState.revision);
    MmsUiDataDisplayV1 display{};display.h=Header<MmsUiDataDisplayV1>();CHECK(data.acquireDisplay(data.context,binding,&display)==MMS_OK);
    CHECK(display.bytes.bytes==sizeof(number)&&!std::memcmp(display.bytes.data,&number,sizeof(number)));
    CHECK(data.releaseDisplay(data.context,display.lease)==MMS_OK&&data.unbind(data.context,binding)==MMS_OK&&data.unregisterSource(data.context,source)==MMS_OK);
}
static void BoundedRetainedContent(){
    Fixture x;std::vector<MmsHandle> buttons,providers;
    for(unsigned index=0;index<48;++index){
        const auto id=x.owner.id+".quota"+std::to_string(index);MmsUiElementV1 e{};e.h=Header<MmsUiElementV1>();
        e.id=Text(id);e.ownerPlugin=Text(x.owner.id);e.parentId=S("mms.toolbar.main.extensions");e.label=S("Extra");
        e.commandId=S("example.content.button.command");e.kind=MMS_UI_BUTTON;e.capabilities=MMS_UI_LAYOUT;
        MmsHandle button=0;CHECK(x.ui.registerElement(x.ui.context,&e,&button)==MMS_OK);buttons.push_back(button);
    }
    x.native->Refresh();CHECK(x.native->Layout(0,4000,800,true)>0);std::string full(4096,'a');unsigned completed=0,busy=0;
    struct Context{Fixture* fixture;std::string* bytes;} context{&x,&full};
    for(auto button:buttons){
        MmsUiRefreshTargetV1 target{};target.h=Header<MmsUiRefreshTargetV1>();CHECK(x.refresh.getTarget(x.refresh.context,button,&target)==MMS_OK&&target.ready);
        MmsUiStateContentDescV1 d{};d.h=Header<MmsUiStateContentDescV1>();d.target=target.target;d.context=&context;d.fields=7;
        d.prepare=[](void* p,const MmsUiTargetRefV1*,MmsHandle builder)->MmsResult{
            auto& c=*static_cast<Context*>(p);MmsUiStateContentV1 value{Header<MmsUiStateContentV1>(),7,0,Text(*c.bytes),Text(*c.bytes),Text(*c.bytes)};
            return c.fixture->content.writePrepared(c.fixture->content.context,builder,&value);
        };
        MmsHandle provider=0;CHECK(x.content.registerProvider(x.content.context,&d,&provider)==MMS_OK);providers.push_back(provider);
        auto request=x.Request(MMS_UI_REFRESH_STATE);request.target=target.target;auto ticket=x.Submit(request);x.queue.Drain();const auto result=x.State(ticket);
        if(result.status==MMS_UI_REFRESH_COMPLETED)++completed;else{CHECK(result.result==MMS_E_BUSY&&!result.completedKinds);++busy;}
    }
    CHECK(completed>10&&completed<48&&busy>0);
    for(auto provider:providers)CHECK(x.content.unregisterProvider(x.content.context,provider)==MMS_OK);
}
static void IdentityReadOnlyAndSdk(){
    Fixture x;auto d=x.Desc();MmsHandle sentinel=777;
    CHECK(x.other.registerProvider(x.other.context,&d,&sentinel)==MMS_E_ARGUMENT&&sentinel==777);
    auto invalid=d;invalid.target.instanceGeneration++;CHECK(x.content.registerProvider(x.content.context,&invalid,&sentinel)==MMS_E_STALE&&sentinel==777);
    sdk::StateContentServices sdkContent;sdk::RefreshServices sdkRefresh;const auto host=x.Host();
    CHECK(sdk::StateContentServices::Query(host,sdkContent)==MMS_OK&&sdk::RefreshServices::Query(host,sdkRefresh)==MMS_OK);
    sdk::StateContentProvider provider;CHECK(sdkContent.Register(d,provider)==MMS_OK&&provider.target().element==x.buttons[0]);
    x.prepare=[&](MmsHandle builder)->MmsResult{
        CHECK(x.registry.InCallback()&&x.queue.ReadOnlyCause(x.client));
        CHECK(x.ui.unregisterItem(x.ui.context,x.buttons[0])==MMS_E_BUSY);
        CHECK(provider.Release()==MMS_E_BUSY&&provider);
        auto request=x.Request();MmsHandle out=777;CHECK(x.refresh.request(x.refresh.context,&request,&out)==MMS_E_BUSY&&out==777);
        return sdkContent.Write(builder,"SDK caption","SDK description","SDK unavailable");
    };
    sdk::RefreshTicket ticket;MmsResult result=MMS_E_FAILED;
    std::thread worker([&]{result=provider.Refresh(sdkRefresh,ticket,true);});worker.join();CHECK(result==MMS_OK);
    x.queue.Drain();MmsUiRefreshStateV1 state{};CHECK(ticket.Query(state)==MMS_OK&&state.status==MMS_UI_REFRESH_COMPLETED&&state.completedKinds==13);
    CHECK(Fixture::Caption(x.controls[0])==L"SDK caption"&&x.Help()==L"SDK description");CHECK(ticket.Release()==MMS_OK);
    const auto old=x.Submit(x.Request());x.queue.AdvanceProjectEpoch();CHECK(x.State(old).result==MMS_E_STALE);
    // New project reference must be explicitly rebound to the same provider.
    auto stale=x.Submit(x.Request());x.queue.Drain();CHECK(x.State(stale).result==MMS_E_STALE);
    d.target=x.Target().target;CHECK(provider.Update(d)==MMS_OK);
    std::thread wrong([&]{result=sdkContent.Write(1,"a","b","c");});wrong.join();CHECK(result==MMS_E_THREAD);
    x.queue.Retire(x.client);x.owner.stopping=true;CHECK(provider.Release()==MMS_OK);
    MmsUiStateContentCapabilitiesV1 caps{};caps.h=Header<MmsUiStateContentCapabilitiesV1>();caps.supportedFields=777;
    CHECK(x.content.getCapabilities(x.content.context,&caps)==MMS_E_NOT_READY&&caps.supportedFields==777);
}
} // namespace state_content_checks
static void StateContentChecks(){
    state_content_checks::ToolInfoPrefixCompatibility();
    state_content_checks::NativeContentAndLayout();
    state_content_checks::ReplacementAndFailures();
    state_content_checks::UpdatedReasonSurvivesNormalRefresh();
    state_content_checks::CaptionFailureAndProviderEpoch();
    state_content_checks::DataIsolationAndPartialKinds();
    state_content_checks::BoundedRetainedContent();
    state_content_checks::IdentityReadOnlyAndSdk();
}
