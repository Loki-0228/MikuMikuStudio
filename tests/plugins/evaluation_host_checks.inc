#include <mms/evaluation.h>
#include "mikumikustudio/bone_curve.hpp"
namespace evaluation_host_checks {
void Run(MMDApp* app) {
    using namespace mikumikustudio::plugins;
    wchar_t modelPath[32768]{};
    Check(GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL",modelPath,32768)>0,"evaluation model fixture supplied");
    const auto module=GetModuleHandleW(L"mms_evaluation_host_probe.dll");
    Check(module!=nullptr,"production runtime loaded evaluation fixture DLL");
    using Getter=MmsResult (MMS_CALL*)(MmsEvaluationApiV1*);
    auto getter=reinterpret_cast<Getter>(GetProcAddress(module,"MmsEvaluationHostGetApi"));
    MmsEvaluationApiV1 api{};
    Check(getter&&getter(&api)==MMS_OK,"real DLL obtained production evaluation service");
    LoadModelFile(app,modelPath,false);
    Check(app->ModelSlot(0)!=nullptr,"load evaluation PMX");
    auto* raw=app->ModelSlot(0);auto& model=*mdl::Mdl(raw);
    auto scene=CreateSceneStore(app);SceneClient client;auto sceneApi=([&]{ MmsSceneApiV1 value{}; if (scene->Api(client, value) != MMS_OK) throw std::runtime_error("scene API binding failed"); return value; }());
    MmsSceneQueryV1 sceneQuery{};sceneQuery.h=Header<MmsSceneQueryV1>();
    sceneQuery.flags=MMS_SCENE_CURRENT_TIME;sceneQuery.channels=MMS_CHANNEL_BONES;
    MmsSnapshotV1 bind{};Check(sceneApi.acquire(sceneApi.context,&sceneQuery,&bind)==MMS_OK,"acquire stable bind identities");
    MmsSpan objects{},bones{};
    Check(sceneApi.objects(sceneApi.context,bind.handle,&objects)==MMS_OK&&objects.count==1,"one real model for evaluation");
    const auto object=*static_cast<const MmsObjectV1*>(objects.data);
    Check(sceneApi.readChannel(sceneApi.context,bind.handle,object.object,MMS_CHANNEL_BONES,&bones)==MMS_OK,"read bind bone identities");
    unsigned selected=0;
    while(selected<model.boneCount&&(model.boneTable[selected].slotIndex>=0||
        (unsigned(model.boneTable[selected].type)>unsigned(mdl::BoneType::Effector)&&model.boneTable[selected].type!=mdl::BoneType::FixedAxis)))++selected;
    Check(selected<model.boneCount,"fixture has independent bone channels");
    const auto bone=static_cast<const MmsBoneV1*>(bones.data)[selected];
    bone_curve::Tracks tracks;
    for(int lane=0;lane<9;++lane){animation_curve::Key first,last;last.frame=4;
        first.value=last.value=lane>=6?1:0;
        if(lane==0)last.value=8;
        if(lane==1){first.outSlope=4;last.inSlope=-4;}
        first.tangent=last.tangent=animation_curve::Tangent::Free;tracks[lane].keys={first,last};}
    std::string error;Check(bone_curve::Write(raw,int(selected),tracks,error),"write real free-curve motion");
    app->CurrentFrame()=7;app->SceneModified()=0;
    const auto author=bone_curve::AuthoringState::Capture(raw);
    auto before=std::make_unique<mdl::ModelRecord>(model);
    const std::vector<mdl::BoneRecord> poses(model.boneTable,model.boneTable+model.boneCount);
    const auto unchanged=[&]{Check(app->CurrentFrame()==7&&app->SceneModified()==0,"evaluation never seeks or modifies project");
        Check(!std::memcmp(before.get(),&model,sizeof(model))&&!std::memcmp(poses.data(),model.boneTable,poses.size()*sizeof(mdl::BoneRecord)),"capture and samples preserve model and edited poses");
        Check(author->Equals(*bone_curve::AuthoringState::Capture(raw)),"capture preserves free authoring");};
    MmsEvaluationSourceQueryV1 query{Header<MmsEvaluationSourceQueryV1>(),object.object,0,0,0};
    MmsHandle first=0;Check(api.createSource(api.context,&query,&first)==MMS_OK,"create actual frozen source");
    MmsEvaluationSourceInfoV1 info{};MmsEvaluationBoneV1 descriptor{};
    Check(api.describeSource(api.context,first,&info)==MMS_OK&&Same(info.object,object.object)&&info.captureId&&info.boneCount==model.boneCount,"source carries stable object and independent capture ID");
    Check(api.boneAt(api.context,first,selected,&descriptor)==MMS_OK&&Same(descriptor.id,bone.id)&&descriptor.flags==MMS_EVALUATION_BONE_LOCAL,"frozen bone ID equals scene channel ID");
    MmsBoneChannelsV1 sample{};
    Check(api.sampleBone(api.context,first,bone.id,{2,30},&sample)==MMS_OK&&std::abs(sample.position[1]-4.f)<1e-4f,"sample seconds returns actual free-curve overshoot");
    unchanged();
    MmsResult workerCreate=MMS_OK,workerRead=MMS_E_FAILED;MmsHandle rejected=4321;
    std::thread worker([&]{workerCreate=api.createSource(api.context,&query,&rejected);MmsBoneChannelsV1 value{};
        workerRead=api.sampleBone(api.context,first,bone.id,{4,30},&value);if(value.position[0]!=8)workerRead=MMS_E_FAILED;});worker.join();
    Check(workerCreate==MMS_E_THREAD&&rejected==4321&&workerRead==MMS_OK,"worker can sample but cannot capture");unchanged();
    const auto endpoint=model.boneKeys[selected].next;Check(endpoint&&model.boneKeys[endpoint].frame==4,"real motion endpoint");
    model.boneKeys[endpoint].position[0]=23;
    const auto pending=bone_curve::AuthoringState::Capture(raw);
    MmsHandle newer=0;Check(api.createSource(api.context,&query,&newer)==MMS_OK,"new source captures unsynchronized native edit");
    MmsEvaluationSourceInfoV1 newerInfo{};Check(api.describeSource(api.context,newer,&newerInfo)==MMS_OK&&newerInfo.captureId!=info.captureId,"new capture is distinct even without history publication");
    Check(pending->Equals(*bone_curve::AuthoringState::Capture(raw)),"new capture does not sync live free author");
    Check(api.sampleBone(api.context,newer,bone.id,{4,30},&sample)==MMS_OK&&sample.position[0]==23,"new source sees native edit");
    Check(api.sampleBone(api.context,first,bone.id,{4,30},&sample)==MMS_OK&&sample.position[0]==8,"old source remains frozen");
    app->CurrentFrame()=0;
    const auto project=std::filesystem::absolute(L"evaluation-identity.pmm");
    Check(SaveSceneCopy(app,project.c_str()),"save project for generation replacement");
    ResetAppState(app);
    MmsHandle stale=999;Check(api.createSource(api.context,&query,&stale)==MMS_E_STALE&&stale==999,"deleted source identity cannot capture again");
    Check(api.sampleBone(api.context,newer,bone.id,{4,30},&sample)==MMS_OK&&sample.position[0]==23,"old source survives real model destruction");
    wcscpy_s(app->state.envFileName,project.c_str());LoadSceneFile();
    Check(app->ModelSlot(0)!=nullptr,"reopen real evaluation project");
    Check(api.createSource(api.context,&query,&stale)==MMS_E_STALE&&stale==999,"old generation cannot capture reopened model");
    Check(api.sampleBone(api.context,first,bone.id,{4,30},&sample)==MMS_OK&&sample.position[0]==8,"first source survives project replacement");
    Check(api.release(api.context,newer)==MMS_OK&&api.release(api.context,first)==MMS_OK,"release production evaluation sources");
    sceneApi.release(sceneApi.context,bind.handle);ResetAppState(app);
    std::puts("Production evaluation service: DLL, identity, time, authoring and lifetime passed");
}
}
