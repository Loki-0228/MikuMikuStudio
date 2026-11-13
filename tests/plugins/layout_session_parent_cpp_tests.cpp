// Fake-host CPU checks of the shared C++ 1.2 lifecycle. No native UI claim.
#include <mms/cpp/layout_session.hpp>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
using namespace mms::sdk;
namespace {
unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);throw std::runtime_error(#x);}}while(false)
template<class T>MmsHeader H(uint16_t minor=0){return {uint32_t(sizeof(T)),1,minor};}
template<class T>bool Same(const T& a,const T& b){return !std::memcmp(&a,&b,sizeof(T));}
enum Op{Service,Caps,CapsEx,Target,TargetEx,Begin,BeginEx,Apply,ApplyEx,Query,Claim,ClaimEx,Rollback,Commit,Release,Count};
struct Fake {
    struct Entry{uint64_t revision=7;uint32_t state=MMS_LAYOUT_SESSION_ACTIVE;MmsUiLayoutSessionTargetV1_2 claim{};};
    MmsUiLayoutSessionApiV1_2 table{};std::map<MmsHandle,Entry> entries;
    std::array<unsigned,Count> calls{};std::array<MmsResult,Count> errors{};std::function<void(Op)> hook;
    MmsHandle next=100;uint32_t lastMinor=99;bool badHeader=false,badParent=false,badWorkspace=false,zeroBegin=false;
    MmsResult Visit(Op op){++calls[op];if(hook)hook(op);return errors[op];}
    template<class T>void Input(T* out,uint16_t minor=0){CHECK(out&&out->h.size==sizeof(T)&&out->h.major==1&&out->h.minor==minor);}
    MmsResult Cap(bool ex,MmsUiLayoutSessionCapsV1* out){Input(out);const auto r=Visit(ex?CapsEx:Caps);
        *out={H<MmsUiLayoutSessionCapsV1>(),ex?15u:7u,31,64,32,256,0};if(badHeader)out->h.minor=2;return r;}
    MmsResult Tgt(bool ex,MmsHandle h,MmsUiLayoutSessionTargetInfoV1* out){Input(out);const auto r=Visit(ex?TargetEx:Target);
        *out={H<MmsUiLayoutSessionTargetInfoV1>(),h,ex?15u:7u,badWorkspace?16u:1u};if(badHeader)out->h.size=8;return r;}
    MmsResult Start(bool ex,MmsSpan targets,MmsHandle* out){CHECK(out&&!*out);const auto r=Visit(ex?BeginEx:Begin);
        if(r){*out=9999;return r;}CHECK(targets.data&&targets.count==1&&targets.recordVersion==(ex?2u:1u));
        Entry entry;if(ex){CHECK(targets.stride==sizeof(MmsUiLayoutSessionTargetV1_2));entry.claim=*static_cast<const MmsUiLayoutSessionTargetV1_2*>(targets.data);
            Input(&entry.claim,2);CHECK(!entry.claim.reserved&&((entry.claim.fields&8)?entry.claim.parent!=0:entry.claim.parent==0));}
        else{CHECK(targets.stride==sizeof(MmsUiLayoutSessionTargetV1));const auto old=*static_cast<const MmsUiLayoutSessionTargetV1*>(targets.data);
            CHECK(old.h.size==sizeof(old)&&old.h.major==1&&!old.h.minor&&!old.reserved);entry.claim=LayoutSessionTarget(old.element,old.fields,0);}
        if(zeroBegin)return MMS_OK;const auto h=++next;entries.emplace(h,entry);*out=h;return MMS_OK;}
    MmsResult Write(bool ex,MmsHandle h,uint64_t revision,MmsSpan values){const auto r=Visit(ex?ApplyEx:Apply);if(r)return r;
        auto it=entries.find(h);if(it==entries.end())return MMS_E_NOT_FOUND;auto& e=it->second;
        if(revision!=e.revision)return MMS_E_STALE;if(e.state!=MMS_LAYOUT_SESSION_ACTIVE)return MMS_E_NOT_READY;
        CHECK(values.data&&values.count==1&&values.recordVersion==(ex?2u:1u));
        if(ex){CHECK(values.stride==sizeof(MmsUiLayoutSessionValueV1_2));auto v=*static_cast<const MmsUiLayoutSessionValueV1_2*>(values.data);Input(&v,2);
            CHECK(v.element==e.claim.element&&v.fields==e.claim.fields&&!v.reserved);if(v.fields==8)CHECK(v.parent&&!v.visible&&!v.width&&!v.height&&!v.order);e.claim.parent=v.parent;}
        else{if(e.claim.fields&8)return MMS_E_UNSUPPORTED;CHECK(values.stride==sizeof(MmsUiLayoutSessionValueV1));}
        ++e.revision;return MMS_OK;}
    MmsResult Finish(Op op,MmsHandle h,uint64_t revision){if(auto r=Visit(op))return r;auto& e=entries.at(h);
        if(e.revision!=revision)return MMS_E_STALE;e.state=op==Commit?MMS_LAYOUT_SESSION_COMMITTED:MMS_LAYOUT_SESSION_ROLLED_BACK;
        e.claim.fields=0;e.claim.parent=0;++e.revision;return MMS_OK;}
    Fake(){table.h=H<MmsUiLayoutSessionApiV1_2>(2);table.context=this;
        table.queryCapabilities=[](void* c,MmsUiLayoutSessionCapsV1* o){return static_cast<Fake*>(c)->Cap(false,o);};
        table.queryCapabilitiesEx=[](void* c,MmsUiLayoutSessionCapsV1* o){return static_cast<Fake*>(c)->Cap(true,o);};
        table.queryTarget=[](void* c,MmsHandle h,MmsUiLayoutSessionTargetInfoV1* o){return static_cast<Fake*>(c)->Tgt(false,h,o);};
        table.queryTargetEx=[](void* c,MmsHandle h,MmsUiLayoutSessionTargetInfoV1* o){return static_cast<Fake*>(c)->Tgt(true,h,o);};
        table.begin=[](void* c,MmsSpan s,MmsHandle* o){return static_cast<Fake*>(c)->Start(false,s,o);};
        table.beginEx=[](void* c,MmsSpan s,MmsHandle* o){return static_cast<Fake*>(c)->Start(true,s,o);};
        table.apply=[](void* c,MmsHandle h,uint64_t r,MmsSpan s){return static_cast<Fake*>(c)->Write(false,h,r,s);};
        table.applyEx=[](void* c,MmsHandle h,uint64_t r,MmsSpan s){return static_cast<Fake*>(c)->Write(true,h,r,s);};
        table.query=[](void* c,MmsHandle h,MmsUiLayoutSessionInfoV1* o)->MmsResult{auto& f=*static_cast<Fake*>(c);f.Input(o);
            const auto r=f.Visit(Query);const auto& e=f.entries.at(h);*o={H<MmsUiLayoutSessionInfoV1>(),e.revision,e.state,0,1,e.claim.fields?1u:0u};return r;};
        table.queryClaimEx=[](void* c,MmsHandle h,uint64_t revision,uint32_t index,MmsUiLayoutSessionTargetV1_2* o)->MmsResult{
            auto& f=*static_cast<Fake*>(c);f.Input(o,2);const auto r=f.Visit(ClaimEx);const auto& e=f.entries.at(h);
            if(revision!=e.revision)return MMS_E_STALE;CHECK(!index);*o=e.claim;if(f.badHeader)o->h.minor=0;if(f.badParent)o->parent=0;return r;};
        table.queryClaim=[](void* c,MmsHandle h,uint64_t revision,uint32_t index,MmsUiLayoutSessionTargetV1* o)->MmsResult{
            auto& f=*static_cast<Fake*>(c);f.Input(o);const auto r=f.Visit(Claim);if(r)return r;const auto& e=f.entries.at(h);
            if(revision!=e.revision)return MMS_E_STALE;if(e.claim.fields&8)return MMS_E_UNSUPPORTED;CHECK(!index);*o=LayoutSessionTarget(e.claim.element,e.claim.fields);return MMS_OK;};
        table.rollback=[](void* c,MmsHandle h,uint64_t r){return static_cast<Fake*>(c)->Finish(Rollback,h,r);};
        table.commit=[](void* c,MmsHandle h,uint64_t r){return static_cast<Fake*>(c)->Finish(Commit,h,r);};
        table.release=[](void* c,MmsHandle h)->MmsResult{auto& f=*static_cast<Fake*>(c);if(auto r=f.Visit(Release))return r;
            if(f.entries.at(h).state==MMS_LAYOUT_SESSION_ACTIVE)return MMS_E_BUSY;f.entries.erase(h);return MMS_OK;};
    }
    MmsHostV1 Host(){MmsHostV1 host{};host.h=H<MmsHostV1>();host.context=this;
        host.queryService=[](void* c,MmsString id,uint32_t major,uint32_t minor,uint32_t size,void* out)->MmsResult{
            auto& f=*static_cast<Fake*>(c);CHECK(major==1&&minor<=2&&id.bytes==21&&!std::memcmp(id.data,"mms.ui.layout.session",21));
            f.lastMinor=minor;const auto result=f.Visit(Service);
            if(minor==2){CHECK(size==sizeof(f.table));std::memcpy(out,&f.table,size);}
            else{CHECK(size==sizeof(MmsUiLayoutSessionApiV1));MmsUiLayoutSessionApiV1 prefix{};std::memcpy(&prefix,&f.table,sizeof prefix);
                prefix.h={sizeof prefix,1,static_cast<uint16_t>(minor)};std::memcpy(out,&prefix,sizeof prefix);}
            return result;};return host;}
    LayoutSessionServices Services(uint32_t minor=2){LayoutSessionServices result;CHECK(LayoutSessionServices::Query(Host(),result,minor)==MMS_OK);return result;}
};
void Versions(){Fake f;LayoutSessionServices service;CHECK(LayoutSessionServices::Query(f.Host(),service)==MMS_OK&&f.lastMinor==0&&!service.extended_api());
    MmsUiLayoutSessionCapsV1 caps{};caps.reserved=23;auto capsBefore=caps;MmsUiLayoutSessionTargetInfoV1 target{};target.element=93;auto targetBefore=target;
    LayoutSession session;const auto calls=f.calls;CHECK(service.CapabilitiesEx(caps)==MMS_E_UNSUPPORTED&&Same(caps,capsBefore));
    CHECK(service.TargetEx(3,target)==MMS_E_UNSUPPORTED&&Same(target,targetBefore));CHECK(service.BeginParent(3,4,session)==MMS_E_UNSUPPORTED&&!session&&f.calls==calls);
    CHECK(service.BeginVisibility(3,session)==MMS_OK);MmsUiLayoutSessionInfoV1 info{};CHECK(session.Query(info)==MMS_OK);
    auto value=LayoutSessionParent(3,4);MmsUiLayoutSessionTargetV1_2 claim=LayoutSessionTarget(999,8,888);const auto claimBefore=claim;const auto beforeEx=f.calls;
    CHECK(session.Apply(&value,1)==MMS_E_UNSUPPORTED&&session.QueryClaim(0,claim)==MMS_E_UNSUPPORTED&&Same(claim,claimBefore)&&f.calls==beforeEx);CHECK(session.Close()==MMS_OK);
    service=f.Services();const auto valid=*service.extended_api();
    CHECK(service.api().h.size==sizeof(MmsUiLayoutSessionApiV1)&&service.api().h.minor==1&&
        valid.h.size==sizeof(MmsUiLayoutSessionApiV1_2)&&valid.h.minor==2);
    for(int bad=0;bad<3;++bad){f.table=valid;if(bad==0)f.table.h.size=88;if(bad==1)f.table.h.major=2;if(bad==2)f.table.h.minor=1;
        CHECK(LayoutSessionServices::Query(f.Host(),service,2)==MMS_E_VERSION&&Same(*service.extended_api(),valid));}
    for(int missing=0;missing<14;++missing){f.table=valid;switch(missing){case 0:f.table.queryCapabilities=nullptr;break;case 1:f.table.queryTarget=nullptr;break;
        case 2:f.table.begin=nullptr;break;case 3:f.table.apply=nullptr;break;case 4:f.table.query=nullptr;break;case 5:f.table.queryClaim=nullptr;break;
        case 6:f.table.rollback=nullptr;break;case 7:f.table.commit=nullptr;break;case 8:f.table.release=nullptr;break;
        case 9:f.table.queryCapabilitiesEx=nullptr;break;case 10:f.table.queryTargetEx=nullptr;break;case 11:f.table.beginEx=nullptr;break;
        case 12:f.table.applyEx=nullptr;break;case 13:f.table.queryClaimEx=nullptr;break;}
        CHECK(LayoutSessionServices::Query(f.Host(),service,2)==MMS_E_UNSUPPORTED&&Same(*service.extended_api(),valid));}
    f.table=valid;f.errors[Service]=MMS_E_VERSION;CHECK(LayoutSessionServices::Query(f.Host(),service,2)==MMS_E_VERSION&&Same(*service.extended_api(),valid));f.errors[Service]=MMS_OK;
    const auto beforeUnknown=f.calls;CHECK(LayoutSessionServices::Query(f.Host(),service,3)==MMS_E_VERSION&&f.calls==beforeUnknown);
    CHECK(service.Capabilities(caps)==MMS_OK&&caps.fields==7&&service.CapabilitiesEx(caps)==MMS_OK&&caps.fields==15&&caps.h.minor==0);
    capsBefore=caps;f.badHeader=true;CHECK(service.CapabilitiesEx(caps)==MMS_E_VERSION&&Same(caps,capsBefore));f.badHeader=false;
    CHECK(service.TargetEx(3,target)==MMS_OK&&target.fields==15&&target.h.minor==0);targetBefore=target;f.badWorkspace=true;
    CHECK(service.TargetEx(3,target)==MMS_E_FAILED&&Same(target,targetBefore));f.badWorkspace=false;
    for(auto op:{CapsEx,TargetEx}){f.errors[op]=MMS_E_BUSY;if(op==CapsEx)CHECK(service.CapabilitiesEx(caps)==MMS_E_BUSY&&Same(caps,capsBefore));
        else CHECK(service.TargetEx(3,target)==MMS_E_BUSY&&Same(target,targetBefore));f.errors[op]=MMS_OK;}
}
void Lifecycle(){Fake f;auto services=f.Services();LayoutSession session;CHECK(services.BeginParent(3,4,session)==MMS_OK&&session.extended_api()&&session.revision()==0);
    CHECK(session.api().h.size==88&&session.api().h.minor==1&&session.extended_api()->h.size==128&&session.extended_api()->h.minor==2);
    MmsUiLayoutSessionInfoV1 info{};CHECK(session.Query(info)==MMS_OK);MmsUiLayoutSessionTargetV1_2 claim{};CHECK(session.QueryClaim(0,claim)==MMS_OK&&claim.parent==4&&claim.h.minor==2);
    MmsUiLayoutSessionTargetV1 old=LayoutSessionTarget(88,1);const auto oldBefore=old;CHECK(session.QueryClaim(0,old)==MMS_E_UNSUPPORTED&&Same(old,oldBefore));
    const auto before=claim;f.errors[ClaimEx]=MMS_E_BUSY;CHECK(session.QueryClaim(0,claim)==MMS_E_BUSY&&Same(claim,before));f.errors[ClaimEx]=MMS_OK;
    f.badHeader=true;CHECK(session.QueryClaim(0,claim)==MMS_E_VERSION&&Same(claim,before));f.badHeader=false;f.badParent=true;
    CHECK(session.QueryClaim(0,claim)==MMS_E_FAILED&&Same(claim,before));f.badParent=false;
    const auto values=std::vector<MmsUiLayoutSessionValueV1_2>{LayoutSessionParent(3,5)};f.errors[ApplyEx]=MMS_E_BUSY;
    const auto revision=session.revision();CHECK(session.Apply(values)==MMS_E_BUSY&&session.revision()==revision);f.errors[ApplyEx]=MMS_OK;
    CHECK(session.Apply(values)==MMS_OK&&!session.revision());const auto calls=f.calls;
    CHECK(session.Apply(values)==MMS_E_NOT_READY&&session.Close()==MMS_E_NOT_READY&&f.calls==calls);
    CHECK(session.Query(info)==MMS_OK&&session.QueryClaim(0,claim)==MMS_OK&&claim.parent==5);
    CHECK(session.Commit()==MMS_OK&&session.terminal());CHECK(session.Query(info)==MMS_OK&&session.QueryClaim(0,claim)==MMS_OK&&!claim.fields&&!claim.parent);
    CHECK(session.QueryClaim(0,old)==MMS_OK&&!old.fields&&old.element==3);CHECK(session.Close()==MMS_OK&&!session&&!session.extended_api());
    // A 1.2-negotiated session begun through the original prefix still shares
    // the same handle and can be inspected/applied through Ex records.
    CHECK(services.BeginVisibility(7,session)==MMS_OK&&session.extended_api());CHECK(session.Query(info)==MMS_OK&&session.QueryClaim(0,claim)==MMS_OK&&!claim.parent&&claim.fields==1);
    MmsUiLayoutSessionValueV1_2 visibility{};visibility.h=H<MmsUiLayoutSessionValueV1_2>(2);visibility.element=7;visibility.fields=1;visibility.visible=1;
    CHECK(session.Apply(&visibility,1)==MMS_OK&&session.Query(info)==MMS_OK&&session.Close()==MMS_OK);
}
void OwnershipAndReentry(){Fake f;auto services=f.Services();LayoutSession source,destination;MmsUiLayoutSessionInfoV1 info{};
    f.errors[BeginEx]=MMS_E_BUSY;CHECK(services.BeginParent(3,4,source)==MMS_E_BUSY&&!source&&!source.extended_api());f.errors[BeginEx]=MMS_OK;
    f.zeroBegin=true;CHECK(services.BeginParent(3,4,source)==MMS_E_FAILED&&!source);f.zeroBegin=false;
    f.hook=[&](Op op){if(op==BeginEx){CHECK(source.Close()==MMS_E_BUSY&&!source.Detach()&&destination.TransferFrom(source)==MMS_E_BUSY);}};
    CHECK(services.BeginParent(3,4,source)==MMS_OK);f.hook={};CHECK(source.Query(info)==MMS_OK);
    auto value=LayoutSessionParent(3,5);f.hook=[&](Op op){if(op==ApplyEx){MmsUiLayoutSessionTargetV1_2 out{};CHECK(source.QueryClaim(0,out)==MMS_E_BUSY&&source.Close()==MMS_E_BUSY&&destination.TransferFrom(source)==MMS_E_BUSY);}};
    CHECK(source.Apply(&value,1)==MMS_OK);f.hook={};CHECK(source.Query(info)==MMS_OK);
    const auto observed=source.revision();f.hook=[](Op op){if(op==ApplyEx)throw std::bad_alloc();};
    CHECK(source.Apply(&value,1)==MMS_E_MEMORY&&source.revision()==observed&&source.extended_api());f.hook={};
    MmsUiLayoutSessionTargetV1_2 output=LayoutSessionTarget(91,8,92);const auto savedOutput=output;
    f.hook=[](Op op){if(op==ClaimEx)throw std::runtime_error("read failure");};
    CHECK(source.QueryClaim(0,output)==MMS_E_FAILED&&Same(output,savedOutput)&&source.revision()==observed);f.hook={};
    MmsResult worker=MMS_OK;const auto beforeThread=f.calls;std::thread thread([&]{worker=source.Apply(&value,1);});thread.join();CHECK(worker==MMS_E_THREAD&&f.calls==beforeThread);
    CHECK(destination.TransferFrom(source)==MMS_OK&&!source&&!source.extended_api()&&destination.extended_api());
    const auto api=*destination.extended_api();const auto h=destination.Detach();CHECK(h&&!destination&&!destination.extended_api());
    MmsUiLayoutSessionInfoV1 raw{};raw.h=H<MmsUiLayoutSessionInfoV1>();CHECK(api.query(api.context,h,&raw)==MMS_OK&&api.rollback(api.context,h,raw.revision)==MMS_OK&&api.release(api.context,h)==MMS_OK);
    MmsHandle orphan=0;MmsUiLayoutSessionApiV1_2 saved{};unsigned releases=0;
    {LayoutSession local;CHECK(services.BeginParent(8,9,local)==MMS_OK);orphan=local.get();saved=*local.extended_api();releases=f.calls[Release];}
    CHECK(f.calls[Release]==releases&&f.entries.count(orphan));raw.h=H<MmsUiLayoutSessionInfoV1>();
    CHECK(saved.query(saved.context,orphan,&raw)==MMS_OK&&saved.rollback(saved.context,orphan,raw.revision)==MMS_OK&&saved.release(saved.context,orphan)==MMS_OK);
    CHECK(services.BeginParent(3,4,source)==MMS_OK&&source.Query(info)==MMS_OK);f.errors[Release]=MMS_E_BUSY;
    CHECK(source.Close()==MMS_E_BUSY&&source.terminal()&&source.extended_api());const auto rollbacks=f.calls[Rollback];f.errors[Release]=MMS_OK;
    CHECK(source.Close()==MMS_OK&&f.calls[Rollback]==rollbacks&&!source.extended_api());
}
}
int main(){try{Versions();Lifecycle();OwnershipAndReentry();std::printf("layout_session_parent_cpp_tests PASS: %u fake-host checks\n",checks);return 0;}
    catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
