// MAINTENANCE: regression-keep. SDK forwarding/lease contracts use only a
// deterministic C table, ordinary memory and std::thread; no native UI exists.
#include <mms/cpp/ui_catalog.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace sdk=mms::sdk;
namespace {
unsigned checks=0;
const char* stage="setup";
void Check(bool condition,const char* message){++checks;if(!condition){std::fprintf(stderr,"FAIL [%s] %s\n",stage,message);std::exit(1);}}
template<class T> constexpr MmsHeader H(){return {sizeof(T),1,0};}
template<class T> bool Same(const T& a,const T& b){return !std::memcmp(&a,&b,sizeof(T));}
MmsString S(const char* value){return {value,std::strlen(value)};}
enum class SnapshotBad {none,header,noHandle,scopes,epoch,nullData,pointerAlignment,stride,strideAlignment,countOverflow,recordVersion,rowHeader,wrongTarget,registrationHeader,string};
enum class EntryBad {none,header,noLease,wrongElement,wrongInstance,wrongEpoch,string,unpresented};
struct Fake {
    unsigned queries=0,caps=0,acquires=0,resolves=0,releases=0;
    MmsUiCatalogApiV1 table{};
    MmsUiCatalogEntryV1 entries[2]{};
    struct ExtendedEntry { MmsUiCatalogEntryV1 entry; uint64_t tail[2]; } extendedEntries[2]{};
    MmsUiCatalogRegistrationV1 registrations[1]{};
    MmsHandle next=1000,lastIssued=0,lastReleased=0;
    std::set<MmsHandle> live;
    uint32_t scopesSeen=0,queryBytes=0,queryMajor=0,queryMinor=0;
    MmsUiTargetRefV1 expectedSeen{};
    MmsResult queryResult=MMS_OK,capsResult=MMS_OK,acquireResult=MMS_OK,resolveResult=MMS_OK,releaseResult=MMS_OK;
    unsigned queryThrow=0,capThrow=0,releaseThrow=0;
    bool badCaps=false;
    bool emptySnapshot=false,extendedSnapshot=false;
    SnapshotBad snapshotBad=SnapshotBad::none;
    EntryBad entryBad=EntryBad::none;
    Fake(){
        table={H<MmsUiCatalogApiV1>(),this,Capabilities,Acquire,Resolve,Release};
        for(unsigned i=0;i<2;++i){auto& e=entries[i];e.h=H<MmsUiCatalogEntryV1>();e.element=9001+i;
            e.id=S(i?"org.test.catalog.button":"mms.catalog.main.control.455");e.ownerPlugin=S(i?"org.test.catalog":"mms");
            e.parentId=S("mms.toolbar.main.extensions");e.label=S(i?"Original button":"Light red");
            e.descriptionGeneration=53+i;e.target={H<MmsUiTargetRefV1>(),e.element,61+i,71};
            e.bounds={1,2,3,4};e.kind=MMS_UI_BUTTON;e.origin=i?MMS_UI_CATALOG_REGISTERED:MMS_UI_CATALOG_NATIVE_OBSERVATION;
            e.scope=i?MMS_UI_CATALOG_REGISTRY:MMS_UI_CATALOG_MAIN_CONTROLS;
            e.knownFields=MMS_UI_CATALOG_LABEL|MMS_UI_CATALOG_BOUNDS|MMS_UI_CATALOG_PRESENTATION;
            e.role=i?MMS_UI_CATALOG_ROLE_BUTTON:MMS_UI_CATALOG_ROLE_SLIDER;
            extendedEntries[i].entry=e;extendedEntries[i].entry.h.size=sizeof(ExtendedEntry);extendedEntries[i].entry.h.minor=1;}
        registrations[0]={H<MmsUiCatalogRegistrationV1>(),8001,S("org.test.catalog.invoke"),S("org.test.catalog"),S("Original command"),MMS_UI_CATALOG_COMMAND,MMS_COMMAND_INVOKE_ONLY};
    }
    MmsHostV1 Host(){MmsHostV1 host{};host.h=H<MmsHostV1>();host.context=this;host.queryService=Query;return host;}
    unsigned Calls()const{return queries+caps+acquires+resolves+releases;}
    static Fake& Get(void* p){return *static_cast<Fake*>(p);}
    static void Throw(unsigned kind){if(kind==1)throw std::bad_alloc();if(kind==2)throw std::runtime_error("owned fake boundary exception");}
    static MmsResult MMS_CALL Query(void* context,MmsString name,uint32_t major,uint32_t minor,uint32_t bytes,void* out){
        auto& f=Get(context);++f.queries;f.queryBytes=bytes;f.queryMajor=major;f.queryMinor=minor;
        Check(name.bytes==14&&!std::memcmp(name.data,"mms.ui.catalog",14),"SDK queries exactly the original catalog service only");
        Throw(f.queryThrow);Check(bytes==sizeof(MmsUiCatalogApiV1),"SDK requests only the known C table prefix");
        *static_cast<MmsUiCatalogApiV1*>(out)=f.table;return f.queryResult;
    }
    static MmsResult MMS_CALL Capabilities(void* context,MmsUiCatalogCapabilitiesV1* out){
        auto& f=Get(context);++f.caps;Check(out&&out->h.size==sizeof(*out)&&out->h.major==1,"SDK initializes the original capabilities output header");Throw(f.capThrow);
        *out={H<MmsUiCatalogCapabilitiesV1>(),7,16,64,1024*1024,2*1024*1024,8*1024*1024,4096,0};
        if(f.badCaps)out->h.major=2;return f.capsResult;
    }
    static MmsResult MMS_CALL Acquire(void* context,uint32_t scopes,MmsUiCatalogSnapshotV1* out){
        auto& f=Get(context);++f.acquires;f.scopesSeen=scopes;
        Check(out&&out->h.size==sizeof(*out)&&out->h.major==1,"SDK initializes the original snapshot output header");
        *out={H<MmsUiCatalogSnapshotV1>(),0,11,12,71,scopes,7,15,0,{f.entries,2,sizeof(f.entries[0]),1},{f.registrations,1,sizeof(f.registrations[0]),1}};
        if(f.emptySnapshot){out->elements={};out->registrations={};}
        if(f.extendedSnapshot)out->elements={f.extendedEntries,2,sizeof(f.extendedEntries[0]),1};
        if(f.acquireResult){out->handle=0xfafafafa;return f.acquireResult;}
        if(f.snapshotBad!=SnapshotBad::noHandle){out->handle=++f.next;f.lastIssued=out->handle;f.live.insert(out->handle);}
        switch(f.snapshotBad){
        case SnapshotBad::header:out->h.size=8;break;
        case SnapshotBad::scopes:out->requestedScopes^=1;break;
        case SnapshotBad::epoch:out->projectEpoch=0;break;
        case SnapshotBad::nullData:out->elements.data=nullptr;break;
        case SnapshotBad::pointerAlignment:out->elements.data=reinterpret_cast<const unsigned char*>(f.entries)+1;break;
        case SnapshotBad::stride:out->elements.stride=1;break;
        case SnapshotBad::strideAlignment:out->elements.stride=sizeof(MmsUiCatalogEntryV1)+1;break;
        case SnapshotBad::countOverflow:out->elements.count=(std::numeric_limits<uint64_t>::max)();break;
        case SnapshotBad::recordVersion:out->elements.recordVersion=2;break;
        case SnapshotBad::rowHeader:f.entries[1].h.major=2;break;
        case SnapshotBad::wrongTarget:f.entries[1].target.element=7654;break;
        case SnapshotBad::registrationHeader:f.registrations[0].h.size=8;break;
        case SnapshotBad::string:f.entries[0].id={nullptr,8};break;
        default:break;}
        return MMS_OK;
    }
    static MmsResult MMS_CALL Resolve(void* context,const MmsUiTargetRefV1* expected,MmsUiCatalogEntryV1* out,MmsHandle* lease){
        auto& f=Get(context);++f.resolves;Check(expected&&out&&lease,"SDK forwards typed original resolve outputs");f.expectedSeen=*expected;
        Check(out->h.size==sizeof(*out)&&out->h.major==1,"SDK initializes the original entry output header");
        *out=f.entries[1];*lease=0;
        if(f.resolveResult){out->element=0xfafafafa;*lease=0xfbfbfbfb;return f.resolveResult;}
        if(f.entryBad!=EntryBad::noLease){*lease=++f.next;f.lastIssued=*lease;f.live.insert(*lease);}
        switch(f.entryBad){
        case EntryBad::header:out->h.major=2;break;
        case EntryBad::wrongElement:out->element=9999;out->target.element=9999;break;
        case EntryBad::wrongInstance:++out->target.instanceGeneration;break;
        case EntryBad::wrongEpoch:++out->target.projectEpoch;break;
        case EntryBad::string:out->ownerPlugin={nullptr,7};break;
        case EntryBad::unpresented:out->knownFields&=~MMS_UI_CATALOG_PRESENTATION;break;
        default:break;}
        return MMS_OK;
    }
    static MmsResult MMS_CALL Release(void* context,MmsHandle handle){
        auto& f=Get(context);++f.releases;f.lastReleased=handle;Throw(f.releaseThrow);
        if(f.releaseResult)return f.releaseResult;
        if(!f.live.erase(handle))return MMS_E_NOT_FOUND;return MMS_OK;
    }
};
sdk::CatalogServices Services(Fake& f){sdk::CatalogServices out;Check(sdk::CatalogServices::Query(f.Host(),out)==MMS_OK,"negotiate original fake C service");return out;}

void NegotiationAndCapabilities(){
    Fake original;auto service=Services(original);const auto table=service.api();
    Check(service.ready()&&service.CheckThread()==MMS_OK&&table.context==&original&&original.queries==1&&original.queryMajor==1&&original.queryMinor==0,
        "Query retains the exact negotiated context/table on its UI thread");
    for(unsigned kind=0;kind<10;++kind){
        Fake bad;auto host=bad.Host();MmsResult expected=MMS_E_UNSUPPORTED;
        switch(kind){
        case 0:host.h.size=0;expected=MMS_E_VERSION;break;
        case 1:host.h.major=2;expected=MMS_E_VERSION;break;
        case 2:host.queryService=nullptr;expected=MMS_E_ARGUMENT;break;
        case 3:bad.table.h.size=8;expected=MMS_E_VERSION;break;
        case 4:bad.table.h.major=2;expected=MMS_E_VERSION;break;
        case 5:bad.table.getCapabilities=nullptr;break;
        case 6:bad.table.acquireSnapshot=nullptr;break;
        case 7:bad.table.resolveElement=nullptr;break;
        case 8:bad.table.releaseSnapshot=nullptr;break;
        case 9:bad.queryResult=MMS_E_NOT_READY;expected=MMS_E_NOT_READY;break;}
        Check(sdk::CatalogServices::Query(host,service)==expected,"bad table/header/negotiation is rejected precisely");
        Check(Same(service.api(),table)&&service.ready()&&service.CheckThread()==MMS_OK,"failed Query preserves the previous usable service");
    }
    for(unsigned throwing:{1u,2u}){Fake bad;bad.queryThrow=throwing;
        Check(sdk::CatalogServices::Query(bad.Host(),service)==(throwing==1?MMS_E_MEMORY:MMS_E_FAILED)&&Same(service.api(),table),
            "C boundary exceptions do not escape or overwrite a negotiated output");}
    Fake newer;newer.table.h.minor=3;newer.table.h.size+=16;sdk::CatalogServices prefix;
    Check(sdk::CatalogServices::Query(newer.Host(),prefix)==MMS_OK&&newer.queryBytes==sizeof(MmsUiCatalogApiV1),"newer minor table uses only the requested compatible prefix");
    MmsUiCatalogCapabilitiesV1 caps{};Check(service.Capabilities(caps)==MMS_OK&&caps.supportedScopes==7&&original.caps==1,"Capabilities forwards exactly one original call");
    const auto saved=caps;original.capsResult=MMS_E_BUSY;
    Check(service.Capabilities(caps)==MMS_E_BUSY&&Same(caps,saved),"failed capabilities preserves caller output despite fake scribbles");
    original.capsResult=MMS_OK;original.badCaps=true;
    Check(service.Capabilities(caps)==MMS_E_FAILED&&Same(caps,saved),"malformed successful capabilities does not replace caller output");
    original.badCaps=false;original.capThrow=1;
    Check(service.Capabilities(caps)==MMS_E_MEMORY&&Same(caps,saved),"capabilities exception maps to MMS error without replacing output");
    sdk::CatalogServices absent;Check(absent.Capabilities(caps)==MMS_E_NOT_READY&&Same(caps,saved),"unqueried service has no callable table");
}

void IdentityViewsMoveAndRelease(){
    Fake f;auto service=Services(f);
    for(uint32_t scope:{1u,2u,4u,7u}){
        sdk::CatalogSnapshot snapshot;Check(service.Acquire(scope,snapshot)==MMS_OK&&f.scopesSeen==scope,"Acquire passes the caller's exact declared scope mask");
        Check(snapshot.get()==f.lastIssued&&snapshot.value().handle==f.lastIssued&&snapshot.Elements().data==f.entries&&snapshot.Registrations().data==f.registrations,
            "lease handle and immutable spans are the original C results without copied records");
        Check(snapshot.Element(0)==&f.entries[0]&&snapshot.Element(1)==&f.entries[1]&&!snapshot.Element(2)&&snapshot.Registration(0)==&f.registrations[0]&&!snapshot.Registration(1),
            "read-only bounded access addresses original host records and original registration handles");
        const auto calls=f.Calls();const auto before=snapshot.value();
        Check(service.Acquire(7,snapshot)==MMS_E_BUSY&&f.Calls()==calls&&Same(snapshot.value(),before),"Acquire never releases or overwrites an already owned output");
        sdk::CatalogSnapshot moved(std::move(snapshot));
        Check(!snapshot&&snapshot.value().elements.data==nullptr&&moved.get()==before.handle,"move transfers exactly one original snapshot lease");
        Check(moved.Release()==MMS_OK&&!moved&&f.lastReleased==before.handle&&f.live.empty(),"explicit release returns the original lease once");
        const auto released=f.releases;Check(snapshot.Release()==MMS_OK&&moved.Release()==MMS_OK&&f.releases==released,"empty release performs no C call");
    }
    f.emptySnapshot=true;
    {sdk::CatalogSnapshot empty;Check(service.Acquire(1,empty)==MMS_OK&&empty.Elements().count==0&&empty.Registrations().count==0&&!empty.Element(0)&&!empty.Registration(0),
        "valid empty original spans stay empty and expose no fabricated records");Check(empty.Release()==MMS_OK,"release valid empty snapshot through the same table");}
    f.emptySnapshot=false;f.extendedSnapshot=true;
    {sdk::CatalogSnapshot extended;Check(service.Acquire(7,extended)==MMS_OK&&extended.Elements().data==f.extendedEntries&&extended.Element(0)==&f.extendedEntries[0].entry&&extended.Element(1)==&f.extendedEntries[1].entry,
        "newer compatible record prefixes use the original host stride without copying or guessing contiguity");Check(extended.Release()==MMS_OK,"release extended-prefix snapshot normally");}
    f.extendedSnapshot=false;
    sdk::CatalogEntryLease entry;const auto expected=f.entries[1].target;
    Check(service.Resolve(expected,entry)==MMS_OK&&Same(f.expectedSeen,expected)&&entry.value().element==9002&&entry.get()==f.lastIssued&&entry.value().id.data==f.entries[1].id.data,
        "Resolve forwards original reference and owns only the original returned lease");
    const auto before=entry.value();const auto handle=entry.get();const auto calls=f.Calls();
    Check(service.Resolve(expected,entry)==MMS_E_BUSY&&f.Calls()==calls&&entry.get()==handle&&Same(entry.value(),before),"Resolve cannot silently replace an existing entry lease");
    sdk::CatalogEntryLease moved(std::move(entry));Check(!entry&&moved.get()==handle&&Same(moved.value(),before),"entry move preserves original metadata and singular ownership");
    for(MmsResult failure:{MMS_E_BUSY,MMS_E_NOT_READY,MMS_E_NOT_FOUND,MMS_E_STALE,MMS_E_FAILED}){
        f.releaseResult=failure;Check(moved.Release()==failure&&moved.get()==handle&&Same(moved.value(),before)&&f.live.count(handle),"every explicit release failure retains the original retryable entry handle");}
    f.releaseResult=MMS_OK;f.releaseThrow=1;Check(moved.Release()==MMS_E_MEMORY&&moved.get()==handle&&f.live.count(handle),"release exception also retains entry ownership");
    f.releaseThrow=0;Check(moved.Release()==MMS_OK&&!moved&&f.live.empty(),"entry release succeeds after a stop-boundary retry");
    sdk::CatalogSnapshot snapshot;Check(service.Acquire(7,snapshot)==MMS_OK,"seed snapshot release retry");const auto snapshotBefore=snapshot.value();
    f.releaseResult=MMS_E_NOT_FOUND;Check(snapshot.Release()==MMS_E_NOT_FOUND&&Same(snapshot.value(),snapshotBefore),"snapshot release retains its entire view on failure");
    f.releaseResult=MMS_OK;Check(snapshot.Release()==MMS_OK&&f.live.empty(),"snapshot release can be retried on the original table");
    const auto releases=f.releases;{sdk::CatalogSnapshot automatic;Check(service.Acquire(7,automatic)==MMS_OK,"seed destructor release");}
    Check(f.releases==releases+1&&f.live.empty(),"UI-thread destructor makes one best-effort release on the same service");
    f.releaseResult=MMS_E_BUSY;const auto attempted=f.releases;MmsHandle abandoned=0;
    {sdk::CatalogSnapshot premature;Check(service.Acquire(7,premature)==MMS_OK,"seed failed best-effort destructor");abandoned=premature.get();}
    Check(f.releases==attempted+1&&f.live.count(abandoned),"destructor failure is one best-effort attempt, not a claim that rejected release reclaimed host memory");
    f.releaseResult=MMS_OK;Check(Fake::Release(&f,abandoned)==MMS_OK,"fake host explicitly reclaims its own lease after intentionally premature destruction");
    Check(f.queries==1,"SDK operations negotiate no second registry or additional service");
}

void FailedOutputsAndBadSuccessCleanup(){
    Fake failed;auto service=Services(failed);sdk::CatalogSnapshot snapshot;sdk::CatalogEntryLease entry;
    const auto emptySnapshot=snapshot.value();const auto emptyEntry=entry.value();
    failed.acquireResult=MMS_E_STALE;failed.resolveResult=MMS_E_NOT_FOUND;
    Check(service.Acquire(7,snapshot)==MMS_E_STALE&&!snapshot&&Same(snapshot.value(),emptySnapshot),"failed C acquire scribbles remain private and preserve SDK output");
    Check(service.Resolve(failed.entries[1].target,entry)==MMS_E_NOT_FOUND&&!entry&&Same(entry.value(),emptyEntry)&&failed.releases==0,
        "failed C resolve does not publish or release an untrusted failure-output handle");
    for(auto bad:{SnapshotBad::header,SnapshotBad::noHandle,SnapshotBad::scopes,SnapshotBad::epoch,SnapshotBad::nullData,SnapshotBad::pointerAlignment,
            SnapshotBad::stride,SnapshotBad::strideAlignment,SnapshotBad::countOverflow,SnapshotBad::recordVersion,SnapshotBad::rowHeader,
            SnapshotBad::wrongTarget,SnapshotBad::registrationHeader,SnapshotBad::string}){
        Fake f;auto s=Services(f);f.snapshotBad=bad;sdk::CatalogSnapshot out;const auto unchanged=out.value();
        Check(s.Acquire(7,out)==MMS_E_FAILED&&!out&&Same(out.value(),unchanged),"malformed successful snapshot never becomes an owned output");
        Check(f.live.empty()&&f.releases==(bad==SnapshotBad::noHandle?0u:1u)&&(!f.releases||f.lastReleased==f.lastIssued),
            "bad successful snapshot releases exactly its original nonzero lease through the original table");
    }
    for(auto bad:{EntryBad::header,EntryBad::noLease,EntryBad::wrongElement,EntryBad::wrongInstance,EntryBad::wrongEpoch,EntryBad::string,EntryBad::unpresented}){
        Fake f;auto s=Services(f);f.entryBad=bad;sdk::CatalogEntryLease out;const auto unchanged=out.value();
        Check(s.Resolve(f.entries[1].target,out)==MMS_E_FAILED&&!out&&Same(out.value(),unchanged),"malformed successful entry preserves the caller's empty output");
        Check(f.live.empty()&&f.releases==(bad==EntryBad::noLease?0u:1u)&&(!f.releases||f.lastReleased==f.lastIssued),"bad successful entry releases only the original lease returned in that call");
    }
    Fake broken;auto brokenService=Services(broken);broken.snapshotBad=SnapshotBad::header;broken.releaseResult=MMS_E_BUSY;
    sdk::CatalogSnapshot badOutput;
    Check(brokenService.Acquire(7,badOutput)==MMS_E_BUSY&&!badOutput&&broken.releases==1&&broken.live.count(broken.lastIssued),
        "malformed-success plus failed cleanup reports the host's error and does not claim automatic reclamation");
    broken.releaseResult=MMS_OK;Check(Fake::Release(&broken,broken.lastIssued)==MMS_OK,"fake host explicitly cleans its own doubly-invalid returned lease");
}

void ThreadsAndBorrowedLifetime(){
    Fake f;Fake other;auto service=Services(f);sdk::CatalogSnapshot snapshot;sdk::CatalogEntryLease entry;
    Check(service.Acquire(7,snapshot)==MMS_OK&&service.Resolve(f.entries[1].target,entry)==MMS_OK,"seed immutable views and live original leases on UI thread");
    const auto calls=f.Calls();const auto snapshotHandle=snapshot.get();const auto entryHandle=entry.get();
    bool workerPassed=false;
    std::thread worker([&]{
        MmsUiCatalogCapabilitiesV1 caps{};const auto saved=caps;
        sdk::CatalogSnapshot absent;sdk::CatalogEntryLease absentEntry;
        workerPassed=service.CheckThread()==MMS_E_THREAD&&service.Capabilities(caps)==MMS_E_THREAD&&Same(caps,saved)&&
            service.Acquire(7,absent)==MMS_E_THREAD&&!absent&&service.Resolve(f.entries[1].target,absentEntry)==MMS_E_THREAD&&!absentEntry&&
            snapshot.Release()==MMS_E_THREAD&&snapshot.get()==snapshotHandle&&entry.Release()==MMS_E_THREAD&&entry.get()==entryHandle&&
            snapshot.Element(0)==&f.entries[0]&&snapshot.Element(1)->element==9002&&snapshot.Registration(0)->handle==8001&&
            entry.value().element==9002&&entry.value().id.data==f.entries[1].id.data;
    });worker.join();
    Check(workerPassed&&f.Calls()==calls,"worker reads original immutable spans but all service/release operations fail before entering the C table");
    Check(snapshot.Release()==MMS_OK&&entry.Release()==MMS_OK&&f.live.empty(),"worker-observed leases return to UI thread for normal release");
    // Each lease owns its captured table even if its service wrapper is later
    // negotiated for another live context. No context lifetime is extended.
    Check(service.Acquire(7,snapshot)==MMS_OK,"seed original table ownership before service requery");
    Check(sdk::CatalogServices::Query(other.Host(),service)==MMS_OK,"requery a non-owning service for another live host context");
    const auto oldReleases=f.releases;Check(snapshot.Release()==MMS_OK&&f.releases==oldReleases+1&&!other.releases&&f.live.empty(),"lease release still uses its captured original context and handle");
}
void Run(const char* name,void(*test)()){stage=name;std::printf("STAGE %s\n",name);test();std::printf("PASS %s\n",name);}
}
int main(){
    static_assert(!std::is_copy_constructible_v<sdk::CatalogSnapshot>&&!std::is_copy_assignable_v<sdk::CatalogSnapshot>);
    static_assert(std::is_nothrow_move_constructible_v<sdk::CatalogSnapshot>&&!std::is_move_assignable_v<sdk::CatalogSnapshot>);
    static_assert(!std::is_copy_constructible_v<sdk::CatalogEntryLease>&&std::is_nothrow_move_constructible_v<sdk::CatalogEntryLease>);
    static_assert(!std::is_copy_assignable_v<sdk::CatalogEntryLease>&&!std::is_move_assignable_v<sdk::CatalogEntryLease>);
    static_assert(std::is_same_v<decltype(std::declval<const sdk::CatalogSnapshot&>().value()),const MmsUiCatalogSnapshotV1&>);
    static_assert(std::is_same_v<decltype(std::declval<const sdk::CatalogSnapshot&>().Element(0)),const MmsUiCatalogEntryV1*>);
    static_assert(std::is_same_v<decltype(std::declval<const sdk::CatalogEntryLease&>().value()),const MmsUiCatalogEntryV1&>);
    std::setvbuf(stdout,nullptr,_IONBF,0);
    Run("negotiation-and-capabilities",NegotiationAndCapabilities);
    Run("original-identity-views-move-and-release",IdentityViewsMoveAndRelease);
    Run("failed-outputs-and-bad-success-cleanup",FailedOutputsAndBadSuccessCleanup);
    Run("UI-thread-gates-and-worker-borrowed-views",ThreadsAndBorrowedLifetime);
    std::printf("PASS UI catalog C++ SDK contracts: %u checks\n",checks);return 0;
}
