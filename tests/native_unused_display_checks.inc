// Include after native_unused_key_checks.inc. Call NativeUnusedDisplayChecks().
#include "../src/plugins/native_display_keys.hpp"
namespace {
namespace unused_display_checks {
using namespace plugins;
struct LiveState {
    unsigned char visible, ik;
    mdl::BoneOrderEntry selector;
    explicit LiveState(const ModelKeyFixture& f) : visible(f.first.m->loadComplete),
        ik(f.firstIk[0].enabled), selector(f.firstOrder[0]) {}
    bool Matches(const ModelKeyFixture& f) const noexcept {
        return f.first.m->loadComplete == visible && f.firstIk[0].enabled == ik &&
            !std::memcmp(&selector, &f.firstOrder[0], sizeof(selector));
    }
};
void Drafts(ModelKeyFixture& f, unsigned mask) {
    if (mask & 1) f.first.m->loadComplete = 0;
    if (mask & 2) f.firstIk[0].enabled = 0;
    if (mask & 4) {
        auto& parent = f.firstOrder[0]; parent.linkedModel = 1; parent.linkedBone = 0;
        parent.windowStart = 7; parent.windowEnd = 19;
    }
}
std::shared_ptr<DisplayKeyDeletion> DisplayPlan(ModelKeyFixture& f) {
    const std::vector<unsigned> selected{1}; std::shared_ptr<HistoryChange> change;
    Check(PlanDisplayKeyDeletion(*f.app, 0, change, true, false, &selected) == MMS_OK && change,
        "private unused display selection prepares without live marks");
    return std::static_pointer_cast<DisplayKeyDeletion>(change);
}
void IndependentAndCombinedDrafts() {
    for (const auto mode : {SharedHistory::RecordingMode::AllOperations, SharedHistory::RecordingMode::ExplicitOnly})
        for (unsigned mask : {1u, 2u, 4u, 7u}) {
            ModelKeyFixture f; unused_key_checks::Constant(f); Drafts(f, mask);
            Check(f.history.SetMode(mode) == MMS_OK, "private display recording mode configured");
            const LiveState draft(f); const ModelKeyState keys(f.first), peer(f.second);
            const auto ring = PoseRingDigest(f.first); const auto frame = f.app->CurrentFrame();
            auto display = DisplayPlan(f); std::vector<mdl::BoneOrderEntry> selectors;
            Check(display->TargetSelectors(true, selectors) == MMS_OK && selectors.size() == 1 &&
                !std::memcmp(selectors.data(), &draft.selector, sizeof(draft.selector)),
                "bone target lookup retains the exact first-publication draft selector and windows");
            auto change = unused_key_checks::Plan(f);
            Check(draft.Matches(f) && keys.Matches(f.first), "unused display planning preserves all live drafts and key payloads");
            ModelKeyCommit(f, change);
            Check(draft.Matches(f) && f.first.bones[0].trans[0] == 91.f && f.first.morphs[0].value == .91f,
                "unused first publication preserves independent visibility IK and parent drafts together with authored poses");
            Check(f.first.m->displayKeys[1].frame == 0 && f.first.m->boneKeys[2].frame == 0 &&
                f.first.m->morphKeys[1].frame == 0 && f.history.UndoCount() == 1,
                "draft-preserving cleanup still deletes all three interior families as one explicit node");
            Check(peer.Matches(f.second) && PoseRingDigest(f.first) == ring &&
                f.app->CurrentFrame() == frame && f.SelectionUnchanged(),
                "draft preservation does not seek change selection alter the peer or write legacy rings");
            const ModelKeyState after(f.first);
            f.app->CurrentFrame() = 20;
            Check(f.history.History(false) == MMS_OK && keys.Matches(f.first), "unused display undo restores exact source keys");
            Check(f.first.m->loadComplete == 1 && f.firstIk[0].enabled == 1 &&
                f.firstOrder[0].linkedModel == -1 && f.firstOrder[0].windowStart == 0 && f.firstOrder[0].windowEnd == 0,
                "unused display undo retains normal current-playhead registered-state evaluation");
            f.app->CurrentFrame() = 25;
            Check(f.history.History(true) == MMS_OK && after.Matches(f.first) &&
                f.first.m->loadComplete == 1 && f.firstIk[0].enabled == 1 && f.firstOrder[0].linkedModel == -1,
                "unused display redo does not reapply the first-publication draft exception");
        }
}
void BoneAndDisplayShareTarget() {
    ModelKeyFixture f; unused_key_checks::Constant(f); Drafts(f, 4);
    // This bone is not redundant. Only the display deletion supplies its
    // external-parent target to the aggregate's local bone reevaluation.
    f.first.m->boneKeys[0].position[0] = 0;
    f.first.m->boneKeys[2].position[0] = 10;
    f.first.m->boneKeys[3].position[0] = 30;
    f.first.bones[0].slotIndex = 0; f.first.dirty[0] = 0; f.app->CurrentFrame() = 20;
    const LiveState draft(f); auto change = unused_key_checks::Plan(f, 2); ModelKeyCommit(f, change);
    // The live parent window ends at frame 19, inside [10,30]. The native
    // sampler therefore snaps to the upper key at frame 20. Using the old
    // registered selector instead would produce interpolated position 20.
    Check(draft.Matches(f) && f.first.bones[0].trans[0] == 30.f,
        "bone reevaluation uses exactly the draft parent window published by the display participant");
    Check(f.history.History(false) == MMS_OK && f.firstOrder[0].linkedModel == -1 && std::fabs(f.first.bones[0].trans[0] - 20.f) < .0001f,
        "undo samples the registered parent window and source bone at the current time");
    Check(f.history.History(true) == MMS_OK && f.firstOrder[0].linkedModel == -1 && std::fabs(f.first.bones[0].trans[0] - 20.f) < .0001f,
        "redo samples the registered target rather than reviving an initial draft window");
}
void DraftValidationAndStaleCandidates() {
    ModelKeyFixture f; unused_key_checks::Constant(f); Drafts(f, 7);
    const LiveState draft(f); auto change = DisplayPlan(f);
    std::unique_ptr<PreparedHistoryChange> candidate;
    Check(change->Prepare(true, candidate) == MMS_OK && candidate->IsCurrent(),
        "private display candidate retains its additional live draft reference");
    f.first.m->loadComplete = 1;
    Check(!candidate->IsCurrent(), "live visibility changes invalidate a prepared private display candidate");
    f.first.m->loadComplete = draft.visible;
    f.firstIk[0].enabled = 1;
    Check(!candidate->IsCurrent(), "live IK changes invalidate a prepared private display candidate");
    f.firstIk[0].enabled = draft.ik;
    ++f.firstOrder[0].windowEnd;
    Check(!candidate->IsCurrent(), "live parent-window changes invalidate a prepared private display candidate");
    f.firstOrder[0] = draft.selector;
    const auto bones = f.second.m->boneCount; f.second.m->boneCount = 0;
    Check(!candidate->IsCurrent(), "draft-only external owner bounds are frozen before publication");
    f.second.m->boneCount = bones; Check(candidate->IsCurrent(), "restoring every unchanged source makes the unused candidate current");
    candidate.reset();
    for (const auto pair : {mdl::BoneReference{255,0}, mdl::BoneReference{254,0}, mdl::BoneReference{1,999}}) {
        f.firstOrder[0].linkedModel = pair.modelIndex; f.firstOrder[0].linkedBone = pair.boneIndex;
        const auto digest = f.first.Digest(); std::vector<mdl::BoneOrderEntry> out{draft.selector};
        const auto result = change->TargetSelectors(true, out);
        Check((result == MMS_E_ARGUMENT || result == MMS_E_NOT_READY) && out.size() == 1 &&
            !std::memcmp(out.data(), &draft.selector, sizeof(draft.selector)) && f.first.Digest() == digest,
            "invalid or absent draft-only parent rejects without output or live mutation");
    }
    f.firstOrder[0] = draft.selector;
}
void DraftPreparationFailures() {
    ModelKeyFixture f; unused_key_checks::Constant(f); Drafts(f, 7);
    const LiveState draft(f); const DisplayTestState keys(f.first); const auto before = f.first.Digest();
    auto change = DisplayPlan(f); std::unique_ptr<PreparedHistoryChange> candidate; bool prepared = false;
    for (long limit = 0; limit < 2048; ++limit) {
        failAfter = limit; const auto result = change->Prepare(true, candidate); failAfter = -1;
        Check(draft.Matches(f) && keys.Matches(f.first) && f.first.Digest() == before,
            "allocation failure while validating or retaining draft selectors leaves the complete source intact");
        if (result == MMS_OK) { prepared = true; break; }
        Check(result == MMS_E_MEMORY && !candidate, "private display preparation failure exposes no partial candidate");
    }
    Check(prepared && candidate && candidate->IsCurrent(), "draft-only owner preparation fault sweep reaches a complete candidate");
    failAfter = 0; candidate->Publish(); candidate.reset(); failAfter = -1;
    Check(draft.Matches(f) && f.first.m->displayKeys[1].frame == 0,
        "private display draft publication performs no allocation and still deletes its selected key");
}
} // namespace unused_display_checks
void NativeUnusedDisplayChecks() {
    unused_display_checks::IndependentAndCombinedDrafts();
    unused_display_checks::BoneAndDisplayShareTarget();
    unused_display_checks::DraftValidationAndStaleCandidates();
    unused_display_checks::DraftPreparationFailures();
}
}
