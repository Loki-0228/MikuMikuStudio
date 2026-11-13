// Include after native_bone_registration_checks.inc and curve authoring checks.
// Call NativeBoneEvaluationChecks(). All scene windows belong to test fixtures.
#include "../src/plugins/native_bone_evaluation.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include <limits>
namespace {
void EvaluationSeed(Fixture& f) {
    bone_curve::Forget(f.raw());bone_scale::Forget(f.raw());
    std::memset(f.m->boneKeys,0,32*sizeof(mdl::BoneKey));
    f.m->ikChainCount=0;f.m->boneOrderCount=0;f.m->boneOrderTable=nullptr;
    f.m->maxFrame=60;f.m->searchCursor=7;f.morphs[0].value=.75f;f.m->loadComplete=3;
    for(unsigned bone=0;bone<2;++bone) {
        f.bones[bone].slotIndex=-1;f.bones[bone].type=mdl::BoneType::RotateMove;
        f.bones[bone].trans[0]=99+float(bone);f.dirty[bone]=0;
        f.m->boneKeys[bone].rotation[3]=1;
    }
    f.m->boneKeys[0].next=2;f.m->boneKeys[1].position[0]=31;
    for(unsigned slot=2;slot<5;++slot) {
        auto& key=f.m->boneKeys[slot];key.frame=slot==2?10:slot==3?30:60;
        key.previous=slot==2?0:slot-1;key.next=slot==4?0:slot+1;
        key.position[0]=float(slot*7);key.position[1]=float(4-int(slot)*3);key.position[2]=float(slot*2);
        key.rotation[1]=slot==2?.6f:slot==3?-.8f:0;
        key.rotation[3]=slot==2?.8f:slot==3?-.6f:1;
        for(unsigned lane=0;lane<4;++lane) {
            key.interpolation[lane]=static_cast<unsigned char>(12+lane*3);
            key.interpolation[lane+4]=static_cast<unsigned char>(35+slot);
            key.interpolation[lane+8]=static_cast<unsigned char>(94+lane);
            key.interpolation[lane+12]=static_cast<unsigned char>(112-slot);
        }
        bone_scale::SetKey(f.raw(),int(slot),Scale(float(slot)));
        bone_scale::SetKeyCurve(f.raw(),int(slot),{12,37,95,115});
    }
    bone_scale::SetKey(f.raw(),0,Scale(1.25f));
    bone_scale::SetPose(f.raw(),0,Scale(7));bone_scale::SetPose(f.raw(),1,Scale(9));
}
struct EvaluationSource {
    std::vector<mdl::BoneKey> keys;
    bone_scale::AuthoringState scale;
    std::unique_ptr<bone_curve::AuthoringState> curves;
    explicit EvaluationSource(Fixture& f) : keys(f.m->boneKeys,f.m->boneKeys+mdl::BoneKeyCapacity(f.m->boneKeys)),
        scale(bone_scale::AuthoringState::Capture(f.raw())),curves(bone_curve::AuthoringState::Capture(f.raw())) {}
    plugins::BoneEvaluationView View() const {
        plugins::BoneEvaluationView view;view.context=this;view.keyCapacity=keys.size();view.scale=&scale;view.curves=curves.get();
        view.key=[](const void* context,std::uint32_t slot) noexcept -> const mdl::BoneKey* {
            const auto& values=static_cast<const EvaluationSource*>(context)->keys;
            return slot<values.size()?&values[slot]:nullptr;
        };return view;
    }
};
void EvaluationPublish(std::unique_ptr<plugins::PreparedHistoryChange>& candidate) {
    Check(candidate&&candidate->IsCurrent(),"derived pose candidate is current before publish");
    failAfter=0;candidate->Publish();failAfter=-1;
    Check(!candidate->IsCurrent(),"derived pose publication is one-shot and allocates nothing");
}
void EvaluationSeekParity() {
    PoseNumericFixture f;EvaluationSeed(f.first);EvaluationSeed(f.second);
    EvaluationSource source(f.first);const std::vector<unsigned> affected{0};
    const auto ring=PoseRingDigest(f.first);const auto nativeKeys=RegistrationKeys(f.first);
    const auto otherBone=f.first.bones[1];const auto otherScale=bone_scale::Pose(f.first.raw(),1);
    for(int frame:{0,5,10,15,30,45,60,75}) {
        f.app->CurrentFrame()=frame;
        const auto before=f.first.Digest();const auto beforeScale=bone_scale::PoseValues(f.first.raw());
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),candidate)==MMS_OK&&
            f.first.Digest()==before&&bone_scale::PoseValuesEqual(f.first.raw(),beforeScale),
            "current-frame evaluation preparation never changes live native or scale pose");
        plugins::BoneEvaluationSample sampled;
        Check(plugins::SampleBoneEvaluation(f.first.bones[0],0,f.first.m->boneCount,f.app->PlaybackPhysicsMode(),
            frame,source.View(),sampled)==MMS_OK&&sampled.writePose,
            "explicit sampling covers exact interpolated and terminal frames");
        SeekModelFrame(f.second.raw(),frame,0);EvaluationPublish(candidate);
        Check(!std::memcmp(sampled.position.data(),f.first.bones[0].trans,sizeof(f.first.bones[0].trans))&&
            !std::memcmp(sampled.rotation.data(),f.first.bones[0].rotQuat,sizeof(f.first.bones[0].rotQuat))&&
            sampled.scale==bone_scale::Pose(f.first.raw(),0),
            "explicit sample and prepared publication agree with the independent native seek oracle");
        Check(!std::memcmp(f.first.bones[0].trans,f.second.bones[0].trans,sizeof(f.first.bones[0].trans))&&
            !std::memcmp(f.first.bones[0].rotQuat,f.second.bones[0].rotQuat,sizeof(f.first.bones[0].rotQuat))&&
            bone_scale::Pose(f.first.raw(),0)==bone_scale::Pose(f.second.raw(),0),
            "pure derived evaluation matches old direct seek at exact interpolated and terminal frames");
        Check(!std::memcmp(&otherBone,&f.first.bones[1],sizeof(otherBone))&&bone_scale::Pose(f.first.raw(),1)==otherScale&&
            f.first.morphs[0].value==.75f&&f.first.m->loadComplete==3&&f.SelectionUnchanged()&&
            PoseRingDigest(f.first)==ring&&SameRegistrationKeys(f.first,nativeKeys)&&f.first.dirty[0]==0,
            "local evaluation preserves other bones morph display selection dirty flags and old ring");
    }
    f.first.dirty[0]=1;f.first.bones[0].trans[0]=432;bone_scale::SetPose(f.first.raw(),0,Scale(12));
    const auto dirtyNative=f.first.Digest();const auto dirtyScale=bone_scale::PoseValues(f.first.raw());
    f.app->CurrentFrame()=13;std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),candidate)==MMS_OK,"dirty affected bone prepares a preserving candidate");
    EvaluationPublish(candidate);
    Check(f.first.Digest()==dirtyNative&&bone_scale::PoseValuesEqual(f.first.raw(),dirtyScale),
        "unregistered dirty pose and scale survive target track reevaluation");
}
void EvaluationFreeCurves() {
    PoseNumericFixture f;EvaluationSeed(f.first);EvaluationSeed(f.second);
    WriteOwnedCurve(f.first,0,15);WriteOwnedCurve(f.second,0,15);
    EvaluationSource source(f.first);const auto authored=bone_curve::AuthoringState::Capture(f.first.raw());
    const std::vector<unsigned> affected{0};f.app->CurrentFrame()=7;
    std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),candidate)==MMS_OK,"exact free curve target prepares from private snapshot");
    SeekModelFrame(f.second.raw(),7,0);EvaluationPublish(candidate);
    Check(!std::memcmp(f.first.bones[0].trans,f.second.bones[0].trans,sizeof(f.first.bones[0].trans))&&
        !std::memcmp(f.first.bones[0].rotQuat,f.second.bones[0].rotQuat,sizeof(f.first.bones[0].rotQuat))&&
        bone_scale::Pose(f.first.raw(),0)==bone_scale::Pose(f.second.raw(),0),
        "free curve sampling uses exact tracks rather than native integer mirrors");
    const auto next=source.keys[0].next;source.keys[next].position[0]=81;
    f.second.m->boneKeys[next].position[0]=81;bone_curve::Forget(f.second.raw());
    Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),candidate)==MMS_OK,
        "stale private curve native hash falls back to target native keys");
    SeekModelFrame(f.second.raw(),7,0);EvaluationPublish(candidate);
    Check(!std::memcmp(f.first.bones[0].trans,f.second.bones[0].trans,sizeof(f.first.bones[0].trans))&&
        bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*authored),
        "target hash fallback does not prune live or owned free curve authoring");
    float position[3]={1,2,3},rotation[4]={4,5,6,7};auto scale=Scale(8);
    failAfter=0;const bool sampled=source.curves->Sample(0,7,0,position,rotation,scale);failAfter=-1;
    Check(!sampled&&position[0]==1&&rotation[3]==7&&scale==Scale(8),"absent free curve match leaves all outputs intact without allocation");
    bone_curve::Forget(f.first.raw());
}
void EvaluationSelectorAndPhysics() {
    PoseNumericFixture f;EvaluationSeed(f.first);EvaluationSource source(f.first);const std::vector<unsigned> affected{0};
    mdl::BoneOrderEntry window{};window.windowStart=7;window.windowEnd=100;
    f.first.m->boneOrderTable=&window;f.first.m->boneOrderCount=1;f.first.bones[0].slotIndex=0;
    std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    for(int frame:{5,8}) {
        f.app->CurrentFrame()=frame;
        Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),candidate)==MMS_OK,"selector snap target prepares");
        EvaluationPublish(candidate);
        const auto& key=source.keys[frame<7?0:2];
        Check(!std::memcmp(f.first.bones[0].trans,key.position,sizeof(key.position))&&
            !std::memcmp(f.first.bones[0].rotQuat,key.rotation,sizeof(key.rotation)),"selector window snaps to the same side as native direct seek");
    }
    window.windowStart=70;f.app->CurrentFrame()=80;
    Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),candidate)==MMS_OK,"terminal selector reset prepares");
    EvaluationPublish(candidate);
    Check(f.first.bones[0].trans[0]==0&&f.first.bones[0].rotQuat[1]==0&&f.first.bones[0].rotQuat[3]==1,
        "terminal selector zero-fill preserves the identity quaternion convention");
    f.first.bones[0].slotIndex=-1;f.first.bones[0].hasRigidBody=1;f.app->CurrentFrame()=15;f.app->PlaybackPhysicsMode()=2;
    source.keys[2].physicsDisabled=1;const auto before=f.first.Digest();
    Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),candidate)==MMS_E_NOT_READY&&!candidate&&f.first.Digest()==before,
        "derived evaluation refuses a solver mode transition before any native mutation");
    f.app->PlaybackPhysicsMode()=3;source.keys[2].physicsDisabled=0;source.keys[3].physicsDisabled=1;f.first.bones[0].physicsDisabled=1;
    Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),candidate)==MMS_E_NOT_READY&&!candidate,
        "mode three IK arming is rejected even when its final mode byte already matches");
    f.app->PlaybackPhysicsMode()=0;
    Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),candidate)==MMS_OK,"disabled solver mode allows pure native pose sampling");
    EvaluationPublish(candidate);Check(f.first.bones[0].physicsDisabled==1,"pure evaluator never calls physics setter or edits physics mode byte");
}
void EvaluationFreshnessAndFailures() {
    PoseNumericFixture f;EvaluationSeed(f.first);EvaluationSource source(f.first);const std::vector<unsigned> affected{0,1};
    f.app->CurrentFrame()=15;std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),candidate)==MMS_OK,"freshness fixture prepares");
    ++f.app->CurrentFrame();Check(!candidate->IsCurrent(),"playhead change invalidates derived evaluation");--f.app->CurrentFrame();
    ++f.first.dirty[0];Check(!candidate->IsCurrent(),"new unregistered pose invalidates derived evaluation");--f.first.dirty[0];
    f.first.bones[0].trans[1]+=1;Check(!candidate->IsCurrent(),"pose source change invalidates derived evaluation");f.first.bones[0].trans[1]-=1;
    ++f.app->PlaybackPhysicsMode();Check(!candidate->IsCurrent(),"physics evaluation mode change invalidates derived evaluation");--f.app->PlaybackPhysicsMode();
    bone_scale::SetPose(f.first.raw(),1,Scale(10));Check(!candidate->IsCurrent(),"unrelated scale pose cannot be overwritten by a stale whole-map exchange");
    bone_scale::SetPose(f.first.raw(),1,Scale(9));Check(candidate->IsCurrent(),"freshness checks source values rather than unrelated authoring revisions");
    const auto native=f.first.Digest();const auto poses=bone_scale::PoseValues(f.first.raw());bool completed=false;unsigned failures=0;
    for(long limit=0;limit<128;++limit) {
        std::unique_ptr<plugins::PreparedHistoryChange> prepared;
        failAfter=limit;const auto status=plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),prepared);failAfter=-1;
        Check(f.first.Digest()==native&&bone_scale::PoseValuesEqual(f.first.raw(),poses),"every evaluation preparation allocation failure preserves live pose");
        if(status==MMS_OK){completed=true;EvaluationPublish(prepared);break;}
        Check(status==MMS_E_MEMORY&&!prepared,"private derived pose allocation failure reports memory");++failures;
    }
    Check(completed&&failures>=4,"derived pose allocation sweep reaches vectors and both scale maps");
    source.keys[2].next=2;
    Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),candidate)==MMS_E_ARGUMENT&&!candidate,"cyclic target track rejects before publication");
    source.keys[2].next=3;source.keys[3].frame=std::uint32_t(frame_limits::maximum)+1;
    Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),candidate)==MMS_E_ARGUMENT&&!candidate,"target frame overflow rejects before publication");
}
void EvaluationPublicationOrdering() {
    PoseNumericFixture f;EvaluationSeed(f.first);EvaluationSource source(f.first);const std::vector<unsigned> affected{0};
    source.scale.SetKey(2,Scale(6));f.app->CurrentFrame()=10;
    // Forget only the live side table: the target owns nonidentity scales.
    bone_scale::Forget(f.first.raw());std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),candidate)==MMS_E_NOT_READY&&!candidate&&!bone_scale::HasPoseStorage(f.first.raw()),
        "evaluation alone never creates live scale storage during preparation");
    auto restore=bone_scale::AuthoringRestore::Prepare(f.first.raw(),source.scale,source.keys.size());
    Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,source.View(),candidate)==MMS_OK&&restore->IsCurrent(),
        "prepared authoring storage lease supports nonidentity derived pose");
    failAfter=0;const bool restored=restore->Publish();const auto expected=bone_scale::Sample(f.first.raw(),0,10);
    const bool current=candidate->IsCurrent();if(current)candidate->Publish();failAfter=-1;
    Check(restored&&current&&bone_scale::Pose(f.first.raw(),0)==expected,
        "authoring publish may change revision before allocation-free derived pose publication");
    // Prepare copies results; its private view may go away before publication.
    f.app->CurrentFrame()=30;
    {
        EvaluationSource temporary(f.first);
        Check(plugins::PrepareBoneEvaluation(*f.app,0,affected,temporary.View(),candidate)==MMS_OK,"temporary target view prepares owned derived results");
    }
    EvaluationPublish(candidate);Check(f.first.bones[0].trans[0]==21,"derived candidate does not retain target view callbacks or contexts");
}
void EvaluationHistoryAtCurrentFrame() {
    BoneRegistrationFixture f;EvaluationSeed(f.first);EvaluationSeed(f.second);f.app->CurrentFrame()=15;
    f.first.m->boneKeys[2].allocated=1;const auto originalKeys=RegistrationKeys(f.second);
    const auto originalScale=bone_scale::AuthoringState::Capture(f.second.raw());const auto ring=PoseRingDigest(f.first);
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanBoneKeyDeletion(*f.app,0,false,change)==MMS_OK&&change,"clean-bone deletion builds a combined authoring and evaluation record");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("derived delete",change,transaction)==MMS_OK,"combined deletion prepares before touching current pose");
    Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,"combined deletion publishes authoring and derived pose without allocation");
    f.second.m->boneKeys[0].next=3;f.second.m->boneKeys[3].previous=0;bone_scale::ClearKey(f.second.raw(),2);
    SeekModelFrame(f.second.raw(),15,0);
    Check(!std::memcmp(f.first.bones[0].trans,f.second.bones[0].trans,sizeof(f.first.bones[0].trans))&&
        bone_scale::Pose(f.first.raw(),0)==bone_scale::Pose(f.second.raw(),0),"delete refreshes current clean pose from its target track");
    f.app->CurrentFrame()=25;f.app->SelectedModelSlot()=1;
    std::memcpy(f.second.m->boneKeys,originalKeys.data(),originalKeys.size()*sizeof(mdl::BoneKey));
    auto restore=bone_scale::AuthoringRestore::Prepare(f.second.raw(),originalScale,originalKeys.size());Check(restore->Publish(),"restore independent old track oracle");
    SeekModelFrame(f.second.raw(),25,0);const auto selectedBefore=f.second.Digest();
    Check(f.history.History(false)==MMS_OK&&f.second.Digest()==selectedBefore&&
        !std::memcmp(f.first.bones[0].trans,f.second.bones[0].trans,sizeof(f.first.bones[0].trans))&&
        bone_scale::Pose(f.first.raw(),0)==bone_scale::Pose(f.second.raw(),0),
        "undo resamples the edited model at the new playhead without touching the selected other model");
    f.app->CurrentFrame()=8;f.first.dirty[0]=1;f.first.bones[0].trans[0]=765;bone_scale::SetPose(f.first.raw(),0,Scale(11));
    Check(f.history.History(true)==MMS_OK&&f.first.bones[0].trans[0]==765&&bone_scale::Pose(f.first.raw(),0)==Scale(11)&&
        f.first.dirty[0]==1&&PoseRingDigest(f.first)==ring,"redo preserves an intervening unregistered pose and never uses the old ring");
}
bool SameEvaluationSample(const plugins::BoneEvaluationSample& a,const plugins::BoneEvaluationSample& b) {
    return a.position==b.position&&a.rotation==b.rotation&&a.scale==b.scale&&a.writePose==b.writePose;
}
void EvaluationExplicitTargetDirections() {
    PoseNumericFixture f;EvaluationSeed(f.first);
    EvaluationSource reverse(f.first),forward(f.first);
    forward.keys[0].position[0]=42;forward.keys[0].rotation[1]=.6f;forward.keys[0].rotation[3]=.8f;
    forward.scale.SetKey(0,Scale(4));
    const auto seekTargetZeroScale=[&](const EvaluationSource& target) {
        // Both direction fixtures have empty free authoring. Seed clears the
        // oracle's curves, then install the target native keys and scale curves.
        // Expected values come from production seek, not the detached sampler.
        EvaluationSeed(f.second);
        Check(target.keys.size()==mdl::BoneKeyCapacity(f.second.m->boneKeys),"direction oracle key capacity matches target");
        std::memcpy(f.second.m->boneKeys,target.keys.data(),target.keys.size()*sizeof(mdl::BoneKey));
        auto restore=bone_scale::AuthoringRestore::Prepare(f.second.raw(),target.scale,target.keys.size());
        Check(restore->Publish(),"install direction target scale authoring in independent oracle");
        SeekModelFrame(f.second.raw(),0,0);
        return bone_scale::Pose(f.second.raw(),0);
    };
    const auto forwardScale=seekTargetZeroScale(forward),reverseScale=seekTargetZeroScale(reverse);
    f.app->CurrentFrame()=-17;f.first.dirty[0]=1;
    f.first.bones[0].trans[0]=765;bone_scale::SetPose(f.first.raw(),0,Scale(12));
    f.first.m->boneKeys[0].position[0]=-100;bone_scale::SetKey(f.first.raw(),0,Scale(8));
    std::array<std::uint32_t,2> cursors{3,1};std::array<unsigned char,2> active{1,0};
    f.first.m->boneKeyCursors=cursors.data();f.first.m->boneTrackActive=active.data();
    const auto before=f.first.Digest(),ring=PoseRingDigest(f.first);
    const auto poses=bone_scale::PoseValues(f.first.raw());
    const auto authored=bone_scale::AuthoringState::Capture(f.first.raw());
    for(const auto* source:{&forward,&reverse,&forward}) {
        const auto& expectedScale=source==&forward?forwardScale:reverseScale;
        plugins::BoneEvaluationSample sampled;
        failAfter=0;
        const auto status=plugins::SampleBoneEvaluation(f.first.bones[0],0,f.first.m->boneCount,
            f.app->PlaybackPhysicsMode(),0,source->View(),sampled);
        failAfter=-1;
        const bool matchesTarget=status==MMS_OK&&sampled.writePose&&sampled.position[0]==(source==&forward?42.f:0.f)&&
            sampled.rotation[1]==(source==&forward?.6f:0.f)&&sampled.rotation[3]==(source==&forward?.8f:1.f)&&
            sampled.scale==expectedScale;
        if(!matchesTarget) {
            // Keep exact comparisons, including every scale matrix component.
            std::fprintf(stderr,
                "TARGET_ZERO direction=%s status=%lld writePose=%d playhead=%d sampleFrame=0 "
                "pos=[%.9g,%.9g,%.9g] quat=[%.9g,%.9g,%.9g,%.9g] "
                "scale=[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g]\n",
                source==&forward?"forward":"reverse",static_cast<long long>(status),int(sampled.writePose),int(f.app->CurrentFrame()),
                double(sampled.position[0]),double(sampled.position[1]),double(sampled.position[2]),
                double(sampled.rotation[0]),double(sampled.rotation[1]),double(sampled.rotation[2]),double(sampled.rotation[3]),
                double(sampled.scale.v[0]),double(sampled.scale.v[1]),double(sampled.scale.v[2]),
                double(sampled.scale.v[3]),double(sampled.scale.v[4]),double(sampled.scale.v[5]),
                double(sampled.scale.v[6]),double(sampled.scale.v[7]),double(sampled.scale.v[8]));
            std::fprintf(stderr,
                "TARGET_ZERO expectedPosX=%.9g expectedQuatY=%.9g expectedQuatW=%.9g "
                "expectedSeekScale=[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g]\n",
                source==&forward?42.:0.,source==&forward?double(.6f):0.,source==&forward?double(.8f):1.,
                double(expectedScale.v[0]),double(expectedScale.v[1]),double(expectedScale.v[2]),
                double(expectedScale.v[3]),double(expectedScale.v[4]),double(expectedScale.v[5]),
                double(expectedScale.v[6]),double(expectedScale.v[7]),double(expectedScale.v[8]));
            std::fflush(stderr);
        }
        Check(matchesTarget,
            "negative playhead explicit zero sample uses the forward or reverse target keys and scales");
        Check(f.app->CurrentFrame()==-17&&f.first.Digest()==before&&PoseRingDigest(f.first)==ring&&
            bone_scale::PoseValuesEqual(f.first.raw(),poses)&&
            bone_scale::AuthoringState::Capture(f.first.raw()).Equals(authored)&&
            cursors==std::array<std::uint32_t,2>{3,1}&&active==std::array<unsigned char,2>{1,0},
            "read-only target sampling preserves dirty drafts native keys cursors flags authoring and real playhead");
    }
    std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(plugins::PrepareBoneEvaluation(*f.app,0,{0},forward.View(),candidate)==MMS_E_ARGUMENT&&!candidate,
        "explicit sampler does not open the existing negative-playhead publication gate");
    f.app->CurrentFrame()=0;
    Check(plugins::PrepareBoneEvaluation(*f.app,0,{0},forward.View(),candidate)==MMS_OK,
        "existing preparation still accepts a dirty bone while keeping its draft");
    EvaluationPublish(candidate);
    Check(f.first.Digest()==before&&bone_scale::PoseValuesEqual(f.first.raw(),poses),
        "dirty draft policy remains in prepared evaluation rather than target sampling");
    // Reading target scales must not require or create live pose storage.
    bone_scale::Forget(f.first.raw());plugins::BoneEvaluationSample sampled;
    Check(plugins::SampleBoneEvaluation(f.first.bones[0],0,f.first.m->boneCount,0,0,forward.View(),sampled)==MMS_OK&&
        sampled.scale==forwardScale&&!bone_scale::HasPoseStorage(f.first.raw()),
        "target zero scale is returned without installing a live storage lease");
}
void EvaluationExplicitCurvesAndSelectors() {
    PoseNumericFixture f;EvaluationSeed(f.first);EvaluationSeed(f.second);
    auto tracks=OwnedCurveTracks(60);tracks[0].keys.front().value=33;tracks[6].keys.front().value=2;
    std::string error;
    Check(bone_curve::Write(f.first.raw(),0,tracks,error)&&bone_curve::Write(f.second.raw(),0,tracks,error),
        "prepare identical target free curves and independent seek oracle");
    EvaluationSource target(f.first);
    WriteOwnedCurve(f.first,0,120);f.app->CurrentFrame()=-9;f.first.dirty[0]=1;
    const auto before=f.first.Digest();const auto poses=bone_scale::PoseValues(f.first.raw());
    const auto curves=bone_curve::AuthoringState::Capture(f.first.raw());
    for(int frame:{0,7}) {
        plugins::BoneEvaluationSample sampled;
        Check(plugins::SampleBoneEvaluation(f.first.bones[0],0,f.first.m->boneCount,0,frame,target.View(),sampled)==MMS_OK&&sampled.writePose,
            "explicit target sampling reads owned free curves while live authoring differs");
        SeekModelFrame(f.second.raw(),frame,0);
        Check(!std::memcmp(sampled.position.data(),f.second.bones[0].trans,sizeof(f.second.bones[0].trans))&&
            !std::memcmp(sampled.rotation.data(),f.second.bones[0].rotQuat,sizeof(f.second.bones[0].rotQuat))&&
            sampled.scale==bone_scale::Pose(f.second.raw(),0),
            "owned free curve and scale result matches independent target seek");
        if(frame==0)Check(sampled.position[0]==33&&sampled.scale==Scale(2),
            "explicit zero result includes target free-curve position and scale");
        Check(f.app->CurrentFrame()==-9&&f.first.Digest()==before&&bone_scale::PoseValuesEqual(f.first.raw(),poses)&&
            bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*curves),
            "sampling neither reconciles live curves nor writes dirty live pose");
    }
    EvaluationSeed(f.first);EvaluationSource native(f.first);
    mdl::BoneOrderEntry live{},direction{};live.windowStart=3;live.windowEnd=100;
    direction.windowStart=7;direction.windowEnd=100;
    f.first.m->boneOrderTable=&live;f.first.m->boneOrderCount=1;f.first.bones[0].slotIndex=0;
    auto view=native.View();view.selectors=&direction;view.selectorCount=1;
    plugins::BoneEvaluationSample sampled;
    Check(plugins::SampleBoneEvaluation(f.first.bones[0],0,f.first.m->boneCount,0,5,view,sampled)==MMS_OK&&
        sampled.position[0]==native.keys[0].position[0],
        "explicit selector window comes from direction view rather than the live window");
    direction.windowStart=3;
    Check(plugins::SampleBoneEvaluation(f.first.bones[0],0,f.first.m->boneCount,0,5,view,sampled)==MMS_OK&&
        sampled.position[0]==native.keys[2].position[0],"opposite selector view changes snap direction");
    const auto saved=sampled;
    Check(plugins::SampleBoneEvaluation(f.first.bones[0],0,f.first.m->boneCount,0,5,native.View(),sampled)==MMS_E_ARGUMENT&&
        SameEvaluationSample(saved,sampled),"explicit selector sampling has no implicit live fallback on missing view");
    direction.windowStart=7;f.app->CurrentFrame()=5;
    std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(plugins::PrepareBoneEvaluation(*f.app,0,{0},view,candidate)==MMS_OK,"target selector prepares with live source guard");
    ++live.windowStart;Check(!candidate->IsCurrent(),"live selector change still invalidates a target-selector candidate");--live.windowStart;
    EvaluationPublish(candidate);
    Check(f.first.bones[0].trans[0]==native.keys[0].position[0]&&live.windowStart==3,
        "prepared evaluation samples target selector without publishing that selector");
    bone_curve::Forget(f.second.raw());
}
void EvaluationExplicitRejections() {
    PoseNumericFixture f;EvaluationSeed(f.first);EvaluationSource source(f.first);f.app->CurrentFrame()=-5;
    plugins::BoneEvaluationSample sampled;sampled.position={17,18,19};sampled.scale=Scale(6);sampled.writePose=true;
    const auto saved=sampled;const auto before=f.first.Digest();const auto poses=bone_scale::PoseValues(f.first.raw());
    const auto reject=[&](int frame,MmsResult expected,const char* label) {
        Check(plugins::SampleBoneEvaluation(f.first.bones[0],0,f.first.m->boneCount,
            f.app->PlaybackPhysicsMode(),frame,source.View(),sampled)==expected&&SameEvaluationSample(saved,sampled),label);
    };
    reject(-1,MMS_E_ARGUMENT,"explicit negative sample frame is rejected without overwriting output");
    source.keys[2].next=2;reject(0,MMS_E_ARGUMENT,"zero sampling validates the complete target chain including later cycles");
    source.keys[2].next=3;const auto frame=source.keys[3].frame;
    source.keys[3].frame=std::uint32_t(frame_limits::maximum)+1;
    reject(0,MMS_E_ARGUMENT,"out-of-range target key is rejected even when sampling zero");source.keys[3].frame=frame;
    source.keys[0].position[0]=std::numeric_limits<float>::quiet_NaN();
    reject(0,MMS_E_ARGUMENT,"nonfinite target pose is rejected with output intact");source.keys[0].position[0]=0;
    Check(f.app->CurrentFrame()==-5&&f.first.Digest()==before&&bone_scale::PoseValuesEqual(f.first.raw(),poses),
        "invalid explicit sampling leaves real model and playhead untouched");
    f.first.bones[0].hasRigidBody=1;f.app->PlaybackPhysicsMode()=2;source.keys[0].physicsDisabled=1;
    const auto physicsBefore=f.first.Digest();reject(0,MMS_E_NOT_READY,"explicit zero sampling retains solver transition refusal");
    source.keys[0].physicsDisabled=0;source.keys[2].physicsDisabled=1;f.app->PlaybackPhysicsMode()=3;
    reject(0,MMS_E_NOT_READY,"explicit target sampling retains mode-three IK arming refusal");
    Check(f.first.Digest()==physicsBefore&&f.app->CurrentFrame()==-5,
        "physics rejection has no setter or live pose side effect");
    // A non-native type without a matching free curve supplies scale only.
    auto metadata=f.first.bones[0];metadata.type=static_cast<mdl::BoneType>(127);
    // Build an independent native seek oracle with the same target key/scale
    // track and empty free curves. Solver mode zero leaves the scale comparison
    // independent of the physics refusals exercised above.
    EvaluationSeed(f.second);
    Check(source.keys.size()==mdl::BoneKeyCapacity(f.second.m->boneKeys),"scale-only oracle key capacity matches target");
    std::memcpy(f.second.m->boneKeys,source.keys.data(),source.keys.size()*sizeof(mdl::BoneKey));
    auto restore=bone_scale::AuthoringRestore::Prepare(f.second.raw(),source.scale,source.keys.size());
    Check(restore->Publish(),"install scale-only target authoring in independent oracle");
    SeekModelFrame(f.second.raw(),0,0);
    const auto expectedScale=bone_scale::Pose(f.second.raw(),0);
    Check(plugins::SampleBoneEvaluation(metadata,0,f.first.m->boneCount,0,0,source.View(),sampled)==MMS_OK&&
        !sampled.writePose&&sampled.scale==expectedScale&&sampled.position[0]==metadata.trans[0],
        "scale-only sampling preserves the non-native fallback pose and writePose contract");
}
}
void NativeBoneEvaluationChecks() {
    EvaluationSeekParity();EvaluationFreeCurves();EvaluationSelectorAndPhysics();
    EvaluationFreshnessAndFailures();EvaluationPublicationOrdering();EvaluationHistoryAtCurrentFrame();
    EvaluationExplicitTargetDirections();EvaluationExplicitCurvesAndSelectors();EvaluationExplicitRejections();
}
