// Include after native_bone_registration_checks.inc. This uses the production
// private planner and modal session; resource-backed dialog UI is checked by
// the isolated real-host harness, not represented by these CPU assertions.
#include "../src/plugins/native_center_offset.hpp"
#include <limits>
namespace center_offset_checks {
using Mode = plugins::SharedHistory::RecordingMode;
struct Fixture : BoneRegistrationFixture {
    Fixture() {
        for (auto* source : {&first, &second}) {
            SeedRegistrationChain(*source);
            source->bones[0].type = source->bones[1].type = mdl::BoneType::RotateMove;
            source->bones[0].parent = -1; source->bones[1].parent = 0;
            source->m->boneKeys[0].allocated = 3; source->m->boneKeys[1].allocated = 4;
            source->m->boneKeys[2].allocated = 5; source->m->boneKeys[3].allocated = 0;
        }
        app->CurrentFrame() = 17; app->PhysicsResetPending() = 0;
    }
    MmsResult Apply(const std::array<float, 3>& offset) {
        plugins::CenterOffsetEdit edit;
        const auto result = edit.Begin(*app, history);
        return result == MMS_OK ? edit.Apply(offset) : result;
    }
};
std::shared_ptr<plugins::HistoryChange> Plan(Fixture& fixture, const std::array<float, 3>& offset, unsigned slot = 0) {
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanBoneCenterOffset(*fixture.app, slot, offset, change) == MMS_OK, "center-offset private plan succeeds");
    return change;
}
void Commit(Fixture& fixture, const std::shared_ptr<plugins::HistoryChange>& change) {
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(change && fixture.history.PrepareNative("center-offset test", change, transaction,
        plugins::SharedHistory::NativeIntent::ExplicitCommit) == MMS_OK, "prepare one explicit center-offset transaction");
    Check(CommitRegistrationNoAllocation(fixture.history, std::move(transaction)) == MMS_OK,
        "center-offset publication changes prepared keys and authoring without allocating");
}
// Independent legacy traversal: unlike the planner's checked owner map, the
// original command resolves each marked pool slot through its previous chain.
void LegacyOracle(::Fixture& source, const std::array<float, 3>& offset) {
    for (std::size_t slot = 0; slot < mdl::BoneKeyCapacity(source.m->boneKeys); ++slot) {
        auto& key = source.m->boneKeys[slot]; if (!key.allocated) continue;
        auto bone = slot;
        while (bone >= source.m->boneCount) bone = source.m->boneKeys[bone].previous;
        if (source.bones[bone].parent != -1) continue;
        for (unsigned axis = 0; axis < offset.size(); ++axis) key.position[axis] += offset[axis];
    }
}
std::vector<std::uint8_t> Encode(const bone_curve::Tracks& tracks) {
    std::vector<animation_curve::NamedTrack> named;
    for (unsigned lane = 0; lane < tracks.size(); ++lane) named.push_back({std::to_string(lane), tracks[lane]});
    std::vector<std::uint8_t> bytes; std::string error;
    Check(animation_curve::Encode(named, bytes, error), "encode exact center-offset free-author tracks"); return bytes;
}
void ParityAndHistory() {
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) {
        Fixture fixture;
        Check(!plugins::TryEditBoundCenterOffset(fixture.app.get()), "unbound center-offset route leaves the original command owner untouched");
        plugins::NativePoseTestBinding binding(*fixture.app, fixture.history);
        Check(fixture.history.SetMode(mode) == MMS_OK, "select center-offset recording policy");
        const std::array<float, 3> offset{1.1234567f, -2.7654321f, .125f};
        auto before = RegistrationKeys(fixture.first); const auto ring = PoseRingDigest(fixture.first);
        const auto scale = bone_scale::AuthoringState::Capture(fixture.first.raw());
        const auto oldMaximum = fixture.first.m->maxFrame; const auto oldCursor = fixture.first.m->searchCursor;
        const auto oldLast = fixture.app->state.lastRegisteredFrame;
        const auto digest = fixture.first.Digest(); auto change = Plan(fixture, offset);
        Check(fixture.first.Digest() == digest && SameRegistrationKeys(fixture.first, before),
            "center-offset planning does not write keys pose selection old rings or metadata");
        Commit(fixture, change); LegacyOracle(fixture.second, offset);
        Check(SameRegistrationKeys(fixture.first, RegistrationKeys(fixture.second)),
            "native root head and selected nonzero keys match legacy float arithmetic while nonroot and unmarked keys remain exact");
        auto after = RegistrationKeys(fixture.first);
        Check(fixture.history.UndoCount() == 1 && PoseRingDigest(fixture.first) == ring &&
            bone_scale::AuthoringState::Capture(fixture.first.raw()).Equals(scale) &&
            fixture.first.m->maxFrame == oldMaximum && fixture.first.m->searchCursor == oldCursor &&
            fixture.app->state.lastRegisteredFrame == oldLast && fixture.app->CurrentFrame() == 17,
            "center offset is one explicit step in either policy and retains scale metadata and playhead");
        fixture.app->SelectedModelSlot() = 1; fixture.app->CameraMode() = 1; fixture.app->CurrentFrame() = 61;
        for (unsigned slot : {0u, 1u, 2u, 3u})
            before[slot].allocated = after[slot].allocated = fixture.first.m->boneKeys[slot].allocated = static_cast<unsigned char>(slot + 7);
        Check(fixture.history.History(false) == MMS_OK && SameRegistrationKeys(fixture.first, before),
            "center-offset undo restores original model keys at another frame without replacing later selection");
        Check(fixture.history.History(true) == MMS_OK && SameRegistrationKeys(fixture.first, after) &&
            fixture.app->CurrentFrame() == 61 && PoseRingDigest(fixture.first) == ring,
            "center-offset redo owns its original payload and ignores current selected model and legacy rings");
    }
}
void SparseAuthors() {
    Fixture fixture; plugins::NativePoseTestBinding binding(*fixture.app, fixture.history); std::string error;
    auto tracks = RegistrationFreeTracks();
    auto middle = tracks[0].keys.front(); middle.frame = 10; middle.value = 5.123456789;
    tracks[0].keys.insert(tracks[0].keys.begin() + 1, middle);
    Check(bone_curve::Write(fixture.first.raw(), 0, tracks, error) &&
        bone_curve::Write(fixture.first.raw(), 1, RegistrationFreeTracks(), error), "seed sparse root and independent child free curves");
    for (std::size_t index = 0; index < mdl::BoneKeyCapacity(fixture.first.m->boneKeys); ++index)
        fixture.first.m->boneKeys[index].allocated = 0;
    unsigned selected = fixture.first.m->boneKeys[0].next;
    Check(selected >= fixture.first.m->boneCount && fixture.first.m->boneKeys[selected].frame == 10, "sparse root mirror owns frame ten");
    fixture.first.m->boneKeys[selected].allocated = 3;
    const auto before = bone_curve::AuthoringState::Capture(fixture.first.raw());
    const auto scale = bone_scale::AuthoringState::Capture(fixture.first.raw()); const auto original = RegistrationKeys(fixture.first);
    bone_curve::Tracks child; Check(bone_curve::ReadDetached(fixture.first.raw(), 1, child), "read independent child author without publishing");
    const auto childBytes = Encode(child);
    Commit(fixture, Plan(fixture, {2, 0, 0}));
    bone_curve::Tracks after, childAfter;
    Check(bone_curve::ReadDetached(fixture.first.raw(), 0, after) && bone_curve::ReadDetached(fixture.first.raw(), 1, childAfter),
        "root and child authors remain valid after native center offset");
    Check(after[0].keys.size() == 3 && after[0].keys[1].frame == 10 &&
        float(after[0].keys[1].value) == fixture.first.m->boneKeys[selected].position[0] && Encode(childAfter) == childBytes,
        "root position edits reconcile the matching free key while retaining the untouched child author");
    for (unsigned lane = 1; lane < after.size(); ++lane)
        Check(after[lane].keys.size() == tracks[lane].keys.size(), "unchanged sparse lanes do not acquire implicit registration keys");
    const auto target = bone_curve::AuthoringState::Capture(fixture.first.raw());
    Check(bone_scale::AuthoringState::Capture(fixture.first.raw()).Equals(scale) &&
        after[0].keys.front().outWeight == tracks[0].keys.front().outWeight &&
        after[0].keys.back().inWeight == tracks[0].keys.back().inWeight,
        "center offset retains native scale keys and free endpoint handles");
    fixture.app->CurrentFrame() = 83;
    Check(fixture.history.History(false) == MMS_OK && SameRegistrationKeys(fixture.first, original) &&
        bone_curve::AuthoringState::Capture(fixture.first.raw())->Equals(*before), "undo restores exact sparse authors and their native baselines");
    Check(fixture.history.History(true) == MMS_OK && bone_curve::AuthoringState::Capture(fixture.first.raw())->Equals(*target),
        "redo restores its owned reconciled author data without reading an input file");
}
void NoopCancelAndBudget() {
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) {
        Fixture fixture; plugins::NativePoseTestBinding binding(*fixture.app, fixture.history);
        Check(fixture.history.SetMode(mode) == MMS_OK, "select center-offset no-op policy");
        Commit(fixture, Plan(fixture, {1, 2, 3})); Check(fixture.history.History(false) == MMS_OK, "seed center-offset redo branch");
        fixture.first.m->boneKeys[0].position[0] = -0.f;
        auto before = fixture.first.Digest(); fixture.app->SceneModified() = fixture.app->RecordedFrameCountByte1() = 0;
        Check(fixture.Apply({-0.f, 0.f, -0.f}) == MMS_OK && fixture.first.Digest() == before &&
            fixture.history.RedoCount() == 1 && !fixture.app->SceneModified(), "signed zero offset preserves exact floating bits redo and dirty hints");
        { plugins::CenterOffsetEdit edit; Check(edit.Begin(*fixture.app, fixture.history) == MMS_OK, "capture cancellable center-offset input"); }
        Check(fixture.first.Digest() == before && fixture.history.RedoCount() == 1, "destroying modal input cancels without touching authoring or history");
        for (std::size_t index = 0; index < mdl::BoneKeyCapacity(fixture.first.m->boneKeys); ++index) fixture.first.m->boneKeys[index].allocated = 0;
        fixture.first.m->boneKeys[1].allocated = 1; before = fixture.first.Digest();
        Check(fixture.Apply({1, 2, 3}) == MMS_OK && fixture.first.Digest() == before && fixture.history.RedoCount() == 1,
            "marked nonroot keys alone leave redo and all original fields intact");
        for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            Check(fixture.Apply({bad, 0, 0}) == MMS_E_ARGUMENT && fixture.first.Digest() == before && fixture.history.RedoCount() == 1,
                "nonfinite offset input refuses before any authored writes or redo trimming");
        }
        fixture.first.m->boneKeys[0].allocated = 1; fixture.first.m->boneKeys[0].position[0] = (std::numeric_limits<float>::max)();
        before = fixture.first.Digest();
        Check(fixture.Apply({(std::numeric_limits<float>::max)(), 0, 0}) == MMS_E_ARGUMENT && fixture.first.Digest() == before,
            "overflow of a selected root addition refuses the entire candidate");
        Check(fixture.history.Configure(mode, 32, 1) == MMS_OK, "configure a deliberate one-byte center-offset budget");
        fixture.first.m->boneKeys[0].position[0] = 0; before = fixture.first.Digest();
        Check(fixture.Apply({1, 2, 3}) == MMS_E_MEMORY && fixture.first.Digest() == before && !fixture.history.UndoCount(),
            "budget refusal keeps all selected key payloads and author state unchanged in either policy");
    }
}
void StaleInput() {
    for (unsigned why = 0; why < 9; ++why) {
        Fixture fixture; plugins::NativePoseTestBinding binding(*fixture.app, fixture.history); plugins::CenterOffsetEdit edit;
        Check(edit.Begin(*fixture.app, fixture.history) == MMS_OK, "capture center-offset modal source and journal identity");
        if (why == 0) fixture.app->SelectedModelSlot() = 1;
        if (why == 1) ++fixture.app->CurrentFrame();
        if (why == 2) fixture.first.m->boneKeys[2].allocated ^= 1;
        if (why == 3) fixture.first.m->boneKeys[2].position[1] = 97;
        if (why == 4) fixture.first.bones[0].parent = 1;
        if (why == 5) Check(fixture.history.Reset() == MMS_OK, "replace journal revision during center-offset input");
        if (why == 6) fixture.first.m->boneTable = nullptr;
        if (why == 7) { plugins::ForgetSceneObject(fixture.first.raw()); plugins::SceneObjectIdentity(fixture.first.raw()); }
        if (why == 8) bone_scale::SetKey(fixture.first.raw(), 2, Scale(9));
        const auto before = fixture.first.Digest(); const auto count = fixture.history.UndoCount();
        Check(edit.Apply({1, 2, 3}) == MMS_E_STALE && fixture.first.Digest() == before && fixture.history.UndoCount() == count &&
            edit.Apply({1, 2, 3}) == MMS_E_NOT_READY, "post-modal identity frame selection parent key scale or journal changes reject and consume the session");
        if (why == 6) fixture.first.m->boneTable = fixture.first.bones.data();
    }
    Fixture fixture; plugins::NativePoseTestBinding binding(*fixture.app, fixture.history); plugins::CenterOffsetEdit edit;
    Check(edit.Begin(*fixture.app, fixture.history) == MMS_OK, "capture center-offset binding generation");
    plugins::StorageStore storage; plugins::SharedHistory replacement(storage);
    { plugins::NativePoseTestBinding rebound(*fixture.app, replacement);
      Check(edit.Apply({1, 2, 3}) == MMS_E_STALE && !replacement.UndoCount(), "center-offset input never writes into a replacement binding"); }
}
void FailuresAndPluginOrder() {
    for (bool duringModal : {false, true}) {
        Fixture pending; plugins::NativePoseTestBinding binding(*pending.app, pending.history);
        plugins::CenterOffsetEdit modal;
        if (duringModal) Check(modal.Begin(*pending.app, pending.history) == MMS_OK,
            "open center-offset input before another native candidate is prepared");
        auto planned = Plan(pending, {1, 2, 3});
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(pending.history.PrepareNative("reserved native center-offset candidate", planned, transaction) == MMS_OK,
            "reserve a competing native transaction");
        const auto before = pending.first.Digest();
        Check((duringModal ? modal.Apply({4, 5, 6}) : modal.Begin(*pending.app, pending.history)) == MMS_E_BUSY &&
            pending.first.Digest() == before && !pending.history.UndoCount(),
            "pending native transaction excludes modal entry and post-modal center-offset application");
        Check(CommitRegistrationNoAllocation(pending.history, std::move(transaction)) == MMS_OK &&
            pending.history.UndoCount() == 1,
            "rejected modal operation leaves the prior prepared native transaction usable");
    }
    Fixture fixture; plugins::NativePoseTestBinding binding(*fixture.app, fixture.history);
    Commit(fixture, Plan(fixture, {1, 2, 3})); Check(fixture.history.History(false) == MMS_OK, "seed redo before center-offset allocation sweep");
    const auto before = fixture.first.Digest(); unsigned failures = 0; bool complete = false;
    for (long limit = 0; limit < 2048; ++limit) {
        plugins::CenterOffsetEdit edit;
        failAfter = limit; auto result = edit.Begin(*fixture.app, fixture.history);
        if (result == MMS_OK) result = edit.Apply({1, 2, 3}); failAfter = -1;
        if (result == MMS_OK && fixture.history.UndoCount()) { complete = true; break; }
        ++failures;
        Check(result == MMS_E_MEMORY && fixture.first.Digest() == before && fixture.history.RedoCount() == 1,
            "allocation failure during modal capture planning or candidate preparation leaves original keys authoring and redo intact");
    }
    Check(complete && failures > 10 && fixture.history.UndoCount() == 1, "allocation sweep reaches full center-offset publication exactly once");
    Check(fixture.history.History(false) == MMS_OK, "undo complete center offset before plugin interleave");
    auto firstBefore = RegistrationKeys(fixture.first), secondBefore = RegistrationKeys(fixture.second);
    plugins::EditStore edits(fixture.history, fixture.storage); plugins::EditClient client{&edits, "test.center-offset", false};
    auto api = edits.Api(client); std::uint64_t revision = 0; fixture.history.Revision(revision); MmsHandle token = 0;
    const char label[] = "plugin root offset"; auto change = Plan(fixture, {4, 5, 6}, 1);
    Check(api.begin(api.context, {label, sizeof(label) - 1}, revision, &token) == MMS_OK &&
        edits.JoinNative(client, token, change) == MMS_OK && api.prepare(api.context, token) == MMS_OK,
        "plugin edit API prepares an offset participant on another model");
    const auto busy = fixture.first.Digest(); plugins::CenterOffsetEdit blocked;
    Check(blocked.Begin(*fixture.app, fixture.history) == MMS_E_BUSY && fixture.first.Digest() == busy,
        "prepared plugin transaction excludes competing menu edits before modal input");
    Check(api.commit(api.context, token) == MMS_OK && fixture.Apply({1, 2, 3}) == MMS_OK && fixture.history.UndoCount() == 2,
        "plugin edit and native modal offset append to the same journal");
    fixture.app->SelectedModelSlot() = 1; fixture.app->CameraMode() = 1; fixture.app->CurrentFrame() = 77;
    Check(api.undo(api.context) == MMS_OK && SameRegistrationKeys(fixture.first, firstBefore) &&
        api.undo(api.context) == MMS_OK && SameRegistrationKeys(fixture.second, secondBefore),
        "plugin undo traverses the latest native offset before its earlier other-model transaction");
    Check(api.redo(api.context) == MMS_OK && api.redo(api.context) == MMS_OK && fixture.app->CurrentFrame() == 77,
        "plugin redo traverses both owned offset payloads without moving the playhead");
    edits.Retire(client);
    Fixture stale; plugins::NativePoseTestBinding other(*stale.app, stale.history); auto plan = Plan(stale, {1, 2, 3});
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(stale.history.PrepareNative("center-offset stale candidate", plan, transaction) == MMS_OK, "prepare complete offset before a conflicting native write");
    stale.first.m->boneKeys[2].position[0] = 125; const auto foreign = stale.first.Digest();
    Check(stale.history.CommitNative(std::move(transaction)) == MMS_E_STALE && stale.first.Digest() == foreign && !stale.history.UndoCount(),
        "false candidate validation cannot publish any partial center offset over foreign state");
}
void ParentDependencies() {
    for (bool prepared : {false, true}) {
        Fixture fixture; plugins::NativePoseTestBinding binding(*fixture.app, fixture.history);
        auto change = Plan(fixture, {1, 2, 3});
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        if (prepared) Check(fixture.history.PrepareNative("center-offset parent candidate", change, transaction) == MMS_OK,
            "prepare root-parent dependencies before a topology conflict");
        fixture.first.bones[0].parent = 1; const auto foreign = fixture.first.Digest();
        if (prepared) {
            Check(fixture.history.CommitNative(std::move(transaction)) == MMS_E_STALE,
                "root-parent changes after Prepare reject before publishing any key participant");
        } else {
            Check(fixture.history.PrepareNative("center-offset stale parent plan", change, transaction) == MMS_E_STALE && !transaction,
                "root-parent changes after Plan reject before preparing the bone payload");
        }
        Check(fixture.first.Digest() == foreign && !fixture.history.UndoCount(), "parent-dependency rejection retains all foreign state and history");
    }
    struct Measured final : plugins::HistoryChange {
        struct Candidate final : plugins::PreparedHistoryChange {
            bool IsCurrent() const noexcept override { return true; }
            void Publish() noexcept override {}
            bool TargetRetainedBytes(std::uint64_t& bytes) const noexcept override { bytes = 37; return true; }
        };
        MmsResult Prepare(bool, std::unique_ptr<plugins::PreparedHistoryChange>& out) override {
            out = std::make_unique<Candidate>(); return MMS_OK;
        }
        std::uint64_t RetainedBytes() const noexcept override { return 17; }
        bool RequiresPhysicsReset() const noexcept override { return false; }
    };
    Fixture fixture; std::shared_ptr<plugins::HistoryChange> measured = std::make_shared<Measured>();
    Check(plugins::WrapCenterOffsetParentDependencies(*fixture.app, 0, measured) == MMS_OK && !measured->RequiresPhysicsReset(),
        "center-offset parent wrapper preserves the inner physics-reset policy");
    std::unique_ptr<plugins::PreparedHistoryChange> candidate; std::uint64_t bytes = 0;
    Check(measured->Prepare(true, candidate) == MMS_OK && candidate->IsCurrent() && candidate->TargetRetainedBytes(bytes) &&
        bytes > 37 && bytes - 37 == measured->RetainedBytes() - 17,
        "parent wrapper adds the same actual parent-vector and wrapper charge to fixed and dynamic measurements");
    fixture.first.bones[1].parent = -1;
    Check(!candidate->IsCurrent(), "prepared parent proxy rechecks even an unedited child's dependency before publication");
}
}
void NativeCenterOffsetChecks() {
    std::puts("RUN center offset native arithmetic and common history"); center_offset_checks::ParityAndHistory();
    std::puts("RUN center offset sparse authors and scale preservation"); center_offset_checks::SparseAuthors();
    std::puts("RUN center offset no-op cancellation and budget"); center_offset_checks::NoopCancelAndBudget();
    std::puts("RUN center offset post-modal stale inputs"); center_offset_checks::StaleInput();
    std::puts("RUN center offset allocation and plugin interleave"); center_offset_checks::FailuresAndPluginOrder();
    std::puts("RUN center offset planned and prepared parent dependencies"); center_offset_checks::ParentDependencies();
}
