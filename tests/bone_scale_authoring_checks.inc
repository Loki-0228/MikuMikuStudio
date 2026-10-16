// Include after Fixture/Scale/Check/failAfter; call BoneScaleAuthoringChecks().
#include <stdexcept>
#include <type_traits>
namespace {
using ScaleAuthoring = bone_scale::AuthoringState;
using ScaleRestore = bone_scale::AuthoringRestore;
static_assert(!std::is_copy_constructible<ScaleAuthoring>::value &&
    std::is_nothrow_move_constructible<ScaleAuthoring>::value, "scale authoring is move-only");
static_assert(!std::is_copy_constructible<ScaleRestore>::value &&
    std::is_nothrow_move_constructible<ScaleRestore>::value, "scale restore is move-only");
bone_scale::Curve AuthorCurve(unsigned value = 0) {
    return {static_cast<std::uint8_t>(10 + value), 55, 90, 110};
}
bool PublishScaleWithoutAllocation(ScaleRestore& candidate) {
    failAfter = 0;
    const bool current = candidate.IsCurrent();
    const bool published = candidate.Publish();
    failAfter = -1;
    return current && published;
}
void ScaleAuthoringMissingStorage() {
    Fixture f;
    Check(!bone_scale::HasPoseStorage(f.raw()), "scale authoring fixture begins without side storage");
    auto empty = ScaleAuthoring::Capture(f.raw());
    auto target = empty.Clone();
    const auto blankBytes = empty.RetainedBytes();
    Check(target.RegisterKey(9, Scale(2.5f), true) && target.SetKeyCurve(9, AuthorCurve()),
        "private scale authoring prepares matrix and curve without a live table");
    Check(!bone_scale::HasPoseStorage(f.raw()) && bone_scale::Revision(f.raw()) == 0 &&
        bone_scale::Identity(bone_scale::Key(f.raw(), 9)) && target.RetainedBytes() > blankBytes,
        "private scale mutations charge their nodes without changing live authoring");
    auto prepared = ScaleRestore::Prepare(f.raw(), target, 32);
    Check(prepared->IsCurrent() && bone_scale::HasPoseStorage(f.raw()) &&
        empty.Equals(ScaleAuthoring::Capture(f.raw())) && bone_scale::Revision(f.raw()) == 0,
        "missing table preparation reserves only an empty storage lease");
    failAfter = 0; prepared.reset(); failAfter = -1;
    Check(!bone_scale::HasPoseStorage(f.raw()), "cancelled missing-table preparation releases its empty lease without allocation");
    prepared = ScaleRestore::Prepare(f.raw(), empty, 32);
    Check(!bone_scale::HasPoseStorage(f.raw()) && PublishScaleWithoutAllocation(*prepared) &&
        !bone_scale::HasPoseStorage(f.raw()), "empty missing-table publication needs no registry insertion");
    prepared = ScaleRestore::Prepare(f.raw(), target, 32);
    Check(PublishScaleWithoutAllocation(*prepared) && target.Equals(ScaleAuthoring::Capture(f.raw())) &&
        bone_scale::Revision(f.raw()) == 1 && !prepared->Publish() && !prepared->IsCurrent(),
        "reserved missing-table authoring publishes once without allocation and advances revision");
    const auto revision = bone_scale::Revision(f.raw());
    prepared = ScaleRestore::Prepare(f.raw(), target, 32);
    Check(PublishScaleWithoutAllocation(*prepared) && bone_scale::Revision(f.raw()) == revision,
        "identical authoring publication does not advance revision");
    prepared = ScaleRestore::Prepare(f.raw(), empty, 32);
    Check(PublishScaleWithoutAllocation(*prepared) && empty.Equals(ScaleAuthoring::Capture(f.raw())) &&
        bone_scale::Revision(f.raw()) == revision + 1,
        "undo to empty keys equals absent storage while keeping reusable live storage");
    auto draft = target.Clone();
    Check(draft.RegisterKey(9, Scale(3), false) && draft.KeyCurve(9) == AuthorCurve(),
        "ordinary private registration preserves an overwritten key curve");
    Check(draft.RegisterKey(9, Scale(4), true) && bone_scale::Linear(draft.KeyCurve(9)),
        "fresh private registration clears the old slot curve");
    Check(draft.SetKeyCurve(12, AuthorCurve(2)) && draft.ClearKey(9) && draft.ClearKey(12) && draft.Equals(empty),
        "clear key removes matrices and curve-only slots");
    auto invalid = Scale(-2);
    Check(!draft.SetKey(-1, Scale(2)) && !draft.SetKey(1, invalid) && !draft.ClearKey(-1) &&
        !draft.SetKeyCurve(-1, AuthorCurve()) && draft.Equals(empty), "invalid private scale edits leave the draft unchanged");
}
void ScaleAuthoringRingsPoseAndCache() {
    Fixture f;
    for (auto& bone : f.bones) for (int i = 0; i < 4; ++i) bone.matInit[i * 5] = 1;
    // Populate both legacy rings. The independent snapshot must not copy any
    // of these 60 snapshots or replace them when key authoring is published.
    for (unsigned ring = 0; ring < 30; ++ring) {
        f.m->undoState[0] = ring;
        bone_scale::SetPose(f.raw(), 0, Scale(float(2 + ring)));
        bone_scale::SetKey(f.raw(), 3, Scale(float(100 + ring)));
        bone_scale::SetKeyCurve(f.raw(), 3, AuthorCurve(ring));
        bone_scale::CaptureUndo(f.raw());
        bone_scale::Undo(f.raw(), int(ring));
    }
    failAfter = 10000;
    auto before = ScaleAuthoring::Capture(f.raw());
    const auto captureAllocations = 10000 - failAfter;
    failAfter = -1;
    Check(captureAllocations < 12, "independent scale capture does not allocate copies of 30 undo and redo rings");
    auto target = before.Clone();
    target.RegisterKey(3, Scale(4.125f), false);
    const auto retained = before.RetainedBytes();
    Check(target.RetainedBytes() == retained && target.KeyCurve(3) == AuthorCurve(29),
        "retained scale snapshot budget follows stored nodes rather than matrix values or native rings");
    bone_scale::SetPose(f.raw(), 1, Scale(2.75f));
    Check(before.Equals(ScaleAuthoring::Capture(f.raw())), "key snapshot equality ignores current pose changes and revision");
    const auto pose = bone_scale::PoseValues(f.raw());
    bone_scale::ApplyTransforms(f.raw());
    Check(f.bones[0].matInit[0] == 31 && f.bones[1].matInit[0] == 2.75f, "scale authoring fixture starts with applied derived transforms");
    const auto revision = bone_scale::Revision(f.raw());
    auto prepared = ScaleRestore::Prepare(f.raw(), target, 32);
    bone_scale::RestoreTransforms(f.raw()); bone_scale::ApplyTransforms(f.raw());
    Check(prepared->IsCurrent(), "scale candidate ignores render-cache rebuilds");
    Check(PublishScaleWithoutAllocation(*prepared) && bone_scale::PoseValues(f.raw()) == pose &&
        bone_scale::Revision(f.raw()) == revision + 1 && f.bones[0].matInit[0] == 1 && f.bones[1].matInit[0] == 1,
        "scale key publication preserves all pose values and invalidates applied transforms without allocation");
    bone_scale::ApplyTransforms(f.raw());
    Check(f.bones[0].matInit[0] == 31 && f.bones[1].matInit[0] == 2.75f,
        "unchanged scale poses rebuild correctly after key publication");
    for (unsigned ring = 0; ring < 30; ++ring) {
        bone_scale::Redo(f.raw(), int(ring));
        Check(bone_scale::Pose(f.raw(), 0) == Scale(float(2 + ring)) &&
            bone_scale::Key(f.raw(), 3) == Scale(float(100 + ring)) && bone_scale::KeyCurve(f.raw(), 3) == AuthorCurve(ring),
            "independent publication preserves each existing legacy redo slot");
    }
    for (unsigned ring = 0; ring < 30; ++ring) {
        bone_scale::Undo(f.raw(), int(ring));
        Check(bone_scale::Pose(f.raw(), 0) == Scale(float(2 + ring)) &&
            bone_scale::Key(f.raw(), 3) == Scale(float(100 + ring)) && bone_scale::KeyCurve(f.raw(), 3) == AuthorCurve(ring),
            "independent publication preserves each existing legacy undo slot");
    }
}
void ScaleAuthoringStaleAndLeases() {
    Fixture f;
    auto target = ScaleAuthoring::Capture(f.raw()); target.SetKey(3, Scale(4));
    auto prepared = ScaleRestore::Prepare(f.raw(), target, 32);
    bone_scale::Forget(f.raw());
    bone_scale::ClearKey(f.raw(), 3); // Recreate an empty, revision-zero table.
    Check(!prepared->IsCurrent() && !prepared->Publish(), "same-content same-revision recreated scale storage rejects ABA publication");
    prepared.reset();
    Check(bone_scale::HasPoseStorage(f.raw()), "cancelled stale lease cannot erase replacement storage");
    bone_scale::Forget(f.raw());
    prepared = ScaleRestore::Prepare(f.raw(), target, 32);
    bone_scale::CaptureUndo(f.raw());
    prepared.reset();
    Check(bone_scale::HasPoseStorage(f.raw()), "cancelling a lease retains storage acquired by a native undo ring");
    bone_scale::SetKey(f.raw(), 3, Scale(7)); bone_scale::Undo(f.raw(), f.m->undoState[0]);
    Check(bone_scale::Identity(bone_scale::Key(f.raw(), 3)), "native snapshot written during a lease survives its cancellation");
    bone_scale::SetKey(f.raw(), 3, Scale(2));
    auto before = ScaleAuthoring::Capture(f.raw());
    prepared = ScaleRestore::Prepare(f.raw(), target, 32);
    bone_scale::SetKey(f.raw(), 3, Scale(3)); bone_scale::SetKey(f.raw(), 3, Scale(2));
    Check(before.Equals(ScaleAuthoring::Capture(f.raw())) && !prepared->IsCurrent() && !prepared->Publish(),
        "candidate revision detects intervening scale edits even when key contents are restored");
    prepared = ScaleRestore::Prepare(f.raw(), target, 32);
    bone_scale::SetPose(f.raw(), 1, Scale(3));
    Check(!prepared->IsCurrent() && !prepared->Publish(), "pose authoring after preparation invalidates its revision guard");
    prepared = ScaleRestore::Prepare(f.raw(), target, 32);
    bone_scale::CurveTableState changed;
    Check(bone_scale::CaptureCurveTable(f.raw(), changed), "capture same-revision curve mutation fixture");
    changed.curves[3] = AuthorCurve();
    bone_scale::SwapCurveTable(f.raw(), changed);
    Check(!prepared->IsCurrent() && !prepared->Publish(), "curve content guard rejects mutations even when revision is unchanged");
    prepared = ScaleRestore::Prepare(f.raw(), target, 32);
    bone_scale::Evaluate(f.raw(), 0);
    const auto evaluatedPose = bone_scale::PoseValues(f.raw());
    Check(prepared->IsCurrent() && PublishScaleWithoutAllocation(*prepared) &&
        bone_scale::PoseValues(f.raw()) == evaluatedPose,
        "seek-updated pose is retained because scale key candidates do not restore a captured pose table");
    // Native pool publication is coordinated by the caller. It may precede
    // scale publication after all candidates have passed their current checks.
    auto larger = target.Clone(); larger.RegisterKey(40, Scale(9), true);
    prepared = ScaleRestore::Prepare(f.raw(), larger, 64);
    auto* original = f.m->boneKeys;
    auto* replacement = mdl::BoneKeyPoolCreate(64);
    Check(replacement != nullptr, "scale key candidate fixture allocates replacement native pool");
    f.m->boneKeys = replacement;
    Check(prepared->IsCurrent() && PublishScaleWithoutAllocation(*prepared) && larger.Equals(ScaleAuthoring::Capture(f.raw())),
        "prepared scale keys publish after a separate native pool replacement without inspecting that pool");
    f.m->boneKeys = original; mdl::BoneKeyPoolDestroy(replacement);
    bool rejected = false;
    try { prepared = ScaleRestore::Prepare(f.raw(), larger, 32); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "scale candidate rejects keys outside the explicit target capacity");
}
void ScaleAuthoringStorageTransfers() {
    Fixture first,second;
    bone_scale::SetKey(first.raw(),3,Scale(2));
    bone_scale::SetKey(second.raw(),3,Scale(2));
    auto before=ScaleAuthoring::Capture(first.raw());
    auto target=before.Clone();target.SetKey(3,Scale(4));
    const auto revision=bone_scale::Revision(first.raw());
    auto firstCandidate=ScaleRestore::Prepare(first.raw(),target,32);
    auto secondCandidate=ScaleRestore::Prepare(second.raw(),target,32);
    bone_scale::ExchangeState(first.raw(),second.raw());
    Check(!firstCandidate->IsCurrent()&&!secondCandidate->IsCurrent(),
        "equal-value whole-state exchange invalidates both prepared scale owners");
    bone_scale::ExchangeState(first.raw(),second.raw());
    Check(before.Equals(ScaleAuthoring::Capture(first.raw()))&&
        bone_scale::Revision(first.raw())==revision&&!firstCandidate->IsCurrent()&&
        !secondCandidate->IsCurrent()&&!firstCandidate->Publish()&&!secondCandidate->Publish(),
        "round-trip whole-state exchange cannot revive scale candidates with equal values and revision");
    firstCandidate=ScaleRestore::Prepare(first.raw(),target,32);
    bone_scale::CopyState(first.raw(),second.raw());
    Check(firstCandidate->IsCurrent(),"copying scale state leaves the source storage identity intact");
    bone_scale::Forget(first.raw());
    bone_scale::CopyState(second.raw(),first.raw());
    Check(before.Equals(ScaleAuthoring::Capture(first.raw()))&&
        bone_scale::Revision(first.raw())==revision&&!firstCandidate->IsCurrent()&&!firstCandidate->Publish(),
        "copying equal authoring back into recreated storage cannot import the old scale identity");
    // Legacy history privately copies State, but must publish only its payload.
    auto legacy=bone_scale::PrepareHistory(first.raw(),{});
    firstCandidate=ScaleRestore::Prepare(first.raw(),target,32);
    Check(legacy->IsCurrent(),"legacy scale history fixture is current before no-op publication");
    failAfter=0;legacy->Publish();failAfter=-1;
    Check(firstCandidate->IsCurrent()&&PublishScaleWithoutAllocation(*firstCandidate),
        "no-op legacy history publication preserves the live scale storage identity");
    bone_scale::Forget(first.raw());bone_scale::Forget(second.raw());
    firstCandidate=ScaleRestore::Prepare(first.raw(),target,32);
    bone_scale::ClearKey(second.raw(),3);
    bone_scale::ExchangeState(first.raw(),second.raw());
    bone_scale::ExchangeState(first.raw(),second.raw());
    Check(!firstCandidate->IsCurrent(),"round-trip empty lease exchange invalidates its original candidate");
    failAfter=0;firstCandidate.reset();failAfter=-1;
    Check(bone_scale::HasPoseStorage(first.raw())&&bone_scale::HasPoseStorage(second.raw()),
        "stale lease cancellation cannot erase storage owned by a later whole-state exchange");
}
void ScaleAuthoringFailureChecks() {
    for (bool present : {false, true}) {
        Fixture f;
        if (present) {
            bone_scale::SetPose(f.raw(), 0, Scale(2));
            bone_scale::SetKey(f.raw(), 3, Scale(3));
            bone_scale::SetKeyCurve(f.raw(), 3, AuthorCurve());
            bone_scale::CaptureUndo(f.raw());
        }
        const auto native = f.Digest();
        const auto revision = bone_scale::Revision(f.raw());
        const auto pose = bone_scale::PoseValues(f.raw());
        auto before = ScaleAuthoring::Capture(f.raw());
        auto target = before.Clone(); target.RegisterKey(7, Scale(7), true); target.SetKeyCurve(7, AuthorCurve(2));
        unsigned failures = 0; bool succeeded = false;
        for (long limit = 0; limit < 512; ++limit) {
            std::unique_ptr<ScaleRestore> prepared;
            try { failAfter = limit; prepared = ScaleRestore::Prepare(f.raw(), target, 32); }
            catch (const std::bad_alloc&) { ++failures; }
            failAfter = -1;
            Check(before.Equals(ScaleAuthoring::Capture(f.raw())) && bone_scale::PoseValues(f.raw()) == pose &&
                bone_scale::Revision(f.raw()) == revision && f.Digest() == native,
                "failed or completed scale preparation leaves native fields authoring pose and revision unchanged");
            if (prepared) Check(prepared->IsCurrent(), "successful scale preparation remains current after allocation sweep");
            succeeded = prepared != nullptr;
            failAfter = 0; prepared.reset(); failAfter = -1;
            Check(bone_scale::HasPoseStorage(f.raw()) == present,
                "every failed preparation and cancellation preserves storage presence without stray empty tables");
            if (succeeded) break;
        }
        Check(succeeded && failures > 5, "scale allocation sweep covers map copies and missing storage reservation");
        auto prepared = ScaleRestore::Prepare(f.raw(), target, 32);
        Check(PublishScaleWithoutAllocation(*prepared) && target.Equals(ScaleAuthoring::Capture(f.raw())) &&
            bone_scale::PoseValues(f.raw()) == pose, "successful scale publication after allocation failures preserves unrelated pose");
    }
    Fixture f;
    bone_scale::SetKey(f.raw(), 3, Scale(2)); bone_scale::SetKeyCurve(f.raw(), 3, AuthorCurve());
    auto baseline = ScaleAuthoring::Capture(f.raw());
    unsigned failures = 0; bool succeeded = false;
    for (long limit = 0; limit < 64; ++limit) {
        try { failAfter = limit; auto copy = baseline.Clone(); copy.RegisterKey(9, Scale(3), true); succeeded = true; }
        catch (const std::bad_alloc&) { ++failures; }
        failAfter = -1;
        Check(baseline.Equals(ScaleAuthoring::Capture(f.raw())), "private scale clone/register allocation failures leave source and live authoring intact");
        if (succeeded) break;
    }
    Check(succeeded && failures > 3, "private scale authoring allocation sweep reaches cloning and new key insertion");
}
}
void BoneScaleAuthoringChecks() {
    std::puts("RUN ScaleAuthoringMissingStorage"); ScaleAuthoringMissingStorage();
    std::puts("RUN ScaleAuthoringRingsPoseAndCache"); ScaleAuthoringRingsPoseAndCache();
    std::puts("RUN ScaleAuthoringStaleAndLeases"); ScaleAuthoringStaleAndLeases();
    std::puts("RUN ScaleAuthoringStorageTransfers"); ScaleAuthoringStorageTransfers();
    std::puts("RUN ScaleAuthoringFailureChecks"); ScaleAuthoringFailureChecks();
}
