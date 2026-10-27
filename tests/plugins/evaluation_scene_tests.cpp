// Mock host contracts only; native scene parity and DLL lifetime have separate targets.
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
#include <type_traits>

static thread_local int failAfter=-1;
void* operator new(std::size_t n){if(failAfter==0)throw std::bad_alloc();if(failAfter>0)--failAfter;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace mikumikustudio::plugins;
using mikumikustudio::EvaluationTime;
using Api=MmsEvaluationApiV1_4;
static unsigned checks=0;static const char* section="setup";
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %s line %d: %s\n",section,__LINE__,#x);std::exit(1);}}while(false)
static constexpr uint64_t MiB=1024ull*1024;
static constexpr MmsHandle sentinel=0x123456789abcdef0ull;
static constexpr MmsObjectRef object{{11,22},3};
template<class T>static T Poison(){T v;std::memset(&v,0xa5,sizeof(v));return v;}
template<class T>static bool Unchanged(const T& v){const auto p=Poison<T>();return !std::memcmp(&v,&p,sizeof(v));}
template<class T>static bool HeaderOk(const T& v,unsigned minor=0){return v.h.size==sizeof(v)&&v.h.major==1&&v.h.minor==minor;}
static MmsEvaluationSceneQueryV1 SceneQuery(uint64_t bytes=0){return {Header<MmsEvaluationSceneQueryV1>(),bytes,0,0};}
static MmsEvaluationFrameQueryV1 FrameQuery(MmsHandle source,MmsTime time={1,4},uint64_t bytes=0,uint64_t workspace=0){return {Header<MmsEvaluationFrameQueryV1>(),source,time,bytes,workspace,0,0};}
static Api Bind(EvaluationStore& store,EvaluationStore::Client& client){client.owner=&store;return store.Api14(client);}
struct Gate{
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr),proceed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<unsigned> entered{0};unsigned expected=1;
    explicit Gate(unsigned count=1):expected(count){}
    ~Gate(){if(ready)CloseHandle(ready);if(proceed)CloseHandle(proceed);}
    bool Wait(){if(entered.fetch_add(1)+1==expected)SetEvent(ready);return WaitForSingleObject(proceed,15000)==WAIT_OBJECT_0;}
};
struct Control{
    std::atomic<int> captureMode{0},frameMode{0},poseMode{0};std::atomic<unsigned> captures{0},evaluations{0},samples{0};
    std::atomic<uint64_t> sourceBytes{4096},frameBytes{8192},generation{3};std::atomic<double> continuous{-1};std::atomic<uint32_t> discrete{UINT32_MAX};
    std::shared_ptr<Gate> evaluateGate,poseGate;std::weak_ptr<const EvaluationSceneModels> models;
    bool empty=false;
};
static MmsEvaluationScenePoseV1 Pose(double frame,uint64_t generation,unsigned model,unsigned boneIndex){
    MmsEvaluationScenePoseV1 p{};p.h={1,77,88};p.bone={999,888}; // Host must replace these two metadata fields.
    for(unsigned i=0;i<16;++i)p.rigid[i]=p.scaled[i]=p.attachmentBefore[i]=p.attachmentAfter[i]=i%5==0?1.f:0.f;
    const float value=float(frame+double(generation));p.rigid[12]=value+10*model+boneIndex;
    p.scaled[0]=2.5f;p.scaled[1]=-.125f;p.scaled[6]=.25f;p.scaled[12]=p.rigid[12]+.25f;
    p.attachmentBefore[13]=-2*value;p.attachmentAfter[14]=3*value;
    p.jointPosition[0]=value+.125f;p.jointPosition[1]=float(model)+.5f;p.jointPosition[2]=float(boneIndex)+.75f;return p;
}
static EvaluationStore::CaptureChannels ChannelCapture(){return [](const auto& query,uint32_t mask,uint64_t,auto& out){
    auto data=std::make_shared<EvaluationData>();data->object=query.object;data->channels=mask;data->retainedBytes=4096;
    data->bones={{Header<MmsEvaluationBoneV1>(),{33,44},MMS_EVALUATION_BONE_LOCAL,0}};
    data->sample=[](uint32_t,double,MmsBoneChannelsV1& v){v={};v.h=Header<MmsBoneChannelsV1>();v.rotation[3]=1;v.scale[0]=v.scale[4]=v.scale[8]=1;return MMS_OK;};out=data;return MMS_OK;
};}
static EvaluationStore::CaptureScene SceneCapture(const std::shared_ptr<Control>& control){return [control](const auto&,uint64_t,auto& out)->MmsResult{
    ++control->captures;const auto mode=control->captureMode.load();if(mode==1)return MMS_OK;if(mode==10)throw std::bad_alloc();if(mode==11)throw std::runtime_error("capture");
    auto models=std::make_shared<EvaluationSceneModels>();const auto generation=control->generation.load();
    if(!control->empty)*models={{{{11,22},generation},{{33,44},{55,66}}},{{{77,88},generation+1},{{99,111}}}};
    if(mode==4)(*models)[0].object.id={};if(mode==5)(*models)[0].object.generation=0;
    if(mode==6)(*models)[1].object={(*models)[0].object.id,generation+10};
    if(mode==7)(*models)[0].bones[0]={};if(mode==8)(*models)[0].bones[1]=(*models)[0].bones[0];
    if(mode==9)(*models)[1].bones[0]=(*models)[0].bones[0];
    if(mode==13)(*models)[1].bones[0]=(*models)[0].object.id;
    auto source=std::make_shared<EvaluationSceneSourceData>();source->models=models;source->hostRevision=123;source->retainedBytes=control->sourceBytes;
    control->models=models;const auto gate=control->evaluateGate;
    source->evaluate=[control,models,generation,gate](const EvaluationTime& time,uint64_t,uint64_t,std::shared_ptr<const EvaluationSceneFrameData>& result)->MmsResult{
        ++control->evaluations;control->continuous=time.Frame();control->discrete=time.DiscreteFrame();
        if(gate&&!gate->Wait())return MMS_E_FAILED;
        const auto mode=control->frameMode.load();if(mode==1)return MMS_OK;if(mode==7)throw std::bad_alloc();if(mode==8)throw std::runtime_error("frame");
        auto frame=std::make_shared<EvaluationSceneFrameData>();frame->models=models;frame->retainedBytes=control->frameBytes;
        for(unsigned i=0;i<models->size();++i)frame->visibility.push_back(i?255:197);
        const auto sampleGate=control->poseGate;const auto continuous=time.Frame();
        frame->sample=[control,generation,continuous,sampleGate](uint32_t model,uint32_t boneIndex,MmsEvaluationScenePoseV1& pose)->MmsResult{
            ++control->samples;if(sampleGate&&!sampleGate->Wait())return MMS_E_FAILED;pose=Pose(continuous,generation,model,boneIndex);
            switch(control->poseMode.load()){
            case 1:pose.rigid[0]=std::numeric_limits<float>::quiet_NaN();break;
            case 2:pose.scaled[5]=std::numeric_limits<float>::infinity();break;
            case 3:pose.attachmentBefore[10]=-std::numeric_limits<float>::infinity();break;
            case 4:pose.attachmentAfter[15]=std::numeric_limits<float>::quiet_NaN();break;
            case 5:pose.jointPosition[2]=std::numeric_limits<float>::infinity();break;
            case 6:pose.reserved=1;break;case 7:throw std::bad_alloc();case 8:throw std::runtime_error("pose");case 9:return MMS_E_CANCELLED;
            }return MMS_OK;
        };
        if(mode==2)frame->models={};if(mode==3)frame->models=std::make_shared<EvaluationSceneModels>(*models);
        if(mode==4)frame->visibility.clear();if(mode==5)frame->visibility.push_back(0);if(mode==6)frame->sample={};
        result=frame;return mode==9?MMS_E_CANCELLED:MMS_OK;
    };
    if(mode==2)source->models={};if(mode==3)source->evaluate={};out=source;return mode==12?MMS_E_CANCELLED:MMS_OK;
};}
static std::unique_ptr<EvaluationStore> Store(const std::shared_ptr<Control>& control){return std::make_unique<EvaluationStore>(ChannelCapture(),1,SceneCapture(control));}
static MmsHandle Capture(const Api& api,uint64_t bytes=0){MmsHandle out=sentinel;const auto q=SceneQuery(bytes);CHECK(api.createSceneSource(api.context,&q,&out)==MMS_OK&&out&&out!=sentinel);return out;}
static MmsHandle Evaluate(const Api& api,MmsHandle source,MmsTime time={1,4},uint64_t bytes=0,uint64_t workspace=0){const auto query=FrameQuery(source,time,bytes,workspace);MmsHandle out=sentinel;CHECK(api.evaluateFrame(api.context,&query,&out)==MMS_OK&&out&&out!=sentinel);return out;}
static MmsEvaluationSceneInfoV1 DescribeSource(const Api& api,MmsHandle source){auto out=Poison<MmsEvaluationSceneInfoV1>();CHECK(api.describeSceneSource(api.context,source,&out)==MMS_OK&&HeaderOk(out));return out;}
static MmsEvaluationFrameInfoV1 DescribeFrame(const Api& api,MmsHandle frame){auto out=Poison<MmsEvaluationFrameInfoV1>();CHECK(api.describeFrame(api.context,frame,&out)==MMS_OK&&HeaderOk(out));return out;}
static bool PoseMatches(const MmsEvaluationScenePoseV1& actual,double time,uint64_t generation,unsigned model,unsigned boneIndex){const auto expected=Pose(time,generation,model,boneIndex);
    const MmsId id=model?MmsId{99,111}:boneIndex?MmsId{55,66}:MmsId{33,44};
    return HeaderOk(actual)&&Same(actual.bone,id)&&!actual.reserved&&!std::memcmp(actual.rigid,expected.rigid,sizeof(actual.rigid))&&
        !std::memcmp(actual.scaled,expected.scaled,sizeof(actual.scaled))&&!std::memcmp(actual.attachmentBefore,expected.attachmentBefore,sizeof(actual.attachmentBefore))&&
        !std::memcmp(actual.attachmentAfter,expected.attachmentAfter,sizeof(actual.attachmentAfter))&&!std::memcmp(actual.jointPosition,expected.jointPosition,sizeof(actual.jointPosition));
}
static void TablesAndCapabilities(){
    section="1.4 ABI prefix and explicit capability discovery";
    static_assert(sizeof(Api)==232&&sizeof(MmsEvaluationSceneCapsV1)==72&&sizeof(MmsEvaluationSceneQueryV1)==24);
    static_assert(sizeof(MmsEvaluationSceneInfoV1)==40&&sizeof(MmsEvaluationSceneModelV1)==48&&sizeof(MmsEvaluationSceneBoneV1)==32);
    static_assert(sizeof(MmsEvaluationFrameQueryV1)==56&&sizeof(MmsEvaluationFrameInfoV1)==64&&sizeof(MmsEvaluationScenePoseV1)==296);
    static_assert(offsetof(Api,getSceneCapabilities)==sizeof(MmsEvaluationApiV1_3));
    static_assert(std::is_same_v<decltype(Api::evaluateFrame),MmsResult(MMS_CALL*)(void*,const MmsEvaluationFrameQueryV1*,MmsHandle*)>);
    static_assert(std::is_same_v<decltype(Api::readBonePose),MmsResult(MMS_CALL*)(void*,MmsHandle,uint32_t,uint32_t,MmsEvaluationScenePoseV1*)>);
    EvaluationStore unsupported(ChannelCapture(),1);EvaluationStore::Client c;const auto absent=Bind(unsupported,c);MmsEvaluationSceneCapsV1 caps{};
    CHECK(absent.getSceneCapabilities(absent.context,&caps)==MMS_OK&&HeaderOk(caps)&&caps.flags==0);const auto query=SceneQuery();MmsHandle out=sentinel;
    CHECK(absent.createSceneSource(absent.context,&query,&out)==MMS_E_UNSUPPORTED&&out==sentinel);
    auto control=std::make_shared<Control>();auto store=Store(control);EvaluationStore::Client client;const auto api=Bind(*store,client);const auto old=store->Api13(client);
    CHECK(HeaderOk(api,4)&&!std::memcmp(reinterpret_cast<const char*>(&api)+sizeof(MmsHeader),reinterpret_cast<const char*>(&old)+sizeof(MmsHeader),sizeof(old)-sizeof(MmsHeader)));
    CHECK(api.getSceneCapabilities(api.context,&caps)==MMS_OK&&HeaderOk(caps)&&caps.flags==MMS_EVALUATION_SCENE_SKELETON&&!caps.reserved);
    CHECK(caps.maxSourceBytes==64*MiB&&caps.maxFrameBytes==64*MiB&&caps.maxClientBytes==128*MiB&&caps.maxTotalBytes==256*MiB);
    CHECK(caps.maxWorkspaceBytes==128*MiB&&caps.maxClientWorkspaceBytes==256*MiB&&caps.maxTotalWorkspaceBytes==512*MiB);
}
static void EnumerationTimeAndLifetime(){
    section="frozen identities and full floating pose records across independent frames";
    auto control=std::make_shared<Control>();auto store=Store(control);EvaluationStore::Client client;const auto api=Bind(*store,client);const auto source=Capture(api);const auto sourceInfo=DescribeSource(api,source);
    CHECK(sourceInfo.modelCount==2&&sourceInfo.flags==MMS_EVALUATION_SCENE_SKELETON&&sourceInfo.hostRevision==123&&sourceInfo.captureId);
    for(unsigned model=0;model<2;++model){MmsEvaluationSceneModelV1 out{};CHECK(api.sceneModelAt(api.context,source,model,&out)==MMS_OK&&HeaderOk(out));
        CHECK(out.boneCount==(model?1u:2u)&&out.object.generation==3+model&&!out.flags&&!out.visibility&&!out.reserved);
        for(unsigned boneIndex=0;boneIndex<out.boneCount;++boneIndex){MmsEvaluationSceneBoneV1 bone{};CHECK(api.sceneBoneAt(api.context,source,model,boneIndex,&bone)==MMS_OK&&HeaderOk(bone)&&bone.index==boneIndex&&!bone.reserved);CHECK(Same(bone.bone,model?MmsId{99,111}:boneIndex?MmsId{55,66}:MmsId{33,44}));}}
    const auto first=Evaluate(api,source,{2,4}),second=Evaluate(api,source,{1,4});const auto firstInfo=DescribeFrame(api,first),secondInfo=DescribeFrame(api,second);
    CHECK(firstInfo.captureId==sourceInfo.captureId&&secondInfo.captureId==sourceInfo.captureId&&firstInfo.frameId&&firstInfo.frameId!=secondInfo.frameId);
    CHECK(firstInfo.time.numerator==1&&firstInfo.time.denominator==2&&firstInfo.modelCount==2&&firstInfo.hostRevision==123&&firstInfo.flags==MMS_EVALUATION_SCENE_SKELETON);
    for(unsigned model=0;model<2;++model){MmsEvaluationSceneModelV1 info{};CHECK(api.frameModelAt(api.context,second,model,&info)==MMS_OK&&HeaderOk(info)&&info.flags==MMS_EVALUATION_SCENE_VISIBILITY_VALID&&info.visibility==(model?255u:197u)&&!info.reserved);
        for(unsigned boneIndex=0;boneIndex<info.boneCount;++boneIndex){MmsEvaluationScenePoseV1 pose{};CHECK(api.readBonePose(api.context,second,model,boneIndex,&pose)==MMS_OK&&PoseMatches(pose,7.5,3,model,boneIndex));}}
    const auto exact=Evaluate(api,source,{9007199254740992ll,270215977642229761ull});CHECK(control->continuous==1&&control->discrete==0);CHECK(api.release(api.context,exact)==MMS_OK);
    const auto zero=Evaluate(api,source,{0,UINT64_MAX});const auto zeroInfo=DescribeFrame(api,zero);CHECK(zeroInfo.time.numerator==0&&zeroInfo.time.denominator==1);CHECK(api.release(api.context,zero)==MMS_OK);
    control->generation=50;const auto changed=Capture(api),newFrame=Evaluate(api,changed);MmsEvaluationSceneModelV1 oldModel{},newModel{};
    CHECK(api.frameModelAt(api.context,first,0,&oldModel)==MMS_OK&&oldModel.object.generation==3);
    CHECK(api.frameModelAt(api.context,newFrame,0,&newModel)==MMS_OK&&newModel.object.generation==50);
    CHECK(api.release(api.context,source)==MMS_OK&&api.release(api.context,changed)==MMS_OK);MmsEvaluationScenePoseV1 pose{};
    failAfter=0;const auto result=api.readBonePose(api.context,first,0,0,&pose);failAfter=-1;CHECK(result==MMS_OK&&PoseMatches(pose,15,3,0,0));
    CHECK(api.release(api.context,first)==MMS_OK&&api.release(api.context,second)==MMS_OK&&api.release(api.context,newFrame)==MMS_OK&&!store->HasLeases(client)&&client.bytes==0);
    control->empty=true;const auto empty=Capture(api),emptyFrame=Evaluate(api,empty);CHECK(DescribeSource(api,empty).modelCount==0&&DescribeFrame(api,emptyFrame).modelCount==0);
    auto absent=Poison<MmsEvaluationSceneModelV1>();CHECK(api.frameModelAt(api.context,emptyFrame,0,&absent)==MMS_E_NOT_FOUND&&Unchanged(absent));
    CHECK(api.release(api.context,empty)==MMS_OK&&api.release(api.context,emptyFrame)==MMS_OK);
}
static void AdmissionAndKinds(){
    section="query validation, owner isolation and three distinct payload kinds";
    auto control=std::make_shared<Control>();auto store=Store(control);EvaluationStore::Client client,foreign;const auto api=Bind(*store,client),other=Bind(*store,foreign);
    auto query=SceneQuery();MmsHandle out=sentinel;
    CHECK(api.createSceneSource(api.context,nullptr,&out)==MMS_E_ARGUMENT&&out==sentinel);
    CHECK(api.createSceneSource(api.context,&query,nullptr)==MMS_E_ARGUMENT);
    CHECK(api.createSceneSource(nullptr,&query,&out)==MMS_E_ARGUMENT&&out==sentinel);
    CHECK(api.getSceneCapabilities(api.context,nullptr)==MMS_E_ARGUMENT);
    for(unsigned field=0;field<5;++field){query=SceneQuery();if(field==0)--query.h.size;if(field==1)query.h.major=2;if(field==2)query.h.minor=1;if(field==3)query.flags=1;if(field==4)query.reserved=1;
        CHECK(api.createSceneSource(api.context,&query,&out)==MMS_E_ARGUMENT&&out==sentinel);}
    CHECK(control->captures==0);const auto source=Capture(api),frame=Evaluate(api,source);
    const MmsEvaluationSourceQueryV1 channelQuery{Header<MmsEvaluationSourceQueryV1>(),object,0,0,0};MmsHandle channel=0;CHECK(api.createSource(api.context,&channelQuery,&channel)==MMS_OK);
    for(MmsHandle wrong:{channel,frame}){auto info=Poison<MmsEvaluationSceneInfoV1>();auto model=Poison<MmsEvaluationSceneModelV1>();auto bone=Poison<MmsEvaluationSceneBoneV1>();
        CHECK(api.describeSceneSource(api.context,wrong,&info)==MMS_E_UNSUPPORTED&&Unchanged(info));
        CHECK(api.sceneModelAt(api.context,wrong,0,&model)==MMS_E_UNSUPPORTED&&Unchanged(model));
        CHECK(api.sceneBoneAt(api.context,wrong,0,0,&bone)==MMS_E_UNSUPPORTED&&Unchanged(bone));
        const auto q=FrameQuery(wrong);CHECK(api.evaluateFrame(api.context,&q,&out)==MMS_E_UNSUPPORTED&&out==sentinel);}
    for(MmsHandle wrong:{channel,source}){auto info=Poison<MmsEvaluationFrameInfoV1>();auto model=Poison<MmsEvaluationSceneModelV1>();auto pose=Poison<MmsEvaluationScenePoseV1>();
        CHECK(api.describeFrame(api.context,wrong,&info)==MMS_E_UNSUPPORTED&&Unchanged(info));
        CHECK(api.frameModelAt(api.context,wrong,0,&model)==MMS_E_UNSUPPORTED&&Unchanged(model));
        CHECK(api.readBonePose(api.context,wrong,0,0,&pose)==MMS_E_UNSUPPORTED&&Unchanged(pose));}
    for(MmsHandle wrong:{source,frame}){auto info=Poison<MmsEvaluationSourceInfoV1>();auto bone=Poison<MmsEvaluationBoneV1>();auto pose=Poison<MmsBoneChannelsV1>();
        CHECK(api.describeSource(api.context,wrong,&info)==MMS_E_UNSUPPORTED&&Unchanged(info));
        CHECK(api.boneAt(api.context,wrong,0,&bone)==MMS_E_UNSUPPORTED&&Unchanged(bone));
        CHECK(api.sampleBone(api.context,wrong,{33,44},{0,1},&pose)==MMS_E_UNSUPPORTED&&Unchanged(pose));}
    auto frameQuery=FrameQuery(source);const auto evaluations=control->evaluations.load();
    CHECK(api.evaluateFrame(api.context,nullptr,&out)==MMS_E_ARGUMENT&&out==sentinel);
    CHECK(api.evaluateFrame(api.context,&frameQuery,nullptr)==MMS_E_ARGUMENT);
    for(unsigned field=0;field<6;++field){frameQuery=FrameQuery(source);if(field==0)--frameQuery.h.size;if(field==1)frameQuery.h.major=2;if(field==2)frameQuery.h.minor=1;if(field==3)frameQuery.flags=1;if(field==4)frameQuery.reserved=1;if(field==5)frameQuery.maxWorkspaceBytes=128*MiB+1;
        CHECK(api.evaluateFrame(api.context,&frameQuery,&out)==MMS_E_ARGUMENT&&out==sentinel);}
    for(const auto time:std::array<MmsTime,5>{{{-1,1},{0,0},{INT64_MAX,1},{2147483647,30},{2147483646000000001ll,30000000000ull}}}){
        frameQuery=FrameQuery(source,time);CHECK(api.evaluateFrame(api.context,&frameQuery,&out)==MMS_E_ARGUMENT&&out==sentinel);}
    CHECK(control->evaluations==evaluations);
    for(MmsHandle bad:{MmsHandle(0),sentinel}){auto info=Poison<MmsEvaluationSceneInfoV1>();auto pose=Poison<MmsEvaluationScenePoseV1>();
        CHECK(api.describeSceneSource(api.context,bad,&info)==MMS_E_NOT_FOUND&&Unchanged(info));CHECK(api.readBonePose(api.context,bad,0,0,&pose)==MMS_E_NOT_FOUND&&Unchanged(pose));}
    auto foreignInfo=Poison<MmsEvaluationFrameInfoV1>();CHECK(other.describeFrame(other.context,frame,&foreignInfo)==MMS_E_NOT_FOUND&&Unchanged(foreignInfo));
    frameQuery=FrameQuery(source);CHECK(other.evaluateFrame(other.context,&frameQuery,&out)==MMS_E_NOT_FOUND&&out==sentinel);
    auto model=Poison<MmsEvaluationSceneModelV1>();auto bone=Poison<MmsEvaluationSceneBoneV1>();auto pose=Poison<MmsEvaluationScenePoseV1>();const auto samples=control->samples.load();
    CHECK(api.sceneModelAt(api.context,source,2,&model)==MMS_E_NOT_FOUND&&Unchanged(model));
    CHECK(api.sceneBoneAt(api.context,source,0,2,&bone)==MMS_E_NOT_FOUND&&Unchanged(bone));
    CHECK(api.readBonePose(api.context,frame,2,0,&pose)==MMS_E_NOT_FOUND&&Unchanged(pose));
    CHECK(api.readBonePose(api.context,frame,1,1,&pose)==MMS_E_NOT_FOUND&&Unchanged(pose)&&control->samples==samples);
    CHECK(api.describeSceneSource(api.context,source,nullptr)==MMS_E_ARGUMENT&&api.describeFrame(api.context,frame,nullptr)==MMS_E_ARGUMENT);
    CHECK(api.sceneModelAt(api.context,source,0,nullptr)==MMS_E_ARGUMENT&&api.sceneBoneAt(api.context,source,0,0,nullptr)==MMS_E_ARGUMENT);
    CHECK(api.frameModelAt(api.context,frame,0,nullptr)==MMS_E_ARGUMENT&&api.readBonePose(api.context,frame,0,0,nullptr)==MMS_E_ARGUMENT);
    MmsResult workerCreate=MMS_OK,workerEvaluate=MMS_E_FAILED;MmsHandle workerSource=sentinel,workerFrame=sentinel;query=SceneQuery();frameQuery=FrameQuery(source);
    std::thread worker([&]{workerCreate=api.createSceneSource(api.context,&query,&workerSource);workerEvaluate=api.evaluateFrame(api.context,&frameQuery,&workerFrame);});worker.join();
    CHECK(workerCreate==MMS_E_THREAD&&workerSource==sentinel&&workerEvaluate==MMS_OK&&workerFrame!=sentinel);
    CHECK(api.release(api.context,workerFrame)==MMS_OK&&api.release(api.context,frame)==MMS_OK&&api.release(api.context,source)==MMS_OK&&api.release(api.context,channel)==MMS_OK);
    CHECK(!store->HasLeases(client)&&!store->HasLeases(foreign)&&client.bytes==0&&client.workspaceBytes==0);
}
static void BadHostPayloads(){
    section="malformed host payloads fail without handle or byte publication";
    auto control=std::make_shared<Control>();auto store=Store(control);EvaluationStore::Client client;const auto api=Bind(*store,client);const auto query=SceneQuery();
    for(int mode=1;mode<=13;++mode){control->captureMode=mode;MmsHandle out=sentinel;
        const auto expected=mode==10?MMS_E_MEMORY:mode==12?MMS_E_CANCELLED:(mode>=4&&mode<=9)||mode==13?MMS_E_ARGUMENT:MMS_E_FAILED;
        CHECK(api.createSceneSource(api.context,&query,&out)==expected&&out==sentinel);
        CHECK(!store->HasLeases(client)&&client.handles==0&&client.active==0&&client.bytes==0&&control->models.expired());}
    control->captureMode=0;control->sourceBytes=0;MmsHandle out=sentinel;CHECK(api.createSceneSource(api.context,&query,&out)==MMS_E_FAILED&&out==sentinel&&client.bytes==0);
    control->sourceBytes=4096;const auto source=Capture(api);const auto bytes=client.bytes;const auto frameQuery=FrameQuery(source);
    for(int mode=1;mode<=9;++mode){control->frameMode=mode;out=sentinel;const auto expected=mode==7?MMS_E_MEMORY:mode==9?MMS_E_CANCELLED:MMS_E_FAILED;
        CHECK(api.evaluateFrame(api.context,&frameQuery,&out)==expected&&out==sentinel);
        CHECK(client.handles==1&&client.active==0&&client.bytes==bytes&&client.workspaceBytes==0);}
    control->frameMode=0;control->frameBytes=0;out=sentinel;CHECK(api.evaluateFrame(api.context,&frameQuery,&out)==MMS_E_FAILED&&out==sentinel&&client.bytes==bytes&&client.workspaceBytes==0);
    control->frameBytes=8192;const auto frame=Evaluate(api,source);const auto charged=client.bytes;
    for(int mode=1;mode<=9;++mode){control->poseMode=mode;auto pose=Poison<MmsEvaluationScenePoseV1>();const auto expected=mode==7?MMS_E_MEMORY:mode==9?MMS_E_CANCELLED:MMS_E_FAILED;
        CHECK(api.readBonePose(api.context,frame,0,0,&pose)==expected&&Unchanged(pose));CHECK(client.bytes==charged&&client.active==0);}
    control->poseMode=0;MmsEvaluationScenePoseV1 pose{};CHECK(api.readBonePose(api.context,frame,0,0,&pose)==MMS_OK&&PoseMatches(pose,7.5,3,0,0));
    CHECK(api.release(api.context,source)==MMS_OK&&api.release(api.context,frame)==MMS_OK&&!store->HasLeases(client));
}
static void SharedBudgetsAndFaults(){
    section="common channel/source/frame budget and aliases without double charging";
    auto control=std::make_shared<Control>();auto store=Store(control);EvaluationStore::Client client;const auto api=Bind(*store,client);
    const auto source=Capture(api);const auto sourceInfo=DescribeSource(api,source);const auto frame=Evaluate(api,source);const auto frameInfo=DescribeFrame(api,frame);
    const MmsEvaluationSourceQueryV1 cq{Header<MmsEvaluationSourceQueryV1>(),object,0,0,0};MmsHandle channel=0;CHECK(api.createSource(api.context,&cq,&channel)==MMS_OK);MmsEvaluationSourceInfoV1 ci{};
    CHECK(api.describeSource(api.context,channel,&ci)==MMS_OK&&client.bytes==sourceInfo.retainedBytes+frameInfo.retainedBytes+ci.retainedBytes);const auto charged=client.bytes;
    std::array<MmsHandle,3> aliases{};const std::array<MmsHandle,3> originals{channel,source,frame};
    for(unsigned i=0;i<3;++i)CHECK(api.retain(api.context,originals[i],&aliases[i])==MMS_OK&&aliases[i]!=originals[i]&&client.bytes==charged);
    CHECK(DescribeSource(api,aliases[1]).captureId==sourceInfo.captureId&&DescribeFrame(api,aliases[2]).frameId==frameInfo.frameId);
    for(auto alias:aliases)CHECK(api.release(api.context,alias)==MMS_OK);
    MmsHandle rejected=sentinel;auto sq=SceneQuery(sourceInfo.retainedBytes-1);CHECK(api.createSceneSource(api.context,&sq,&rejected)==MMS_E_MEMORY&&rejected==sentinel);
    const auto exactSource=Capture(api,sourceInfo.retainedBytes);CHECK(DescribeSource(api,exactSource).retainedBytes==sourceInfo.retainedBytes&&api.release(api.context,exactSource)==MMS_OK);
    auto fq=FrameQuery(source,{1,4},frameInfo.retainedBytes-1);CHECK(api.evaluateFrame(api.context,&fq,&rejected)==MMS_E_MEMORY&&rejected==sentinel);
    const auto exactFrame=Evaluate(api,source,{1,4},frameInfo.retainedBytes);CHECK(DescribeFrame(api,exactFrame).retainedBytes==frameInfo.retainedBytes&&api.release(api.context,exactFrame)==MMS_OK);
    control->sourceBytes=64*MiB+1;sq=SceneQuery();CHECK(api.createSceneSource(api.context,&sq,&rejected)==MMS_E_MEMORY&&rejected==sentinel);control->sourceBytes=4096;
    control->frameBytes=64*MiB+1;fq=FrameQuery(source);CHECK(api.evaluateFrame(api.context,&fq,&rejected)==MMS_E_MEMORY&&rejected==sentinel);control->frameBytes=8192;
    CHECK(client.bytes==charged&&client.active==0&&client.workspaceBytes==0);
    std::vector<MmsHandle> handles;
    while(client.handles<128){MmsHandle alias=0;CHECK(api.retain(api.context,frame,&alias)==MMS_OK);handles.push_back(alias);}
    CHECK(api.retain(api.context,source,&rejected)==MMS_E_BUSY&&rejected==sentinel&&client.bytes==charged);
    for(auto handle:handles)CHECK(api.release(api.context,handle)==MMS_OK);
    section="allocation failures cannot half-publish a source or frame";
    for(unsigned operation=0;operation<2;++operation){bool done=false;unsigned failed=0;
        for(int allowance=0;allowance<256&&!done;++allowance){MmsHandle out=sentinel;const auto sourceQuery=SceneQuery();const auto frameQuery=FrameQuery(source);
            failAfter=allowance;const auto status=operation?api.evaluateFrame(api.context,&frameQuery,&out):api.createSceneSource(api.context,&sourceQuery,&out);failAfter=-1;
            if(status==MMS_E_MEMORY){++failed;CHECK(out==sentinel);}else{CHECK(status==MMS_OK&&out!=sentinel);CHECK(api.release(api.context,out)==MMS_OK);done=true;}
            CHECK(client.bytes==charged&&client.handles==3&&client.active==0&&client.workspaceBytes==0);
            MmsEvaluationScenePoseV1 pose{};CHECK(api.readBonePose(api.context,frame,0,0,&pose)==MMS_OK&&PoseMatches(pose,7.5,3,0,0));
        }CHECK(done&&failed>0);
    }
    failAfter=0;MmsHandle alias=sentinel;const auto failedRetain=api.retain(api.context,frame,&alias);MmsEvaluationScenePoseV1 pose{};const auto sampled=api.readBonePose(api.context,frame,0,1,&pose);failAfter=-1;
    CHECK(failedRetain==MMS_E_MEMORY&&alias==sentinel&&sampled==MMS_OK&&PoseMatches(pose,7.5,3,0,1)&&client.bytes==charged);
    for(auto handle:originals)CHECK(api.release(api.context,handle)==MMS_OK);CHECK(client.bytes==0&&!store->HasLeases(client));
    section="client and global byte ceilings recover after payload release";
    control->sourceBytes=40*MiB;std::array<EvaluationStore::Client,3> clients;std::array<Api,3> apis{};std::array<std::vector<MmsHandle>,3> leases;
    unsigned total=0;
    for(unsigned c=0;c<3;++c){apis[c]=Bind(*store,clients[c]);for(unsigned n=0;n<8;++n){MmsHandle handle=sentinel;const auto q=SceneQuery();const auto before=clients[c].bytes;const auto status=apis[c].createSceneSource(apis[c].context,&q,&handle);
        if(status==MMS_E_MEMORY){CHECK(handle==sentinel&&clients[c].bytes==before);break;}CHECK(status==MMS_OK);leases[c].push_back(handle);++total;
    }}
    CHECK(total==6&&leases[0].size()==3&&leases[1].size()==3&&leases[2].empty());
    for(auto handle:leases[0])CHECK(apis[0].release(apis[0].context,handle)==MMS_OK);leases[0].clear();
    const auto restored=Capture(apis[2]);CHECK(apis[2].release(apis[2].context,restored)==MMS_OK);
    for(unsigned c=0;c<3;++c){for(auto handle:leases[c])CHECK(apis[c].release(apis[c].context,handle)==MMS_OK);CHECK(clients[c].bytes==0&&!store->HasLeases(clients[c]));}
}
struct Workers{
    std::shared_ptr<Gate> gate;std::array<std::thread,4> threads;
    ~Workers(){SetEvent(gate->proceed);for(auto& thread:threads)if(thread.joinable())thread.join();}
};
static void SourceAndFrameInFlight(){
    section="last source release during evaluate and retirement before publication";
    for(bool retire:{false,true}){auto control=std::make_shared<Control>();control->evaluateGate=std::make_shared<Gate>();const auto gate=control->evaluateGate;CHECK(gate->ready&&gate->proceed);
        auto store=Store(control);EvaluationStore::Client client;const auto api=Bind(*store,client);const auto source=Capture(api);const auto charged=client.bytes;const auto models=control->models;
        MmsResult status=MMS_E_FAILED;MmsHandle frame=sentinel;const auto query=FrameQuery(source);Workers workers{gate,{}};
        workers.threads[0]=std::thread([&]{status=api.evaluateFrame(api.context,&query,&frame);});CHECK(WaitForSingleObject(gate->ready,10000)==WAIT_OBJECT_0);
        CHECK(api.release(api.context,source)==MMS_OK&&client.handles==0&&client.bytes==charged&&client.workspaceBytes==128*MiB&&!models.expired()&&store->HasLeases(client));
        if(retire){store->Retire(client);MmsHandle denied=sentinel;CHECK(api.evaluateFrame(api.context,&query,&denied)==MMS_E_NOT_READY&&denied==sentinel);}
        SetEvent(gate->proceed);workers.threads[0].join();CHECK(client.active==0&&client.workspaceBytes==0);
        if(retire)CHECK(status==MMS_E_NOT_READY&&frame==sentinel&&client.bytes==0&&!store->HasLeases(client)&&models.expired());
        else{CHECK(status==MMS_OK&&frame!=sentinel&&client.handles==1&&!models.expired());MmsEvaluationScenePoseV1 pose{};
            CHECK(api.readBonePose(api.context,frame,1,0,&pose)==MMS_OK&&PoseMatches(pose,7.5,3,1,0));CHECK(api.release(api.context,frame)==MMS_OK&&models.expired()&&!store->HasLeases(client));}
    }
    section="admitted pose read survives frame release and retirement";
    auto control=std::make_shared<Control>();control->poseGate=std::make_shared<Gate>();const auto gate=control->poseGate;CHECK(gate->ready&&gate->proceed);
    auto store=Store(control);EvaluationStore::Client client;const auto api=Bind(*store,client);const auto source=Capture(api),frame=Evaluate(api,source);const auto frameBytes=DescribeFrame(api,frame).retainedBytes;
    CHECK(api.release(api.context,source)==MMS_OK);MmsEvaluationScenePoseV1 pose{};MmsResult status=MMS_E_FAILED;Workers workers{gate,{}};
    workers.threads[0]=std::thread([&]{status=api.readBonePose(api.context,frame,0,0,&pose);});CHECK(WaitForSingleObject(gate->ready,10000)==WAIT_OBJECT_0);
    store->Retire(client);CHECK(api.release(api.context,frame)==MMS_OK&&client.handles==0&&client.active==1&&client.bytes==frameBytes&&store->HasLeases(client));
    auto denied=Poison<MmsEvaluationScenePoseV1>();CHECK(api.readBonePose(api.context,frame,0,0,&denied)==MMS_E_NOT_READY&&Unchanged(denied));
    SetEvent(gate->proceed);workers.threads[0].join();CHECK(status==MMS_OK&&PoseMatches(pose,7.5,3,0,0)&&client.active==0&&client.bytes==0&&!store->HasLeases(client));
}
static void ParallelWorkspaceBudgets(){
    section="parallel workspace admission obeys client and global ceilings";
    auto control=std::make_shared<Control>();control->evaluateGate=std::make_shared<Gate>(4);const auto gate=control->evaluateGate;CHECK(gate->ready&&gate->proceed);
    auto store=Store(control);std::array<EvaluationStore::Client,3> clients;std::array<Api,3> apis{};std::array<MmsHandle,3> sources{};
    for(unsigned c=0;c<3;++c){apis[c]=Bind(*store,clients[c]);sources[c]=Capture(apis[c]);}
    std::array<MmsHandle,4> frames{sentinel,sentinel,sentinel,sentinel};std::array<MmsResult,4> statuses{};Workers workers{gate,{}};
    for(unsigned n=0;n<4;++n)workers.threads[n]=std::thread([&,n]{const auto c=n/2;const auto q=FrameQuery(sources[c]);statuses[n]=apis[c].evaluateFrame(apis[c].context,&q,&frames[n]);});
    CHECK(WaitForSingleObject(gate->ready,10000)==WAIT_OBJECT_0);CHECK(clients[0].workspaceBytes==256*MiB&&clients[1].workspaceBytes==256*MiB&&clients[2].workspaceBytes==0);
    const auto entered=control->evaluations.load();
    for(unsigned c:{0u,2u}){const auto q=FrameQuery(sources[c],{1,4},0,MiB);MmsHandle out=sentinel;
        CHECK(apis[c].evaluateFrame(apis[c].context,&q,&out)==MMS_E_MEMORY&&out==sentinel&&control->evaluations==entered);}
    SetEvent(gate->proceed);for(auto& thread:workers.threads)thread.join();
    for(unsigned n=0;n<4;++n){CHECK(statuses[n]==MMS_OK&&frames[n]!=sentinel);const auto c=n/2;CHECK(apis[c].release(apis[c].context,frames[n])==MMS_OK);}
    for(unsigned c=0;c<3;++c){CHECK(clients[c].workspaceBytes==0&&clients[c].active==0);const auto frame=Evaluate(apis[c],sources[c],{3,4},0,MiB);
        MmsEvaluationScenePoseV1 pose{};CHECK(apis[c].readBonePose(apis[c].context,frame,0,0,&pose)==MMS_OK&&PoseMatches(pose,22.5,3,0,0));
        CHECK(apis[c].release(apis[c].context,frame)==MMS_OK&&apis[c].release(apis[c].context,sources[c])==MMS_OK&&!store->HasLeases(clients[c]));}
}
static void FailedLegacyCaptureKeepsLeaseDuringDestruction(){
    section="failed old-channel candidate destruction retains admission without holding store mutex";
    for(bool invalidIdentity:{false,true}){
        auto gate=std::make_shared<Gate>();CHECK(gate->ready&&gate->proceed);
        std::atomic<bool> destructorTimedOut{false},observerDone{false},observedLease{false},controllerTimedOut{false};
        struct Signal{HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);~Signal(){if(value)CloseHandle(value);}} observed;CHECK(observed.value);
        struct CandidateGuard{
            std::shared_ptr<Gate> gate;std::atomic<bool>* timedOut;
            CandidateGuard(std::shared_ptr<Gate> g,std::atomic<bool>* t):gate(std::move(g)),timedOut(t){}
            ~CandidateGuard(){if(!gate->Wait())*timedOut=true;}
        };
        EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query,uint32_t mask,uint64_t,auto& out)->MmsResult{
            auto data=std::make_shared<EvaluationData>();data->object=query.object;data->channels=mask;data->retainedBytes=4096;
            data->bones={{Header<MmsEvaluationBoneV1>(),invalidIdentity?MmsId{}:MmsId{33,44},MMS_EVALUATION_BONE_LOCAL,0}};
            auto guard=std::make_shared<CandidateGuard>(gate,&destructorTimedOut);
            data->sample=[guard](uint32_t,double,MmsBoneChannelsV1&)->MmsResult{return MMS_E_NOT_FOUND;};
            out=data;return invalidIdentity?MMS_OK:MMS_E_FAILED;
        }),1);
        EvaluationStore::Client client;const auto api=Bind(store,client);Workers workers{gate,{}};
        workers.threads[0]=std::thread([&]{
            if(WaitForSingleObject(gate->ready,10000)==WAIT_OBJECT_0){observedLease=store.HasLeases(client);observerDone=true;}
            SetEvent(observed.value);
        });
        workers.threads[1]=std::thread([&]{
            if(WaitForSingleObject(gate->ready,10000)!=WAIT_OBJECT_0||WaitForSingleObject(observed.value,5000)!=WAIT_OBJECT_0)controllerTimedOut=true;
            // Also release on timeout: a regression that destroys under the
            // mutex must fail explicitly rather than deadlock the test process.
            SetEvent(gate->proceed);
        });
        const MmsEvaluationSourceQueryV1 query{Header<MmsEvaluationSourceQueryV1>(),object,0,0,0};MmsHandle out=sentinel;
        // Capture remains on the store's creating/UI thread throughout.
        const auto result=api.createSource(api.context,&query,&out);
        workers.threads[0].join();workers.threads[1].join();
        CHECK(!controllerTimedOut&&!destructorTimedOut&&observerDone&&observedLease);
        CHECK(result==(invalidIdentity?MMS_E_ARGUMENT:MMS_E_FAILED)&&out==sentinel);
        CHECK(client.active==0&&client.handles==0&&client.bytes==0&&client.workspaceBytes==0&&!store.HasLeases(client));
    }
}
int main(){try{TablesAndCapabilities();EnumerationTimeAndLifetime();AdmissionAndKinds();BadHostPayloads();SharedBudgetsAndFaults();SourceAndFrameInFlight();ParallelWorkspaceBudgets();FailedLegacyCaptureKeepsLeaseDuringDestruction();
    std::printf("PASS %u mock evaluation scene checks\n",checks);return 0;
}catch(const std::exception& error){failAfter=-1;std::fprintf(stderr,"FAIL %s unexpected exception: %s\n",section,error.what());return 1;}}
