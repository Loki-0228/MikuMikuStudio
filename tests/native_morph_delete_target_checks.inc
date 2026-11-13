// Include after native_morph_key_checks.inc; call NativeMorphDeleteTargetChecks().
// Uses MorphKeyFixture and its independent native delete/seek oracle.
#include "../src/plugins/native_morph_keys.hpp"
namespace {
void CheckMorphDeletionZeroTargets(const plugins::PreparedMorphKeyDeletion& candidate,
    const std::array<mdl::MorphRecord,3>& expected) {
    const auto& targets=candidate.ZeroTargets();
    Check(targets.size()==2&&targets[0].morph==0&&targets[1].morph==1,
        "signed morph deletion exposes exactly the two affected targets in stable order");
    for(const auto& target:targets)Check(MorphSameFloat(target.zeroWeight,expected[target.morph].value),
        "signed morph zero target exactly matches independent production delete and seek");
}
void PublishSignedMorphDeletion(std::unique_ptr<plugins::PreparedMorphKeyDeletion>& candidate) {
    Check(candidate&&candidate->IsCurrent(),"signed morph candidate is current before publication");
    failAfter=0;candidate->Publish();failAfter=-1;
    Check(!candidate->IsCurrent(),"signed morph publication is allocation-free and one-shot");
}
void MorphDeletionTargetDirections() {
    for(bool authored:{false,true})for(bool root:{false,true}) {
        MorphKeyFixture f;MorphKeySeed(f.first);MorphKeySeed(f.second);std::string error;
        if(authored) {
            auto track=MorphAuthoredTrack(f.firstMorphs[0],0);
            // A nonzero pristine endpoint distinguishes root reset from restore.
            track.track.keys[0].value=.36;
            Check(morph_curve::Write(f.first.raw(),{track},error)&&morph_curve::Write(f.second.raw(),{track},error),
                "signed deletion seeds identical pristine curves in target and oracle");
        }
        for(auto* fixture:{&f.first,&f.second}) {
            for(std::size_t i=0;i<mdl::MorphKeyCapacity(fixture->m->morphKeys);++i)fixture->m->morphKeys[i].allocated=0;
            fixture->m->morphKeys[root?0:fixture->m->morphKeys[0].next].allocated=3;
            fixture->m->morphKeys[fixture->m->morphKeys[1].next].allocated=1;
        }
        const auto beforeKeys=MorphKeysCopy(f.first);
        SeekModelFrame(f.second.raw(),0,0);const auto reverseExpected=f.secondMorphs;
        DeleteMarkedModelKeys(f.second.raw(),0);SeekModelFrame(f.second.raw(),0,0);
        const auto forwardExpected=f.secondMorphs;const auto afterKeys=MorphKeysCopy(f.second);
        if(root)Check(!MorphSameFloat(forwardExpected[0].value,reverseExpected[0].value),
            "root deletion oracle distinguishes direction-specific zero weights");
        f.app->CurrentFrame()=-17;f.firstMorphs[0].value=4.75f;f.firstMorphs[1].value=-2.5f;
        f.first.dirty[0]=1;f.first.bones[0].trans[0]=765;
        std::array<std::uint32_t,3> cursors{7,6,2};std::array<unsigned char,3> active{1,0,1};
        f.first.m->morphKeyCursors=cursors.data();f.first.m->morphTrackActive=active.data();
        const auto before=f.first.Digest(),ring=PoseRingDigest(f.first);
        const auto weights=f.firstMorphs,oracleWeights=f.secondMorphs;
        const auto maxFrame=f.first.m->maxFrame;
        const auto searchCursor=f.first.m->searchCursor;
        const auto bones=f.first.bones;const auto dirty=f.first.dirty;const auto boneKeys=RegistrationKeys(f.first);
        morph_curve::AuthoredLayers layers;
        Check(morph_curve::CaptureLayers(f.first.raw(),layers,error),"capture untouched signed deletion author layers");
        std::shared_ptr<plugins::MorphKeyDeletion> change;
        Check(plugins::PlanSignedMorphKeyDeletion(*f.app,0,change)==MMS_OK&&change,
            "typed deletion planning accepts root and positive-only selection at a negative playhead");
        std::unique_ptr<plugins::PreparedHistoryChange> ordinary;
        Check(change->Prepare(true,ordinary)==MMS_E_ARGUMENT&&!ordinary,
            "typed deletion still rejects negative playhead through ordinary Prepare");
        auto legacy=MorphDeletePlan(f);
        Check(legacy->Prepare(true,ordinary)==MMS_E_ARGUMENT&&!ordinary,
            "ordinary deletion plan retains its existing negative-playhead gate");
        std::unique_ptr<plugins::PreparedMorphKeyDeletion> candidate;
        Check(change->PrepareSigned(true,candidate)==MMS_OK&&candidate->IsCurrent(),
            "signed deletion prepares forward target at the real negative playhead");
        CheckMorphDeletionZeroTargets(*candidate,forwardExpected);
        Check(f.first.Digest()==before&&!std::memcmp(f.firstMorphs.data(),weights.data(),sizeof(weights))&&
            morph_curve::LayersUnchanged(f.first.raw(),layers)&&f.app->CurrentFrame()==-17,
            "signed preparation changes no native data live weight or author layer");
        for(bool forward:{true,false,true}) {
            if(!forward||!candidate->IsCurrent())Check(change->PrepareSigned(forward,candidate)==MMS_OK&&candidate->IsCurrent(),
                "signed reverse and redo prepare from their actual source keys");
            CheckMorphDeletionZeroTargets(*candidate,forward?forwardExpected:reverseExpected);
            PublishSignedMorphDeletion(candidate);
            Check(MorphKeysMatch(f.first,forward?afterKeys:beforeKeys)&&
                !std::memcmp(f.firstMorphs.data(),weights.data(),sizeof(weights))&&
                !std::memcmp(f.secondMorphs.data(),oracleWeights.data(),sizeof(oracleWeights))&&
                !std::memcmp(f.first.bones.data(),bones.data(),sizeof(bones))&&f.first.dirty==dirty&&
                SameRegistrationKeys(f.first,boneKeys)&&PoseRingDigest(f.first)==ring&&
                f.first.m->maxFrame==maxFrame&&f.first.m->searchCursor==searchCursor&&f.SelectionUnchanged()&&
                cursors==std::array<std::uint32_t,3>{7,6,2}&&active==std::array<unsigned char,3>{1,0,1}&&
                morph_curve::LayersUnchanged(f.first.raw(),layers)&&f.app->CurrentFrame()==-17&&
                !f.history.UndoCount()&&!f.history.RedoCount(),
                "negative signed publication changes only target morph keys and marks, never live poses cursors layers or history");
        }
    }
}
void MorphDeletionTargetStalenessAndFailures() {
    MorphKeyFixture f;MorphKeySeed(f.first);std::string error;
    const auto track=MorphAuthoredTrack(f.firstMorphs[0],0);
    Check(morph_curve::Write(f.first.raw(),{track},error),"seed signed target freshness authoring");
    for(std::size_t i=0;i<mdl::MorphKeyCapacity(f.first.m->morphKeys);++i)f.first.m->morphKeys[i].allocated=0;
    f.first.m->morphKeys[0].allocated=1;f.app->CurrentFrame()=-5;
    std::shared_ptr<plugins::MorphKeyDeletion> change;
    Check(plugins::PlanSignedMorphKeyDeletion(*f.app,0,change)==MMS_OK&&change,"prepare typed freshness record");
    std::unique_ptr<plugins::PreparedMorphKeyDeletion> candidate;
    Check(change->PrepareSigned(true,candidate)==MMS_OK&&candidate->IsCurrent(),"prepare signed freshness candidate");
    auto* saved=candidate.get();const auto target=candidate->ZeroTargets()[0].zeroWeight;
    const auto keys=MorphKeysCopy(f.first);const auto weights=f.firstMorphs;
    const auto before=f.first.Digest();morph_curve::AuthoredLayers layers;
    Check(morph_curve::CaptureLayers(f.first.raw(),layers,error),"snapshot signed preparation source layers");
    failAfter=0;const auto allocationStatus=change->PrepareSigned(true,candidate);failAfter=-1;
    Check(allocationStatus==MMS_E_MEMORY&&candidate.get()==saved&&candidate->IsCurrent()&&
        MorphSameFloat(candidate->ZeroTargets()[0].zeroWeight,target)&&f.first.Digest()==before&&
        !std::memcmp(f.firstMorphs.data(),weights.data(),sizeof(weights))&&morph_curve::LayersUnchanged(f.first.raw(),layers),
        "failed signed preparation preserves the previous output and the entire live source");
    const auto oldRoot=f.first.m->morphKeys[0].value;f.first.m->morphKeys[0].value+=.25f;
    Check(!candidate->IsCurrent()&&change->PrepareSigned(true,candidate)==MMS_E_STALE&&candidate.get()==saved,
        "changed source key invalidates candidate and failed reprepare preserves output");
    f.first.m->morphKeys[0].value=oldRoot;
    ++f.first.m->morphKeys[0].allocated;Check(!candidate->IsCurrent(),"source selection change invalidates signed candidate");
    --f.first.m->morphKeys[0].allocated;
    ++f.app->CurrentFrame();Check(!candidate->IsCurrent(),"signed candidate guards the actual negative playhead");--f.app->CurrentFrame();
    f.firstMorphs[0].value+=1;Check(!candidate->IsCurrent(),"negative signed candidate still guards live source weight");
    f.firstMorphs[0].value=weights[0].value;
    auto edited=track;edited.track.keys[1].inSlope=.12;
    morph_curve::AuthoredLayers replacement;
    Check(morph_curve::RebuildLayers(f.first.raw(),{edited},replacement,error),"prepare tangent-only replacement for signed stale check");
    morph_curve::SwapLayers(f.first.raw(),replacement);
    Check(!candidate->IsCurrent()&&MorphKeysMatch(f.first,keys),"pristine curve-only mutation invalidates signed target without native writes");
    morph_curve::SwapLayers(f.first.raw(),replacement);
    Check(candidate->IsCurrent()&&f.first.Digest()==before&&morph_curve::LayersUnchanged(f.first.raw(),layers),
        "restoring exact source values restores signed candidate freshness");
    auto preserved=change;
    Check(plugins::PlanSignedMorphKeyDeletion(*f.app,kModelSlotCount,preserved)==MMS_E_NOT_READY&&preserved==change,
        "failed typed planning leaves caller output unchanged");
    const std::vector<unsigned> empty;
    Check(plugins::PlanSignedMorphKeyDeletion(*f.app,0,preserved,false,&empty)==MMS_OK&&!preserved,
        "empty typed private selection returns null success without touching marked keys");
}
void MorphDeletionTargetNonnegativeCompatibility() {
    for(bool privateSelection:{false,true}) {
        MorphKeyFixture f;MorphKeySeed(f.first);MorphKeySeed(f.second);
        f.app->CurrentFrame()=19;const std::vector<unsigned> selected{0,7};
        for(auto* fixture:{&f.first,&f.second})for(auto slot:selected)fixture->m->morphKeys[slot].allocated=1;
        std::shared_ptr<plugins::MorphKeyDeletion> signedChange;
        std::shared_ptr<plugins::HistoryChange> ordinaryChange;
        Check(plugins::PlanSignedMorphKeyDeletion(*f.app,0,signedChange,false,privateSelection?&selected:nullptr)==MMS_OK&&signedChange&&
            plugins::PlanMorphKeyDeletion(*f.app,1,ordinaryChange,false,privateSelection?&selected:nullptr)==MMS_OK&&ordinaryChange,
            "signed and ordinary positive plans use the same public selection options");
        std::unique_ptr<plugins::PreparedMorphKeyDeletion> signedCandidate;
        std::unique_ptr<plugins::PreparedHistoryChange> ordinaryCandidate;
        for(bool forward:{true,false,true}) {
            Check(signedChange->PrepareSigned(forward,signedCandidate)==MMS_OK&&ordinaryChange->Prepare(forward,ordinaryCandidate)==MMS_OK&&
                signedCandidate->IsCurrent()&&ordinaryCandidate->IsCurrent(),"both nonnegative preparation routes accept the same source");
            Check(signedCandidate->ZeroTargets().size()==1&&signedCandidate->ZeroTargets()[0].morph==0,
                "nonnegative signed preparation also returns zero targets");
            PublishSignedMorphDeletion(signedCandidate);
            failAfter=0;ordinaryCandidate->Publish();failAfter=-1;
            Check(MorphKeysMatch(f.first,MorphKeysCopy(f.second))&&
                !std::memcmp(f.firstMorphs.data(),f.secondMorphs.data(),sizeof(f.firstMorphs))&&f.app->CurrentFrame()==19,
                "nonnegative signed publication preserves ordinary deletion pose and private-draft semantics in both directions");
        }
    }
}
}
void NativeMorphDeleteTargetChecks() {
    MorphDeletionTargetDirections();
    MorphDeletionTargetStalenessAndFailures();
    MorphDeletionTargetNonnegativeCompatibility();
}
