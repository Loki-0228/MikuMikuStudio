// Participant contracts. Include after ObjectCommandFixture and
// camera_multiply_checks::PluginState in native_edit_history_tests.cpp.
// These CPU contracts do not invoke menu 263, serialize a model or create
// source files. The production journal/UI/file integration has separate tests.
#include "../src/plugins/native_model_source.hpp"
#include "../src/plugins/history_runtime.hpp"
#include "../src/plugins/scene_object_history.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <type_traits>
#include <vector>

#if !MMD_ORIGINAL_PROFILE
namespace model_source_checks {
using namespace plugins;
using Path = std::array<wchar_t, 256>;
static_assert(sizeof(wchar_t) == 2 && sizeof(Path) == 512);
static_assert(sizeof(mdl::ModelRecord::path) == sizeof(Path));
static_assert(!std::is_copy_constructible_v<NativeModelSourcePlan>);
static_assert(!std::is_copy_assignable_v<NativeModelSourcePlan>);

void Trace(const char* phase, int detail = -1, MmsResult result = MMS_OK) {
    std::printf("MODEL_SOURCE phase=%s detail=%d result=%d\n", phase, detail, int(result)); std::fflush(stdout);
}
Path Read(const unsigned char* object) {
    Check(object != nullptr, "source-path read has a living fixture model");
    Path out{}; std::memcpy(out.data(), mdl::Mdl(object)->path, sizeof(out)); return out;
}
bool Same(const Path& a, const Path& b) { return !std::memcmp(a.data(), b.data(), sizeof(a)); }
void Match(const unsigned char* object, const Path& expected) {
    Check(Same(Read(object), expected), "source-path comparison preserves all 512 bytes including the early-NUL tail");
}
Path Replaced(Path previous, const wchar_t* value) {
    const auto count = std::wcslen(value);
    Check(count > 0 && count < previous.size(), "source-path expected string fits the fixed native field");
    std::memcpy(previous.data(), value, (count + 1) * sizeof(wchar_t)); return previous;
}
bool SameId(MmsId a, MmsId b) { return a.lo == b.lo && a.hi == b.hi; }

struct Fixture : ObjectCommandFixture {
    std::wstring cwd;
    Fixture() {
        app->HInstance() = GetModuleHandleW(nullptr);
        app->TimeNowLow() = 0x12345678u; app->TimeNowHigh() = 0x42u; app->DeltaTime() = .03125f;
        app->MessageSeen() = 1;
        for (unsigned slot = 0; slot < 2; ++slot) {
            auto& model = *mdl::Mdl(app->ModelSlot(slot));
            for (unsigned i = 0; i < 256; ++i) {
                model.path[i] = wchar_t(0x4e00 + 100 * slot + i % 89);
                model.modelDirectory[i] = wchar_t(0x5200 + 100 * slot + i % 89);
            }
            const auto* path = slot ? L"Z:\\model-source\\original-B.pmx" : L"Z:\\model-source\\original-long-A.pmx";
            const auto* directory = slot ? L"D:\\unchanged-B\\" : L"D:\\unchanged-A\\";
            std::wmemcpy(model.path, path, std::wcslen(path) + 1);
            std::wmemcpy(model.modelDirectory, directory, std::wcslen(directory) + 1);
            model.lightDir[2] = .125f + slot;
        }
        std::array<wchar_t, 32768> current{};
        const auto count = GetCurrentDirectoryW(DWORD(current.size()), current.data());
        Check(count && count < current.size(), "source-path fixture captures CWD without changing it"); cwd.assign(current.data(), count);
    }
    std::shared_ptr<HistoryChange> Change(const wchar_t* path, unsigned slot = 0) {
        NativeModelSourcePlan plan; std::shared_ptr<HistoryChange> change;
        Check(plan.Begin(*app, slot) == MMS_OK && plan.Object() == app->ModelSlot(slot) &&
            plan.Journal() == &history && plan.PermissionHistory() == &history && plan.IsCurrent(),
            "source-path bound plan captures the exact requested object and existing journal");
        Check(plan.Set(path) == MMS_OK && plan.Take(change) == MMS_OK && change && plan.IsCurrent(),
            "source-path Take retains the before-source/owner while exposing one native participant");
        return change;
    }
    void CwdUnchanged() const {
        std::array<wchar_t, 32768> current{};
        Check(GetCurrentDirectoryW(DWORD(current.size()), current.data()) == cwd.size() && cwd == current.data(),
            "source-path participant never changes the process directory");
    }
};

struct Bytes {
    const void* pointer = nullptr;
    std::vector<unsigned char> bytes;
    Bytes() = default;
    Bytes(const void* p, size_t count) : pointer(p) {
        Check(p || !count, "source-path snapshot uses a real fixture allocation or an empty range");
        if (count) { bytes.resize(count); std::memcpy(bytes.data(), p, count); }
    }
    void Unchanged(const void* p, size_t count) const {
        Check(pointer == p && bytes.size() == count && (!count || !std::memcmp(p, bytes.data(), count)),
            "source-path edit preserves unrelated allocation identities, capacities and payload bytes");
    }
};
struct ModelSnapshot {
    unsigned char* object;
    Bytes record, bones, morphs, display;
    std::uint64_t boneId, morphId, displayId;
    explicit ModelSnapshot(unsigned char* p) : object(p), record(p, sizeof(mdl::ModelRecord)),
        bones(mdl::Mdl(p)->boneKeys, mdl::BoneKeyCapacity(mdl::Mdl(p)->boneKeys) * sizeof(mdl::BoneKey)),
        morphs(mdl::Mdl(p)->morphKeys, mdl::MorphKeyCapacity(mdl::Mdl(p)->morphKeys) * sizeof(mdl::MorphKey)),
        display(mdl::Mdl(p)->displayKeys, mdl::KeyPoolCapacity(mdl::Mdl(p)->displayKeys) * sizeof(mdl::DisplayKey)),
        boneId(mdl::BoneKeyPoolIdentity(mdl::Mdl(p)->boneKeys)), morphId(mdl::MorphKeyPoolIdentity(mdl::Mdl(p)->morphKeys)),
        displayId(mdl::KeyPoolIdentity(mdl::Mdl(p)->displayKeys)) {}
    void ExceptPath(unsigned char* current, const Path& expected) const {
        Check(current == object && current, "source-path edit stays attached to the original model object");
        const size_t begin = offsetof(mdl::ModelRecord, path), end = begin + sizeof(Path);
        Check(!std::memcmp(current, record.bytes.data(), begin) &&
            !std::memcmp(current + end, record.bytes.data() + end, sizeof(mdl::ModelRecord) - end),
            "source-path Publish changes no model byte outside the exact 512-byte path field");
        Match(current, expected); const auto& model = *mdl::Mdl(current);
        Check(boneId == mdl::BoneKeyPoolIdentity(model.boneKeys) && morphId == mdl::MorphKeyPoolIdentity(model.morphKeys) &&
            displayId == mdl::KeyPoolIdentity(model.displayKeys), "source-path edit preserves all existing key-pool identities");
        bones.Unchanged(model.boneKeys, mdl::BoneKeyCapacity(model.boneKeys) * sizeof(mdl::BoneKey));
        morphs.Unchanged(model.morphKeys, mdl::MorphKeyCapacity(model.morphKeys) * sizeof(mdl::MorphKey));
        display.Unchanged(model.displayKeys, mdl::KeyPoolCapacity(model.displayKeys) * sizeof(mdl::DisplayKey));
    }
};
struct Snapshot {
    Fixture& fixture;
    std::array<Path, 2> paths;
    std::array<ModelSnapshot, 2> models;
    std::array<Bytes, 2> accessories, accessoryKeys;
    std::uint64_t scene, revision;
    std::uint32_t message, timeLow, timeHigh;
    std::uint8_t dirty, recorded, enhanced, physics, input;
    int frame, mode, selectedModel, selectedAccessory;
    std::array<float, 10> values;
    static std::array<float, 10> Values(MMDApp& app) {
        return {app.CameraFov(), app.CameraPositionX(), app.CameraDistance(), app.DeltaTime(),
            app.LightColor()[0], app.LightColor()[1], app.LightColor()[2],
            app.LightDirection()[0], app.LightDirection()[1], app.LightDirection()[2]};
    }
    explicit Snapshot(Fixture& f) : fixture(f), paths{Read(f.app->ModelSlot(0)), Read(f.app->ModelSlot(1))},
        models{ModelSnapshot(f.app->ModelSlot(0)), ModelSnapshot(f.app->ModelSlot(1))},
        accessories{Bytes(f.app->AccessorySlot(0), sizeof(mdl::AccessoryRecord)), Bytes(f.app->AccessorySlot(1), sizeof(mdl::AccessoryRecord))},
        accessoryKeys{Bytes(f.app->AccessoryKeys(0), mdl::KeyPoolCapacity(f.app->AccessoryKeys(0)) * sizeof(mdl::AccessoryKey)),
            Bytes(f.app->AccessoryKeys(1), mdl::KeyPoolCapacity(f.app->AccessoryKeys(1)) * sizeof(mdl::AccessoryKey))},
        scene(f.app->SceneGeneration()), revision(f.app->SceneRevision()), message(f.app->MessageSeen()),
        timeLow(f.app->TimeNowLow()), timeHigh(f.app->TimeNowHigh()), dirty(f.app->SceneModified()),
        recorded(f.app->RecordedFrameCountByte1()), enhanced(f.app->EnhancedModelDirty()), physics(f.app->PhysicsResetPending()),
        input(f.app->ViewportInputActive()), frame(f.app->CurrentFrame()), mode(f.app->CameraMode()),
        selectedModel(f.app->SelectedModelSlot()), selectedAccessory(f.app->SelectedAccessorySlot()), values(Values(*f.app)) {}
    void WithPaths(const Path& a, const Path& b) const {
        auto& app = *fixture.app; const auto after = Values(app);
        models[0].ExceptPath(app.ModelSlot(0), a); models[1].ExceptPath(app.ModelSlot(1), b);
        for (unsigned i = 0; i < 2; ++i) {
            accessories[i].Unchanged(app.AccessorySlot(i), sizeof(mdl::AccessoryRecord));
            accessoryKeys[i].Unchanged(app.AccessoryKeys(i), mdl::KeyPoolCapacity(app.AccessoryKeys(i)) * sizeof(mdl::AccessoryKey));
        }
        Check(scene == app.SceneGeneration() && revision == app.SceneRevision() && message == app.MessageSeen() &&
            timeLow == app.TimeNowLow() && timeHigh == app.TimeNowHigh() && dirty == app.SceneModified() &&
            recorded == app.RecordedFrameCountByte1() && enhanced == app.EnhancedModelDirty() && physics == app.PhysicsResetPending() &&
            input == app.ViewportInputActive() && frame == app.CurrentFrame() && mode == app.CameraMode() &&
            selectedModel == app.SelectedModelSlot() && selectedAccessory == app.SelectedAccessorySlot() &&
            !std::memcmp(values.data(), after.data(), sizeof(values)) && app.WindowLayoutReady() && !app.SceneMutationInProgress(),
            "source-path participant preserves scene revision, dirty/physics/UI/input flags, selection and transport/time values");
        fixture.CwdUnchanged();
    }
    void Unchanged() const { WithPaths(paths[0], paths[1]); }
};

void Commit(Fixture& f, const std::shared_ptr<HistoryChange>& change, const char* label = "model source explicit transaction") {
    Check(change && change->Effects() == HistoryEffects::ProjectData && !change->RequiresPhysicsReset(),
        "source-path participant declares ProjectData without physics invalidation");
    std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative(label, change, transaction, SharedHistory::NativeIntent::ExplicitCommit) == MMS_OK && transaction,
        "source-path explicit commit reserves the existing common journal before writing");
    failAfter = 0; const auto result = f.history.CommitNative(std::move(transaction)); failAfter = -1;
    Check(result == MMS_OK && f.history.LastPublicationEffects() == HistoryEffects::ProjectData &&
        !f.history.LastPublicationRequiresPhysicsReset(), "source-path final journal/512-byte publication allocates nothing and freezes metadata-only effects");
}

void RoundTripAndSameString() {
    for (auto recording : {SharedHistory::RecordingMode::AllOperations, SharedHistory::RecordingMode::ExplicitOnly}) {
        Trace("roundtrip", int(recording)); Fixture f; NativePoseTestBinding binding(*f.app, f.history);
        Check(f.history.SetMode(recording) == MMS_OK, "source-path accepts each existing history recording mode");
        if (recording == SharedHistory::RecordingMode::ExplicitOnly) {
            f.app->state.sceneModified = 7; f.app->RecordedFrameCountByte1() = 9;
            f.app->EnhancedModelDirty() = 5; f.app->PhysicsResetPending() = 2;
        }
        const Snapshot before(f); const auto after = Replaced(before.paths[0], L"Q:\\新😀.pmd");
        auto* object = f.app->ModelSlot(0); const auto identity = SceneObjectIdentity(object);
        NativeModelSourcePlan plan; std::shared_ptr<HistoryChange> change;
        Check(plan.Begin(*f.app, 0) == MMS_OK && plan.Set(after.data()) == MMS_OK && plan.Take(change) == MMS_OK && change &&
            plan.IsCurrent() && plan.Object() == object && plan.Journal() == &f.history && plan.PermissionHistory() == &f.history,
            "source-path Take keeps the original complete before image, exact object and journal alive");
        before.Unchanged(); Commit(f, change); before.WithPaths(after, before.paths[1]);
        Check(!plan.IsCurrent() && f.history.UndoCount() == 1 && !f.history.RedoCount() && SceneObjectPresent(object, identity.id),
            "source-path publication advances only one explicit node and invalidates the old before-source check");
        Check(f.history.History(false) == MMS_OK && !f.history.UndoCount() && f.history.RedoCount() == 1,
            "source-path undo restores one complete raw buffer in either recording mode"); before.Unchanged();
        NativeModelSourcePlan noop; std::shared_ptr<HistoryChange> empty;
        std::uint64_t revision = 0, current = 0;
        Check(f.history.Revision(revision) == MMS_OK && noop.Begin(*f.app, 0) == MMS_OK && noop.Set(before.paths[0].data()) == MMS_OK &&
            noop.Take(empty) == MMS_OK && !empty && noop.IsCurrent() && noop.Object() == object && noop.Journal() == &f.history,
            "same source string preserves every tail byte and leaves a usable retained no-op plan");
        Check(f.history.Revision(current) == MMS_OK && current == revision && !f.history.UndoCount() && f.history.RedoCount() == 1,
            "same-string confirmation consumes neither a revision nor the existing redo branch"); before.Unchanged();
        Check(f.history.History(true) == MMS_OK && f.history.UndoCount() == 1 && !f.history.RedoCount() && !noop.IsCurrent() &&
            f.history.LastPublicationEffects() == HistoryEffects::ProjectData && !f.history.LastPublicationRequiresPhysicsReset(),
            "source-path redo needs no source file and does not remain authorized by an obsolete no-op plan");
        before.WithPaths(after, before.paths[1]);
    }
}

void CapturedObjectSelectionAndRank() {
    Trace("captured-object-selection-rank"); Fixture f; NativePoseTestBinding binding(*f.app, f.history);
    NativeModelSourcePlan plan; std::shared_ptr<HistoryChange> change;
    Check(plan.Begin(*f.app, 0) == MMS_OK && plan.Object() == f.app->ModelSlot(0),
        "source-path plan captures the requested original model before selection changes");
    f.app->SetSelectedModelSlot(1); f.app->CameraMode() = 1; f.app->SelectedAccessorySlot() = 1;
    f.app->CurrentFrame() = 251;
    NativeModelSourcePlan notSelected;
    Check(notSelected.Begin(*f.app, 0) == MMS_OK && notSelected.Object() == f.app->ModelSlot(0) && notSelected.IsCurrent(),
        "source-path Begin addresses a supplied model slot even while another model/camera is selected");
    auto& a = *mdl::Mdl(f.app->ModelSlot(0)); auto& b = *mdl::Mdl(f.app->ModelSlot(1));
    // Direct fixture rank/pose changes isolate the participant's dependency
    // set. They are not claimed to exercise the separate order menu/UI.
    std::swap(a.comboSelIndex, b.comboSelIndex); std::swap(a.comboSelIndex2, b.comboSelIndex2);
    a.lightDir[2] = .875f; b.morphKeys[0].value = .625f;
    const Snapshot selected(f); const auto target = Replaced(selected.paths[0], L"Y:\\target-A.pmx");
    Check(plan.IsCurrent() && plan.Set(target.data()) == MMS_OK && plan.Take(change) == MMS_OK && change,
        "source-path plan remains tied to the original object through selection, rank and unrelated pose changes");
    Commit(f, change); selected.WithPaths(target, selected.paths[1]);
    // Change ranks again after publication. A replay that wrongly pins the
    // rank captured at Take must fail this case instead of passing because
    // only the pre-commit ranks ever changed.
    std::swap(a.comboSelIndex, b.comboSelIndex); std::swap(a.comboSelIndex2, b.comboSelIndex2);
    const Snapshot reranked(f);
    Check(f.history.History(false) == MMS_OK, "source-path undo accepts ranks changed after the node was committed");
    reranked.WithPaths(selected.paths[0], selected.paths[1]);
    Check(f.history.History(true) == MMS_OK, "source-path redo preserves the current ranks and unregistered values"); reranked.Unchanged();
}

void CancellationAndBudget() {
    Trace("cancellation");
    {
        Fixture f; NativePoseTestBinding binding(*f.app, f.history); const Snapshot before(f);
        const auto after = Replaced(before.paths[0], L"R:\\cancel.pmd");
        Commit(f, f.Change(after.data())); Check(f.history.History(false) == MMS_OK, "source-path cancellation fixture has a real redo node");
        const auto cancelled = [&] {
            before.Unchanged(); Check(!f.history.UndoCount() && f.history.RedoCount() == 1,
                "source-path cancellation retains the original model and the full redo branch");
        };
        { NativeModelSourcePlan plan; Check(plan.Begin(*f.app, 0) == MMS_OK && plan.Set(after.data()) == MMS_OK,
            "source-path changed draft may be dropped before Take"); } cancelled();
        { auto change = f.Change(after.data()); std::unique_ptr<PreparedHistoryChange> candidate;
          Check(change->Prepare(true, candidate) == MMS_OK && candidate && candidate->IsCurrent(),
              "source-path private candidate may be dropped before publication"); } cancelled();
        { auto change = f.Change(after.data()); std::unique_ptr<SharedHistory::NativeTransaction> transaction;
          Check(f.history.PrepareNative("cancel source reservation", change, transaction, SharedHistory::NativeIntent::ExplicitCommit) == MMS_OK,
              "source-path cancellation may discard a reserved common-history transaction"); transaction.reset(); } cancelled();
        Check(f.history.History(true) == MMS_OK, "source-path redo remains usable after every cancellation stage"); before.WithPaths(after, before.paths[1]);
    }
    Trace("measured-budget");
    {
        Fixture f; NativePoseTestBinding binding(*f.app, f.history); const Snapshot before(f);
        auto change = f.Change(L"R:\\budget.pmd"); const auto charge = change->RetainedBytes();
        Check(charge > 1, "source-path budget uses the actual retained participant charge");
        Check(f.history.Configure(SharedHistory::RecordingMode::ExplicitOnly, 128, charge - 1) == MMS_OK,
            "source-path budget is set immediately below the measured retained charge");
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("under-budget source publication", change, transaction, SharedHistory::NativeIntent::ExplicitCommit) == MMS_E_MEMORY &&
            !transaction && !f.history.UndoCount() && !f.history.RedoCount(), "source-path incomplete budget refuses before any live write"); before.Unchanged();
        Check(f.history.Configure(SharedHistory::RecordingMode::AllOperations, 128, 256ull * 1024 * 1024) == MMS_OK,
            "source-path restores the actual journal budget after refusal");
        Commit(f, change); before.WithPaths(Replaced(before.paths[0], L"R:\\budget.pmd"), before.paths[1]);
    }
}

void AllocationFailure() {
    Trace("allocation-prepare"); Fixture f; NativePoseTestBinding binding(*f.app, f.history); const Snapshot before(f);
    const auto after = Replaced(before.paths[0], L"R:\\allocation.pmd");
    Commit(f, f.Change(after.data())); Check(f.history.History(false) == MMS_OK, "source-path allocation fixture owns a genuine redo branch");
    bool success = false; unsigned failures = 0;
    for (long limit = 0; limit < 1024 && !success; ++limit) {
        NativeModelSourcePlan plan; std::shared_ptr<HistoryChange> change;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        failAfter = limit;
        const auto result = Boundary([&]() -> MmsResult {
            auto status = plan.Begin(*f.app, 0); if (!status) status = plan.Set(after.data());
            if (!status) status = plan.Take(change);
            return status ? status : f.history.PrepareNative("source-path allocation sweep non-SSO journal label", change,
                transaction, SharedHistory::NativeIntent::ExplicitCommit);
        });
        failAfter = -1; Trace("allocation-attempt", int(limit), result); transaction.reset(); before.Unchanged();
        Check(!f.history.UndoCount() && f.history.RedoCount() == 1,
            "source-path allocation refusal/cancellation never consumes redo or records a partial operation");
        if (result == MMS_OK) success = true;
        else { ++failures; Check(result == MMS_E_MEMORY && !transaction, "source-path allocation failure is reported as memory without a live reservation"); }
    }
    Check(success && failures >= 2, "source-path fault sweep reaches private state/owner and common-history allocation before eventual preparation");
    Commit(f, f.Change(after.data())); before.WithPaths(after, before.paths[1]);
}

void PermissionBeforeAllocationAndOwner() {
    Trace("permission-before-allocation-owner"); Fixture f; StorageStore storage;
    unsigned gateCalls = 0; MmsResult permission = MMS_E_BUSY;
    SharedHistory permissionHistory(storage, [&] { ++gateCalls; return permission; });
    NativePoseTestBinding binding(*f.app, permissionHistory); const Snapshot before(f); auto* object = f.app->ModelSlot(0);
    ForgetSceneObject(object); MmsObjectRef identity{};
    Check(!FindSceneObjectIdentity(object, identity), "source-path permission fixture starts without an existing object identity");
    NativeModelSourcePlan denied; failAfter = 0; const auto result = denied.Begin(*f.app, 0); failAfter = -1;
    Check(result == MMS_E_BUSY && gateCalls == 1 && !FindSceneObjectIdentity(object, identity) &&
        !denied.IsCurrent() && !denied.Object() && !denied.Journal() && !denied.PermissionHistory(),
        "source-path real journal gate refuses before State allocation, identity registration or retained-owner acquisition");
    before.Unchanged(); Check(!permissionHistory.UndoCount() && !permissionHistory.RedoCount() && !f.history.UndoCount(),
        "source-path permission refusal creates no common or parallel history");
    permission = MMS_OK;
    Check(denied.Begin(*f.app, 0) == MMS_OK && denied.Object() == object && denied.Journal() == &permissionHistory &&
        denied.PermissionHistory() == &permissionHistory && denied.IsCurrent(),
        "source-path failed Begin may retry using the same gate after actual authorization");
    before.Unchanged();
}

void InitialSourceStaleness() {
    // Each case has its own plan/owner so an old stale identity cannot pollute
    // the next case or accidentally be accepted through pointer reuse.
    for (unsigned route = 0; route < 7; ++route) {
        Trace("initial-stale", int(route)); Fixture f; NativePoseTestBinding binding(*f.app, f.history);
        NativeModelSourcePlan plan; Check(plan.Begin(*f.app, 0) == MMS_OK && plan.IsCurrent(), "source-path staleness starts with a valid captured source");
        auto* object = f.app->ModelSlot(0); const auto previousIdentity = SceneObjectIdentity(object);
        const auto original = Read(object), other = Read(f.app->ModelSlot(1));
        if (route == 0) { NativePoseTestBinding nested(*f.app, f.history); }
        if (route == 1) f.app->SceneModified() = 7; // advances the real app SceneRevision
        if (route == 2) f.app->AdvanceSceneGeneration();
        if (route == 3) Check(f.history.Reset() == MMS_OK, "source-path staleness advances the real permission journal revision");
        if (route == 4) {
            ForgetSceneObject(object); const auto replacement = SceneObjectIdentity(object);
            Check(!SameId(previousIdentity.id, replacement.id), "source-path staleness actually retires and replaces the stable ID");
        }
        if (route == 5) {
            auto owner = RetainSceneObject(*f.app, MMS_OBJECT_MODEL, 0);
            auto absent = owner->PreparePresence(false); Check(absent->IsCurrent(), "source-path stale fixture can privately remove its exact owner");
            absent->Publish(); absent.reset(); auto restored = owner->PreparePresence(true);
            Check(restored->IsCurrent(), "source-path stale fixture can restore its owner without changing the journal"); restored->Publish(); restored.reset();
            const auto current = SceneObjectIdentity(object);
            Check(SameId(current.id, previousIdentity.id) && current.generation != previousIdentity.generation,
                "source-path staleness has a true object-generation transition at the same pointer/ID");
        }
        if (route == 6) f.app->ModelSlot(0) = f.app->ModelSlot(1);
        Check(!plan.IsCurrent(), "source-path initial check rejects an obsolete binding/scene/revision/object generation or occupant");
        std::shared_ptr<HistoryChange> change;
        Check(plan.Set(L"R:\\must-not-apply.pmd") == MMS_E_STALE && plan.Take(change) == MMS_E_STALE && !change,
            "source-path stale plan cannot mint a new writable participant");
        if (route == 6) f.app->ModelSlot(0) = object;
        Match(object, original); Match(f.app->ModelSlot(1), other);
        Check(!f.history.UndoCount() && !f.history.RedoCount(), "source-path stale initial operation records no history");
        f.CwdUnchanged();
    }
}

void ExternalBufferChanges() {
    for (unsigned route = 0; route < 3; ++route) {
        Trace("external-raw-buffer", int(route)); Fixture f; NativePoseTestBinding binding(*f.app, f.history);
        const Snapshot before(f); const auto target = Replaced(before.paths[0], L"R:\\external-check.pmd");
        auto change = f.Change(target.data()); auto& model = *mdl::Mdl(f.app->ModelSlot(0));
        if (route == 0) {
            std::unique_ptr<PreparedHistoryChange> candidate;
            Check(change->Prepare(true, candidate) == MMS_OK && candidate && candidate->IsCurrent(), "source-path private raw-buffer candidate starts valid");
            model.path[230] ^= wchar_t(1); const auto external = Read(f.app->ModelSlot(0));
            Check(!candidate->IsCurrent(), "source-path candidate notices an external change beyond the first NUL");
            std::unique_ptr<SharedHistory::NativeTransaction> transaction;
            Check(f.history.PrepareNative("stale raw source", change, transaction) == MMS_E_STALE && !transaction,
                "source-path journal refuses a candidate whose full original buffer changed"); before.WithPaths(external, before.paths[1]);
        } else if (route == 1) {
            std::unique_ptr<SharedHistory::NativeTransaction> transaction;
            Check(f.history.PrepareNative("late raw source mutation", change, transaction) == MMS_OK,
                "source-path journal captures a complete candidate before the final raw-buffer recheck");
            model.path[231] ^= wchar_t(1); const auto external = Read(f.app->ModelSlot(0));
            Check(f.history.CommitNative(std::move(transaction)) == MMS_E_STALE && !f.history.UndoCount(),
                "source-path late raw-buffer mutation refuses final publication instead of overwriting the external value");
            before.WithPaths(external, before.paths[1]);
        } else {
            Commit(f, change); model.path[232] ^= wchar_t(1); const auto external = Read(f.app->ModelSlot(0));
            Check(f.history.History(false) == MMS_E_STALE && f.history.UndoCount() == 1 && !f.history.RedoCount(),
                "source-path undo rejects an externally modified complete after image without moving the cursor");
            before.WithPaths(external, before.paths[1]); std::memcpy(model.path, target.data(), sizeof(target));
            Check(f.history.History(false) == MMS_OK, "source-path failed stale replay does not poison a later valid full-buffer replay"); before.Unchanged();
        }
    }
}

void DeletionRestorationGeneration() {
    Trace("delete-restore-generation"); Fixture f; NativePoseTestBinding binding(*f.app, f.history); const Snapshot before(f);
    const auto after = Replaced(before.paths[0], L"R:\\retained-source.pmd"); Commit(f, f.Change(after.data()));
    auto* object = f.app->ModelSlot(0); const auto initial = SceneObjectIdentity(object);
    auto owner = RetainSceneObject(*f.app, MMS_OBJECT_MODEL, 0); auto removal = owner->CaptureRemoval();
    std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("remove source-edited model", removal, transaction) == MMS_OK &&
        f.history.CommitNative(std::move(transaction)) == MMS_OK && !f.app->ModelSlot(0) && !owner->Present(),
        "source-path chain removes the original retained model through its real scene-object history participant");
    Check(f.history.History(false) == MMS_OK && f.app->ModelSlot(0) == object && owner->Present(),
        "source-path chain restores the same model before traversing its older source-path node");
    const auto restored = SceneObjectIdentity(object);
    Check(SameId(initial.id, restored.id) && initial.generation != restored.generation,
        "source-path deletion restoration renews the actual object generation without replacing its stable identity");
    Check(f.history.History(false) == MMS_OK, "source-path older undo accepts a legitimately restored generation"); before.Unchanged();
    Check(f.history.History(true) == MMS_OK, "source-path older redo revalidates against the current restored generation"); before.WithPaths(after, before.paths[1]);
    Check(f.history.History(true) == MMS_OK && !f.app->ModelSlot(0), "source-path later model deletion still replays in journal order");
    Check(f.history.History(false) == MMS_OK && f.app->ModelSlot(0) == object && f.history.History(false) == MMS_OK,
        "source-path second restoration cycle also refreshes the replay candidate generation"); before.Unchanged();
    removal.reset(); owner.reset(); Check(f.history.Reset() == MMS_OK, "source-path release all retained nodes while scene owners are alive"); f.history.Collect();
}

void UnboundAndBoundaries() {
    Trace("unbound");
    {
        Fixture f; Check(!BoundNativePoseHistory(f.app.get()) && !NativeHistory(f.app.get()), "source-path ordinary fixture has no migration or runtime edit host");
        const Snapshot before(f); const auto after = Replaced(before.paths[0], L"R:\\unrecorded-source.pmd");
        NativeModelSourcePlan plan; std::shared_ptr<HistoryChange> change; std::unique_ptr<PreparedHistoryChange> candidate;
        Check(plan.Begin(*f.app, 0) == MMS_OK && !plan.Journal() && !plan.PermissionHistory() && plan.Set(after.data()) == MMS_OK &&
            plan.Take(change) == MMS_OK && change && plan.IsCurrent(), "source-path unbound full configuration still prepares the same safe native participant");
        Check(change->Prepare(true, candidate) == MMS_OK && candidate && candidate->IsCurrent(), "source-path unrecorded caller obtains a validated real candidate");
        failAfter = 0; candidate->Publish(); failAfter = -1; candidate.reset();
        before.WithPaths(after, before.paths[1]); Check(!plan.IsCurrent() && !f.history.UndoCount() && !f.history.RedoCount(),
            "source-path unbound publication is atomic without silently installing a journal or creating an undo entry");
        NativeModelSourcePlan noop; std::shared_ptr<HistoryChange> empty;
        Check(noop.Begin(*f.app, 0) == MMS_OK && noop.Set(after.data()) == MMS_OK && noop.Take(empty) == MMS_OK &&
            !empty && noop.IsCurrent() && noop.Object() == f.app->ModelSlot(0) && !noop.Journal() && !noop.PermissionHistory(),
            "source-path unbound same-string Take also retains its original source/owner for the caller's final check");
        before.WithPaths(after, before.paths[1]);
    }
    Trace("input-boundaries");
    {
        Fixture f; NativePoseTestBinding binding(*f.app, f.history); const Snapshot before(f);
        NativeModelSourcePlan empty; std::shared_ptr<HistoryChange> change;
        Check(!empty.IsCurrent() && !empty.Object() && !empty.Journal() && !empty.PermissionHistory() &&
            empty.Set(L"R:\\not-begun.pmd") == MMS_E_NOT_READY && empty.Take(change) == MMS_E_NOT_READY,
            "source-path empty plan exposes no source or writable participant");
        Check(empty.Begin(*f.app, unsigned(kModelSlotCount)) == MMS_E_ARGUMENT && empty.Begin(*f.app, 2) == MMS_E_ARGUMENT,
            "source-path Begin rejects an out-of-range or empty model slot");
        NativeModelSourcePlan plan; Check(plan.Begin(*f.app, 0) == MMS_OK, "source-path string boundary begins with a valid object");
        std::array<wchar_t, 256> tooLong{}; tooLong.fill(L'x');
        Check(plan.Set(nullptr) == MMS_E_ARGUMENT && plan.Set(tooLong.data()) == MMS_E_ARGUMENT && plan.IsCurrent(),
            "source-path null and unterminated/256-character input cannot change the private or live source");
        std::wstring maximum(255, L'界'); const auto after = Replaced(before.paths[0], maximum.c_str());
        Check(plan.Set(maximum.c_str()) == MMS_OK && plan.Take(change) == MMS_OK && change,
            "source-path accepts exactly 255 UTF16 units plus its final terminator");
        Commit(f, change); before.WithPaths(after, before.paths[1]); Check(f.history.History(false) == MMS_OK, "source-path maximum-length undo retains the entire old tail"); before.Unchanged();
    }
}

void MixedPluginAggregate() {
    Trace("mixed-plugin-aggregate"); Fixture f; NativePoseTestBinding binding(*f.app, f.history); const Snapshot before(f);
    const auto after = Replaced(before.paths[0], L"R:\\mixed-source.pmd"); auto change = f.Change(after.data());
    camera_multiply_checks::PluginState plugin; StorageClient storage; storage.id = "test.model-source-aggregate";
    f.storage.Api(storage); auto serializer = plugin.Serializer(); MmsHandle provider = 0;
    Check(f.storage.Register(storage, Text(storage.id), &serializer, sizeof(serializer), &provider) == MMS_OK,
        "source-path aggregate registers the real serializer beside the existing journal");
    EditStore edits(f.history, f.storage); EditClient client; client.id = storage.id; auto api = edits.Api(client);
    std::uint64_t revision = 0; MmsHandle transaction = 0; auto participant = plugin.Participant(Text(client.id));
    Check(f.history.Revision(revision) == MMS_OK && api.begin(api.context, Text("source path plus plugin"), revision, &transaction) == MMS_OK &&
        api.join(api.context, transaction, &participant) == MMS_OK && edits.JoinNative(client, transaction, change) == MMS_OK &&
        api.prepare(api.context, transaction) == MMS_OK && api.commit(api.context, transaction) == MMS_OK,
        "source-path and real plugin private state publish in one actual EditStore.JoinNative transaction");
    Check(plugin.value == 42 && f.history.UndoCount() == 1 && !f.history.RedoCount() &&
        f.history.LastPublicationEffects() == HistoryEffects::All && f.history.LastPublicationRequiresPhysicsReset(),
        "source-path mixed plugin node freezes conservative All/physics instead of its native-only ProjectData mask");
    before.WithPaths(after, before.paths[1]);
    Check(f.history.History(false) == MMS_OK && plugin.value == 0 && f.history.RedoCount() == 1 &&
        f.history.LastPublicationEffects() == HistoryEffects::All, "source-path real mixed undo restores both participants"); before.Unchanged();
    Check(f.history.History(true) == MMS_OK && plugin.value == 42 && f.history.UndoCount() == 1 &&
        f.history.LastPublicationEffects() == HistoryEffects::All, "source-path real mixed redo restores both participants without source files");
    before.WithPaths(after, before.paths[1]); change.reset();
    Check(f.history.Reset() == MMS_OK, "source-path mixed nodes retire before their plugin context"); f.history.Collect();
    Check(f.storage.Unregister(storage, provider) == MMS_OK, "source-path serializer unregisters only after its history is released");
}
} // namespace model_source_checks
void NativeModelSourcePathChecks() {
    model_source_checks::RoundTripAndSameString();
    model_source_checks::CapturedObjectSelectionAndRank();
    model_source_checks::CancellationAndBudget();
    model_source_checks::AllocationFailure();
    model_source_checks::PermissionBeforeAllocationAndOwner();
    model_source_checks::InitialSourceStaleness();
    model_source_checks::ExternalBufferChanges();
    model_source_checks::DeletionRestorationGeneration();
    model_source_checks::UnboundAndBoundaries();
    model_source_checks::MixedPluginAggregate();
}
#endif
