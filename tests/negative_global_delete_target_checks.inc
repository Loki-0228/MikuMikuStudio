// Include after native_history_mode_checks.inc and the global deletion checks.
// Internal participants only: this does not admit a signed UI delete command.
#include "../src/animation/negative_history.hpp"
#include "../src/plugins/native_global_key_delete.hpp"

namespace negative_global_delete_target_checks {
using namespace mikumikustudio;
namespace native = global_delete_target_checks;
using Kind = curve_host::Kind;
using Positive = plugins::GlobalKeyDeletion;
using PositiveCandidate = plugins::PreparedGlobalKeyDeletion;
using Negative = negative_frames::SelectedDeletion;
using Candidate = plugins::PreparedHistoryChange;
constexpr std::uint64_t preparationBudget = 64ull * 1024 * 1024;

struct Fixture : native::Fixture {
    MMDApp* previous = g_Block;
    Fixture() { g_Block = app.get(); negative_frames::Reset(app.get()); }
    ~Fixture() { negative_frames::Reset(app.get()); g_Block = previous; }
};

struct Snapshot {
    std::uint64_t digest, revision = 0;
    native::Authors authors;
    std::vector<std::uint8_t> keys, selection;
    std::size_t count, undo, redo;
    explicit Snapshot(Fixture& f)
        : digest(f.FullDigest()), authors(native::AuthorsOf(f)),
          keys(NegativeTestRecord(*f.app, negative_frames::kPmmKey)),
          selection(NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey)),
          count(negative_frames::Count(f.app.get())), undo(f.history.UndoCount()), redo(f.history.RedoCount()) {
        Check(f.history.Revision(revision) == MMS_OK, "read signed participant snapshot revision");
    }
    bool Same(Fixture& f) const {
        std::uint64_t now = 0;
        return digest == f.FullDigest() && native::AuthorsSame(f, authors) &&
            keys == NegativeTestRecord(*f.app, negative_frames::kPmmKey) &&
            selection == NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey) &&
            count == negative_frames::Count(f.app.get()) && undo == f.history.UndoCount() &&
            redo == f.history.RedoCount() && f.history.Revision(now) == MMS_OK && now == revision;
    }
};

std::shared_ptr<Positive> PlanPositive(Fixture& f) {
    std::shared_ptr<Positive> change;
    Check(plugins::PlanSignedGlobalKeyDeletion(*f.app, change) == MMS_OK && change,
        "real signed native global deletion plan exists");
    return change;
}
std::shared_ptr<Negative> PlanNegative(Fixture& f, bool includeUnchanged = false) {
    std::shared_ptr<Negative> change;
    Check(negative_frames::PlanSignedSelectedDeletion(*f.app, change, includeUnchanged) == MMS_OK && change,
        "signed negative participant plans selected keys or explicit zero dependencies");
    return change;
}
std::unique_ptr<PositiveCandidate> PreparePositive(const std::shared_ptr<Positive>& change, bool forward) {
    std::unique_ptr<PositiveCandidate> candidate;
    Check(change->PrepareSigned(forward, candidate) == MMS_OK && candidate && candidate->IsCurrent(),
        "real directional native candidate exposes zero targets before publication");
    return candidate;
}
MmsResult PrepareNegative(const std::shared_ptr<Negative>& change, bool forward, bool initial,
    const PositiveCandidate& positive, std::uint64_t& budget, std::unique_ptr<Candidate>& out) {
    // The descriptor is borrowed for this call only; no model participant joins.
    negative_frames::DeletionTargets targets;
    targets.globals = &positive;
    return change->PrepareSigned(forward, initial, targets, budget, out);
}
native::Poses TargetPoses(const PositiveCandidate& positive) {
    native::Poses result{};
    std::array<bool, 6> seen{};
    Check(positive.ZeroTargets().size() == seen.size(), "real target enumerates all six fixture tracks");
    for (const auto& target : positive.ZeroTargets()) {
        unsigned i = 0;
        while (i < seen.size() && (native::kinds[i] != target.kind || native::slots[i] != target.slot)) ++i;
        Check(i < seen.size() && !seen[i], "real target has a unique expected family and accessory slot");
        seen[i] = true;
        result[i] = target.zeroPose;
    }
    return result;
}
std::uint32_t GravityKeyWord(MMDApp& app) {
    std::uint32_t word = 0;
    std::memcpy(&word, &app.GravityKeys()[0].noiseEnabled, sizeof(word));
    return word;
}
void CheckPose(Fixture& f, native::Poses expected, std::uint32_t liveGravityUpper) {
    // The negative pose stores only the low mode byte. Its native key and the
    // target retain the full word; applying a pose leaves live upper bytes alone.
    expected[3][5] = liveGravityUpper | (std::uint32_t(expected[3][5]) & 0xffu);
    const auto actual = native::PosesOf(f);
    for (unsigned i = 0; i < actual.size(); ++i)
        for (unsigned lane = 0; lane < actual[i].size(); ++lane)
            Check(native::Close(actual[i][lane], expected[i][lane]),
                "negative final pose uses directional zero targets in native units");
    Check(std::uint32_t(f.app->GravityNoiseModeWord()) == std::uint32_t(expected[3][5]),
        "negative gravity pose copies only the mode byte and preserves live upper bytes exactly");
}
void PublishTogether(PositiveCandidate& positive, Candidate& negative) {
    // Both source guards refer to the OLD live baseline. Validate both before
    // the first write; do not recheck negative IsCurrent after positive Publish.
    Check(positive.IsCurrent() && negative.IsCurrent(), "both signed participants validate before joint commit");
    failAfter = 0;
    positive.Publish();
    negative.Publish();
    failAfter = -1;
}

struct NegativeSamples {
    std::array<double, 12> camera, light;
};
NegativeSamples SeedMixed(Fixture& f, bool authored = false) {
    f.app->CameraKeys()[0].eye[0] = 14.f;
    f.app->CameraKeys()[0].target[0] = .75f;
    native::Seed(f, authored);
    if (!authored) {
        const std::uint32_t word = 0x00573101u;
        std::memcpy(&f.app->GravityKeys()[0].noiseEnabled, &word, sizeof(word));
    }
    f.ClearMarks();
    f.app->CurrentFrame() = -20;
    f.app->CameraPositionX() = 30.f;
    f.app->CameraRotation()[0] = -.625f;
    Check(negative_frames::RegisterGlobal(f.app.get(), negative_frames::Global::Camera, -20),
        "seed surviving negative camera from a real registration");
    const auto camera = native::NativePoseOf(f, Kind::Camera, 0);
    f.app->CurrentFrame() = -10;
    f.app->LightDirection()[0] = 6.f;
    Check(negative_frames::RegisterGlobal(f.app.get(), negative_frames::Global::Light, -10),
        "seed different negative light to remove");
    const auto light = native::NativePoseOf(f, Kind::Light, 0);
    Check(negative_frames::SelectCell(f.app.get(), -10, 1, false) &&
        !negative_frames::CellSelected(f.app.get(), -20, 0), "select light while leaving negative camera unselected");
    f.Mark(0);
    f.app->CurrentFrame() = -5;
    negative_frames::EvaluateGlobals(f.app.get(), -5);
    Check(f.history.Reset() == MMS_OK, "discard fixture setup journal before detached preparation");
    f.history.Collect();
    f.app->SceneModified() = 0;
    return {camera, light};
}

void DirectionalOverrides() {
    for (bool authored : {false, true}) {
        Fixture f;
        const auto samples = SeedMixed(f, authored);
        const auto originalKeys = NegativeTestRecord(*f.app, negative_frames::kPmmKey);
        const global_key_delete_checks::Keys nativeBefore(*f.app);
        const auto authorsBefore = native::AuthorsOf(f);
        const auto initialPose = native::PosesOf(f);
        // The byte adjacent to enabled is selection; only the top two bytes
        // are reserved. Native deletion legitimately clears selection.
        const auto nativeUpper = GravityKeyWord(*f.app) & 0xffff0000u;
        const auto liveUpper = std::uint32_t(f.app->GravityNoiseModeWord()) & 0xffffff00u;
        const Snapshot pristine(f);
        auto positive = PlanPositive(f);
        auto negative = PlanNegative(f);
        Check(pristine.Same(f), "planning both signed halves preserves live sources selection pose and history");
        std::vector<std::uint8_t> forwardKeys;
        native::Poses firstPose{};
        std::unique_ptr<global_key_delete_checks::Keys> forwardNative;
        native::Authors forwardAuthors;
        for (unsigned step = 0; step < 3; ++step) {
            const bool forward = step != 1;
            const Snapshot before(f);
            auto nativeCandidate = PreparePositive(positive, forward);
            const auto zero = TargetPoses(*nativeCandidate);
            std::unique_ptr<Candidate> candidate;
            auto budget = preparationBudget;
            Check(PrepareNegative(negative, forward, step == 0, *nativeCandidate, budget, candidate) == MMS_OK &&
                candidate && candidate->IsCurrent() && budget < preparationBudget,
                "initial undo and redo negative candidates prepare with a charged transient budget");
            Check(before.Same(f), "joint preparation never seeks or writes keys authoring selection pose or history");

            // Destroy the actual borrowed target object, then prepare an equivalent
            // native half. The negative candidate must own the sampled values.
            nativeCandidate.reset();
            Check(candidate->IsCurrent(), "negative candidate does not retain the borrowed target object");
            nativeCandidate = PreparePositive(positive, forward);
            Check(TargetPoses(*nativeCandidate) == zero, "replacement real native candidate has identical targets");
            auto expected = zero;
            // At -5, a camera at -20 interpolates 3/4 towards zero. On undo,
            // restored light at -10 interpolates halfway towards its old zero.
            expected[0] = samples.camera;
            for (unsigned lane = 0; lane < 8; ++lane)
                expected[0][lane] = float(samples.camera[lane] + .75 * (zero[0][lane] - samples.camera[lane]));
            if (!forward) {
                expected[1] = samples.light;
                for (unsigned lane = 0; lane < 6; ++lane)
                    expected[1][lane] = float(samples.light[lane] + .5 * (zero[1][lane] - samples.light[lane]));
            }
            if (!forward) {
                constexpr double radians = 3.14159265358979323846 / 180.;
                Check(native::Close(zero[0][3], authored ? 23.125 * radians : .75) &&
                    native::Close(zero[4][4], authored ? -31.625 * radians : .375),
                    "camera and accessory reverse targets contain radians without double conversion");
            }
            PublishTogether(*nativeCandidate, *candidate);
            CheckPose(f, expected, liveUpper);
            if(GravityKeyWord(*f.app)!=std::uint32_t(zero[3][5])||(GravityKeyWord(*f.app)&0xffff0000u)!=nativeUpper)
                std::printf("gravity word authored=%u step=%u actual=%08x target=%08x initialUpper=%08x\n",
                    unsigned(authored),step,GravityKeyWord(*f.app),std::uint32_t(zero[3][5]),nativeUpper);
            Check(GravityKeyWord(*f.app) == std::uint32_t(zero[3][5]) &&
                (GravityKeyWord(*f.app) & 0xffff0000u) == nativeUpper,
                "positive native publication retains the complete gravity mode word and reserved upper bytes");
            double value = 0;
            Check(negative_frames::Count(f.app.get()) == (forward ? 1u : 2u) &&
                NegativeTestGlobal(*f.app, 2, -20, value) && value == 30 &&
                (forward ? !NegativeTestGlobal(*f.app, 3, -10, value) :
                    NegativeTestGlobal(*f.app, 3, -10, value) && value == 6),
                "only selected negative light is removed and undo restores its exact authored value");
            Check(negative_frames::CellSelected(f.app.get(), -20, 0) == (step != 0) &&
                negative_frames::CellSelected(f.app.get(), -10, 1) == !forward,
                "replay preserves surviving live selection and restores selected deleted keys");
            Check(f.app->CurrentFrame() == -5 && !f.history.UndoCount() && !f.history.RedoCount() &&
                !f.app->SceneModified(), "detached signed publication leaves playhead journal and dirty admission to its caller");
            if (step == 0) {
                Check(f.app->CameraRotation()[0] != initialPose[0][3],
                    "unselected negative camera is resampled when positive camera zero changes");
                forwardKeys = NegativeTestRecord(*f.app, negative_frames::kPmmKey);
                firstPose = native::PosesOf(f);
                forwardNative = std::make_unique<global_key_delete_checks::Keys>(*f.app);
                forwardAuthors = native::AuthorsOf(f);
                Check(negative_frames::SelectCell(f.app.get(), -20, 0, false),
                    "change surviving selection between initial publication and undo");
            } else if (!forward) {
                Check(originalKeys == NegativeTestRecord(*f.app, negative_frames::kPmmKey) &&
                    nativeBefore.Same(*f.app) && native::AuthorsSame(f, authorsBefore),
                    "reverse restores exact negative records native pools and sparse authoring");
            } else {
                Check(forwardKeys == NegativeTestRecord(*f.app, negative_frames::kPmmKey) &&
                    native::PosesOf(f) == firstPose && forwardNative->Same(*f.app) && native::AuthorsSame(f, forwardAuthors),
                    "redo repeats forward keys zero overrides and final poses exactly");
            }
        }
    }
}

void PositiveOnlyWithoutState() {
    for (unsigned index : {0u, 1u}) {
        Fixture f;
        native::Seed(f, false);
        f.ClearMarks();
        f.Mark(index);
        f.app->CurrentFrame() = -5;
        const auto liveUpper = std::uint32_t(f.app->GravityNoiseModeWord()) & 0xffffff00u;
        const Snapshot original(f);
        std::shared_ptr<Negative> empty;
        Check(negative_frames::PlanSignedSelectedDeletion(*f.app, empty) == MMS_OK && !empty && original.Same(f),
            "ordinary signed negative planning is empty without selected negative data");
        auto positive = PlanPositive(f);
        auto negative = PlanNegative(f, true);
        Check(original.Same(f), "includeUnchanged planning does not install negative storage");
        for (unsigned step = 0; step < 3; ++step) {
            const Snapshot before(f);
            auto nativeCandidate = PreparePositive(positive, step != 1);
            const auto zero = TargetPoses(*nativeCandidate);
            std::unique_ptr<Candidate> candidate;
            auto budget = preparationBudget;
            Check(PrepareNegative(negative, step != 1, step == 0, *nativeCandidate, budget, candidate) == MMS_OK &&
                candidate && candidate->IsCurrent() && before.Same(f),
                "positive-only initial reverse and redo prepare without a live negative state");
            PublishTogether(*nativeCandidate, *candidate);
            CheckPose(f, zero, liveUpper);
            Check(!negative_frames::Count(f.app.get()) &&
                NegativeTestRecord(*f.app, negative_frames::kPmmKey).empty() &&
                NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey).empty() &&
                !f.history.UndoCount() && !f.history.RedoCount() && f.app->CurrentFrame() == -5,
                "positive-only publication and replay leave negative count and serialized storage absent");
        }
    }
}

void StaleCandidates() {
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
        Fixture f;
        SeedMixed(f);
        auto positive = PlanPositive(f);
        auto negative = PlanNegative(f);
        auto nativeCandidate = PreparePositive(positive, true);
        auto budget = preparationBudget;
        std::unique_ptr<Candidate> candidate;
        Check(PrepareNegative(negative, true, true, *nativeCandidate, budget, candidate) == MMS_OK &&
            candidate && candidate->IsCurrent(), "prepare current negative candidate before external mutation");
        switch (mutation) {
        case 0: {
            const auto pose = f.app->CameraPositionX();
            f.app->CameraPositionX() = 43;
            Check(negative_frames::RegisterGlobal(f.app.get(), negative_frames::Global::Camera, -20),
                "replace negative source through production registration");
            f.app->CameraPositionX() = pose;
            Check(nativeCandidate->IsCurrent(), "negative source replacement leaves positive target source current");
            break;
        }
        case 1:
            Check(negative_frames::SelectCell(f.app.get(), -20, 0, false), "change negative selection after preparation");
            break;
        case 2: f.app->CurrentFrame() = -4; break;
        case 3: f.app->CameraKeys()[0].eye[0] += 1; break;
        case 4: f.app->CameraPositionX() += 1; break;
        case 5: f.app->CameraMode() = 0; break;
        }
        const Snapshot changed(f);
        Check(!candidate->IsCurrent() && changed.Same(f),
            "negative source selection frame zero baseline live pose or initial timeline mutation stales candidate without writes");
        // New preparations may legitimately sample a new playhead or merge new
        // selection. Authored source replacement must reject the retained plan.
        if (mutation == 0) {
            budget = preparationBudget;
            Check(PrepareNegative(negative, true, true, *nativeCandidate, budget, candidate) == MMS_E_STALE &&
                !candidate && budget == preparationBudget && changed.Same(f),
                "stale negative source reprepare clears existing output preserves budget and external edits");
        }
    }
}

void ForeignGlobalTargetRejected() {
    for (bool hasNegativeState : {false, true}) {
        Fixture foreign, receiving;
        native::Seed(foreign, false);
        foreign.ClearMarks();
        foreign.app->CameraKeys()[0].selected = 1;
        foreign.app->CameraKeys()[0].fov = 81;
        foreign.app->CurrentFrame() = -5;
        if (hasNegativeState) SeedMixed(receiving);
        else receiving.app->CurrentFrame() = -5;

        auto positive = PlanPositive(foreign);
        auto foreignCandidate = PreparePositive(positive, true);
        Check(foreignCandidate->OwnerApp() == foreign.app.get() &&
            foreignCandidate->OwnerApp() != receiving.app.get() &&
            foreignCandidate->ZeroTargets().size() == 1 &&
            foreignCandidate->ZeroTargets()[0].kind == Kind::Camera &&
            foreignCandidate->ZeroTargets()[0].object == nullptr,
            "foreign target is a real current camera candidate without an accessory identity rejection path");
        auto negative = PlanNegative(receiving, !hasNegativeState);
        const Snapshot foreignBefore(foreign), receivingBefore(receiving);
        // Check ordinary rejection, then require the ownership gate to reject
        // before any allocation, both with and without receiving negative state.
        for (long allowance : {-1L, 0L}) {
            auto budget = preparationBudget;
            std::unique_ptr<Candidate> candidate;
            failAfter = allowance;
            const auto result = PrepareNegative(negative, true, true, *foreignCandidate, budget, candidate);
            failAfter = -1;
            Check(result == MMS_E_ARGUMENT && !candidate && budget == preparationBudget,
                "foreign global owner rejects before producing output or consuming preparation budget");
            Check(foreignCandidate->IsCurrent() && foreignBefore.Same(foreign) && receivingBefore.Same(receiving),
                "foreign target rejection preserves both apps keys selection poses authoring storage and history");
        }
    }
}

void BudgetAndAllocationFailures() {
    Fixture f;
    SeedMixed(f);
    const Snapshot before(f);
    auto positive = PlanPositive(f);
    auto nativeCandidate = PreparePositive(positive, true);
    std::shared_ptr<Negative> negative;
    unsigned planFailures = 0;
    for (long allowance = 0; allowance < 512; ++allowance) {
        failAfter = allowance;
        const auto result = negative_frames::PlanSignedSelectedDeletion(*f.app, negative);
        failAfter = -1;
        Check(before.Same(f), "negative planning OOM has no live or journal side effects");
        if (result == MMS_OK) { Check(bool(negative), "bounded negative planning sweep returns a complete plan"); break; }
        Check(result == MMS_E_MEMORY && !negative, "negative planning OOM reports memory and null output");
        ++planFailures;
    }
    Check(negative && planFailures > 0, "negative planning sweep succeeds within 512 allocation cutoffs");
    for (std::uint64_t limit : {std::uint64_t(0), std::uint64_t(1)}) {
        std::unique_ptr<Candidate> candidate;
        auto budget = preparationBudget;
        Check(PrepareNegative(negative, true, true, *nativeCandidate, budget, candidate) == MMS_OK && candidate,
            "seed nonnull output before budget rejection");
        budget = limit;
        Check(PrepareNegative(negative, true, true, *nativeCandidate, budget, candidate) == MMS_E_MEMORY &&
            !candidate && budget == limit && before.Same(f) && nativeCandidate->IsCurrent(),
            "budget rejection clears prior output without consuming budget or publishing either side");
    }
    unsigned failures = 0;
    bool prepared = false;
    for (long allowance = 0; allowance < 2048; ++allowance) {
        auto budget = preparationBudget;
        std::unique_ptr<Candidate> candidate;
        failAfter = allowance;
        const auto result = PrepareNegative(negative, true, true, *nativeCandidate, budget, candidate);
        failAfter = -1;
        Check(before.Same(f) && nativeCandidate->IsCurrent(), "negative preparation OOM never alters either signed source");
        if (result == MMS_OK) {
            Check(candidate && candidate->IsCurrent() && budget < preparationBudget,
                "successful bounded preparation owns a current candidate and charges budget");
            prepared = true;
            break;
        }
        Check(result == MMS_E_MEMORY && !candidate && budget == preparationBudget,
            "every negative preparation OOM returns null output and unchanged caller budget");
        ++failures;
    }
    Check(prepared && failures > 0 && before.Same(f),
        "negative preparation sweep succeeds within 2048 cutoffs and cancellation leaves all live state unchanged");
}
}

inline void NegativeGlobalDeleteTargetChecks() {
    std::puts("  negative global deletion targets: internal joint preparation and ordered publication");
    negative_global_delete_target_checks::DirectionalOverrides();
    negative_global_delete_target_checks::PositiveOnlyWithoutState();
    negative_global_delete_target_checks::StaleCandidates();
    negative_global_delete_target_checks::ForeignGlobalTargetRejected();
    negative_global_delete_target_checks::BudgetAndAllocationFailures();
}
