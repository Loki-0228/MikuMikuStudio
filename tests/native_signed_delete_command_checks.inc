// Include after native_history_mode_checks.inc and
// negative_global_delete_target_checks.inc (and their native global helpers).
// Keep this success regression separate from the cross-zero PASTE refusal.
#include "../src/plugins/native_signed_key_delete.hpp"
#include "mikumikustudio/frame_limits.hpp"

namespace native_signed_delete_command_checks {
using namespace mikumikustudio;
namespace participants = negative_global_delete_target_checks;
namespace native = global_delete_target_checks;
struct Fixture : participants::Fixture {
    Fixture() {
        app->MainWindow()=CreateWindowExW(0,L"STATIC",L"signed deletion commands",WS_OVERLAPPED,
            0,0,320,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(bool(app->MainWindow()),"real command history owns a hidden window on this thread");
        for(int id:{panel::kUndoButton,panel::kRedoButton})
            Check(bool(CreateWindowExW(0,L"BUTTON",L"",WS_CHILD,0,0,20,20,app->MainWindow(),
                reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr)),"create history readout buttons");
    }
    ~Fixture() {
        plugins::ForgetBoundObjectUi(app.get());
        DestroyWindow(app->MainWindow());app->MainWindow()=nullptr;
    }
};
using Snapshot = participants::Snapshot;
using Keys = global_key_delete_checks::Keys;
using Mode = plugins::SharedHistory::RecordingMode;
using Transaction = plugins::SharedHistory::NativeTransaction;
enum class Route { Menu423, KeyboardDirect };
enum class Selection { Root, Positive, Maximum, NegativeOnly, PositiveRootOnly, None };

struct ActiveApp {
    MMDApp* previous;
    explicit ActiveApp(Fixture& f) : previous(g_Block) { g_Block = f.app.get(); }
    ~ActiveApp() { g_Block = previous; }
};

void Seek(Fixture& f, int frame) {
    ActiveApp active(f);
    // Use the actual evaluators on both fixtures; never reproduce interpolation.
    native::EvaluateProduction(f, frame);
    if (frame < 0) negative_frames::EvaluateGlobals(f.app.get(), frame);
}

void CheckNoPrivateHistory(Fixture& f) {
    ActiveApp active(f);
    const int frame = f.app->CurrentFrame();
    f.app->CurrentFrame() = -5;
    const Snapshot before(f);
    // These are the old negative ring entry points, NOT shared-history undo.
    // A negative cursor makes an accidental non-drag legacy entry observable.
    Check(!negative_frames::Command(f.app.get(), 400) &&
        !negative_frames::Command(f.app.get(), 401) && before.Same(f),
        "signed global command leaves no extra private negative undo or redo ring");
    f.app->CurrentFrame() = frame;
}

void Seed(Fixture& f, bool authored, Selection selection, int frame) {
    ActiveApp active(f);
    Check(!plugins::BoundNativePoseHistory(f.app.get()),
        "seed curves and negative sources before binding the command history");
    participants::SeedMixed(f, authored);
    f.ClearMarks();
    if (selection == Selection::Root || selection == Selection::PositiveRootOnly) f.Mark(0);
    if (selection == Selection::Positive) f.Mark(1);
    if (selection == Selection::Maximum) {
        Check(!authored, "maximum-frame case uses native sources without a stale authored frame map");
        f.app->CameraKeys()[2].frame = frame_limits::maximum;
        f.app->LightKeys()[2].frame = frame_limits::maximum;
        f.app->ShadowKeys()[2].frame = frame_limits::maximum;
        f.app->GravityKeys()[2].frame = frame_limits::maximum;
        for (unsigned slot : {7u, 8u}) f.app->AccessoryKeys(slot)[2].frame = frame_limits::maximum;
        f.Mark(2);
    }
    if (selection == Selection::None || selection == Selection::PositiveRootOnly)
        negative_frames::SelectRange(f.app.get(), 1, 2);
    f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
    f.accessories[0].rowSelected = 1;
    f.accessories[1].rowSelected = 0;

    // Real unbound registrations may have setup-only private history. Reload
    // their exact serialized data/selection so a later legacy probe starts empty.
    const auto keys = NegativeTestRecord(*f.app, negative_frames::kPmmKey);
    const auto marks = NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey);
    pmm_extension::Document document;
    negative_frames::Capture(*f.app, document);
    Check(negative_frames::Load(*f.app, document) &&
        keys == NegativeTestRecord(*f.app, negative_frames::kPmmKey) &&
        marks == NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey),
        "discard setup private history through exact negative data and selection reload");
    Seek(f, frame);
    Check(f.history.Reset() == MMS_OK, "signed command setup has no common journal entries");
    f.history.Collect();
    f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
    CheckNoPrivateHistory(f);
}

// Independent oracle: the pre-existing real directional participants only.
// It never calls PlanSignedKeyDeletion and never installs a test binding.
struct Oracle {
    Fixture& f;
    std::shared_ptr<participants::Positive> positive;
    std::shared_ptr<participants::Negative> negative;
    Oracle(Fixture& source, bool fallback, bool expectPositive, bool expectNegative) : f(source) {
        ActiveApp active(f);
        const Snapshot before(f);
        Check(plugins::PlanSignedGlobalKeyDeletion(*f.app, positive, fallback) == MMS_OK &&
            bool(positive) == expectPositive, "independent native oracle has exactly the expected selection");
        Check(negative_frames::PlanSignedSelectedDeletion(*f.app, negative, bool(positive)) == MMS_OK &&
            bool(negative) == expectNegative && before.Same(f),
            "independent negative oracle plans selected keys or the native zero dependency without writes");
    }
    void Publish(bool forward, bool initial) {
        ActiveApp active(f);
        const Snapshot before(f);
        std::unique_ptr<participants::PositiveCandidate> p;
        std::unique_ptr<plugins::PreparedHistoryChange> n;
        if (positive) p = participants::PreparePositive(positive, forward);
        if (negative) {
            negative_frames::DeletionTargets targets;
            targets.globals = p.get();
            auto budget = participants::preparationBudget;
            Check(negative->PrepareSigned(forward, initial, targets, budget, n) == MMS_OK && n,
                "independent negative oracle consumes the real directional native targets");
        }
        Check(before.Same(f) && (!p || p->IsCurrent()) && (!n || n->IsCurrent()),
            "both oracle halves prepare against the unchanged source before either publishes");
        failAfter = 0;
        if (p) p->Publish();
        if (n) n->Publish();
        failAfter = -1;
        Check(!f.history.UndoCount() && !f.history.RedoCount(), "detached oracle creates no journal entries");
    }
};

void CheckOracle(Fixture& f, Fixture& oracle) {
    Check(Keys(*oracle.app).Same(*f.app) && native::AuthorsSame(f, native::AuthorsOf(oracle)),
        "real signed command matches independent native pools marks and exact sparse authoring");
    Check(NegativeTestRecord(*f.app, negative_frames::kPmmKey) ==
            NegativeTestRecord(*oracle.app, negative_frames::kPmmKey) &&
        NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey) ==
            NegativeTestRecord(*oracle.app, negative_frames::kPmmSelectionKey) &&
        negative_frames::Count(f.app.get()) == negative_frames::Count(oracle.app.get()),
        "real signed command matches independent negative payload selection and count");
    const auto actual = native::PosesOf(f), expected = native::PosesOf(oracle);
    for (unsigned track = 0; track < actual.size(); ++track)
        for (unsigned lane = 0; lane < actual[track].size(); ++lane)
            Check(native::Close(actual[track][lane], expected[track][lane]),
                "commit undo and redo sample both signs at their actual playhead using the oracle");
    Check(f.app->GravityNoiseModeWord() == oracle.app->GravityNoiseModeWord() &&
        native::Cursors(f) == native::Cursors(oracle) && native::Active(f) == native::Active(oracle) &&
        f.app->CurrentFrame() == oracle.app->CurrentFrame(),
        "signed command retains exact gravity mode word cursors active flags and the requested playhead");
}

std::vector<std::uint8_t> NegativeCameraSource(Fixture& f) {
    const auto bytes = NegativeTestRecord(*f.app, negative_frames::kPmmKey);
    Check(bytes.size() >= 8 && NegativeTestU32(bytes.data()) == 1 &&
        std::uint64_t(NegativeTestU32(bytes.data() + 4)) * 164 == bytes.size() - 8,
        "surviving negative camera uses the existing serialized source format");
    for (std::size_t offset = 8; offset < bytes.size(); offset += 164) {
        const auto* row = bytes.data() + offset;
        if (NegativeTestU32(row + 4) == 2 && NegativeTestU32(row + 8) == 2 &&
            static_cast<std::int32_t>(NegativeTestU32(row + 12)) == -20)
            return {row, row + 164};
    }
    Check(false, "unselected negative camera source remains present");
    return {};
}

template<class K> bool SameUnselectedPayload(const K& actual, const K& expected) {
    // Deletion may reconnect neighbors; payload, marks and reserved bytes stay.
    K copy;
    std::memcpy(&copy, &actual, sizeof(K));
    copy.previous = expected.previous;
    copy.next = expected.next;
    return !std::memcmp(&copy, &expected, sizeof(K));
}
void CheckSources(Fixture& f, const Keys& before, unsigned row,
    const std::vector<std::uint8_t>& camera) {
    Check(SameUnselectedPayload(f.app->CameraKeys()[row], before.camera[row]) &&
        SameUnselectedPayload(f.app->LightKeys()[row], before.light[row]) &&
        SameUnselectedPayload(f.app->ShadowKeys()[row], before.shadow[row]) &&
        SameUnselectedPayload(f.app->GravityKeys()[row], before.gravity[row]) &&
        SameUnselectedPayload(f.app->AccessoryKeys(7)[row], before.accessories[7][row]) &&
        SameUnselectedPayload(f.app->AccessoryKeys(8)[row], before.accessories[8][row]),
        "all six unselected positive source payloads marks and reserved bytes survive both directions");
    Check(camera == NegativeCameraSource(f) && !negative_frames::CellSelected(f.app.get(), -20, 0),
        "unselected negative camera retains its entire authored source and remains unselected");
}

void Delete(Fixture& f, Route route) {
    ActiveApp active(f);
    if (route == Route::Menu423) CmdControl400(f.app.get(), f.app->MainWindow(), 423, 0);
    else Check(plugins::TryDeleteBoundKeys(f.app.get(), true),
        "keyboard direct route is owned by the real bound deletion command");
    // KeyboardDirect is PumpDeleteRebuild's actual command call. This does not
    // claim end-to-end VK_DELETE polling, foreground or focus dispatch coverage.
}
void Replay(Fixture& f, bool redo) {
    ActiveApp active(f);
    CmdControl400(f.app.get(), f.app->MainWindow(), redo ? 401 : 400, 0);
    Check(f.history.UndoCount() == (redo ? 1u : 0u) && f.history.RedoCount() == (redo ? 0u : 1u),
        "one real menu undo or redo moves the entire signed deletion as exactly one journal node");
}

void CommandCase(Route route, Mode mode, bool authored, Selection selection, int frame) {
    std::printf("  signed global command route=%s mode=%d authored=%u selection=%u frame=%d\n",
        route == Route::Menu423 ? "menu423" : "keyboard-direct", int(mode), unsigned(authored), unsigned(selection), frame);
    std::fflush(stdout);
    Fixture reference;
    Seed(reference, authored, selection, frame);
    Fixture f;
    Seed(f, authored, selection, frame);
    Check(f.history.SetMode(mode) == MMS_OK, "signed global command chooses the requested recording mode");
    const bool fallback = selection == Selection::None && route == Route::KeyboardDirect;
    const bool positive = selection == Selection::Root || selection == Selection::Positive ||
        selection == Selection::Maximum || selection == Selection::PositiveRootOnly || (fallback && frame == 12);
    const bool selectedNegative = selection != Selection::None && selection != Selection::PositiveRootOnly;
    const bool changed = positive || selectedNegative;
    // Pure negative selection explicitly disables the positive oracle fallback.
    Oracle oracle(reference, fallback, positive, changed);
    const Keys original(*f.app);
    const auto originalAuthors = native::AuthorsOf(f);
    const auto originalNegative = NegativeTestRecord(*f.app, negative_frames::kPmmKey);
    const auto camera = NegativeCameraSource(f);
    const auto oldPose = native::PosesOf(f);
    const unsigned survivor = selection == Selection::Maximum ? 1u : 2u;
    oracle.Publish(true, true);

    // The only binding in this case starts AFTER every fixture/curve write and
    // oracle plan. Oracle replay below also stays unbound: no generation churn.
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    const Snapshot before(f);
    Delete(f, route);
    if (!changed) {
        Check(before.Same(f), "unselected menu or nonpositive keyboard fallback is a complete no-op");
        CheckNoPrivateHistory(f);
        return;
    }
    Check(f.history.UndoCount() == 1 && !f.history.RedoCount() && f.app->SceneModified() &&
        f.app->RecordedFrameCountByte1() && f.app->CurrentFrame() == frame,
        "successful signed command records exactly one explicit dirty transaction in either mode");
    CheckOracle(f, reference);
    CheckSources(f, original, survivor, camera);
    CheckNoPrivateHistory(f);
    if ((selection == Selection::Root || selection == Selection::PositiveRootOnly) && frame < 0)
        Check(f.app->CameraRotation()[0] != oldPose[0][3],
            "positive root deletion resamples the unselected negative camera at the negative playhead");
    if (selection == Selection::NegativeOnly)
        Check(original.Same(*f.app) && native::AuthorsSame(f, originalAuthors),
            "pure negative selection suppresses keyboard current-row fallback on the positive side");
    if (selection == Selection::None)
        Check(!f.app->CameraKeys()[1].frame && !f.app->AccessoryKeys(7)[1].frame &&
            !std::memcmp(&f.app->LightKeys()[1], &original.light[1], sizeof(mdl::LightKey)) &&
            !std::memcmp(&f.app->AccessoryKeys(8)[1], &original.accessories[8][1], sizeof(mdl::AccessoryKey)),
            "unselected keyboard fallback still deletes only the current camera and selected accessory row");
    const Keys forward(*f.app);
    const auto forwardAuthors = native::AuthorsOf(f);
    const auto forwardNegative = NegativeTestRecord(*f.app, negative_frames::kPmmKey);
    // Both first undo and first redo run on the OTHER sign, at different frames.
    // A second pair returns to the submission frame and checks exact targets.
    const std::array<int, 4> replayFrames{{frame < 0 ? 7 : -7, frame < 0 ? 23 : -3, frame, frame}};
    for (unsigned step = 0; step < replayFrames.size(); ++step) {
        const bool redo = step % 2 != 0;
        Seek(reference, replayFrames[step]);
        Seek(f, replayFrames[step]);
        oracle.Publish(redo, false);
        Replay(f, redo);
        CheckOracle(f, reference);
        CheckSources(f, original, survivor, camera);
        CheckNoPrivateHistory(f);
        Check((redo ? forward.Same(*f.app) : original.Same(*f.app)) &&
            native::AuthorsSame(f, redo ? forwardAuthors : originalAuthors) &&
            NegativeTestRecord(*f.app, negative_frames::kPmmKey) == (redo ? forwardNegative : originalNegative),
            "paired undo and redo restore exact original or forward data on both sides of zero");
    }
}

std::shared_ptr<plugins::HistoryChange> Plan(Fixture& f) {
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanSignedKeyDeletion(*f.app, f.history, false, true, change) == MMS_OK && change,
        "auxiliary signed aggregate plans the real mixed global selection");
    return change;
}
MmsResult Prepare(Fixture& f, const std::shared_ptr<plugins::HistoryChange>& change,
    std::unique_ptr<Transaction>& transaction) {
    return f.history.PrepareNative("signed global delete regression", change, transaction,
        plugins::SharedHistory::NativeIntent::ExplicitCommit);
}

void TraceAdmission(Mode mode, const char* phase, long detail = -1) {
    // Flush BEFORE arming failAfter. Diagnostics must never become an injected
    // allocation or obscure the last entered phase after a fail-fast exit.
    std::printf("  signed global admission mode=%d phase=%s detail=%ld\n", int(mode), phase, detail);
    std::fflush(stdout);
}

void CancelStaleAndBudget() {
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) {
        Fixture f;
        Seed(f, true, Selection::Root, -5);
        Check(f.history.SetMode(mode) == MMS_OK, "auxiliary admission chooses recording mode");
        plugins::NativePoseTestBinding binding(*f.app, f.history);
        const Snapshot before(f);
        TraceAdmission(mode, "cancel-plan");
        auto change = Plan(f);
        std::unique_ptr<Transaction> transaction;
        TraceAdmission(mode, "cancel-prepare");
        Check(Prepare(f, change, transaction) == MMS_OK && transaction,
            "prepare the whole signed command before cancellation");
        TraceAdmission(mode, "cancel-release");
        transaction.reset();
        Check(before.Same(f), "cancelling signed admission leaves both sides pose selection revision and history unchanged");
        for (unsigned mutation = 0; mutation < 3; ++mutation) {
            TraceAdmission(mode, "stale-prepare", mutation);
            change = Plan(f);
            Check(Prepare(f, change, transaction) == MMS_OK && transaction,
                "prepare both halves before a deliberate external source mutation");
            if (mutation == 0) f.app->LightKeys()[2].color[0] += .125f;
            if (mutation == 1) Check(negative_frames::SelectCell(f.app.get(), -20, 0, true),
                "change surviving negative selection after signed preparation");
            if (mutation == 2) f.app->CurrentFrame() = -4;
            const Snapshot external(f);
            TraceAdmission(mode, "stale-commit", mutation);
            Check(f.history.CommitNative(std::move(transaction)) == MMS_E_STALE && external.Same(f),
                "stale native source negative selection or playhead refuses both halves without overwriting external state");
        }
        TraceAdmission(mode, "budget-configure");
        Check(f.history.Configure(mode, 32, 1) == MMS_OK, "apply a deliberately insufficient shared history budget");
        const Snapshot capped(f);
        change.reset();
        TraceAdmission(mode, "budget-plan");
        const auto planned = plugins::PlanSignedKeyDeletion(*f.app, f.history, false, true, change);
        if (planned == MMS_OK) {
            TraceAdmission(mode, "budget-prepare");
            Check(change && Prepare(f, change, transaction) == MMS_E_MEMORY && !transaction,
                "history byte cap rejects signed admission before either half writes");
        } else Check(planned == MMS_E_MEMORY && !change,
            "early history byte cap refusal exposes no partial signed plan");
        Check(capped.Same(f), "history byte cap refusal preserves both sources pose selection revision and journal");
        CheckNoPrivateHistory(f);
        TraceAdmission(mode, "cancel-stale-budget-complete");
    }
}

void AllocationSamples() {
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) {
        Fixture reference;
        Seed(reference, true, Selection::Root, -5);
        Fixture f;
        Seed(f, true, Selection::Root, -5);
        Check(f.history.SetMode(mode) == MMS_OK, "allocation samples choose explicit command recording mode");
        Oracle oracle(reference, false, true, true);
        oracle.Publish(true, true);
        plugins::NativePoseTestBinding binding(*f.app, f.history);
        const Snapshot before(f);
        unsigned planFailures = 0, prepareFailures = 0;
        // Deliberately sampled failure points, not a claimed exhaustive sweep.
        for (long allowance : {0L, 1L, 2L, 8L, 32L, 128L}) {
            std::shared_ptr<plugins::HistoryChange> change;
            TraceAdmission(mode, "oom-plan", allowance);
            failAfter = allowance;
            const auto result = plugins::PlanSignedKeyDeletion(*f.app, f.history, false, true, change);
            failAfter = -1;
            if (result == MMS_OK) Check(bool(change), "successful allocation sample returns the complete signed plan");
            else { Check(result == MMS_E_MEMORY && !change, "signed planning OOM exposes no partial plan"); ++planFailures; }
            change.reset();
            Check(before.Same(f), "sampled planning OOM and cancellation preserve both signed sources");
        }
        auto change = Plan(f);
        for (long allowance : {0L, 1L, 2L, 8L, 32L, 128L}) {
            std::unique_ptr<Transaction> transaction;
            // Construct the label before failing allocations in admission.
            std::string label = "signed allocation sample";
            TraceAdmission(mode, "oom-prepare", allowance);
            failAfter = allowance;
            const auto result = f.history.PrepareNative(std::move(label), change, transaction,
                plugins::SharedHistory::NativeIntent::ExplicitCommit);
            failAfter = -1;
            if (result == MMS_OK) Check(bool(transaction), "successful allocation sample prepares the whole command");
            else { Check(result == MMS_E_MEMORY && !transaction, "signed admission OOM exposes no partial token"); ++prepareFailures; }
            transaction.reset();
            Check(before.Same(f), "sampled preparation OOM or cancellation cannot publish either signed half");
        }
        Check(planFailures && prepareFailures, "allocation samples actually exercise both planning and preparation refusal");
        std::unique_ptr<Transaction> transaction;
        TraceAdmission(mode, "oom-recovery-prepare");
        Check(Prepare(f, change, transaction) == MMS_OK && transaction,
            "the same signed plan remains admissible after cancelled OOM samples");
        TraceAdmission(mode, "oom-recovery-noalloc-commit");
        failAfter = 0;
        const auto result = f.history.CommitNative(std::move(transaction));
        failAfter = -1;
        Check(result == MMS_OK && f.history.UndoCount() == 1 && !f.history.RedoCount() &&
            negative_frames::Count(f.app.get()) == 1,
            "fully prepared mixed transaction commits both halves without allocation as one entry");
        CheckOracle(f, reference);
        for (bool redo : {false, true}) {
            TraceAdmission(mode, redo ? "oom-recovery-redo" : "oom-recovery-undo");
            oracle.Publish(redo, false);
            Check(f.history.History(redo) == MMS_OK && f.history.UndoCount() == (redo ? 1u : 0u) &&
                f.history.RedoCount() == (redo ? 0u : 1u),
                "allocation recovery transaction still supports paired undo and redo");
            CheckOracle(f, reference);
        }
        CheckNoPrivateHistory(f);
        TraceAdmission(mode, "oom-complete");
    }
}
} // namespace native_signed_delete_command_checks

void NativeSignedDeleteCommandChecks() {
    using namespace native_signed_delete_command_checks;
    std::puts("  signed global deletion: real menu423 / keyboard direct route, not VK_DELETE end-to-end or paste");
    for (auto route : {Route::Menu423, Route::KeyboardDirect})
        for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) {
            for (bool authored : {false, true}) {
                for (auto selection : {Selection::Root, Selection::Positive})
                    for (int frame : {-5, 0, 7}) CommandCase(route, mode, authored, selection, frame);
                CommandCase(route, mode, authored, Selection::NegativeOnly, 12);
                CommandCase(route, mode, authored, Selection::PositiveRootOnly, -5);
                for (int frame : {-5, 0, 12}) CommandCase(route, mode, authored, Selection::None, frame);
            }
            for (int frame : {-5, 0, 7}) CommandCase(route, mode, false, Selection::Maximum, frame);
        }
    CancelStaleAndBudget();
    AllocationSamples();
}
