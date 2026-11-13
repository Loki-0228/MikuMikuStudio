// Include after native_signed_delete_model_checks.inc, which follows
// negative_model_delete_target_checks.inc and native_model_delete_target_checks.inc.
// Entry: NativeSignedDeleteModelCommandChecks(). No coordinator replacement,
// production evaluator copy, cross-zero paste expectation or fault injection.
// Not covered here: physical VK_DELETE/focus dispatch, maximum-frame keys,
// per-bone selector windows, replay after changing model/binding, dirty-pose/physics rigs, or budget/OOM
// admission (the separate coordinator regressions own those failure cases).
#include "../src/plugins/native_key_commands.hpp"

namespace native_signed_delete_model_command_checks {
using namespace mikumikustudio;
namespace n = negative_model_delete_target_checks;
namespace m = model_delete_target_checks;
namespace coordinator = signed_delete_model_checks;
using Fixture = n::Fixture;
using Mode = plugins::SharedHistory::RecordingMode;
using BoneKeys = std::vector<mdl::BoneKey>;
enum class Route { Menu423, KeyboardDirect };
enum class Selection { Mixed, NegativeOnly, None, Interpolation };

void CheckCommandWindow(Fixture& f) {
    const HWND window = f.app->MainWindow();
    Check(window && IsWindow(window) && !IsWindowVisible(window) &&
        GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId(),
        "model command fixture owns a live hidden main window on the actual history command thread");
    for (int id : {panel::kUndoButton, panel::kRedoButton}) {
        const HWND button = GetDlgItem(window, id);
        Check(button && IsWindow(button) && GetParent(button) == window,
            "model command fixture has real undo and redo buttons attached to its main window");
    }
}

void NoPrivateHistory(Fixture& f) {
    n::Owner owner(f.app.get());
    const int frame = f.app->CurrentFrame();
    f.app->CurrentFrame() = -5;
    const n::Snapshot before(f);
    // Direct negative commands 400/401 expose the old private ring, bypassing
    // shared-history menu dispatch. The negative cursor admits legacy entries.
    Check(!negative_frames::Command(f.app.get(), 400) &&
        !negative_frames::Command(f.app.get(), 401) && before.Matches(f),
        "model command creates no additional private negative undo or redo entry");
    f.app->CurrentFrame() = frame;
}

unsigned Seed(Fixture& f, bool display, bool automatic, Selection selection) {
    n::Owner owner(f.app.get());
    CheckCommandWindow(f);
    Check(!plugins::BoundNativePoseHistory(f.app.get()),
        "model command fixture writes all authored sources before any binding");
    // n::Seed installs a binding internally. Reuse its underlying real fixture
    // and author seed instead, so even oracle SETUP never installs a binding.
    const bool interpolation = selection == Selection::Interpolation;
    auto marks = m::Seed(f, interpolation ? m::Families::Bone : m::Families::All, true);
    f.firstOrder[0].boneIndex = f.secondOrder[0].boneIndex = -1;
    // This fixture has only the whole-model selector at slot zero. Production
    // assigns per-bone selectors from slot one; do not point a bone at the
    // whole-model dummy as the older detached window fixture does.
    for(auto& bone:f.first.bones)bone.slotIndex=-1;
    f.first.dirty = {0, 0};
    f.app->CameraMode() = 0;
    f.app->SelectedModelSlot() = 0;
    f.Automatic(automatic);
    Check(IsDlgButtonChecked(f.app->MainWindow(), panel::kPhysicsFrameCheckbox) ==
        (automatic ? BST_CHECKED : BST_UNCHECKED),
        "real model command interpolation checkbox has the requested state");
    const unsigned positiveSlot = f.first.m->boneKeys[0].next;
    Check(positiveSlot >= 2 && positiveSlot < mdl::BoneKeyCapacity(f.first.m->boneKeys) &&
        f.first.m->boneKeys[positiveSlot].frame > 0 && f.first.selected[0] && !f.first.selected[1],
        "fixture has a real positive key on exactly one viewport-selected bone");
    if (interpolation) {
        const unsigned next = f.first.m->boneKeys[positiveSlot].next;
        Check(next && f.first.m->boneKeys[next].frame > f.first.m->boneKeys[positiveSlot].frame,
            "automatic interpolation fixture deletes an interior native key");
        for (unsigned lane = 0; lane < 4; ++lane) {
            auto& a = f.first.m->boneKeys[positiveSlot].interpolation;
            auto& b = f.first.m->boneKeys[next].interpolation;
            a[lane] = 9 + lane; a[lane + 4] = 71 - lane;
            a[lane + 8] = 98 - lane; a[lane + 12] = 121 - lane;
            b[lane] = 35 + lane; b[lane + 4] = 17 + lane;
            b[lane + 8] = 100 - lane; b[lane + 12] = 82 - lane;
        }
        marks.bones = {positiveSlot};
    }

    f.app->CurrentFrame() = -20;
    f.first.bones[0].trans[0] = 8;
    f.first.bones[0].rotQuat[0] = .6f; f.first.bones[0].rotQuat[3] = .8f;
    Check(bone_scale::SetPose(f.first.raw(), 0, Scale(2)) &&
        negative_frames::RegisterBone(f.first.raw(), 0, -20) &&
        negative_frames::RegisterMorph(f.first.raw(), 0, -20, .9f),
        "seed unselected negative bone rotation scale and morph zero dependencies through real registration");
    f.app->CurrentFrame() = -10;
    if (display) {
        f.first.m->loadComplete = 0; f.firstIk[0].enabled = 0;
        f.firstOrder[0].linkedModel = 1; f.firstOrder[0].linkedBone = 0;
        Check(negative_frames::RegisterDisplay(f.first.raw(), -10) &&
            negative_frames::SelectCell(f.app.get(), -10, 0, false),
            "select actual negative model visibility IK and parent keys");
    } else {
        f.first.bones[1].trans[0] = 14;
        Check(bone_scale::SetPose(f.first.raw(), 1, Scale(3)) &&
            negative_frames::RegisterBone(f.first.raw(), 1, -10) &&
            negative_frames::SelectCell(f.app.get(), -10, 2, false),
            "select actual negative bone key while retaining different unselected dependencies");
    }
    if (selection == Selection::None) negative_frames::SelectRange(f.app.get(), 1, 2);
    if (selection == Selection::Mixed || interpolation) coordinator::Mark(f, marks);
    Check(!negative_frames::CellSelected(f.app.get(), -20, 1) &&
        !negative_frames::CellSelected(f.app.get(), -20, 3),
        "source negative bone and morph remain unselected");

    // Remove only setup history via an exact serialized reload; future private
    // ring probes can then distinguish accidental legacy command publication.
    const auto keys = NegativeTestRecord(*f.app, negative_frames::kPmmKey);
    const auto selected = NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey);
    pmm_extension::Document document;
    negative_frames::Capture(*f.app, document);
    Check(negative_frames::Load(*f.app, document) &&
        keys == NegativeTestRecord(*f.app, negative_frames::kPmmKey) &&
        selected == NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey),
        "unbound model seed reload preserves exact negative payload and selection");
    Check(f.history.Reset() == MMS_OK, "isolate model command from all setup history");
    f.history.Collect();
    coordinator::Frame(f, -5);
    f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
    NoPrivateHistory(f);
    return positiveSlot;
}

// The same verified typed participants as signed_delete_model_checks::Directions.
// No call to the coordinator under test and no oracle binding, including replay.
struct Oracle {
    Fixture& f;
    std::shared_ptr<plugins::ModelKeyDeletion> positive;
    std::shared_ptr<negative_frames::SelectedDeletion> negative;
    Oracle(Fixture& fixture, bool automatic, bool fallback, bool hasPositive, bool changed) : f(fixture) {
        n::Owner owner(f.app.get());
        Check(!plugins::BoundNativePoseHistory(f.app.get()), "typed model oracle has no bound journal");
        const n::Snapshot before(f);
        Check(plugins::PlanSignedModelKeyDeletion(*f.app, 0, automatic, positive, fallback) == MMS_OK &&
            bool(positive) == hasPositive,
            "independent typed native oracle has exactly the expected model selection");
        Check(negative_frames::PlanSignedSelectedDeletion(*f.app, negative, bool(positive)) == MMS_OK &&
            bool(negative) == changed && before.Matches(f),
            "independent typed negative oracle plans selection and zero dependencies without writes");
    }
    void Publish(bool forward, bool initial) {
        n::Owner owner(f.app.get());
        Check(!plugins::BoundNativePoseHistory(f.app.get()), "model oracle replay never changes the active binding");
        const n::Snapshot before(f);
        std::unique_ptr<plugins::PreparedModelKeyDeletion> p;
        std::unique_ptr<plugins::PreparedHistoryChange> q;
        if (positive) Check(positive->PrepareSigned(forward, p) == MMS_OK && p,
            "typed model oracle prepares the real signed directional target");
        if (negative) {
            negative_frames::DeletionTargets targets{nullptr, p.get(), 0};
            auto remaining = n::kPreparationBytes;
            const auto result=negative->PrepareSigned(forward, initial, targets, remaining, q);
            if(result)std::fprintf(stderr,"model oracle negative result=%d forward=%d initial=%d frame=%d positive=%d\n",int(result),forward,initial,f.app->CurrentFrame(),bool(p));
            Check(result == MMS_OK && q,
                "typed negative oracle consumes the native bone morph display and parent targets");
        }
        Check(before.Matches(f) && (!p || p->IsCurrent()) && (!q || q->IsCurrent()),
            "oracle validates both prepared halves against the old source before any publication");
        failAfter = 0;
        if (p) p->Publish();
        if (q) q->Publish();
        failAfter = -1;
        Check(!f.history.UndoCount() && !f.history.RedoCount(), "typed oracle never records shared history");
    }
};

std::vector<std::uint8_t> UnselectedNegativeSources(Fixture& f) {
    const auto data = NegativeTestRecord(*f.app, negative_frames::kPmmKey);
    Check(data.size() >= 8 && NegativeTestU32(data.data()) == 1 &&
        std::uint64_t(NegativeTestU32(data.data() + 4)) * 164 == data.size() - 8,
        "model source assertions use the existing serialized negative record format");
    std::vector<std::uint8_t> sources;
    for (std::size_t offset = 8; offset < data.size(); offset += 164) {
        const auto* row = data.data() + offset;
        if (NegativeTestU32(row) == 0 && NegativeTestU32(row + 4) <= 1 && NegativeTestU32(row + 8) == 0 &&
            static_cast<std::int32_t>(NegativeTestU32(row + 12)) == -20)
            sources.insert(sources.end(), row, row + 164);
    }
    Check(sources.size() == 2 * 164, "both unselected negative bone and morph sources remain present");
    return sources;
}

void MatchesOracle(Fixture& f, Fixture& reference) {
    m::PayloadsMatch(f, reference);
    Check(m::Live(reference).Matches(f) && f.app->CurrentFrame() == reference.app->CurrentFrame(),
        "real model command matches typed live bone scale dirty morph visibility IK and parent-window targets");
    Check(NegativeTestRecord(*f.app, negative_frames::kPmmKey) ==
            NegativeTestRecord(*reference.app, negative_frames::kPmmKey) &&
        NegativeTestRecord(*f.app, negative_frames::kPmmSelectionKey) ==
            NegativeTestRecord(*reference.app, negative_frames::kPmmSelectionKey) &&
        negative_frames::Count(f.app.get()) == negative_frames::Count(reference.app.get()),
        "real model command matches typed negative payload selection and count in every direction");
}

void HistoryPosition(Fixture& f, bool forward) {
    Check(f.history.UndoCount() == (forward ? 1u : 0u) && f.history.RedoCount() == (forward ? 0u : 1u),
        "one model menu or keyboard deletion moves as exactly one shared undo/redo entry");
    Check(bool(IsWindowEnabled(GetDlgItem(f.app->MainWindow(), panel::kUndoButton))) == forward &&
        bool(IsWindowEnabled(GetDlgItem(f.app->MainWindow(), panel::kRedoButton))) == !forward,
        "real command refreshes the fixture undo and redo buttons from the single shared entry");
}
void Delete(Fixture& f, Route route) {
    n::Owner owner(f.app.get());
    CheckCommandWindow(f);
    if (route == Route::Menu423) CmdControl400(f.app.get(), f.app->MainWindow(), 423, 0);
    else Check(plugins::TryDeleteBoundKeys(f.app.get(), true),
        "actual keyboard direct command owns model deletion and fallback");
    // This is the command invoked by PumpDeleteRebuild, not an end-to-end
    // physical key, foreground/focus or keyboard polling simulation.
}

BoneKeys CommandCase(Route route, Mode mode, bool automatic, bool display,
    Selection selection, int requestedFrame, bool atPositiveKey = false) {
    std::printf("  signed model command route=%s mode=%d automatic=%u display=%u selection=%u frame=%d currentKey=%u\n",
        route == Route::Menu423 ? "menu423" : "keyboard-direct", int(mode), unsigned(automatic), unsigned(display),
        unsigned(selection), requestedFrame, unsigned(atPositiveKey));
    std::fflush(stdout);
    Fixture reference;
    const unsigned referenceSlot = Seed(reference, display, automatic, selection);
    Fixture f;
    const unsigned positiveSlot = Seed(f, display, automatic, selection);
    const int keyFrame = int(f.first.m->boneKeys[positiveSlot].frame);
    Check(referenceSlot == positiveSlot && int(reference.first.m->boneKeys[referenceSlot].frame) == keyFrame,
        "independent fixtures agree on the existing positive fallback key");
    const int frame = atPositiveKey ? keyFrame : requestedFrame;
    coordinator::Frame(reference, frame); coordinator::Frame(f, frame);
    Check(f.history.SetMode(mode) == MMS_OK, "model command chooses the requested history recording mode");
    const bool fallback = selection == Selection::None && route == Route::KeyboardDirect;
    const bool hasPositive = selection == Selection::Mixed || selection == Selection::Interpolation ||
        (fallback && frame == keyFrame);
    const bool changed = hasPositive || selection != Selection::None;
    // In the negative-only case fallback is false in this independent oracle,
    // even though the actual keyboard route always requests current-frame fallback.
    Oracle oracle(reference, automatic, fallback, hasPositive, changed);
    const ModelKeyState original(f.first);
    const m::Authors authorsBefore(f);
    const auto negativeBefore = NegativeTestRecord(*f.app, negative_frames::kPmmKey);
    const auto source = UnselectedNegativeSources(f);
    const auto peer = f.second.Digest(), ring = PoseRingDigest(f.first);
    const auto sourceBone = f.first.m->boneKeys[1];
    const auto viewportSelection = f.first.selected;
    oracle.Publish(true, true);

    n::Owner owner(f.app.get());
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    const auto bindingGeneration = plugins::BoundNativePoseGeneration(f.app.get());
    const n::Snapshot before(f);
    std::puts("    actual model delete"); std::fflush(stdout);
    Delete(f, route);
    if (!changed) {
        Check(before.Matches(f) && !f.app->RecordedFrameCountByte1(),
            "unselected model menu and nonpositive keyboard fallback leave every source and journal untouched");
        NoPrivateHistory(f);
        return {};
    }
    HistoryPosition(f, true);
    Check(f.app->SceneModified() && f.app->RecordedFrameCountByte1(),
        "successful model deletion reports project data modification through the real command");
    MatchesOracle(f, reference);
    if (selection == Selection::NegativeOnly)
        Check(original.Matches(f.first) && authorsBefore.Matches(f),
            "selected negative keys suppress all positive selected-bone fallback writes");
    if (selection == Selection::None) {
        Check(!f.first.m->boneKeys[positiveSlot].frame &&
            mdl::MorphKeyCapacity(f.first.m->morphKeys) == original.morphs.size() &&
            !std::memcmp(f.first.m->morphKeys, original.morphs.data(), original.morphs.size() * sizeof(mdl::MorphKey)) &&
            original.display.Matches(f.first) &&
            negativeBefore == NegativeTestRecord(*f.app, negative_frames::kPmmKey),
            "unselected keyboard fallback removes only the current selected-bone key and preserves morph display and negatives");
    }
    const ModelKeyState forward(f.first);
    const m::Authors forwardAuthors(f);
    const auto forwardNegative = NegativeTestRecord(*f.app, negative_frames::kPmmKey);
    const BoneKeys forwardBones = RegistrationKeys(f.first);
    const auto intact = [&] {
        Check(source == UnselectedNegativeSources(f) &&
            !negative_frames::CellSelected(f.app.get(), -20, 1) && !negative_frames::CellSelected(f.app.get(), -20, 3) &&
            !std::memcmp(&sourceBone, &f.first.m->boneKeys[1], sizeof(sourceBone)),
            "unselected negative bone/morph sources and the untouched native bone root remain exact");
        Check(f.second.Digest() == peer && PoseRingDigest(f.first) == ring &&
            f.first.selected == viewportSelection && f.SelectionUnchanged(),
            "model command preserves the peer model viewport selection and native legacy undo rings");
        Check(plugins::BoundNativePoseHistory(f.app.get()) == &f.history &&
            plugins::BoundNativePoseGeneration(f.app.get()) == bindingGeneration,
            "the actual signed history entry keeps one uninterrupted main-fixture binding");
        NoPrivateHistory(f);
    };
    intact();
    // Both the first undo and first redo execute on the opposite sign. The
    // second pair returns to the submission frame without replacing the binding.
    const std::array<int, 4> frames{{frame < 0 ? 17 : -7, frame < 0 ? 23 : -3, frame, frame}};
    for (unsigned step = 0; step < frames.size(); ++step) {
        const bool redo = step % 2 != 0;
        std::printf("    actual model %s frame=%d\n", redo ? "redo" : "undo", frames[step]); std::fflush(stdout);
        coordinator::Frame(reference, frames[step]); coordinator::Frame(f, frames[step]);
        oracle.Publish(redo, false);
        Check(plugins::BoundNativePoseGeneration(f.app.get()) == bindingGeneration,
            "oracle preparation and publication do not invalidate the actual record binding");
        CheckCommandWindow(f);
        CmdControl400(f.app.get(), f.app->MainWindow(), redo ? 401 : 400, 0);
        HistoryPosition(f, redo);
        MatchesOracle(f, reference);
        Check((redo ? forward.Matches(f.first) : original.Matches(f.first)) &&
            (redo ? forwardAuthors.Matches(f) : authorsBefore.Matches(f)) &&
            NegativeTestRecord(*f.app, negative_frames::kPmmKey) == (redo ? forwardNegative : negativeBefore),
            "paired model undo and redo restore complete original or forward native authored and negative data");
        intact();
    }
    return forwardBones;
}
} // namespace native_signed_delete_model_command_checks

void NativeSignedDeleteModelCommandChecks() {
    using namespace native_signed_delete_model_command_checks;
    std::puts("  signed model commands: menu423 and keyboard direct route; typed unbound oracle, not physical key dispatch");
    for (auto route : {Route::Menu423, Route::KeyboardDirect})
        for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) {
            for (bool automatic : {false, true}) {
                for (bool display : {false, true}) {
                    for (int frame : {-5, 0, 11}) CommandCase(route, mode, automatic, display, Selection::Mixed, frame);
                    CommandCase(route, mode, automatic, display, Selection::NegativeOnly, 0, true);
                }
                CommandCase(route, mode, automatic, false, Selection::None, -5);
                CommandCase(route, mode, automatic, false, Selection::None, 0);
                CommandCase(route, mode, automatic, false, Selection::None, 0, true);
            }
            const auto manual = CommandCase(route, mode, false, false, Selection::Interpolation, -5);
            const auto automatic = CommandCase(route, mode, true, false, Selection::Interpolation, -5);
            Check(manual.size() == automatic.size() && !manual.empty() &&
                std::memcmp(manual.data(), automatic.data(), manual.size() * sizeof(mdl::BoneKey)) != 0,
                "automatic checkbox regression has distinguishable native targets and detects a command ignoring the switch");
        }
}
