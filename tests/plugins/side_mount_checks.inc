// Include after input_state_content_checks.inc. These are actual NativeUi
// mounts and controls. The small INT journal fixture observes edit side
// effects; it does not construct MMDApp or claim scene/playhead verification.
#include "mikumikustudio/viewport_bounds.hpp"
#include "mikumikustudio/viewport_panel_layout.hpp"
#include <array>

namespace side_mount_checks {
using native_property_input_checks::Caption;
using native_property_input_checks::Enter;
using native_property_input_checks::Escape;
using native_property_input_checks::FindField;
using native_property_input_checks::Pump;
constexpr const char* roots[]{"mms.sidebar.left.extensions", "mms.sidebar.right.extensions"};
constexpr int sidebar = 171;
RECT In(HWND parent, HWND child) {
    RECT rect{}; CHECK(GetWindowRect(child, &rect)); SetLastError(ERROR_SUCCESS);
    CHECK(MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2) || GetLastError() == ERROR_SUCCESS);
    return rect;
}
struct Fixture : input_state_content_checks::Fixture {
    struct Command { Fixture* owner; unsigned side; } contexts[2]{{this,0},{this,1}};
    std::array<std::string,2> groups{"example.input.side-left", "example.input.side-right"};
    std::array<std::string,2> inputIds, commandIds, buttonIds;
    std::array<MmsHandle,2> groupHandles{}, inputs{}, commands{}, buttons{}, separators{};
    std::array<HWND,2> inputFields{}, wrappers{}, buttonWindows{};
    std::array<unsigned,2> invokes{}, stateQueries{};
    std::vector<MmsHandle> fillers;
    uint64_t historyRevision=0;
    LayoutReservations reservations{};
    LayoutReservations synchronizedReservations{};
    unsigned publications=0;
    MmsResult publishedResult=MMS_OK;
    bool publishedThrows=false;
    int maximumWidth=240;
    RECT bounds{};
    explicit Fixture(bool editable=true):input_state_content_checks::Fixture(editable) {
        auto layout=data.registry.LayoutApi(data.owner);
        for(unsigned side=0;side<2;++side){
            const auto suffix=std::to_string(side); inputIds[side]=side?"example.input.side-peer":data.elementId;
            commandIds[side]="example.input.side-command-"+suffix;buttonIds[side]="example.input.side-button-"+suffix;
            MmsUiElementV1 group{};group.h=Header<MmsUiElementV1>();group.id=Text(groups[side]);group.ownerPlugin=Text(data.owner.id);
            group.parentId=S(roots[side]);group.kind=MMS_UI_GROUP;group.capabilities=MMS_UI_CONTAINER|MMS_UI_LAYOUT|MMS_UI_MOVE|MMS_UI_HIDE;
            CHECK(data.ui.registerElement(data.ui.context,&group,&groupHandles[side])==MMS_OK);
            MmsUiLayoutHintsV1 hints{};hints.h=Header<MmsUiLayoutHintsV1>();hints.flow=MMS_FLOW_COLUMN;
            hints.grow[0]=hints.shrink[0]=1;CHECK(layout.setDefaultHints(layout.context,Text(groups[side]),&hints)==MMS_OK);
            auto input=ui_input_broker_checks::Input(inputIds[side],data.owner.id,data.propertyId);input.parentId=Text(groups[side]);
            input.label=side?S("Right original property"):S("Left original property");
            if(side)CHECK(data.ui.registerElement(data.ui.context,&input,&inputs[side])==MMS_OK);
            else{inputs[side]=data.element;CHECK(data.ui.updateElement(data.ui.context,data.element,&input)==MMS_OK);}
            hints={};hints.h=Header<MmsUiLayoutHintsV1>();hints.minimum[0]=80;hints.preferred[0]=240;hints.preferred[1]=28;
            hints.grow[0]=hints.shrink[0]=1;CHECK(layout.setDefaultHints(layout.context,Text(inputIds[side]),&hints)==MMS_OK);
            MmsCommandDescV1 command{};command.info.h=Header<MmsCommandDescV1>();command.info.id=Text(commandIds[side]);
            command.info.ownerPlugin=Text(data.owner.id);command.info.label=side?S("Right side command"):S("Left side command");command.context=&contexts[side];
            command.getState=[](void* p,MmsObjectRef object,MmsCommandStateV1* out)->MmsResult{
                auto& context=*static_cast<Command*>(p);CHECK(!object.id.lo&&!object.id.hi&&!object.generation);++context.owner->stateQueries[context.side];
                *out={Header<MmsCommandStateV1>(),1,0,1,0,{}};return MMS_OK;
            };
            command.invoke=[](void* p,MmsObjectRef object,MmsSpan arguments,MmsHandle transaction)->MmsResult{
                auto& context=*static_cast<Command*>(p);CHECK(!object.id.lo&&!object.id.hi&&!object.generation&&!arguments.count&&!transaction);
                CHECK(context.owner->data.registry.InvokingClient()==&context.owner->data.owner);
                ++context.owner->invokes[context.side];return MMS_OK;
            };
            CHECK(data.ui.registerCommand(data.ui.context,&command,&commands[side])==MMS_OK);
            MmsUiElementV1 button{};button.h=Header<MmsUiElementV1>();button.id=Text(buttonIds[side]);button.ownerPlugin=Text(data.owner.id);
            button.parentId=Text(groups[side]);button.commandId=Text(commandIds[side]);button.label=command.info.label;button.kind=MMS_UI_BUTTON;
            button.capabilities=MMS_UI_LAYOUT|MMS_UI_MOVE|MMS_UI_HIDE|MMS_UI_RESIZE;
            CHECK(data.ui.registerElement(data.ui.context,&button,&buttons[side])==MMS_OK);
            hints={};hints.h=Header<MmsUiLayoutHintsV1>();hints.minimum[0]=70;hints.preferred[0]=220;hints.preferred[1]=24;
            hints.shrink[0]=1;CHECK(layout.setDefaultHints(layout.context,Text(buttonIds[side]),&hints)==MMS_OK);
            const auto separatorId="example.input.side-separator-"+suffix;
            MmsUiElementV1 separator{};separator.h=Header<MmsUiElementV1>();separator.id=Text(separatorId);
            separator.ownerPlugin=Text(data.owner.id);separator.parentId=Text(groups[side]);separator.kind=MMS_UI_SEPARATOR;
            separator.capabilities=MMS_UI_LAYOUT|MMS_UI_MOVE|MMS_UI_HIDE;
            CHECK(data.ui.registerElement(data.ui.context,&separator,&separators[side])==MMS_OK);
        }
        CHECK(history.Revision(historyRevision)==MMS_OK);Arrange(true);Bind();
        synchronizedReservations={0,native->PropertyAreaHeight(),native->LeftAreaWidth(),native->RightAreaWidth()};
        native->SetLayoutPublished(std::function<MmsResult(const LayoutReservations&)>([this](const LayoutReservations& value)->MmsResult{
            CHECK(data.registry.InCallback()&&queue.ReadOnlyCause(client));
            auto layout=data.registry.LayoutApi(data.owner);MmsUiLayoutSnapshotV1 pending{};pending.h=Header<MmsUiLayoutSnapshotV1>();pending.handle=991;
            CHECK(layout.acquireSnapshot(layout.context,&pending)==MMS_E_NOT_READY&&pending.handle==991);
            ++publications;reservations=value;
            if(publishedThrows)throw std::runtime_error("controlled side reservation publication failure");
            if(publishedResult==MMS_OK)synchronizedReservations=value;
            return publishedResult;
        }));
    }
    ~Fixture(){if(native)native->SetLayoutPublished(std::function<MmsResult(const LayoutReservations&)>{});}
    HWND Area(unsigned side)const{return native->SideArea(side!=0);}
    void Configure(int cap=-1,bool shown=true){
        if(cap>=0)maximumWidth=cap;
        RECT clientRect{};CHECK(GetClientRect(window.value,&clientRect));
        bounds={sidebar+9,25,clientRect.right,clientRect.bottom-192};
        native->SetSideAreas(bounds,maximumWidth,shown);
        native->SetPropertyArea(std::max(0,int(clientRect.bottom)-160),100,shown);
    }
    void Arrange(bool refreshViews=false,int cap=-1,bool shown=true){
        Configure(cap,shown);if(refreshViews)native->Refresh();
        native->Layout(bounds.left,std::max(0L,bounds.right-bounds.left),180,shown);
    }
    void Bind(){
        for(unsigned side=0;side<2;++side){
            CHECK(Area(side)&&IsWindow(Area(side)));inputFields[side]=FindField(Area(side));wrappers[side]=GetParent(inputFields[side]);
            CHECK(GetParent(wrappers[side])==Area(side));
            buttonWindows[side]=nullptr;
            const auto label=side?L"Right side command":L"Left side command";
            for(auto child=GetWindow(Area(side),GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))if(Caption(child)==label){CHECK(!buttonWindows[side]);buttonWindows[side]=child;}
            CHECK(buttonWindows[side]&&IsWindowVisible(buttonWindows[side]));
        }
        field=inputFields[0];wrapper=wrappers[0];labelWindow=input_state_content_checks::Label(wrapper);group=groupHandles[0];
    }
    MmsUiRefreshTargetV1 AnyTarget(MmsHandle handle){
        MmsUiRefreshTargetV1 target{};target.h=Header<MmsUiRefreshTargetV1>();
        CHECK(refresh.getTarget(refresh.context,handle,&target)==MMS_OK);return target;
    }
    MmsHandle RequestExact(const MmsUiTargetRefV1& target,uint32_t kinds=MMS_UI_REFRESH_LAYOUT|MMS_UI_REFRESH_REPAINT){
        MmsUiRefreshRequestV1 request{};request.h=Header<MmsUiRefreshRequestV1>();request.target=target;
        request.scope=MMS_UI_REFRESH_SELF;request.kinds=kinds;MmsHandle ticket=0;
        CHECK(refresh.request(refresh.context,&request,&ticket)==MMS_OK&&ticket);tickets.push_back(ticket);return ticket;
    }
    MmsHandle Request(MmsHandle handle,uint32_t kinds=MMS_UI_REFRESH_LAYOUT|MMS_UI_REFRESH_REPAINT){
        const auto target=AnyTarget(handle);CHECK((target.supportedKinds&kinds)==kinds);
        return RequestExact(target.target,kinds);
    }
    void RejectSuspended(const MmsUiTargetRefV1& target){
        for(const uint32_t kinds:{uint32_t(MMS_UI_REFRESH_STATE),uint32_t(MMS_UI_REFRESH_REPAINT),uint32_t(MMS_UI_REFRESH_DATA),
            uint32_t(MMS_UI_REFRESH_STATE|MMS_UI_REFRESH_LAYOUT),uint32_t(MMS_UI_REFRESH_DATA|MMS_UI_REFRESH_LAYOUT)}){
            MmsUiRefreshRequestV1 request{};request.h=Header<MmsUiRefreshRequestV1>();request.target=target;
            request.scope=MMS_UI_REFRESH_SELF;request.kinds=kinds;MmsHandle unchanged=0xace5;
            CHECK(refresh.request(refresh.context,&request,&unchanged)==MMS_E_NOT_READY&&unchanged==0xace5);
        }
    }
    MmsUiLayoutSnapshotV1 Snapshot(){
        auto layout=data.registry.LayoutApi(data.owner);MmsUiLayoutSnapshotV1 value{};value.h=Header<MmsUiLayoutSnapshotV1>();
        CHECK(layout.acquireSnapshot(layout.context,&value)==MMS_OK);return value;
    }
    static MmsUiLayoutItemV1 Item(const MmsUiLayoutSnapshotV1& snapshot,const std::string& id){
        for(uint64_t i=0;i<snapshot.items.count;++i){const auto& item=*reinterpret_cast<const MmsUiLayoutItemV1*>(
            static_cast<const unsigned char*>(snapshot.items.data)+size_t(i*snapshot.items.stride));if(String(item.elementId)==id)return item;}
        CHECK(false);return {};
    }
    void Release(MmsUiLayoutSnapshotV1& value){auto layout=data.registry.LayoutApi(data.owner);layout.releaseSnapshot(layout.context,value.handle);value.handle=0;}
    void Pure(unsigned steps=0,int value=2){
        uint64_t revision=0;CHECK(history.Revision(revision)==MMS_OK&&revision==historyRevision+steps);
        CHECK(history.UndoCount()==steps&&!history.RedoCount()&&data.edits==steps&&data.value.value.signedValue==value);
        CHECK(data.finished==invokes[0]+invokes[1]&&observation.frame==23&&observation.dirty==bool(steps));
        CHECK(!data.registry.InCallback()&&!queue.ReadOnlyCause()&&!storage.Prepared()&&!storage.InCallback()&&storage.Records().empty());
    }
    void Bounds(){
        const int left=native->LeftAreaWidth(),right=native->RightAreaWidth();
        CHECK(left>=0&&right>=0&&left<=maximumWidth&&right<=maximumWidth);
        CHECK(left+right<std::max(1L,bounds.right-bounds.left));
        const auto a=In(window.value,Area(0)),b=In(window.value,Area(1));
        if(left){CHECK(a.left==bounds.left&&a.right-a.left==left&&a.top==bounds.top&&a.bottom==bounds.bottom);CHECK(IsWindowVisible(Area(0)));}
        if(right){CHECK(b.right==bounds.right&&b.right-b.left==right&&b.top==bounds.top&&b.bottom==bounds.bottom);CHECK(IsWindowVisible(Area(1)));}
        if(left&&right)CHECK(a.right<b.left);
        auto snapshot=Snapshot();using DpiFn=UINT(WINAPI*)(HWND);
        const auto getDpi=reinterpret_cast<DpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
        HDC dc=getDpi?nullptr:GetDC(window.value);const float scale=96.0f/(getDpi?getDpi(window.value):dc?GetDeviceCaps(dc,LOGPIXELSX):96);if(dc)ReleaseDC(window.value,dc);
        for(unsigned side=0;side<2;++side){const auto rect=In(window.value,Area(side));const auto item=Item(snapshot,roots[side]);
            CHECK(std::abs(item.rect[0]-rect.left*scale)<1&&std::abs(item.rect[1]-rect.top*scale)<1);
            CHECK(std::abs(item.rect[2]-(rect.right-rect.left)*scale)<1&&std::abs(item.rect[3]-(rect.bottom-rect.top)*scale)<1);}
        Release(snapshot);
    }
    void MoveGroup(unsigned side,const char* parent){
        MmsHandle transaction=0;CHECK(data.ui.beginLayout(data.ui.context,data.registry.Revision(),&transaction)==MMS_OK);
        MmsLayoutOpV1 op{};op.h=Header<MmsLayoutOpV1>();op.operation=MMS_LAYOUT_MOVE;op.elementId=Text(groups[side]);op.parentId=S(parent);
        op.expectedGeneration=data.registry.elements.at(groups[side]).desc.generation;
        CHECK(data.ui.applyLayout(data.ui.context,transaction,&op)==MMS_OK&&data.ui.commitLayout(data.ui.context,transaction)==MMS_OK);Arrange(true);
    }
};
void AnchorsCommandsAndSingleProperty(){
    Fixture f;auto surface=f.data.registry.SurfaceApi(f.data.owner);MmsSpan anchors{};MmsHandle lease=0;
    CHECK(surface.acquireAnchors(surface.context,&anchors,&lease)==MMS_OK);std::array<unsigned,2> found{};
    const auto kinds=(1ull<<MMS_UI_GROUP)|(1ull<<MMS_UI_BUTTON)|(1ull<<MMS_UI_SEPARATOR)|(1ull<<MMS_UI_INPUT);
    for(uint64_t i=0;i<anchors.count;++i){const auto& anchor=*reinterpret_cast<const MmsUiAnchorV1*>(static_cast<const unsigned char*>(anchors.data)+size_t(i*anchors.stride));
        for(unsigned side=0;side<2;++side)if(String(anchor.id)==roots[side]){++found[side];CHECK(anchor.allowedKinds==kinds&&!anchor.flags&&!anchor.reserved);}}
    surface.releaseAnchors(surface.context,lease);CHECK((found==std::array<unsigned,2>{1,1}));
    const auto record=f.data.registry.properties.at(f.data.propertyId);CHECK(f.data.registry.properties.size()==1&&record->handle==f.data.property);
    f.Bounds();
    for(unsigned side=0;side<2;++side){
        CHECK(String(f.data.registry.elements.at(f.inputIds[side]).desc.propertyId)==f.data.propertyId);
        CHECK(f.AnyTarget(f.inputs[side]).target.element==f.inputs[side]&&(f.AnyTarget(f.inputs[side]).supportedKinds&15)==15);
        CHECK(f.AnyTarget(f.separators[side]).supportedKinds==(MMS_UI_REFRESH_LAYOUT|MMS_UI_REFRESH_REPAINT));
        CHECK(f.AnyTarget(f.groupHandles[side]).supportedScopes==MMS_UI_REFRESH_SUBTREE);
        SendMessageW(f.buttonWindows[side],BM_CLICK,0,0);CHECK(f.invokes[side]==1);
    }
    CHECK(f.data.registry.properties.at(f.data.propertyId)==record);f.Pure();
    f.data.value=ui_input_broker_checks::Integer(9);
    for(unsigned side=0;side<2;++side){const auto ticket=f.Request(f.inputs[side],MMS_UI_REFRESH_DATA|MMS_UI_REFRESH_REPAINT);f.queue.Drain();f.Complete(ticket,3);CHECK(Caption(f.inputFields[side])==L"9");}
    f.Pure(0,9);
}
void ResizeDraftFocusAndGeometry(){
    for(unsigned active=0;active<2;++active){Fixture f;
        const auto fields=f.inputFields,wrappers=f.wrappers;const auto record=f.data.registry.properties.at(f.data.propertyId);
        const auto left=f.AnyTarget(f.inputs[0]),right=f.AnyTarget(f.inputs[1]);const auto ui=f.data.registry.Revision(),preferences=f.data.registry.PreferencesRevision();
        native_property_input_checks::Text(fields[active],L"-");SendMessageW(fields[active],EM_SETSEL,0,1);SendMessageW(fields[active],WM_IME_STARTCOMPOSITION,0,0);
        auto original=f.Snapshot();const auto old=Fixture::Item(original,f.inputIds[active]);
        CHECK(SetWindowPos(f.window.value,nullptr,0,0,620,600,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE));f.Arrange(false);f.Bounds();
        CHECK(f.native->LeftAreaWidth()<240||f.native->RightAreaWidth()<240);
        for(unsigned side=0;side<2;++side){const auto now=f.AnyTarget(f.inputs[side]);const auto& before=side?right:left;
            CHECK(now.ready&&now.target.instanceGeneration==before.target.instanceGeneration&&FindField(f.Area(side))==fields[side]&&GetParent(fields[side])==wrappers[side]);}
        auto narrow=f.Snapshot();const auto changed=Fixture::Item(narrow,f.inputIds[active]);
        CHECK(narrow.geometryRevision!=original.geometryRevision&&std::memcmp(old.rect,changed.rect,sizeof(old.rect))!=0);
        CHECK(std::memcmp(old.rect,Fixture::Item(original,f.inputIds[active]).rect,sizeof(old.rect))==0);f.Release(narrow);f.Release(original);
        DWORD begin=0,end=0;SendMessageW(fields[active],EM_GETSEL,reinterpret_cast<WPARAM>(&begin),reinterpret_cast<LPARAM>(&end));
        CHECK(Caption(fields[active])==L"-"&&GetFocus()==fields[active]&&begin==0&&end==1);
        Enter(fields[active]);CHECK(!f.data.edits); // An unfinished IME draft cannot submit through layout.
        CHECK(SetWindowPos(f.window.value,nullptr,0,0,1000,600,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE));f.Arrange(false);f.Bounds();
        CHECK(Caption(fields[active])==L"-"&&GetFocus()==fields[active]&&f.data.registry.properties.at(f.data.propertyId)==record);
        CHECK(f.data.registry.Revision()==ui&&f.data.registry.PreferencesRevision()==preferences);
        SendMessageW(fields[active],WM_IME_ENDCOMPOSITION,0,0);Escape(fields[active]);CHECK(Caption(fields[active])==L"2");f.Pure();
    }
}
void ReadOnlyMounts(){
    Fixture f(false);const auto fields=f.inputFields;
    for(unsigned side=0;side<2;++side){CHECK(GetWindowLongPtrW(fields[side],GWL_STYLE)&ES_READONLY);Enter(fields[side]);}
    f.Arrange(false,120);f.Bounds();
    for(unsigned side=0;side<2;++side)CHECK(FindField(f.Area(side))==fields[side]&&(GetWindowLongPtrW(fields[side],GWL_STYLE)&ES_READONLY));
    f.Pure();
}
void ScrollingAndIndependentColumns(){
    Fixture f;auto layout=f.data.registry.LayoutApi(f.data.owner);
    for(unsigned side=0;side<2;++side)for(unsigned row=0;row<24;++row){
        const auto id="example.input.side-filler-"+std::to_string(side)+"-"+std::to_string(row);
        MmsUiElementV1 element{};element.h=Header<MmsUiElementV1>();element.id=Text(id);element.ownerPlugin=Text(f.data.owner.id);
        element.parentId=Text(f.groups[side]);element.commandId=Text(f.commandIds[side]);element.label=S("Scroll row");element.kind=MMS_UI_BUTTON;
        element.capabilities=MMS_UI_LAYOUT|MMS_UI_MOVE|MMS_UI_HIDE;MmsHandle handle=0;
        CHECK(f.data.ui.registerElement(f.data.ui.context,&element,&handle)==MMS_OK);f.fillers.push_back(handle);
        MmsUiLayoutHintsV1 hints{};hints.h=Header<MmsUiLayoutHintsV1>();hints.preferred[0]=220;hints.preferred[1]=28;hints.shrink[0]=1;
        CHECK(layout.setDefaultHints(layout.context,Text(id),&hints)==MMS_OK);
    }
    f.Arrange(true);f.Bind();const auto fields=f.inputFields;const auto a=f.AnyTarget(f.inputs[0]),b=f.AnyTarget(f.inputs[1]);f.Bounds();
    for(unsigned side=0;side<2;++side){SCROLLINFO before{sizeof(before),SIF_ALL};CHECK(GetScrollInfo(f.Area(side),SB_VERT,&before));
        CHECK(before.nMax+1>int(before.nPage));const auto other=In(f.window.value,f.wrappers[1-side]),current=In(f.window.value,f.wrappers[side]);
        SendMessageW(f.Area(side),WM_VSCROLL,SB_LINEDOWN,0);SCROLLINFO after{sizeof(after),SIF_ALL};CHECK(GetScrollInfo(f.Area(side),SB_VERT,&after));
        CHECK(after.nPos>before.nPos&&In(f.window.value,f.wrappers[side]).top<current.top);const auto unmoved=In(f.window.value,f.wrappers[1-side]);CHECK(EqualRect(&other,&unmoved));
        CHECK(FindField(f.Area(side))==fields[side]&&f.AnyTarget(f.inputs[side]).target.instanceGeneration==(side?b:a).target.instanceGeneration);
        SendMessageW(f.Area(side),WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);SCROLLINFO restored{sizeof(restored),SIF_POS};CHECK(GetScrollInfo(f.Area(side),SB_VERT,&restored)&&restored.nPos==before.nPos);
    }
    f.Pure();
}
void EmptyGroupsAndRemoval(){
    {
        native_property_input_checks::Window window;UiRegistry registry;UiClient owner{&registry,"example.side.empty"};auto ui=registry.Api(owner);
        for(unsigned side=0;side<2;++side){MmsUiElementV1 group{};group.h=Header<MmsUiElementV1>();const auto id="example.side.empty.group-"+std::to_string(side);
            group.id=Text(id);group.ownerPlugin=Text(owner.id);group.parentId=S(roots[side]);group.kind=MMS_UI_GROUP;group.capabilities=MMS_UI_CONTAINER|MMS_UI_LAYOUT;
            MmsHandle handle=0;CHECK(ui.registerElement(ui.context,&group,&handle)==MMS_OK);}
        const auto sink=In(window.value,window.sink);NativeUi native(registry,window.value,Log{});RECT client{};CHECK(GetClientRect(window.value,&client));
        native.SetSideAreas({sidebar+9,25,client.right,client.bottom-192},240,true);native.Refresh();CHECK(native.Layout(sidebar+9,client.right-sidebar-9,180,true)==0);
        CHECK(!native.LeftAreaWidth()&&!native.RightAreaWidth()&&!IsWindowVisible(native.SideArea(false))&&!IsWindowVisible(native.SideArea(true)));
        const auto after=In(window.value,window.sink);CHECK(EqualRect(&sink,&after));
    }
    Fixture f;const auto old=f.inputFields;const auto pending=f.Request(f.inputs[0]);const auto sink=In(f.window.value,f.window.sink);
    f.data.registry.Retire(f.data.owner);f.Arrange(true);f.queue.Drain();
    CHECK(!IsWindow(old[0])&&!IsWindow(old[1])&&!f.native->LeftAreaWidth()&&!f.native->RightAreaWidth());
    CHECK(!IsWindowVisible(f.Area(0))&&!IsWindowVisible(f.Area(1))&&f.State(pending).status==MMS_UI_REFRESH_FAILED&&!f.State(pending).completedKinds);
    const auto after=In(f.window.value,f.window.sink);CHECK(EqualRect(&sink,&after));f.Pure();
}
void CrossMountKeepsRegistration(){
    for(unsigned side=0;side<2;++side){Fixture f;const auto record=f.data.registry.properties.at(f.data.propertyId);const auto element=f.inputs[side];
        HWND current=f.inputFields[side];const int control=GetDlgCtrlID(f.wrappers[side]);
        for(const auto* parent:{roots[1-side],"mms.toolbar.main.extensions","mms.properties.main.extensions",roots[side]}){
            const auto original=f.AnyTarget(element).target;native_property_input_checks::Text(current,L"7");const auto pending=f.Request(element,MMS_UI_REFRESH_DATA|MMS_UI_REFRESH_REPAINT);
            f.MoveGroup(side,parent);HWND host=std::strcmp(parent,roots[0])==0?f.Area(0):std::strcmp(parent,roots[1])==0?f.Area(1):
                std::strcmp(parent,"mms.toolbar.main.extensions")==0?f.native->Toolbar():f.native->PropertyArea();
            const HWND nextWrapper=GetDlgItem(host,control),next=GetDlgItem(nextWrapper,0x7201);CHECK(nextWrapper&&next&&GetParent(nextWrapper)==host&&Caption(next)==L"2");
            const auto replacement=f.AnyTarget(element).target;CHECK(replacement.element==original.element&&replacement.instanceGeneration!=original.instanceGeneration);
            CHECK(f.State(pending).status==MMS_UI_REFRESH_FAILED&&f.State(pending).result==MMS_E_STALE&&!f.State(pending).completedKinds);
            CHECK(GetFocus()!=current&&f.data.registry.properties.size()==1&&f.data.registry.properties.at(f.data.propertyId)==record&&f.data.registry.elements.at(f.inputIds[side]).handle==element);
            current=next;f.Pure();
        }
    }
}
void FailedReservationRecovery(){
    for(unsigned failure=0;failure!=3;++failure){Fixture f;const auto fields=f.inputFields;const auto target=f.AnyTarget(f.inputs[0]);auto held=f.Snapshot();const auto old=Fixture::Item(held,roots[0]);
        const auto synchronized=f.synchronizedReservations;
        const MmsResult error=failure==1?MMS_E_NOT_READY:MMS_E_FAILED;
        f.publishedResult=error;f.publishedThrows=failure==2;f.Configure(120);const auto ticket=f.Request(f.inputs[0]);f.queue.Drain();
        CHECK(f.State(ticket).status==MMS_UI_REFRESH_FAILED&&f.State(ticket).result==error&&!f.State(ticket).completedKinds&&f.publications==1);
        CHECK(f.reservations.leftWidth>0&&f.reservations.rightWidth>0&&f.reservations.leftWidth<=120&&f.reservations.rightWidth<=120);
        CHECK(f.synchronizedReservations.leftWidth==synchronized.leftWidth&&f.synchronizedReservations.rightWidth==synchronized.rightWidth&&
            f.synchronizedReservations.propertyHeight==synchronized.propertyHeight);
        const auto failedTarget=f.AnyTarget(f.inputs[0]);CHECK(!failedTarget.ready&&(failedTarget.supportedKinds&MMS_UI_REFRESH_LAYOUT)&&
            failedTarget.target.instanceGeneration==target.target.instanceGeneration);
        auto layout=f.data.registry.LayoutApi(f.data.owner);MmsUiLayoutSnapshotV1 unavailable{};unavailable.h=Header<MmsUiLayoutSnapshotV1>();unavailable.handle=733;
        CHECK(layout.acquireSnapshot(layout.context,&unavailable)==MMS_E_NOT_READY&&unavailable.handle==733);
        CHECK(std::memcmp(old.rect,Fixture::Item(held,roots[0]).rect,sizeof(old.rect))==0);
        const auto measured=f.reservations;
        CHECK(synchronized.leftWidth!=measured.leftWidth||synchronized.rightWidth!=measured.rightWidth);
        f.RejectSuspended(target.target);CHECK(f.publications==1);
        f.publishedResult=MMS_OK;f.publishedThrows=false;
        // No Configure, Arrange or Refresh repairs readiness here. The same
        // original object requests exactly the layout whose host sync failed.
        const uint32_t retryKinds=failure==0?uint32_t(MMS_UI_REFRESH_LAYOUT):uint32_t(MMS_UI_REFRESH_LAYOUT|MMS_UI_REFRESH_REPAINT);
        const auto retry=f.RequestExact(target.target,retryKinds);f.queue.Drain();f.Complete(retry,retryKinds);f.Bounds();
        CHECK(f.publications==2&&f.reservations.leftWidth==measured.leftWidth&&f.reservations.rightWidth==measured.rightWidth&&
            f.synchronizedReservations.leftWidth==f.native->LeftAreaWidth()&&f.synchronizedReservations.rightWidth==f.native->RightAreaWidth()&&
            f.synchronizedReservations.propertyHeight==f.native->PropertyAreaHeight());
        CHECK(f.AnyTarget(f.inputs[0]).ready&&f.AnyTarget(f.inputs[0]).target.instanceGeneration==target.target.instanceGeneration&&FindField(f.Area(0))==fields[0]&&FindField(f.Area(1))==fields[1]);
        const auto stable=f.RequestExact(target.target);f.queue.Drain();f.Complete(stable,MMS_UI_REFRESH_LAYOUT|MMS_UI_REFRESH_REPAINT);
        CHECK(f.publications==2); // A successful publication does not repeat.
        f.Release(held);f.Pure();
    }
}
void FailedReservationOrdinaryRecovery(){
    Fixture f;const auto fields=f.inputFields;const auto target=f.AnyTarget(f.inputs[0]);
    f.publishedResult=MMS_E_FAILED;f.Configure(120);const auto ticket=f.Request(f.inputs[0]);f.queue.Drain();
    CHECK(f.State(ticket).status==MMS_UI_REFRESH_FAILED&&f.publications==1&&!f.AnyTarget(f.inputs[0]).ready);
    f.publishedResult=MMS_OK;f.Arrange(false);f.Bounds();
    CHECK(f.AnyTarget(f.inputs[0]).ready&&f.AnyTarget(f.inputs[0]).target.instanceGeneration==target.target.instanceGeneration&&
        FindField(f.Area(0))==fields[0]&&FindField(f.Area(1))==fields[1]);
    // Ordinary layout remains a separate recovery path. A later genuine
    // extent change still publishes through the queued callback.
    f.Configure(200);const auto retry=f.Request(f.inputs[0]);f.queue.Drain();f.Complete(retry,MMS_UI_REFRESH_LAYOUT|MMS_UI_REFRESH_REPAINT);
    CHECK(f.publications==2&&f.reservations.leftWidth==f.native->LeftAreaWidth()&&f.reservations.rightWidth==f.native->RightAreaWidth()&&
        f.reservations.propertyHeight==f.native->PropertyAreaHeight());f.Pure();
}
void PendingReservationSurvivesInternalLayout(){
    for(unsigned side=0;side<2;++side){Fixture f;const auto fields=f.inputFields;const auto target=f.AnyTarget(f.inputs[side]);
        const auto synchronized=f.synchronizedReservations;auto held=f.Snapshot();const auto old=Fixture::Item(held,roots[side]);
        f.publishedResult=MMS_E_FAILED;f.Configure(120);const auto failed=f.Request(f.inputs[side]);f.queue.Drain();
        CHECK(f.State(failed).status==MMS_UI_REFRESH_FAILED&&f.State(failed).result==MMS_E_FAILED&&
            !f.State(failed).completedKinds&&f.publications==1&&!f.AnyTarget(f.inputs[side]).ready);
        f.publishedResult=MMS_OK;
        // A real mount message enters internal Layout(false), without the
        // public application handshake which consumes viewport reservations.
        SendMessageW(f.Area(side),WM_VSCROLL,SB_ENDSCROLL,0);
        CHECK(f.publications==1&&!f.AnyTarget(f.inputs[side]).ready&&
            f.synchronizedReservations.leftWidth==synchronized.leftWidth&&f.synchronizedReservations.rightWidth==synchronized.rightWidth);
        auto layout=f.data.registry.LayoutApi(f.data.owner);MmsUiLayoutSnapshotV1 unavailable{};
        unavailable.h=Header<MmsUiLayoutSnapshotV1>();unavailable.handle=0x5117;
        CHECK(layout.acquireSnapshot(layout.context,&unavailable)==MMS_E_NOT_READY&&unavailable.handle==0x5117);
        CHECK(std::memcmp(old.rect,Fixture::Item(held,roots[side]).rect,sizeof(old.rect))==0);
        f.RejectSuspended(target.target);
        const auto retry=f.RequestExact(target.target,MMS_UI_REFRESH_LAYOUT);f.queue.Drain();f.Complete(retry,MMS_UI_REFRESH_LAYOUT);f.Bounds();
        CHECK(f.publications==2&&f.AnyTarget(f.inputs[side]).ready&&
            f.AnyTarget(f.inputs[side]).target.instanceGeneration==target.target.instanceGeneration&&
            f.synchronizedReservations.leftWidth==f.native->LeftAreaWidth()&&f.synchronizedReservations.rightWidth==f.native->RightAreaWidth()&&
            FindField(f.Area(0))==fields[0]&&FindField(f.Area(1))==fields[1]);
        f.Release(held);f.Pure();
    }
}
struct ConstrainSide {
    HWND window=nullptr;bool armed=false;unsigned changes=0;
    static LRESULT CALLBACK Proc(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR context){
        auto& self=*reinterpret_cast<ConstrainSide*>(context);
        if(self.armed&&message==WM_WINDOWPOSCHANGING&&lp){auto& position=*reinterpret_cast<WINDOWPOS*>(lp);
            if(!(position.flags&SWP_NOSIZE)&&position.cx>7){position.cx-=7;++self.changes;}}
        return DefSubclassProc(window,message,wp,lp);
    }
    explicit ConstrainSide(HWND value):window(value){CHECK(SetWindowSubclass(window,Proc,0x53a1,reinterpret_cast<DWORD_PTR>(this)));}
    ~ConstrainSide(){if(IsWindow(window))RemoveWindowSubclass(window,Proc,0x53a1);}
};
void NativeGeometryFailureRecovery(){
    for(unsigned side=0;side<2;++side){Fixture f;ConstrainSide fault(f.Area(side));const auto fields=f.inputFields;
        const auto target=f.AnyTarget(f.inputs[side]);auto held=f.Snapshot();const auto old=Fixture::Item(held,roots[side]);
        const auto synchronized=f.synchronizedReservations;
        fault.armed=true;f.Configure(160);const auto ticket=f.Request(f.inputs[side]);f.queue.Drain();
        CHECK(fault.changes&&f.State(ticket).status==MMS_UI_REFRESH_FAILED&&!f.State(ticket).completedKinds&&!f.AnyTarget(f.inputs[side]).ready&&!f.publications);
        CHECK(f.synchronizedReservations.leftWidth==synchronized.leftWidth&&f.synchronizedReservations.rightWidth==synchronized.rightWidth);
        auto layout=f.data.registry.LayoutApi(f.data.owner);MmsUiLayoutSnapshotV1 pending{};pending.h=Header<MmsUiLayoutSnapshotV1>();pending.handle=813;
        CHECK(layout.acquireSnapshot(layout.context,&pending)==MMS_E_NOT_READY&&pending.handle==813);
        CHECK(std::memcmp(old.rect,Fixture::Item(held,roots[side]).rect,sizeof(old.rect))==0);
        const int measuredLeft=f.native->LeftAreaWidth(),measuredRight=f.native->RightAreaWidth();const auto changes=fault.changes;
        f.RejectSuspended(target.target);CHECK(!f.publications);
        fault.armed=false;
        const auto retry=f.RequestExact(target.target);f.queue.Drain();f.Complete(retry,MMS_UI_REFRESH_LAYOUT|MMS_UI_REFRESH_REPAINT);f.Bounds();
        CHECK(fault.changes==changes&&f.publications==1&&f.native->LeftAreaWidth()==measuredLeft&&f.native->RightAreaWidth()==measuredRight&&
            f.synchronizedReservations.leftWidth==measuredLeft&&f.synchronizedReservations.rightWidth==measuredRight&&
            f.synchronizedReservations.propertyHeight==f.native->PropertyAreaHeight());
        CHECK(f.AnyTarget(f.inputs[side]).ready&&f.AnyTarget(f.inputs[side]).target.instanceGeneration==target.target.instanceGeneration&&
            FindField(f.Area(0))==fields[0]&&FindField(f.Area(1))==fields[1]);
        const auto stable=f.RequestExact(target.target);f.queue.Drain();f.Complete(stable,MMS_UI_REFRESH_LAYOUT|MMS_UI_REFRESH_REPAINT);CHECK(f.publications==1);
        f.Release(held);f.Pure();
    }
}
void ViewportReservations(){
    using namespace mikumikustudio;Fixture f;RECT client{};CHECK(GetClientRect(f.window.value,&client));FrameViewportGeometry before,reserved,restored;
    CHECK(CalculateFrameViewport(client,sidebar,0,FrameViewportMode::Docked,1920,1080,before));
    CHECK(CalculateFrameViewport(client,sidebar,0,FrameViewportMode::Docked,1920,1080,reserved,0,f.native->LeftAreaWidth(),f.native->RightAreaWidth()));
    CHECK(reserved.available.left==before.available.left+f.native->LeftAreaWidth()&&reserved.available.right==before.available.right-f.native->RightAreaWidth());
    CHECK(FrameRectWithin(reserved.scene,reserved.available));const auto panels=MakeViewportPanelLayout(reserved.available,reserved.scene,true,true);
    CHECK(FrameRectWithin(panels.overlay,reserved.presentationBounds));
    f.data.registry.Retire(f.data.owner);f.Arrange(true);
    CHECK(CalculateFrameViewport(client,sidebar,0,FrameViewportMode::Docked,1920,1080,restored,0,f.native->LeftAreaWidth(),f.native->RightAreaWidth()));
    CHECK(EqualRect(&before.available,&restored.available)&&EqualRect(&before.scene,&restored.scene)&&EqualRect(&before.presentationBounds,&restored.presentationBounds));
    for(const auto mode:{FrameViewportMode::Floating,FrameViewportMode::Fullscreen}){CHECK(CalculateFrameViewport(client,sidebar,0,mode,1920,1080,before));
        CHECK(CalculateFrameViewport(client,sidebar,0,mode,1920,1080,reserved,0,200,200));CHECK(EqualRect(&before.available,&reserved.available)&&EqualRect(&before.scene,&reserved.scene));}
    f.Pure();
}
} // namespace side_mount_checks
static void SideMountChecks(){
    std::cout<<"SIDE_MOUNT anchors/commands/property\n";side_mount_checks::AnchorsCommandsAndSingleProperty();
    std::cout<<"SIDE_MOUNT resize/draft/focus/geometry\n";side_mount_checks::ResizeDraftFocusAndGeometry();
    std::cout<<"SIDE_MOUNT read-only properties\n";side_mount_checks::ReadOnlyMounts();
    std::cout<<"SIDE_MOUNT independent scrolling\n";side_mount_checks::ScrollingAndIndependentColumns();
    std::cout<<"SIDE_MOUNT empty groups/removal\n";side_mount_checks::EmptyGroupsAndRemoval();
    std::cout<<"SIDE_MOUNT cross-mount identity\n";side_mount_checks::CrossMountKeepsRegistration();
    std::cout<<"SIDE_MOUNT reservation failure/recovery\n";side_mount_checks::FailedReservationRecovery();
    std::cout<<"SIDE_MOUNT ordinary reservation recovery\n";side_mount_checks::FailedReservationOrdinaryRecovery();
    std::cout<<"SIDE_MOUNT internal layout preserves pending publication\n";side_mount_checks::PendingReservationSurvivesInternalLayout();
    std::cout<<"SIDE_MOUNT native geometry failure/recovery\n";side_mount_checks::NativeGeometryFailureRecovery();
    std::cout<<"SIDE_MOUNT viewport reservation\n";side_mount_checks::ViewportReservations();
    std::cout<<"Side mounts original objects/native commands/resize/draft/scroll/identity/recovery/viewport PASS\n";
}
