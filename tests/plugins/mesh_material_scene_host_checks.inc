#include "mikumikustudio/mesh_scene_evaluation.hpp"
#include "mikumikustudio/bone_transform_context.hpp"

#if !MMD_ORIGINAL_PROFILE
namespace mesh_material_scene_probe {
using namespace mikumikustudio::plugins;
namespace {
template<class A,class B> bool SameArray(const A& a,const B& b) {
    return std::memcmp(a.data(),b,sizeof(float)*a.size())==0;
}
bool Matches(const MeshSceneMaterial& value,const mdl::ModelMaterialRecord& native,
             const mdl::MaterialMorphChannels& add,const mdl::MaterialMorphChannels& mul,
             const std::array<float,3>& originalDiffuse) {
    return value.physicsMode==2 && value.nativeDiffuse==originalDiffuse &&
        !std::memcmp(value.diffuseMirrorAndAlpha.data(),native.diffuseMirror,3*sizeof(float)) &&
        std::memcmp(&value.diffuseMirrorAndAlpha[3],&native.diffuse[3],sizeof(float))==0 &&
        SameArray(value.specular,native.specular) && SameArray(value.ambient,native.ambient) &&
        std::memcmp(&value.specularPower,&native.specularPower,sizeof(float))==0 &&
        SameArray(value.edgeColor,native.edgeColor) && std::memcmp(&value.edgeSize,&native.edgeSize,sizeof(float))==0 &&
        SameArray(value.textureAdd,add.textureTint) && SameArray(value.textureMultiply,mul.textureTint) &&
        SameArray(value.sphereAdd,add.sphereTint) && SameArray(value.sphereMultiply,mul.sphereTint) &&
        SameArray(value.toonAdd,add.toonTint) && SameArray(value.toonMultiply,mul.toonTint);
}
bool SameId(MmsId a,MmsId b){return a.lo==b.lo&&a.hi==b.hi;}
bool PublicMatches(const MmsEvaluationMaterialV1& a,const MeshSceneMaterial& b,MmsId id,unsigned index) {
    return a.h.size==sizeof(a)&&a.h.major==1&&!a.h.minor&&SameId(a.material,id)&&a.index==index&&
        a.flags==(b.physicsMode==2?MMS_EVALUATION_MATERIAL_FORMAT_PMX:MMS_EVALUATION_MATERIAL_FORMAT_PMD)&&
        !a.reserved[0]&&!a.reserved[1]&&SameArray(b.nativeDiffuse,a.nativeDiffuse)&&
        SameArray(b.diffuseMirrorAndAlpha,a.diffuseMirrorAndAlpha)&&SameArray(b.specular,a.specular)&&
        SameArray(b.ambient,a.ambient)&&!std::memcmp(&b.specularPower,&a.specularPower,4)&&
        SameArray(b.edgeColor,a.edgeColor)&&!std::memcmp(&b.edgeSize,&a.edgeSize,4)&&
        SameArray(b.textureAdd,a.textureAdd)&&SameArray(b.textureMultiply,a.textureMultiply)&&
        SameArray(b.sphereAdd,a.sphereAdd)&&SameArray(b.sphereMultiply,a.sphereMultiply)&&
        SameArray(b.toonAdd,a.toonAdd)&&SameArray(b.toonMultiply,a.toonMultiply);
}
MmsEvaluationApiV1_6 PluginApi(bool required) {
    using Getter=MmsResult(MMS_CALL*)(MmsEvaluationApiV1_6*);
    const auto module=GetModuleHandleW(L"mms_evaluation_host_probe.dll");
    const auto getter=module?reinterpret_cast<Getter>(GetProcAddress(module,"MmsEvaluationHostGetApi16")):nullptr;
    MmsEvaluationApiV1_6 api{};
    if(required || module) Check(getter&&getter(&api)==MMS_OK&&api.h.major==1&&api.h.minor==6,"real plugin negotiates material 1.6 through manager");
    return api;
}
std::vector<MmsId> BindMaterialIds(MMDApp* app,unsigned expected) {
    auto store=CreateSceneStore(app);SceneClient client;auto api=store->Api(client);
    MmsSceneQueryV1 query{};query.h=Header<MmsSceneQueryV1>();query.stage=MMS_STAGE_BIND;
    query.flags=MMS_SCENE_CURRENT_TIME;query.channels=MMS_CHANNEL_MATERIALS;
    MmsSnapshotV1 snapshot{};MmsSpan objects{},materials{};
    Check(api.acquire(api.context,&query,&snapshot)==MMS_OK,"material BIND snapshot acquired");
    Check(api.objects(api.context,snapshot.handle,&objects)==MMS_OK&&objects.count==1,"one material BIND model");
    const auto object=static_cast<const MmsObjectV1*>(objects.data)[0].object;
    Check(api.readChannel(api.context,snapshot.handle,object,MMS_CHANNEL_MATERIALS,&materials)==MMS_OK&&
          materials.count==expected&&materials.stride==sizeof(MmsMaterialV1),"material BIND identity list available");
    std::vector<MmsId> result;const auto* rows=static_cast<const MmsMaterialV1*>(materials.data);
    for(unsigned i=0;i<expected;++i)result.push_back(rows[i].id);
    api.release(api.context,snapshot.handle);return result;
}
MmsHandle CaptureMaterial(const MmsEvaluationApiV1_6& api,const std::vector<MmsId>& ids) {
    MmsEvaluationSceneQueryV1 query{};query.h=Header<MmsEvaluationSceneQueryV1>();MmsHandle source=0;
    Check(api.createMaterialSceneSource(api.context,&query,&source)==MMS_OK&&source,"capture public material scene");
    for(uint32_t i=0;i<ids.size();++i){MmsEvaluationMaterialIdV1 value{};
        Check(api.materialAt(api.context,source,0,i,&value)==MMS_OK&&value.index==i&&SameId(value.material,ids[i]),"material source identity matches BIND");}
    MmsEvaluationMaterialIdV1 unchanged{};unchanged.index=123;
    Check(api.materialAt(api.context,source,0,uint32_t(ids.size()),&unchanged)==MMS_E_NOT_FOUND&&unchanged.index==123,"invalid material index preserves output");
    return source;
}
MmsHandle EvaluateMaterial(const MmsEvaluationApiV1_6& api,MmsHandle source) {
    MmsEvaluationFrameQueryV1 query{};query.h=Header<MmsEvaluationFrameQueryV1>();query.source=source;query.time={0,1};MmsHandle frame=0;
    Check(api.evaluateFrame(api.context,&query,&frame)==MMS_OK&&frame,"evaluate public material frame");return frame;
}
void CheckPublic(const MmsEvaluationApiV1_6& api,MmsHandle frame,const std::vector<MmsId>& ids,
                 const std::array<MeshSceneMaterial,2>& expected) {
    std::array<MmsEvaluationMaterialV1,2> actual{};
    Check(api.readFrameMaterials(api.context,frame,0,0,2,actual.data())==MMS_OK,"read both public material values");
    for(unsigned i=0;i<2;++i)Check(PublicMatches(actual[i],expected[i],ids[i],i),"public material value and identity match independent frozen result");
    auto unchanged=actual;Check(api.readFrameMaterials(api.context,frame,0,1,2,actual.data())==MMS_E_ARGUMENT&&
        std::memcmp(actual.data(),unchanged.data(),sizeof(actual))==0,"invalid material range leaves all output unchanged");
    Check(api.readFrameMaterials(api.context,frame,0,2,0,nullptr)==MMS_OK,"empty material read validates endpoint");
}
void CheckSdk(const std::vector<MmsId>& ids,const std::array<MeshSceneMaterial,2>& expected) {
    using Callback=MmsResult(MMS_CALL*)(MmsEvaluationMaterialIdV1*,MmsEvaluationMaterialV1*,uint32_t);
    const auto module=GetModuleHandleW(L"mms_evaluation_host_probe.dll");
    const auto callback=module?reinterpret_cast<Callback>(GetProcAddress(module,"MmsEvaluationHostSdkMaterials")):nullptr;
    std::array<MmsEvaluationMaterialIdV1,2> actualIds{};std::array<MmsEvaluationMaterialV1,2> values{};
    Check(callback&&callback(actualIds.data(),values.data(),2)==MMS_OK,"real plugin C++ material SDK reads independent frame after source release");
    for(unsigned i=0;i<2;++i)Check(actualIds[i].index==i&&SameId(actualIds[i].material,ids[i])&&
        PublicMatches(values[i],expected[i],ids[i],i),"real C++ material SDK preserves BIND identity and native values");
}
void Pmd(MMDApp* app,unsigned char* raw,const MmsEvaluationApiV1_6& api) {
    auto& model=*mdl::Mdl(raw);Check(model.physicsMode!=2&&model.materialCount==2,"actual PMD fixture has two static materials");
    const auto ids=BindMaterialIds(app,2);auto context=BoneTransformContext::Create();Check(bool(context),"PMD CPU context available");
    BoneSceneModelInput input{0,{345,678,1},raw};EvaluationTime time;Check(EvaluationTime::FromFrame(0,time),"PMD sample time valid");
    const mesh_definition_probe::Proof proof(app,raw);
    auto internal=MeshSceneSource::CaptureWithMaterials(&input,1);auto frame=internal->EvaluateAt(time,*context);
    Check(frame->MaterialCount(0)==2,"actual PMD frame owns two materials");
    std::array<MeshSceneMaterial,2> expected{};const auto* native=static_cast<const mdl::ModelMaterialRecord*>(model.materials);
    for(unsigned i=0;i<2;++i){Check(frame->MaterialAt(0,i,expected[i]),"read actual PMD static material");
        Check(!std::memcmp(expected[i].nativeDiffuse.data(),native[i].diffuse,12)&&
              !std::memcmp(expected[i].diffuseMirrorAndAlpha.data(),native[i].diffuseMirror,12)&&
              !std::memcmp(&expected[i].diffuseMirrorAndAlpha[3],&native[i].diffuse[3],4)&&
              SameArray(expected[i].specular,native[i].specular)&&SameArray(expected[i].ambient,native[i].ambient)&&
              !std::memcmp(&expected[i].specularPower,&native[i].specularPower,4)&&
              SameArray(expected[i].edgeColor,native[i].edgeColor)&&!std::memcmp(&expected[i].edgeSize,&native[i].edgeSize,4),"PMD color, specular and edge fields match native static material");
        for(unsigned c=0;c<4;++c)Check(expected[i].textureAdd[c]==0&&expected[i].textureMultiply[c]==1&&
            expected[i].sphereAdd[c]==0&&expected[i].sphereMultiply[c]==1&&expected[i].toonAdd[c]==0&&expected[i].toonMultiply[c]==1,"PMD tint has native neutral defaults");}
    auto source=CaptureMaterial(api,ids);auto publicFrame=EvaluateMaterial(api,source);CheckPublic(api,publicFrame,ids,expected);CheckSdk(ids,expected);proof.Verify();
    Check(api.release(api.context,source)==MMS_OK,"release PMD material source");ResetAppState(app);internal.reset();context.reset();
    CheckPublic(api,publicFrame,ids,expected);Check(api.release(api.context,publicFrame)==MMS_OK,"PMD frame survives deletion and releases cleanly");
    std::puts("Frozen mesh material scene: public PMD static materials and lifetime passed");
}
}
void Run(MMDApp* app) {
    wchar_t path[32768]{};const auto length=GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL",path,32768);
    Check(length&&length<32768,"material scene fixture supplied");LoadModelFile(app,path,false);
    auto* raw=app->ModelSlot(0);Check(raw&&!app->ModelSlot(1),"load actual material scene model");
    auto& model=*mdl::Mdl(raw);
    const auto api=PluginApi(false);
    if(model.physicsMode!=2){Check(api.context,"PMD public probe requires a loaded 1.6 plugin");Pmd(app,raw,api);return;}
    Check(model.physicsMode==2&&model.materialCount==2&&model.morphCount==3,"fixture has two PMX materials and group/add/multiply morphs");
    Check(model.morphs[0].type==0&&model.morphs[1].type==8&&model.morphs[2].type==8,"real file loads group and two material morphs");
    Check(mdl::MorphKeyPoolEnsure(model,model.morphCount),"reserve registered material weights");
    const float weights[]{.5f,.75f,-.5f};
    for(uint32_t i=0;i<model.morphCount;++i){model.morphKeys[i]={};model.morphKeys[i].allocated=1;model.morphKeys[i].value=weights[i];
        if(model.morphKeyCursors)model.morphKeyCursors[i]=i;if(model.morphTrackActive)model.morphTrackActive[i]=0;model.morphs[i].value=-.125f;}
    app->CurrentFrame()=7;app->SceneModified()=0;
    std::array<std::array<float,3>,2> originalDiffuse{};
    const auto* materials=static_cast<const mdl::ModelMaterialRecord*>(model.materials);
    for(unsigned i=0;i<2;++i)std::copy(materials[i].diffuse,materials[i].diffuse+3,originalDiffuse[i].begin());
    auto context=BoneTransformContext::Create();Check(bool(context),"create independent CPU math");
    BoneSceneModelInput input{0,{123,456,1},raw};EvaluationTime time;Check(EvaluationTime::FromFrame(0,time),"sample time valid");
    std::unique_ptr<MeshSceneSource> source;std::unique_ptr<MeshSceneFrame> frame;
    {const mesh_definition_probe::Proof proof(app,raw);source=MeshSceneSource::CaptureWithMaterials(&input,1);frame=source->EvaluateAt(time,*context);
        Check(frame->MaterialCount(0)==2,"independent frame owns both material results");proof.Verify();}
    std::vector<MmsId> ids;MmsHandle publicSource=0,publicFrame=0,noGroupSource=0,noGroupFrame=0;
    if(api.context){
        MmsEvaluationMaterialCapsV1 caps{};Check(api.getMaterialCapabilities(api.context,&caps)==MMS_OK&&
            (caps.flags&(MMS_EVALUATION_MATERIAL_CAP_CPU_VALUES|MMS_EVALUATION_MATERIAL_CAP_PMD|MMS_EVALUATION_MATERIAL_CAP_PMX))==
            (MMS_EVALUATION_MATERIAL_CAP_CPU_VALUES|MMS_EVALUATION_MATERIAL_CAP_PMD|MMS_EVALUATION_MATERIAL_CAP_PMX)&&caps.maxItemsPerRead>=2,"production material capabilities include PMD and PMX");
        using Getter15=MmsResult(MMS_CALL*)(MmsEvaluationApiV1_5*);const auto module=GetModuleHandleW(L"mms_evaluation_host_probe.dll");
        const auto get15=reinterpret_cast<Getter15>(GetProcAddress(module,"MmsEvaluationHostGetApi15"));MmsEvaluationApiV1_5 previous{};
        Check(get15&&get15(&previous)==MMS_OK&&std::memcmp(&previous.context,&api.context,sizeof(previous)-offsetof(MmsEvaluationApiV1_5,context))==0,"1.6 table preserves actual 1.5 prefix");
        auto& vertex=model.pmxVertices[0];const auto previousType=vertex.weightType;MmsHandle rejected=0x123456;MmsEvaluationSceneQueryV1 query{};query.h=Header<MmsEvaluationSceneQueryV1>();
        MmsResult status=MMS_E_FAILED;try{vertex.weightType=static_cast<mdl::PmxWeightType>(4);status=api.createMaterialSceneSource(api.context,&query,&rejected);}catch(...){vertex.weightType=previousType;throw;}
        vertex.weightType=previousType;Check(status==MMS_E_UNSUPPORTED&&rejected==0x123456,"runtime QDEF rejects material source without publishing");
        ids=BindMaterialIds(app,2);const mesh_definition_probe::Proof proof(app,raw);publicSource=CaptureMaterial(api,ids);publicFrame=EvaluateMaterial(api,publicSource);
        MmsHandle legacy=0;Check(api.createMeshSceneSource(api.context,&query,&legacy)==MMS_OK,"capture legacy 1.5 mesh source");MmsEvaluationMaterialIdV1 absent{};absent.index=123;
        Check(api.materialAt(api.context,legacy,0,0,&absent)==MMS_E_UNSUPPORTED&&absent.index==123,"1.5 mesh source does not expose 1.6 materials");
        Check(api.release(api.context,legacy)==MMS_OK,"release legacy mesh source");proof.Verify();
    }
    const auto groupWeight=model.morphKeys[0].value;model.morphKeys[0].value=0;
    std::unique_ptr<MeshSceneFrame> noGroup;
    {const mesh_definition_probe::Proof proof(app,raw);auto other=MeshSceneSource::CaptureWithMaterials(&input,1);noGroup=other->EvaluateAt(time,*context);proof.Verify();}
    if(api.context){noGroupSource=CaptureMaterial(api,ids);noGroupFrame=EvaluateMaterial(api,noGroupSource);}
    SeekModelFrame(raw,0,0);ModelApplyMorphs(raw);
    const auto* noGroupAdd=mdl::MaterialMorphAdd(raw);const auto* noGroupMul=mdl::MaterialMorphMul(raw);
    std::array<MeshSceneMaterial,2> withoutGroup{};
    for(unsigned i=0;i<2;++i){Check(noGroup->MaterialAt(0,i,withoutGroup[i]),"read material without group");
        Check(Matches(withoutGroup[i],materials[i],noGroupAdd[i].channels,noGroupMul[i].channels,originalDiffuse[i]),"disabled group result matches native material");}
    if(api.context)CheckPublic(api,noGroupFrame,ids,withoutGroup);
    model.morphKeys[0].value=groupWeight;
    SeekModelFrame(raw,0,0);ModelApplyMorphs(raw);
    const auto* add=mdl::MaterialMorphAdd(raw);const auto* mul=mdl::MaterialMorphMul(raw);
    std::array<MeshSceneMaterial,2> expected{};
    for(unsigned i=0;i<2;++i){Check(frame->MaterialAt(0,i,expected[i]),"read independent material");
        Check(Matches(expected[i],materials[i],add[i].channels,mul[i].channels,originalDiffuse[i]),"material matches native group/add/multiply and tint fields");}
    if(api.context){CheckPublic(api,publicFrame,ids,expected);CheckSdk(ids,expected);Check(api.release(api.context,publicSource)==MMS_OK&&api.release(api.context,noGroupSource)==MMS_OK,"release both public material sources");}
    Check(expected[0].textureAdd!=withoutGroup[0].textureAdd&&expected[1].sphereMultiply!=withoutGroup[1].sphereMultiply,
        "group weight changes both additive and multiplicative material results");
    Check(add[0].channels.textureTint[0]!=0 && mul[1].channels.sphereTint[0]!=1,"actual native additive and multiplicative texture paths are active");
    ResetAppState(app);source.reset();context.reset();
    for(unsigned i=0;i<2;++i){MeshSceneMaterial actual{};Check(frame->MaterialAt(0,i,actual),"owned material survives model and source deletion");
        Check(actual.nativeDiffuse==expected[i].nativeDiffuse&&actual.diffuseMirrorAndAlpha==expected[i].diffuseMirrorAndAlpha&&
              actual.textureAdd==expected[i].textureAdd&&actual.toonMultiply==expected[i].toonMultiply,"detached material retains original values");}
    if(api.context){CheckPublic(api,publicFrame,ids,expected);CheckPublic(api,noGroupFrame,ids,withoutGroup);
        Check(api.release(api.context,publicFrame)==MMS_OK&&api.release(api.context,noGroupFrame)==MMS_OK,"public material frames survive deletion and release cleanly");}
    std::puts("Frozen mesh material scene: native material morph parity and independent lifetime passed");
}
}
#endif
