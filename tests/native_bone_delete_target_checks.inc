// Include after native_bone_evaluation_checks.inc and native_display_key_checks.inc.
// Detached participants only: this is not a signed model DELETE aggregate.
#include "mikumikustudio/display_motion_snapshot.hpp"
namespace bone_delete_target_checks {
using Change=plugins::BoneKeyDeletion;
using Prepared=plugins::PreparedBoneKeyDeletion;
bool CurvesMatch(PoseNumericFixture& f,PoseNumericFixture& oracle) {
    const auto a=bone_curve::AuthoringState::Capture(f.first.raw());
    const auto b=bone_curve::AuthoringState::Capture(oracle.first.raw());
    for(int bone=0;bone<2;++bone) {
        bone_curve::Tracks left,right;const bool hasLeft=a->Read(bone,left),hasRight=b->Read(bone,right);
        if(hasLeft!=hasRight)return false;if(!hasLeft)continue;
        std::vector<animation_curve::NamedTrack> l,r;
        for(unsigned lane=0;lane<left.size();++lane){l.push_back({std::to_string(lane),left[lane]});r.push_back({std::to_string(lane),right[lane]});}
        std::vector<std::uint8_t> lb,rb;std::string error;
        Check(animation_curve::Encode(l,lb,error)&&animation_curve::Encode(r,rb,error),"encode independent bone author payloads without comparing model identities");
        if(lb!=rb)return false;
    }
    return true;
}
void Seed(PoseNumericFixture& f,bool root,bool authored) {
    EvaluationSeed(f.first);
    f.first.m->boneKeys[0].position[0]=4;
    f.first.m->boneKeys[0].rotation[1]=.6f;f.first.m->boneKeys[0].rotation[3]=.8f;
    if(authored) {
        auto tracks=RegistrationFreeTracks();tracks[0].keys.front().value=4.25;
        tracks[0].keys.front().outSlope=.123456789;tracks[6].keys.front().value=1.25;
        std::string error;Check(bone_curve::Write(f.first.raw(),0,tracks,error),"bone targets seed real scale and Free authoring");
    }
    for(std::size_t i=0;i<mdl::BoneKeyCapacity(f.first.m->boneKeys);++i)f.first.m->boneKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::MorphKeyCapacity(f.first.m->morphKeys);++i)f.first.m->morphKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::KeyPoolCapacity(f.first.m->displayKeys);++i)f.first.m->displayKeys[i].allocated=0;
    const auto slot=root?0:f.first.m->boneKeys[0].next;
    Check(root||slot>=f.first.m->boneCount,"bone targets positive deletion selects a real nonzero row");
    f.first.m->boneKeys[slot].allocated=3;f.first.m->boneKeys[1].allocated=5;
}
std::shared_ptr<Change> Plan(PoseNumericFixture& f,std::shared_ptr<plugins::DisplayKeyDeletion> display={}) {
    std::shared_ptr<Change> change;
    Check(plugins::PlanSignedBoneKeyDeletion(*f.app,0,false,change,false,bool(display),display)==MMS_OK&&change,
        "typed bone deletion plans real selected keys or display-only evaluation");return change;
}
void Publish(Prepared& candidate) {
    Check(candidate.IsCurrent(),"bone target candidate is current before publication");
    failAfter=0;candidate.Publish();failAfter=-1;
    Check(!candidate.IsCurrent(),"bone target publication is one-shot and allocation-free");
}
void TargetsMatch(const Prepared& candidate,PoseNumericFixture& oracle,unsigned expectedCount=2) {
    Check(candidate.ZeroTargets().size()==expectedCount,"bone zero target count includes every affected bone even dirty ones");
    std::array<bool,2> seen{};
    for(const auto& target:candidate.ZeroTargets()) {
        Check(target.bone<2&&!seen[target.bone],"bone zero targets have unique in-range identities");seen[target.bone]=true;
        const auto& bone=oracle.first.bones[target.bone];
        Check(target.zeroPose.writePose &&
            !std::memcmp(target.zeroPose.position.data(),bone.trans,sizeof(bone.trans)) &&
            !std::memcmp(target.zeroPose.rotation.data(),bone.rotQuat,sizeof(bone.rotQuat)) &&
            target.zeroPose.scale==bone_scale::Pose(oracle.first.raw(),int(target.bone)),
            "directional frame-zero bone sample matches independent native SeekModelFrame including rotation scale and Free curves");
    }
}
void Directions() {
    for(bool root:{false,true})for(bool authored:{false,true})for(bool dirty:{false,true}) {
        PoseNumericFixture f,oracle;Seed(f,root,authored);Seed(oracle,root,authored);
        f.app->CurrentFrame()=-17;oracle.app->CurrentFrame()=0;f.first.dirty[0]=dirty?7:0;
        std::array<std::uint32_t,2> cursors{4,1};std::array<unsigned char,2> active{3,7};
        f.first.m->boneKeyCursors=cursors.data();f.first.m->boneTrackActive=active.data();
        const auto before=f.first.Digest();const auto keys=RegistrationKeys(f.first);
        const auto scales=bone_scale::AuthoringState::Capture(f.first.raw());const auto curves=bone_curve::AuthoringState::Capture(f.first.raw());
        auto change=Plan(f);std::shared_ptr<plugins::HistoryChange> normal;
        Check(plugins::PlanBoneKeyDeletion(*oracle.app,0,false,normal)==MMS_OK&&normal,"independent ordinary bone deletion plans zero-frame oracle");
        Check(f.first.Digest()==before&&scales.Equals(bone_scale::AuthoringState::Capture(f.first.raw()))&&
            curves->Equals(*bone_curve::AuthoringState::Capture(f.first.raw())),"bone target planning leaves keys pose flags scale and pristine authoring unchanged");
        std::unique_ptr<plugins::PreparedHistoryChange> rejected;
        Check(change->Prepare(true,rejected)==MMS_E_ARGUMENT&&!rejected,"ordinary bone Prepare still rejects negative evaluation frame");
        const auto live=f.first.bones;const auto flags=f.first.dirty;const auto liveScales=bone_scale::PoseValues(f.first.raw());
        const auto ring=PoseRingDigest(f.first);std::vector<mdl::BoneKey> after;
        for(bool forward:{true,false,true}) {
            const auto digest=f.first.Digest();std::unique_ptr<Prepared> candidate;
            Check(change->PrepareSigned(forward,nullptr,candidate)==MMS_OK&&candidate&&candidate->IsCurrent()&&
                f.first.Digest()==digest&&bone_scale::PoseValuesEqual(f.first.raw(),liveScales)&&f.app->CurrentFrame()==-17,
                "signed bone preparation samples frame zero without seeking the real negative playhead or mutating dirty/live pose");
            std::unique_ptr<plugins::PreparedHistoryChange> other;
            Check(normal->Prepare(forward,other)==MMS_OK&&other&&other->IsCurrent(),"ordinary oracle prepares matching deletion direction");
            other->Publish();other.reset();SeekModelFrame(oracle.first.raw(),0,0);TargetsMatch(*candidate,oracle);
            Publish(*candidate);candidate.reset();
            Check(SameRegistrationKeys(f.first,RegistrationKeys(oracle.first))&&
                bone_scale::AuthoringState::Capture(f.first.raw()).Equals(bone_scale::AuthoringState::Capture(oracle.first.raw()))&&
                CurvesMatch(f,oracle),
                "signed bone publication matches ordinary target native keys scale keys and pristine Free authoring");
            Check(!std::memcmp(live.data(),f.first.bones.data(),sizeof(live))&&flags==f.first.dirty&&
                bone_scale::PoseValuesEqual(f.first.raw(),liveScales)&&PoseRingDigest(f.first)==ring&&f.app->CurrentFrame()==-17&&
                cursors==std::array<std::uint32_t,2>{4,1}&&active==std::array<unsigned char,2>{3,7}&&
                !f.history.UndoCount()&&!f.history.RedoCount(),"negative bone publication preserves clean and dirty live poses scales physics flags ring and playback cursor");
            if(!forward)Check(SameRegistrationKeys(f.first,keys)&&scales.Equals(bone_scale::AuthoringState::Capture(f.first.raw()))&&
                curves->Equals(*bone_curve::AuthoringState::Capture(f.first.raw())),"reverse bone deletion restores exact original keys scales and Free curves");
            else if(after.empty())after=RegistrationKeys(f.first);
            else Check(SameRegistrationKeys(f.first,after),"redo bone target reproduces forward native data");
        }
    }
}
void NonnegativeParity() {
    for(int frame:{0,5,30})for(bool root:{false,true})for(bool dirty:{false,true}) {
        PoseNumericFixture f,oracle;Seed(f,root,true);Seed(oracle,root,true);
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=frame;f.first.dirty[0]=oracle.first.dirty[0]=dirty?1:0;
        auto change=Plan(f);std::shared_ptr<plugins::HistoryChange> normal;
        Check(plugins::PlanBoneKeyDeletion(*oracle.app,0,false,normal)==MMS_OK&&normal,"nonnegative ordinary bone oracle plans");
        for(bool forward:{true,false,true}) {
            std::unique_ptr<Prepared> candidate;std::unique_ptr<plugins::PreparedHistoryChange> other;
            Check(change->PrepareSigned(forward,nullptr,candidate)==MMS_OK&&normal->Prepare(forward,other)==MMS_OK&&
                candidate->IsCurrent()&&other->IsCurrent(),"signed and ordinary nonnegative bone candidates prepare");
            Publish(*candidate);other->Publish();
            Check(SameRegistrationKeys(f.first,RegistrationKeys(oracle.first))&&
                !std::memcmp(f.first.bones.data(),oracle.first.bones.data(),sizeof(f.first.bones))&&f.first.dirty==oracle.first.dirty&&
                bone_scale::PoseValues(f.first.raw())==bone_scale::PoseValues(oracle.first.raw())&&
                f.first.m->searchCursor==oracle.first.m->searchCursor&&f.first.m->maxFrame==oracle.first.m->maxFrame&&
                f.app->CurrentFrame()==frame,"nonnegative signed deletion retains ordinary pose dirty-policy keys and metadata behavior");
        }
    }
}
void Selectors() {
    {
        DisplayFixture f;Seed(f,false,false);
        f.first.m->boneOrderCount=1;f.first.m->boneOrderTable=f.firstOrder.data();f.first.bones[0].slotIndex=0;
        f.firstOrder[0].boneIndex=-1;f.firstOrder[0].windowStart=12;f.firstOrder[0].windowEnd=30;
        f.firstOrder[0].linkedModel=1;f.firstOrder[0].linkedBone=0;
        f.first.m->displayKeys[0].selectorStates[0]={-1,0};
        f.app->CurrentFrame()=-23;
        auto change=Plan(f);const auto digest=f.first.Digest();std::unique_ptr<Prepared> candidate;
        Check(change->PrepareSigned(true,nullptr,candidate)==MMS_E_ARGUMENT&&!candidate&&f.first.Digest()==digest,
            "no-display-edit signed bone deletion still rejects null zero selectors for a selector-related bone");
        // Use the existing registered-track sampler as a test oracle. The bone
        // participant consumes only the supplied vector, not any new display API.
        const auto registered=DisplayMotionSnapshot::Capture(f.first.raw());DisplaySelectorSample sample;
        Check(registered->SampleSelector(0,0,sample)==DisplayMotionStatus::Ok,
            "read registered display frame zero independently of the negative playhead and live window");
        std::vector<mdl::BoneOrderEntry> zero{f.firstOrder[0]};
        zero[0].windowStart=sample.windowStart;zero[0].windowEnd=sample.windowEnd;
        zero[0].linkedModel=sample.modelIndex;zero[0].linkedBone=sample.boneIndex;
        Check(zero[0].windowStart==0&&zero[0].windowEnd==0&&zero[0].linkedModel==-1&&
            f.firstOrder[0].windowStart==12&&f.firstOrder[0].linkedModel==1,
            "registered zero selectors are demonstrably different from the current edited selector window");
        Check(change->PrepareSigned(true,&zero,candidate)==MMS_OK&&candidate&&candidate->IsCurrent()&&
            candidate->ZeroTargets().size()==2&&f.first.Digest()==digest&&f.app->CurrentFrame()==-23,
            "no-display-edit signed bone deletion accepts explicit registered frame-zero selectors without seeking or mutation");
        const auto live=f.first.bones;const auto scale=bone_scale::PoseValues(f.first.raw());Publish(*candidate);
        Check(!std::memcmp(live.data(),f.first.bones.data(),sizeof(live))&&bone_scale::PoseValuesEqual(f.first.raw(),scale)&&
            f.firstOrder[0].windowStart==12&&f.firstOrder[0].linkedModel==1&&f.app->CurrentFrame()==-23,
            "explicit registered zero target leaves live selector windows and negative-playhead poses unchanged");
    }
    for(bool displayOnly:{false,true}) {
        DisplayFixture f;Seed(f,false,false);
        f.first.m->boneOrderCount=1;f.first.m->boneOrderTable=f.firstOrder.data();f.first.bones[0].slotIndex=0;
        f.firstOrder[0].windowStart=12;f.firstOrder[0].windowEnd=30;f.firstOrder[0].linkedModel=1;f.firstOrder[0].linkedBone=0;
        f.first.m->displayKeys[0].selectorStates[0]={1,0};f.first.m->displayKeys[0].allocated=1;
        if(displayOnly)for(std::size_t i=0;i<mdl::BoneKeyCapacity(f.first.m->boneKeys);++i)f.first.m->boneKeys[i].allocated=0;
        f.app->CurrentFrame()=0;std::shared_ptr<plugins::HistoryChange> displayBase;
        Check(plugins::PlanDisplayKeyDeletion(*f.app,0,displayBase,true,false)==MMS_OK&&displayBase,
            "selector target fixture uses an actual detached display deletion record");
        auto display=std::dynamic_pointer_cast<plugins::DisplayKeyDeletion>(displayBase);
        Check(bool(display),"real display deletion provides existing target selector API");
        std::vector<mdl::BoneOrderEntry> zero;
        Check(display->TargetSelectors(true,zero)==MMS_OK&&zero.size()==1,"obtain explicit display frame-zero target before moving test playhead");
        f.app->CurrentFrame()=-23;auto change=Plan(f,display);const auto digest=f.first.Digest();
        std::unique_ptr<Prepared> candidate;
        Check(change->PrepareSigned(true,nullptr,candidate)==MMS_E_ARGUMENT&&!candidate&&f.first.Digest()==digest,
            "signed bone participant refuses display changes without explicit frame-zero selectors");
        auto invalid=zero;invalid[0].linkedModel=254;invalid[0].linkedBone=0;
        Check(change->PrepareSigned(true,&invalid,candidate)==MMS_E_ARGUMENT&&!candidate,
            "signed zero selector target validates parent identity rather than guessing current-frame windows");
        invalid.clear();Check(change->PrepareSigned(true,&invalid,candidate)==MMS_E_ARGUMENT&&!candidate,
            "signed zero selector target rejects an incomplete selector vector");
        Check(change->PrepareSigned(true,&zero,candidate)==MMS_OK&&candidate->IsCurrent()&&
            candidate->ZeroTargets().size()==(displayOnly?1u:2u)&&f.first.Digest()==digest,
            "explicit zero selectors support mixed and display-only bone evaluation at a negative playhead");
        ++f.firstOrder[0].windowEnd;Check(!candidate->IsCurrent(),"live source selector window is independently guarded");--f.firstOrder[0].windowEnd;
        auto* parent=f.app->ModelSlot(1);f.app->ModelSlot(1)=nullptr;
        Check(!candidate->IsCurrent(),"source selector parent membership invalidates signed candidate");f.app->ModelSlot(1)=parent;
        const auto live=f.first.bones;const auto flags=f.first.dirty;const auto liveScale=bone_scale::PoseValues(f.first.raw());
        Check(candidate->IsCurrent(),"restored source selector and parent leave original frozen candidate current");
        zero[0].windowStart=999; // Prepared targets own results, not caller storage.
        Publish(*candidate);
        Check(!std::memcmp(live.data(),f.first.bones.data(),sizeof(live))&&flags==f.first.dirty&&
            bone_scale::PoseValuesEqual(f.first.raw(),liveScale)&&f.firstOrder[0].windowStart==12&&f.app->CurrentFrame()==-23,
            "bone-only signed publication does not write display windows or live bone poses");
    }
}
void FreshnessAndFailures() {
    for(unsigned mutation=0;mutation<12;++mutation) {
        PoseNumericFixture f;Seed(f,false,true);f.app->CurrentFrame()=-17;f.first.dirty[0]=1;
        auto change=Plan(f);std::unique_ptr<Prepared> candidate;
        Check(change->PrepareSigned(true,nullptr,candidate)==MMS_OK&&candidate->IsCurrent(),"signed bone stale fixture prepares");
        auto* raw=f.app->ModelSlot(0);
        switch(mutation) {
        case 0:++f.app->CurrentFrame();break;
        case 1:++f.first.dirty[0];break;
        case 2:f.first.bones[0].trans[0]+=1;break;
        case 3:Check(bone_scale::SetPose(raw,0,Scale(11)),"mutate affected dirty scale after signed preparation");break;
        case 4:++f.app->PlaybackPhysicsMode();break;
        case 5:++f.first.bones[0].physicsDisabled;break;
        case 6:++f.first.m->physicsMode;break;
        case 7:++f.first.m->boneKeys[0].position[0];break;
        case 8:++f.first.m->boneKeys[0].allocated;break;
        case 9:f.app->ModelSlot(0)=f.second.raw();break;
        case 10:{bone_curve::Tracks tracks;const auto source=bone_curve::AuthoringState::Capture(raw);
            Check(source->Read(0,tracks),"read source Free author for high precision invalidation");
            const auto keys=RegistrationKeys(f.first);tracks[0].keys.front().value+=1e-12;
            bone_curve::NativeKeyView view;view.context=raw;view.capacity=mdl::BoneKeyCapacity(mdl::Mdl(raw)->boneKeys);
            view.key=[](const void* p,std::uint32_t slot)noexcept->const mdl::BoneKey*{
                const auto* model=static_cast<const unsigned char*>(p);const auto* keys=mdl::Mdl(model)->boneKeys;
                return slot<mdl::BoneKeyCapacity(keys)?&keys[slot]:nullptr;};
            view.scale=[](const void* p,std::uint32_t slot,bone_scale::Matrix& matrix,bone_scale::Curve& curve)noexcept{
                const auto* model=static_cast<const unsigned char*>(p);
                matrix=bone_scale::Key(model,int(slot));curve=bone_scale::KeyCurve(model,int(slot));return true;};
            auto target=source->Clone();Check(target->Replace(0,tracks,view),"prepare author-only precision edit");
            auto restore=bone_curve::AuthoringRestore::Prepare(raw,*target);
            Check(restore->IsCurrent()&&restore->Publish()&&SameRegistrationKeys(f.first,keys),
                "edit high precision bone authoring while native float payload remains unchanged");break;}
        case 11:plugins::ForgetSceneObject(raw);plugins::SceneObjectIdentity(raw);break;
        }
        const auto digest=f.first.Digest();const auto scale=bone_scale::PoseValues(raw);
        Check(!candidate->IsCurrent()&&f.first.Digest()==digest&&bone_scale::PoseValuesEqual(raw,scale),
            "signed candidate guards real frame dirty pose scale physics native selection and model identity without writes");
        f.app->ModelSlot(0)=raw;
    }
    {
        PoseNumericFixture f;Seed(f,false,false);f.app->CurrentFrame()=-17;
        f.first.bones[0].hasRigidBody=1;f.first.bones[0].physicsDisabled=1;f.app->PlaybackPhysicsMode()=2;f.first.dirty[0]=1;
        auto change=Plan(f);std::unique_ptr<Prepared> candidate;const auto digest=f.first.Digest();
        Check(change->PrepareSigned(true,nullptr,candidate)==MMS_E_NOT_READY&&!candidate&&f.first.Digest()==digest,
            "dirty live pose cannot bypass signed zero-sample physics transition refusal");
    }
    {
        BoneRegistrationFixture f;Seed(f,false,false);auto registration=RegistrationPlan(f,0,15);
        Check(bool(registration),"misuse fixture plans an actual registration change");auto typed=std::dynamic_pointer_cast<Change>(registration);
        std::unique_ptr<Prepared> candidate;f.app->CurrentFrame()=-17;
        Check(!typed||typed->PrepareSigned(true,nullptr,candidate)==MMS_E_NOT_READY,
            "shared registration implementation cannot masquerade as signed deletion");
    }
    PoseNumericFixture f;Seed(f,true,true);f.app->CurrentFrame()=-17;
    auto change=Plan(f);const auto digest=f.first.Digest();const auto poses=bone_scale::PoseValues(f.first.raw());
    const auto curves=bone_curve::AuthoringState::Capture(f.first.raw());const auto scales=bone_scale::AuthoringState::Capture(f.first.raw());
    unsigned failures=0;bool ready=false;
    for(long allowance=0;allowance<4096;++allowance) {
        std::unique_ptr<Prepared> candidate;failAfter=allowance;const auto result=change->PrepareSigned(true,nullptr,candidate);failAfter=-1;
        Check(f.first.Digest()==digest&&bone_scale::PoseValuesEqual(f.first.raw(),poses)&&
            curves->Equals(*bone_curve::AuthoringState::Capture(f.first.raw()))&&scales.Equals(bone_scale::AuthoringState::Capture(f.first.raw()))&&
            f.app->CurrentFrame()==-17,"signed bone OOM and cancellation preserve native state live pose and authoring");
        if(result==MMS_OK){Check(candidate&&candidate->IsCurrent()&&candidate->ZeroTargets().size()==2,"OOM sweep eventually prepares all directional bone targets");ready=true;break;}
        Check(result==MMS_E_MEMORY&&!candidate,"signed bone OOM exposes no partial candidate");++failures;
    }
    Check(ready&&failures>3,"signed bone allocation sweep exercises multiple detached stages");
}
}
inline void NativeBoneDeleteTargetChecks() {
    std::puts("  signed bone deletion targets: explicit zero sampling and independent source guards");
    bone_delete_target_checks::Directions();bone_delete_target_checks::NonnegativeParity();
    bone_delete_target_checks::Selectors();bone_delete_target_checks::FreshnessAndFailures();
}
