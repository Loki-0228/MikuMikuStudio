// Include after ui_refresh_subtree_checks.inc; uses its real HWND/data fixture.
#include "mikumikustudio/window_dpi_scope.hpp"
namespace ui_refresh_subtree_rect_checks {
using Fixture = ui_refresh_subtree_checks::Fixture;
static RefreshPresentation& Presentation(Fixture& x, MmsHandle element) {
    const auto found = std::find_if(x.presentations.begin(), x.presentations.end(),
        [&](const auto& p) { return p.element == element; });
    CHECK(found != x.presentations.end()); return *found;
}
static MmsUiRefreshRequestV1 Request(Fixture& x, const MmsUiDirtyRectV1* rects, size_t count,
    uint32_t kinds = MMS_UI_REFRESH_REPAINT, MmsHandle root = 0) {
    auto r = x.Request(x.Target(root ? root : x.group), rects, count);
    r.scope = MMS_UI_REFRESH_SUBTREE; r.kinds = kinds; return r;
}
static void UnrelatedClean(Fixture& x) {
    CHECK(refresh_checks::Empty(x.parent) && refresh_checks::Empty(x.panel));
}
static void ExactClippingAndNestedRoots() {
    Fixture x; MmsUiDirtyRectV1 rect{280,10,60,30};
    auto ticket = x.Submit(Request(x, &rect, 1)); x.queue.Drain(); x.Done(ticket, MMS_UI_REFRESH_REPAINT);
    refresh_checks::Region(x.button, {{280,10,300,40}});
    refresh_checks::Region(x.sibling, {{0,10,20,40}});
    refresh_checks::Region(x.separatorWindow, {{0,10,2,40}}); UnrelatedClean(x);
    x.CleanAll(); rect = {-10,-20,20,30};
    ticket = x.Submit(Request(x, &rect, 1)); x.queue.Drain(); x.Done(ticket, MMS_UI_REFRESH_REPAINT);
    refresh_checks::Region(x.button, {{0,0,10,10}});
    CHECK(refresh_checks::Empty(x.sibling) && refresh_checks::Empty(x.separatorWindow)); UnrelatedClean(x);
    x.CleanAll();
    const MmsUiDirtyRectV1 empty[]{{600,0,10,10},{0,0,0,10},{0,0,10,0},{-5,0,2,2}};
    ticket = x.Submit(Request(x, empty, std::size(empty))); x.queue.Drain(); x.Done(ticket, MMS_UI_REFRESH_REPAINT);
    CHECK(refresh_checks::Empty(x.button) && refresh_checks::Empty(x.sibling) && refresh_checks::Empty(x.separatorWindow)); UnrelatedClean(x);
    auto& nested = Presentation(x, x.nested); nested.x = 345; nested.width = 195; x.queue.SyncTargets(x.presentations);
    rect = {0,10,30,30}; ticket = x.Submit(Request(x, &rect, 1, MMS_UI_REFRESH_REPAINT, x.nested));
    x.queue.Drain(); x.Done(ticket, MMS_UI_REFRESH_REPAINT);
    refresh_checks::Region(x.sibling, {{0,10,15,40}}); refresh_checks::Region(x.separatorWindow, {{0,10,2,40}});
    CHECK(refresh_checks::Empty(x.button)); UnrelatedClean(x);
    x.CleanAll(); rect = {2,3,8,7}; ticket = x.Submit(Request(x, &rect, 1, MMS_UI_REFRESH_REPAINT, x.panelElement));
    x.queue.Drain(); x.Done(ticket, MMS_UI_REFRESH_REPAINT); refresh_checks::Region(x.panel, {{2,3,10,10}});
    CHECK(refresh_checks::Empty(x.parent) && refresh_checks::Empty(x.button) && refresh_checks::Empty(x.sibling));
}
static void UnionCancellationAndBudgets() {
    Fixture x; MmsUiDirtyRectV1 a{10,10,8,9}, b{30,40,4,7}, cancelled{100,40,20,20};
    auto first = x.Submit(Request(x,&a,1)), second = x.Submit(Request(x,&b,1)), third = x.Submit(Request(x,&cancelled,1));
    CHECK(x.api.cancel(x.api.context,third)==MMS_OK); x.queue.Drain();
    x.Done(first,MMS_UI_REFRESH_REPAINT); x.Done(second,MMS_UI_REFRESH_REPAINT);
    CHECK(x.State(third).status==MMS_UI_REFRESH_CANCELLED);
    refresh_checks::Region(x.button,{{10,10,18,19},{30,40,34,47}});
    CHECK(refresh_checks::Empty(x.sibling)&&refresh_checks::Empty(x.separatorWindow)); UnrelatedClean(x);
    x.CleanAll(); first=x.Submit(Request(x,&a,1)); second=x.Submit(Request(x,nullptr,0));
    x.queue.Drain(); x.Done(first,MMS_UI_REFRESH_REPAINT); x.Done(second,MMS_UI_REFRESH_REPAINT);
    refresh_checks::Region(x.button,{{0,0,300,120}}); refresh_checks::Region(x.sibling,{{0,0,180,120}});
    refresh_checks::Region(x.separatorWindow,{{0,0,2,100}}); UnrelatedClean(x); x.CleanAll();
    MmsUiDirtyRectV1 rectangles[33]{}; auto bad=Request(x,rectangles,33); MmsHandle unchanged=983;
    CHECK(x.api.request(x.api.context,&bad,&unchanged)==MMS_E_BUSY&&unchanged==983);
    bad=Request(x,&a,1); bad.expectedGeometryRevision=0;
    CHECK(x.api.request(x.api.context,&bad,&unchanged)==MMS_E_ARGUMENT&&unchanged==983);
    a.width=-1; bad=Request(x,&a,1);
    CHECK(x.api.request(x.api.context,&bad,&unchanged)==MMS_E_ARGUMENT&&unchanged==983);
    a.width=std::numeric_limits<float>::infinity();
    CHECK(x.api.request(x.api.context,&bad,&unchanged)==MMS_E_ARGUMENT&&unchanged==983&&!x.queue.Pending());
    for(auto& r:rectangles)r={4,6,2,3};
    first=x.Submit(Request(x,rectangles,32));x.queue.Drain();x.Done(first,MMS_UI_REFRESH_REPAINT);
    refresh_checks::Region(x.button,{{4,6,6,9}});CHECK(refresh_checks::Empty(x.sibling));UnrelatedClean(x);x.CleanAll();
    auto foreign=x.Button("example.foreign.rect-child");foreign.ownerPlugin=S("example.foreign");foreign.parentId=S("example.refresh.tree");
    auto other=x.registry.Api(x.foreign);MmsHandle element=0;CHECK(other.registerElement(other.context,&foreign,&element)==MMS_OK);
    bad=Request(x,rectangles,1);
    CHECK(x.api.request(x.api.context,&bad,&unchanged)==MMS_E_ARGUMENT&&unchanged==983&&!x.queue.Pending());
    CHECK(refresh_checks::Empty(x.button)&&refresh_checks::Empty(x.sibling));UnrelatedClean(x);
}
static void MixedDpiAndExactOrigins() {
    Fixture x;
    for(auto& p:x.presentations){
        const auto pixelWidth=p.width,pixelHeight=p.height;
        p.coordinateDpi=168;p.coordinateWindow=x.parent;p.pixelOrigin=true;
        p.pixelX=LONG(std::lround(p.x));p.pixelY=LONG(std::lround(p.y));
        p.x=p.pixelX*96.0f/p.coordinateDpi;p.y=p.pixelY*96.0f/p.coordinateDpi;
        p.dpi=p.element==x.element?192:p.element==x.second?120:p.element==x.separator?144:168;
        p.width=pixelWidth*96.0f/p.dpi;p.height=pixelHeight*96.0f/p.dpi;
    }
    x.queue.SyncTargets(x.presentations);
    MmsUiDirtyRectV1 rect{4,8,16,12}; auto ticket=x.Submit(Request(x,&rect,1));
    x.queue.Drain();x.Done(ticket,MMS_UI_REFRESH_REPAINT);refresh_checks::Region(x.button,{{7,14,35,35}});
    CHECK(refresh_checks::Empty(x.sibling)&&refresh_checks::Empty(x.separatorWindow));UnrelatedClean(x);x.CleanAll();
    rect={.25f,.5f,1,1.25f};ticket=x.Submit(Request(x,&rect,1));x.queue.Drain();x.Done(ticket,MMS_UI_REFRESH_REPAINT);
    refresh_checks::Region(x.button,{{0,0,3,4}});UnrelatedClean(x);x.CleanAll();
    // A backend must publish a single common frame for a root-local request.
    Presentation(x,x.second).coordinateWindow=x.sibling;x.queue.SyncTargets(x.presentations);
    auto bad=Request(x,&rect,1);MmsHandle unchanged=753;
    CHECK(x.api.request(x.api.context,&bad,&unchanged)==MMS_E_NOT_READY&&unchanged==753&&!x.queue.Pending());
}
static void BackgroundCaptureAndInvalidation() {
    Fixture x;MmsUiDirtyRectV1 rect{12,14,8,6};auto request=Request(x,&rect,1);MmsHandle ticket=0;
    MmsResult result=MMS_E_FAILED;
    std::thread worker([&]{result=x.api.request(x.api.context,&request,&ticket);});worker.join();
    CHECK(result==MMS_OK&&ticket&&x.stateReads==0&&!x.layouts&&!x.prepares[0]&&!x.prepares[1]);x.tickets.push_back(ticket);
    rect={400,0,100,100};x.queue.Drain();x.Done(ticket,MMS_UI_REFRESH_REPAINT);
    refresh_checks::Region(x.button,{{12,14,20,20}});CHECK(refresh_checks::Empty(x.sibling));UnrelatedClean(x);
    for(unsigned change=0;change<8;++change){
        Fixture y;rect={2,3,4,5};ticket=y.Submit(Request(y,&rect,1));
        if(change==0){Presentation(y,y.second).x+=1;y.queue.SyncTargets(y.presentations);}
        else if(change==1){Presentation(y,y.second).dpi=168;y.queue.SyncTargets(y.presentations);}
        else if(change==2){auto& p=Presentation(y,y.second);p.pixelOrigin=true;p.pixelX=361;p.pixelY=50;y.queue.SyncTargets(y.presentations);}
        else if(change==3){Presentation(y,y.group).width-=1;y.queue.SyncTargets(y.presentations);}
        else if(change==4)y.Reparent("example.refresh.sibling","mms.toolbar.main.extensions");
        else if(change==5)y.queue.AdvanceProjectEpoch();
        else if(change==6)y.queue.SuspendTargets();
        else y.queue.Retire(y.client);
        y.queue.Drain();const auto state=y.State(ticket);
        CHECK(state.status==(change==7?MMS_UI_REFRESH_CANCELLED:MMS_UI_REFRESH_FAILED));
        CHECK(state.result==(change==7?MMS_E_CANCELLED:change==6?MMS_E_NOT_READY:MMS_E_STALE)&&!state.completedKinds);
        CHECK(refresh_checks::Empty(y.button)&&refresh_checks::Empty(y.sibling)&&refresh_checks::Empty(y.separatorWindow));UnrelatedClean(y);
    }
}
static void FullPipelineAndChangedLayout() {
    Fixture x;MmsUiDirtyRectV1 rect{3,5,7,9};
    x.onState=[&]{CHECK(x.Display(0)==10&&x.Display(1)==11);x.stages.push_back(2);};
    auto ticket=x.Submit(Request(x,&rect,1,15));x.queue.Drain();x.Done(ticket,15);
    CHECK(x.stages==std::vector<unsigned>({1,1,2,2,3})&&x.layouts==1);
    refresh_checks::Region(x.button,{{3,5,10,14}});CHECK(refresh_checks::Empty(x.sibling)&&refresh_checks::Empty(x.separatorWindow));UnrelatedClean(x);
    x.CleanAll();x.Publish(0,24);x.Publish(1,25);x.onState=[&]{CHECK(x.Display(0)==24&&x.Display(1)==25);};
    x.queue.SetLayout([&](MmsHandle root)->MmsResult{
        CHECK(root==x.group&&x.registry.InCallback()&&x.queue.ReadOnlyCause(x.client));
        Presentation(x,x.second).x+=1;x.queue.SyncTargets(x.presentations);return MMS_OK;
    });
    ticket=x.Submit(Request(x,&rect,1,15));x.queue.Drain();const auto state=x.State(ticket);
    CHECK(state.status==MMS_UI_REFRESH_FAILED&&state.result==MMS_E_STALE&&state.completedKinds==14);
    CHECK(x.Display(0)==24&&x.Display(1)==25&&refresh_checks::Empty(x.button)&&refresh_checks::Empty(x.sibling)&&refresh_checks::Empty(x.separatorWindow));UnrelatedClean(x);
    // DATA preparation failure retains each previously complete display and
    // stops STATE/LAYOUT/REPAINT; the rectangle does not change that contract.
    x.onState={};x.Publish(0,30);x.Publish(1,31);const auto reads=x.stateReads;
    x.onPrepare=[](unsigned)->MmsResult{return MMS_E_FAILED;};
    ticket=x.Submit(Request(x,&rect,1,15));x.queue.Drain();const auto failed=x.State(ticket);
    CHECK(failed.status==MMS_UI_REFRESH_FAILED&&failed.result==MMS_E_FAILED&&!failed.completedKinds&&x.stateReads==reads);
    CHECK(x.Display(0)==24&&x.Display(1)==25&&refresh_checks::Empty(x.button)&&refresh_checks::Empty(x.sibling));UnrelatedClean(x);
}
static void NativePublicationAndSdk() {
    layout_refresh_checks::Fixture x(400);
    const mikumikustudio::WindowDpiScope dpiContext(x.window.value);
    MmsUiElementV1 group{};group.h=Header<MmsUiElementV1>();group.id=S("example.layout.rect-group");
    group.ownerPlugin=Text(x.owner.id);group.parentId=S("mms.toolbar.main.extensions");
    group.kind=MMS_UI_GROUP;group.capabilities=MMS_UI_CONTAINER|MMS_UI_LAYOUT|MMS_UI_MOVE;
    MmsHandle handle=0;CHECK(x.ui.registerElement(x.ui.context,&group,&handle)==MMS_OK);
    for(const char* id:{"example.layout.first","example.layout.second"}){
        auto e=x.registry.elements.at(id).desc;e.parentId=group.id;
        CHECK(x.ui.updateElement(x.ui.context,x.registry.elements.at(id).handle,&e)==MMS_OK);
    }
    x.native->Refresh();CHECK(x.native->Layout(10,400,layout_refresh_checks::Fixture::heightLimit,true)>0);
    MmsUiRefreshTargetV1 target{};target.h=Header<MmsUiRefreshTargetV1>();CHECK(x.api.getTarget(x.api.context,handle,&target)==MMS_OK);
    const auto snapshot=x.Snapshot();const auto box=layout_refresh_checks::Fixture::Item(snapshot,"example.layout.rect-group");
    // Infer the first button's actual client offset in this logical root from
    // the native HWND. The test does not use refresh-private target metadata.
    POINT point{};SetLastError(ERROR_SUCCESS);CHECK(MapWindowPoints(x.controls[0],x.window.value,&point,1)||GetLastError()==ERROR_SUCCESS);
    using DpiFn=UINT(WINAPI*)(HWND);auto getDpi=reinterpret_cast<DpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
    HDC dc=GetDC(x.window.value);CHECK(dc);const auto dpi=getDpi?getDpi(x.window.value):UINT(GetDeviceCaps(dc,LOGPIXELSX));ReleaseDC(x.window.value,dc);
    CHECK(dpi>=48&&dpi<=960);
    const float rootX=float(std::lround(double(box.rect[0])*dpi/96)),rootY=float(std::lround(double(box.rect[1])*dpi/96));
    MmsUiDirtyRectV1 rect{(point.x-rootX+4)*96.0f/dpi,(point.y-rootY+4)*96.0f/dpi,8*96.0f/dpi,6*96.0f/dpi};
    MmsHostV1 host{};host.h=Header<MmsHostV1>();host.context=&x.api;
    host.queryService=[](void* p,MmsString name,uint32_t major,uint32_t minor,uint32_t size,void* out)->MmsResult{
        if(String(name)!="mms.ui.refresh"||major!=1||minor||size!=sizeof(MmsUiRefreshApiV1))return MMS_E_VERSION;
        std::memcpy(out,p,size);return MMS_OK;
    };
    mms::sdk::RefreshServices service;CHECK(mms::sdk::RefreshServices::Query(host,service)==MMS_OK);
    UpdateWindow(x.window.value);for(const auto control:x.controls){UpdateWindow(control);ValidateRect(control,nullptr);}
    ValidateRect(x.window.value,nullptr);mms::sdk::RefreshTicket ticket;
    CHECK(service.RepaintSubtreeRects(target,&rect,1,ticket)==MMS_OK);x.queue.Drain();MmsUiRefreshStateV1 state{};
    CHECK(ticket.Query(state)==MMS_OK&&state.status==MMS_UI_REFRESH_COMPLETED&&state.completedKinds==MMS_UI_REFRESH_REPAINT);
    // Outward rounding tolerates only the submitted fractional-DIP boundary;
    // every pixel outside this small box must remain clean.
    const RECT expected{LONG(std::floor(double(rootX-point.x)+double(rect.x)*dpi/96)),LONG(std::floor(double(rootY-point.y)+double(rect.y)*dpi/96)),
        LONG(std::ceil(double(rootX-point.x)+(double(rect.x)+rect.width)*dpi/96)),LONG(std::ceil(double(rootY-point.y)+(double(rect.y)+rect.height)*dpi/96))};
    refresh_checks::Region(x.controls[0],{expected});CHECK(refresh_checks::Empty(x.controls[1])&&refresh_checks::Empty(x.controls[2])&&refresh_checks::Empty(x.window.value));
    CHECK(ticket.Release()==MMS_OK);
}
} // namespace ui_refresh_subtree_rect_checks
static void UiRefreshSubtreeRectChecks() {
    ui_refresh_subtree_rect_checks::ExactClippingAndNestedRoots();
    ui_refresh_subtree_rect_checks::UnionCancellationAndBudgets();
    ui_refresh_subtree_rect_checks::MixedDpiAndExactOrigins();
    ui_refresh_subtree_rect_checks::BackgroundCaptureAndInvalidation();
    ui_refresh_subtree_rect_checks::FullPipelineAndChangedLayout();
    ui_refresh_subtree_rect_checks::NativePublicationAndSdk();
}
