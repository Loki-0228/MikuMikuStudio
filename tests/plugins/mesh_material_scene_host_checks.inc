#include "mikumikustudio/mesh_scene_evaluation.hpp"
#include "mikumikustudio/bone_transform_context.hpp"

#if !MMD_ORIGINAL_PROFILE
namespace mesh_material_scene_probe {
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
}
void Run(MMDApp* app) {
    wchar_t path[32768]{};const auto length=GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL",path,32768);
    Check(length&&length<32768,"material scene fixture supplied");LoadModelFile(app,path,false);
    auto* raw=app->ModelSlot(0);Check(raw&&!app->ModelSlot(1),"load actual material scene model");
    auto& model=*mdl::Mdl(raw);
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
    const auto groupWeight=model.morphKeys[0].value;model.morphKeys[0].value=0;
    std::unique_ptr<MeshSceneFrame> noGroup;
    {const mesh_definition_probe::Proof proof(app,raw);auto other=MeshSceneSource::CaptureWithMaterials(&input,1);noGroup=other->EvaluateAt(time,*context);proof.Verify();}
    SeekModelFrame(raw,0,0);ModelApplyMorphs(raw);
    const auto* noGroupAdd=mdl::MaterialMorphAdd(raw);const auto* noGroupMul=mdl::MaterialMorphMul(raw);
    std::array<MeshSceneMaterial,2> withoutGroup{};
    for(unsigned i=0;i<2;++i){Check(noGroup->MaterialAt(0,i,withoutGroup[i]),"read material without group");
        Check(Matches(withoutGroup[i],materials[i],noGroupAdd[i].channels,noGroupMul[i].channels,originalDiffuse[i]),"disabled group result matches native material");}
    model.morphKeys[0].value=groupWeight;
    SeekModelFrame(raw,0,0);ModelApplyMorphs(raw);
    const auto* add=mdl::MaterialMorphAdd(raw);const auto* mul=mdl::MaterialMorphMul(raw);
    std::array<MeshSceneMaterial,2> expected{};
    for(unsigned i=0;i<2;++i){Check(frame->MaterialAt(0,i,expected[i]),"read independent material");
        Check(Matches(expected[i],materials[i],add[i].channels,mul[i].channels,originalDiffuse[i]),"material matches native group/add/multiply and tint fields");}
    Check(expected[0].textureAdd!=withoutGroup[0].textureAdd&&expected[1].sphereMultiply!=withoutGroup[1].sphereMultiply,
        "group weight changes both additive and multiplicative material results");
    Check(add[0].channels.textureTint[0]!=0 && mul[1].channels.sphereTint[0]!=1,"actual native additive and multiplicative texture paths are active");
    ResetAppState(app);source.reset();context.reset();
    for(unsigned i=0;i<2;++i){MeshSceneMaterial actual{};Check(frame->MaterialAt(0,i,actual),"owned material survives model and source deletion");
        Check(actual.nativeDiffuse==expected[i].nativeDiffuse&&actual.diffuseMirrorAndAlpha==expected[i].diffuseMirrorAndAlpha&&
              actual.textureAdd==expected[i].textureAdd&&actual.toonMultiply==expected[i].toonMultiply,"detached material retains original values");}
    std::puts("Frozen mesh material scene: native material morph parity and independent lifetime passed");
}
}
#endif
