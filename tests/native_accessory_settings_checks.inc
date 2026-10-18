#include "../src/plugins/native_accessory_settings.hpp"
#include "../src/plugins/scene_store.hpp"
namespace {
struct AccessorySettingsFixture {
    ObjectCommandFixture f;
    std::array<mdl::AccessoryRecord*, 2> order{};
    HWND dialog = nullptr;
    AccessorySettingsFixture() {
        dialog = CreateWindowExW(0, L"STATIC", L"settings plan", WS_CHILD, 0, 0, 200, 150,
            f.app->MainWindow(), nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(dialog != nullptr, "settings plan dialog fixture");
        Check(CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD, 0, 0, 100, 80, dialog,
            reinterpret_cast<HMENU>(INT_PTR(panel::kOrderListBox)), GetModuleHandleW(nullptr), nullptr) != nullptr,
            "settings plan real listbox");
        Check(CreateWindowExW(0, L"EDIT", L"", WS_CHILD, 0, 90, 100, 24, dialog,
            reinterpret_cast<HMENU>(INT_PTR(panel::kAccessoryOrderEdit)), GetModuleHandleW(nullptr), nullptr) != nullptr,
            "settings plan real split edit");
        f.app->AccessoryOrderArray() = order.data(); Fill(true);
    }
    ~AccessorySettingsFixture() { f.app->AccessoryOrderArray() = nullptr; DestroyWindow(dialog); }
    void Fill(bool change) {
        const auto list = GetDlgItem(dialog, panel::kOrderListBox); SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (unsigned slot = 0; slot < 2; ++slot) order[f.app->AccessorySlot(slot)->order] = f.app->AccessorySlot(slot);
        if (change) std::swap(order[0], order[1]);
        for (int row = 0; row < 2; ++row) {
            const auto text = change && row == 0 ? std::wstring(L"名前と名称") : text_encoding::Display(order[row]->name);
            Check(SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())) == row, "settings plan list row");
        }
        SetDlgItemInt(dialog, panel::kAccessoryOrderEdit, change ? 0 : f.app->AccessoryRenderSplitOrder(), FALSE);
    }
    std::array<mdl::AccessoryRecord, 2> Values() const { return {*f.app->AccessorySlot(0), *f.app->AccessorySlot(1)}; }
    bool Same(const std::array<mdl::AccessoryRecord, 2>& values) const {
        return !std::memcmp(&values[0], f.app->AccessorySlot(0), sizeof values[0]) &&
            !std::memcmp(&values[1], f.app->AccessorySlot(1), sizeof values[1]);
    }
    MmsResult Prepare(std::unique_ptr<plugins::SharedHistory::NativeTransaction>& transaction) {
        plugins::AccessorySettingsPlan plan; std::shared_ptr<plugins::HistoryChange> change;
        if (const auto r = plan.Begin(*f.app)) return r;
        if (const auto r = plan.ReadDialog(dialog)) return r;
        if (const auto r = plan.Take(change)) return r;
        if (!change) return MMS_E_FAILED;
        return plugins::Boundary([&] { return f.history.PrepareNative("Accessory settings", std::move(change), transaction); });
    }
};
void AccessorySettingsRoundTrip() {
    AccessorySettingsFixture x; auto& f = x.f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    const auto before = x.Values(); const auto frame = f.app->CurrentFrame();
    const auto originalName = text_encoding::Display(before[1].name);
    f.app->CameraMode() = 1;
    Check(scene_history_ui::RefreshObjectLists(*f.app, scene_history_ui::CaptureSelection(*f.app), false, false) ==
        scene_history_ui::Result::Ok, "settings fixture uses the camera-mode accessory registration scope");
    SendDlgItemMessageW(f.app->MainWindow(), panel::kRegisterScopeCombo, CB_SETCURSEL, 5, 0);
    const auto selection = scene_history_ui::CaptureSelection(*f.app);
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(x.Prepare(transaction) == MMS_OK && x.Same(before), "combined metadata preparation is private");
    failAfter = 0; const auto committed = f.history.CommitNative(std::move(transaction)); failAfter = -1;
    Check(committed == MMS_OK && f.history.UndoCount() == 1, "combined order, name and split publishes without allocating once");
    Check(!f.history.LastPublicationRequiresPhysicsReset(), "metadata publication declares no physics invalidation");
    Check(f.app->AccessorySlot(1)->order == 0 && f.app->AccessorySlot(0)->order == 1 &&
        text_encoding::Display(f.app->AccessorySlot(1)->name) == L"名前と名称" && f.app->AccessoryRenderSplitOrder() == 0,
        "combined metadata target applied");
    for (int slot = 0; slot < 2; ++slot) {
        auto current = *f.app->AccessorySlot(slot); auto previous = before[slot];
        current.order = previous.order = 0; std::memset(current.name, 0, sizeof current.name); std::memset(previous.name, 0, sizeof previous.name);
        Check(!std::memcmp(&current, &previous, sizeof current), "metadata leaves every other accessory byte unchanged");
    }
    // Scratch editor has closed before production history/readout entry.
    f.app->AccessoryOrderArray() = nullptr;
    Check(scene_history_ui::RefreshObjectLists(*f.app, selection, true, false) == scene_history_ui::Result::Ok,
        "publish metadata lists using the selection captured before the edit");
    Check(SendDlgItemMessageW(f.app->MainWindow(), panel::kRegisterScopeCombo, CB_GETCURSEL, 0, 0) == 4,
        "scope retains the second accessory identity after order changes");
    f.app->CurrentFrame() = frame + 12; f.app->AccessorySlot(0)->position[0] = 93.f;
    Check(text_encoding::EncodeDisplay(f.app->AccessorySlot(0)->name, 100, L"later unrelated name"), "unowned name changes later");
    f.app->PhysicsResetPending() = 0;
    Check(plugins::RunBoundObjectHistory(*f.app, f.history, false) == MMS_OK, "real history wrapper undoes metadata");
    Check(!f.app->PhysicsResetPending() && !f.history.LastPublicationRequiresPhysicsReset(), "metadata undo does not reset simulation");
    Check(f.app->AccessorySlot(0)->order == 0 && f.app->AccessorySlot(1)->order == 1 &&
        text_encoding::Display(f.app->AccessorySlot(1)->name) == originalName && f.app->AccessoryRenderSplitOrder() == 2,
        "undo restores original metadata");
    Check(f.app->CurrentFrame() == frame + 12 && f.app->AccessorySlot(0)->position[0] == 93.f &&
        text_encoding::Display(f.app->AccessorySlot(0)->name) == L"later unrelated name", "undo preserves frame, pose and unowned name");
    wchar_t label[128]{};
    Check(SendDlgItemMessageW(f.app->MainWindow(), panel::kAccessoryCombo, CB_GETLBTEXT, 1, reinterpret_cast<LPARAM>(label)) != CB_ERR &&
        std::wstring(label) == originalName, "undo refreshes names even when object pointers did not change");
    Check(SendDlgItemMessageW(f.app->MainWindow(), panel::kRegisterScopeCombo, CB_GETCURSEL, 0, 0) == 5,
        "undo refresh preserves current registration identity");
    // The byte-exact unmodified dialog must not clear this redo branch.
    f.app->AccessoryOrderArray() = x.order.data(); x.Fill(false);
    plugins::AccessorySettingsPlan noOp; std::shared_ptr<plugins::HistoryChange> empty;
    Check(noOp.Begin(*f.app) == MMS_OK && noOp.ReadDialog(x.dialog) == MMS_OK && noOp.Take(empty) == MMS_OK && !empty &&
        f.history.UndoCount() == 0 && f.history.RedoCount() == 1, "same metadata creates no node and retains redo");
    f.app->AccessoryOrderArray() = nullptr;
    Check(plugins::RunBoundObjectHistory(*f.app, f.history, true) == MMS_OK, "real history wrapper redoes metadata");
    Check(f.app->AccessorySlot(1)->order == 0 && f.app->AccessoryRenderSplitOrder() == 0 &&
        text_encoding::Display(f.app->AccessorySlot(1)->name) == L"名前と名称", "redo uses retained metadata");
    Check(f.app->CurrentFrame() == frame + 12 && f.app->AccessorySlot(0)->position[0] == 93.f, "redo never evaluates the current pose");
    f.failInsert = panel::kAccessoryCombo;
    Check(plugins::RunBoundObjectHistory(*f.app, f.history, false) == MMS_OK && plugins::NativeObjectUiPending(f.app.get()) &&
        f.history.UndoCount() == 0 && f.history.RedoCount() == 1, "failed metadata UI refresh never repeats the committed undo");
    Check(plugins::RefreshBoundObjectUi(*f.app, f.history) && !plugins::NativeObjectUiPending(f.app.get()),
        "metadata UI retries from stable identity after a real combo insertion failure");
    Check(plugins::RunBoundObjectHistory(*f.app, f.history, true) == MMS_OK, "redo remains available after metadata UI failure");
    Check(!f.app->PhysicsResetPending() && !f.history.LastPublicationRequiresPhysicsReset(), "metadata redo and UI retry do not reset simulation");
}
void AccessorySettingsObjectLifetime() {
    AccessorySettingsFixture x; auto& f = x.f; const auto before = x.Values();
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(x.Prepare(transaction) == MMS_OK && f.history.CommitNative(std::move(transaction)) == MMS_OK,
        "metadata lifetime fixture publishes combined change");
    auto owner = plugins::RetainSceneObject(*f.app, MMS_OBJECT_ACCESSORY, 0);
    const auto initial = plugins::SceneObjectIdentity(f.app->AccessorySlot(0));
    Check(f.history.PrepareNative("remove", owner->CaptureRemoval(4096), transaction) == MMS_OK &&
        f.history.CommitNative(std::move(transaction)) == MMS_OK && !f.app->AccessorySlot(0), "interleave actual retained object removal");
    Check(f.history.LastPublicationRequiresPhysicsReset(), "object removal retains conservative physics invalidation");
    Check(f.history.History(false) == MMS_OK && f.app->AccessorySlot(0) &&
        plugins::SceneObjectPresent(f.app->AccessorySlot(0),initial.id) &&
        plugins::SceneObjectIdentity(f.app->AccessorySlot(0)).generation != initial.generation,
        "restoring removal changes generation while retaining the same object identity");
    Check(f.history.History(false) == MMS_OK && x.Same(before) && f.app->AccessoryRenderSplitOrder() == 2,
        "metadata undo accepts a correctly restored object with a newer generation");
    Check(f.history.History(true) == MMS_OK && f.history.History(true) == MMS_OK && !f.app->AccessorySlot(0),
        "metadata redo and later object removal preserve the shared journal ordering");
}
void AccessorySettingsRefusals() {
    for (int stale = 0; stale < 5; ++stale) {
        AccessorySettingsFixture x; auto& f = x.f; plugins::AccessorySettingsPlan plan;
        Check(plan.Begin(*f.app) == MMS_OK, "capture settings source");
        if (stale == 0) std::strcpy(f.app->AccessorySlot(0)->name, "modified");
        if (stale == 1) f.app->AccessoryRenderSplitOrder() = 1;
        if (stale == 2) { plugins::ForgetSceneObject(f.app->AccessorySlot(0)); plugins::SceneObjectIdentity(f.app->AccessorySlot(0)); }
        if (stale == 3) x.order[1] = x.order[0];
        if (stale == 4) SetDlgItemInt(x.dialog, panel::kAccessoryOrderEdit, 3, FALSE);
        const auto before = x.Values(); const auto split = f.app->AccessoryRenderSplitOrder();
        Check(plan.ReadDialog(x.dialog) != MMS_OK && x.Same(before) && f.app->AccessoryRenderSplitOrder() == split,
            "stale source, duplicate scratch or invalid split refuses without mutation");
    }
    {
        AccessorySettingsFixture x; auto before = x.Values(); std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(x.Prepare(transaction) == MMS_OK, "prepare metadata stale candidate");
        std::strcpy(x.f.app->AccessorySlot(0)->name, "new source"); before = x.Values();
        Check(x.f.history.CommitNative(std::move(transaction)) == MMS_E_STALE && x.Same(before) && x.f.history.UndoCount() == 0,
            "final validation rejects even an initially unowned source name change");
    }
    {
        AccessorySettingsFixture x; const auto before = x.Values(); std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(x.f.history.Configure(plugins::SharedHistory::RecordingMode::ExplicitOnly, 8, 1) == MMS_OK, "tiny explicit history budget");
        Check(x.Prepare(transaction) == MMS_E_MEMORY && !transaction && x.Same(before), "settings remain explicit and atomic in explicit-only mode");
    }
}
void AccessorySettingsFailures() {
    unsigned failures = 0;
    for (long limit = 0; limit < 256; ++limit) {
        AccessorySettingsFixture x; const auto before = x.Values();
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        failAfter = limit; const auto result = x.Prepare(transaction); failAfter = -1;
        Check(x.Same(before) && x.f.history.UndoCount() == 0 && x.f.history.RedoCount() == 0,
            "all allocation preparation failures preserve live accessory data and journal");
        if (result == MMS_OK) {
            transaction.reset(); Check(failures > 8 && x.Same(before), "cancelled metadata releases private state after allocation sweep"); return;
        }
        Check(result == MMS_E_MEMORY && !transaction, "allocation refusal reports memory without a candidate"); ++failures;
    }
    Check(false, "accessory settings allocation sweep terminates");
}
void AccessorySettingsSnapshots() {
    AccessorySettingsFixture x; auto& f = x.f; plugins::NativePoseTestBinding binding(*f.app,f.history);
    auto scene=plugins::CreateSceneStore(f.app.get()); plugins::SceneClient client{scene.get()};
    auto api=scene->Api(client); const auto object=plugins::SceneObjectIdentity(f.app->AccessorySlot(1));
    const auto acquire=[&]() {
        MmsSceneQueryV1 query{};query.h=plugins::Header<MmsSceneQueryV1>();
        query.flags=MMS_SCENE_CURRENT_TIME;query.channels=MMS_CHANNEL_BONES;
        MmsSnapshotV1 snapshot{};
        Check(api.acquire(api.context,&query,&snapshot)==MMS_OK,"acquire actual scene snapshot after metadata publication");return snapshot;
    };
    const auto name=[&](const MmsSnapshotV1& snapshot) {
        MmsSpan objects{};Check(api.objects(api.context,snapshot.handle,&objects)==MMS_OK,"read immutable scene object records");
        for(std::uint64_t i=0;i<objects.count;++i) {
            const auto& value=*reinterpret_cast<const MmsObjectV1*>(static_cast<const unsigned char*>(objects.data)+i*objects.stride);
            if(value.object.id.lo==object.id.lo&&value.object.id.hi==object.id.hi)return plugins::String(value.name);
        }
        Check(false,"metadata object identity remains visible in snapshots");return std::string{};
    };
    const auto old=acquire();const auto original=plugins::Utf8(text_encoding::Display(f.app->AccessorySlot(1)->name));
    Check(name(old)==original,"scene exposes editable accessory name instead of source stem");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(x.Prepare(transaction)==MMS_OK&&f.history.CommitNative(std::move(transaction))==MMS_OK,"publish snapshot metadata fixture");
    const auto changed=acquire();
    Check(name(changed)==plugins::Utf8(L"名前と名称")&&name(old)==original,"new snapshot sees rename while old lease retains original name");
    Check(changed.sceneRevision==old.sceneRevision+1&&changed.structureRevision>old.structureRevision,
        "renaming updates native edit and scene metadata revisions");
    Check(f.history.History(false)==MMS_OK,"undo snapshot metadata fixture");const auto restored=acquire();
    Check(name(restored)==original&&name(changed)==plugins::Utf8(L"名前と名称"),"undo produces restored metadata without changing previous snapshot");
    for(const auto handle:{old.handle,changed.handle,restored.handle})api.release(api.context,handle);
    f.app->AccessorySlot(1)->name[0]=0;
    wcscpy_s(f.app->AccessorySlot(1)->sourcePath,L"C:/folder/fallback.x");
    const auto fallback=acquire();Check(name(fallback)=="fallback","empty legacy accessory label retains source-stem fallback");api.release(api.context,fallback.handle);
    std::memset(f.app->AccessorySlot(1)->name,'x',sizeof f.app->AccessorySlot(1)->name);
    MmsSceneQueryV1 query{};query.h=plugins::Header<MmsSceneQueryV1>();query.flags=MMS_SCENE_CURRENT_TIME;query.channels=MMS_CHANNEL_BONES;
    MmsSnapshotV1 invalid{};Check(api.acquire(api.context,&query,&invalid)==MMS_E_NOT_READY,"unterminated legacy name cannot overread scene snapshot memory");
    f.app->AccessorySlot(1)->name[0]=0;
}
void NativeAccessorySettingsChecks() {
    std::puts("  settings round trip"); std::fflush(stdout); AccessorySettingsRoundTrip();
    std::puts("  settings refusal"); std::fflush(stdout); AccessorySettingsRefusals();
    std::puts("  settings object lifetime"); std::fflush(stdout); AccessorySettingsObjectLifetime();
    std::puts("  settings scene snapshots"); std::fflush(stdout); AccessorySettingsSnapshots();
    std::puts("  settings failure sweep"); std::fflush(stdout); AccessorySettingsFailures();
}
}
