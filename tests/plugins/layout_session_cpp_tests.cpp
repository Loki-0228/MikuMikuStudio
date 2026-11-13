// CPU-only fake-host checks of SDK forwarding, ownership and failure rules.
// This program does not exercise NativeUi or claim real-window acceptance.
#include <mms/cpp/layout_session.hpp>
#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <type_traits>

using namespace mms::sdk;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)){++failures;std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);throw std::runtime_error(#x);} } while(false)
template<class T> constexpr MmsHeader H(){return {uint32_t(sizeof(T)),1,0};}
template<class T> bool Same(const T& a,const T& b){return std::memcmp(&a,&b,sizeof(T))==0;}
enum Op {Service,Caps,Target,Begin,Apply,Query,Claim,Rollback,Commit,Release,OpCount};
struct Fake {
    struct Entry {
        uint64_t revision=11;
        uint32_t state=MMS_LAYOUT_SESSION_ACTIVE,reasons=0;
        std::vector<MmsUiLayoutSessionTargetV1> claims;
        std::vector<MmsUiLayoutSessionValueV1> values;
    };
    std::array<unsigned,OpCount> calls{};
    std::array<MmsResult,OpCount> errors{};
    std::map<MmsHandle,Entry> entries;
    std::function<void(Op)> hook;
    MmsUiLayoutSessionApiV1 table{};
    MmsHandle next=100,lastHandle=0;
    uint64_t lastRevision=0;
    uint32_t lastIndex=0;
    uint32_t requestedMinor=0,targetMount=MMS_LAYOUT_SESSION_TOOLBAR;
    Op throwing=OpCount,malformed=OpCount;
    int exception=0,badRecord=0;
    bool zeroBegin=false;
    MmsResult Visit(Op op){
        ++calls[op];if(hook)hook(op);
        if(throwing==op){if(exception==1)throw std::bad_alloc();if(exception==2)throw std::length_error("test");throw std::runtime_error("test");}
        return errors[op];
    }
    template<class T> void Input(T* out){CHECK(out && out->h.size==sizeof(T) && out->h.major==1 && out->h.minor==0);}
    template<class T> void Corrupt(Op op,T& out){if(malformed==op)out.h.major=2;}
    Entry* Find(MmsHandle handle){auto it=entries.find(handle);return it==entries.end()?nullptr:&it->second;}
    Fake(){
        table.h=H<MmsUiLayoutSessionApiV1>();table.context=this;
        table.queryCapabilities=[](void* context,MmsUiLayoutSessionCapsV1* out)->MmsResult{
            auto& f=*static_cast<Fake*>(context);f.Input(out);const auto result=f.Visit(Caps);
            *out={H<MmsUiLayoutSessionCapsV1>(),7,f.table.h.minor?31u:15u,64,32,256,0};f.Corrupt(Caps,*out);
            if(f.badRecord==1)out->reserved=1;return result;
        };
        table.queryTarget=[](void* context,MmsHandle element,MmsUiLayoutSessionTargetInfoV1* out)->MmsResult{
            auto& f=*static_cast<Fake*>(context);f.Input(out);f.lastHandle=element;const auto result=f.Visit(Target);
            *out={H<MmsUiLayoutSessionTargetInfoV1>(),element,7,f.targetMount};f.Corrupt(Target,*out);
            if(f.badRecord==2)++out->element;return result;
        };
        table.begin=[](void* context,MmsSpan targets,MmsHandle* out)->MmsResult{
            auto& f=*static_cast<Fake*>(context);CHECK(out && !*out);const auto result=f.Visit(Begin);
            if(result){*out=99999;return result;}
            if(!targets.data || !targets.count || targets.stride!=sizeof(MmsUiLayoutSessionTargetV1) || targets.recordVersion!=1)return MMS_E_ARGUMENT;
            const auto* first=static_cast<const MmsUiLayoutSessionTargetV1*>(targets.data);
            for(uint64_t i=0;i<targets.count;++i)CHECK(first[i].h.size==sizeof(*first) && first[i].h.major==1 && !first[i].h.minor && !first[i].reserved);
            if(f.zeroBegin){*out=0;return MMS_OK;}
            Entry entry;entry.claims.assign(first,first+targets.count);const auto handle=++f.next;
            f.entries.emplace(handle,std::move(entry));*out=handle;return MMS_OK;
        };
        table.apply=[](void* context,MmsHandle handle,uint64_t revision,MmsSpan values)->MmsResult{
            auto& f=*static_cast<Fake*>(context);f.lastHandle=handle;f.lastRevision=revision;
            if(const auto result=f.Visit(Apply))return result;
            auto* entry=f.Find(handle);if(!entry)return MMS_E_NOT_FOUND;
            if(entry->state!=MMS_LAYOUT_SESSION_ACTIVE)return MMS_E_NOT_READY;
            if(entry->revision!=revision)return MMS_E_STALE;
            if(!values.data || values.stride!=sizeof(MmsUiLayoutSessionValueV1) || values.recordVersion!=1)return MMS_E_ARGUMENT;
            const auto* first=static_cast<const MmsUiLayoutSessionValueV1*>(values.data);size_t index=0;
            for(const auto& target:entry->claims)if(target.fields){
                if(index>=values.count || first[index].element!=target.element || first[index].fields!=target.fields)return MMS_E_ARGUMENT;
                CHECK(first[index].h.size==sizeof(*first) && first[index].h.major==1 && !first[index].h.minor && !first[index].reserved);
                ++index;
            }
            if(index!=values.count)return MMS_E_ARGUMENT;
            entry->values.assign(first,first+values.count);entry->revision+=7;return MMS_OK;
        };
        table.query=[](void* context,MmsHandle handle,MmsUiLayoutSessionInfoV1* out)->MmsResult{
            auto& f=*static_cast<Fake*>(context);f.Input(out);f.lastHandle=handle;const auto result=f.Visit(Query);
            if(result){*out={H<MmsUiLayoutSessionInfoV1>(),99999,0,0,1,1};return result;}
            const auto* entry=f.Find(handle);if(!entry)return MMS_E_NOT_FOUND;
            uint32_t active=0;for(const auto& claim:entry->claims)active+=claim.fields!=0;
            *out={H<MmsUiLayoutSessionInfoV1>(),entry->revision,entry->state,entry->reasons,uint32_t(entry->claims.size()),active};
            f.Corrupt(Query,*out);if(f.badRecord==3)out->activeTargetCount=out->targetCount+1;return MMS_OK;
        };
        table.queryClaim=[](void* context,MmsHandle handle,uint64_t revision,uint32_t index,MmsUiLayoutSessionTargetV1* out)->MmsResult{
            auto& f=*static_cast<Fake*>(context);f.Input(out);f.lastHandle=handle;f.lastRevision=revision;f.lastIndex=index;
            if(const auto result=f.Visit(Claim)){*out=LayoutSessionTarget(99999,7);return result;}
            const auto* entry=f.Find(handle);if(!entry)return MMS_E_NOT_FOUND;
            if(revision!=entry->revision)return MMS_E_STALE;if(index>=entry->claims.size())return MMS_E_ARGUMENT;
            *out=entry->claims[index];f.Corrupt(Claim,*out);if(f.badRecord==4)out->reserved=1;return MMS_OK;
        };
        table.rollback=[](void* context,MmsHandle handle,uint64_t revision)->MmsResult{return static_cast<Fake*>(context)->Finish(Rollback,handle,revision);};
        table.commit=[](void* context,MmsHandle handle,uint64_t revision)->MmsResult{return static_cast<Fake*>(context)->Finish(Commit,handle,revision);};
        table.release=[](void* context,MmsHandle handle)->MmsResult{
            auto& f=*static_cast<Fake*>(context);f.lastHandle=handle;if(const auto result=f.Visit(Release))return result;
            const auto* entry=f.Find(handle);if(!entry)return MMS_E_NOT_FOUND;
            if(entry->state==MMS_LAYOUT_SESSION_ACTIVE)return MMS_E_BUSY;
            f.entries.erase(handle);return MMS_OK;
        };
    }
    MmsResult Finish(Op op,MmsHandle handle,uint64_t revision){
        lastHandle=handle;lastRevision=revision;if(const auto result=Visit(op))return result;
        auto* entry=Find(handle);if(!entry)return MMS_E_NOT_FOUND;
        if(entry->state!=MMS_LAYOUT_SESSION_ACTIVE)return MMS_E_NOT_READY;
        if(entry->revision!=revision)return MMS_E_STALE;
        entry->state=op==Commit?MMS_LAYOUT_SESSION_COMMITTED:MMS_LAYOUT_SESSION_ROLLED_BACK;entry->revision+=7;
        for(auto& target:entry->claims)target.fields=0;return MMS_OK;
    }
    MmsHostV1 Host(){
        MmsHostV1 host{};host.h=H<MmsHostV1>();host.context=this;
        host.queryService=[](void* context,MmsString name,uint32_t major,uint32_t minor,uint32_t size,void* out)->MmsResult{
            auto& f=*static_cast<Fake*>(context);CHECK(name.bytes==21 && !std::memcmp(name.data,"mms.ui.layout.session",21));
            CHECK(major==1 && minor==f.requestedMinor && size==sizeof(MmsUiLayoutSessionApiV1));
            const auto result=f.Visit(Service);*static_cast<MmsUiLayoutSessionApiV1*>(out)=f.table;return result;
        };return host;
    }
    LayoutSessionServices Services(){LayoutSessionServices out;CHECK(LayoutSessionServices::Query(Host(),out)==MMS_OK);return out;}
};
static_assert(!std::is_copy_constructible_v<LayoutSession>);
static_assert(!std::is_copy_assignable_v<LayoutSession>);
static_assert(std::is_move_constructible_v<LayoutSession>);
static_assert(!std::is_move_assignable_v<LayoutSession>);

void ServiceAndOutputChecks(){
    Fake f;LayoutSessionServices service;LayoutSession empty;
    MmsUiLayoutSessionCapsV1 caps{};caps.reserved=77;const auto capsBefore=caps;
    CHECK(service.Capabilities(caps)==MMS_E_NOT_READY && Same(caps,capsBefore));
    auto host=f.Host();host.h.major=2;CHECK(LayoutSessionServices::Query(host,service)==MMS_E_VERSION && !service.ready());
    host=f.Host();host.queryService=nullptr;CHECK(LayoutSessionServices::Query(host,service)==MMS_E_ARGUMENT);
    f.errors[Service]=MMS_E_NOT_FOUND;CHECK(LayoutSessionServices::Query(f.Host(),service)==MMS_E_NOT_FOUND && !service.ready());
    f.errors[Service]=MMS_OK;service=f.Services();const auto original=service.api();
    f.table.h.size=8;CHECK(LayoutSessionServices::Query(f.Host(),service)==MMS_E_VERSION && service.api().context==original.context);
    f.table=original;f.table.h.minor=1;CHECK(LayoutSessionServices::Query(f.Host(),service)==MMS_E_VERSION && Same(service.api(),original));
    for(unsigned missing=0;missing<9;++missing){
        f.table=original;
        switch(missing){case 0:f.table.queryCapabilities=nullptr;break;case 1:f.table.queryTarget=nullptr;break;case 2:f.table.begin=nullptr;break;
            case 3:f.table.apply=nullptr;break;case 4:f.table.query=nullptr;break;case 5:f.table.queryClaim=nullptr;break;
            case 6:f.table.rollback=nullptr;break;case 7:f.table.commit=nullptr;break;case 8:f.table.release=nullptr;break;}
        CHECK(LayoutSessionServices::Query(f.Host(),service)==MMS_E_UNSUPPORTED && service.api().apply==original.apply);
    }f.table=original;
    for(int kind=0;kind<3;++kind){f.throwing=Service;f.exception=kind;CHECK(LayoutSessionServices::Query(f.Host(),service)==(kind?MMS_E_MEMORY:MMS_E_FAILED));}f.throwing=OpCount;
    CHECK(service.Capabilities(caps)==MMS_OK && caps.fields==7 && caps.maxTargets==64);
    auto goodCaps=caps;f.errors[Caps]=MMS_E_BUSY;CHECK(service.Capabilities(caps)==MMS_E_BUSY && Same(caps,goodCaps));f.errors[Caps]=MMS_OK;
    f.malformed=Caps;CHECK(service.Capabilities(caps)==MMS_E_VERSION && Same(caps,goodCaps));f.malformed=OpCount;
    f.badRecord=1;CHECK(service.Capabilities(caps)==MMS_E_FAILED && Same(caps,goodCaps));f.badRecord=0;
    MmsUiLayoutSessionTargetInfoV1 target{};CHECK(service.Target(23,target)==MMS_OK && target.element==23 && target.mount==1);auto targetBefore=target;
    f.badRecord=2;CHECK(service.Target(23,target)==MMS_E_FAILED && Same(target,targetBefore));f.badRecord=0;
    f.malformed=Target;CHECK(service.Target(23,target)==MMS_E_VERSION && Same(target,targetBefore));f.malformed=OpCount;
    CHECK(empty.Close()==MMS_OK && empty.Release()==MMS_OK);
}

void WorkspaceVersionChecks(){
    Fake f;auto legacy=f.Services();const auto before=legacy.api();
    const auto calls=f.calls[Service];
    CHECK(LayoutSessionServices::Query(f.Host(),legacy,3)==MMS_E_VERSION && Same(before,legacy.api()) && f.calls[Service]==calls);
    f.requestedMinor=1;f.errors[Service]=MMS_E_VERSION;
    CHECK(LayoutSessionServices::Query(f.Host(),legacy,1)==MMS_E_VERSION && Same(before,legacy.api()));
    f.errors[Service]=MMS_OK;f.table.h.minor=1;f.targetMount=16;
    LayoutSessionServices workspace;
    CHECK(LayoutSessionServices::Query(f.Host(),workspace,1)==MMS_OK && workspace.api().h.minor==1);
    MmsUiLayoutSessionCapsV1 caps{};MmsUiLayoutSessionTargetInfoV1 target{};
    CHECK(workspace.Capabilities(caps)==MMS_OK && caps.mounts==31 && !caps.h.minor);
    CHECK(workspace.Target(72,target)==MMS_OK && target.mount==16 && !target.h.minor);
    auto targetBefore=target;
    CHECK(legacy.Target(72,target)==MMS_E_FAILED && Same(target,targetBefore));
    LayoutSession session;CHECK(workspace.BeginVisibility(72,session)==MMS_OK);
    MmsUiLayoutSessionInfoV1 info{};CHECK(session.Query(info)==MMS_OK);
    auto value=LayoutSessionVisibility(72,false);CHECK(session.Apply(&value,1)==MMS_OK);
    CHECK(session.Query(info)==MMS_OK && session.Close()==MMS_OK);
    f.targetMount=32;CHECK(workspace.Target(72,target)==MMS_E_FAILED && Same(target,targetBefore));
    f.targetMount=17;CHECK(workspace.Target(72,target)==MMS_E_FAILED && Same(target,targetBefore));
    // The caller explicitly chooses a 1.0 fallback; Query never does so silently.
    f.requestedMinor=0;f.table.h.minor=0;
    CHECK(LayoutSessionServices::Query(f.Host(),workspace)==MMS_OK && !workspace.api().h.minor);
    CHECK(workspace.Capabilities(caps)==MMS_OK && caps.mounts==15);
}

void ClaimsAndRevisionChecks(){
    Fake f;auto service=f.Services();LayoutSession session;
    std::vector<MmsUiLayoutSessionTargetV1> targets{LayoutSessionTarget(21,3),LayoutSessionTarget(22,4)};
    CHECK(service.Begin(targets,session)==MMS_OK && session.get()==101 && session.revision()==0);
    CHECK(f.calls[Query]==0 && session.Close()==MMS_E_NOT_READY && f.calls[Rollback]==0);
    targets[0].element=999;CHECK(f.entries.at(session.get()).claims[0].element==21);
    const auto begins=f.calls[Begin];CHECK(service.BeginSize(44,session)==MMS_E_BUSY && f.calls[Begin]==begins && session.get()==101);
    MmsUiLayoutSessionInfoV1 info{};CHECK(session.Query(info)==MMS_OK && info.revision==11 && session.revision()==11 && !session.terminal());
    MmsUiLayoutSessionTargetV1 claim{};CHECK(session.QueryClaim(1,claim)==MMS_OK && claim.element==22 && claim.fields==4 && f.lastRevision==11 && f.lastIndex==1);
    auto first=LayoutSessionSize(21,180,52);first.fields|=MMS_LAYOUT_SESSION_VISIBLE;first.visible=1;
    std::vector<MmsUiLayoutSessionValueV1> values{first,LayoutSessionOrder(22,-10)};
    CHECK(session.Apply(values)==MMS_OK && f.lastHandle==101 && f.lastRevision==11 && session.revision()==0);
    CHECK(f.entries.at(101).values.size()==2 && Same(f.entries.at(101).values[0],first));
    const auto applies=f.calls[Apply];CHECK(session.Apply(values)==MMS_E_NOT_READY && f.calls[Apply]==applies);
    CHECK(session.Query(info)==MMS_OK && session.revision()==18);
    ++f.entries.at(101).revision;CHECK(session.Apply(values)==MMS_E_STALE && session.revision()==18 && f.calls[Apply]==applies+1);
    CHECK(session.Query(info)==MMS_OK && session.revision()==19);
    // Partial host takeover changes the original first claim, not its index.
    f.entries.at(101).claims[0].fields=0;f.entries.at(101).reasons=MMS_LAYOUT_SESSION_HOST_INTERACTION;++f.entries.at(101).revision;
    const auto before=claim;CHECK(session.QueryClaim(0,claim)==MMS_E_STALE && Same(claim,before));
    CHECK(session.Query(info)==MMS_OK && info.targetCount==2 && info.activeTargetCount==1 && info.reasons==MMS_LAYOUT_SESSION_HOST_INTERACTION);
    CHECK(session.QueryClaim(0,claim)==MMS_OK && claim.element==21 && !claim.fields);
    CHECK(session.Apply(values)==MMS_E_ARGUMENT && session.revision()==20);
    values.erase(values.begin());CHECK(session.Apply(20,values)==MMS_OK && session.revision()==0 && f.entries.at(101).values[0].element==22);
    CHECK(session.Query(info)==MMS_OK && session.revision()==27);
    CHECK(session.Commit()==MMS_OK && session.terminal() && !session.revision());
    f.errors[Release]=MMS_E_FAILED;CHECK(session.Close()==MMS_E_FAILED && session.get()==101 && session.terminal());
    const auto rollbacks=f.calls[Rollback];f.errors[Release]=MMS_OK;CHECK(session.Close()==MMS_OK && !session && f.calls[Rollback]==rollbacks);
}

void FailureAndReentryChecks(){
    Fake f;auto service=f.Services();LayoutSession session;
    f.errors[Begin]=MMS_E_FAILED;CHECK(service.BeginVisibility(31,session)==MMS_E_FAILED && !session && f.calls[Release]==0);f.errors[Begin]=MMS_OK;
    f.zeroBegin=true;CHECK(service.BeginOrder(31,session)==MMS_E_FAILED && !session);f.zeroBegin=false;
    f.throwing=Begin;f.exception=1;CHECK(service.BeginSize(31,session)==MMS_E_MEMORY && !session);f.throwing=OpCount;
    f.hook=[&](Op op){if(op!=Begin)return;LayoutSession moved(std::move(session));CHECK(!moved && !session);
        CHECK(service.BeginSize(32,session)==MMS_E_BUSY && session.Close()==MMS_E_BUSY && !session.Detach());};
    CHECK(service.BeginVisibility(31,session)==MMS_OK);f.hook={};
    MmsUiLayoutSessionInfoV1 info{};CHECK(session.Query(info)==MMS_OK);const auto infoBefore=info;
    f.errors[Query]=MMS_E_STALE;CHECK(session.Query(info)==MMS_E_STALE && Same(info,infoBefore) && session.revision()==11);f.errors[Query]=MMS_OK;
    f.malformed=Query;CHECK(session.Query(info)==MMS_E_VERSION && Same(info,infoBefore) && session.revision()==11);f.malformed=OpCount;
    f.badRecord=3;CHECK(session.Query(info)==MMS_E_FAILED && Same(info,infoBefore));f.badRecord=0;
    f.throwing=Query;f.exception=0;CHECK(session.Query(info)==MMS_E_FAILED && Same(info,infoBefore) && session.revision()==11);f.throwing=OpCount;
    MmsUiLayoutSessionTargetV1 claim{};CHECK(session.QueryClaim(0,claim)==MMS_OK);const auto oldClaim=claim;
    f.malformed=Claim;CHECK(session.QueryClaim(0,claim)==MMS_E_VERSION && Same(claim,oldClaim));f.malformed=OpCount;
    f.badRecord=4;CHECK(session.QueryClaim(0,claim)==MMS_E_FAILED && Same(claim,oldClaim));f.badRecord=0;
    auto hidden=LayoutSessionVisibility(31,false);CHECK(!hidden.visible && !hidden.width && !hidden.height && !hidden.order && !hidden.reserved);
    f.errors[Apply]=MMS_E_BUSY;CHECK(session.Apply(&hidden,1)==MMS_E_BUSY && session.revision()==11 && f.entries.at(101).values.empty());f.errors[Apply]=MMS_OK;
    f.errors[Rollback]=MMS_E_BUSY;CHECK(session.Close()==MMS_E_BUSY && session.get()==101 && !session.terminal() && session.revision()==11);f.errors[Rollback]=MMS_OK;
    f.errors[Commit]=MMS_E_STALE;CHECK(session.Commit()==MMS_E_STALE && session.revision()==11 && !session.terminal());f.errors[Commit]=MMS_OK;
    f.hook=[&](Op op){if(op!=Apply)return;CHECK(session.Release()==MMS_E_BUSY && session.Query(info)==MMS_E_BUSY && session.Commit()==MMS_E_BUSY);
        LayoutSession moved(std::move(session));CHECK(!moved && session.get()==101);CHECK(!session.Detach());};
    CHECK(session.Apply(&hidden,1)==MMS_OK);f.hook={};CHECK(session.Query(info)==MMS_OK && session.revision()==18);
    f.errors[Release]=MMS_E_BUSY;CHECK(session.Close()==MMS_E_BUSY && session.terminal() && session.get()==101 && f.calls[Rollback]==2);
    const auto rolls=f.calls[Rollback];f.errors[Release]=MMS_OK;
    f.hook=[&](Op op){if(op!=Release)return;CHECK(session.Close()==MMS_E_BUSY && session.Release()==MMS_E_BUSY);
        LayoutSession moved(std::move(session));CHECK(!moved && session.get()==101);};
    CHECK(session.Close()==MMS_OK && !session && f.calls[Rollback]==rolls);f.hook={};
}

void LifetimeAndThreadChecks(){
    Fake f,other;LayoutSession session;
    {
        auto service=f.Services();auto replacement=other.Services();
        f.hook=[&](Op op){if(op==Begin)service=replacement;};
        CHECK(service.BeginOrder(41,session)==MMS_OK);f.hook={};
        CHECK(session.api().context==&f && service.api().context==&other);
    }
    CHECK(session.get()==101 && f.calls[Release]==0 && other.calls[Begin]==0);
    LayoutSession moved(std::move(session));CHECK(!session && moved.get()==101);
    auto service=f.Services();LayoutSession occupied;CHECK(service.BeginSize(42,occupied)==MMS_OK);
    CHECK(occupied.TransferFrom(moved)==MMS_E_BUSY && moved.get()==101 && occupied.get()==102);
    LayoutSession transferred;CHECK(transferred.TransferFrom(moved)==MMS_OK && !moved && transferred.get()==101);
    MmsUiLayoutSessionInfoV1 info{};CHECK(transferred.Query(info)==MMS_OK);
    auto before=f.calls;std::exception_ptr workerError;std::thread worker([&]{try{
        MmsUiLayoutSessionInfoV1 result{};MmsUiLayoutSessionTargetV1 claim{};MmsUiLayoutSessionCapsV1 caps{};LayoutSession out;
        CHECK(transferred.Query(result)==MMS_E_THREAD && transferred.QueryClaim(0,claim)==MMS_E_THREAD);
        CHECK(transferred.Close()==MMS_E_THREAD && transferred.Release()==MMS_E_THREAD);
        CHECK(service.Capabilities(caps)==MMS_E_THREAD && service.BeginVisibility(99,out)==MMS_E_THREAD);
    }catch(...){workerError=std::current_exception();}});worker.join();if(workerError)std::rethrow_exception(workerError);
    CHECK(f.calls==before && transferred.get()==101);
    CHECK(transferred.Close()==MMS_OK);
    CHECK(occupied.Query(info)==MMS_OK);const auto api=occupied.api();const auto detached=occupied.Detach();
    CHECK(detached==102 && !occupied && api.rollback(api.context,detached,info.revision)==MMS_OK && api.release(api.context,detached)==MMS_OK);
    MmsHandle unreleased=0;{
        LayoutSession manual;CHECK(service.BeginVisibility(43,manual)==MMS_OK);unreleased=manual.get();
    }
    CHECK(f.entries.count(unreleased)==1); // The caller, not the destructor, owns explicit shutdown cleanup.
    CHECK(f.table.rollback(&f,unreleased,11)==MMS_OK && f.table.release(&f,unreleased)==MMS_OK);
}
void RevocationAndExplicitRevisionChecks(){
    Fake f;auto service=f.Services();LayoutSession session;
    CHECK(service.BeginOrder(51,session)==MMS_OK);
    // Explicit expected revisions are forwarded even without a cached Query.
    auto order=LayoutSessionOrder(51,9);CHECK(session.Apply(11,&order,1)==MMS_OK && f.lastRevision==11 && !session.revision());
    auto& entry=f.entries.at(session.get());entry.state=MMS_LAYOUT_SESSION_REVOKED;
    entry.reasons=MMS_LAYOUT_SESSION_STOPPING;entry.revision=99;entry.claims[0].fields=0;
    MmsUiLayoutSessionInfoV1 info{};CHECK(session.Query(info)==MMS_OK && session.terminal() && session.revision()==99);
    MmsUiLayoutSessionTargetV1 claim{};CHECK(session.QueryClaim(0,claim)==MMS_OK && claim.element==51 && !claim.fields);
    const auto rolls=f.calls[Rollback];CHECK(session.Close()==MMS_OK && f.calls[Rollback]==rolls);
    CHECK(service.BeginSize(52,session)==MMS_OK);
    CHECK(session.Rollback(11)==MMS_OK && session.terminal() && !session.revision());
    f.throwing=Release;f.exception=1;CHECK(session.Release()==MMS_E_MEMORY && session && session.terminal());f.throwing=OpCount;
    CHECK(session.Close()==MMS_OK && !session);
    const auto releases=f.calls[Release];CHECK(session.Close()==MMS_OK && session.Release()==MMS_OK && f.calls[Release]==releases);
}
}
int main(){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{ServiceAndOutputChecks();WorkspaceVersionChecks();ClaimsAndRevisionChecks();FailureAndReentryChecks();LifetimeAndThreadChecks();RevocationAndExplicitRevisionChecks();}
    catch(const std::exception& error){std::fprintf(stderr,"layout_session_cpp_tests failed after %u checks: %s\n",checks,error.what());return 1;}
    if(failures){std::fprintf(stderr,"layout_session_cpp_tests: %u assertions failed inside callbacks\n",failures);return 1;}
    std::printf("layout_session_cpp_tests: %u fake-host checks passed (no native UI tested)\n",checks);return 0;
}
