#include "../../src/plugins/ui_registry.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>

namespace { thread_local bool failNextAllocation = false; }
void* operator new(std::size_t bytes) {
    if (failNextAllocation) { failNextAllocation = false; throw std::bad_alloc{}; }
    if (auto* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

using namespace mikumikustudio::plugins;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while (false)
MmsString S(const char* text) { return {text, std::strlen(text)}; }
template<class T> MmsSpan Span(const T* data, size_t count = 1) { return {data,count,sizeof(T),1}; }
template<class T> bool Same(const T& left, const T& right) { return !std::memcmp(&left,&right,sizeof(T)); }
template<class T> T Sentinel() { T value; std::memset(&value,0x5a,sizeof(value)); value.h=Header<T>(); return value; }
MmsUiLayoutSessionTargetV1 Target(MmsHandle element, uint32_t fields) { return {Header<MmsUiLayoutSessionTargetV1>(),element,fields,0}; }
MmsUiLayoutSessionValueV1 Value(MmsHandle element, uint32_t fields, bool visible=false, float width=140, float height=40, int order=-7) {
    return {Header<MmsUiLayoutSessionValueV1>(),element,fields,
        (fields & MMS_LAYOUT_SESSION_VISIBLE) ? uint32_t(visible) : 0u,
        (fields & MMS_LAYOUT_SESSION_SIZE) ? width : 0.f, (fields & MMS_LAYOUT_SESSION_SIZE) ? height : 0.f,
        (fields & MMS_LAYOUT_SESSION_ORDER) ? order : 0,0};
}
struct Fixture {
    std::function<void()> changed;
    UiRegistry registry{[this]{if(changed)changed();}};
    UiClient owner{&registry,"example.session"},other{&registry,"example.other"};
    MmsUiApiV1 ui=registry.Api(owner);
    MmsUiLayoutSessionApiV1 api=registry.LayoutSessionApi(owner),foreign=registry.LayoutSessionApi(other);
    MmsHandle first=0,left=0,foreignElement=0,limited=0,menu=0;
    Fixture() {
        Command(owner,"example.session.command");Command(other,"example.other.command");
        Element(owner,"example.session.group","mms.toolbar.main.extensions",MMS_UI_GROUP,MMS_UI_CONTAINER|MMS_UI_HIDE|MMS_UI_LAYOUT);
        first=Element(owner,"example.session.first","example.session.group");
        left=Element(owner,"example.session.left","mms.sidebar.left.extensions");
        foreignElement=Element(other,"example.other.button","mms.properties.main.extensions");
        limited=Element(owner,"example.session.limited","mms.sidebar.right.extensions",MMS_UI_BUTTON,MMS_UI_HIDE);
        menu=Element(owner,"example.session.menu","mms.menu.plugins",MMS_UI_MENU,MMS_UI_CONTAINER|MMS_UI_HIDE);
    }
    void Command(UiClient& client,const char* id) {
        auto table=registry.Api(client);MmsCommandDescV1 desc{};desc.info.h=Header<MmsCommandDescV1>();desc.info.id=S(id);desc.info.ownerPlugin=Text(client.id);
        desc.getState=[](void*,MmsObjectRef,MmsCommandStateV1* state)->MmsResult{state->enabled=state->visible=1;return MMS_OK;};
        desc.invoke=[](void*,MmsObjectRef,MmsSpan,MmsHandle)->MmsResult{return MMS_OK;};MmsHandle handle=0;
        CHECK(table.registerCommand(table.context,&desc,&handle)==MMS_OK);
    }
    MmsHandle Element(UiClient& client,const char* id,const char* parent,uint32_t kind=MMS_UI_BUTTON,
                      uint32_t caps=MMS_UI_HIDE|MMS_UI_RESIZE|MMS_UI_LAYOUT|MMS_UI_MOVE) {
        auto table=registry.Api(client);MmsUiElementV1 desc{};desc.h=Header<MmsUiElementV1>();desc.id=S(id);desc.parentId=S(parent);
        desc.ownerPlugin=Text(client.id);desc.kind=kind;desc.capabilities=caps;
        const auto command=client.id+".command";if(kind==MMS_UI_BUTTON)desc.commandId=Text(command);
        MmsHandle handle=0;CHECK(table.registerElement(table.context,&desc,&handle)==MMS_OK);return handle;
    }
    MmsHandle Begin(std::initializer_list<MmsUiLayoutSessionTargetV1> targets) {
        MmsHandle handle=0;CHECK(api.begin(api.context,Span(targets.begin(),targets.size()),&handle)==MMS_OK&&handle);return handle;
    }
    MmsHandle Panel(UiClient& client, const char* id) {
        auto surfaces=registry.SurfaceApi(client);const std::string surfaceId=std::string(id)+".surface";
        MmsUiSurfaceDescV1 surface{};surface.h=Header<MmsUiSurfaceDescV1>();surface.id=Text(surfaceId);
        surface.createView=[](void*,const MmsUiMountV1*,void**,uintptr_t*)->MmsResult{CHECK(false);return MMS_E_FAILED;};
        surface.resizeView=[](void*,const MmsUiRectV1*,uint32_t){CHECK(false);};
        surface.showView=[](void*,uint32_t){CHECK(false);};surface.focusView=[](void*)->MmsResult{CHECK(false);return MMS_E_FAILED;};
        surface.destroyView=[](void*){CHECK(false);};MmsHandle handle=0;
        CHECK(surfaces.registerSurface(surfaces.context,&surface,&handle)==MMS_OK);
        auto table=registry.Api(client);MmsUiElementV1 element{};element.h=Header<MmsUiElementV1>();
        element.id=S(id);element.ownerPlugin=Text(client.id);element.parentId=S("mms.workspace.tabs.extensions");
        element.kind=MMS_UI_PANEL;element.capabilities=MMS_UI_HIDE|MMS_UI_RESIZE|MMS_UI_LAYOUT|MMS_UI_MOVE;element.surfaceId=Text(surfaceId);
        CHECK(table.registerElement(table.context,&element,&handle)==MMS_OK);return handle;
    }
    MmsUiLayoutSessionInfoV1 Info(MmsHandle handle) {
        MmsUiLayoutSessionInfoV1 info{};info.h=Header<MmsUiLayoutSessionInfoV1>();CHECK(api.query(api.context,handle,&info)==MMS_OK);return info;
    }
    MmsUiLayoutSessionTargetV1 Claim(MmsHandle handle,uint32_t index) {
        auto out=Target(0,0);CHECK(api.queryClaim(api.context,handle,Info(handle).revision,index,&out)==MMS_OK);return out;
    }
    void PersistVisible(bool visible) {
        MmsHandle tx=0;CHECK(ui.beginLayout(ui.context,registry.Revision(),&tx)==MMS_OK);
        MmsLayoutOpV1 op{};op.h=Header<MmsLayoutOpV1>();op.operation=MMS_LAYOUT_VISIBLE;op.elementId=S("example.session.first");
        op.expectedGeneration=registry.elements.at("example.session.first").desc.generation;op.value.visible=visible;
        CHECK(ui.applyLayout(ui.context,tx,&op)==MMS_OK&&ui.commitLayout(ui.context,tx)==MMS_OK);
    }
};
void CapabilitiesAndStaticTargets() {
    Fixture f;auto caps=Sentinel<MmsUiLayoutSessionCapsV1>();
    CHECK(f.api.queryCapabilities(f.api.context,&caps)==MMS_OK&&caps.fields==7&&caps.mounts==15&&caps.maxTargets==64&&
          caps.maxSessionsPerPlugin==32&&caps.maxSessionsGlobal==256&&!caps.reserved);
    auto target=Sentinel<MmsUiLayoutSessionTargetInfoV1>();
    CHECK(f.api.queryTarget(f.api.context,f.first,&target)==MMS_OK&&target.element==f.first&&target.fields==7&&target.mount==1);
    CHECK(f.api.queryTarget(f.api.context,f.limited,&target)==MMS_OK&&target.fields==1&&target.mount==8);
    const auto before=target;
    CHECK(f.api.queryTarget(f.api.context,f.menu,&target)==MMS_E_UNSUPPORTED&&Same(target,before));
    CHECK(f.api.queryTarget(f.api.context,f.foreignElement,&target)==MMS_E_ARGUMENT&&Same(target,before));
    CHECK(f.api.queryTarget(f.api.context,UINT64_MAX,&target)==MMS_E_NOT_FOUND&&Same(target,before));
    const auto session=f.Begin({Target(f.first,1)});
    CHECK(f.api.queryTarget(f.api.context,f.first,&target)==MMS_OK&&target.fields==7);
    uint64_t token=0;CHECK(f.registry.BeginHostLayout(HostLayoutToolbar,token)==MMS_OK);
    CHECK(f.api.queryTarget(f.api.context,f.first,&target)==MMS_OK&&target.fields==7&&target.mount==1);
    auto input=Target(f.first,1);MmsHandle untouched=999;
    CHECK(f.api.begin(f.api.context,Span(&input),&untouched)==MMS_E_BUSY&&untouched==999);
    CHECK(f.Info(session).reasons==MMS_LAYOUT_SESSION_HOST_INTERACTION);
    CHECK(f.api.release(f.api.context,session)==MMS_OK);f.registry.EndHostLayout(token);
}
void InvalidInputAndOutputs() {
    Fixture f;auto target=Target(f.first,1);MmsHandle handle=999;
    const auto reject=[&](MmsSpan span,MmsResult result=MMS_E_ARGUMENT){CHECK(f.api.begin(f.api.context,span,&handle)==result&&handle==999);};
    auto span=Span(&target);span.data=nullptr;reject(span);span=Span(&target);span.count=0;reject(span);
    span=Span(&target);span.count=65;reject(span);span=Span(&target);--span.stride;reject(span);
    span=Span(&target);++span.stride;reject(span);span=Span(&target);span.recordVersion=2;reject(span,MMS_E_VERSION);
    CHECK(f.api.begin(f.api.context,Span(&target),nullptr)==MMS_E_ARGUMENT);
    auto bad=target;bad.h.major=2;reject(Span(&bad),MMS_E_VERSION);bad=target;bad.h.minor=1;reject(Span(&bad),MMS_E_VERSION);
    bad=target;--bad.h.size;reject(Span(&bad));bad=target;++bad.h.size;reject(Span(&bad));bad=target;bad.reserved=1;reject(Span(&bad));
    bad=target;bad.fields=0;reject(Span(&bad));bad=target;bad.fields=8;reject(Span(&bad));bad=target;bad.element=0;reject(Span(&bad));
    MmsUiLayoutSessionTargetV1 duplicate[]{target,target};reject(Span(duplicate,2));
    failNextAllocation=true;const auto oom=f.api.begin(f.api.context,Span(&target),&handle);failNextAllocation=false;
    CHECK(oom==MMS_E_MEMORY&&handle==999);
    handle=f.Begin({target});const auto value=Value(f.first,1);const auto original=f.Info(handle);
    const auto rejectValue=[&](MmsUiLayoutSessionValueV1 input,MmsResult result=MMS_E_ARGUMENT){
        CHECK(f.api.apply(f.api.context,handle,original.revision,Span(&input))==result);CHECK(Same(f.Info(handle),original));};
    auto v=value;v.visible=2;rejectValue(v);v=value;v.reserved=1;rejectValue(v);v=value;v.width=1;rejectValue(v);
    v=value;v.height=(std::numeric_limits<float>::quiet_NaN)();rejectValue(v);v=value;v.order=1;rejectValue(v);
    v=value;v.h.major=2;rejectValue(v,MMS_E_VERSION);v=value;v.h.minor=1;rejectValue(v,MMS_E_VERSION);
    v=value;++v.h.size;rejectValue(v);v=value;v.fields=8;rejectValue(v);
    v=Value(f.first,2);v.visible=1;rejectValue(v); // Unclaimed/unused visibility is not ignored.
    auto inputSpan=Span(&value);inputSpan.recordVersion=0;
    CHECK(f.api.apply(f.api.context,handle,original.revision,inputSpan)==MMS_E_VERSION);
    inputSpan=Span(&value);inputSpan.stride=0;CHECK(f.api.apply(f.api.context,handle,original.revision,inputSpan)==MMS_E_ARGUMENT);
    failNextAllocation=true;const auto applyOom=f.api.apply(f.api.context,handle,original.revision,Span(&value));failNextAllocation=false;
    CHECK(applyOom==MMS_E_MEMORY&&Same(f.Info(handle),original));
    auto caps=Sentinel<MmsUiLayoutSessionCapsV1>();caps.h.minor=1;const auto capsBefore=caps;
    CHECK(f.api.queryCapabilities(f.api.context,&caps)==MMS_E_VERSION&&Same(caps,capsBefore));
    auto info=Sentinel<MmsUiLayoutSessionInfoV1>();--info.h.size;const auto infoBefore=info;
    CHECK(f.api.query(f.api.context,handle,&info)==MMS_E_ARGUMENT&&Same(info,infoBefore));
    auto claim=Sentinel<MmsUiLayoutSessionTargetV1>();const auto claimBefore=claim;
    CHECK(f.api.queryClaim(f.api.context,handle,original.revision+1,0,&claim)==MMS_E_STALE&&Same(claim,claimBefore));
    CHECK(f.api.queryClaim(f.api.context,handle,original.revision,1,&claim)==MMS_E_ARGUMENT&&Same(claim,claimBefore));
    CHECK(f.api.queryCapabilities(nullptr,&caps)==MMS_E_ARGUMENT);
    CHECK(f.api.queryCapabilities(f.api.context,nullptr)==MMS_E_ARGUMENT);
    CHECK(f.api.queryTarget(f.api.context,f.first,nullptr)==MMS_E_ARGUMENT);
    CHECK(f.api.query(f.api.context,handle,nullptr)==MMS_E_ARGUMENT);
    CHECK(f.api.queryClaim(f.api.context,handle,original.revision,0,nullptr)==MMS_E_ARGUMENT);
    CHECK(f.api.release(f.api.context,handle)==MMS_E_BUSY);
    CHECK(f.api.apply(f.api.context,handle,original.revision,Span(&value))==MMS_OK);
    const auto applied=f.Info(handle);const auto stored=f.registry.ExportPreferences();
    failNextAllocation=true;const auto commitOom=f.api.commit(f.api.context,handle,applied.revision);failNextAllocation=false;
    CHECK(commitOom==MMS_E_MEMORY&&Same(f.Info(handle),applied)&&f.registry.ExportPreferences()==stored);
    CHECK(f.api.rollback(f.api.context,handle,applied.revision)==MMS_OK);
    CHECK(f.Claim(handle,0).fields==0&&f.Info(handle).activeTargetCount==0);
    CHECK(f.api.release(f.api.context,handle)==MMS_OK);
}
void PartialClaimsAndReasons() {
    Fixture f;const auto stored=f.registry.ExportPreferences();
    const auto handle=f.Begin({Target(f.first,3),Target(f.left,5)});
    MmsUiLayoutSessionValueV1 values[]{Value(f.first,3,false,180,60),Value(f.left,5,false,0,0,-40)};
    CHECK(f.api.apply(f.api.context,handle,1,Span(values,2))==MMS_OK);
    f.PersistVisible(true);auto info=f.Info(handle);
    CHECK(info.state==MMS_LAYOUT_SESSION_ACTIVE&&info.targetCount==2&&info.activeTargetCount==2&&info.reasons==MMS_LAYOUT_SESSION_PERSISTENT_EDIT);
    CHECK(f.Claim(handle,0).element==f.first&&f.Claim(handle,0).fields==2&&f.Claim(handle,1).fields==5);
    uint64_t token=0;CHECK(f.registry.BeginHostLayout(HostLayoutToolbar,token)==MMS_OK);
    info=f.Info(handle);CHECK(info.targetCount==2&&info.activeTargetCount==1&&info.state==MMS_LAYOUT_SESSION_ACTIVE&&
        info.reasons==(MMS_LAYOUT_SESSION_PERSISTENT_EDIT|MMS_LAYOUT_SESSION_HOST_INTERACTION));
    CHECK(f.Claim(handle,0).element==f.first&&!f.Claim(handle,0).fields&&f.Claim(handle,1).element==f.left);
    auto remaining=Value(f.left,5,false,0,0,-70);
    CHECK(f.api.apply(f.api.context,handle,info.revision,Span(&remaining))==MMS_OK);
    CHECK(f.Info(handle).targetCount==2&&f.Claim(handle,0).element==f.first&&!f.Claim(handle,0).fields&&f.Claim(handle,1).element==f.left);
    auto desc=f.registry.elements.at("example.session.left").desc;
    CHECK(f.ui.updateElement(f.ui.context,f.left,&desc)==MMS_OK);
    info=f.Info(handle);CHECK(info.state==MMS_LAYOUT_SESSION_REVOKED&&!info.activeTargetCount&&info.targetCount==2&&
        info.reasons==(MMS_LAYOUT_SESSION_PERSISTENT_EDIT|MMS_LAYOUT_SESSION_HOST_INTERACTION|MMS_LAYOUT_SESSION_TARGET_CHANGED));
    CHECK(!f.Claim(handle,0).fields&&!f.Claim(handle,1).fields);
    CHECK(f.api.apply(f.api.context,handle,info.revision,Span(&remaining))==MMS_E_NOT_READY);
    CHECK(f.api.release(f.api.context,handle)==MMS_OK);f.registry.EndHostLayout(token);
    CHECK(f.registry.ExportPreferences()==stored);
    const auto removed=f.Begin({Target(f.left,1)});
    CHECK(f.ui.unregisterItem(f.ui.context,f.left)==MMS_OK);
    CHECK(f.Info(removed).state==MMS_LAYOUT_SESSION_REVOKED&&f.Info(removed).reasons==MMS_LAYOUT_SESSION_TARGET_CHANGED);
    CHECK(f.api.release(f.api.context,removed)==MMS_OK);
}
void ThreadOwnerStopAndReentry() {
    Fixture f;auto target=Target(f.first,1);auto value=Value(f.first,1);const auto handle=f.Begin({target});
    auto sentinel=Sentinel<MmsUiLayoutSessionInfoV1>();const auto before=sentinel;
    CHECK(f.foreign.query(f.foreign.context,handle,&sentinel)==MMS_E_NOT_FOUND&&Same(sentinel,before));
    auto claim=Target(123,7);const auto claimBefore=claim;
    CHECK(f.foreign.queryClaim(f.foreign.context,handle,1,0,&claim)==MMS_E_NOT_FOUND&&Same(claim,claimBefore));
    CHECK(f.foreign.apply(f.foreign.context,handle,1,Span(&value))==MMS_E_NOT_FOUND);
    CHECK(f.foreign.rollback(f.foreign.context,handle,1)==MMS_E_NOT_FOUND);
    CHECK(f.foreign.commit(f.foreign.context,handle,1)==MMS_E_NOT_FOUND);
    CHECK(f.foreign.release(f.foreign.context,handle)==MMS_E_NOT_FOUND);
    CHECK(f.registry.WithReadOnlyCallback([&]{return f.api.apply(f.api.context,handle,1,Span(&value));})==MMS_E_BUSY);
    CHECK(f.registry.WithReadOnlyCallback([&]{return f.api.rollback(f.api.context,handle,1);})==MMS_E_BUSY);
    CHECK(f.registry.WithReadOnlyCallback([&]{return f.api.query(f.api.context,handle,&sentinel);})==MMS_OK);
    std::array<MmsResult,9> results{};MmsHandle output=777;
    std::thread worker([&]{
        auto caps=Sentinel<MmsUiLayoutSessionCapsV1>();auto info=Sentinel<MmsUiLayoutSessionTargetInfoV1>();
        results={f.api.queryCapabilities(f.api.context,&caps),f.api.queryTarget(f.api.context,f.first,&info),
            f.api.begin(f.api.context,Span(&target),&output),f.api.apply(f.api.context,handle,1,Span(&value)),
            f.api.query(f.api.context,handle,&sentinel),f.api.queryClaim(f.api.context,handle,1,0,&claim),
            f.api.rollback(f.api.context,handle,1),f.api.commit(f.api.context,handle,1),f.api.release(f.api.context,handle)};
    });worker.join();for(auto result:results)CHECK(result==MMS_E_THREAD);CHECK(output==777&&Same(claim,claimBefore));
    f.owner.stopping=true;
    CHECK(f.api.begin(f.api.context,Span(&target),&output)==MMS_E_NOT_READY&&output==777);
    CHECK(f.api.apply(f.api.context,handle,1,Span(&value))==MMS_E_NOT_READY);
    CHECK(f.api.commit(f.api.context,handle,1)==MMS_E_NOT_READY);
    CHECK(f.api.rollback(f.api.context,handle,1)==MMS_OK&&f.Info(handle).state==MMS_LAYOUT_SESSION_ROLLED_BACK);
    CHECK(f.api.release(f.api.context,handle)==MMS_OK);f.owner.stopping=false;
    const auto stopped=f.Begin({target});f.owner.stopping=true;f.registry.RevokeLayoutPreviews(f.owner);
    CHECK(f.Info(stopped).state==MMS_LAYOUT_SESSION_REVOKED&&f.Info(stopped).reasons==MMS_LAYOUT_SESSION_STOPPING);
    CHECK(f.api.release(f.api.context,stopped)==MMS_OK);f.owner.stopping=false;
    const auto committed=f.Begin({target});CHECK(f.api.apply(f.api.context,committed,1,Span(&value))==MMS_OK);
    const auto revision=f.Info(committed).revision;bool armed=true;
    f.changed=[&]{if(!armed)return;armed=false;CHECK(f.Info(committed).state==MMS_LAYOUT_SESSION_COMMITTED);
        CHECK(!f.Claim(committed,0).fields&&f.api.release(f.api.context,committed)==MMS_OK);};
    CHECK(f.api.commit(f.api.context,committed,revision)==MMS_OK);f.changed={};CHECK(!armed);
    CHECK(!f.registry.elements.at("example.session.first").visible);
    CHECK(f.api.query(f.api.context,committed,&sentinel)==MMS_E_NOT_FOUND);
}
void WorkspaceVersionsAndAdmission() {
    Fixture f;const auto panel=f.Panel(f.owner,"example.session.panel");
    const auto other=f.Panel(f.other,"example.other.panel");
    auto api=f.registry.LayoutSessionApi(f.owner,1);auto badApi=f.registry.LayoutSessionApi(f.owner,2);
    CHECK(api.h.size==88&&api.h.major==1&&api.h.minor==1&&api.context==f.api.context);
    CHECK(f.api.h.size==88&&f.api.h.major==1&&!f.api.h.minor&&!badApi.h.size&&!badApi.begin);
    auto caps=Sentinel<MmsUiLayoutSessionCapsV1>();
    CHECK(api.queryCapabilities(api.context,&caps)==MMS_OK&&caps.mounts==31&&caps.fields==7&&!caps.h.minor);
    CHECK(f.api.queryCapabilities(f.api.context,&caps)==MMS_OK&&caps.mounts==15);
    auto info=Sentinel<MmsUiLayoutSessionTargetInfoV1>();const auto untouched=info;
    CHECK(f.api.queryTarget(f.api.context,panel,&info)==MMS_E_UNSUPPORTED&&Same(info,untouched));
    CHECK(api.queryTarget(api.context,panel,&info)==MMS_OK&&info.mount==MMS_LAYOUT_SESSION_WORKSPACE&&info.fields==7&&!info.h.minor);
    const auto before=info;
    CHECK(api.queryTarget(api.context,other,&info)==MMS_E_ARGUMENT&&Same(info,before));
    MmsHandle handle=998;auto target=Target(panel,7);
    CHECK(f.api.begin(f.api.context,Span(&target),&handle)==MMS_E_UNSUPPORTED&&handle==998);
    auto foreignTarget=Target(other,1);
    CHECK(api.begin(api.context,Span(&foreignTarget),&handle)==MMS_E_ARGUMENT&&handle==998);
    MmsUiLayoutSessionTargetV1 mixed[]{Target(f.first,1),target};
    CHECK(f.api.begin(f.api.context,Span(mixed,2),&handle)==MMS_E_UNSUPPORTED&&handle==998);
    const auto ordinary=f.Begin({Target(f.first,1)}); // A rejected mixed begin claimed nothing.
    CHECK(f.api.rollback(f.api.context,ordinary,1)==MMS_OK&&f.api.release(f.api.context,ordinary)==MMS_OK);
    auto desc=f.registry.elements.at("example.session.panel").desc;
    desc.parentId=S("mms.toolbar.main.extensions");
    CHECK(f.ui.updateElement(f.ui.context,panel,&desc)==MMS_E_ARGUMENT);
    auto& stored=f.registry.elements.at("example.session.panel");
    const auto parent=stored.desc.parentId;stored.desc.parentId=S("example.session.group");
    CHECK(api.queryTarget(api.context,panel,&info)==MMS_E_UNSUPPORTED&&Same(info,before));
    CHECK(api.begin(api.context,Span(&target),&handle)==MMS_E_UNSUPPORTED&&handle==998);stored.desc.parentId=parent;
    stored.nativeObserved=true;
    CHECK(api.queryTarget(api.context,panel,&info)==MMS_E_UNSUPPORTED&&Same(info,before));stored.nativeObserved=false;
    stored.desc.capabilities|=MMS_UI_DOCK;
    CHECK(api.queryTarget(api.context,panel,&info)==MMS_E_UNSUPPORTED&&Same(info,before));
    CHECK(api.begin(api.context,Span(&target),&handle)==MMS_E_UNSUPPORTED&&handle==998);stored.desc.capabilities&=~MMS_UI_DOCK;
    target.h.minor=1;
    CHECK(api.begin(api.context,Span(&target),&handle)==MMS_E_VERSION&&handle==998);target.h.minor=0;
    caps.h.minor=1;const auto capsBefore=caps;
    CHECK(api.queryCapabilities(api.context,&caps)==MMS_E_VERSION&&Same(caps,capsBefore));
    CHECK(f.registry.WithReadOnlyCallback([&]{return api.begin(api.context,Span(&target),&handle);})==MMS_E_BUSY&&handle==998);
    f.owner.stopping=true;
    CHECK(api.begin(api.context,Span(&target),&handle)==MMS_E_NOT_READY&&handle==998);f.owner.stopping=false;
    CHECK(api.begin(api.context,Span(&target),&handle)==MMS_OK&&handle!=998);
    CHECK(f.api.rollback(f.api.context,handle,1)==MMS_OK&&f.api.release(f.api.context,handle)==MMS_OK);
}
void WorkspaceSharedHandlesAndTakeover() {
    Fixture f;const auto panel=f.Panel(f.owner,"example.session.panel");auto api=f.registry.LayoutSessionApi(f.owner,1);
    const auto saved=f.registry.ExportPreferences();const auto baseline=f.registry.EffectiveLayout(f.registry.elements.at("example.session.panel"));
    auto history=f.registry.LayoutHistoryApi(f.owner);MmsUiLayoutHistoryInfoV1 oldHistory{};oldHistory.h=Header<MmsUiLayoutHistoryInfoV1>();
    CHECK(history.queryHistory(history.context,&oldHistory)==MMS_OK);
    auto target=Target(panel,7);MmsHandle handle=0;
    CHECK(api.begin(api.context,Span(&target),&handle)==MMS_OK&&!f.registry.HasLayoutPreviewFields(panel,7));
    auto value=Value(panel,7,false,360,180,-30);
    // Versioned admission shares the exact same object and mutation table.
    CHECK(f.Info(handle).revision==1&&f.Claim(handle,0).element==panel);
    CHECK(f.api.apply(f.api.context,handle,1,Span(&value))==MMS_OK);
    CHECK(f.registry.HasLayoutPreviewFields(panel,PreviewVisible)&&f.registry.HasLayoutPreviewFields(panel,PreviewSize));
    CHECK(!f.registry.HasLayoutPreviewFields(panel,0)&&!f.registry.HasLayoutPreviewFields(panel,8)&&!f.registry.HasLayoutPreviewFields(0,1));
    const auto effective=f.registry.EffectiveLayout(f.registry.elements.at("example.session.panel"));
    CHECK(!effective.visible&&effective.width==360&&effective.height==180&&effective.hints.order==-30);
    CHECK(f.registry.ExportPreferences()==saved);
    uint64_t controls=0;CHECK(f.registry.BeginHostLayout(15,controls)==MMS_OK);
    CHECK(f.Info(handle).state==MMS_LAYOUT_SESSION_ACTIVE&&f.Info(handle).revision==2&&!f.Info(handle).reasons);
    CHECK(f.registry.IsHostLayoutCurrent(15,controls)&&!f.registry.IsHostLayoutCurrent(HostLayoutAll,controls));
    CHECK(f.api.rollback(f.api.context,handle,2)==MMS_OK&&!f.registry.HasLayoutPreviewFields(panel,7));
    CHECK(api.release(api.context,handle)==MMS_OK&&f.registry.ExportPreferences()==saved);
    CHECK(f.registry.EffectiveLayout(f.registry.elements.at("example.session.panel")).visible==baseline.visible);
    CHECK(api.begin(api.context,Span(&target),&handle)==MMS_OK);
    CHECK(api.apply(api.context,handle,1,Span(&value))==MMS_OK);
    CHECK(f.api.commit(f.api.context,handle,2)==MMS_OK&&!f.registry.HasLayoutPreviewFields(panel,7));
    CHECK(f.Info(handle).state==MMS_LAYOUT_SESSION_COMMITTED&&api.release(api.context,handle)==MMS_OK);
    MmsUiLayoutHistoryInfoV1 committed{};committed.h=Header<MmsUiLayoutHistoryInfoV1>();
    CHECK(history.queryHistory(history.context,&committed)==MMS_OK&&committed.undoDepth==oldHistory.undoDepth+1);
    CHECK(history.undo(history.context)==MMS_OK&&f.registry.ExportPreferences()==saved);
    f.registry.EndHostLayout(controls);
    MmsUiLayoutSessionTargetV1 mixed[]{Target(f.first,1),Target(panel,3)};
    CHECK(api.begin(api.context,Span(mixed,2),&handle)==MMS_OK);
    MmsUiLayoutSessionValueV1 values[]{Value(f.first,1),Value(panel,3,false,240,160)};
    CHECK(api.apply(api.context,handle,1,Span(values,2))==MMS_OK);
    CHECK(f.registry.BeginHostLayout(15,controls)==MMS_OK);
    CHECK(f.Info(handle).state==MMS_LAYOUT_SESSION_ACTIVE&&f.Info(handle).activeTargetCount==1&&
        f.Info(handle).reasons==MMS_LAYOUT_SESSION_HOST_INTERACTION);
    CHECK(!f.Claim(handle,0).fields&&f.Claim(handle,1).fields==3&&f.registry.HasLayoutPreviewFields(panel,3));
    uint64_t workspace=0;CHECK(f.registry.BeginHostLayout(HostLayoutWorkspace,workspace)==MMS_OK);
    CHECK(f.registry.IsHostLayoutCurrent(HostLayoutWorkspace,workspace)&&f.registry.IsHostLayoutCurrent(15,controls));
    CHECK(f.Info(handle).state==MMS_LAYOUT_SESSION_REVOKED&&!f.registry.HasLayoutPreviewFields(panel,3));
    CHECK(f.api.release(f.api.context,handle)==MMS_OK);
    MmsHandle untouched=999;CHECK(api.begin(api.context,Span(&target),&untouched)==MMS_E_BUSY&&untouched==999);
    f.registry.EndHostLayout(workspace);CHECK(!f.registry.IsHostLayoutCurrent(HostLayoutWorkspace,workspace));
    CHECK(api.begin(api.context,Span(&target),&handle)==MMS_OK);
    CHECK(api.apply(api.context,handle,1,Span(&value))==MMS_OK);
    uint64_t all=0;CHECK(f.registry.BeginHostLayout(HostLayoutAll,all)==MMS_OK&&f.registry.IsHostLayoutCurrent(31,all));
    CHECK(f.Info(handle).state==MMS_LAYOUT_SESSION_REVOKED&&!f.registry.IsHostLayoutCurrent(15,controls));
    f.registry.EndHostLayout(controls);CHECK(f.registry.IsHostLayoutCurrent(31,all));
    CHECK(api.release(api.context,handle)==MMS_OK);f.registry.EndHostLayout(all);
    CHECK(!f.registry.IsHostLayoutCurrent(31,all));
    CHECK(f.registry.BeginHostLayout(32,untouched)==MMS_E_ARGUMENT&&untouched==999);
    CHECK(api.begin(api.context,Span(&target),&handle)==MMS_OK&&api.apply(api.context,handle,1,Span(&value))==MMS_OK);
    f.owner.stopping=true;f.registry.RevokeLayoutPreviews(f.owner);
    CHECK(f.Info(handle).reasons==MMS_LAYOUT_SESSION_STOPPING&&!f.registry.HasLayoutPreviewFields(panel,7));
    CHECK(f.api.release(f.api.context,handle)==MMS_OK);
}
}
int main() {
    try { CapabilitiesAndStaticTargets();InvalidInputAndOutputs();PartialClaimsAndReasons();ThreadOwnerStopAndReentry();
        WorkspaceVersionsAndAdmission();WorkspaceSharedHandlesAndTakeover(); }
    catch(...) { failNextAllocation=false;CHECK(false); }
    std::printf("layout_session_tests: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
