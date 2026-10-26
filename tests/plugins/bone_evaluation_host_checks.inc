#include "mikumikustudio/bone_evaluation.hpp"
#include "mikumikustudio/bone_transform_context.hpp"
#include <cmath>

// Include after bone_kinematic_host_checks.inc and morph_evaluation_host_checks.inc:
// their byte snapshots cover native nested definitions, key pools and history.
#if !MMD_ORIGINAL_PROFILE
namespace bone_evaluation_probe {
namespace {
constexpr int kFrames[] = {0, 5, 10, 15, 20, 25};
using Poses = std::vector<BoneEvaluationPose>;
using Oracle = std::array<Poses, 6>;
bool Near(float a, float b) { return std::isfinite(a) && std::isfinite(b) && std::abs(a-b) <= 2e-4f * (1.f + std::abs(b)); }
template<size_t N> bool Same(const std::array<float,N>& a, const std::array<float,N>& b) {
    for (size_t i=0;i<N;++i) if (!Near(a[i],b[i])) return false; return true;
}
bool Same(const BoneEvaluationPose& a, const BoneEvaluationPose& b) {
    return Same(a.rigid,b.rigid) && Same(a.scaled,b.scaled) && Same(a.jointPosition,b.jointPosition) &&
        Same(a.attachment[0],b.attachment[0]) && Same(a.attachment[1],b.attachment[1]);
}
bool Different(const Poses& a, const Poses& b) {
    if (a.size()!=b.size()) return true;
    for (size_t i=0;i<a.size();++i) if (!Same(a[i],b[i])) return true; return false;
}
void Fresh(unsigned char* raw) {
    bone_scale::RestoreTransforms(raw);
    auto& model=*mdl::Mdl(raw);
    for (uint32_t i=0;i<model.boneCount;++i) {
        auto& b=model.boneTable[i];
        std::memset(b.matInit,0,sizeof b.matInit); std::memset(b.matLocal,0,sizeof b.matLocal);
        std::memset(b.matWorld,0,sizeof b.matWorld); std::memset(b.matExtra,0,sizeof b.matExtra);
        b.matWorld[10]=b.matWorld[15]=1;
        if (model.physicsMode==2) b.matWorld[0]=b.matWorld[5]=1;
        std::memset(b.trans,0,sizeof b.trans); std::memset(b.rotQuat,0,sizeof b.rotQuat); b.rotQuat[3]=1;
        std::memset(b.rotQuat2,0,sizeof b.rotQuat2); b.rotQuat2[3]=1;
        std::memset(b.physicsOffset,0,sizeof b.physicsOffset); std::memset(b.physicsQuat,0,sizeof b.physicsQuat);
        std::memset(b.ikBackup,0,sizeof b.ikBackup); b.physicsDisabled=0;
    }
}
Poses NativeAt(MMDApp* app, unsigned char* raw, int frame) {
    auto& model=*mdl::Mdl(raw); Fresh(raw); SeekModelFrame(raw,frame,0);
    Poses result(model.boneCount);
    for (int phase=0;phase<2;++phase) {
        ModelApplyMorphs(raw); SetPhysicsMode(raw,phase,app->ModelSlots(),0);
        for (uint32_t b=0;b<model.boneCount;++b)
            std::copy(model.boneTable[b].matExtra,model.boneTable[b].matExtra+16,result[b].attachment[phase].begin());
    }
    for (uint32_t b=0;b<model.boneCount;++b)
        std::copy(model.boneTable[b].matInit,model.boneTable[b].matInit+16,result[b].rigid.begin());
    bone_scale::ApplyTransforms(raw);
    for (uint32_t b=0;b<model.boneCount;++b) {
        const auto& bone=model.boneTable[b]; auto& out=result[b];
        std::copy(bone.matInit,bone.matInit+16,out.scaled.begin());
        for (unsigned c=0;c<3;++c) out.jointPosition[c]=bone.position[0]*bone.matInit[c]+
            bone.position[1]*bone.matInit[4+c]+bone.position[2]*bone.matInit[8+c]+bone.matInit[12+c];
    }
    return result;
}
Oracle Native(MMDApp* app, unsigned char* raw) {
    Oracle result; for (unsigned t=0;t<6;++t) result[t]=NativeAt(app,raw,kFrames[t]); return result;
}
bool FrameMatches(const BoneEvaluationFrame& frame, const Poses& wanted, double time) {
    if (frame.BoneCount()!=wanted.size() || frame.Frame()!=time || frame.Visibility()!=1) return false;
    for (uint32_t b=0;b<frame.BoneCount();++b) {
        BoneEvaluationPose pose; if (!frame.BoneAt(b,pose) || !Same(pose,wanted[b])) return false;
    }
    return true;
}
bool Matches(const BoneEvaluationSource& source,const BoneTransformContext& context,const Oracle& wanted) {
    for (unsigned t=0;t<6;++t) {
        const auto frame=source.Evaluate(kFrames[t],context);
        if (!frame || !FrameMatches(*frame,wanted[t],kFrames[t])) return false;
    }
    return true;
}
void Workers(const BoneEvaluationSource& old,const BoneEvaluationSource& newer,const BoneTransformContext& context,
    const Oracle& original,const Oracle& edited) {
    bool firstOk=true,secondOk=true;
    const auto run=[&](bool& ok) { try { for (unsigned i=0;i<6;++i)
        ok=Matches(old,context,original)&&Matches(newer,context,edited)&&ok; } catch (...) { ok=false; } };
    std::thread first([&]{run(firstOk);}),second;
    try {second=std::thread([&]{run(secondOk);});} catch (...) {first.join();throw;}
    first.join();second.join();Check(firstOk&&secondOk,"concurrent frozen bone poses match both independent native revisions");
}
struct Proof {
    bone_kinematic_probe::Proof bones;
    unsigned char* raw;
    std::vector<unsigned char> morphBytes;
    morph_curve::AuthoredLayers layers;
    Proof(MMDApp* app,unsigned char* model):bones(app,model),raw(model),morphBytes(morph_evaluation_probe::SourceBytes(*mdl::Mdl(model))) {
        std::string error;Check(morph_curve::CaptureLayers(raw,layers,error),"capture registered morph authoring proof");
    }
    void Verify() const {
        bones.Verify();Check(morphBytes==morph_evaluation_probe::SourceBytes(*mdl::Mdl(raw)) &&
            morph_curve::LayersUnchanged(raw,layers),"frozen full bone evaluation preserves morph definitions keys current values and authors");
    }
};
void Seed(unsigned char* raw) {
    auto& model=*mdl::Mdl(raw);
    Check(mdl::BoneKeyPoolEnsure(model,model.boneCount*3ull),"reserve three registered bone keys per track");
    for (uint32_t b=0;b<model.boneCount;++b) {
        const uint32_t slots[]{b,model.boneCount+b*2,model.boneCount+b*2+1};
        for (unsigned k=0;k<3;++k) {
            auto& key=model.boneKeys[slots[k]];key={};key.frame=k*10;key.allocated=1;
            key.previous=k?slots[k-1]:0;key.next=k<2?slots[k+1]:0;
            const float factor=k==1?1.f:k==2?-.25f:0.f;
            key.position[0]=(b==3?1.2f:.1f*float(b+1))*factor;
            key.position[1]=-.15f*factor;key.position[2]=.1f*factor;
            const float angle=.03f*float(b+1)*factor;key.rotation[2]=std::sin(angle);key.rotation[3]=std::cos(angle);
            for (unsigned lane=0;lane<4;++lane) {key.interpolation[lane]=key.interpolation[lane+4]=20;key.interpolation[lane+8]=key.interpolation[lane+12]=107;}
            bone_scale::SetKey(raw,int(slots[k]),{{k==1?1.3f:1.f,0,0,0,1,0,0,0,1}});
        }
    }
    Check(mdl::MorphKeyPoolEnsure(model,model.morphCount*3ull),"reserve registered morph tracks for bone evaluation");
    for (uint32_t m=0;m<model.morphCount;++m) {
        const uint32_t slots[]{m,model.morphCount+m*2,model.morphCount+m*2+1};
        for (unsigned k=0;k<3;++k) {
            auto& key=model.morphKeys[slots[k]];key={};key.frame=k*10;key.allocated=1;
            key.previous=k?slots[k-1]:0;key.next=k<2?slots[k+1]:0;
            key.value=(model.physicsMode==2||m!=0)?(k==1?.75f:0.f):0.f;
        }
    }
    auto& display=model.displayKeys[0];display.frame=display.previous=display.next=0;display.visible=1;
    for (uint32_t i=0;i<model.ikChainCount;++i) display.ikStates[i]=1;
    for (uint32_t i=0;i<model.boneOrderCount;++i) display.selectorStates[i]={-1,0};
    model.maxFrame=(std::max)(model.maxFrame,20u);
}
}
void Run(MMDApp* app) {
    wchar_t path[32768]{};const auto length=GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL",path,32768);
    Check(length&&length<32768,"bone evaluation fixture supplied");LoadModelFile(app,path,false);
    auto* raw=app->ModelSlot(0);Check(raw,"load actual frozen-bone evaluation fixture");auto& model=*mdl::Mdl(raw);
    Check(model.boneCount>=5&&model.ikChainCount==1&&model.ikChains[0].linkCount&&model.boneOrderCount>1,"loaded fixture has real IK chain and selectors");
    const auto ik=model.ikChains[0].boneIndex;
    const auto link=model.ikChains[0].links[0];
    Check(ik==3&&model.boneTable[link].type==mdl::BoneType::UnderIk,"real PMX and PMD IK link uses fresh accumulator semantics");
    int boneMorph=-1;
    for (uint32_t m=0;m<model.morphCount;++m) if (model.physicsMode==2&&model.morphs[m].type==2&&model.morphs[m].boneCount) boneMorph=int(m);
    Check(model.physicsMode!=2||boneMorph>=0,"PMX fixture includes actual parsed bone morph");Seed(raw);
    auto context=BoneTransformContext::Create();Check(bool(context),"create independent CPU mathematics context");
    const auto original=Native(app,raw);
    if (model.physicsMode==2) {
        bool afterPhysicsEvaluated=false;
        for (uint32_t b=0;b<model.boneCount;++b) if (model.boneTable[b].flags&0x1000) {
            const auto& middle=model.boneKeys[model.boneCount+b*2];
            const bool registeredMotion=middle.allocated && middle.frame==10 &&
                (middle.position[0]!=0 || middle.position[1]!=0 || middle.position[2]!=0 ||
                 middle.rotation[0]!=0 || middle.rotation[1]!=0 || middle.rotation[2]!=0);
            afterPhysicsEvaluated=afterPhysicsEvaluated || (registeredMotion &&
                !Same(original[2][b].attachment[0],original[2][b].attachment[1]));
        }
        Check(afterPhysicsEvaluated,"loaded PMX after-physics bone has registered motion and a nonempty second pose sweep");
    }
    bool scaled=false;for (const auto& pose:original[2]) scaled=scaled||!Same(pose.rigid,pose.scaled);
    Check(scaled,"registered nonidentity scale changes final native matrices");
    model.displayKeys[0].ikStates[0]=0;const auto disabled=Native(app,raw);
    Check(Different(original[2],disabled[2]),"registered IK enable byte actually changes the solved native pose");
    std::unique_ptr<BoneEvaluationSource> withoutIk;
    {const Proof proof(app,raw);withoutIk=BoneEvaluationSource::Capture(raw);Check(Matches(*withoutIk,*context,disabled),"frozen IK-disabled source matches independent native solver");proof.Verify();}
    model.displayKeys[0].ikStates[0]=1;
    if (boneMorph>=0) {
        auto& value=model.morphKeys[model.morphCount+uint32_t(boneMorph)*2].value;const auto saved=value;value=0;
        const auto absent=NativeAt(app,raw,10);value=saved;
        Check(Different(original[2],absent),"registered PMX bone morph affects final pose, not merely the mesh");
    }
    const auto changedKey=model.boneCount+uint32_t(ik)*2;const auto savedKey=model.boneKeys[changedKey];
    model.boneKeys[changedKey].position[0]+=1.75f;const auto edited=Native(app,raw);model.boneKeys[changedKey]=savedKey;
    Check(Different(original[2],edited[2]),"independent native oracle distinguishes registered IK driver revisions");
    bone_scale::RestoreTransforms(raw);
    for (uint32_t b=0;b<model.boneCount;++b) {model.boneTable[b].trans[0]=123;model.boneTable[b].rotQuat[0]=.125f;}
    app->CurrentFrame()=7;app->SceneModified()=0;
    std::unique_ptr<BoneEvaluationSource> old,newer;
    {const Proof proof(app,raw);old=BoneEvaluationSource::Capture(raw);Check(old&&Matches(*old,*context,original),"frozen evaluated pose ignores current unregistered edits");proof.Verify();}
    model.boneKeys[changedKey].position[0]+=1.75f;
    {const Proof proof(app,raw);newer=BoneEvaluationSource::Capture(raw);Workers(*old,*newer,*context,original,edited);proof.Verify();}
    model.boneKeys[changedKey]=savedKey;
    auto retainedOld=old->Evaluate(10,*context),retainedNew=newer->Evaluate(10,*context);
    Check(FrameMatches(*retainedOld,original[2],10)&&FrameMatches(*retainedNew,edited[2],10),"owned result frames preserve distinct native revisions");
    NativeAt(app,raw,0);bone_scale::RestoreTransforms(raw);app->CurrentFrame()=0;
    const auto project=std::filesystem::absolute(L"bone-evaluation.pmm");Check(SaveSceneCopy(app,project.c_str()),"save real PMM with original bone morph scale and IK tracks");
    ResetAppState(app);Check(!app->ModelSlot(0),"delete original evaluated bone source");Workers(*old,*newer,*context,original,edited);
    wcscpy_s(app->state.envFileName,project.c_str());LoadSceneFile();Check(app->ModelSlot(0),"reload source PMM into reused model slot");
    {auto* reloaded=app->ModelSlot(0);const Proof proof(app,reloaded);auto restored=BoneEvaluationSource::Capture(reloaded);
        Check(restored&&Matches(*restored,*context,original),"reloaded PMM reproduces original native two-stage and scale oracle");
        Workers(*old,*newer,*context,original,edited);proof.Verify();}
    ResetAppState(app);LoadModelFile(app,path,false);Check(app->ModelSlot(0),"reuse slot zero for a new unanimated model");
    {const Proof proof(app,app->ModelSlot(0));Workers(*old,*newer,*context,original,edited);proof.Verify();}
    ResetAppState(app);old.reset();newer.reset();withoutIk.reset();context.reset();
    bool retained=false;std::thread reader([&]{retained=FrameMatches(*retainedOld,original[2],10)&&FrameMatches(*retainedNew,edited[2],10);});reader.join();
    Check(retained,"results survive source models snapshots context and slot reuse");
    std::puts("Frozen bone evaluation: native two-phase IK/morph/scale parity, unchanged editing and independent lifetime passed");
}
}
#endif
