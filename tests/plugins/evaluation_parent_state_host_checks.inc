#include "mikumikustudio/bone_key_pool.hpp"
#if !MMD_ORIGINAL_PROFILE
// Include after evaluation_display_host_checks.inc and bone_evaluation_host_checks.inc.
namespace evaluation_parent_state_probe {
namespace {
using namespace mikumikustudio::plugins;
struct Identities {MmsObjectRef source{},target{};MmsId globalTarget{},localTarget{};};
Identities Publish(MMDApp* app) {
    auto scene=CreateSceneStore(app);SceneClient client;auto api=([&]{ MmsSceneApiV1 value{}; if (scene->Api(client, value) != MMS_OK) throw std::runtime_error("scene API binding failed"); return value; }());
    MmsSceneQueryV1 q{};q.h=Header<MmsSceneQueryV1>();q.stage=MMS_STAGE_BIND;q.flags=MMS_SCENE_CURRENT_TIME;q.channels=MMS_CHANNEL_BONES;
    MmsSnapshotV1 bind{};Check(api.acquire(api.context,&q,&bind)==MMS_OK,"publish real parent-state model identities");
    MmsSpan objects{},bones{};Check(api.objects(api.context,bind.handle,&objects)==MMS_OK&&objects.count==2,"source and target BIND objects exist");
    const auto* rows=static_cast<const MmsObjectV1*>(objects.data);Identities out;out.source=rows[0].object;out.target=rows[1].object;
    Check(api.readChannel(api.context,bind.handle,out.target,MMS_CHANNEL_BONES,&bones)==MMS_OK&&bones.count>1,"distinct target bone identities exist");
    out.globalTarget=static_cast<const MmsBoneV1*>(bones.data)[0].id;out.localTarget=static_cast<const MmsBoneV1*>(bones.data)[1].id;
    api.release(api.context,bind.handle);return out;
}
void Seed(mdl::ModelRecord& m,bool source,unsigned local) {
    Check(mdl::DisplayKeyPoolEnsure(m,4),"reserve four actual parent-state display records");
    for(unsigned k=0;k<4;++k) {auto& key=m.displayKeys[k];Check(key.selectorStates&&(!m.ikChainCount||key.ikStates),"display record owns nested selector and IK payloads");
        key.frame=k*4;key.previous=k?k-1:0;key.next=k<3?k+1:0;key.allocated=0;key.visible=1;
        for(uint32_t i=0;i<m.ikChainCount;++i)key.ikStates[i]=1;
        for(uint32_t i=0;i<m.boneOrderCount;++i)key.selectorStates[i]={-1,0};
        if(source) for(const auto selector:{0u,local}) {
            key.selectorStates[selector]=k==1?mdl::BoneReference{-2,int32_t(selector+77)}:
                k==2?mdl::BoneReference{1,selector?1:0}:mdl::BoneReference{-1,int32_t(selector+k+17)};
        }
    }
    m.maxFrame=(std::max)(m.maxFrame,12u);
}
struct Proof {
    std::array<std::unique_ptr<bone_evaluation_probe::Proof>,2> models;
    std::array<std::vector<unsigned char>,2> payloads;
    MMDApp* app;
    static std::vector<unsigned char> Bytes(const mdl::ModelRecord& m) {
        std::vector<unsigned char> bytes;
        for(unsigned k=0;k<4;++k){const auto& key=m.displayKeys[k];const auto* p=reinterpret_cast<const unsigned char*>(key.selectorStates);
            bytes.insert(bytes.end(),p,p+m.boneOrderCount*sizeof(mdl::BoneReference));if(m.ikChainCount)bytes.insert(bytes.end(),key.ikStates,key.ikStates+m.ikChainCount);}
        return bytes;
    }
    explicit Proof(MMDApp* owner):app(owner){for(unsigned i=0;i<2;++i){models[i]=std::make_unique<bone_evaluation_probe::Proof>(app,app->ModelSlot(i));payloads[i]=Bytes(*mdl::Mdl(app->ModelSlot(i)));}}
    void Verify()const{for(unsigned i=0;i<2;++i){models[i]->Verify();Check(payloads[i]==Bytes(*mdl::Mdl(app->ModelSlot(i))),"parent-state sampling preserves all four deep display payloads");}}
};
bool Read(const MmsEvaluationApiV1_3& api,const MmsEvaluationApiV1_2& old,MmsHandle source,const Identities& ids,unsigned local) {
    struct Case {MmsTime time;uint32_t kind,start,end;};
    const Case cases[]={
        {{0,1},MMS_EVALUATION_PARENT_INHERIT,0,4},
        {{36028797018963968LL,270215977642229761ULL},MMS_EVALUATION_PARENT_INHERIT,0,4},
        {{4,30},MMS_EVALUATION_PARENT_GROUND,4,8},
        {{36028797018963968LL,270215977642229759ULL},MMS_EVALUATION_PARENT_GROUND,4,8},
        {{3,20},MMS_EVALUATION_PARENT_GROUND,4,8},
        {{72057594037927936LL,270215977642229761ULL},MMS_EVALUATION_PARENT_GROUND,4,8},
        {{8,30},MMS_EVALUATION_PARENT_MODEL,8,12},
        {{17,60},MMS_EVALUATION_PARENT_MODEL,8,12},
        {{108086391056891904LL,270215977642229761ULL},MMS_EVALUATION_PARENT_MODEL,8,12},
        {{12,30},MMS_EVALUATION_PARENT_INHERIT,12,0},
        {{5,12},MMS_EVALUATION_PARENT_INHERIT,12,0}};
    for(const auto selector:{0u,local})for(const auto& c:cases) {
        MmsParentStateV1 state{};
        if(api.sampleParentState(api.context,source,selector,c.time,&state)!=MMS_OK||state.kind!=c.kind||
            state.windowStart!=c.start||state.windowEnd!=c.end||state.reserved)return false;
        if(c.kind==MMS_EVALUATION_PARENT_MODEL){if(!Same(state.target,ids.target)||!Same(state.bone,selector?ids.localTarget:ids.globalTarget))return false;}
        else if(!Empty(state.target.id)||state.target.generation||!Empty(state.bone))return false;
        MmsParentSelectionV1 legacy;std::memset(&legacy,0xA5,sizeof legacy);const auto untouched=legacy;
        const auto status=old.sampleParent(old.context,source,selector,c.time,&legacy);
        if(c.kind==MMS_EVALUATION_PARENT_GROUND){if(status!=MMS_E_UNSUPPORTED||std::memcmp(&legacy,&untouched,sizeof legacy))return false;}
        else {
            if(status!=MMS_OK||legacy.windowStart!=c.start||legacy.windowEnd!=c.end||legacy.reserved)return false;
            if(c.kind==MMS_EVALUATION_PARENT_MODEL){if(legacy.flags!=MMS_EVALUATION_PARENT_ACTIVE||!Same(legacy.target,ids.target)||!Same(legacy.bone,state.bone))return false;}
            else if(legacy.flags||!Empty(legacy.target.id)||legacy.target.generation||!Empty(legacy.bone))return false;
        }
    }
    // Rejected new/old requests must not overwrite even a caller's nonzero payload.
    for(const auto index:{0u,UINT32_MAX}) {
        const auto time=index?MmsTime{0,1}:MmsTime{-1,30};
        MmsParentStateV1 value;std::memset(&value,0x3C,sizeof value);const auto before=value;
        MmsParentSelectionV1 legacy;std::memset(&legacy,0xC3,sizeof legacy);const auto oldBefore=legacy;
        if(api.sampleParentState(api.context,source,index,time,&value)==MMS_OK||std::memcmp(&value,&before,sizeof value)||
            old.sampleParent(old.context,source,index,time,&legacy)==MMS_OK||std::memcmp(&legacy,&oldBefore,sizeof legacy))return false;
    }
    return true;
}
}
void Run(MMDApp* app) {
    using namespace mikumikustudio::plugins;
    using Getter13=MmsResult(MMS_CALL*)(MmsEvaluationApiV1_3*);using Getter12=MmsResult(MMS_CALL*)(MmsEvaluationApiV1_2*);
    const auto module=GetModuleHandleW(L"mms_evaluation_host_probe.dll");
    const auto get13=reinterpret_cast<Getter13>(GetProcAddress(module,"MmsEvaluationHostGetApi13"));
    const auto get12=reinterpret_cast<Getter12>(GetProcAddress(module,"MmsEvaluationHostGetApi12"));
    MmsEvaluationApiV1_3 api{};MmsEvaluationApiV1_2 oldApi{};
    Check(get13&&get12&&get13(&api)==MMS_OK&&get12(&oldApi)==MMS_OK&&api.sampleParentState,"real DLL negotiates both evaluate 1.3 and 1.2");
    wchar_t path[32768]{};const auto length=GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL",path,32768);Check(length&&length<32768,"parent-state fixture supplied");
    unsigned local=0;
    const auto load=[&]{LoadModelFile(app,path,false);LoadModelFile(app,path,false);Check(app->ModelSlot(0)&&app->ModelSlot(1),"load real source and external target");
        auto& source=*mdl::Mdl(app->ModelSlot(0));Check(source.boneOrderCount>1&&source.ikChainCount,"real fixture has local selector and IK chain");
        local=unsigned(source.boneTable[0].slotIndex);Check(local>0&&local<source.boneOrderCount,"real Move bone owns local selector");
        Seed(source,true,local);Seed(*mdl::Mdl(app->ModelSlot(1)),false,local);};load();
    const auto ids=Publish(app);MmsEvaluationSourceQueryV1 query{};query.h=Header<MmsEvaluationSourceQueryV1>();query.object=ids.source;
    auto& model=*mdl::Mdl(app->ModelSlot(0));
    // Unsaved current selectors deliberately disagree with the registered keys.
    auto* live=static_cast<mdl::BoneOrderEntry*>(model.boneOrderTable);
    live[0].linkedModel=99;live[0].linkedBone=999;live[local].linkedModel=-1;live[local].windowStart=100;
    model.ikChains[0].enabled=0;model.boneTable[0].trans[0]=123;app->CurrentFrame()=0;app->SceneModified()=0;
    MmsHandle display=0,combined=0;
    {const Proof proof(app);
        Check(api.createSourceChannels(api.context,&query,4,&display)==MMS_OK&&api.createSourceChannels(api.context,&query,7,&combined)==MMS_OK,
            "future ground references capture successfully without target identities");
        Check(Read(api,oldApi,display,ids,local)&&Read(api,oldApi,combined,ids,local),"new states and old contract preserve exact registered windows and identities");
        MmsHandle retained=0;MmsEvaluationSourceInfoV1_2 originalInfo{},retainedInfo{};
        Check(oldApi.retain(oldApi.context,display,&retained)==MMS_OK &&
            api.describeSourceDisplay(api.context,display,&originalInfo)==MMS_OK &&
            oldApi.describeSourceDisplay(oldApi.context,retained,&retainedInfo)==MMS_OK &&
            originalInfo.captureId==retainedInfo.captureId&&originalInfo.retainedBytes==retainedInfo.retainedBytes&&
            Read(api,oldApi,retained,ids,local),"old retain and new sampling share one source identity and retained data accounting");
        Check(api.release(api.context,retained)==MMS_OK,"release handle retained through ABI 1.2 using ABI 1.3");proof.Verify();}
    auto& future=model.displayKeys[3].selectorStates[0];const auto saved=future;
    for(const auto invalid:{mdl::BoneReference{-3,0},mdl::BoneReference{99,0},mdl::BoneReference{1,100000}}) {
        future=invalid;const Proof proof(app);MmsHandle out=0x123456;
        Check(api.createSourceChannels(api.context,&query,4,&out)==MMS_E_STALE&&out==0x123456,"future invalid sentinel or model target rejects source without publishing a handle");proof.Verify();
    }future=saved;
    const auto workers=[&]{bool a=true,b=true;const auto read=[&](bool& ok){for(unsigned i=0;i<24;++i)ok=Read(api,oldApi,display,ids,local)&&Read(api,oldApi,combined,ids,local)&&ok;};
        std::thread first([&]{read(a);}),second;try{second=std::thread([&]{read(b);});}catch(...){first.join();throw;}first.join();second.join();Check(a&&b,"concurrent old sources retain ground states and frozen target identity");};
    {const Proof proof(app);workers();proof.Verify();}
    for(unsigned slot=0;slot<2;++slot)SeekModelFrame(app->ModelSlot(slot),0,0);app->CurrentFrame()=0;
    const auto project=std::filesystem::absolute(L"evaluation-parent-states.pmm");Check(SaveSceneCopy(app,project.c_str()),"save real PMM with inherit ground and model parent keys");
    ResetAppState(app);workers();load();const auto replacement=Publish(app);Check(!Same(replacement.target,ids.target),"slot reuse publishes a different actual target identity");
    query.object=replacement.source;MmsHandle fresh=0;
    {const Proof proof(app);Check(api.createSourceChannels(api.context,&query,4,&fresh)==MMS_OK&&Read(api,oldApi,fresh,replacement,local),"new capture binds replacement identities while old ground samples stay frozen");workers();proof.Verify();}
    Check(api.release(api.context,fresh)==MMS_OK,"release replacement source");ResetAppState(app);
    wcscpy_s(app->state.envFileName,project.c_str());LoadSceneFile();Check(app->ModelSlot(0)&&app->ModelSlot(1),"reload actual parent-state PMM");const auto reloaded=Publish(app);query.object=reloaded.source;
    {const Proof proof(app);Check(api.createSourceChannels(api.context,&query,4,&fresh)==MMS_OK&&Read(api,oldApi,fresh,reloaded,local),"reloaded registered parent kinds and windows match saved PMM");workers();proof.Verify();}
    Check(api.release(api.context,fresh)==MMS_OK&&api.release(api.context,display)==MMS_OK&&api.release(api.context,combined)==MMS_OK,"release all parent sources across ABI versions");ResetAppState(app);
    std::puts("Production evaluate 1.3: real inherit/ground/model keys, exact windows, legacy errors and frozen identity lifetime passed");
}
}
#endif
