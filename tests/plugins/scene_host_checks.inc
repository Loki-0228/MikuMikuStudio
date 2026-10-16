namespace scene_host_checks {
void Run(MMDApp* app) {
    using namespace mikudancestudio::plugins;
    wchar_t modelPath[32768]{};
    Check(GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL",modelPath,32768)>0,"scene model fixture supplied");
    LoadModelFile(app,modelPath,false);
    Check(app->ModelSlot(0)!=nullptr,"load real scene fixture");
    auto store=CreateSceneStore(app);SceneClient client;auto api=store->Api(client);
    MmsSceneQueryV1 query{};query.h=Header<MmsSceneQueryV1>();query.time={0,1};query.channels=MMS_CHANNEL_BONES;
    MmsSnapshotV1 old{};Check(api.acquire(api.context,&query,&old)==MMS_OK,"acquire real bind scene");
    MmsSpan objects{},bones{};
    Check(api.objects(api.context,old.handle,&objects)==MMS_OK && objects.count==1,"enumerate real model");
    auto object=*static_cast<const MmsObjectV1*>(objects.data);
    Check(object.kind==MMS_OBJECT_MODEL && !Empty(object.object.id) && object.object.generation,"opaque model ID and generation");
    Check(api.readChannel(api.context,old.handle,object.object,MMS_CHANNEL_BONES,&bones)==MMS_OK,"read real bone channel");
    const auto& model=*mdl::Mdl(app->ModelSlot(0));
    Check(bones.count==model.boneCount && bones.count>0,"bone count matches the loaded PMX");
    const auto bone=*static_cast<const MmsBoneV1*>(bones.data);
    for(unsigned axis=0;axis<3;++axis)
        Check(bone.bindWorld[12+axis]==model.boneTable[0].position[axis] && bone.inverseBindWorld[12+axis]==-model.boneTable[0].position[axis],"bind matrices use source bone origins");
    const std::string name=String(object.name),boneName=String(bone.name);
    const auto project=std::filesystem::absolute(L"scene-identity.pmm");
    Check(SaveSceneCopy(app,project.c_str()),"save real PMM with stable identity extension");
    ResetAppState(app);
    query.objects={&object.object,1,sizeof(MmsObjectRef),1};MmsSnapshotV1 ignored{};
    Check(api.acquire(api.context,&query,&ignored)==MMS_E_STALE,"deleted object reference is stale");
    Check(String(object.name)==name && String(bone.name)==boneName,"leased scene survives model teardown");
    wcscpy_s(app->state.envFileName,project.c_str());LoadSceneFile();
    Check(app->ModelSlot(0)!=nullptr,"reopen real PMM");
    query.objects={};MmsSnapshotV1 reopened{};
    Check(api.acquire(api.context,&query,&reopened)==MMS_OK,"acquire reopened scene");
    Check(api.objects(api.context,reopened.handle,&objects)==MMS_OK && objects.count==1,"enumerate reopened model");
    const auto restored=*static_cast<const MmsObjectV1*>(objects.data);
    Check(Same(restored.object.id,object.object.id) && restored.object.generation!=object.object.generation,"save/open preserves ID and changes runtime generation");
    Check(api.readChannel(api.context,reopened.handle,restored.object,MMS_CHANNEL_BONES,&bones)==MMS_OK && Same(static_cast<const MmsBoneV1*>(bones.data)->id,bone.id),"bone identity survives real PMM reopen");
    query.objects={&object.object,1,sizeof(MmsObjectRef),1};
    Check(api.acquire(api.context,&query,&ignored)==MMS_E_STALE,"pre-open reference cannot address reopened model");
    query.objects={};query.channels=MMS_CHANNEL_RIGIDS;
    Check(api.acquire(api.context,&query,&ignored)==MMS_E_UNSUPPORTED,"unimplemented channel is not advertised as empty success");
    query.channels=MMS_CHANNEL_BONES;query.stage=MMS_STAGE_PHYSICS;
    Check(api.acquire(api.context,&query,&ignored)==MMS_E_UNSUPPORTED,"bind pose cannot masquerade as physics output");
    query.stage=MMS_STAGE_BIND;query.time={1,30};
    Check(api.acquire(api.context,&query,&ignored)==MMS_E_NOT_READY && app->CurrentFrame()==0,"snapshot query cannot seek the global timeline");
    MmsResult read=MMS_E_FAILED;std::thread worker([&]{MmsSpan records{};read=api.objects(api.context,old.handle,&records);api.release(api.context,old.handle);});worker.join();
    Check(read==MMS_OK,"old snapshot can be read and released on a worker");
    api.release(api.context,reopened.handle);Check(!store->HasLeases(client),"all scene leases released");
    ResetAppState(app);
    std::puts("Production scene snapshot, identity and PMM round-trip checks passed");
}
}
