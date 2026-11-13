// Include after native_history_mode_checks.inc and native_model_delete_target_checks.inc.
// Exercises internal participants only; mixed DELETE command admission is unchanged.
#include "../src/animation/negative_history.hpp"
namespace negative_model_delete_target_checks {
namespace target = model_delete_target_checks;
constexpr std::uint64_t kPreparationBytes = 64ull * 1024 * 1024;

struct Owner {
    MMDApp* previous = g_Block;
    explicit Owner(MMDApp* app) { g_Block = app; }
    ~Owner() { g_Block = previous; }
};
struct Fixture : ModelKeyFixture {
    std::array<std::uint32_t, 2> boneRows{1, 2};
    std::array<std::uint32_t, 1> morphRows{3};
    Fixture() {
        first.m->boneKeyIndices = boneRows.data();
        first.m->morphKeyIndices = morphRows.data();
    }
    ~Fixture() {
        history.Reset(); history.Collect(); negative_frames::Reset(app.get());
        first.m->boneKeyIndices = first.m->morphKeyIndices = nullptr;
    }
};

plugins::ModelKeySelection Seed(Fixture& f, bool display = false, bool negatives = true) {
    Owner owner(f.app.get());
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    auto selection = target::Seed(f, target::Families::All, true);
    // Production display snapshots reserve selector zero for the whole model.
    f.firstOrder[0].boneIndex = -1; f.secondOrder[0].boneIndex = -1;
    f.first.dirty = {0, 0};
    if (negatives) {
        f.app->CurrentFrame() = -20;
        f.first.bones[0].trans[0] = 8;
        f.first.bones[0].rotQuat[0] = .6f; f.first.bones[0].rotQuat[3] = .8f;
        Check(bone_scale::SetPose(f.first.raw(), 0, Scale(2)) &&
              negative_frames::RegisterBone(f.first.raw(), 0, -20),
              "seed real negative bone dependency with position rotation and scale");
        Check(negative_frames::RegisterMorph(f.first.raw(), 0, -20, .9f),
              "seed real negative morph dependency");
        f.app->CurrentFrame() = -10;
        if (display) {
            f.first.m->loadComplete = 0; f.firstIk[0].enabled = 0;
            f.firstOrder[0].linkedModel = 1; f.firstOrder[0].linkedBone = 0;
            Check(negative_frames::RegisterDisplay(f.first.raw(), -10) &&
                  negative_frames::SelectCell(f.app.get(), -10, 0, false),
                  "select negative visibility IK and parent keys for deletion");
        } else {
            f.first.bones[1].trans[0] = 14;
            Check(bone_scale::SetPose(f.first.raw(), 1, Scale(3)) &&
                  negative_frames::RegisterBone(f.first.raw(), 1, -10) &&
                  negative_frames::SelectCell(f.app.get(), -10, 2, false),
                  "select a different negative bone track for deletion");
        }
        Check(!negative_frames::CellSelected(f.app.get(), -20, 1) &&
              !negative_frames::CellSelected(f.app.get(), -20, 3),
              "negative bone and morph zero dependencies are genuinely unselected");
    }
    Check(f.history.Reset() == MMS_OK, "isolate deletion from fixture registration history");
    f.history.Collect();
    f.app->CurrentFrame() = -5;
    SeekModelFrame(f.first.raw(), -5, 0);
    f.app->SceneModified() = 0;
    return selection;
}

struct Snapshot {
    ModelKeyState keys;
    target::Authors authors;
    target::Live live;
    std::vector<std::uint8_t> negative, selection;
    std::uint64_t digest, peer, ring;
    std::size_t undo, redo;
    int frame, modified, registered;
    explicit Snapshot(Fixture& f) : keys(f.first), authors(f), live(f),
        negative(NegativeTestRecord(*f.app, negative_frames::kPmmKey)),
        selection(NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey)),
        digest(f.first.Digest()), peer(f.second.Digest()), ring(PoseRingDigest(f.first)),
        undo(f.history.UndoCount()), redo(f.history.RedoCount()), frame(f.app->CurrentFrame()),
        modified(f.app->SceneModified()), registered(f.app->state.lastRegisteredFrame) {}
    bool Matches(Fixture& f) const {
        return keys.Matches(f.first) && authors.Matches(f) && live.Matches(f) &&
            negative == NegativeTestRecord(*f.app, negative_frames::kPmmKey) &&
            selection == NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey) &&
            digest == f.first.Digest() && peer == f.second.Digest() && ring == PoseRingDigest(f.first) &&
            undo == f.history.UndoCount() && redo == f.history.RedoCount() && frame == f.app->CurrentFrame() &&
            modified == f.app->SceneModified() && registered == f.app->state.lastRegisteredFrame;
    }
};

// Test-only joining adapter. Both targets are real production candidates.
// Native model preparation enforces its own budget; the negative participant
// receives a separate, explicit remaining transient allowance.
struct Pair final : plugins::PreparedHistoryChange {
    std::unique_ptr<plugins::PreparedModelKeyDeletion> positive;
    std::unique_ptr<plugins::PreparedHistoryChange> negative;
    bool published = false;
    bool IsCurrent() const noexcept override {
        return !published && positive && negative && positive->IsCurrent() && negative->IsCurrent();
    }
    void Publish() noexcept override {
        positive->Publish();
        // Native publication intentionally invalidates negative source guards.
        // No late IsCurrent call is permitted between these publications.
        negative->Publish();
        published = true;
    }
};
struct Change final : plugins::HistoryChange {
    std::shared_ptr<plugins::ModelKeyDeletion> positive;
    std::shared_ptr<negative_frames::SelectedDeletion> negative;
    MmsResult PreparePair(bool forward, bool initial,
                          std::unique_ptr<plugins::PreparedHistoryChange>& out) {
        out.reset();
        try {
            auto candidate = std::make_unique<Pair>();
            auto result = positive->PrepareSigned(forward, candidate->positive);
            if (result != MMS_OK) return result;
            if (!candidate->positive) return MMS_E_NOT_READY;
            negative_frames::DeletionTargets targets;
            targets.model = candidate->positive.get(); targets.modelSlot = 0;
            std::uint64_t remaining = kPreparationBytes;
            result = negative->PrepareSigned(forward, initial, targets, remaining, candidate->negative);
            if (result != MMS_OK) return result;
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        } catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
          catch (const std::length_error&) { return MMS_E_MEMORY; }
    }
    MmsResult Prepare(bool forward, std::unique_ptr<plugins::PreparedHistoryChange>& out) override {
        return PreparePair(forward, false, out);
    }
    MmsResult PrepareInitial(plugins::SharedHistory&, bool,
                             std::unique_ptr<plugins::PreparedHistoryChange>& out) override {
        return PreparePair(true, true, out);
    }
    std::uint64_t RetainedBytes() const noexcept override {
        return sizeof(*this) + positive->RetainedBytes() + negative->RetainedBytes();
    }
};
std::shared_ptr<Change> Plan(Fixture& f, const plugins::ModelKeySelection& selection) {
    auto change = std::make_shared<Change>();
    Check(plugins::PlanSignedModelKeyDeletion(*f.app, 0, false, change->positive, false, &selection) == MMS_OK &&
          change->positive && negative_frames::PlanSignedSelectedDeletion(*f.app, change->negative, true) == MMS_OK &&
          change->negative, "plan real positive and typed negative model deletion participants");
    return change;
}
void Publish(plugins::PreparedHistoryChange& candidate) {
    Check(candidate.IsCurrent(), "validate both signed participants before the first write");
    failAfter = 0; candidate.Publish(); failAfter = -1;
}

struct Oracle {
    Fixture& fixture;
    std::shared_ptr<plugins::HistoryChange> positive, negative;
    Oracle(Fixture& f, const plugins::ModelKeySelection& selection) : fixture(f) {
        f.app->CurrentFrame() = 0;
        Check(plugins::PlanModelKeyDeletion(*f.app, 0, false, positive, false, &selection) == MMS_OK && positive &&
              negative_frames::PlanSelectedDeletion(*f.app, negative) == MMS_OK,
              "independent oracle plans ordinary positive deletion and selected negative key removal");
    }
    void Apply(bool forward, int frame, bool seek = true) {
        Owner owner(fixture.app.get());
        plugins::NativePoseTestBinding binding(*fixture.app, fixture.history);
        fixture.app->CurrentFrame() = frame < 0 ? 0 : frame;
        std::unique_ptr<plugins::PreparedHistoryChange> prepared;
        Check(positive->Prepare(forward, prepared) == MMS_OK && prepared,
              "independent ordinary positive oracle prepares direction");
        Publish(*prepared); prepared.reset();
        if (negative) {
            Check(negative->Prepare(forward, prepared) == MMS_OK && prepared,
                  "independent oracle removes or restores real negative keys at a nonnegative frame");
            Publish(*prepared);
        }
        fixture.app->CurrentFrame() = frame;
        if (seek) SeekModelFrame(fixture.first.raw(), frame, 0);
    }
};
void PoseMatches(Fixture& f, Fixture& oracle, unsigned dirtyMask) {
    for (unsigned bone = 0; bone < 2; ++bone) {
        if (dirtyMask & (1u << bone)) continue;
        for (unsigned axis = 0; axis < 3; ++axis)
            Check(std::fabs(f.first.bones[bone].trans[axis] - oracle.first.bones[bone].trans[axis]) < .0001f,
                  "signed bone position matches a real explicit negative frame seek");
        for (unsigned axis = 0; axis < 4; ++axis)
            Check(std::fabs(f.first.bones[bone].rotQuat[axis] - oracle.first.bones[bone].rotQuat[axis]) < .0001f,
                  "signed bone rotation matches a real explicit negative frame seek");
        const auto actual = bone_scale::Pose(f.first.raw(), int(bone));
        const auto expected = bone_scale::Pose(oracle.first.raw(), int(bone));
        for (unsigned axis = 0; axis < 9; ++axis)
            Check(std::fabs(actual.v[axis] - expected.v[axis]) < .0001f,
                  "signed bone scale matches a real explicit negative frame seek");
    }
    Check(std::fabs(f.first.morphs[0].value - oracle.first.morphs[0].value) < .0001f &&
          f.first.m->loadComplete == oracle.first.m->loadComplete && f.firstIk[0].enabled == oracle.firstIk[0].enabled &&
          f.firstOrder[0].linkedModel == oracle.firstOrder[0].linkedModel &&
          f.firstOrder[0].linkedBone == oracle.firstOrder[0].linkedBone,
          "signed morph visibility IK and external parent match the explicit frame oracle");
    // A signed seek establishes the registered-zero window before overlaying
    // negative parent references; surviving references do not replace it.
    Check(f.firstOrder[0].windowStart == oracle.firstOrder[0].windowStart &&
          f.firstOrder[0].windowEnd == oracle.firstOrder[0].windowEnd,
          "signed parent selector window matches the registered-zero window from the explicit frame oracle");
}

void Directions() {
    for (auto mode : {plugins::SharedHistory::RecordingMode::AllOperations,
                      plugins::SharedHistory::RecordingMode::ExplicitOnly})
    for (bool display : {false, true})
    for (unsigned dirtyMask : {0u, 3u}) {
        Fixture f, expected;
        const auto selection = Seed(f, display), expectedSelection = Seed(expected, display);
        plugins::NativePoseTestBinding binding(*f.app, f.history);
        const ModelKeyState original(f.first); const target::Authors originalAuthors(f);
        const auto negativeBefore = NegativeTestRecord(*f.app, negative_frames::kPmmKey);
        const float sourceMorph = f.first.morphs[0].value, sourceBone = f.first.bones[0].trans[0];
        const auto peer = f.second.Digest(), ring = PoseRingDigest(f.first);
        for (unsigned bone = 0; bone < 2; ++bone) if (dirtyMask & (1u << bone)) {
            f.first.dirty[bone] = 1; f.first.bones[bone].trans[0] = float(700 + bone);
            Check(bone_scale::SetPose(f.first.raw(), int(bone), Scale(float(4 + bone))), "seed dirty bone draft");
        }
        const auto drafts = f.first.bones; const auto dirty = f.first.dirty;
        const auto draftScales = bone_scale::PoseValues(f.first.raw());
        Check(f.history.SetMode(mode) == MMS_OK, "select signed deletion history mode");
        const Snapshot before(f); auto change = Plan(f, selection);
        Check(before.Matches(f), "signed planning preserves all native negative author and live state");
        Oracle oracle(expected, expectedSelection);
        for (unsigned step = 0; step < 3; ++step) {
            const bool forward = step != 1;
            const int frame = step == 0 ? -5 : step == 1 ? -9 : -3;
            f.app->CurrentFrame() = frame;
            if (step == 0) {
                std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
                const auto prepared=f.history.PrepareNative("signed model target test", change, transaction);
                if(prepared!=MMS_OK||!transaction||!before.Matches(f))
                    std::printf("model target prepare mode=%u display=%u dirty=%u result=%d transaction=%u unchanged=%u\n",
                        unsigned(mode),unsigned(display),dirtyMask,int(prepared),unsigned(bool(transaction)),unsigned(before.Matches(f)));
                Check(prepared == MMS_OK && transaction &&
                      before.Matches(f), "initial signed history reservation is detached");
                Check(CommitRegistrationNoAllocation(f.history, std::move(transaction)) == MMS_OK,
                      "initial signed model deletion publishes without allocation");
            } else Check(f.history.History(forward) == MMS_OK, "signed model deletion undo and redo succeed");
            oracle.Apply(forward, frame);
            target::PayloadsMatch(f, expected); PoseMatches(f, expected, dirtyMask);
            Check(NegativeTestRecord(*f.app, negative_frames::kPmmKey) ==
                  NegativeTestRecord(*expected.app, negative_frames::kPmmKey),
                  "signed negative key payloads match independent deletion and restoration");
            Check(negative_frames::Count(f.app.get()) == (forward ? 2u : display ? 5u : 3u) &&
                  !negative_frames::CellSelected(f.app.get(), -20, 1) &&
                  !negative_frames::CellSelected(f.app.get(), -20, 3) &&
                  negative_frames::CellSelected(f.app.get(), -10, display ? 0 : 2) == !forward,
                  "only the selected negative track disappears and undo restores its selection");
            Check(f.first.dirty == dirty && f.app->CurrentFrame() == frame && f.second.Digest() == peer &&
                  PoseRingDigest(f.first) == ring && f.SelectionUnchanged() && !f.app->SceneModified() &&
                  f.history.UndoCount() == (forward ? 1u : 0u) && f.history.RedoCount() == (forward ? 0u : 1u),
                  "both history modes preserve drafts playhead peer legacy rings and one journal node");
            for (unsigned bone = 0; bone < 2; ++bone) if (dirtyMask & (1u << bone)) {
                Check(!std::memcmp(&f.first.bones[bone], &drafts[bone], sizeof(drafts[bone])),
                      "selected and unselected dirty bones retain complete draft records");
            }
            if (dirtyMask) Check(bone_scale::PoseValuesEqual(f.first.raw(), draftScales),
                                  "dirty bone scales retain the unregistered draft policy");
            if (step == 0) {
                Check(std::fabs(f.first.morphs[0].value - sourceMorph) > .001f,
                      "positive root deletion actually refreshes the unselected negative morph endpoint");
                if (!dirtyMask) Check(std::fabs(f.first.bones[0].trans[0] - sourceBone) > .001f,
                                      "positive root deletion actually refreshes the unselected negative bone endpoint");
                if (display) Check(f.first.m->loadComplete == 1 && f.firstIk[0].enabled == 1 &&
                                   f.firstOrder[0].linkedModel == -1,
                                   "deleting the last negative display keys uses changed positive zero visibility IK and parent");
            }
            if (!forward) Check(original.Matches(f.first) && originalAuthors.Matches(f) &&
                                NegativeTestRecord(*f.app, negative_frames::kPmmKey) == negativeBefore,
                                "undo restores original positive authoring and negative key payloads");
        }
    }
}

void PositiveOnlyAndNonnegative() {
    for (bool negatives : {false, true}) for (int frame : {-5, 0, 19}) {
        if (negatives && frame < 0) continue; // Covered with both history modes above.
        Fixture f, expected;
        const auto selection = Seed(f, false, negatives), other = Seed(expected, false, negatives);
        plugins::NativePoseTestBinding binding(*f.app, f.history);
        f.app->CurrentFrame() = expected.app->CurrentFrame() = frame;
        if (!negatives) {
            std::shared_ptr<negative_frames::SelectedDeletion> empty;
            Check(negative_frames::PlanSignedSelectedDeletion(*f.app, empty) == MMS_OK && !empty &&
                  NegativeTestRecord(*f.app, negative_frames::kPmmKey).empty(),
                  "ordinary negative planning remains empty when no negative state exists");
        }
        const Snapshot before(f); auto change = Plan(f, selection);
        Check(before.Matches(f), "includeUnchanged planning leaves absent negative storage observable as absent");
        Oracle oracle(expected, other);
        for (unsigned step = 0; step < 3; ++step) {
            std::unique_ptr<plugins::PreparedHistoryChange> candidate;
            const Snapshot source(f);
            Check(change->PreparePair(step != 1, step == 0, candidate) == MMS_OK && candidate && source.Matches(f),
                  "positive-only or nonnegative signed preparation is detached");
            if (!negatives) {
                // Reset erases the internal state entry. On an absent source it
                // must be a no-op, including for this already-prepared receipt.
                // A candidate that inserted an empty state/lifetime becomes stale.
                negative_frames::Reset(f.app.get());
                Check(candidate->IsCurrent() && source.Matches(f),
                      "positive-only preparation does not insert an empty negative state entry");
            }
            Publish(*candidate); oracle.Apply(step != 1, frame, frame < 0);
            target::PayloadsMatch(f, expected);
            if (frame < 0) PoseMatches(f, expected, 0);
            else Check(target::Live(expected).Matches(f),
                       "nonnegative negative participant leaves the ordinary positive final pose untouched");
            if (!negatives) Check(!negative_frames::Count(f.app.get()) &&
                                  NegativeTestRecord(*f.app, negative_frames::kPmmKey).empty() &&
                                  NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey).empty(),
                                  "positive-only first undo and redo never create negative keys or selection records");
        }
    }
}

void PositiveBoneWithoutScaleStorage() {
    // Do not use Seed, SetPose, SetKey, authored curves, or a preparatory seek:
    // this regression requires the registry entry itself to be absent.
    ModelKeyFixture f;
    Owner owner(f.app.get());
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    f.firstOrder[0].boneIndex = f.secondOrder[0].boneIndex = -1;
    f.first.m->boneKeys[0].position[0] = 6;
    f.app->CurrentFrame() = -5;
    Check(!bone_scale::HasPoseStorage(f.first.raw()) && !bone_scale::HasPoseStorage(f.second.raw()) &&
          !negative_frames::Count(f.app.get()), "minimal bone fixture starts without scale storage or negative keys");
    const auto curves = bone_curve::AuthoringState::Capture(f.first.raw());
    bone_curve::Tracks lanes;
    Check(!curves->Read(0, lanes) && !curves->Read(1, lanes),
          "minimal bone regression has no Free authored curves");
    const ModelKeyState sourceKeys(f.first);
    const target::Authors sourceAuthors(f);
    const target::Live sourceLive(f);
    const auto digest = f.first.Digest(), peer = f.second.Digest(), ring = PoseRingDigest(f.first);
    const auto negativeKeys = NegativeTestRecord(*f.app, negative_frames::kPmmKey);
    const auto negativeSelection = NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey);
    Check(negativeKeys.empty() && negativeSelection.empty(), "minimal bone fixture has no negative records");
    const auto unchanged = [&]() {
        return sourceKeys.Matches(f.first) && sourceAuthors.Matches(f) && sourceLive.Matches(f) &&
            f.first.Digest() == digest && f.second.Digest() == peer && PoseRingDigest(f.first) == ring &&
            f.app->CurrentFrame() == -5 && !f.app->SceneModified() && f.app->state.lastRegisteredFrame == 37 &&
            f.SelectionUnchanged() && !f.history.UndoCount() && !f.history.RedoCount() &&
            !negative_frames::Count(f.app.get()) &&
            NegativeTestRecord(*f.app, negative_frames::kPmmKey) == negativeKeys &&
            NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey) == negativeSelection;
    };
    plugins::ModelKeySelection selection; selection.bones = {0, 2};
    Change change;
    Check(plugins::PlanSignedModelKeyDeletion(*f.app, 0, false, change.positive, false, &selection) == MMS_OK &&
          change.positive && negative_frames::PlanSignedSelectedDeletion(*f.app, change.negative, true) == MMS_OK &&
          change.negative && unchanged() && !bone_scale::HasPoseStorage(f.first.raw()),
          "positive-only bone planning leaves scale storage absent");
    for (unsigned cancellation = 0; cancellation < 3; ++cancellation) {
        Pair pair;
        Check(!bone_scale::HasPoseStorage(f.first.raw()), "each bone preparation starts with absent scale storage");
        Check(change.positive->PrepareSigned(true, pair.positive) == MMS_OK && pair.positive &&
              pair.positive->IsCurrent() && unchanged(), "real positive bone preparation preserves authored and live state");
        if (cancellation != 0) {
            negative_frames::DeletionTargets targets;
            targets.model = pair.positive.get(); targets.modelSlot = 0;
            std::uint64_t remaining = kPreparationBytes;
            Check(change.negative->PrepareSigned(true, true, targets, remaining, pair.negative) == MMS_OK && pair.negative,
                  "negative zero fallback prepares from the real storage-absent positive bone candidate");
            Check(pair.positive->IsCurrent() && pair.negative->IsCurrent() && pair.IsCurrent() && unchanged(),
                  "negative pose storage reservation must not invalidate the positive authoring guard");
            // The isolated fixture has never registered negative keys. Reset
            // must be a no-op for an absent entry and leave its receipt current.
            negative_frames::Reset(f.app.get());
            Check(pair.IsCurrent(), "positive-only bone preparation does not create negative state");
        }
        if (cancellation == 2) { pair.positive.reset(); pair.negative.reset(); }
        else { pair.negative.reset(); pair.positive.reset(); }
        Check(!bone_scale::HasPoseStorage(f.first.raw()) && !bone_scale::HasPoseStorage(f.second.raw()) && unchanged(),
              "positive-only and joint cancellation in either destruction order restore storage absence without mutation");
    }
    std::unique_ptr<plugins::PreparedHistoryChange> prepared;
    unsigned failures = 0; bool ready = false;
    for (long limit = 0; limit < 4096; ++limit) {
        Check(!bone_scale::HasPoseStorage(f.first.raw()), "each OOM attempt starts with genuinely absent scale storage");
        failAfter = limit;
        const auto result = change.PreparePair(true, true, prepared);
        failAfter = -1;
        Check(unchanged(), "storage-absent joined preparation never publishes keys authoring pose or history");
        if (result == MMS_OK) { ready = prepared && prepared->IsCurrent(); break; }
        Check(result == MMS_E_MEMORY && !prepared && !bone_scale::HasPoseStorage(f.first.raw()) &&
              !bone_scale::HasPoseStorage(f.second.raw()), "joined OOM releases the reversible bone storage reservation");
        ++failures;
    }
    Check(ready && failures > 3, "storage-absent OOM sweep reaches jointly current positive and negative candidates");
    Publish(*prepared);
    prepared.reset();
    Check(f.first.m->boneKeys[0].position[0] == 0 && f.first.m->boneKeys[0].next == 3 &&
          f.first.m->boneKeys[2].frame == 0 && f.first.bones[0].trans[0] == 0 &&
          f.first.bones[0].rotQuat[0] == 0 && f.first.bones[0].rotQuat[1] == 0 &&
          f.first.bones[0].rotQuat[2] == 0 && f.first.bones[0].rotQuat[3] == 1 &&
          bone_scale::Pose(f.first.raw(), 0) == bone_scale::Matrix{},
          "allocation-free publication deletes native bone keys and installs the negative-playhead zero pose");
    Check(!std::memcmp(&f.first.bones[1], &sourceLive.bones[1], sizeof(f.first.bones[1])) &&
          sourceLive.derived.Matches(f) && f.first.m->loadComplete == sourceLive.visible &&
          f.first.dirty == sourceLive.dirty && f.first.selected == sourceLive.selected &&
          f.second.Digest() == peer && !bone_scale::HasPoseStorage(f.second.raw()) && PoseRingDigest(f.first) == ring &&
          f.app->CurrentFrame() == -5 && !f.app->SceneModified() && f.app->state.lastRegisteredFrame == 37 &&
          f.SelectionUnchanged() && !f.history.UndoCount() && !f.history.RedoCount() &&
          !negative_frames::Count(f.app.get()) &&
          NegativeTestRecord(*f.app, negative_frames::kPmmKey) == negativeKeys &&
          NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey) == negativeSelection,
          "positive-only bone success preserves unrelated channels peer state and empty negative history");
}

void Stale() {
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
        Fixture f; const auto selection = Seed(f);
        plugins::NativePoseTestBinding binding(*f.app, f.history);
        auto change = Plan(f, selection);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("signed model stale test", change, transaction) == MMS_OK && transaction,
              "prepare both model participants before an external stale mutation");
        const ModelKeyState original(f.first);
        if (mutation == 0) f.first.m->boneKeys[0].position[0] += 1;
        if (mutation == 1) f.app->CurrentFrame() = -4;
        if (mutation == 2) {
            morph_curve::AuthoredLayers layers; std::vector<animation_curve::NamedTrack> tracks; std::string error;
            Check(morph_curve::CaptureLayers(f.first.raw(), layers, error) &&
                  morph_curve::LayersTracks(layers, tracks, error) && !tracks.empty(), "capture author-only mutation source");
            tracks[0].track.keys[0].outSlope += .125;
            morph_curve::AuthoredLayers replacement;
            Check(morph_curve::RebuildLayers(f.first.raw(), tracks, replacement, error), "prepare author-only mutation");
            morph_curve::SwapLayers(f.first.raw(), replacement);
            Check(original.Matches(f.first), "author-only mutation preserves every native key byte");
        }
        if (mutation == 3) {
            // ForgetModel is an actual source invalidation, not another edit
            // admitted through the already-held shared-history gate.
            negative_frames::ForgetModel(f.first.raw());
        }
        if (mutation == 4) Check(negative_frames::SelectCell(f.app.get(), -20, 3, false),
                                 "change negative selection after both candidates were prepared");
        const Snapshot external(f);
        Check(CommitRegistrationNoAllocation(f.history, std::move(transaction)) == MMS_E_STALE && !transaction &&
              external.Matches(f), "stale source frame author or selection rejects both participants without publication");
    }
}

void BudgetAndAllocationFailures() {
    Fixture f; const auto selection = Seed(f);
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    const Snapshot source(f);
    unsigned planningFailures = 0; bool planned = false;
    for (long limit = 0; limit < 1024; ++limit) {
        std::shared_ptr<negative_frames::SelectedDeletion> attempt;
        failAfter = limit;
        const auto result = negative_frames::PlanSignedSelectedDeletion(*f.app, attempt, true);
        failAfter = -1;
        Check(source.Matches(f), "negative planning allocation sweep never mutates model or negative state");
        if (result == MMS_OK) { planned = bool(attempt); break; }
        Check(result == MMS_E_MEMORY && !attempt, "negative planning OOM has no partial result"); ++planningFailures;
    }
    Check(planned && planningFailures, "negative planning allocation sweep reaches success");
    auto change = Plan(f, selection);
    std::unique_ptr<plugins::PreparedModelKeyDeletion> positive;
    Check(change->positive->PrepareSigned(true, positive) == MMS_OK && positive && positive->IsCurrent(),
          "prepare real positive targets for negative budget and allocation failures");
    negative_frames::DeletionTargets targets; targets.model = positive.get(); targets.modelSlot = 0;
    std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    unsigned failures = 0; bool prepared = false; std::uint64_t used = 0;
    for (long limit = 0; limit < 4096; ++limit) {
        std::uint64_t remaining = kPreparationBytes;
        failAfter = limit;
        const auto result = change->negative->PrepareSigned(true, true, targets, remaining, candidate);
        failAfter = -1;
        Check(source.Matches(f) && positive->IsCurrent(), "negative OOM never publishes or damages borrowed positive targets");
        if (result == MMS_OK) {
            prepared = candidate && candidate->IsCurrent(); used = kPreparationBytes - remaining; break;
        }
        Check(result == MMS_E_MEMORY && !candidate && remaining == kPreparationBytes,
              "negative OOM returns no candidate and preserves the caller's remaining budget"); ++failures;
    }
    Check(prepared && failures > 3 && used > 1, "negative allocation sweep reaches a charged complete candidate");
    candidate.reset();
    for (std::uint64_t limit : {std::uint64_t(0), used - 1}) {
        auto remaining = limit;
        Check(change->negative->PrepareSigned(true, true, targets, remaining, candidate) == MMS_E_MEMORY &&
              !candidate && remaining == limit && source.Matches(f) && positive->IsCurrent(),
              "zero and just-insufficient negative preparation budgets fail atomically");
    }
    // Exercise the complete joined preparation too, so failures after positive
    // preparation cannot accidentally expose a half-prepared result.
    failures = 0; prepared = false;
    for (long limit = 0; limit < 4096; ++limit) {
        failAfter = limit; const auto result = change->PreparePair(true, true, candidate); failAfter = -1;
        Check(source.Matches(f), "joined allocation sweep leaves positive negative and live state untouched");
        if (result == MMS_OK) { prepared = candidate && candidate->IsCurrent(); break; }
        Check(result == MMS_E_MEMORY && !candidate, "joined OOM exposes neither participant"); ++failures;
    }
    Check(prepared && failures > 3, "joined allocation sweep reaches eventual complete preparation");
    Publish(*candidate);
    Check(negative_frames::Count(f.app.get()) == 2 && !f.history.UndoCount() && !f.history.RedoCount(),
          "successful direct publication removes exactly the selected negative track without inventing a journal entry");
}
}

void NegativeModelDeleteTargetChecks() {
    negative_model_delete_target_checks::Directions();
    negative_model_delete_target_checks::PositiveOnlyAndNonnegative();
    negative_model_delete_target_checks::PositiveBoneWithoutScaleStorage();
    negative_model_delete_target_checks::Stale();
    negative_model_delete_target_checks::BudgetAndAllocationFailures();
}
