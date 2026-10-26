// Evaluation 1.3: inherited, model and ground states share one source/lease.
#include "evaluation_store.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>

static thread_local int failAfter=-1;
void* operator new(std::size_t n){if(failAfter==0)throw std::bad_alloc();if(failAfter>0)--failAfter;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace mikumikustudio::plugins;
static unsigned checks=0;static const char* section="setup";
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %s line %d: %s\n",section,__LINE__,#x);std::exit(1);}}while(false)
static constexpr MmsHandle sentinel=0x123456789abcdef0ull;
static constexpr MmsObjectRef object{{101,202},7},target{{303,404},9};
static constexpr MmsId bone{505,606};
static constexpr uint32_t display=MMS_EVALUATION_CAP_DISPLAY;
template<class T>static T Poison(){T value;std::memset(&value,0xa5,sizeof(value));return value;}
template<class T>static bool Unchanged(const T& value){const auto before=Poison<T>();return !std::memcmp(&value,&before,sizeof(value));}
template<class T>static bool HeaderOk(const T& value,unsigned minor=0){return value.h.size==sizeof(value)&&value.h.major==1&&value.h.minor==minor;}
static MmsEvaluationSourceQueryV1 Query(uint64_t budget=0){return {Header<MmsEvaluationSourceQueryV1>(),object,budget,0,0};}
static MmsEvaluationApiV1_3 Bind(EvaluationStore& store,EvaluationStore::Client& client){client.owner=&store;return store.Api13(client);}
static MmsParentStateV1 State(double frame){
    MmsParentStateV1 out{};out.h=Header<MmsParentStateV1>();
    if(frame<10)out.windowEnd=10;
    else if(frame<20){out.kind=MMS_EVALUATION_PARENT_MODEL;out.target=target;out.bone=bone;out.windowStart=10;out.windowEnd=20;}
    else{out.kind=MMS_EVALUATION_PARENT_GROUND;out.windowStart=20;}
    return out;
}
static MmsParentSelectionV1 Legacy(double frame){
    MmsParentSelectionV1 out{};out.h=Header<MmsParentSelectionV1>();const auto state=State(frame);
    out.target=state.target;out.bone=state.bone;out.windowStart=state.windowStart;out.windowEnd=state.windowEnd;
    out.flags=state.kind==MMS_EVALUATION_PARENT_MODEL?MMS_EVALUATION_PARENT_ACTIVE:0;return out;
}
static std::shared_ptr<EvaluationData> Data(const MmsEvaluationSourceQueryV1& query,uint32_t mask){
    auto data=std::make_shared<EvaluationData>();data->object=query.object;data->channels=mask;data->hostRevision=77;data->retainedBytes=4096;
    if(mask&1)data->sample=[](uint32_t,double,MmsBoneChannelsV1&)->MmsResult{return MMS_E_NOT_FOUND;};
    if(mask&2)data->sampleMorph=[](uint32_t,double,MmsMorphWeightV1&)->MmsResult{return MMS_E_NOT_FOUND;};
    if(mask&display){
        data->selectors={{Header<MmsEvaluationSelectorV1>(),{},MMS_EVALUATION_SELECTOR_GLOBAL,0},{Header<MmsEvaluationSelectorV1>(),bone,0,0}};
        data->sampleVisibility=[](double,MmsDisplayStateV1& out){out={Header<MmsDisplayStateV1>(),197,0};return MMS_OK;};
        data->sampleIk=[](uint32_t,double,MmsIkStateV1&)->MmsResult{return MMS_E_NOT_FOUND;};
        data->sampleParentState=[](uint32_t,double frame,MmsParentStateV1& out){out=State(frame);return MMS_OK;};
    }
    return data;
}
static MmsHandle Create(const MmsEvaluationApiV1_3& api,uint32_t mask=display,uint64_t budget=0){
    const auto query=Query(budget);MmsHandle handle=sentinel;
    CHECK(api.createSourceChannels(api.context,&query,mask,&handle)==MMS_OK&&handle&&handle!=sentinel);return handle;
}
static MmsEvaluationSourceInfoV1_2 Describe(const MmsEvaluationApiV1_3& api,MmsHandle source){
    auto info=Poison<MmsEvaluationSourceInfoV1_2>();CHECK(api.describeSourceDisplay(api.context,source,&info)==MMS_OK&&HeaderOk(info,2));return info;
}
static bool SameState(const MmsParentStateV1& a,const MmsParentStateV1& b){return HeaderOk(a)&&Same(a.target,b.target)&&Same(a.bone,b.bone)&&a.kind==b.kind&&a.windowStart==b.windowStart&&a.windowEnd==b.windowEnd&&!a.reserved;}
static void PrefixesKindsAndAdapters(){
    section="all old function prefixes and three native parent kinds";
    static_assert(sizeof(MmsParentStateV1)==64&&sizeof(MmsEvaluationApiV1_3)==160);
    static_assert(offsetof(MmsEvaluationApiV1_3,sampleParentState)==sizeof(MmsEvaluationApiV1_2));
    unsigned canonicalCalls=0,legacyCalls=0;
    for(unsigned factory=0;factory<3;++factory){
        EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query,uint32_t mask,uint64_t,auto& out){
            auto data=Data(query,mask);
            if(mask&display){
                data->sampleParentState=[&](uint32_t,double frame,auto& value){++canonicalCalls;value=State(frame);return MMS_OK;};
                if(factory){data->sampleParent=[&](uint32_t,double frame,auto& value){++legacyCalls;value=Legacy(frame);if(factory==2)value.target.generation=999;return MMS_OK;};}
                if(factory==1)data->sampleParentState={};
            }
            out=data;return MMS_OK;
        }),7);
        EvaluationStore::Client client;const auto api=Bind(store,client);const auto old=store.Api(client);const auto mid=store.Api11(client);const auto prior=store.Api12(client);
        CHECK(HeaderOk(api,3)&&HeaderOk(old)&&HeaderOk(mid,1)&&HeaderOk(prior,2));
        const auto prefix=[&](const auto& value){return !std::memcmp(reinterpret_cast<const char*>(&api)+sizeof(MmsHeader),reinterpret_cast<const char*>(&value)+sizeof(MmsHeader),sizeof(value)-sizeof(MmsHeader));};
        CHECK(prefix(old)&&prefix(mid)&&prefix(prior)&&api.sampleParentState);
        const auto handle=Create(api);const auto info=Describe(api,handle);CHECK(info.selectorCount==2&&info.flags==display&&info.boneCount==0&&info.morphCount==0&&info.ikCount==0);
        const auto beforeCanonical=canonicalCalls,beforeLegacy=legacyCalls;
        for(unsigned selector:{0u,1u})for(const auto time:std::array<MmsTime,3>{{{0,1},{1,2},{5,6}}}){
            const auto frame=time.numerator==0?0.0:time.numerator==1?15.0:25.0;auto wanted=State(frame);if(factory==1&&frame>=20)wanted.kind=MMS_EVALUATION_PARENT_INHERIT;
            auto state=Poison<MmsParentStateV1>();CHECK(api.sampleParentState(api.context,handle,selector,time,&state)==MMS_OK&&SameState(state,wanted));
            auto legacy=Poison<MmsParentSelectionV1>();const auto result=prior.sampleParent(prior.context,handle,selector,time,&legacy);
            if(wanted.kind==MMS_EVALUATION_PARENT_GROUND)CHECK(result==MMS_E_UNSUPPORTED&&Unchanged(legacy));
            else CHECK(result==MMS_OK&&HeaderOk(legacy)&&Same(legacy.target,wanted.target)&&Same(legacy.bone,wanted.bone)&&
                legacy.flags==(wanted.kind==MMS_EVALUATION_PARENT_MODEL?MMS_EVALUATION_PARENT_ACTIVE:0)&&legacy.windowStart==wanted.windowStart&&legacy.windowEnd==wanted.windowEnd&&!legacy.reserved);
        }
        CHECK(factory==1?(canonicalCalls==beforeCanonical&&legacyCalls==beforeLegacy+12):(canonicalCalls==beforeCanonical+12&&legacyCalls==beforeLegacy));
        MmsHandle alias=0;CHECK(prior.retain(prior.context,handle,&alias)==MMS_OK&&alias!=handle);const auto aliasInfo=Describe(api,alias);
        CHECK(aliasInfo.captureId==info.captureId&&aliasInfo.retainedBytes==info.retainedBytes&&client.bytes==info.retainedBytes);
        CHECK(old.release(old.context,handle)==MMS_OK);MmsParentStateV1 state{};CHECK(api.sampleParentState(api.context,alias,0,{1,2},&state)==MMS_OK);
        CHECK(mid.release(mid.context,alias)==MMS_OK&&client.bytes==0&&!store.HasLeases(client));
    }
}
static void AdmissionAndCapture(){
    section="parent callback presence, channel isolation and failed capture atomicity";
    int mode=0;
    EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query,uint32_t mask,uint64_t,auto& out)->MmsResult{
        auto data=Data(query,mask);
        if(mode==1)data->sampleParentState={};
        if(mode==2||mode==4)data->sampleParentState=[](uint32_t,double frame,auto& v){v=State(frame);return MMS_OK;};
        if(mode==3||mode==4)data->sampleParent=[](uint32_t,double frame,auto& v){v=Legacy(frame);return MMS_OK;};
        if(mode==5){data->sampleParentState={};data->sampleParent=[](uint32_t,double frame,auto& v){v=Legacy(frame);return MMS_OK;};}
        if(mode==6)throw std::runtime_error("capture");if(mode==7)throw std::bad_alloc();
        out=data;return MMS_OK;
    }),7);
    EvaluationStore::Client client,foreign;const auto api=Bind(store,client),other=Bind(store,foreign);const auto query=Query();
    for(int invalid:{1,2,3,4,6,7}){mode=invalid;MmsHandle out=sentinel;const auto mask=invalid>=2&&invalid<=4?1u:display;
        const auto expected=(invalid==1||invalid==6)?MMS_E_FAILED:invalid==7?MMS_E_MEMORY:MMS_E_ARGUMENT;
        CHECK(api.createSourceChannels(api.context,&query,mask,&out)==expected&&out==sentinel&&client.handles==0&&client.bytes==0&&!store.HasLeases(client));}
    mode=5;const auto oldFactory=Create(api);MmsParentStateV1 state{};CHECK(api.sampleParentState(api.context,oldFactory,0,{1,2},&state)==MMS_OK&&state.kind==MMS_EVALUATION_PARENT_MODEL);
    CHECK(api.release(api.context,oldFactory)==MMS_OK);mode=0;const auto handle=Create(api),noDisplay=Create(api,1);
    auto out=Poison<MmsParentStateV1>();
    CHECK(api.sampleParentState(api.context,noDisplay,0,{0,1},&out)==MMS_E_UNSUPPORTED&&Unchanged(out));
    CHECK(other.sampleParentState(other.context,handle,0,{0,1},&out)==MMS_E_NOT_FOUND&&Unchanged(out));
    CHECK(api.sampleParentState(api.context,handle,2,{0,1},&out)==MMS_E_NOT_FOUND&&Unchanged(out));
    CHECK(api.sampleParentState(api.context,sentinel,0,{0,1},&out)==MMS_E_NOT_FOUND&&Unchanged(out));
    CHECK(api.sampleParentState(nullptr,handle,0,{0,1},&out)==MMS_E_ARGUMENT&&Unchanged(out));
    CHECK(api.sampleParentState(api.context,handle,0,{0,1},nullptr)==MMS_E_ARGUMENT);
    MmsResult created=MMS_OK,sampled=MMS_E_FAILED;MmsHandle workerHandle=sentinel;MmsParentStateV1 workerOut{};
    std::thread worker([&]{created=api.createSourceChannels(api.context,&query,display,&workerHandle);sampled=api.sampleParentState(api.context,handle,1,{5,6},&workerOut);});worker.join();
    CHECK(created==MMS_E_THREAD&&workerHandle==sentinel&&sampled==MMS_OK&&workerOut.kind==MMS_EVALUATION_PARENT_GROUND);
    CHECK(api.release(api.context,handle)==MMS_OK&&api.release(api.context,noDisplay)==MMS_OK);
    CHECK(api.sampleParentState(api.context,handle,0,{0,1},&out)==MMS_E_NOT_FOUND&&Unchanged(out)&&!store.HasLeases(client));
}
static void InvalidSamples(){
    section="canonical state validation and legacy flag adapter never leak partial output";
    for(bool legacyOnly:{false,true}){
        int bad=0;unsigned fallbackCalls=0;
        EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query,uint32_t mask,uint64_t,auto& out){
            auto data=Data(query,mask);
            const auto mutate=[&](auto& value){
                switch(bad){
                case 1:value.target.id={};break;case 2:value.target.generation=0;break;case 3:value.bone={};break;
                case 4:value.reserved=1;break;case 5:value.windowStart=16;break;case 6:value.windowEnd=15;break;
                case 7:value.windowStart=20;value.windowEnd=20;break;case 8:value.windowEnd=2147483647u;break;
                case 9:value.windowStart=2147483647u;value.windowEnd=0;break;
                case 13:throw std::runtime_error("sample");case 14:throw std::bad_alloc();
                }
            };
            data->sampleParent=[&,mutate](uint32_t,double frame,MmsParentSelectionV1& value)->MmsResult{
                if(!legacyOnly){++fallbackCalls;return MMS_E_FAILED;}value=Legacy(frame);mutate(value);
                if(bad==10)value.flags=2;if(bad==11)value.flags=0;if(bad==12){value.flags=0;value.target={};value.bone={1,0};}
                return bad==15?MMS_E_NOT_FOUND:bad==16?MMS_E_UNSUPPORTED:MMS_OK;
            };
            data->sampleParentState=[&,mutate](uint32_t,double frame,MmsParentStateV1& value)->MmsResult{
                value=State(frame);mutate(value);
                if(bad==10)value.kind=3;if(bad==11)value.kind=MMS_EVALUATION_PARENT_INHERIT;
                if(bad==12){value.kind=MMS_EVALUATION_PARENT_GROUND;value.target={};value.bone={1,0};}
                return bad==15?MMS_E_NOT_FOUND:bad==16?MMS_E_UNSUPPORTED:MMS_OK;
            };
            if(legacyOnly)data->sampleParentState={};out=data;return MMS_OK;
        }),display);
        EvaluationStore::Client client;const auto api=Bind(store,client);const auto handle=Create(api);const auto bytes=client.bytes;
        for(bad=1;bad<=16;++bad){auto state=Poison<MmsParentStateV1>();auto old=Poison<MmsParentSelectionV1>();
            const auto status=bad==14?MMS_E_MEMORY:bad==15?MMS_E_NOT_FOUND:bad==16?MMS_E_UNSUPPORTED:MMS_E_FAILED;
            CHECK(api.sampleParentState(api.context,handle,0,{1,2},&state)==status&&Unchanged(state));
            CHECK(api.sampleParent(api.context,handle,0,{1,2},&old)==status&&Unchanged(old));CHECK(client.bytes==bytes&&client.active==0&&fallbackCalls==0);
        }
        bad=0;MmsParentStateV1 state{};CHECK(api.sampleParentState(api.context,handle,1,{1,2},&state)==MMS_OK&&SameState(state,State(15)));
        CHECK(api.release(api.context,handle)==MMS_OK&&!store.HasLeases(client));
    }
}
static void ExactTime(){
    section="parent state uses exact rational discrete time at 30 fps";
    double received=-1;unsigned calls=0;
    EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query,uint32_t mask,uint64_t,auto& out){auto data=Data(query,mask);
        data->sampleParentState=[&](uint32_t,double frame,auto& value){received=frame;++calls;value=State(frame);return MMS_OK;};out=data;return MMS_OK;
    }),display);
    EvaluationStore::Client client;const auto api=Bind(store,client);const auto handle=Create(api);
    struct Case{MmsTime time;uint32_t frame;};
    // Integer identities below provide the oracle; no double division is used.
    const Case cases[]={
        {{9007199254740992ll,270215977642229761ull},0},{{9007199254740992ll,270215977642229760ull},1},{{9007199254740992ll,270215977642229759ull},1},
        {{27021597764222976ll,810647932926689283ull},0},{{1,30},1},{{3,90},1},
        {{9007199254740993ll,27021597764222980ull},9},{{9007199254740993ll,27021597764222979ull},10},{{9007199254740993ll,27021597764222978ull},10},
        {{1,3},10},{{3,9},10},{{1,4},7},{{2,3},20},
        {{99999999999999999ll,3000000000ull},999999999},{{100000000000000000ll,3000000000ull},1000000000},
        {{2147483645999999999ll,30000000000ull},2147483645u},{{2147483646000000000ll,30000000000ull},2147483646u},
        {{INT64_MAX,UINT64_MAX},14},{{INT64_MAX,18446744073709551614ull},15},{{INT64_MAX,18446744073709551613ull},15}
    };
    for(const auto& item:cases){auto state=Poison<MmsParentStateV1>();CHECK(api.sampleParentState(api.context,handle,0,item.time,&state)==MMS_OK&&received==item.frame&&SameState(state,State(item.frame)));}
    for(const auto time:std::array<MmsTime,5>{{{2147483646000000001ll,30000000000ull},{2147483647,30},{-1,1},{0,0},{INT64_MAX,1}}}){
        const auto before=calls;auto state=Poison<MmsParentStateV1>();CHECK(api.sampleParentState(api.context,handle,0,time,&state)==MMS_E_ARGUMENT&&Unchanged(state)&&calls==before);}
    CHECK(api.release(api.context,handle)==MMS_OK&&!store.HasLeases(client));
}
static void BudgetsAndFailures(){
    section="same parent payload accounting across both API versions and aliases";
    uint64_t bytes=4096;
    EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query,uint32_t mask,uint64_t,auto& out){auto data=Data(query,mask);data->retainedBytes=bytes;out=data;return MMS_OK;}),display);
    EvaluationStore::Client client;const auto api=Bind(store,client);const auto old=store.Api12(client);const auto handle=Create(api);const auto info=Describe(api,handle);
    CHECK(info.retainedBytes>=bytes&&client.bytes==info.retainedBytes);
    auto query=Query(info.retainedBytes-1);MmsHandle rejected=sentinel;CHECK(api.createSourceChannels(api.context,&query,display,&rejected)==MMS_E_MEMORY&&rejected==sentinel);
    const auto exact=Create(api,display,info.retainedBytes);CHECK(Describe(api,exact).retainedBytes==info.retainedBytes);CHECK(api.release(api.context,exact)==MMS_OK);
    MmsHandle alias=0;CHECK(old.retain(old.context,handle,&alias)==MMS_OK&&client.bytes==info.retainedBytes);
    auto state=Poison<MmsParentStateV1>();MmsParentSelectionV1 parent{};
    failAfter=0;const auto newStatus=api.sampleParentState(api.context,alias,1,{5,6},&state);const auto oldStatus=old.sampleParent(old.context,handle,1,{1,2},&parent);failAfter=-1;
    CHECK(newStatus==MMS_OK&&oldStatus==MMS_OK&&state.kind==MMS_EVALUATION_PARENT_GROUND&&parent.flags==MMS_EVALUATION_PARENT_ACTIVE);
    CHECK(api.release(api.context,alias)==MMS_OK&&client.bytes==info.retainedBytes);
    bool success=false;unsigned failures=0;query=Query();
    for(int allowance=0;allowance<256&&!success;++allowance){MmsHandle candidate=sentinel;failAfter=allowance;
        const auto result=api.createSourceChannels(api.context,&query,display,&candidate);failAfter=-1;
        if(result==MMS_E_MEMORY){++failures;CHECK(candidate==sentinel&&client.handles==1&&client.bytes==info.retainedBytes&&client.active==0);}
        else{CHECK(result==MMS_OK&&candidate!=sentinel);success=true;CHECK(api.release(api.context,candidate)==MMS_OK);}
        state=Poison<MmsParentStateV1>();CHECK(api.sampleParentState(api.context,handle,0,{1,2},&state)==MMS_OK&&SameState(state,State(15)));
    }
    CHECK(success&&failures>0&&api.release(api.context,handle)==MMS_OK&&!store.HasLeases(client));
    // Reported immutable backend bytes count against existing source quotas;
    // this exercises accounting without allocating a large physical buffer.
    bytes=32ull*1024*1024;std::array<MmsHandle,8> handles{};unsigned count=0;
    for(;count<handles.size();++count){MmsHandle next=sentinel;const auto before=client.bytes;const auto status=api.createSourceChannels(api.context,&query,display,&next);
        if(status==MMS_E_MEMORY){CHECK(next==sentinel&&client.bytes==before);break;}CHECK(status==MMS_OK);handles[count]=next;}
    CHECK(count>1&&count<handles.size());for(unsigned i=0;i<count;++i)CHECK(api.release(api.context,handles[i])==MMS_OK);
    CHECK(client.bytes==0&&client.handles==0&&!store.HasLeases(client));
}
struct Gate{
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr),proceed=CreateEventW(nullptr,TRUE,FALSE,nullptr);std::atomic<unsigned> entered{0};
    ~Gate(){if(ready)CloseHandle(ready);if(proceed)CloseHandle(proceed);}
};
static void InFlightRetirement(){
    section="old and new parent calls pin one retired payload until both return";
    for(unsigned repeat=0;repeat<4;++repeat){auto gate=std::make_shared<Gate>();CHECK(gate->ready&&gate->proceed);
        EvaluationStore store(EvaluationStore::CaptureChannels([gate](const auto& query,uint32_t mask,uint64_t,auto& out){auto data=Data(query,mask);
            data->sampleParentState=[gate](uint32_t,double frame,auto& value)->MmsResult{
                if(gate->entered.fetch_add(1)+1==2)SetEvent(gate->ready);
                if(WaitForSingleObject(gate->proceed,15000)!=WAIT_OBJECT_0)return MMS_E_FAILED;
                value=State(frame);return MMS_OK;
            };out=data;return MMS_OK;
        }),display);
        EvaluationStore::Client client;const auto api=Bind(store,client);const auto handle=Create(api);const auto charged=client.bytes;
        std::array<MmsResult,2> status{MMS_E_FAILED,MMS_E_FAILED};MmsParentStateV1 state{};MmsParentSelectionV1 parent{};
        struct Threads{std::shared_ptr<Gate> gate;std::array<std::thread,2> workers;~Threads(){SetEvent(gate->proceed);for(auto& worker:workers)if(worker.joinable())worker.join();}} threads{gate,{}};
        threads.workers[0]=std::thread([&]{status[0]=api.sampleParentState(api.context,handle,0,{5,6},&state);});
        threads.workers[1]=std::thread([&]{status[1]=api.sampleParent(api.context,handle,1,{1,2},&parent);});
        CHECK(WaitForSingleObject(gate->ready,10000)==WAIT_OBJECT_0);store.Retire(client);CHECK(store.HasLeases(client));
        CHECK(api.release(api.context,handle)==MMS_OK&&client.handles==0&&client.bytes==charged&&client.active==2&&store.HasLeases(client));
        auto denied=Poison<MmsParentStateV1>();CHECK(api.sampleParentState(api.context,handle,0,{0,1},&denied)==MMS_E_NOT_READY&&Unchanged(denied));
        MmsHandle retained=sentinel;CHECK(api.retain(api.context,handle,&retained)==MMS_E_NOT_READY&&retained==sentinel);
        SetEvent(gate->proceed);for(auto& worker:threads.workers)worker.join();
        CHECK(status[0]==MMS_OK&&status[1]==MMS_OK&&SameState(state,State(25))&&parent.flags==MMS_EVALUATION_PARENT_ACTIVE);
        CHECK(!store.HasLeases(client)&&client.active==0&&client.bytes==0&&client.handles==0);
    }
}
static void ConcurrentReads(){
    section="repeated API queries do not rewrite a concurrently used client owner";
    EvaluationStore store(EvaluationStore::CaptureChannels([](const auto& query,uint32_t mask,uint64_t,auto& out){out=Data(query,mask);return MMS_OK;}),display);
    EvaluationStore::Client client;const auto api=Bind(store,client);const auto handle=Create(api);const auto charged=client.bytes;std::atomic<unsigned> failed{0};std::array<std::thread,4> workers;
    try{for(unsigned i=0;i<4;++i)workers[i]=std::thread([&,i]{try{for(unsigned n=0;n<60;++n){MmsHandle alias=0;if(api.retain(api.context,handle,&alias)!=MMS_OK){++failed;continue;}
        const auto table=store.Api13(client);MmsParentStateV1 state{};MmsParentSelectionV1 old{};
        const auto status=table.sampleParentState(table.context,alias,i%2,{5,6},&state);const auto legacy=table.sampleParent(table.context,alias,i%2,{1,2},&old);
        if(status!=MMS_OK||legacy!=MMS_OK||!SameState(state,State(25))||old.target.generation!=target.generation||table.context!=api.context||table.sampleParentState!=api.sampleParentState)++failed;
        if(api.release(api.context,alias)!=MMS_OK)++failed;
    }}catch(...){++failed;}});}catch(...){for(auto& worker:workers)if(worker.joinable())worker.join();throw;}
    for(auto& worker:workers)worker.join();CHECK(failed==0&&client.owner==&store&&client.bytes==charged&&client.handles==1&&client.active==0);
    CHECK(api.release(api.context,handle)==MMS_OK&&!store.HasLeases(client));
}
int main(){try{PrefixesKindsAndAdapters();AdmissionAndCapture();InvalidSamples();ExactTime();BudgetsAndFailures();InFlightRetirement();ConcurrentReads();
    std::printf("PASS %u evaluation parent state checks\n",checks);return 0;
}catch(const std::exception& error){failAfter=-1;std::fprintf(stderr,"FAIL %s unexpected exception: %s\n",section,error.what());return 1;}}
