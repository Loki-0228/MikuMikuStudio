// Include after native_display_key_checks.inc. Call NativeDisplayDeleteTargetChecks().
// Reuses DisplayFixture, DisplayTestState and the native DisplayLegacyFrame oracle.
#include "../src/plugins/native_display_keys.hpp"
namespace {
namespace display_delete_target_checks {
plugins::DisplayDeletionTarget Live(const Fixture& f) {
    plugins::DisplayDeletionTarget out;out.visible=f.m->loadComplete;
    for(unsigned i=0;i<f.m->ikChainCount;++i)out.ik.push_back(f.m->ikChains[i].enabled);
    const auto* order=static_cast<const mdl::BoneOrderEntry*>(f.m->boneOrderTable);
    if(f.m->boneOrderCount)out.selectors.assign(order,order+f.m->boneOrderCount);
    return out;
}
bool SameSelectors(const std::vector<mdl::BoneOrderEntry>& a,const std::vector<mdl::BoneOrderEntry>& b) {
    return a.size()==b.size()&&(a.empty()||!std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0])));
}
bool Same(const plugins::DisplayDeletionTarget& a,const plugins::DisplayDeletionTarget& b) {
    return a.visible==b.visible&&a.ik==b.ik&&SameSelectors(a.selectors,b.selectors);
}
void Draft(Fixture& f) {
    f.m->loadComplete=81;f.m->ikChains[0].enabled=72;f.m->ikChains[1].enabled=63;
    auto* order=static_cast<mdl::BoneOrderEntry*>(f.m->boneOrderTable);
    order[0].linkedModel=1;order[0].linkedBone=1;order[0].windowStart=71;order[0].windowEnd=92;
}
void Publish(std::unique_ptr<plugins::PreparedDisplayKeyDeletion>& candidate) {
    Check(candidate&&candidate->IsCurrent(),"signed display candidate is current before publication");
    failAfter=0;candidate->Publish();failAfter=-1;
    Check(!candidate->IsCurrent(),"signed display publication is allocation-free and one-shot");
}
void DirectionsAndDrafts() {
    for(bool root:{false,true})for(bool privateSelection:{false,true}) {
        DisplayFixture f;DisplayEvaluationSeed(f);
        const std::vector<unsigned> selected{root?0u:3u};
        for(auto* fixture:{&f.first,&f.second}) {
            for(unsigned i=0;i<4;++i)fixture->m->displayKeys[i].allocated=0;
            fixture->m->displayKeys[selected[0]].allocated=3;
        }
        const DisplayTestState beforeKeys(f.first);
        DisplayLegacyFrame(f.second,0);const auto reverseZero=Live(f.second);
        DeleteMarkedModelKeys(f.second.raw(),0);
        const DisplayTestState afterKeys(f.second);
        DisplayLegacyFrame(f.second,0);const auto forwardZero=Live(f.second);
        Check(!Same(forwardZero,reverseZero),"root or positive deletion changes registered zero value or parent window in oracle");
        Draft(f.first);f.app->CurrentFrame()=0;
        std::shared_ptr<plugins::DisplayKeyDeletion> change;
        Check(plugins::PlanSignedDisplayKeyDeletion(*f.app,0,change,false,true,privateSelection?&selected:nullptr)==MMS_OK&&change,
            "typed display deletion prepares root and positive private or marked selections");
        const auto draft=Live(f.first);
        std::vector<mdl::BoneOrderEntry> ordinarySelectors;
        Check(change->TargetSelectors(true,ordinarySelectors)==MMS_OK&&
            SameSelectors(ordinarySelectors,privateSelection?draft.selectors:forwardZero.selectors),
            "ordinary selector query preserves only the existing private first-publication draft rule");
        f.app->CurrentFrame()=-17;
        const auto before=f.first.Digest(),ring=PoseRingDigest(f.first);
        const auto bones=f.first.bones;const auto dirty=f.first.dirty;
        const auto scale=bone_scale::PoseValues(f.first.raw());
        const auto maxFrame=f.first.m->maxFrame;const auto searchCursor=f.first.m->searchCursor;
        const auto boneKeys=RegistrationKeys(f.first);
        const std::vector<mdl::MorphKey> morphKeys(f.first.m->morphKeys,f.first.m->morphKeys+mdl::MorphKeyCapacity(f.first.m->morphKeys));
        std::unique_ptr<plugins::PreparedHistoryChange> ordinary;
        Check(change->Prepare(true,ordinary)==MMS_E_ARGUMENT&&!ordinary,
            "ordinary display preparation still refuses a real negative playhead");
        for(bool forward:{true,false,true}) {
            (forward?afterKeys:beforeKeys).Restore(f.second);
            DisplayLegacyFrame(f.second,0);const auto zero=Live(f.second);
            std::unique_ptr<plugins::PreparedDisplayKeyDeletion> candidate;
            const auto source=f.first.Digest();const DisplayTestState sourceKeys(f.first);
            Check(change->PrepareSigned(forward,candidate)==MMS_OK&&candidate->IsCurrent()&&Same(candidate->ZeroTarget(),zero),
                "signed display zero target matches independent native frame-zero evaluation in both directions");
            for(int frame:{0,5,25}) {
                DisplayLegacyFrame(f.second,unsigned(frame));const auto expected=Live(f.second);
                std::vector<mdl::BoneOrderEntry> target;
                Check(change->TargetSelectorsAt(forward,frame,target)==MMS_OK&&SameSelectors(target,expected.selectors),
                    "explicit registered selectors match native parent windows without live draft overrides");
            }
            Check(f.first.Digest()==source&&sourceKeys.Matches(f.first)&&Same(Live(f.first),draft)&&
                bone_scale::PoseValuesEqual(f.first.raw(),scale)&&f.app->CurrentFrame()==-17,
                "signed preparation and explicit selector queries never write live keys display state or bone scale");
            Publish(candidate);
            Check((forward?afterKeys:beforeKeys).Matches(f.first)&&Same(Live(f.first),draft)&&
                !std::memcmp(f.first.bones.data(),bones.data(),sizeof(bones))&&f.first.dirty==dirty&&
                bone_scale::PoseValuesEqual(f.first.raw(),scale)&&SameRegistrationKeys(f.first,boneKeys)&&
                !std::memcmp(f.first.m->morphKeys,morphKeys.data(),morphKeys.size()*sizeof(mdl::MorphKey))&&
                f.first.m->maxFrame==maxFrame&&f.first.m->searchCursor==searchCursor&&PoseRingDigest(f.first)==ring&&
                f.app->CurrentFrame()==-17&&!f.history.UndoCount()&&!f.history.RedoCount()&&f.SelectionUnchanged(),
                "negative display publication changes only deep key payload and marks, not state poses metadata or history");
            if(!forward)Check(f.first.Digest()==before,"signed inverse exactly restores the original native source");
        }
    }
}
void StaleParentsAndFailures() {
    DisplayFixture f;DisplayEvaluationSeed(f);
    // The selected root is the only registered reference to model 1. Deleting
    // it removes that owner from the entire target track, but not source guards.
    for(unsigned i=0;i<4;++i) {
        f.first.m->displayKeys[i].allocated=i==0;
        for(unsigned p=0;p<3;++p)f.first.m->displayKeys[i].selectorStates[p]={i==0?1:-1,0};
    }
    f.app->CurrentFrame()=-9;
    std::shared_ptr<plugins::DisplayKeyDeletion> change;
    Check(plugins::PlanSignedDisplayKeyDeletion(*f.app,0,change)==MMS_OK&&change,"plan source-only parent signed deletion");
    std::unique_ptr<plugins::PreparedDisplayKeyDeletion> candidate;
    Check(change->PrepareSigned(true,candidate)==MMS_OK&&candidate->IsCurrent(),"prepare source-only parent leases");
    auto* saved=candidate.get();const auto zero=candidate->ZeroTarget();
    const auto before=f.first.Digest();const auto live=Live(f.first);const DisplayTestState keys(f.first);
    const auto scale=bone_scale::PoseValues(f.first.raw());
    const auto stale=[&](const char* message){Check(!candidate->IsCurrent(),message);};
    f.app->ModelSlot(1)=nullptr;stale("removed source-only parent invalidates signed candidate even though target is unbound");
    Check(change->PrepareSigned(true,candidate)==MMS_E_NOT_READY&&candidate.get()==saved,
        "missing removed-key source parent refuses signed preparation without replacing output");
    f.app->ModelSlot(1)=f.second.raw();
    --f.second.m->boneCount;stale("source parent bone-table bounds change invalidates signed candidate");++f.second.m->boneCount;
    f.first.m->displayKeys[2].ikStates[1]^=1;stale("unmodified source deep IK payload remains guarded");f.first.m->displayKeys[2].ikStates[1]^=1;
    ++f.first.m->displayKeys[0].selectorStates[0].boneIndex;stale("source parent payload change invalidates signed candidate");--f.first.m->displayKeys[0].selectorStates[0].boneIndex;
    ++f.first.m->displayKeys[0].allocated;stale("source selection mark change invalidates signed candidate");--f.first.m->displayKeys[0].allocated;
    ++f.first.m->loadComplete;stale("current display visibility remains guarded at a negative playhead");--f.first.m->loadComplete;
    f.firstIk[1].enabled^=1;stale("current IK state remains guarded without pose publication");f.firstIk[1].enabled^=1;
    ++f.firstOrder[1].windowStart;stale("complete live selector state remains guarded");--f.firstOrder[1].windowStart;
    ++f.app->CurrentFrame();stale("candidate guards the actual signed playhead");--f.app->CurrentFrame();
    Check(candidate->IsCurrent()&&f.first.Digest()==before,"restoring guarded values leaves original candidate usable");
    bool succeeded=false;unsigned failures=0;
    for(long allowance=0;allowance<2048;++allowance) {
        failAfter=allowance;const auto status=change->PrepareSigned(true,candidate);failAfter=-1;
        Check(f.first.Digest()==before&&keys.Matches(f.first)&&Same(Live(f.first),live)&&
            bone_scale::PoseValuesEqual(f.first.raw(),scale)&&f.app->CurrentFrame()==-9,
            "every signed display allocation failure preserves all live native and pose state");
        if(status==MMS_OK){succeeded=true;break;}
        ++failures;
        Check(status==MMS_E_MEMORY&&candidate.get()==saved&&candidate->IsCurrent()&&Same(candidate->ZeroTarget(),zero),
            "failed signed preparation preserves previous candidate and complete zero target");
    }
    Check(succeeded&&failures>8,"signed display failure sweep reaches deep targets source references and leases");
    auto selectors=zero.selectors;const auto preserved=selectors;
    failAfter=0;const auto selectorStatus=change->TargetSelectorsAt(true,0,selectors);failAfter=-1;
    Check(selectorStatus==MMS_E_MEMORY&&SameSelectors(selectors,preserved),"explicit selector OOM preserves caller output");
    Check(change->TargetSelectorsAt(true,-1,selectors)==MMS_E_ARGUMENT&&SameSelectors(selectors,preserved),
        "explicit negative sampling frame is rejected without replacing selector output");
    // Now the deleted key's target counterpart is the only reference to model 1.
    Publish(candidate);Check(change->PrepareSigned(false,candidate)==MMS_OK&&candidate->IsCurrent(),"inverse retains target-only parent owner");
    f.app->ModelSlot(1)=nullptr;stale("target-only inverse parent identity remains guarded");f.app->ModelSlot(1)=f.second.raw();
    // Live drafts can reference an owner absent from both registered tracks.
    Publish(candidate);
    for(unsigned i=0;i<4;++i)for(unsigned p=0;p<3;++p)f.first.m->displayKeys[i].selectorStates[p]={-1,0};
    f.firstOrder[0].linkedModel=1;f.firstOrder[0].linkedBone=0;
    Check(plugins::PlanSignedDisplayKeyDeletion(*f.app,0,change)==MMS_OK&&change&&change->PrepareSigned(true,candidate)==MMS_OK,
        "signed preparation leases live draft-only parent separately from registered zero target");
    f.app->ModelSlot(1)=nullptr;stale("draft-only parent disappearance invalidates signed candidate");f.app->ModelSlot(1)=f.second.raw();
}
void NonnegativeAndRecordScope() {
    for(bool privateSelection:{false,true}) {
        DisplayFixture f;DisplayEvaluationSeed(f);Draft(f.first);Draft(f.second);f.app->CurrentFrame()=15;
        const std::vector<unsigned> selected{3};
        std::shared_ptr<plugins::DisplayKeyDeletion> signedChange;
        std::shared_ptr<plugins::HistoryChange> ordinaryChange;
        Check(plugins::PlanSignedDisplayKeyDeletion(*f.app,0,signedChange,false,false,privateSelection?&selected:nullptr)==MMS_OK&&signedChange&&
            plugins::PlanDisplayKeyDeletion(*f.app,1,ordinaryChange,false,false,privateSelection?&selected:nullptr)==MMS_OK&&ordinaryChange,
            "nonnegative signed and ordinary display plans share selection and draft policies");
        for(bool forward:{true,false,true}) {
            std::unique_ptr<plugins::PreparedDisplayKeyDeletion> signedCandidate;
            std::unique_ptr<plugins::PreparedHistoryChange> ordinaryCandidate;
            Check(signedChange->PrepareSigned(forward,signedCandidate)==MMS_OK&&ordinaryChange->Prepare(forward,ordinaryCandidate)==MMS_OK&&
                signedCandidate->IsCurrent()&&ordinaryCandidate->IsCurrent(),"nonnegative signed display candidate retains normal evaluation");
            std::vector<mdl::BoneOrderEntry> selectors;
            Check(signedChange->TargetSelectorsAt(forward,0,selectors)==MMS_OK&&SameSelectors(selectors,signedCandidate->ZeroTarget().selectors),
                "zero target remains registered even when nonnegative publication preserves first draft");
            Publish(signedCandidate);failAfter=0;ordinaryCandidate->Publish();failAfter=-1;
            Check(DisplayTestState(f.second).Matches(f.first)&&DisplayFrameEqual(f.first,f.second),
                "signed and ordinary nonnegative publication agree for initial inverse and redo including private drafts");
        }
    }
    DisplayFixture f;DisplayEvaluationSeed(f);f.app->CurrentFrame()=5;
    std::shared_ptr<plugins::HistoryChange> registration;
    Check(plugins::PlanDisplayRegistration(*f.app,0,5,registration)==MMS_OK&&registration,"create non-deletion display record for signed admission test");
    auto display=std::dynamic_pointer_cast<plugins::DisplayKeyDeletion>(registration);
    std::unique_ptr<plugins::PreparedDisplayKeyDeletion> candidate;
    std::vector<mdl::BoneOrderEntry> selectors(1);selectors[0].windowStart=83;const auto before=selectors;
    Check(display&&display->PrepareSigned(true,candidate)==MMS_E_NOT_READY&&!candidate&&
        display->TargetSelectorsAt(true,0,selectors)==MMS_E_NOT_READY&&SameSelectors(selectors,before),
        "registration cannot masquerade as a signed deletion or explicit deletion-target query");
    DisplayFixture rangeFixture;DisplayRangeSeed(rangeFixture);
    const plugins::BoneRangeScaleOptions options{10,30,1.4,false};
    auto range=DisplayRangePlan(rangeFixture,options);
    auto rangeDisplay=std::dynamic_pointer_cast<plugins::DisplayKeyDeletion>(range.change);
    Check(rangeDisplay&&rangeDisplay->PrepareSigned(true,candidate)==MMS_E_NOT_READY&&!candidate&&
        rangeDisplay->TargetSelectorsAt(true,0,selectors)==MMS_E_NOT_READY&&SameSelectors(selectors,before),
        "range edit record is excluded even though it shares the internal display implementation");
}
void SourceOnlyZeroRead() {
    for(int frame:{17,-17}) {
        DisplayFixture f;DisplayEvaluationSeed(f);
        for(auto* fixture:{&f.first,&f.second})for(unsigned i=0;i<4;++i)fixture->m->displayKeys[i].allocated=0;
        // Model 1 occurs only in a later registered payload, not the zero key
        // or live draft. The full-track lease must still protect that parent.
        for(auto* fixture:{&f.first,&f.second})for(unsigned i=0;i<4;++i)for(unsigned p=0;p<3;++p)
            fixture->m->displayKeys[i].selectorStates[p]={i==2?1:-1,0};
        DisplayLegacyFrame(f.second,0);const auto expected=Live(f.second);
        Draft(f.first);f.firstOrder[0].linkedModel=-1;
        f.first.m->boneKeys[0].allocated=1;f.app->CurrentFrame()=frame;
        const auto draft=Live(f.first);const auto before=f.first.Digest();const DisplayTestState keys(f.first);
        const auto scale=bone_scale::PoseValues(f.first.raw());
        const auto scaleAuthor=bone_scale::AuthoringState::Capture(f.first.raw());
        const auto curves=bone_curve::AuthoringState::Capture(f.first.raw());
        const auto* pool=f.first.m->displayKeys;const auto capacity=mdl::KeyPoolCapacity(pool);
        Check(!Same(draft,expected),"source-only fixture live draft differs from registered zero at a nonzero real frame");
        std::unique_ptr<plugins::PreparedDisplayKeyDeletion> candidate;
        Check(plugins::PrepareDisplayKeyZeroRead(*f.app,0,candidate)==MMS_OK&&candidate&&candidate->IsCurrent()&&
            Same(candidate->ZeroTarget(),expected),"source-only zero read works without display marks and ignores current-frame draft windows");
        const auto unchanged=[&]() {
            return f.first.Digest()==before&&keys.Matches(f.first)&&Same(Live(f.first),draft)&&
                bone_scale::PoseValuesEqual(f.first.raw(),scale)&&
                bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scaleAuthor)&&
                bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*curves)&&
                f.first.m->displayKeys==pool&&mdl::KeyPoolCapacity(pool)==capacity&&
                f.app->CurrentFrame()==frame&&!f.history.UndoCount()&&!f.history.RedoCount();
        };
        Check(unchanged(),"source-only preparation leaves keys poses metadata and authoring unchanged");
        auto* saved=candidate.get();
        failAfter=0;const auto status=plugins::PrepareDisplayKeyZeroRead(*f.app,0,candidate);failAfter=-1;
        Check(status==MMS_E_MEMORY&&candidate.get()==saved&&candidate->IsCurrent()&&unchanged(),
            "source-only OOM preserves previous read candidate and live state");
        f.first.m->displayKeys[2].ikStates[1]^=1;
        Check(!candidate->IsCurrent(),"source-only read guards deep IK payload beyond zero interval");
        f.first.m->displayKeys[2].ikStates[1]^=1;
        ++f.first.m->displayKeys[2].selectorStates[2].boneIndex;
        Check(!candidate->IsCurrent(),"source-only read guards complete deep parent payload");
        --f.first.m->displayKeys[2].selectorStates[2].boneIndex;
        f.app->ModelSlot(1)=nullptr;
        Check(!candidate->IsCurrent()&&plugins::PrepareDisplayKeyZeroRead(*f.app,0,candidate)==MMS_E_NOT_READY&&candidate.get()==saved,
            "parent used only by a later payload is guarded and required during source-only preparation");
        f.app->ModelSlot(1)=f.second.raw();
        ++f.firstOrder[0].windowStart;Check(!candidate->IsCurrent(),"source-only read guards full live selector draft");--f.firstOrder[0].windowStart;
        ++f.app->CurrentFrame();Check(!candidate->IsCurrent(),"source-only read guards real signed frame");--f.app->CurrentFrame();
        Check(candidate->IsCurrent()&&unchanged(),"restoring source dependencies restores read candidate freshness");
        Publish(candidate);
        Check(unchanged()&&Same(candidate->ZeroTarget(),expected),
            "source-only Publish consumes candidate without keys visibility IK selectors metadata authoring or bone writes");
        Check(plugins::PrepareDisplayKeyZeroRead(*f.app,0,candidate)==MMS_OK&&candidate->IsCurrent()&&unchanged(),
            "source-only dependency can be rebuilt without any edit publication");
    }
}
}
}
void NativeDisplayDeleteTargetChecks() {
    display_delete_target_checks::DirectionsAndDrafts();
    display_delete_target_checks::StaleParentsAndFailures();
    display_delete_target_checks::NonnegativeAndRecordScope();
    display_delete_target_checks::SourceOnlyZeroRead();
}
