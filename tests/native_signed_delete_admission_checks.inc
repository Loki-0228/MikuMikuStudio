// Include after negative_global_delete_target_checks.inc and
// negative_model_delete_target_checks.inc; call NativeSignedDeleteAdmissionChecks().
#include "../src/animation/negative_history.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/scene_ownership.hpp"

namespace signed_delete_admission_checks {
using namespace mikumikustudio;
namespace nf = negative_frames;
namespace globals = negative_global_delete_target_checks;
namespace models = negative_model_delete_target_checks;
using Selection = nf::DeletionSelection;

MmsResult Query(MMDApp& app, plugins::SharedHistory& history, Selection& out) {
    failAfter = 0;
    const auto result = nf::QueryDeletionSelection(app, history, out);
    failAfter = -1;
    return result;
}
bool Interaction(MMDApp& app) {
    failAfter = 0;
    const bool ready = nf::DeletionInteractionReady(app);
    failAfter = -1;
    return ready;
}
void Expect(globals::Fixture& f, bool negative, bool positive) {
    const globals::Snapshot before(f);
    Selection out{!negative, !positive};
    Check(Interaction(*f.app) && Query(*f.app, f.history, out) == MMS_OK &&
        out.negative == negative && out.positive == positive && before.Same(f),
        "signed selection query is allocation-free and preserves keys poses selection and journal");
}
void Refuse(globals::Fixture& f, MmsResult expected = MMS_E_NOT_READY, bool interaction = false) {
    const globals::Snapshot before(f);
    for (const Selection sentinel : {Selection{true, false}, Selection{false, true}}) {
        auto out = sentinel;
        Check(Interaction(*f.app) == interaction && Query(*f.app, f.history, out) == expected &&
            out.negative == sentinel.negative && out.positive == sentinel.positive && before.Same(f),
            "rejected signed query preserves output and all live state without allocation");
    }
}
void GlobalSelections() {
    globals::Fixture f;
    // Interaction checks are independent of binding; selection queries are not.
    Refuse(f, MMS_E_NOT_READY, true);
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    for (int frame : {frame_limits::minimum, -1, 0, frame_limits::maximum}) {
        f.app->CurrentFrame() = frame;
        Expect(f, false, false);
    }
    Check(!nf::Count(f.app.get()) && NegativeTestRecord(*f.app, nf::kPmmKey).empty() &&
        NegativeTestRecord(*f.app, nf::kPmmSelectionKey).empty(),
        "empty query leaves negative storage absent even with allocations disabled");
    // Every global family, including an accessory, must count maximum as a mark.
    for (unsigned family = 0; family < 5; ++family) {
        f.ClearMarks();
        switch (family) {
        case 0: f.app->CameraKeys()[2].frame = frame_limits::maximum; f.app->CameraKeys()[2].selected = 1; break;
        case 1: f.app->LightKeys()[2].frame = frame_limits::maximum; f.app->LightKeys()[2].selected = 1; break;
        case 2: f.app->ShadowKeys()[2].frame = frame_limits::maximum; f.app->ShadowKeys()[2].selected = 1; break;
        case 3: f.app->GravityKeys()[2].frame = frame_limits::maximum; f.app->GravityKeys()[2].selected = 1; break;
        case 4: f.app->AccessoryKeys(7)[2].frame = frame_limits::maximum; f.app->AccessoryKeys(7)[2].selected = 1; break;
        }
        Expect(f, false, true);
    }
    f.ClearMarks();
    f.app->CurrentFrame() = -10;
    Check(nf::RegisterGlobal(f.app.get(), nf::Global::Camera, -10), "seed explicit negative selection");
    Expect(f, true, false);
    f.app->LightKeys()[0].selected = 1;
    Expect(f, true, true);
    nf::SelectBox(f.app.get(), -10, -9, 0, 1, false, true);
    Expect(f, false, true);
    f.ClearMarks();
    // A key under the playhead and row selection alone are not explicit marks.
    f.app->CurrentFrame() = 12;
    f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
    Expect(f, false, false);
}

void InteractionAndBinding() {
    globals::Fixture f;
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    f.app->CurrentFrame() = -10;
    Check(nf::RegisterGlobal(f.app.get(), nf::Global::Camera, -10), "seed guarded selection");
    f.app->LightKeys()[1].selected = 1;
    for (int frame : {frame_limits::minimum - 1, frame_limits::maximum + 1}) {
        f.app->CurrentFrame() = frame; Refuse(f);
    }
    f.app->CurrentFrame() = -10;
    const HWND dialog = reinterpret_cast<HWND>(INT_PTR(1));
    for (unsigned gate = 0; gate < 11; ++gate) {
        const auto set = [&](bool on) {
            switch (gate) {
            case 0: f.app->PlaybackActive() = on; break;
            case 1: f.app->FrameStepPlayback() = on; break;
            case 2: f.app->state.recordPlaybackStartPending = on; break;
            case 3: f.app->FrameRangeDialog() = on ? dialog : nullptr; break;
            case 4: f.app->state.frameCopyDialog = on ? dialog : nullptr; break;
            case 5: f.app->RecordingWindow() = on ? dialog : nullptr; break;
            case 6: f.app->GravitySettingDialog() = on ? dialog : nullptr; break;
            case 7: f.app->GroundShadowColorDialog() = on ? dialog : nullptr; break;
            case 8: f.app->AccessoryOrderArray() = on ? reinterpret_cast<void*>(INT_PTR(1)) : nullptr; break;
            case 9: f.app->state.accessoryEditDialogOpen = on; break;
            case 10: f.app->state.edgeThicknessDialog = on ? dialog : nullptr; break;
            }
        };
        set(true); Refuse(f); set(false);
    }
    { bake::ScopedUiInputGate gate; Refuse(f);
        Check(!nf::DeletionInteractionReady(*f.app,true),"replay request alone cannot bypass an unrelated input gate"); }
    { ScopedSceneMutation mutation(*f.app); Refuse(f);
        Check(!nf::DeletionInteractionReady(*f.app,true),"replay request alone cannot authorize scene mutation"); }
    Check(f.history.Enter() == MMS_OK, "hold journal gate before interaction-only check");
    Refuse(f, MMS_E_BUSY, true);
    Check(f.history.Enter() == MMS_E_BUSY, "queries do not release the caller's journal gate");
    f.history.Leave();
    plugins::StorageStore storage;
    plugins::SharedHistory replacement(storage);
    { plugins::NativePoseTestBinding other(*f.app, replacement); Refuse(f, MMS_E_NOT_READY, true); }
    auto otherApp = std::make_unique<MMDApp>();
    ::new(static_cast<void*>(&otherApp->state)) MMDAppState{};
    { plugins::NativePoseTestBinding other(*otherApp, f.history); Refuse(f, MMS_E_NOT_READY, true); }
    Expect(f, true, true);

    nf::BeginDrag(f.app.get());
    nf::Drag(f.app.get(), 2);
    Check(nf::CellSelected(f.app.get(), -8, 0), "negative drag is live before admission check");
    Refuse(f);
    nf::Drag(f.app.get(), 3);
    Check(nf::CellSelected(f.app.get(), -7, 0), "rejection retains the original drag transaction");
    nf::EndDrag(f.app.get());
    Expect(f, true, true);
}

template<class Key> void UnmanagedPool(MMDApp& app, plugins::SharedHistory& history, Key*& pool) {
    Key borrowed{};
    auto* saved = pool;
    pool = &borrowed;
    Selection out{true, true};
    Check(!Interaction(app) && Query(app, history, out) == MMS_E_NOT_READY && out.negative && out.positive,
        "unmanaged one-element pool is refused before legacy capacity scanning");
    pool = saved;
}
void PoolAdmission() {
    globals::Fixture f;
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    const globals::Snapshot before(f);
    UnmanagedPool(*f.app, f.history, f.app->CameraKeys());
    UnmanagedPool(*f.app, f.history, f.app->LightKeys());
    UnmanagedPool(*f.app, f.history, f.app->ShadowKeys());
    UnmanagedPool(*f.app, f.history, f.app->GravityKeys());
    // NativeSelectionFirst scans accessory arrays even at unoccupied slots.
    UnmanagedPool(*f.app, f.history, f.app->AccessoryKeys(254));
    Check(before.Same(f), "unmanaged global pool rejection leaves all real data unchanged");
    mdl::AccessoryRecord unregistered{};
    f.app->AccessorySlot(254) = &unregistered;
    MmsObjectRef identity{};
    Check(!plugins::FindSceneObjectIdentity(&unregistered, identity), "accessory has no scene identity before query");
    Expect(f, false, false);
    Check(!plugins::FindSceneObjectIdentity(&unregistered, identity), "query never registers accessory identity");
    f.app->AccessorySlot(254) = nullptr;
    f.app->CurrentFrame() = -10;
    Check(nf::RegisterAccessory(f.app.get(), 7, -10), "seed selected negative accessory");
    Expect(f, true, false);
    auto* accessory = f.app->AccessorySlot(7);
    f.app->AccessorySlot(7) = &unregistered;
    Expect(f, false, false);
    Check(nf::Count(f.app.get()) == 1 && !plugins::FindSceneObjectIdentity(&unregistered, identity),
        "dormant negative accessory is ignored without erasing it or registering its replacement");
    f.app->AccessorySlot(7) = accessory;
    Expect(f, true, false);
}

void ModelSelections() {
    models::Fixture f;
    models::Owner owner(f.app.get());
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    f.Marks(false, false, false);
    const auto expect = [&](bool negative, bool positive) {
        const models::Snapshot before(f);
        Selection out{!negative, !positive};
        Check(Interaction(*f.app) && Query(*f.app, f.history, out) == MMS_OK &&
            out.negative == negative && out.positive == positive && before.Matches(f),
            "model query preserves all families negative tracks poses and history without allocation");
    };
    expect(false, false);
    for (unsigned family = 0; family < 3; ++family) {
        f.first.m->boneKeys[3].allocated = family == 0;
        f.first.m->morphKeys[2].allocated = family == 1;
        f.first.m->displayKeys[2].allocated = family == 2;
        f.first.m->boneKeys[3].frame = frame_limits::maximum;
        f.first.m->morphKeys[2].frame = frame_limits::maximum;
        f.first.m->displayKeys[2].frame = frame_limits::maximum;
        expect(false, true);
    }
    f.first.m->boneKeys[3].allocated = 0;
    f.first.m->morphKeys[2].allocated = 0;
    f.first.m->displayKeys[2].allocated = 0;
    f.Marks(false, false, false);
    f.app->CurrentFrame() = -10;
    Check(nf::RegisterBone(f.first.raw(), 0, -10), "seed selected negative model key");
    expect(true, false);
    f.Marks(true, false, false);
    expect(true, true);
    f.app->SelectedModelSlot() = 1;
    expect(false, true);
    f.Marks(false, false, false);
    expect(false, false);
    f.app->CameraMode() = 1;
    expect(false, false);
    f.app->CameraMode() = 0;
    f.app->SelectedModelSlot() = 0;
    // Replacing a table makes the old negative key non-current without erasing it.
    auto* table = f.first.m->boneTable;
    f.first.m->boneTable = f.second.m->boneTable;
    Selection stale{true, true};
    Check(Query(*f.app, f.history, stale) == MMS_OK && !stale.negative && !stale.positive,
        "non-current negative model identity is ignored");
    f.first.m->boneTable = table;
    expect(true, false);
    const models::Snapshot before(f);
    f.app->AccessoryKeys(7)[0].selected = 1;
    for (unsigned slot : {2u, 255u}) {
        f.app->SelectedModelSlot() = std::uint8_t(slot);
        Selection out{true, true};
        Check(!Interaction(*f.app) && Query(*f.app, f.history, out) == MMS_E_NOT_READY && out.negative && out.positive,
            "missing or out-of-range selected model slot refuses admission without global fallback");
    }
    f.app->AccessoryKeys(7)[0].selected = 0;
    f.app->SelectedModelSlot() = 0;
    UnmanagedPool(*f.app, f.history, f.first.m->boneKeys);
    UnmanagedPool(*f.app, f.history, f.first.m->morphKeys);
    UnmanagedPool(*f.app, f.history, f.first.m->displayKeys);
    Check(before.Matches(f), "invalid model slot and unmanaged model pools never mutate live state");
}

void JournalPreparation() {
    // Exercise the actual PrepareNative/IsCurrent path, which holds Enter().
    struct Candidate final : plugins::PreparedHistoryChange {
        MMDApp& app;
        explicit Candidate(MMDApp& value) : app(value) {}
        bool IsCurrent() const noexcept override { return nf::DeletionInteractionReady(app); }
        void Publish() noexcept override {}
    };
    struct Change final : plugins::HistoryChange {
        MMDApp& app;
        explicit Change(MMDApp& value) : app(value) {}
        MmsResult Prepare(bool, std::unique_ptr<plugins::PreparedHistoryChange>& out) override {
            if (!Interaction(app)) return MMS_E_NOT_READY;
            out = std::make_unique<Candidate>(app);
            failAfter = 0;
            const bool current = out->IsCurrent();
            failAfter = -1;
            Check(current, "candidate interaction validation remains allocation-free under PrepareNative's gate");
            return MMS_OK;
        }
        std::uint64_t RetainedBytes() const noexcept override { return sizeof(Change); }
    };
    globals::Fixture f;
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    const globals::Snapshot before(f);
    auto change = std::make_shared<Change>(*f.app);
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("signed interaction admission", change, transaction) == MMS_OK && transaction,
        "interaction checks admit preparation and IsCurrent under the actual journal gate");
    transaction.reset();
    Check(before.Same(f), "cancelling interaction-only preparation leaves the journal and selection unchanged");
    Check(f.history.PrepareNative("signed interaction admission", change, transaction) == MMS_OK && transaction,
        "prepare a candidate before a late interaction blocker");
    f.app->state.recordPlaybackStartPending = true;
    failAfter = 0;
    const auto rejected = f.history.CommitNative(std::move(transaction));
    failAfter = -1;
    f.app->state.recordPlaybackStartPending = false;
    Check(rejected == MMS_E_STALE && before.Same(f),
        "actual journal IsCurrent rejects a late UI blocker without reentering the history gate or publishing");
}
}

void NativeSignedDeleteAdmissionChecks() {
    signed_delete_admission_checks::GlobalSelections();
    signed_delete_admission_checks::InteractionAndBinding();
    signed_delete_admission_checks::PoolAdmission();
    signed_delete_admission_checks::ModelSelections();
    signed_delete_admission_checks::JournalPreparation();
}
