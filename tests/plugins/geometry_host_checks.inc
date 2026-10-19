namespace scene_geometry_checks {
using namespace mikumikustudio::plugins;
bool Near(float a,float b) { return std::fabs(a-b)<.00001f; }
template<class T> const T& Channel(const MmsSceneApiV1& api,MmsHandle handle,MmsObjectRef object,uint32_t channel) {
    MmsSpan span{};Check(api.readChannel(api.context,handle,object,channel,&span)==MMS_OK && span.count && span.stride==sizeof(T),"typed geometry channel");
    return *static_cast<const T*>(span.data);
}
#include "definition_host_checks.inc"
void Run(MMDApp* app) {
    wchar_t path[32768]{};GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL",path,32768);LoadModelFile(app,path,false);
    Check(app->ModelSlot(0)!=nullptr,"load original geometry model");
    auto& model=*mdl::Mdl(app->ModelSlot(0));const bool pmx=model.physicsMode==2;
    GetEnvironmentVariableW(L"MMS_SCENE_TEST_ACCESSORY",path,32768);LoadAccessoryFile(path);
    Check(app->AccessorySlot(0)!=nullptr,"load original accessory geometry");
    auto store=CreateSceneStore(app);SceneClient client;auto api=store->Api(client);
    MmsSceneQueryV1 query{};query.h=Header<MmsSceneQueryV1>();query.time={0,1};
    query.channels=511;
    MmsSnapshotV1 snapshot{};Check(api.acquire(api.context,&query,&snapshot)==MMS_OK,"acquire bind geometry and materials");
    MmsSpan objects{};api.objects(api.context,snapshot.handle,&objects);Check(objects.count==2,"enumerate model and accessory");
    const auto object=static_cast<const MmsObjectV1*>(objects.data)[0];
    const auto accessory=static_cast<const MmsObjectV1*>(objects.data)[1];
    const auto& mesh=Channel<MmsMeshV1>(api,snapshot.handle,object.object,MMS_CHANNEL_MESH);
    const auto& binding=Channel<MmsBindingV1>(api,snapshot.handle,object.object,MMS_CHANNEL_BINDING);
    const auto& material=Channel<MmsMaterialV1>(api,snapshot.handle,object.object,MMS_CHANNEL_MATERIALS);
    const auto& texture=Channel<MmsTextureV1>(api,snapshot.handle,object.object,MMS_CHANNEL_TEXTURES);
    Check(mesh.vertices.count==(pmx?5u:4u) && mesh.triangles.count==2,"original model topology counts");
    const auto* vertices=static_cast<const MmsVertexV1*>(mesh.vertices.data);
    const auto* triangles=static_cast<const MmsTriangleV1*>(mesh.triangles.data);
    const auto* weights=static_cast<const MmsWeightV1*>(binding.weights.data);
    Check(triangles[0].vertex[0]==0 && triangles[0].vertex[1]==1 && triangles[0].vertex[2]==2 && Same(triangles[0].material,material.id),"triangle indices and stable material association");
    Check(binding.bindVertices.count==mesh.vertices.count && binding.morphVertices.data==binding.bindVertices.data && binding.weights.count==mesh.vertices.count,"stage zero binding arrays correspond");
    const auto* mapping=static_cast<const uint32_t*>(binding.vertexBindings.data);
    Check(binding.vertexBindings.stride==4 && mapping[mesh.vertices.count-1]==mesh.vertices.count-1,"display vertex maps to binding index");
    Check(Same(material.texture,texture.id) && String(texture.path).find("color.bmp")!=std::string::npos,"material texture resolves to owned path");
    Check(texture.width==0 && texture.height==0 && texture.format==0,"unqueried resource metadata explicitly unknown");
    MmsSpan bones{};api.readChannel(api.context,snapshot.handle,object.object,MMS_CHANNEL_BONES,&bones);
    const auto* bone=static_cast<const MmsBoneV1*>(bones.data);
    Check(Same(weights[0].bones[0],bone[0].id),"weights use bone IDs from the same scene");
    const auto definitionIds=Definitions(app,api,snapshot.handle,object.object,pmx);
    app->CurrentFrame()=17;auto current=query;current.flags=MMS_SCENE_CURRENT_TIME;current.time={};MmsSnapshotV1 currentSnapshot{};
    Check(api.acquire(api.context,&current,&currentSnapshot)==MMS_OK && app->CurrentFrame()==17,"current-time query works away from frame zero without seeking");
    api.release(api.context,currentSnapshot.handle);app->CurrentFrame()=0;
    if(pmx) {
        Check(Near(vertices[0].position[0],0) && Near(vertices[4].position[1],8) && Near(mesh.boundsMax[0],4),"PMX source positions and bounds");
        for(unsigned i=0;i<4;++i)Check(weights[i].type==i,"PMX weight type retained");
        Check(Near(weights[0].weights[0],1) && Empty(weights[0].bones[1]) && Near(weights[1].weights[1],.75f),"implicit BDEF weights and absent bones normalized");
        Check(Near(weights[4].weights[3],.4f) && Same(weights[4].bones[3],bone[3].id),"BDEF4 keeps all four bones and weights");
        Check(Near(weights[3].sdefCenter[0],1) && Near(weights[3].sdefR0[0],-3) && Near(weights[3].sdefR1[0],1),"SDEF exports weighted-pivot offsets");
        std::printf("UV base=%g,%g,%g,%g absent=%g SDEF=%g\n",vertices[0].additionalUv[0],vertices[0].additionalUv[1],vertices[0].additionalUv[2],vertices[0].additionalUv[3],vertices[0].additionalUv[4],weights[0].sdefCenter[0]);
        Check(Near(vertices[0].additionalUv[3],4) && vertices[0].additionalUv[4]==0 && weights[0].sdefCenter[0]==0,"unused UV and SDEF fields are zero");
        auto& source=model.pmxVertices[0];auto& sourceMaterial=static_cast<mdl::ModelMaterialRecord*>(model.materials)[0];
        const auto saved=source;const float savedDiffuse=sourceMaterial.diffuse[0];
        source.position[0]=99;source.uv[0]=99;source.additionalUvByComponent[0][0]=99;sourceMaterial.diffuse[0]=99;
        MmsSnapshotV1 morphed{};Check(api.acquire(api.context,&query,&morphed)==MMS_OK,"capture while working arrays contain morph output");
        const auto& restored=Channel<MmsMeshV1>(api,morphed.handle,object.object,MMS_CHANNEL_MESH);
        const auto& restoredMaterial=Channel<MmsMaterialV1>(api,morphed.handle,object.object,MMS_CHANNEL_MATERIALS);
        const auto& v=*static_cast<const MmsVertexV1*>(restored.vertices.data);
        Check(Near(v.position[0],0) && Near(v.uv[0],.25f) && Near(v.additionalUv[0],1) && Near(restoredMaterial.diffuse[0],.8f),"bind snapshot restores position, UV and material bases");
        Check(source.position[0]==99 && sourceMaterial.diffuse[0]==99,"snapshot leaves live morph state unchanged");
        source=saved;sourceMaterial.diffuse[0]=savedDiffuse;api.release(api.context,morphed.handle);
        model.pmxVertices[4].weightType=mdl::PmxWeightType::qdef;
        MmsSnapshotV1 qdef{};Check(api.acquire(api.context,&query,&qdef)==MMS_OK,"acquire a runtime QDEF record independently of file importer");
        const auto& qdefBinding=Channel<MmsBindingV1>(api,qdef.handle,object.object,MMS_CHANNEL_BINDING);
        const auto& qdefWeight=static_cast<const MmsWeightV1*>(qdefBinding.weights.data)[4];
        Check(qdefWeight.type==MMS_WEIGHT_QDEF && Near(qdefWeight.weights[3],.4f) && Same(qdefWeight.bones[3],bone[3].id),"QDEF ABI preserves all influences");
        Check(qdef.sceneRevision>snapshot.sceneRevision,"changed binding advances the scene revision");
        model.pmxVertices[4].weightType=mdl::PmxWeightType::bdef4;api.release(api.context,qdef.handle);
    } else {
        Check(Near(vertices[0].position[0],-6) && Near(mesh.boundsMax[1],16) && Near(weights[0].weights[0],1),"PMD source positions and percentage weights");
        Check(triangles[1].vertex[0]==2 && triangles[1].vertex[1]==1 && triangles[1].vertex[2]==3,"PMD padded CPU indices decoded");
    }
    const auto& acsMesh=Channel<MmsMeshV1>(api,snapshot.handle,accessory.object,MMS_CHANNEL_MESH);
    const auto& acsBinding=Channel<MmsBindingV1>(api,snapshot.handle,accessory.object,MMS_CHANNEL_BINDING);
    Check(acsMesh.vertices.count==3 && acsMesh.triangles.count==1 && Near(acsMesh.boundsMax[0],1),"accessory original mesh and bounds");
    const auto& acsWeight=*static_cast<const MmsWeightV1*>(acsBinding.weights.data);
    Check(acsWeight.type==MMS_WEIGHT_NONE && Empty(acsWeight.bones[0]) && acsWeight.weights[0]==0,"accessory does not invent a skinning bone");
    const auto& acsMaterial=Channel<MmsMaterialV1>(api,snapshot.handle,accessory.object,MMS_CHANNEL_MATERIALS);
    Check(Same(static_cast<const MmsTriangleV1*>(acsMesh.triangles.data)->material,acsMaterial.id),"accessory face attributes resolve to material IDs");
    MmsSnapshotV1 settled{};Check(api.acquire(api.context,&query,&settled)==MMS_OK,"publish restored binding");
    query.channels=MMS_CHANNEL_BONES;MmsSnapshotV1 minimal{};
    Check(api.acquire(api.context,&query,&minimal)==MMS_OK && minimal.sceneRevision==settled.sceneRevision,"channel selection does not change scene revision");
    api.release(api.context,settled.handle);
    api.release(api.context,minimal.handle);query.channels=511;
    const auto oldCount=model.indexCount;model.indexCount=1;
    query.objects={&accessory.object,1,sizeof(MmsObjectRef),1};MmsSnapshotV1 filtered{};
    Check(api.acquire(api.context,&query,&filtered)==MMS_OK,"object filter avoids unrelated unavailable geometry");
    api.objects(api.context,filtered.handle,&objects);Check(objects.count==1 && Same(static_cast<const MmsObjectV1*>(objects.data)->object,accessory.object),"filtered geometry has only the requested object");
    api.release(api.context,filtered.handle);query.objects={};MmsSnapshotV1 rejected{};rejected.handle=12345;
    Check(api.acquire(api.context,&query,&rejected)==MMS_E_FAILED && rejected.handle==12345,"invalid topology cannot publish a partial snapshot");
    model.indexCount=oldCount;
    if(pmx) {
        const auto type=model.morphs[0].type;model.morphs[0].type=9;
        Check(api.acquire(api.context,&query,&rejected)==MMS_E_UNSUPPORTED && rejected.handle==12345,"unsupported morph kinds cannot masquerade as empty data");
        model.morphs[0].type=type;
        const auto index=model.jointTable[0].rigidB;model.jointTable[0].rigidB=-1;
        Check(api.acquire(api.context,&query,&rejected)==MMS_E_NOT_READY && rejected.handle==12345,"unresolved joint cannot publish dangling body references");
        model.jointTable[0].rigidB=index;
    }
    const auto project=std::filesystem::absolute(L"geometry-identity.pmm");
    Check(SaveSceneCopy(app,project.c_str()),"save geometry element identities in real PMM");
    ResetAppState(app);wcscpy_s(app->state.envFileName,project.c_str());LoadSceneFile();
    MmsSnapshotV1 reopened{};Check(api.acquire(api.context,&query,&reopened)==MMS_OK,"reopen geometry scene");
    api.objects(api.context,reopened.handle,&objects);Check(objects.count==2,"reopened objects");
    const auto restored=static_cast<const MmsObjectV1*>(objects.data)[0];
    const auto restoredAcs=static_cast<const MmsObjectV1*>(objects.data)[1];
    const auto restoredDefinitions=Definitions(app,api,reopened.handle,restored.object,pmx);
    Check(Same(definitionIds.morph,restoredDefinitions.morph) && Same(definitionIds.rigid,restoredDefinitions.rigid) && Same(definitionIds.shape,restoredDefinitions.shape) && Same(definitionIds.joint,restoredDefinitions.joint),"morph and physics definition IDs survive PMM reopen");
    const auto& newMesh=Channel<MmsMeshV1>(api,reopened.handle,restored.object,MMS_CHANNEL_MESH);
    const auto& newMaterial=Channel<MmsMaterialV1>(api,reopened.handle,restored.object,MMS_CHANNEL_MATERIALS);
    const auto& newTexture=Channel<MmsTextureV1>(api,reopened.handle,restored.object,MMS_CHANNEL_TEXTURES);
    Check(Same(vertices[0].sourceVertex,static_cast<const MmsVertexV1*>(newMesh.vertices.data)->sourceVertex) && Same(material.id,newMaterial.id) && Same(texture.id,newTexture.id),"vertex, material and texture IDs survive PMM reopen");
    const auto& newAcs=Channel<MmsMeshV1>(api,reopened.handle,restoredAcs.object,MMS_CHANNEL_MESH);
    Check(Same(static_cast<const MmsVertexV1*>(acsMesh.vertices.data)->sourceVertex,static_cast<const MmsVertexV1*>(newAcs.vertices.data)->sourceVertex),"accessory vertex ID survives PMM reopen");
    Check(mesh.vertices.count==(pmx?5u:4u) && Same(triangles[0].material,material.id) && !String(texture.path).empty(),"nested old lease arrays and strings survive scene teardown");
    pmm_extension::Document identity;SaveSceneIdentity(*app,identity);
    auto corrupt=identity;Check(!corrupt.records.empty(),"identity extension available");corrupt.records[0].value.push_back(0xff);
    LoadSceneIdentity(*app,corrupt);auto retained=corrupt;SaveSceneIdentity(*app,retained);
    Check(retained.records[0].value==corrupt.records[0].value,"malformed identity payload retained without rewriting");
    MmsSnapshotV1 afterBad{};Check(api.acquire(api.context,&query,&afterBad)==MMS_OK,"malformed identity does not disable valid snapshots");
    api.objects(api.context,afterBad.handle,&objects);
    Check(Same(static_cast<const MmsObjectV1*>(objects.data)->object,restored.object),"invalid extension does not partially replace identities");
    api.release(api.context,afterBad.handle);api.release(api.context,reopened.handle);api.release(api.context,snapshot.handle);
    ResetAppState(app);std::puts("Production bind geometry and persistence checks passed");
}
}
