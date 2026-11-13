#include "../../src/plugins/ui_registry.hpp"
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
using namespace mikumikustudio::plugins;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);}} while(false)
MmsString S(const char* s){return {s,std::strlen(s)};}
template<class T> MmsSpan Span(const T* p,size_t n=1){return {p,n,sizeof(T),2};}
template<class T> T Ex(){T v{};v.h=Header<T>();v.h.minor=2;return v;}
template<class T> T Sentinel(){T v;std::memset(&v,0x5a,sizeof(v));v.h=Header<T>();return v;}
template<class T> bool Same(const T& a,const T& b){return !std::memcmp(&a,&b,sizeof(T));}
MmsUiLayoutSessionTargetV1_2 Target(MmsHandle element,MmsHandle parent,uint32_t fields=PreviewParent){
    auto v=Ex<MmsUiLayoutSessionTargetV1_2>();v.element=element;v.parent=parent;v.fields=fields;return v;
}
MmsUiLayoutSessionValueV1_2 Value(MmsHandle element,MmsHandle parent,uint32_t fields=PreviewParent){
    auto v=Ex<MmsUiLayoutSessionValueV1_2>();v.element=element;v.parent=parent;v.fields=fields;return v;
}
struct Fixture {
    UiRegistry registry;
    UiClient owner{&registry,"p"},other{&registry,"q"};
    MmsUiApiV1 ui=registry.Api(owner);
    MmsUiLayoutSessionApiV1_2 api=registry.LayoutSessionApi12(owner),foreign=registry.LayoutSessionApi12(other);
    MmsUiLayoutSessionApiV1 old=registry.LayoutSessionApi(owner,0);
    MmsHandle a=0,b=0,item=0,foreignGroup=0,limited=0,root=0,otherRoot=0,panel=0;
    Fixture(){
        a=Element(owner,"p.a","mms.toolbar.main.extensions",MMS_UI_GROUP);
        b=Element(owner,"p.b","mms.toolbar.main.extensions",MMS_UI_GROUP);
        item=Element(owner,"p.item","p.a");
        foreignGroup=Element(other,"q.group","mms.toolbar.main.extensions",MMS_UI_GROUP);
        limited=Element(owner,"p.onlymove","p.a",MMS_UI_SEPARATOR,MMS_UI_MOVE);
        root=registry.elements.at("mms.toolbar.main.extensions").handle;
        otherRoot=registry.elements.at("mms.properties.main.extensions").handle;
        auto surfaces=registry.SurfaceApi(owner);MmsUiSurfaceDescV1 surface{};surface.h=Header<MmsUiSurfaceDescV1>();surface.id=S("p.surface");
        surface.createView=[](void*,const MmsUiMountV1*,void**,uintptr_t*)->MmsResult{CHECK(false);return MMS_E_FAILED;};
        surface.resizeView=[](void*,const MmsUiRectV1*,uint32_t){CHECK(false);};surface.showView=[](void*,uint32_t){CHECK(false);};
        surface.focusView=[](void*)->MmsResult{CHECK(false);return MMS_E_FAILED;};surface.destroyView=[](void*){CHECK(false);};
        MmsHandle handle=0;CHECK(surfaces.registerSurface(surfaces.context,&surface,&handle)==MMS_OK);
        MmsUiElementV1 desc{};desc.h=Header<MmsUiElementV1>();desc.id=S("p.panel");desc.ownerPlugin=S("p");
        desc.parentId=S("mms.workspace.tabs.extensions");desc.kind=MMS_UI_PANEL;desc.surfaceId=S("p.surface");
        desc.capabilities=MMS_UI_MOVE|MMS_UI_HIDE|MMS_UI_RESIZE|MMS_UI_LAYOUT;CHECK(ui.registerElement(ui.context,&desc,&panel)==MMS_OK);
    }
    MmsHandle Element(UiClient& c,const char* id,const char* parent,uint32_t kind=MMS_UI_SEPARATOR,
                      uint32_t caps=MMS_UI_MOVE|MMS_UI_HIDE|MMS_UI_RESIZE|MMS_UI_LAYOUT){
        auto table=registry.Api(c);MmsUiElementV1 e{};e.h=Header<MmsUiElementV1>();e.id=S(id);e.parentId=S(parent);
        e.ownerPlugin=Text(c.id);e.kind=kind;e.capabilities=caps|(kind==MMS_UI_GROUP?MMS_UI_CONTAINER:0);
        MmsHandle h=0;CHECK(table.registerElement(table.context,&e,&h)==MMS_OK);return h;
    }
    MmsHandle Begin(const MmsUiLayoutSessionTargetV1_2& target){MmsHandle h=0;CHECK(api.beginEx(api.context,Span(&target),&h)==MMS_OK&&h);return h;}
    MmsUiLayoutSessionInfoV1 Info(MmsHandle h){MmsUiLayoutSessionInfoV1 i{};i.h=Header<MmsUiLayoutSessionInfoV1>();CHECK(old.query(old.context,h,&i)==MMS_OK);return i;}
    std::string Parent(){return String(registry.EffectiveParent(registry.elements.at("p.item")));}
    void MoveSaved(){
        MmsHandle tx=0;CHECK(ui.beginLayout(ui.context,registry.Revision(),&tx)==MMS_OK);
        MmsLayoutOpV1 op{};op.h=Header<MmsLayoutOpV1>();op.operation=MMS_LAYOUT_MOVE;op.elementId=S("p.item");op.parentId=S("p.a");
        op.expectedGeneration=registry.elements.at("p.item").desc.generation;
        CHECK(ui.applyLayout(ui.context,tx,&op)==MMS_OK&&ui.commitLayout(ui.context,tx)==MMS_OK);
    }
};
void CapabilitiesAndIdentity(){
    Fixture f;CHECK(f.api.h.size==128&&f.api.h.major==1&&f.api.h.minor==2&&f.old.h.size==88&&!f.old.h.minor);
    auto caps=Sentinel<MmsUiLayoutSessionCapsV1>();CHECK(f.api.queryCapabilitiesEx(f.api.context,&caps)==MMS_OK&&caps.fields==15&&caps.mounts==31&&!caps.h.minor);
    CHECK(f.api.queryCapabilities(f.api.context,&caps)==MMS_OK&&caps.fields==7&&caps.mounts==31);
    CHECK(f.old.queryCapabilities(f.old.context,&caps)==MMS_OK&&caps.fields==7&&caps.mounts==15);
    auto info=Sentinel<MmsUiLayoutSessionTargetInfoV1>();CHECK(f.api.queryTargetEx(f.api.context,f.item,&info)==MMS_OK&&info.fields==15&&info.mount==1);
    CHECK(f.api.queryTargetEx(f.api.context,f.limited,&info)==MMS_OK&&info.fields==PreviewParent);
    const auto before=info;CHECK(f.api.queryTarget(f.api.context,f.limited,&info)==MMS_E_UNSUPPORTED&&Same(before,info));
    CHECK(f.api.queryTargetEx(f.api.context,f.panel,&info)==MMS_OK&&info.fields==7&&info.mount==16);
    const auto saved=f.registry.ExportPreferences();const auto target=Target(f.item,f.b);const auto h=f.Begin(target);
    CHECK(f.Parent()=="p.a"&&f.registry.ExportPreferences()==saved);
    auto claim=Ex<MmsUiLayoutSessionTargetV1_2>();CHECK(f.api.queryClaimEx(f.api.context,h,1,0,&claim)==MMS_OK&&claim.parent==f.b&&claim.fields==PreviewParent);
    auto oldClaim=Sentinel<MmsUiLayoutSessionTargetV1>();const auto untouched=oldClaim;
    CHECK(f.old.queryClaim(f.old.context,h,1,0,&oldClaim)==MMS_E_UNSUPPORTED&&Same(oldClaim,untouched));
    auto value=Value(f.item,f.b);CHECK(f.api.applyEx(f.api.context,h,1,Span(&value))==MMS_OK&&f.Parent()=="p.b"&&f.registry.ExportPreferences()==saved);
    const auto revision=f.Info(h).revision;CHECK(revision==2);
    MmsUiLayoutSessionValueV1 oldValue{};oldValue.h=Header<MmsUiLayoutSessionValueV1>();oldValue.element=f.item;oldValue.fields=PreviewParent;
    CHECK(f.old.apply(f.old.context,h,revision,{&oldValue,1,sizeof(oldValue),1})==MMS_E_ARGUMENT&&f.Info(h).revision==revision&&f.Parent()=="p.b");
    CHECK(f.old.rollback(f.old.context,h,revision)==MMS_OK&&f.Parent()=="p.a");
    CHECK(f.api.queryClaimEx(f.api.context,h,f.Info(h).revision,0,&claim)==MMS_OK&&!claim.fields&&!claim.parent);
    CHECK(f.old.release(f.old.context,h)==MMS_OK);
    const auto commit=f.Begin(target);CHECK(f.api.applyEx(f.api.context,commit,1,Span(&value))==MMS_OK);
    auto history=f.registry.LayoutHistoryApi(f.owner);MmsUiLayoutHistoryInfoV1 prior{};prior.h=Header<MmsUiLayoutHistoryInfoV1>();
    CHECK(history.queryHistory(history.context,&prior)==MMS_OK);
    CHECK(f.old.commit(f.old.context,commit,f.Info(commit).revision)==MMS_OK&&f.Parent()=="p.b");
    MmsUiLayoutHistoryInfoV1 after{};after.h=Header<MmsUiLayoutHistoryInfoV1>();CHECK(history.queryHistory(history.context,&after)==MMS_OK&&after.undoDepth==prior.undoDepth+1);
    CHECK(history.undo(history.context)==MMS_OK&&f.Parent()=="p.a");CHECK(history.redo(history.context)==MMS_OK&&f.Parent()=="p.b");
    CHECK(f.api.release(f.api.context,commit)==MMS_OK);
    // A session created through 1.0 stays the same object when read and
    // changed through the extended functions; Parent is not synthesized.
    MmsUiLayoutSessionTargetV1 legacy{};legacy.h=Header<MmsUiLayoutSessionTargetV1>();legacy.element=f.item;legacy.fields=PreviewVisible;
    MmsHandle legacyHandle=0;CHECK(f.old.begin(f.old.context,{&legacy,1,sizeof(legacy),1},&legacyHandle)==MMS_OK);
    CHECK(f.api.queryClaimEx(f.api.context,legacyHandle,1,0,&claim)==MMS_OK&&claim.fields==PreviewVisible&&!claim.parent);
    auto visible=Value(f.item,0,PreviewVisible);visible.visible=0;
    CHECK(f.api.applyEx(f.api.context,legacyHandle,1,Span(&visible))==MMS_OK);
    CHECK(!f.registry.EffectiveLayout(f.registry.elements.at("p.item")).visible&&f.Info(legacyHandle).revision==2);
    CHECK(f.old.rollback(f.old.context,legacyHandle,2)==MMS_OK&&f.old.release(f.old.context,legacyHandle)==MMS_OK);
}
void MalformedRecords(){
    Fixture f;const auto base=Target(f.item,f.b);const auto saved=f.registry.ExportPreferences();const auto revision=f.registry.Revision();
    const auto reject=[&](const MmsUiLayoutSessionTargetV1_2& target,MmsSpan span,MmsResult expected){
        span.data=&target;MmsHandle out=919;CHECK(f.api.beginEx(f.api.context,span,&out)==expected&&out==919);
        CHECK(f.registry.Revision()==revision&&f.registry.ExportPreferences()==saved&&f.Parent()=="p.a");
    };
    for(unsigned i=0;i<9;++i){auto t=base;auto s=Span(&t);auto expected=MMS_E_ARGUMENT;
        switch(i){case 0:t.h.major=2;expected=MMS_E_VERSION;break;case 1:t.h.minor=0;expected=MMS_E_VERSION;break;
        case 2:--t.h.size;break;case 3:++s.stride;break;case 4:s.recordVersion=1;expected=MMS_E_VERSION;break;
        case 5:t.reserved=1;break;case 6:t.parent=0;break;case 7:t.fields=PreviewVisible;break;case 8:t.fields=16;break;}reject(t,s,expected);}
    MmsHandle out=919;CHECK(f.api.beginEx(f.api.context,{},&out)==MMS_E_ARGUMENT&&out==919);
    CHECK(f.api.beginEx(f.api.context,Span(&base),nullptr)==MMS_E_ARGUMENT);
    const auto h=f.Begin(base);const auto value=Value(f.item,f.b);
    for(unsigned i=0;i<13;++i){auto v=value;auto s=Span(&v);auto expected=MMS_E_ARGUMENT;
        switch(i){case 0:v.h.major=2;expected=MMS_E_VERSION;break;case 1:v.h.minor=0;expected=MMS_E_VERSION;break;
        case 2:--v.h.size;break;case 3:--s.stride;break;case 4:s.recordVersion=1;expected=MMS_E_VERSION;break;
        case 5:v.reserved=1;break;case 6:v.parent=0;break;case 7:v.fields=PreviewVisible;break;case 8:v.visible=1;break;
        case 9:v.width=1;break;case 10:v.height=1;break;case 11:v.order=1;break;case 12:v.fields=16;break;}
        CHECK(f.api.applyEx(f.api.context,h,1,s)==expected&&f.Info(h).revision==1&&f.Parent()=="p.a");}
    auto claim=Ex<MmsUiLayoutSessionTargetV1_2>();claim.element=919;claim.parent=919;
    for(unsigned i=0;i<5;++i){auto output=claim;auto rev=uint64_t(1);auto index=uint32_t(0);auto expected=MMS_E_VERSION;
        switch(i){case 0:output.h.minor=0;break;case 1:output.h.major=2;break;case 2:--output.h.size;expected=MMS_E_ARGUMENT;break;
        case 3:rev=0;expected=MMS_E_STALE;break;case 4:index=1;expected=MMS_E_ARGUMENT;break;}
        const auto before=output;CHECK(f.api.queryClaimEx(f.api.context,h,rev,index,&output)==expected&&Same(output,before));}
    auto caps=Sentinel<MmsUiLayoutSessionCapsV1>();caps.h.minor=2;const auto oldCaps=caps;
    CHECK(f.api.queryCapabilitiesEx(f.api.context,&caps)==MMS_E_VERSION&&Same(caps,oldCaps));
    CHECK(f.api.queryCapabilitiesEx(nullptr,&caps)==MMS_E_ARGUMENT&&Same(caps,oldCaps));
    auto info=Sentinel<MmsUiLayoutSessionTargetInfoV1>();--info.h.size;const auto oldInfo=info;
    CHECK(f.api.queryTargetEx(f.api.context,f.item,&info)==MMS_E_ARGUMENT&&Same(info,oldInfo));
    CHECK(f.api.rollback(f.api.context,h,1)==MMS_OK&&f.api.release(f.api.context,h)==MMS_OK);
}
void PermissionsAndLifecycle(){
    Fixture f;for(auto parent:{f.foreignGroup,f.otherRoot,f.item,MmsHandle(999999)}){
        auto target=Target(f.item,parent);MmsHandle out=919;CHECK(f.api.beginEx(f.api.context,Span(&target),&out)!=MMS_OK&&out==919&&f.Parent()=="p.a");}
    auto target=Target(f.item,f.b,PreviewParent|PreviewVisible);const auto h=f.Begin(target);auto value=Value(f.item,f.b,target.fields);value.visible=1;
    auto foreignTarget=Target(f.item,f.b);MmsHandle untouched=919;
    CHECK(f.foreign.beginEx(f.foreign.context,Span(&foreignTarget),&untouched)==MMS_E_ARGUMENT&&untouched==919);
    auto claim=Ex<MmsUiLayoutSessionTargetV1_2>();const auto empty=claim;
    CHECK(f.foreign.queryClaimEx(f.foreign.context,h,1,0,&claim)==MMS_E_NOT_FOUND&&Same(claim,empty));
    CHECK(f.api.applyEx(f.api.context,h,1,Span(&value))==MMS_OK);
    MmsUiLayoutSessionValueV1 incomplete{};incomplete.h=Header<MmsUiLayoutSessionValueV1>();incomplete.element=f.item;incomplete.fields=PreviewVisible;
    const auto beforeIncomplete=f.registry.ExportPreferences();const auto beforeTree=f.registry.Revision();
    CHECK(f.old.apply(f.old.context,h,2,{&incomplete,1,sizeof(incomplete),1})==MMS_E_ARGUMENT);
    CHECK(f.Info(h).revision==2&&f.Parent()=="p.b"&&f.registry.Revision()==beforeTree&&f.registry.ExportPreferences()==beforeIncomplete);
    CHECK(f.registry.EffectiveLayout(f.registry.elements.at("p.item")).visible);
    f.MoveSaved();const auto remaining=f.Info(h);CHECK(remaining.state==MMS_LAYOUT_SESSION_ACTIVE&&remaining.revision>2);
    CHECK(f.api.queryClaimEx(f.api.context,h,remaining.revision,0,&claim)==MMS_OK&&claim.fields==PreviewVisible&&!claim.parent);
    MmsUiLayoutSessionValueV1 oldValue{};oldValue.h=Header<MmsUiLayoutSessionValueV1>();oldValue.element=f.item;oldValue.fields=PreviewVisible;
    CHECK(f.old.apply(f.old.context,h,remaining.revision,{&oldValue,1,sizeof(oldValue),1})==MMS_OK);
    f.owner.stopping=true;const auto stopped=f.Info(h);auto single=Target(f.limited,f.b);
    CHECK(f.api.beginEx(f.api.context,Span(&single),&untouched)==MMS_E_NOT_READY&&untouched==919);
    CHECK(f.api.applyEx(f.api.context,h,stopped.revision,Span(&value))==MMS_E_NOT_READY);
    CHECK(f.api.queryClaimEx(f.api.context,h,stopped.revision,0,&claim)==MMS_OK&&!claim.parent);
    CHECK(f.old.rollback(f.old.context,h,stopped.revision)==MMS_OK&&f.old.release(f.old.context,h)==MMS_OK);
    f.owner.stopping=false;
    MmsUiLayoutSessionTargetV1_2 cycle[]{Target(f.a,f.b),Target(f.b,f.a)};
    CHECK(f.api.beginEx(f.api.context,Span(cycle,2),&untouched)==MMS_E_ARGUMENT&&untouched==919);
    const auto active=f.Begin(Target(f.item,f.b));const auto rev=f.Info(active).revision;
    MmsResult a=0,b=0,c=0;auto output=Ex<MmsUiLayoutSessionTargetV1_2>();const auto initial=output;
    std::thread wrong([&]{a=f.api.queryClaimEx(f.api.context,active,rev,0,&output);
        b=f.api.applyEx(f.api.context,active,rev,Span(&value));c=f.api.rollback(f.api.context,active,rev);});wrong.join();
    CHECK(a==MMS_E_THREAD&&b==MMS_E_THREAD&&c==MMS_E_THREAD&&Same(output,initial)&&f.Info(active).revision==rev);
    CHECK(f.old.rollback(f.old.context,active,rev)==MMS_OK&&f.old.release(f.old.context,active)==MMS_OK);
}
}
int main(){try{CapabilitiesAndIdentity();MalformedRecords();PermissionsAndLifecycle();}catch(...){CHECK(false);}
    std::printf("layout_session_parent_tests: %u checks, %u failures\n",checks,failures);return failures?1:0;}
