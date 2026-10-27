// Header-only SDK contracts against deterministic C tables, without a host/GUI.
#include <mms/cpp/evaluation.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <type_traits>

static thread_local int failAfter=-1;
void* operator new(std::size_t n){if(failAfter==0)throw std::bad_alloc();if(failAfter>0)--failAfter;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace sdk=mms::sdk;
namespace {
unsigned checks=0;const char* stage="setup";
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %s line %d: %s\n",stage,__LINE__,#x);std::exit(1);}}while(false)
template<class T>constexpr MmsHeader H(){return {sizeof(T),1,0};}
template<class T>bool Same(const T& a,const T& b){return !std::memcmp(&a,&b,sizeof(T));}
template<class T>T Poison(){T out;std::memset(&out,0xa5,sizeof(out));return out;}
template<class T>bool Unchanged(const T& out){const auto before=Poison<T>();return Same(out,before);}
enum Op{Query,Caps,Capture,SourceInfo,SourceModel,SourceBone,Evaluate,FrameInfo,FrameModel,Read,Retain,Release,Count};
struct Fake {
    struct Slot{MmsHandle handle=0;bool frame=false;uint64_t capture=0,frameId=0;MmsTime time{0,1};};
    MmsEvaluationApiV1_4 table{};std::array<Slot,128> slots{};
    std::array<unsigned,Count> calls{};std::array<MmsResult,Count> results{};std::array<int,Count> throws{},bad{};
    std::thread::id ui=std::this_thread::get_id();uint64_t identity=0,next=100,serial=1;
    uint32_t queryMajor=0,queryMinor=0,queryBytes=0,supported=MMS_EVALUATION_SCENE_SKELETON;
    bool queriedName=false;MmsEvaluationSceneQueryV1 captureQuery{};MmsEvaluationFrameQueryV1 frameQuery{};
    MmsHandle lastIssued=0,lastReleased=0;static constexpr MmsHandle illegal=0xfedcba9876543210ull;
    explicit Fake(uint64_t id=0):identity(id){
        table.h={sizeof(table),1,4};table.context=this;
        table.getCapabilities=[](void*,MmsEvaluationCapsV1*){return MMS_E_UNSUPPORTED;};
        table.createSource=[](void*,const MmsEvaluationSourceQueryV1*,MmsHandle*){return MMS_E_UNSUPPORTED;};
        table.describeSource=[](void*,MmsHandle,MmsEvaluationSourceInfoV1*){return MMS_E_UNSUPPORTED;};
        table.boneAt=[](void*,MmsHandle,uint32_t,MmsEvaluationBoneV1*){return MMS_E_UNSUPPORTED;};
        table.sampleBone=[](void*,MmsHandle,MmsId,MmsTime,MmsBoneChannelsV1*){return MMS_E_UNSUPPORTED;};
        table.retain=DoRetain;table.release=DoRelease;
        table.createSourceChannels=[](void*,const MmsEvaluationSourceQueryV1*,uint32_t,MmsHandle*){return MMS_E_UNSUPPORTED;};
        table.describeSourceChannels=[](void*,MmsHandle,MmsEvaluationSourceInfoV1_1*){return MMS_E_UNSUPPORTED;};
        table.morphAt=[](void*,MmsHandle,uint32_t,MmsEvaluationMorphV1*){return MMS_E_UNSUPPORTED;};
        table.sampleMorph=[](void*,MmsHandle,MmsId,MmsTime,MmsMorphWeightV1*){return MMS_E_UNSUPPORTED;};
        table.describeSourceDisplay=[](void*,MmsHandle,MmsEvaluationSourceInfoV1_2*){return MMS_E_UNSUPPORTED;};
        table.ikAt=[](void*,MmsHandle,uint32_t,MmsEvaluationIkV1*){return MMS_E_UNSUPPORTED;};
        table.selectorAt=[](void*,MmsHandle,uint32_t,MmsEvaluationSelectorV1*){return MMS_E_UNSUPPORTED;};
        table.sampleVisibility=[](void*,MmsHandle,MmsTime,MmsDisplayStateV1*){return MMS_E_UNSUPPORTED;};
        table.sampleIk=[](void*,MmsHandle,uint32_t,MmsTime,MmsIkStateV1*){return MMS_E_UNSUPPORTED;};
        table.sampleParent=[](void*,MmsHandle,uint32_t,MmsTime,MmsParentSelectionV1*){return MMS_E_UNSUPPORTED;};
        table.sampleParentState=[](void*,MmsHandle,uint32_t,MmsTime,MmsParentStateV1*){return MMS_E_UNSUPPORTED;};
        table.getSceneCapabilities=GetCaps;table.createSceneSource=Create;table.describeSceneSource=DescribeSource;
        table.sceneModelAt=GetSourceModel;table.sceneBoneAt=GetBone;table.evaluateFrame=EvaluateFrame;table.describeFrame=DescribeFrame;
        table.frameModelAt=GetFrameModel;table.readBonePose=ReadPose;
    }
    MmsHostV1 Host(){MmsHostV1 host{};host.h=H<MmsHostV1>();host.context=this;host.queryService=QueryService;return host;}
    MmsResult Touch(Op op){++calls[op];if(throws[op]==1)throw std::bad_alloc();if(throws[op]==2)throw std::length_error("fake length");if(throws[op]==3)throw std::runtime_error("fake error");return results[op];}
    unsigned Live()const{unsigned count=0;for(const auto& slot:slots)count+=slot.handle!=0;return count;}
    Slot* Find(MmsHandle handle){for(auto& slot:slots)if(slot.handle==handle&&handle)return &slot;return nullptr;}
    MmsHandle Issue(Slot value){for(auto& slot:slots)if(!slot.handle){value.handle=next++;slot=value;lastIssued=value.handle;return value.handle;}return 0;}
    template<class T>void HeaderFault(Op op,T& out){if(bad[op]==1)--out.h.size;if(bad[op]==2)out.h.major=2;if(bad[op]==3)out.h.minor=1;if(bad[op]==7)out.h.size+=8;}
    static Fake& Self(void* context){return *static_cast<Fake*>(context);}
    static MmsResult MMS_CALL QueryService(void* context,MmsString name,uint32_t major,uint32_t minor,uint32_t bytes,void* out){auto& f=Self(context);
        f.queryMajor=major;f.queryMinor=minor;f.queryBytes=bytes;f.queriedName=name.bytes==18&&!std::memcmp(name.data,"mms.scene.evaluate",18);
        if(!out||bytes<sizeof(f.table))return MMS_E_ARGUMENT;std::memcpy(out,&f.table,sizeof(f.table));return f.Touch(Query);
    }
    static MmsResult MMS_CALL GetCaps(void* context,MmsEvaluationSceneCapsV1* out){auto& f=Self(context);
        *out={H<MmsEvaluationSceneCapsV1>(),64,64,128,256,128,256,512,f.supported,0};f.HeaderFault(Caps,*out);
        if(f.bad[Caps]==4)out->flags=2;if(f.bad[Caps]==5)out->reserved=1;if(f.bad[Caps]==6)out->maxClientWorkspaceBytes=1;return f.Touch(Caps);
    }
    static MmsResult MMS_CALL Create(void* context,const MmsEvaluationSceneQueryV1* query,MmsHandle* out){auto& f=Self(context);f.captureQuery=*query;
        *out=illegal;const auto result=f.Touch(Capture);if(result)return result;if(std::this_thread::get_id()!=f.ui)return MMS_E_THREAD;if(!f.supported)return MMS_E_UNSUPPORTED;
        if(f.bad[Capture]==1){*out=0;return MMS_OK;}*out=f.Issue({0,false,f.serial++,0,{0,1}});return *out?MMS_OK:MMS_E_BUSY;
    }
    static MmsResult MMS_CALL DescribeSource(void* context,MmsHandle handle,MmsEvaluationSceneInfoV1* out){auto& f=Self(context);const auto* slot=f.Find(handle);if(!slot||slot->frame)return MMS_E_NOT_FOUND;
        *out={H<MmsEvaluationSceneInfoV1>(),slot->capture,77+f.identity,4096,2,MMS_EVALUATION_SCENE_SKELETON};f.HeaderFault(SourceInfo,*out);
        if(f.bad[SourceInfo]==4)out->captureId=0;if(f.bad[SourceInfo]==5)out->flags=2;if(f.bad[SourceInfo]==6)out->retainedBytes=0;return f.Touch(SourceInfo);
    }
    static MmsResult Model(void* context,MmsHandle handle,uint32_t index,MmsEvaluationSceneModelV1* out,bool frame){auto& f=Self(context);const auto* slot=f.Find(handle);if(!slot||slot->frame!=frame||index>1)return MMS_E_NOT_FOUND;
        const auto op=frame?FrameModel:SourceModel;*out={H<MmsEvaluationSceneModelV1>(),{{f.identity+11+index,22},7},index?1u:2u,frame?MMS_EVALUATION_SCENE_VISIBILITY_VALID:0u,frame?197u:0u,0};f.HeaderFault(op,*out);
        if(f.bad[op]==4)out->object.id={};if(f.bad[op]==5)out->reserved=1;if(f.bad[op]==6)out->visibility=256;return f.Touch(op);
    }
    static MmsResult MMS_CALL GetSourceModel(void* c,MmsHandle h,uint32_t i,MmsEvaluationSceneModelV1* out){return Model(c,h,i,out,false);}
    static MmsResult MMS_CALL GetFrameModel(void* c,MmsHandle h,uint32_t i,MmsEvaluationSceneModelV1* out){return Model(c,h,i,out,true);}
    MmsId BoneId(uint32_t model,uint32_t bone)const{return {identity+33+100*model+2*bone,44};}
    static MmsResult MMS_CALL GetBone(void* context,MmsHandle handle,uint32_t model,uint32_t index,MmsEvaluationSceneBoneV1* out){auto& f=Self(context);const auto* slot=f.Find(handle);if(!slot||slot->frame||model>1||index>=(model?1u:2u))return MMS_E_NOT_FOUND;
        *out={H<MmsEvaluationSceneBoneV1>(),f.BoneId(model,index),index,0};f.HeaderFault(SourceBone,*out);
        if(f.bad[SourceBone]==4)out->bone={};if(f.bad[SourceBone]==5)++out->index;if(f.bad[SourceBone]==6)out->reserved=1;return f.Touch(SourceBone);
    }
    static MmsResult MMS_CALL EvaluateFrame(void* context,const MmsEvaluationFrameQueryV1* query,MmsHandle* out){auto& f=Self(context);f.frameQuery=*query;*out=illegal;
        const auto result=f.Touch(Evaluate);if(result)return result;const auto* slot=f.Find(query->source);if(!slot||slot->frame)return MMS_E_NOT_FOUND;
        if(!query->time.denominator||query->time.numerator<0)return MMS_E_ARGUMENT;
        if(f.bad[Evaluate]==1){*out=0;return MMS_OK;}if(f.bad[Evaluate]==2){*out=query->source;return MMS_OK;}
        auto time=query->time;const auto divisor=std::gcd(static_cast<uint64_t>(time.numerator),time.denominator);time.numerator=static_cast<int64_t>(static_cast<uint64_t>(time.numerator)/divisor);time.denominator/=divisor;
        *out=f.Issue({0,true,slot->capture,f.serial++,time});return *out?MMS_OK:MMS_E_BUSY;
    }
    static MmsResult MMS_CALL DescribeFrame(void* context,MmsHandle handle,MmsEvaluationFrameInfoV1* out){auto& f=Self(context);const auto* slot=f.Find(handle);if(!slot||!slot->frame)return MMS_E_NOT_FOUND;
        *out={H<MmsEvaluationFrameInfoV1>(),slot->capture,slot->frameId,77+f.identity,8192,slot->time,2,MMS_EVALUATION_SCENE_SKELETON};f.HeaderFault(FrameInfo,*out);
        if(f.bad[FrameInfo]==4)out->frameId=0;if(f.bad[FrameInfo]==5)out->time.denominator=0;if(f.bad[FrameInfo]==6)out->time.numerator=-1;return f.Touch(FrameInfo);
    }
    static MmsResult MMS_CALL ReadPose(void* context,MmsHandle handle,uint32_t model,uint32_t index,MmsEvaluationScenePoseV1* out){auto& f=Self(context);const auto* slot=f.Find(handle);if(!slot||!slot->frame||model>1||index>=(model?1u:2u))return MMS_E_NOT_FOUND;
        *out={};out->h=H<MmsEvaluationScenePoseV1>();out->bone=f.BoneId(model,index);
        for(unsigned i=0;i<16;++i)out->rigid[i]=out->scaled[i]=out->attachmentBefore[i]=out->attachmentAfter[i]=i%5==0?1.f:0.f;
        out->rigid[12]=float(double(slot->time.numerator)*30/double(slot->time.denominator));out->scaled[0]=2.5f;out->scaled[1]=-.125f;
        out->attachmentBefore[13]=float(model)+.25f;out->attachmentAfter[14]=float(index)-.75f;out->jointPosition[0]=42.125f;f.HeaderFault(Read,*out);
        if(f.bad[Read]==4)out->scaled[0]=std::numeric_limits<float>::quiet_NaN();if(f.bad[Read]==5)out->reserved=1;if(f.bad[Read]==6)out->bone={};return f.Touch(Read);
    }
    static MmsResult MMS_CALL DoRetain(void* context,MmsHandle handle,MmsHandle* out){auto& f=Self(context);*out=illegal;const auto result=f.Touch(Retain);if(result)return result;const auto* slot=f.Find(handle);if(!slot)return MMS_E_NOT_FOUND;
        if(f.bad[Retain]==1){*out=0;return MMS_OK;}if(f.bad[Retain]==2){*out=handle;return MMS_OK;}*out=f.Issue(*slot);return *out?MMS_OK:MMS_E_BUSY;
    }
    static MmsResult MMS_CALL DoRelease(void* context,MmsHandle handle){auto& f=Self(context);const auto result=f.Touch(Release);if(result)return result;auto* slot=f.Find(handle);if(!slot)return MMS_E_NOT_FOUND;
        f.lastReleased=handle;*slot={};return MMS_OK;
    }
};
sdk::EvaluationServices Service(Fake& fake){sdk::EvaluationServices service;CHECK(sdk::EvaluationServices::Query(fake.Host(),service)==MMS_OK);return service;}

static_assert(!std::is_copy_constructible_v<sdk::EvaluationSceneSource>);
static_assert(!std::is_copy_assignable_v<sdk::EvaluationSceneSource>);
static_assert(!std::is_move_assignable_v<sdk::EvaluationSceneSource>);
static_assert(std::is_nothrow_move_constructible_v<sdk::EvaluationSceneSource>);
static_assert(std::is_nothrow_destructible_v<sdk::EvaluationSceneSource>);
static_assert(!std::is_copy_constructible_v<sdk::EvaluationSceneFrame>);
static_assert(!std::is_copy_assignable_v<sdk::EvaluationSceneFrame>);
static_assert(!std::is_move_assignable_v<sdk::EvaluationSceneFrame>);
static_assert(std::is_nothrow_move_constructible_v<sdk::EvaluationSceneFrame>);
static_assert(std::is_nothrow_destructible_v<sdk::EvaluationSceneFrame>);
static_assert(noexcept(sdk::EvaluationServices::Query(std::declval<const MmsHostV1&>(),std::declval<sdk::EvaluationServices&>())));
static_assert(noexcept(std::declval<const sdk::EvaluationSceneSource&>().Evaluate(MmsTime{},std::declval<sdk::EvaluationSceneFrame&>())));
static_assert(noexcept(std::declval<sdk::EvaluationSceneFrame&>().Release()));

void Negotiation(){
    stage="negotiation";Fake a(1000),b(2000);auto service=Service(a);const auto original=service.api();
    CHECK(a.queriedName&&a.queryMajor==1&&a.queryMinor==4&&a.queryBytes==sizeof(MmsEvaluationApiV1_4));
    auto host=b.Host();host.h.size=0;CHECK(sdk::EvaluationServices::Query(host,service)==MMS_E_VERSION);CHECK(Same(service.api(),original));
    host=b.Host();host.h.major=2;CHECK(sdk::EvaluationServices::Query(host,service)==MMS_E_VERSION);CHECK(Same(service.api(),original));
    host=b.Host();host.queryService=nullptr;CHECK(sdk::EvaluationServices::Query(host,service)==MMS_E_ARGUMENT);CHECK(Same(service.api(),original));
    for(auto error:{MMS_E_VERSION,MMS_E_UNSUPPORTED,MMS_E_THREAD,MMS_E_MEMORY,MMS_E_BUSY,MMS_E_FAILED}){
        b.results[Query]=error;CHECK(sdk::EvaluationServices::Query(b.Host(),service)==error);CHECK(Same(service.api(),original));
    }b.results[Query]=MMS_OK;
    for(int mode=1;mode<=3;++mode){b.throws[Query]=mode;CHECK(sdk::EvaluationServices::Query(b.Host(),service)==(mode==3?MMS_E_FAILED:MMS_E_MEMORY));CHECK(Same(service.api(),original));}b.throws[Query]=0;
    const auto valid=b.table;
    b.table.h.size=sizeof(b.table)-1;CHECK(sdk::EvaluationServices::Query(b.Host(),service)==MMS_E_VERSION);CHECK(Same(service.api(),original));
    b.table=valid;b.table.h.major=2;CHECK(sdk::EvaluationServices::Query(b.Host(),service)==MMS_E_VERSION);
    b.table=valid;b.table.h.minor=3;CHECK(sdk::EvaluationServices::Query(b.Host(),service)==MMS_E_VERSION);
    using Remove=void(*)(MmsEvaluationApiV1_4&);
    const Remove remove[]={
#define MISSING(member) [](MmsEvaluationApiV1_4& table){table.member=nullptr;}
        MISSING(getCapabilities),MISSING(createSource),MISSING(describeSource),MISSING(boneAt),MISSING(sampleBone),MISSING(retain),MISSING(release),
        MISSING(createSourceChannels),MISSING(describeSourceChannels),MISSING(morphAt),MISSING(sampleMorph),MISSING(describeSourceDisplay),
        MISSING(ikAt),MISSING(selectorAt),MISSING(sampleVisibility),MISSING(sampleIk),MISSING(sampleParent),MISSING(sampleParentState),
        MISSING(getSceneCapabilities),MISSING(createSceneSource),MISSING(describeSceneSource),MISSING(sceneModelAt),MISSING(sceneBoneAt),
        MISSING(evaluateFrame),MISSING(describeFrame),MISSING(frameModelAt),MISSING(readBonePose)
#undef MISSING
    };
    for(auto erase:remove){b.table=valid;erase(b.table);CHECK(sdk::EvaluationServices::Query(b.Host(),service)==MMS_E_UNSUPPORTED);CHECK(Same(service.api(),original));}
    b.table=valid;b.table.h.minor=5;b.table.h.size+=8;CHECK(sdk::EvaluationServices::Query(b.Host(),service)==MMS_OK);CHECK(service.api().context==&b);
    MmsEvaluationSceneCapsV1 caps{};CHECK(service.Capabilities(caps)==MMS_OK);CHECK(caps.flags==MMS_EVALUATION_SCENE_SKELETON&&caps.maxWorkspaceBytes==128);
    b.supported=0;CHECK(service.Capabilities(caps)==MMS_OK);CHECK(caps.flags==0);sdk::EvaluationSceneSource absent;
    CHECK(service.Capture(absent)==MMS_E_UNSUPPORTED);CHECK(!absent&&b.Live()==0);
}

void Ownership(){
    stage="ownership";Fake a(1000),b(2000);auto service=Service(a);sdk::EvaluationSceneSource source;
    CHECK(service.Capture(source,123456)==MMS_OK);CHECK(source.get()==a.lastIssued&&source.api().context==&a);
    CHECK(a.captureQuery.h.size==sizeof(MmsEvaluationSceneQueryV1)&&a.captureQuery.h.major==1&&a.captureQuery.h.minor==0);
    CHECK(a.captureQuery.maxRetainedBytes==123456&&a.captureQuery.flags==0&&a.captureQuery.reserved==0);
    MmsEvaluationSceneInfoV1 sourceInfo{};CHECK(source.Info(sourceInfo)==MMS_OK);CHECK(sourceInfo.modelCount==2&&sourceInfo.hostRevision==1077);
    MmsEvaluationSceneModelV1 model{};CHECK(source.ModelAt(1,model)==MMS_OK);CHECK(model.object.id.lo==1012&&model.object.id.hi==22&&model.object.generation==7&&model.boneCount==1&&model.flags==0);
    MmsEvaluationSceneBoneV1 bone{};CHECK(source.BoneAt(0,1,bone)==MMS_OK);CHECK(Same(bone.bone,a.BoneId(0,1))&&bone.index==1);
    sdk::EvaluationSceneFrame frame;const MmsTime exact{9007199254740992ll,270215977642229761ull};
    CHECK(source.Evaluate(exact,frame,{9000,10000})==MMS_OK);CHECK(Same(a.frameQuery.time,exact)&&a.frameQuery.source==source.get());
    CHECK(a.frameQuery.maxRetainedBytes==9000&&a.frameQuery.maxWorkspaceBytes==10000&&a.frameQuery.flags==0&&a.frameQuery.reserved==0);
    CHECK(a.frameQuery.h.size==sizeof(MmsEvaluationFrameQueryV1)&&a.frameQuery.h.major==1&&a.frameQuery.h.minor==0);
    MmsEvaluationFrameInfoV1 info{};CHECK(frame.Info(info)==MMS_OK);CHECK(info.captureId==sourceInfo.captureId&&info.frameId!=0&&info.modelCount==2);
    CHECK(frame.ModelAt(1,model)==MMS_OK);CHECK(model.flags==MMS_EVALUATION_SCENE_VISIBILITY_VALID&&model.visibility==197);
    MmsEvaluationScenePoseV1 pose{};CHECK(frame.ReadBonePose(1,0,pose)==MMS_OK);CHECK(Same(pose.bone,a.BoneId(1,0)));
    CHECK(pose.scaled[0]==2.5f&&pose.scaled[1]==-.125f&&pose.attachmentBefore[13]==1.25f&&pose.attachmentAfter[14]==-.75f&&pose.jointPosition[0]==42.125f);
    const auto captureCalls=a.calls[Capture],evaluateCalls=a.calls[Evaluate],retainCalls=a.calls[Retain];
    CHECK(service.Capture(source)==MMS_E_BUSY);CHECK(source.Evaluate({2,4},frame)==MMS_E_BUSY);CHECK(source.Retain(source)==MMS_E_BUSY);CHECK(frame.Retain(frame)==MMS_E_BUSY);
    CHECK(a.calls[Capture]==captureCalls&&a.calls[Evaluate]==evaluateCalls&&a.calls[Retain]==retainCalls);
    sdk::EvaluationSceneSource retained;CHECK(source.Retain(retained)==MMS_OK);CHECK(retained.get()!=source.get());
    MmsEvaluationSceneInfoV1 retainedInfo{};CHECK(retained.Info(retainedInfo)==MMS_OK);CHECK(Same(sourceInfo,retainedInfo));
    sdk::EvaluationSceneFrame retainedFrame;CHECK(frame.Retain(retainedFrame)==MMS_OK);CHECK(retainedFrame.get()!=frame.get());
    MmsEvaluationFrameInfoV1 retainedFrameInfo{};CHECK(retainedFrame.Info(retainedFrameInfo)==MMS_OK);CHECK(Same(info,retainedFrameInfo));
    const auto sourceHandle=source.get(),frameHandle=frame.get();const auto beforeMove=a.calls;
    sdk::EvaluationSceneSource moved(std::move(source));sdk::EvaluationSceneFrame movedFrame(std::move(frame));
    CHECK(!source&&!frame&&moved.get()==sourceHandle&&movedFrame.get()==frameHandle&&a.calls==beforeMove);
    CHECK(source.Release()==MMS_OK&&frame.Release()==MMS_OK&&a.calls==beforeMove);
    // A services object can be queried again, but previously acquired owners
    // keep their original table even when another host uses equal handle bits.
    CHECK(sdk::EvaluationServices::Query(b.Host(),service)==MMS_OK);sdk::EvaluationSceneSource other;CHECK(service.Capture(other)==MMS_OK);
    CHECK(other.get()==sourceHandle&&other.api().context==&b&&moved.api().context==&a);
    CHECK(moved.Info(sourceInfo)==MMS_OK&&sourceInfo.hostRevision==1077);CHECK(other.Info(sourceInfo)==MMS_OK&&sourceInfo.hostRevision==2077);
    CHECK(moved.Release()==MMS_OK&&retained.Release()==MMS_OK);CHECK(movedFrame.ReadBonePose(0,0,pose)==MMS_OK);
    CHECK(movedFrame.Release()==MMS_OK&&retainedFrame.Release()==MMS_OK&&other.Release()==MMS_OK);CHECK(a.Live()==0&&b.Live()==0);
    sdk::EvaluationSceneSource noAllocation;auto aService=Service(a);failAfter=0;
    const auto capture=aService.Capture(noAllocation);sdk::EvaluationSceneFrame normalized;const auto evaluated=noAllocation.Evaluate({2,4},normalized);
    sdk::EvaluationSceneFrame copy;const auto retainedResult=normalized.Retain(copy);failAfter=-1;
    CHECK(capture==MMS_OK&&evaluated==MMS_OK&&retainedResult==MMS_OK);CHECK(normalized.Info(info)==MMS_OK&&info.time.numerator==1&&info.time.denominator==2);
    CHECK(noAllocation.Release()==MMS_OK&&normalized.Release()==MMS_OK&&copy.Release()==MMS_OK&&a.Live()==0);
}

template<class T,class ReadFunction>void ReadFailures(Fake& fake,Op operation,ReadFunction read){
    for(auto error:{MMS_E_ARGUMENT,MMS_E_NOT_FOUND,MMS_E_NOT_READY,MMS_E_THREAD,MMS_E_MEMORY,MMS_E_FAILED}){
        auto output=Poison<T>();fake.results[operation]=error;CHECK(read(output)==error);CHECK(Unchanged(output));
    }fake.results[operation]=MMS_OK;
    for(int fault=1;fault<=3;++fault){auto output=Poison<T>();fake.throws[operation]=fault;CHECK(read(output)==(fault==3?MMS_E_FAILED:MMS_E_MEMORY));CHECK(Unchanged(output));}fake.throws[operation]=0;
    for(int fault=1;fault<=6;++fault){auto output=Poison<T>();fake.bad[operation]=fault;CHECK(read(output)==(fault<=3?MMS_E_VERSION:MMS_E_FAILED));CHECK(Unchanged(output));}
    fake.bad[operation]=7;T larger{};CHECK(read(larger)==MMS_OK&&larger.h.size==sizeof(T)+8);fake.bad[operation]=0;
}
void FailedReads(){
    stage="read failures";Fake fake;auto service=Service(fake);sdk::EvaluationSceneSource source;CHECK(service.Capture(source)==MMS_OK);sdk::EvaluationSceneFrame frame;CHECK(source.Evaluate({1,2},frame)==MMS_OK);
    ReadFailures<MmsEvaluationSceneCapsV1>(fake,Caps,[&](auto& out){return service.Capabilities(out);});
    ReadFailures<MmsEvaluationSceneInfoV1>(fake,SourceInfo,[&](auto& out){return source.Info(out);});
    ReadFailures<MmsEvaluationSceneModelV1>(fake,SourceModel,[&](auto& out){return source.ModelAt(0,out);});
    ReadFailures<MmsEvaluationSceneBoneV1>(fake,SourceBone,[&](auto& out){return source.BoneAt(0,1,out);});
    ReadFailures<MmsEvaluationFrameInfoV1>(fake,FrameInfo,[&](auto& out){return frame.Info(out);});
    ReadFailures<MmsEvaluationSceneModelV1>(fake,FrameModel,[&](auto& out){return frame.ModelAt(0,out);});
    ReadFailures<MmsEvaluationScenePoseV1>(fake,Read,[&](auto& out){return frame.ReadBonePose(0,1,out);});
    auto model=Poison<MmsEvaluationSceneModelV1>();auto bone=Poison<MmsEvaluationSceneBoneV1>();auto pose=Poison<MmsEvaluationScenePoseV1>();
    CHECK(source.ModelAt(2,model)==MMS_E_NOT_FOUND&&Unchanged(model));CHECK(frame.ModelAt(2,model)==MMS_E_NOT_FOUND&&Unchanged(model));
    CHECK(source.BoneAt(1,1,bone)==MMS_E_NOT_FOUND&&Unchanged(bone));CHECK(frame.ReadBonePose(1,1,pose)==MMS_E_NOT_FOUND&&Unchanged(pose));
    sdk::EvaluationServices emptyService;sdk::EvaluationSceneSource emptySource;sdk::EvaluationSceneFrame emptyFrame;
    auto caps=Poison<MmsEvaluationSceneCapsV1>();auto sourceInfo=Poison<MmsEvaluationSceneInfoV1>();auto frameInfo=Poison<MmsEvaluationFrameInfoV1>();
    CHECK(emptyService.Capabilities(caps)==MMS_E_NOT_READY&&Unchanged(caps));CHECK(emptyService.Capture(emptySource)==MMS_E_NOT_READY&&!emptySource);
    CHECK(emptySource.Info(sourceInfo)==MMS_E_NOT_READY&&Unchanged(sourceInfo));CHECK(emptySource.ModelAt(0,model)==MMS_E_NOT_READY&&Unchanged(model));
    CHECK(emptySource.BoneAt(0,0,bone)==MMS_E_NOT_READY&&Unchanged(bone));CHECK(emptySource.Evaluate({0,1},emptyFrame)==MMS_E_NOT_READY&&!emptyFrame);
    CHECK(emptySource.Retain(emptySource)==MMS_E_NOT_READY&&!emptySource);CHECK(emptyFrame.Retain(emptyFrame)==MMS_E_NOT_READY&&!emptyFrame);
    CHECK(emptyFrame.Info(frameInfo)==MMS_E_NOT_READY&&Unchanged(frameInfo));CHECK(emptyFrame.ModelAt(0,model)==MMS_E_NOT_READY&&Unchanged(model));CHECK(emptyFrame.ReadBonePose(0,0,pose)==MMS_E_NOT_READY&&Unchanged(pose));
}

void FailedOwnership(){
    stage="acquire failures";Fake fake;auto service=Service(fake);sdk::EvaluationSceneSource source;CHECK(service.Capture(source)==MMS_OK);
    sdk::EvaluationSceneFrame frame;CHECK(source.Evaluate({0,1},frame)==MMS_OK);const auto initialLive=fake.Live(),releases=fake.calls[Release];
    // Failure scribbles are deliberately not published by this fake. A C host
    // that secretly allocated on failure violates the ABI; the SDK cannot tell
    // such an allocation from arbitrary bits and must not release those bits.
    for(Op operation:{Capture,Evaluate,Retain}){
        for(auto error:{MMS_E_ARGUMENT,MMS_E_THREAD,MMS_E_BUSY,MMS_E_MEMORY,MMS_E_NOT_READY,MMS_E_FAILED}){
            fake.results[operation]=error;sdk::EvaluationSceneSource out;sdk::EvaluationSceneFrame frameOut;
            const auto result=operation==Capture?service.Capture(out):operation==Evaluate?source.Evaluate({0,1},frameOut):source.Retain(out);
            CHECK(result==error&&!out&&!frameOut&&fake.Live()==initialLive&&fake.calls[Release]==releases);
        }fake.results[operation]=MMS_OK;
        for(int fault=1;fault<=3;++fault){fake.throws[operation]=fault;sdk::EvaluationSceneSource out;sdk::EvaluationSceneFrame frameOut;
            const auto result=operation==Capture?service.Capture(out):operation==Evaluate?source.Evaluate({0,1},frameOut):frame.Retain(frameOut);
            CHECK(result==(fault==3?MMS_E_FAILED:MMS_E_MEMORY));CHECK(!out&&!frameOut&&fake.Live()==initialLive&&fake.calls[Release]==releases);
        }fake.throws[operation]=0;
        for(int fault=1;fault<=(operation==Capture?1:2);++fault){fake.bad[operation]=fault;sdk::EvaluationSceneSource out;sdk::EvaluationSceneFrame frameOut;
            const auto result=operation==Capture?service.Capture(out):operation==Evaluate?source.Evaluate({0,1},frameOut):source.Retain(out);
            CHECK(result==MMS_E_FAILED&&!out&&!frameOut&&fake.Live()==initialLive&&fake.calls[Release]==releases);
        }fake.bad[operation]=0;
    }
    sdk::EvaluationSceneFrame invalid;CHECK(source.Evaluate({-1,1},invalid)==MMS_E_ARGUMENT&&!invalid);CHECK(source.Evaluate({0,0},invalid)==MMS_E_ARGUMENT&&!invalid);
    // No post-acquisition read: ownership transfers even if a separate
    // description is malformed, so the caller can always release the lease.
    fake.bad[SourceInfo]=4;sdk::EvaluationSceneSource malformed;const auto describes=fake.calls[SourceInfo];
    CHECK(service.Capture(malformed)==MMS_OK&&malformed&&fake.calls[SourceInfo]==describes);auto info=Poison<MmsEvaluationSceneInfoV1>();
    CHECK(malformed.Info(info)==MMS_E_FAILED&&Unchanged(info));CHECK(malformed.Release()==MMS_OK);fake.bad[SourceInfo]=0;
    fake.bad[FrameInfo]=4;sdk::EvaluationSceneFrame malformedFrame;const auto frameDescribes=fake.calls[FrameInfo];
    CHECK(source.Evaluate({1,2},malformedFrame)==MMS_OK&&malformedFrame&&fake.calls[FrameInfo]==frameDescribes);auto frameInfo=Poison<MmsEvaluationFrameInfoV1>();
    CHECK(malformedFrame.Info(frameInfo)==MMS_E_FAILED&&Unchanged(frameInfo));CHECK(malformedFrame.Release()==MMS_OK);fake.bad[FrameInfo]=0;
}

void ReleaseAndTransfer(){
    stage="release and transfer";Fake fake;auto service=Service(fake);sdk::EvaluationSceneSource source;CHECK(service.Capture(source)==MMS_OK);const auto handle=source.get();const auto table=source.api();
    for(auto error:{MMS_E_BUSY,MMS_E_NOT_READY,MMS_E_NOT_FOUND,MMS_E_FAILED}){
        fake.results[Release]=error;CHECK(source.Release()==error&&source.get()==handle&&Same(source.api(),table)&&fake.Find(handle));
    }fake.results[Release]=MMS_OK;
    for(int fault=1;fault<=3;++fault){fake.throws[Release]=fault;CHECK(source.Release()==(fault==3?MMS_E_FAILED:MMS_E_MEMORY));CHECK(source.get()==handle&&Same(source.api(),table)&&fake.Find(handle));}fake.throws[Release]=0;
    CHECK(source.Release()==MMS_OK&&!source&&!fake.Find(handle));const auto releases=fake.calls[Release];CHECK(source.Release()==MMS_OK&&fake.calls[Release]==releases);
    {sdk::EvaluationSceneSource owner;CHECK(service.Capture(owner)==MMS_OK);sdk::EvaluationSceneSource moved(std::move(owner));}
    CHECK(fake.calls[Release]==releases+1&&fake.Live()==0);
    MmsHandle abandoned=0;const auto beforeFailure=fake.calls[Release];
    {sdk::EvaluationSceneSource owner;CHECK(service.Capture(owner)==MMS_OK);abandoned=owner.get();fake.results[Release]=MMS_E_BUSY;}
    CHECK(fake.calls[Release]==beforeFailure+1&&fake.Find(abandoned));fake.results[Release]=MMS_OK;
    CHECK(Fake::DoRelease(&fake,abandoned)==MMS_OK); // Destructor tries once, without an invisible retry registry.
    MmsHandle detached=0;MmsEvaluationApiV1_4 detachedTable{};const auto beforeDetach=fake.calls[Release];
    {sdk::EvaluationSceneSource owner;CHECK(service.Capture(owner)==MMS_OK);detachedTable=owner.api();detached=owner.detach();CHECK(!owner&&owner.api().context==nullptr);}
    CHECK(detached&&fake.calls[Release]==beforeDetach&&fake.Find(detached));CHECK(detachedTable.release(detachedTable.context,detached)==MMS_OK);
    sdk::EvaluationSceneSource frameSource;CHECK(service.Capture(frameSource)==MMS_OK);sdk::EvaluationSceneFrame frame;CHECK(frameSource.Evaluate({0,1},frame)==MMS_OK);
    const auto frameHandle=frame.get();fake.results[Release]=MMS_E_BUSY;CHECK(frame.Release()==MMS_E_BUSY&&frame.get()==frameHandle);
    fake.results[Release]=MMS_OK;detachedTable=frame.api();detached=frame.detach();CHECK(!frame&&detached==frameHandle);CHECK(detachedTable.release(detachedTable.context,detached)==MMS_OK);
    CHECK(frameSource.Release()==MMS_OK&&fake.Live()==0);
}

void ThreadAuthority(){
    stage="thread authority";Fake fake;sdk::EvaluationServices service;MmsResult result=MMS_E_FAILED;
    std::thread query([&]{result=sdk::EvaluationServices::Query(fake.Host(),service);});query.join();CHECK(result==MMS_OK);
    sdk::EvaluationSceneSource source;CHECK(service.Capture(source)==MMS_OK); // Querying worker is NOT the UI authority.
    sdk::EvaluationSceneSource rejected;std::thread capture([&]{result=service.Capture(rejected);});capture.join();CHECK(result==MMS_E_THREAD&&!rejected);
    bool readOk=false;std::thread reader([&]{
        sdk::EvaluationSceneFrame frame,retained;MmsEvaluationScenePoseV1 pose{};MmsEvaluationSceneInfoV1 info{};
        readOk=source.Info(info)==MMS_OK&&source.Evaluate({7,30},frame)==MMS_OK&&frame.ReadBonePose(0,0,pose)==MMS_OK&&pose.rigid[12]==7.f&&frame.Retain(retained)==MMS_OK&&frame.Release()==MMS_OK&&retained.Release()==MMS_OK;
    });reader.join();CHECK(readOk);CHECK(source.Release()==MMS_OK&&fake.Live()==0);
}
} // namespace
int main(){
    try{Negotiation();Ownership();FailedReads();FailedOwnership();ReleaseAndTransfer();ThreadAuthority();}
    catch(const std::exception& error){failAfter=-1;std::fprintf(stderr,"EXCEPTION %s: %s\n",stage,error.what());return 1;}
    catch(...){failAfter=-1;std::fprintf(stderr,"EXCEPTION %s\n",stage);return 1;}
    std::printf("evaluation_cpp_sdk_tests: %u checks passed\n",checks);return 0;
}
