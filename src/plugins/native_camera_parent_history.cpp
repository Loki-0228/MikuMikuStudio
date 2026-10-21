#include "native_camera_parent_history.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_camera_follow_ui.hpp"
#include "native_object_commands.hpp"
#include "native_pose_scope.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_order_projection.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include "../window/accessory_bone_combo_cache.hpp"
#include <CommCtrl.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <vector>

namespace mikumikustudio::plugins {
namespace {
constexpr wchar_t kCaptureOwner[]=L"MMS.Native.CameraParentCapture.V1";
struct CaptureAdmission {
    inline static thread_local CaptureAdmission* active = nullptr;
    MMDApp& app; const HWND main, combo, list, capture;
    CaptureAdmission* previous;
    HANDLE previousOwner=nullptr; bool tagged=false;
    CaptureAdmission(MMDApp& value, HWND control, HWND popup)
        : app(value), main(value.MainWindow()), combo(control), list(popup), capture(GetCapture()), previous(active) {
        if(capture&&(capture==combo||capture==list)){
            previousOwner=GetPropW(combo,kCaptureOwner);
            if(!SetPropW(combo,kCaptureOwner,reinterpret_cast<HANDLE>(this)))throw std::bad_alloc();
            tagged=true;
        }
        active = this;
    }
    ~CaptureAdmission() {
        if(tagged&&GetPropW(combo,kCaptureOwner)==reinterpret_cast<HANDLE>(this)){
            if(previousOwner)SetPropW(combo,kCaptureOwner,previousOwner);else RemovePropW(combo,kCaptureOwner);
        }
        active = previous;
    }
    bool Allows(HWND current) const noexcept {
        // The trusted entry validated GetComboBoxInfo once. Do not send a
        // combo query from the final history publication check. Destroying
        // the window removes this token, including same-HWND recreation.
        return tagged&&current&&current==capture&&(current==combo||current==list)&&app.MainWindow()==main&&
            GetPropW(combo,kCaptureOwner)==reinterpret_cast<HANDLE>(const_cast<CaptureAdmission*>(this));
    }
};
bool Ready(MMDApp& app, bool replay = false) noexcept {
    const bool own = replay && NativeObjectPublicationAuthorized(&app);
    const auto capture = GetCapture();
    const bool ownCapture = CaptureAdmission::active && &CaptureAdmission::active->app == &app && CaptureAdmission::active->Allows(capture);
    return (!app.SceneMutationInProgress() || own) && (!bake::UiLocked() || own) &&
        (!NativeObjectOperationActive(&app) || own) && !NativeObjectUiPending(&app) &&
        !SceneSaveBlocksEdits() && (!capture || ownCapture) && !app.PlaybackActive() && !app.FrameStepPlayback() &&
        !app.state.recordPlaybackStartPending && !app.FrameRangeDialog() && !app.state.frameCopyDialog &&
        !app.RecordingWindow() && !app.GravitySettingDialog() && !app.GroundShadowColorDialog() &&
        !app.AccessoryOrderArray() && !app.state.accessoryEditDialogOpen && !NativeModelOutlineDialogBlocks(app);
}
bool Selectable(const mdl::BoneRecord& bone) noexcept {
    return bone.type < mdl::BoneType::InertTip || bone.type == mdl::BoneType::FixedAxis;
}
// All remembered pointers are tested through the current scene membership
// before dereferencing the model or table. Runtime generation is deliberately
// absent here: a restored retained owner remains a valid long-term source.
struct ParentSource {
    MMDApp& app;
    SharedHistory* const history;
    const std::uint64_t scene, pool;
    const int slot;
    unsigned char* const raw;
    mdl::BoneRecord* const bones;
    const std::uint32_t count;
    std::shared_ptr<RetainedSceneObject> owner;
    MmsObjectRef identity{};
    explicit ParentSource(MMDApp& value, bool retain = true)
        : app(value), history(BoundNativePoseHistory(&value)), scene(value.SceneGeneration()),
          pool(mdl::KeyPoolIdentity(value.CameraKeys())), slot(value.CameraParentModel()), raw(value.ModelSlot(slot)),
          bones(mdl::Mdl(raw)->boneTable), count(mdl::Mdl(raw)->boneCount),
          owner(retain ? RetainSceneObject(value, MMS_OBJECT_MODEL, unsigned(slot)) : nullptr) {
        if (!FindSceneObjectIdentity(raw, identity)) identity = SceneObjectIdentity(raw);
    }
    bool Current() const noexcept {
        if (BoundNativePoseHistory(&app) != history || app.SceneGeneration() != scene || !pool ||
            mdl::KeyPoolIdentity(app.CameraKeys()) != pool || app.CameraParentModel() != slot ||
            slot < 0 || slot >= kModelSlotCount || app.ModelSlot(slot) != raw ||
            (owner && !owner->Present()) || !SceneObjectPresent(raw, identity.id)) return false;
        for (unsigned i = 0; i < kModelSlotCount; ++i) if (int(i) != slot && app.ModelSlot(i) == raw) return false;
        return mdl::Mdl(raw)->boneTable == bones && mdl::Mdl(raw)->boneCount == count;
    }
    std::uint64_t Generation() const noexcept { MmsObjectRef current{}; return FindSceneObjectIdentity(raw,current) ? current.generation : 0; }
};
bool HasParent(MMDApp& app) noexcept {
    const int slot = app.CameraParentModel();
    return slot >= 0 && slot < kModelSlotCount && app.ModelSlot(slot) &&
        mdl::Mdl(app.ModelSlot(slot))->boneTable && mdl::Mdl(app.ModelSlot(slot))->boneCount;
}
struct ParentChange final : HistoryChange, std::enable_shared_from_this<ParentChange> {
    ParentSource source;
    const std::int32_t before, after;
    const std::uint64_t initialBinding, initialObject;
    std::uint64_t initialRevision = 0, serial = 1;
    bool applied = false, published = false;
    ParentChange(MMDApp& app, std::int32_t value)
        : source(app), before(app.CameraParentBone()), after(value), initialBinding(BoundNativePoseGeneration(&app)),
          initialObject(source.Generation()) {}
    bool InitialCurrent() const noexcept {
        if (published) return true;
        std::uint64_t revision = 0;
        return source.Current() && BoundNativePoseGeneration(&source.app) == initialBinding &&
            source.Generation() == initialObject && source.history->Revision(revision) == MMS_OK &&
            revision == initialRevision && source.app.CameraParentBone() == before;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<ParentChange> change;
        const bool forward;
        const std::uint64_t serial, binding, object;
        const HWND capture;
        std::uint64_t revision = 0;
        const int frame;
        const std::int32_t expected;
        mutable resource_budget::Collector resources;
        std::uint64_t bytes = 0;
        bool consumed = false;
        Candidate(std::shared_ptr<ParentChange> value, bool direction)
            : change(std::move(value)), forward(direction), serial(change->serial),
              binding(BoundNativePoseGeneration(&change->source.app)), object(change->source.Generation()), capture(GetCapture()),
              frame(change->source.app.CurrentFrame()), expected(change->source.app.CameraParentBone()) {}
        bool IsCurrent() const noexcept override {
            const auto& c = *change; const auto& s = c.source; std::uint64_t currentRevision = 0;
            return !consumed && serial == c.serial && forward != c.applied && s.Current() && c.InitialCurrent() &&
                Ready(s.app, true) && binding == BoundNativePoseGeneration(&s.app) && object == s.Generation() &&
                frame == s.app.CurrentFrame() && GetCapture() == capture && expected == s.app.CameraParentBone() &&
                s.history->Revision(currentRevision) == MMS_OK && currentRevision == revision &&
                s.owner->CoversBookkeeping(resources) && resources.Result().Complete();
        }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            auto& c = *change;
            c.source.app.CameraParentBone() = forward ? c.after : c.before;
            c.applied = forward; c.published = true; ++c.serial; consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!source.Current() || !InitialCurrent() || forward == applied) return MMS_E_STALE;
            if (!Ready(source.app, true)) return MMS_E_BUSY;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            if (auto r = source.history->Revision(candidate->revision)) return r;
            if (auto r = source.owner->MeasureBookkeeping(candidate->resources)) return r;
            if (!candidate->resources.Result().Complete()) return MMS_E_NOT_READY;
            candidate->bytes = resource_budget::Collector::Sum(RetainedBytes(), candidate->resources.Result().totalBytes);
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this) + 4 * sizeof(void*); }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};

struct Controls {
    ParentSource& source;
    const HWND main, models, bones;
    const std::uint64_t binding, object;
    const int language, value;
    explicit Controls(ParentSource& value)
        : source(value), main(value.app.MainWindow()), models(GetDlgItem(main, panel::kMainComboNormal)),
          bones(GetDlgItem(main, panel::kBoneRegisterCombo)), binding(BoundNativePoseGeneration(&value.app)),
          object(value.Generation()), language(value.app.EnglishUI()), value(value.app.CameraParentBone()) {}
    bool CurrentIdentity() const noexcept {
        return source.Current() && BoundNativePoseGeneration(&source.app) == binding && source.Generation() == object &&
            source.app.EnglishUI() == language && source.app.MainWindow() == main && IsWindow(main) &&
            models && bones && GetDlgItem(main, panel::kMainComboNormal) == models &&
            GetDlgItem(main, panel::kBoneRegisterCombo) == bones;
    }
    bool Current() const noexcept { return CurrentIdentity() && source.app.CameraParentBone() == value; }
    LRESULT Send(HWND window, UINT message, WPARAM w = 0, LPARAM l = 0) const {
        if (!Current()) throw MMS_E_STALE;
        const auto result = SendMessageW(window, message, w, l);
        if (!Current()) throw MMS_E_STALE;
        return result;
    }
};
using Projection = model_order::Projection<kModelSlotCount>;
Projection Order(MMDApp& app) noexcept {
    return model_order::Project<kModelSlotCount>([&](std::size_t slot) noexcept {
        const auto* raw = app.ModelSlot(int(slot)); return raw ? int(mdl::Mdl(raw)->comboSelIndex) : -1;
    });
}
// Float payloads are copied and compared as bytes. Unchanged fields, including
// noncanonical NaNs and signed zeros, are never normalized by an attachment.
struct ModelValues {
    std::int32_t parent, bone;
    std::array<float, 3> position, rotation;
    float distance;
};
ModelValues ReadModelValues(MMDApp& app) noexcept {
    ModelValues value{}; value.parent = app.CameraParentModel(); value.bone = app.CameraParentBone();
    for (unsigned i = 0; i < 3; ++i) {
        std::memcpy(&value.position[i], &app.CameraPositionAxis(i), sizeof(float));
        std::memcpy(&value.rotation[i], &app.CameraRotation()[i], sizeof(float));
    }
    std::memcpy(&value.distance, &app.CameraDistance(), sizeof(float)); return value;
}
std::uint16_t Different(const ModelValues& a, const ModelValues& b) noexcept {
    std::uint16_t mask = (a.parent != b.parent ? 1 : 0) | (a.bone != b.bone ? 2 : 0);
    for (unsigned i = 0; i < 3; ++i) {
        if (std::memcmp(&a.position[i], &b.position[i], sizeof(float))) mask |= std::uint16_t(4u << i);
        if (std::memcmp(&a.rotation[i], &b.rotation[i], sizeof(float))) mask |= std::uint16_t(32u << i);
    }
    if (std::memcmp(&a.distance, &b.distance, sizeof(float))) mask |= 256;
    return mask;
}
void WriteModelValues(MMDApp& app, const ModelValues& value, std::uint16_t mask) noexcept {
    if (mask & 1) app.CameraParentModel() = value.parent;
    if (mask & 2) app.CameraParentBone() = value.bone;
    for (unsigned i = 0; i < 3; ++i) {
        if (mask & (4u << i)) std::memcpy(&app.CameraPositionAxis(i), &value.position[i], sizeof(float));
        if (mask & (32u << i)) std::memcpy(&app.CameraRotation()[i], &value.rotation[i], sizeof(float));
    }
    if (mask & 256) std::memcpy(&app.CameraDistance(), &value.distance, sizeof(float));
}
int FirstBone(const mdl::ModelRecord& model) noexcept {
    if (!model.boneTable) return -1;
    for (std::uint32_t i = 0; i < model.boneCount; ++i) if (Selectable(model.boneTable[i])) return int(i);
    return -1;
}
struct ModelOwner {
    int slot = -1; unsigned char* raw = nullptr; mdl::BoneRecord* bones = nullptr; std::uint32_t count = 0;
    MmsObjectRef identity{}; std::shared_ptr<RetainedSceneObject> owner;
    bool Current(MMDApp& app) const noexcept {
        if (slot < 0 || slot >= kModelSlotCount || app.ModelSlot(slot) != raw || !owner || !owner->Present() ||
            !SceneObjectPresent(raw, identity.id)) return false;
        for (unsigned i = 0; i < kModelSlotCount; ++i) if (int(i) != slot && app.ModelSlot(i) == raw) return false;
        return mdl::Mdl(raw)->boneTable == bones && mdl::Mdl(raw)->boneCount == count;
    }
    std::uint64_t Generation() const noexcept {
        MmsObjectRef current{}; return FindSceneObjectIdentity(raw, current) ? current.generation : 0;
    }
    MmsResult Capture(MMDApp& app, int value) {
        if (value < 0 || value >= kModelSlotCount || !app.ModelSlot(value)) return MMS_E_NOT_READY;
        slot = value; raw = app.ModelSlot(slot); bones = mdl::Mdl(raw)->boneTable; count = mdl::Mdl(raw)->boneCount;
        if (!bones || !count) return MMS_E_NOT_READY;
        owner = RetainSceneObject(app, MMS_OBJECT_MODEL, unsigned(slot));
        if (!FindSceneObjectIdentity(raw, identity)) return MMS_E_STALE;
        return Current(app) ? MMS_OK : MMS_E_STALE;
    }
};
struct ModelChange final : HistoryChange, std::enable_shared_from_this<ModelChange> {
    MMDApp& app; SharedHistory* const history;
    const std::uint64_t scene, pool, binding;
    const int frame; const HWND capture;
    const ModelValues before;
    ModelValues after;
    std::array<ModelOwner, 2> owners;
    std::array<std::uint64_t, 2> initialObjects{};
    unsigned ownerCount = 0;
    std::uint16_t mask = 0;
    std::uint64_t initialRevision = 0, serial = 1;
    bool applied = false, published = false, sampled = false;
    // This private pre-publication sample is only a stale check. Undo/redo use
    // the saved authoring scalars and never resample a later bone pose.
    std::array<float, 19> pivot{};
    explicit ModelChange(MMDApp& value)
        : app(value), history(BoundNativePoseHistory(&value)), scene(value.SceneGeneration()),
          pool(mdl::KeyPoolIdentity(value.CameraKeys())), binding(BoundNativePoseGeneration(&value)),
          frame(value.CurrentFrame()), capture(GetCapture()), before(ReadModelValues(value)), after(before) {}
    MmsResult AddOwner(int slot) {
        if (slot < 0) return MMS_OK;
        for (unsigned i = 0; i < ownerCount; ++i) if (owners[i].slot == slot) return MMS_OK;
        if (ownerCount == owners.size()) return MMS_E_FAILED;
        if (auto r = owners[ownerCount].Capture(app, slot)) return r;
        initialObjects[ownerCount] = owners[ownerCount].Generation(); ++ownerCount; return MMS_OK;
    }
    bool SourcesCurrent() const noexcept {
        if (BoundNativePoseHistory(&app) != history || app.SceneGeneration() != scene || !pool ||
            mdl::KeyPoolIdentity(app.CameraKeys()) != pool) return false;
        for (unsigned i = 0; i < ownerCount; ++i) if (!owners[i].Current(app)) return false;
        return true;
    }
    const ModelOwner* Owner(int slot) const noexcept {
        for (unsigned i = 0; i < ownerCount; ++i) if (owners[i].slot == slot) return &owners[i];
        return nullptr;
    }
    bool InitialCurrent() const noexcept {
        if (published) return true;
        std::uint64_t revision = 0;
        if (!SourcesCurrent() || BoundNativePoseGeneration(&app) != binding || app.CurrentFrame() != frame ||
            GetCapture() != capture || Different(ReadModelValues(app), before) ||
            history->Revision(revision) != MMS_OK || revision != initialRevision) return false;
        for (unsigned i = 0; i < ownerCount; ++i) if (owners[i].Generation() != initialObjects[i]) return false;
        if (after.parent >= 0) {
            const auto* source = Owner(after.parent);
            if (!source || FirstBone(*mdl::Mdl(source->raw)) != after.bone) return false;
        }
        if (sampled) {
            const auto* source = Owner(before.parent);
            if (!source || before.bone < 0 || std::uint32_t(before.bone) >= source->count) return false;
            const auto& bone = source->bones[before.bone];
            if (std::memcmp(pivot.data(), bone.position, sizeof(float) * 3) ||
                std::memcmp(pivot.data() + 3, bone.matInit, sizeof(float) * 16)) return false;
        }
        return true;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<ModelChange> change;
        const bool forward; const std::uint64_t serial, binding;
        const int frame; const HWND capture; const ModelValues expected;
        std::array<std::uint64_t, 2> objects{};
        std::uint64_t revision = 0, bytes = 0; mutable resource_budget::Collector resources;
        bool consumed = false;
        Candidate(std::shared_ptr<ModelChange> value, bool direction)
            : change(std::move(value)), forward(direction), serial(change->serial),
              binding(BoundNativePoseGeneration(&change->app)), frame(change->app.CurrentFrame()),
              capture(GetCapture()), expected(ReadModelValues(change->app)) {
            for (unsigned i = 0; i < change->ownerCount; ++i) objects[i] = change->owners[i].Generation();
        }
        bool IsCurrent() const noexcept override {
            const auto& c = *change; std::uint64_t now = 0;
            if (consumed || serial != c.serial || forward == c.applied || !c.SourcesCurrent() || !c.InitialCurrent() ||
                !Ready(c.app, true) || binding != BoundNativePoseGeneration(&c.app) || frame != c.app.CurrentFrame() ||
                capture != GetCapture() || Different(ReadModelValues(c.app), expected) ||
                (!(c.mask & 1) && c.app.CameraParentModel() != c.before.parent) ||
                c.history->Revision(now) != MMS_OK || now != revision || !resources.Result().Complete()) return false;
            for (unsigned i = 0; i < c.ownerCount; ++i)
                if (objects[i] != c.owners[i].Generation() || !c.owners[i].owner->CoversBookkeeping(resources)) return false;
            return true;
        }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            auto& c = *change; WriteModelValues(c.app, forward ? c.after : c.before, c.mask);
            c.applied = forward; c.published = true; ++c.serial; consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            // Registration undo can evaluate an older track parent. A change
            // owning the parent field safely restores its saved attachment;
            // an index-only same-model reset must keep the 450 source rule.
            if (!SourcesCurrent() || !InitialCurrent() || forward == applied ||
                (!(mask & 1) && app.CameraParentModel() != before.parent)) return MMS_E_STALE;
            if (!Ready(app, true)) return MMS_E_BUSY;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            if (auto r = history->Revision(candidate->revision)) return r;
            for (unsigned i = 0; i < ownerCount; ++i)
                if (auto r = owners[i].owner->MeasureBookkeeping(candidate->resources)) return r;
            if (!candidate->resources.Result().Complete()) return MMS_E_NOT_READY;
            candidate->bytes = resource_budget::Collector::Sum(RetainedBytes(), candidate->resources.Result().totalBytes);
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this) + 4 * sizeof(void*); }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
MmsResult MakeModelChange(MMDApp& app, int target, std::shared_ptr<ModelChange>& out) {
    out.reset(); auto* history = BoundNativePoseHistory(&app); if (!history) return MMS_E_NOT_READY;
    if (auto r = history->CheckNativeNavigation()) return r;
    if (auto r = history->CheckNativeEdit()) return r;
    if (!Ready(app)) return MMS_E_BUSY;
    if (target < -1 || target >= kModelSlotCount) return MMS_E_ARGUMENT;
    if (!mdl::KeyPoolIdentity(app.CameraKeys())) return MMS_E_NOT_READY;
    auto change = std::make_shared<ModelChange>(app);
    if (auto r = history->Revision(change->initialRevision)) return r;
    if (auto r = change->AddOwner(change->before.parent)) return r;
    if (auto r = change->AddOwner(target)) return r;
    auto& after = change->after; after.parent = target;
    if (target >= 0) {
        const auto* owner = change->Owner(target); after.bone = FirstBone(*mdl::Mdl(owner->raw));
        if (after.bone < 0) return MMS_E_NOT_READY;
        if (change->before.parent < 0) {
            after.position[0] = after.position[1] = 0;
            const auto distance = change->before.distance;
            after.position[2] = std::isfinite(distance) && distance < -.1f ? distance : -45.f;
            after.rotation.fill(0);
        }
    } else if (change->before.parent >= 0) {
        const auto* owner = change->Owner(change->before.parent); const auto index = change->before.bone;
        if (index < 0 || std::uint32_t(index) >= owner->count) return MMS_E_NOT_READY;
        const auto& bone = owner->bones[index]; change->sampled = true;
        std::memcpy(change->pivot.data(), bone.position, sizeof(float) * 3);
        std::memcpy(change->pivot.data() + 3, bone.matInit, sizeof(float) * 16);
        const double px = change->pivot[0], py = change->pivot[1], pz = change->pivot[2];
        const auto* matrix = change->pivot.data() + 3;
        for (unsigned i = 0; i < 3; ++i)
            after.position[i] = float(py * matrix[4 + i] + px * matrix[i] + pz * matrix[8 + i] + matrix[12 + i]);
        after.distance = -20.f; after.rotation.fill(0);
    }
    change->mask = Different(change->before, after);
    // Parent and bone identify one binding. Replaying a parent change must
    // restore its saved bone even when both endpoints originally used index
    // zero; an intervening registration undo may have evaluated another bone.
    if (change->mask & 1) change->mask |= 2;
    if (!change->InitialCurrent()) return MMS_E_STALE;
    out = std::move(change); return MMS_OK;
}
// Every native read can pump custom control code. Capture the entire model
// mapping before the first read, then never resolve a request against a newly
// inserted/replaced object. Only the two accepted owners are retained by the
// resulting history record.
struct ModelListWatch {
    HWND window; std::uint64_t mutations = 0, selections = 0; bool alive = true, attached = false;
    static LRESULT CALLBACK Observe(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) noexcept {
        auto& watch = *reinterpret_cast<ModelListWatch*>(data);
        switch (message) {
        case CB_RESETCONTENT: case CB_ADDSTRING: case CB_INSERTSTRING: case CB_DELETESTRING: case CB_SETITEMDATA: case CB_DIR:
            if (watch.mutations == UINT64_MAX) watch.alive = false; else ++watch.mutations;
            break;
        case CB_SETCURSEL:
            if (watch.selections == UINT64_MAX) watch.alive = false; else ++watch.selections;
            break;
        case WM_NCDESTROY:
            watch.alive = false; watch.attached = false; RemoveWindowSubclass(window, Observe, id); break;
        default: break;
        }
        return DefSubclassProc(window, message, w, l);
    }
    explicit ModelListWatch(HWND value) noexcept : window(value) {
        attached = window && SetWindowSubclass(window, Observe, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this));
    }
    bool Current() const noexcept {
        DWORD_PTR data = 0;
        return alive && attached && GetWindowSubclass(window, Observe, reinterpret_cast<UINT_PTR>(this), &data) && data == reinterpret_cast<DWORD_PTR>(this);
    }
    ~ModelListWatch() { if (Current()) RemoveWindowSubclass(window, Observe, reinterpret_cast<UINT_PTR>(this)); }
};
struct ModelInput {
    MMDApp& app; SharedHistory* const history;
    const ModelValues before;
    const HWND main, models, bones, capture;
    const int frame, language;
    const std::uint64_t binding, scene, pool;
    const Projection order;
    ModelListWatch list, boneList;
    std::uint64_t revision = 0;
    struct Item { unsigned char* raw = nullptr; mdl::BoneRecord* bones = nullptr; std::uint32_t count = 0;
        MmsObjectRef identity{}; unsigned rank = 0; int first = -1; };
    std::array<Item, kModelSlotCount> items{};
    std::array<float, 19> pivot{}; bool sampled = false;
    explicit ModelInput(MMDApp& value)
        : app(value), history(BoundNativePoseHistory(&value)), before(ReadModelValues(value)), main(value.MainWindow()),
          models(GetDlgItem(main, panel::kMainComboNormal)), bones(GetDlgItem(main, panel::kBoneRegisterCombo)),
          capture(GetCapture()), frame(value.CurrentFrame()), language(value.EnglishUI()),
          binding(BoundNativePoseGeneration(&value)), scene(value.SceneGeneration()),
          pool(mdl::KeyPoolIdentity(value.CameraKeys())), order(Order(value)), list(models), boneList(bones) {
        if (auto r = history->Revision(revision)) throw r;
        for (unsigned i = 0; i < items.size(); ++i) if (auto* raw = app.ModelSlot(i)) {
            auto& item = items[i]; item.raw = raw; item.bones = mdl::Mdl(raw)->boneTable;
            item.count = mdl::Mdl(raw)->boneCount; item.rank = mdl::Mdl(raw)->comboSelIndex;
            item.first = FirstBone(*mdl::Mdl(raw));
            if (!FindSceneObjectIdentity(raw, item.identity)) item.identity = SceneObjectIdentity(raw);
        }
        if (before.parent >= 0 && before.parent < kModelSlotCount) {
            const auto& item = items[before.parent];
            if (item.raw && item.bones && before.bone >= 0 && std::uint32_t(before.bone) < item.count) {
                const auto& bone = item.bones[before.bone]; sampled = true;
                std::memcpy(pivot.data(), bone.position, sizeof(float) * 3);
                std::memcpy(pivot.data() + 3, bone.matInit, sizeof(float) * 16);
            }
        }
    }
    bool SourcesCurrent() const noexcept {
        if (BoundNativePoseHistory(&app) != history || BoundNativePoseGeneration(&app) != binding ||
            app.SceneGeneration() != scene || mdl::KeyPoolIdentity(app.CameraKeys()) != pool ||
            app.MainWindow() != main || !IsWindow(main) || app.EnglishUI() != language || !models || !bones ||
            GetDlgItem(main, panel::kMainComboNormal) != models || GetDlgItem(main, panel::kBoneRegisterCombo) != bones ||
            !list.Current() || !boneList.Current()) return false;
        for (unsigned i = 0; i < items.size(); ++i) {
            const auto& item = items[i]; if (app.ModelSlot(i) != item.raw) return false;
            if (!item.raw) continue;
            MmsObjectRef current{};
            if (!FindSceneObjectIdentity(item.raw, current) || current.id.lo != item.identity.id.lo || current.id.hi != item.identity.id.hi ||
                current.generation != item.identity.generation || mdl::Mdl(item.raw)->boneTable != item.bones ||
                mdl::Mdl(item.raw)->boneCount != item.count || mdl::Mdl(item.raw)->comboSelIndex != item.rank ||
                FirstBone(*mdl::Mdl(item.raw)) != item.first) return false;
        }
        return true;
    }
    bool IdentityCurrent() const noexcept { return !list.mutations && SourcesCurrent(); }
    bool Current() const noexcept {
        std::uint64_t now = 0;
        if (!IdentityCurrent() || list.selections || boneList.mutations || boneList.selections ||
            Different(ReadModelValues(app), before) || app.CurrentFrame() != frame || GetCapture() != capture ||
            history->Revision(now) != MMS_OK || now != revision) return false;
        if (sampled) {
            const auto& bone = items[before.parent].bones[before.bone];
            if (std::memcmp(pivot.data(), bone.position, sizeof(float) * 3) ||
                std::memcmp(pivot.data() + 3, bone.matInit, sizeof(float) * 16)) return false;
        }
        return true;
    }
    LRESULT Send(UINT message, WPARAM w = 0, LPARAM l = 0) const {
        if (!Current()) throw MMS_E_STALE;
        const auto result = SendMessageW(models, message, w, l);
        if (!Current()) throw MMS_E_STALE;
        return result;
    }
};
int ModelRow(const Controls& controls, const Projection& order) {
    // Resolve uses noexcept item access; native calls must be revalidated by
    // the outer function, never throw through its noexcept projection helper.
    const auto count = controls.Send(controls.models, CB_GETCOUNT);
    std::array<LRESULT, kModelSlotCount + 1> items{};
    if (count < 1 || count > kModelSlotCount + 1) return -1;
    for (LRESULT row = 0; row < count; ++row) items[std::size_t(row)] = controls.Send(controls.models, CB_GETITEMDATA, row);
    return model_order::FindRow(order, unsigned(controls.source.slot), count,
        [&](std::size_t row) noexcept { return items[row]; });
}
struct Row { std::wstring text; int value; };
// This is a display operation, not edit admission. Its source snapshot is the
// same full model mapping used by 449, but its own native list writes are
// counted explicitly instead of weakening ModelInput::Current's edit rules.
struct ModelReadout {
    ModelInput source;
    const unsigned char camera;
    bool unique = true;
    std::uint64_t selectedMutation = 0, selectedVersion = 0;
    struct TextSource {
        int slot;
        char native[sizeof(mdl::ModelRecord::name)], english[sizeof(mdl::ModelRecord::nameEn)];
        wchar_t path[sizeof(mdl::ModelRecord::path) / sizeof(wchar_t)];
        std::array<const wchar_t*, 2> wide;
        std::array<std::wstring, 2> contents;
        TextSource(int value, const mdl::ModelRecord& model) : slot(value), wide{model.pmxTextBuffers[0], model.pmxTextBuffers[1]} {
            std::memcpy(native, model.name, sizeof native); std::memcpy(english, model.nameEn, sizeof english);
            std::memcpy(path, model.path, sizeof path);
            for (unsigned i = 0; i < wide.size(); ++i) if (wide[i]) contents[i] = wide[i];
        }
        bool Matches(const mdl::ModelRecord& model) const noexcept {
            if (std::memcmp(native, model.name, sizeof native) || std::memcmp(english, model.nameEn, sizeof english) ||
                std::memcmp(path, model.path, sizeof path)) return false;
            for (unsigned i = 0; i < wide.size(); ++i)
                if (model.pmxTextBuffers[i] != wide[i] || (wide[i] && contents[i] != wide[i])) return false;
            return true;
        }
    };
    // Empty on the allocation-free healthy path. A rebuild owns all labels
    // and their complete text sources before the first native list write.
    std::vector<TextSource> texts;
    explicit ModelReadout(MMDApp& app) : source(app), camera(app.CameraMode()) {
        for (std::size_t i = 0; i < source.items.size(); ++i) if (source.items[i].raw)
            for (std::size_t j = 0; j < i; ++j) if (source.items[j].raw == source.items[i].raw) unique = false;
    }
    bool Current() const noexcept {
        std::uint64_t revision = 0;
        if (!unique || !source.SourcesCurrent() || Different(ReadModelValues(source.app), source.before) ||
            source.app.CameraMode() != camera || source.app.CurrentFrame() != source.frame || GetCapture() != source.capture ||
            source.history->Revision(revision) != MMS_OK || revision != source.revision) return false;
        for (const auto& text : texts) if (!text.Matches(*mdl::Mdl(source.items[std::size_t(text.slot)].raw))) return false;
        return true;
    }
    bool Version(std::uint64_t mutation, std::uint64_t selection) const noexcept {
        return Current() && source.list.mutations == mutation && source.list.selections == selection;
    }
    LRESULT Send(HWND window, UINT message, WPARAM w = 0, LPARAM l = 0) const {
        if (!Current()) throw MMS_E_STALE;
        const auto result = SendMessageW(window, message, w, l);
        if (!Current()) throw MMS_E_STALE;
        return result;
    }
    LRESULT Write(UINT message, WPARAM w = 0, LPARAM l = 0) const {
        const auto mutation = source.list.mutations, selection = source.list.selections;
        const bool select = message == CB_SETCURSEL;
        if ((select ? selection : mutation) == UINT64_MAX) throw MMS_E_NOT_READY;
        const auto result = Send(source.models, message, w, l);
        // Our one message may change exactly one counter. An extra native
        // write, even one which restores the same data, belongs to a nested
        // continuation and cannot be accepted as part of this rebuild.
        if (!Version(mutation + (select ? 0 : 1), selection + (select ? 1 : 0))) throw MMS_E_NOT_READY;
        return result;
    }
    enum class Mapping { Valid, Invalid, Changed };
    Mapping Read(int& row, LRESULT& item, std::uint64_t& mutation, std::uint64_t& selection) const {
        mutation = source.list.mutations; selection = source.list.selections;
        const auto count = Send(source.models, CB_GETCOUNT);
        if (!Version(mutation, selection)) return Mapping::Changed;
        if (count != LRESULT(source.order.count + 1)) return Mapping::Invalid;
        std::array<LRESULT, kModelSlotCount + 1> items{};
        for (LRESULT i = 0; i < count; ++i) {
            items[std::size_t(i)] = Send(source.models, CB_GETITEMDATA, i);
            if (!Version(mutation, selection)) return Mapping::Changed;
        }
        if (source.order.count) {
            if (model_order::Resolve(source.order, 1, count, [&](std::size_t i) noexcept { return items[i]; }) < 0) return Mapping::Invalid;
        } else if (items[0] != -1 && items[0] != 0) return Mapping::Invalid;
        if (source.before.parent < 0) row = 0;
        else if (!HasParent(source.app)) row = -1;
        else {
            row = model_order::FindRow(source.order, unsigned(source.before.parent), count,
                [&](std::size_t i) noexcept { return items[i]; });
            if (row < 1) return Mapping::Invalid;
        }
        item = row >= 0 ? items[std::size_t(row)] : CB_ERR;
        return Mapping::Valid;
    }
    std::vector<Row> Rows() {
        std::vector<Row> rows; std::vector<TextSource> prepared;
        rows.reserve(source.order.count + 1); prepared.reserve(source.order.count);
        rows.push_back({UiUsesEnglishNames(source.language) ? L"non" : L"\x306a\x3057", -1});
        if (!Current()) throw MMS_E_STALE;
        for (std::size_t i = 1; i <= source.order.count; ++i) {
            const int slot = source.order.rows[i] - 1;
            const auto& model = *mdl::Mdl(source.items[std::size_t(slot)].raw);
            prepared.emplace_back(slot, model);
            rows.push_back({text_encoding::ModelLabel(model, UiUsesEnglishNames(source.language)), slot});
        }
        if (!Current()) throw MMS_E_STALE;
        texts.swap(prepared);
        if (!Current()) throw MMS_E_STALE;
        return rows;
    }
    bool Rebuild(const std::vector<Row>& rows) const {
        Write(CB_RESETCONTENT);
        const auto mutation = source.list.mutations, selection = source.list.selections;
        if (Send(source.models, CB_GETCOUNT) != 0 || !Version(mutation, selection)) return false;
        for (const auto& row : rows) {
            const auto index = Write(CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.text.c_str()));
            if (index < 0 || Write(CB_SETITEMDATA, index, row.value) == CB_ERR) return false;
        }
        return true;
    }
    MmsResult Synchronize() {
        bool rebuilt = false;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            int row = -1; LRESULT item = CB_ERR; std::uint64_t mutation = 0, selection = 0;
            const auto mapping = Read(row, item, mutation, selection);
            if (mapping == Mapping::Changed) continue;
            if (mapping == Mapping::Invalid) {
                if (rebuilt) return MMS_E_NOT_READY;
                const auto rows = Rows(); if (!Rebuild(rows)) return MMS_E_NOT_READY;
                rebuilt = true; continue;
            }
            const auto previous = Send(source.models, CB_GETCURSEL);
            if (!Version(mutation, selection)) continue;
            if (previous != row) {
                if (Write(CB_SETCURSEL, WPARAM(row)) != row) return MMS_E_NOT_READY;
                selection = source.list.selections;
            }
            if (Send(source.models, CB_GETCURSEL) != row || !Version(mutation, selection)) continue;
            if (row >= 0 && (Send(source.models, CB_GETITEMDATA, row) != item || !Version(mutation, selection))) continue;
            selectedMutation = mutation; selectedVersion = selection; return MMS_OK;
        }
        return MMS_E_NOT_READY;
    }
    bool SelectedCurrent() const noexcept { return Version(selectedMutation, selectedVersion); }
};
std::vector<Row> BoneRows(const Controls& controls) {
    std::vector<Row> rows;
    for (std::uint32_t i = 0; i < controls.source.count; ++i) if (Selectable(controls.source.bones[i]))
        rows.push_back({text_encoding::BoneLabel(controls.source.bones[i], UiUsesEnglishNames(controls.language)), int(i)});
    return rows;
}
bool VerifyBoneList(const Controls& controls, const std::vector<Row>& rows, const accessory_bone_combo::Cache& cache) {
    if (controls.Send(controls.bones, CB_GETCOUNT) != LRESULT(rows.size())) return false;
    std::vector<bool> used(rows.size());
    for (std::size_t row = 0; row < rows.size(); ++row) {
        const auto value = controls.Send(controls.bones, CB_GETITEMDATA, row);
        const auto found = std::lower_bound(rows.begin(), rows.end(), value, [](const Row& item,LRESULT id){ return item.value < id; });
        if (found == rows.end() || found->value != value) return false;
        const auto index = std::size_t(found - rows.begin()); if (used[index]) return false; used[index] = true;
        accessory_bone_combo::ReadGuard read(cache);
        const auto length = controls.Send(controls.bones, CB_GETLBTEXTLEN, row);
        if (length < 0 || std::size_t(length) != found->text.size()) return false;
        std::wstring text(std::size_t(length) + 1, L'\0');
        if (controls.Send(controls.bones, CB_GETLBTEXT, row, reinterpret_cast<LPARAM>(text.data())) != length ||
            !std::equal(found->text.begin(), found->text.end(), text.begin())) return false;
    }
    return true;
}
bool BoneList(const Controls& controls) {
    auto cache=accessory_bone_combo::Find(controls.bones,true);
    if(!controls.Current()||!cache||cache->busy)return false;
    const auto* model=mdl::Mdl(controls.source.raw);const bool english=UiUsesEnglishNames(controls.language);
    if(cache->valid&&cache->source.Matches(model,english))return controls.Current();
    accessory_bone_combo::Guard guard(cache);accessory_bone_combo::Source source;
    const bool captured=accessory_bone_combo::Source::Capture(model,english,source);
    const auto mutation=cache->mutations;const auto rows=BoneRows(controls);
    if(!VerifyBoneList(controls,rows,cache)||!cache->alive||cache->mutations!=mutation||!controls.Current())return false;
    if(captured){if(!source.Matches(model,english))return false;cache->Remember(std::move(source),mutation);}
    return true;
}
bool SelectByData(const Controls& controls, HWND combo, int value) {
    const auto cache=accessory_bone_combo::Find(combo);
    if(cache&&cache->valid){
        const auto selected=controls.Send(combo,CB_GETCURSEL);
        if(selected>=0&&controls.Send(combo,CB_GETITEMDATA,selected)==value)return true;
    }
    const auto count = controls.Send(combo, CB_GETCOUNT); LRESULT found = -1;
    for (LRESULT row = 0; row < count; ++row) if (controls.Send(combo, CB_GETITEMDATA, row) == value) {
        if (found >= 0) return false; found = row;
    }
    if (controls.Send(combo, CB_GETCURSEL) != found) {
        const auto result = controls.Send(combo, CB_SETCURSEL, found);
        if (found >= 0 && result != found) return false;
    }
    return controls.Send(combo, CB_GETCURSEL) == found;
}
bool Rebuild(const Controls& controls, HWND combo, const std::vector<Row>& rows) {
    controls.Send(combo, CB_RESETCONTENT);
    if (controls.Send(combo, CB_GETCOUNT) != 0) return false;
    for (const auto& row : rows) {
        const auto index = controls.Send(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.text.c_str()));
        if (index < 0 || controls.Send(combo, CB_SETITEMDATA, index, row.value) == CB_ERR) return false;
    }
    return controls.Send(combo, CB_GETCOUNT) == LRESULT(rows.size());
}
thread_local MMDApp* reading = nullptr;
LRESULT CALLBACK RepairProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) noexcept {
    if (message == WM_NCDESTROY) { KillTimer(window, id); RemoveWindowSubclass(window, RepairProc, id); }
    if (message == WM_TIMER && w == id) {
        KillTimer(window, id);
        auto* app = reinterpret_cast<MMDApp*>(data);
        if (app && BoundNativePoseHistory(app) && GetDlgItem(app->MainWindow(), panel::kBoneRegisterCombo) == window)
            RefreshBoundCameraParentBoneReadout(*app);
        return 0;
    }
    return DefSubclassProc(window, message, w, l);
}
void Retry(MMDApp& app) noexcept {
    const auto combo = GetDlgItem(app.MainWindow(), panel::kBoneRegisterCombo);
    const auto id = reinterpret_cast<UINT_PTR>(&RepairProc);
    if (combo && SetWindowSubclass(combo, RepairProc, id, reinterpret_cast<DWORD_PTR>(&app))) SetTimer(combo, id, 50, nullptr);
}
// No allocations or label generation: a budget/OOM refusal can still restore
// a healthy list's exact canonical row. Corrupt lists are left for repair.
void RestoreFast(MMDApp& app) noexcept {
    if (!HasParent(app)) return;
    const int slot = app.CameraParentModel(); const auto* raw = app.ModelSlot(slot);
    const auto* bones = mdl::Mdl(raw)->boneTable; const auto boneCount = mdl::Mdl(raw)->boneCount;
    const auto scene = app.SceneGeneration(), pool = mdl::KeyPoolIdentity(app.CameraKeys()), binding = BoundNativePoseGeneration(&app);
    MmsObjectRef identity{}; if(!FindSceneObjectIdentity(raw,identity))return;
    const auto main = app.MainWindow(), combo = GetDlgItem(main, panel::kBoneRegisterCombo);
    const int value = app.CameraParentBone(); LRESULT found = -1;
    const auto current = [&]() noexcept {
        MmsObjectRef live{};
        return app.SceneGeneration() == scene && BoundNativePoseGeneration(&app) == binding &&
            mdl::KeyPoolIdentity(app.CameraKeys()) == pool && app.CameraParentModel() == slot && app.ModelSlot(slot) == raw &&
            FindSceneObjectIdentity(raw,live)&&live.id.lo==identity.id.lo&&live.id.hi==identity.id.hi&&live.generation==identity.generation&&
            mdl::Mdl(raw)->boneTable == bones && mdl::Mdl(raw)->boneCount == boneCount && app.CameraParentBone() == value &&
            main == app.MainWindow() && GetDlgItem(main, panel::kBoneRegisterCombo) == combo;
    };
    const auto count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (LRESULT row = 0; row < count && current(); ++row) {
        const auto data = SendMessageW(combo, CB_GETITEMDATA, row, 0); if (!current()) return;
        if (data == value) { if (found >= 0) return; found = row; }
    }
    if (current()) SendMessageW(combo, CB_SETCURSEL, found, 0);
}
void Accepted(MMDApp& app, SharedHistory& history, bool changed, bool model = false) {
    bake::ScopedUiInputGate input;
    const auto main=app.MainWindow();const auto binding=BoundNativePoseGeneration(&app),scene=app.SceneGeneration();
    const auto* journal=&history;
    const bool undo=history.UndoCount()!=0,redo=history.RedoCount()!=0;
    const auto current=[&]() noexcept {return BoundNativePoseHistory(&app)==journal&&BoundNativePoseGeneration(&app)==binding&&app.SceneGeneration()==scene&&app.MainWindow()==main&&(!main||IsWindow(main));};
    // The old RefreshRequest(-1) only enters an unselected camera row. Keep
    // that selection effect explicit and never run a model-mode pose sweep.
    const bool selectionChanged=!app.GlobalTrackSelected(GlobalTimelineTrack::Camera);
    if (selectionChanged) {
        app.SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
        for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (auto* object = app.AccessorySlot(slot)) object->rowSelected = 0;
    }
    if (changed) {
        app.SceneModified() = 1; app.RecordedFrameCountByte1() = 1;
        EnableWindow(GetDlgItem(main, panel::kUndoButton),undo);if(!current())return;
        EnableWindow(GetDlgItem(main, panel::kRedoButton),redo);if(!current())return;
    }
    if (model) {
        // The camera branch only reads its scalar readouts; the model branch
        // can install scale transforms and update bone editing state.
        if (app.CameraMode()) { PostViewRefresh(&app); if (!current()) return; }
        RefreshBoundCameraFollowReadout(app);
    }
    else RefreshBoundCameraParentBoneReadout(app);
    if(!current())return;
    // WM_PAINT only blits this bitmap. Refresh its camera-track highlight
    // after acceptance, while drafts and the input gate remain protected.
    // The model-mode sweep can change selectedBone and is never used here.
    if(selectionChanged&&app.CameraMode()&&app.Renderer()&&app.PanelDC()) {
        PostLanguageSweep(&app);if(!current())return;
    }
    if(main)InvalidateRect(main, nullptr, FALSE);
    if(!current())return;
    updates::RequestViewport(&app, false);
}
MmsResult ApplyModelChange(const std::shared_ptr<ModelChange>& change, bool& changed, const ModelInput* input = nullptr) {
    auto& app = change->app;
    // Even a same-value request pins both owners before reading any EDIT.
    if (!change->InitialCurrent()) return MMS_E_STALE;
    ScopedFollowReadoutDrafts drafts(app);
    if (!change->InitialCurrent()) return MMS_E_STALE;
    if (input && !input->Current()) return MMS_E_STALE;
    if (auto r = change->history->CheckNativeNavigation()) return r;
    if (auto r = change->history->CheckNativeEdit()) return r;
    if (!Ready(app)) return MMS_E_BUSY;
    if (change->mask) {
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (auto r = change->history->PrepareNative("Camera parent model", change, transaction,
                                                  SharedHistory::NativeIntent::Adjustment)) return r;
        if (auto r = change->history->CommitNative(std::move(transaction))) return r;
        changed = true;
    }
    Boundary([&]() -> MmsResult { Accepted(app, *change->history, changed, true); return MMS_OK; });
    return MMS_OK;
}
void RestoreModelFast(MMDApp& app) noexcept {
    // No label allocation is needed to restore a healthy existing mapping
    // after budget/OOM refusal. A damaged list is left to the common repair.
    const auto main = app.MainWindow(), combo = GetDlgItem(main, panel::kMainComboNormal);
    const auto binding = BoundNativePoseGeneration(&app), scene = app.SceneGeneration();
    const int parent = app.CameraParentModel(); const auto order = Order(app);
    ModelListWatch watch(combo);
    const auto current = [&]() noexcept { return BoundNativePoseHistory(&app) &&
        BoundNativePoseGeneration(&app) == binding && app.SceneGeneration() == scene && app.CameraParentModel() == parent &&
        app.MainWindow() == main && GetDlgItem(main, panel::kMainComboNormal) == combo && watch.Current() &&
        Order(app).rows == order.rows && Order(app).count == order.count; };
    if (!combo || !current()) return;
    for (unsigned attempt = 0; attempt < 2 && current(); ++attempt) {
        const auto mutation = watch.mutations;
        const auto stable = [&]() noexcept { return current() && watch.mutations == mutation; };
        const auto restore = [&]() noexcept {
            const auto count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
            if (!stable() || count != LRESULT(order.count + 1)) return false;
            std::array<LRESULT, kModelSlotCount + 1> items{};
            for (LRESULT row = 0; row < count; ++row) {
                items[std::size_t(row)] = SendMessageW(combo, CB_GETITEMDATA, row, 0); if (!stable()) return false;
            }
            const int row = parent < 0 ? 0 : model_order::FindRow(order, unsigned(parent), count,
                [&](std::size_t index) noexcept { return items[index]; });
            if (row < 0 || !stable() || SendMessageW(combo, CB_SETCURSEL, row, 0) != row || !stable()) return false;
            if (SendMessageW(combo, CB_GETCURSEL, 0, 0) != row || !stable()) return false;
            const auto data = SendMessageW(combo, CB_GETITEMDATA, row, 0);
            return stable() && (parent < 0 ? (row == 0 && (data == -1 || data == 0)) :
                (items[0] == -1 ? data == parent : row == order.Row(unsigned(parent)) && data == items[std::size_t(row)]));
        };
        if (restore()) return;
    }
    if (current()) Retry(app);
}
}
MmsResult PlanCameraParentModel(MMDApp& app, std::int32_t target, std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset(); return Boundary([&]() -> MmsResult {
        std::shared_ptr<ModelChange> change; if (auto r = MakeModelChange(app, target, change)) return r;
        if (change->mask) out = std::move(change);
        return MMS_OK;
    });
}
MmsResult ApplyBoundCameraParentModel(MMDApp& app, std::int32_t target, bool& changed) noexcept {
    changed = false; return Boundary([&]() -> MmsResult {
        std::shared_ptr<ModelChange> change; if (auto r = MakeModelChange(app, target, change)) return r;
        return ApplyModelChange(change, changed);
    });
}
bool TrySelectBoundCameraParentModel(MMDApp* app, HWND combo) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    const auto result = Boundary([&]() -> MmsResult {
        ModelInput input(*app); if (!input.Current() || combo != input.models) return MMS_E_ARGUMENT;
        const auto row = input.Send(CB_GETCURSEL), count = input.Send(CB_GETCOUNT);
        if (row < 0 || count != LRESULT(input.order.count + 1) || row >= count) return MMS_E_STALE;
        std::array<LRESULT, kModelSlotCount + 1> items{};
        for (LRESULT i = 0; i < count; ++i) items[std::size_t(i)] = input.Send(CB_GETITEMDATA, i);
        if (input.order.count) {
            if (model_order::Resolve(input.order, 1, count, [&](std::size_t i) noexcept { return items[i]; }) < 0) return MMS_E_STALE;
        } else if (items[0] != -1 && items[0] != 0) return MMS_E_STALE;
        const int target = row == 0 ? -1 : model_order::Resolve(input.order, int(row), count,
            [&](std::size_t i) noexcept { return items[i]; });
        if (row > 0 && target < 0) return MMS_E_STALE;
        COMBOBOXINFO info{sizeof(info)};
        if (!GetComboBoxInfo(combo, &info) || !input.Current() || info.hwndCombo != combo) return MMS_E_STALE;
        CaptureAdmission admission(*app, combo, info.hwndList);
        std::shared_ptr<ModelChange> change; if (auto r = MakeModelChange(*app, target, change)) return r;
        if (!input.Current() || input.Send(CB_GETCURSEL) != row) return MMS_E_STALE;
        bool changed = false; if (auto r = ApplyModelChange(change, changed, &input)) return r;
        if (input.IdentityCurrent() && !Different(ReadModelValues(*app), change->after)) {
            bake::ScopedUiInputGate gate; SetFocus(input.main);
        }
        return MMS_OK;
    });
    if (result) {
        runtime_log::Write("controlled camera parent model refused: %d", int(result));
        RefreshBoundCameraParentBoneReadout(*app); RestoreModelFast(*app);
    }
    return true;
}
MmsResult PlanCameraParentBone(MMDApp& app, std::int32_t value, std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset(); return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app); if (!history) return MMS_E_NOT_READY;
        if (auto r = history->CheckNativeNavigation()) return r;
        if (auto r = history->CheckNativeEdit()) return r;
        if (!Ready(app)) return MMS_E_BUSY;
        if (!HasParent(app) || !mdl::KeyPoolIdentity(app.CameraKeys())) return MMS_E_NOT_READY;
        const auto* model = mdl::Mdl(app.ModelSlot(app.CameraParentModel()));
        if (value < 0 || std::uint32_t(value) >= model->boneCount || !Selectable(model->boneTable[value])) return MMS_E_ARGUMENT;
        auto change = std::make_shared<ParentChange>(app, value);
        if (auto r = history->Revision(change->initialRevision)) return r;
        if (!change->InitialCurrent()) return MMS_E_STALE;
        if (change->before == change->after) return MMS_OK;
        out = std::move(change); return MMS_OK;
    });
}
MmsResult ApplyBoundCameraParentBone(MMDApp& app, std::int32_t value, bool& changed) noexcept {
    changed = false; return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app); if (!history) return MMS_E_NOT_READY;
        if(!HasParent(app)||!mdl::KeyPoolIdentity(app.CameraKeys()))return MMS_E_NOT_READY;
        // Pin the original target before draft collection sends any native
        // reads. A callback may retire this source without changing binding.
        // No-op actions need the same guard before their selection effect.
        ParentSource original(app);
        const auto binding=BoundNativePoseGeneration(&app);
        const auto object=original.Generation();const auto frame=app.CurrentFrame(),before=app.CameraParentBone();
        const auto type=value>=0&&std::uint32_t(value)<original.count?original.bones[value].type:mdl::BoneType::InertTip;
        std::uint64_t revision=0;if(auto r=history->Revision(revision))return r;
        const auto current=[&]() noexcept {
            std::uint64_t now=0;
            return BoundNativePoseHistory(&app)==history&&BoundNativePoseGeneration(&app)==binding&&original.Current()&&
                original.Generation()==object&&app.CurrentFrame()==frame&&app.CameraParentBone()==before&&
                value>=0&&std::uint32_t(value)<original.count&&original.bones[value].type==type&&
                history->Revision(now)==MMS_OK&&now==revision;
        };
        std::shared_ptr<HistoryChange> change;if(auto r=PlanCameraParentBone(app,value,change))return r;
        if(!current())return MMS_E_STALE;
        ScopedFollowReadoutDrafts drafts(app);
        if(!current())return MMS_E_STALE;
        if (change) {
            std::unique_ptr<SharedHistory::NativeTransaction> transaction;
            if (auto r = history->PrepareNative("Camera parent bone", change, transaction, SharedHistory::NativeIntent::Adjustment)) return r;
            if (auto r = history->CommitNative(std::move(transaction))) return r;
            changed = true;
        }
        Boundary([&]() -> MmsResult { Accepted(app, *history, changed); return MMS_OK; });
        return MMS_OK;
    });
}
void RefreshBoundCameraParentBoneReadout(MMDApp& app) noexcept {
    if (!BoundNativePoseHistory(&app) || reading == &app) return;
    bake::ScopedUiInputGate input;
    struct Scope { MMDApp* previous; explicit Scope(MMDApp& a) : previous(reading) { reading = &a; } ~Scope() { reading = previous; } } scope(app);
    const auto result = Boundary([&]() -> MmsResult {
        if (!HasParent(app) && !GetDlgItem(app.MainWindow(), panel::kBoneRegisterCombo)) return MMS_OK;
        ModelReadout models(app);
        if (!models.Current()) return MMS_E_STALE;
        if (auto r = models.Synchronize()) return r;
        if (!HasParent(app)) {
            const auto combo = models.source.bones;
            const auto count = models.Send(combo, CB_GETCOUNT);
            if (count > 0) models.Send(combo, CB_RESETCONTENT);
            if (models.Send(combo, CB_GETCOUNT) != 0 || !models.SelectedCurrent()) return MMS_E_NOT_READY;
            KillTimer(combo,reinterpret_cast<UINT_PTR>(&RepairProc));return MMS_OK;
        }
        ParentSource source(app,false); Controls controls(source); if (!controls.Current()) return MMS_E_STALE;
        if (!BoneList(controls)) {
            if(!controls.Current() || !models.SelectedCurrent())return MMS_E_STALE;
            const auto rows=BoneRows(controls);
            if(!Rebuild(controls, controls.bones, rows)||!BoneList(controls))return MMS_E_NOT_READY;
        }
        if (!SelectByData(controls, controls.bones, controls.value) || !models.SelectedCurrent()) return MMS_E_NOT_READY;
        KillTimer(controls.bones, reinterpret_cast<UINT_PTR>(&RepairProc));
        return MMS_OK;
    });
    if (result) { RestoreFast(app); Retry(app); }
}
bool TrySelectBoundCameraParentBone(MMDApp* app, HWND combo) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    const auto result = Boundary([&]() -> MmsResult {
        if (!HasParent(*app)) return MMS_E_NOT_READY;
        ParentSource source(*app); Controls controls(source);
        const auto original = app->CameraParentBone(); const int frame = app->CurrentFrame();
        std::uint64_t revision = 0; if (auto r = source.history->Revision(revision)) return r;
        if (!controls.Current() || combo != controls.bones) return MMS_E_ARGUMENT;
        const auto order = Order(*app); const int parent = ModelRow(controls, order);
        if (parent < 1 || controls.Send(controls.models, CB_GETCURSEL) != parent) return MMS_E_STALE;
        if (!BoneList(controls)) return MMS_E_STALE;
        const auto row = controls.Send(combo, CB_GETCURSEL); if (row < 0) return MMS_E_ARGUMENT;
        const auto value = controls.Send(combo, CB_GETITEMDATA, row);
        if (value < 0 || value > INT32_MAX || std::uint64_t(value) >= source.count || !Selectable(source.bones[value])) return MMS_E_ARGUMENT;
        COMBOBOXINFO info{sizeof(info)};
        if (!GetComboBoxInfo(combo, &info) || !controls.Current() || info.hwndCombo != combo) return MMS_E_STALE;
        CaptureAdmission admission(*app, combo, info.hwndList);
        ScopedFollowReadoutDrafts drafts(*app);
        std::uint64_t currentRevision = 0;
        if (!controls.Current() || app->CameraParentBone() != original || app->CurrentFrame() != frame ||
            source.history->Revision(currentRevision) != MMS_OK || currentRevision != revision) return MMS_E_STALE;
        bool changed = false; if (auto r = ApplyBoundCameraParentBone(*app, std::int32_t(value), changed)) return r;
        if (controls.CurrentIdentity()) { bake::ScopedUiInputGate input; SetFocus(controls.main); }
        return MMS_OK;
    });
    if (result) { runtime_log::Write("controlled camera parent bone refused: %d", int(result)); RefreshBoundCameraParentBoneReadout(*app); }
    return true;
}
}
#endif
