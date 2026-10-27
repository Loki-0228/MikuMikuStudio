#if !MMD_ORIGINAL_PROFILE
// Included after the internal scene oracle and evaluation plugin helpers.
namespace scene_evaluation_abi_probe {
using namespace mikumikustudio::plugins;
using namespace bone_scene_evaluation_probe;
struct PublicModel {
    MmsObjectRef object{};
    std::vector<MmsId> bones;
};
using PublicModels=std::vector<PublicModel>;
bool SameFloat(const float* a,const float* b,size_t count) {
    for(size_t i=0;i<count;++i) if(!std::isfinite(a[i])||!std::isfinite(b[i])||std::abs(a[i]-b[i])>5e-5f*(1.f+std::abs(b[i])))return false;
    return true;
}
PublicModels Identities(MMDApp* app,const MmsEvaluationApiV1_4& api,MmsHandle source) {
    auto store=CreateSceneStore(app);SceneClient client;auto scene=store->Api(client);
    MmsSceneQueryV1 query{};query.h=Header<MmsSceneQueryV1>();query.stage=MMS_STAGE_BIND;query.flags=MMS_SCENE_CURRENT_TIME;query.channels=MMS_CHANNEL_BONES;
    MmsSnapshotV1 bind{};MmsSpan objects{};Check(scene.acquire(scene.context,&query,&bind)==MMS_OK&&scene.objects(scene.context,bind.handle,&objects)==MMS_OK&&objects.count==3,"BIND snapshot publishes three actual models");
    const auto* rows=static_cast<const MmsObjectV1*>(objects.data);PublicModels result;
    for(uint32_t slot=0;slot<3;++slot) {
        MmsEvaluationSceneModelV1 model{};MmsSpan bones{};
        Check(api.sceneModelAt(api.context,source,slot,&model)==MMS_OK&&Same(model.object,rows[slot].object)&&model.flags==0&&model.reserved==0,"scene source model identity matches BIND");
        Check(scene.readChannel(scene.context,bind.handle,model.object,MMS_CHANNEL_BONES,&bones)==MMS_OK&&bones.count==model.boneCount,"scene source bone count matches BIND");
        PublicModel item;item.object=model.object;const auto* values=static_cast<const MmsBoneV1*>(bones.data);
        for(uint32_t b=0;b<model.boneCount;++b){MmsEvaluationSceneBoneV1 bone{};
            Check(api.sceneBoneAt(api.context,source,slot,b,&bone)==MMS_OK&&bone.index==b&&Same(bone.bone,values[b].id),"scene bone identity matches BIND index");item.bones.push_back(bone.bone);}
        result.push_back(std::move(item));
    }
    scene.release(scene.context,bind.handle);return result;
}
bool FrameMatches(const MmsEvaluationApiV1_4& api,MmsHandle frame,const PublicModels& models,const ScenePoses& expected,MmsTime time={1,3},uint32_t targetVisibility=197) {
    MmsEvaluationFrameInfoV1 info{};
    if(api.describeFrame(api.context,frame,&info)!=MMS_OK||info.modelCount!=3||info.time.numerator!=time.numerator||info.time.denominator!=time.denominator)return false;
    for(uint32_t slot=0;slot<3;++slot){
        MmsEvaluationSceneModelV1 model{};
        const bool beforeKey=time.numerator==36028797018963968LL&&time.denominator==270215977642229761ULL;
        const uint32_t expectedVisibility=slot==1&&!beforeKey?targetVisibility:1u;
        if(api.frameModelAt(api.context,frame,slot,&model)!=MMS_OK||!Same(model.object,models[slot].object)||model.boneCount!=models[slot].bones.size()||
            model.flags!=MMS_EVALUATION_SCENE_VISIBILITY_VALID||model.visibility!=expectedVisibility)return false;
        unsigned row=0;while(kSlots[row]!=slot)++row;
        for(uint32_t b=0;b<model.boneCount;++b){MmsEvaluationScenePoseV1 pose{};const auto& want=expected[row][b];
            if(api.readBonePose(api.context,frame,slot,b,&pose)!=MMS_OK||!Same(pose.bone,models[slot].bones[b])||pose.reserved||
                !SameFloat(pose.rigid,want.rigid.data(),16)||!SameFloat(pose.scaled,want.scaled.data(),16)||
                !SameFloat(pose.attachmentBefore,want.attachment[0].data(),16)||!SameFloat(pose.attachmentAfter,want.attachment[1].data(),16)||
                !SameFloat(pose.jointPosition,want.jointPosition.data(),3))return false;
        }
    }return true;
}
void Run(MMDApp* app) {
    using Getter=MmsResult(MMS_CALL*)(MmsEvaluationApiV1_4*);
    const auto module=GetModuleHandleW(L"mms_evaluation_host_probe.dll");const auto getter=reinterpret_cast<Getter>(GetProcAddress(module,"MmsEvaluationHostGetApi14"));
    MmsEvaluationApiV1_4 api{};Check(getter&&getter(&api)==MMS_OK,"real plugin negotiates evaluation 1.4");
    MmsEvaluationSceneCapsV1 caps{};Check(api.getSceneCapabilities(api.context,&caps)==MMS_OK&&(caps.flags&MMS_EVALUATION_SCENE_SKELETON),"production advertises independent skeleton scene");
    wchar_t path[32768]{},target[32768]{};Check(GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL",path,32768)&&GetEnvironmentVariableW(L"MMS_SCENE_TEST_TARGET_MODEL",target,32768),"real PMX fixtures supplied");
    const auto load=[&]{LoadModelFile(app,path,false);LoadModelFile(app,target,false);LoadModelFile(app,path,false);Check(app->ModelSlot(0)&&app->ModelSlot(1)&&app->ModelSlot(2),"load three actual models");Seed(app);
        mdl::Mdl(app->ModelSlot(1))->displayKeys[1].visible=197;};load();
    const auto expected=NativeAt(app,10);
    auto& targetModel=*mdl::Mdl(app->ModelSlot(1));const auto key=targetModel.boneCount+3*2;const auto originalKey=targetModel.boneKeys[key];
    targetModel.boneKeys[key].position[0]+=1.75f;const auto changedExpected=NativeAt(app,10);targetModel.boneKeys[key]=originalKey;
    Check(bone_evaluation_probe::Different(expected[1],changedExpected[1])&&bone_evaluation_probe::Different(expected[0],changedExpected[0]),"target-only registered motion changes consumer and transitive model");
    for(unsigned slot=0;slot<3;++slot){bone_scale::RestoreTransforms(app->ModelSlot(slot));mdl::Mdl(app->ModelSlot(slot))->boneTable[0].trans[0]=123;}
    app->CurrentFrame()=0;app->SceneModified()=0;
    MmsEvaluationSceneQueryV1 query{};query.h=Header<MmsEvaluationSceneQueryV1>();MmsHandle source=0,frame=0;
    const auto create=[&](MmsHandle& out){return api.createSceneSource(api.context,&query,&out);};
    auto& future=mdl::Mdl(app->ModelSlot(0))->displayKeys[2].selectorStates[0];const auto savedFuture=future;
    for(const auto invalid:{mdl::BoneReference{99,0},mdl::BoneReference{1,100000}}){future=invalid;MmsHandle rejected=0x123456;
        const Proof proof(app);Check(create(rejected)==MMS_E_STALE&&rejected==0x123456,"invalid future target rejects complete source without publishing");proof.Verify();}
    future=savedFuture;
    {const Proof proof(app);Check(create(source)==MMS_OK,"capture complete public scene");proof.Verify();}
    const auto ids=Identities(app,api,source);MmsEvaluationSceneInfoV1 info{};
    Check(api.describeSceneSource(api.context,source,&info)==MMS_OK&&info.modelCount==3&&info.captureId,"describe public scene source");
    MmsEvaluationFrameQueryV1 request{};request.h=Header<MmsEvaluationFrameQueryV1>();request.source=source;request.time={10,30};
    {const Proof proof(app);Check(api.evaluateFrame(api.context,&request,&frame)==MMS_OK&&FrameMatches(api,frame,ids,expected),"public frame matches native full-scene two-phase results");proof.Verify();}
    {const Proof proof(app);bool firstOk=true,secondOk=true;
        const auto worker=[&](bool& ok){for(unsigned n=0;n<3;++n){MmsHandle sample=0;const auto result=api.evaluateFrame(api.context,&request,&sample);
            if(result!=MMS_OK){ok=false;continue;}ok=FrameMatches(api,sample,ids,expected)&&ok;if(api.release(api.context,sample)!=MMS_OK)ok=false;}};
        std::thread first([&]{worker(firstOk);}),second;try{second=std::thread([&]{worker(secondOk);});}catch(...){first.join();throw;}
        first.join();second.join();Check(firstOk&&secondOk,"two background workers evaluate and release real public scene frames");proof.Verify();}
    {const Proof proof(app);const auto inputs=PublishInputs(app);auto internal=BoneSceneSource::Capture(inputs.data(),3);auto math=BoneTransformContext::Create();Check(bool(math),"create independent time-boundary oracle");
        const auto left=internal->Evaluate(std::nextafter(4.0,0.0),*math),right=internal->Evaluate(4,*math);ScenePoses leftPoses,rightPoses;
        for(unsigned row=0;row<3;++row){const auto* a=left->ModelFrame(row);const auto* b=right->ModelFrame(row);leftPoses[row].resize(a->BoneCount());rightPoses[row].resize(b->BoneCount());
            for(unsigned bone=0;bone<a->BoneCount();++bone)Check(a->BoneAt(bone,leftPoses[row][bone])&&b->BoneAt(bone,rightPoses[row][bone]),"read independent time-boundary poses");}
        Check(bone_evaluation_probe::Different(leftPoses[1],rightPoses[1]),"real consumer detects premature parent key");
        const MmsTime beforeKey{36028797018963968LL,270215977642229761ULL};request.time=beforeKey;MmsHandle exact=0;
        Check(api.evaluateFrame(api.context,&request,&exact)==MMS_OK&&FrameMatches(api,exact,ids,leftPoses,beforeKey),"public rational frame preserves pre-key parent matrices");
        Check(api.release(api.context,exact)==MMS_OK,"release exact-boundary frame");request.time={10,30};proof.Verify();}
    MmsEvaluationFrameInfoV1 frameInfo{};Check(api.describeFrame(api.context,frame,&frameInfo)==MMS_OK&&frameInfo.captureId==info.captureId&&frameInfo.frameId,"frame identifies its captured source and independent result");
    targetModel.boneKeys[key].position[0]+=1.75f;
    {const Proof proof(app);MmsHandle newer=0,newFrame=0,oldFrame=0;Check(create(newer)==MMS_OK,"capture changed target motion");
        request.source=newer;Check(api.evaluateFrame(api.context,&request,&newFrame)==MMS_OK&&FrameMatches(api,newFrame,ids,changedExpected),"new source propagates changed target motion");
        request.source=source;Check(api.evaluateFrame(api.context,&request,&oldFrame)==MMS_OK&&FrameMatches(api,oldFrame,ids,expected),"old source keeps registered target motion frozen");
        Check(api.release(api.context,newFrame)==MMS_OK&&api.release(api.context,oldFrame)==MMS_OK&&api.release(api.context,newer)==MMS_OK,"release independent updated captures");proof.Verify();}
    targetModel.boneKeys[key]=originalKey;
    MmsHandle alias=0;Check(api.retain(api.context,frame,&alias)==MMS_OK&&api.release(api.context,frame)==MMS_OK,"retain and release frame using shared lease");frame=alias;
    Check(api.release(api.context,source)==MMS_OK&&FrameMatches(api,frame,ids,expected),"frame survives its source handle");
    for(unsigned slot=0;slot<3;++slot)SeekModelFrame(app->ModelSlot(slot),0,0);app->CurrentFrame()=0;
    const auto project=std::filesystem::absolute(L"scene-evaluation-abi.pmm");Check(SaveSceneCopy(app,project.c_str()),"save actual PMM before deletion");ResetAppState(app);
    Check(FrameMatches(api,frame,ids,expected),"frame survives all model deletion");load();MmsHandle replacement=0;
    {const Proof proof(app);Check(create(replacement)==MMS_OK,"capture replacement model slots");const auto changed=Identities(app,api,replacement);Check(!Same(changed[1].object,ids[1].object),"reused model slot gets new object identity");Check(FrameMatches(api,frame,ids,expected),"old frame stays frozen after replacement");proof.Verify();}
    Check(api.release(api.context,replacement)==MMS_OK,"release replacement source");ResetAppState(app);
    wcscpy_s(app->state.envFileName,project.c_str());LoadSceneFile();Check(app->ModelSlot(0)&&app->ModelSlot(1)&&app->ModelSlot(2),"reload actual PMM");
    Check(mdl::Mdl(app->ModelSlot(1))->displayKeys[1].visible==1,"PMM stores the visibility byte as a standard boolean");
    Check(FrameMatches(api,frame,ids,expected),"old frame retains original identities and matrices after PMM reload");
    {const Proof proof(app);Check(create(replacement)==MMS_OK,"capture reloaded scene");const auto reloaded=Identities(app,api,replacement);request.source=replacement;MmsHandle fresh=0;
        Check(api.evaluateFrame(api.context,&request,&fresh)==MMS_OK&&FrameMatches(api,fresh,reloaded,expected,{1,3},1),"reloaded PMM produces native-equivalent scene frame");Check(api.release(api.context,fresh)==MMS_OK,"release reloaded frame");proof.Verify();}
    Check(api.release(api.context,replacement)==MMS_OK&&api.release(api.context,frame)==MMS_OK,"release all public scene leases");ResetAppState(app);
    std::puts("Production evaluation 1.4: BIND identities, native skeleton scene and independent frame lifetime passed");
}
}
#endif
