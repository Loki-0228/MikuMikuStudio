#include "native_model_multiply.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_object_commands.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace mikumikustudio::plugins {
namespace {
std::uint64_t Add(std::uint64_t a, std::uint64_t b) noexcept { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
void Count(std::uint64_t& total, std::uint64_t count, std::uint64_t stride) noexcept {
    total = stride && count > (UINT64_MAX - total) / stride ? UINT64_MAX : total + count * stride;
}
bool Ready(MMDApp& app, bool replay) noexcept {
    const bool authorized = replay && NativeObjectPublicationAuthorized(&app);
    return (!bake::UiLocked() || authorized) && (!app.SceneMutationInProgress() || authorized) &&
        !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        !SceneSaveBlocksEdits() && !NativeObjectUiPending(&app) && !GetCapture() && !app.RecordingWindow();
}
struct BoneInput {
    float position[3], rotation[4], rest[3], axis[3];
    mdl::BoneType type;
    std::uint16_t flags;
    unsigned char dirty, rigid, disabled;
    int parent, tail, selector;
};
BoneInput Input(const mdl::ModelRecord& record, unsigned bone) noexcept {
    BoneInput result{};
    std::memcpy(result.position, record.boneTable[bone].trans, sizeof(result.position));
    std::memcpy(result.rotation, record.boneTable[bone].rotQuat, sizeof(result.rotation));
    const auto& source = record.boneTable[bone];
    std::memcpy(result.rest, source.position, sizeof(result.rest)); std::memcpy(result.axis, source.axis, sizeof(result.axis));
    result.type = source.type; result.flags = source.flags; result.rigid = source.hasRigidBody;
    result.disabled = source.physicsDisabled; result.parent = source.parent; result.tail = source.tailBone; result.selector = source.slotIndex;
    result.dirty = record.bonePhysicsState[bone]; return result;
}
bool Same(const BoneInput& a, const BoneInput& b) noexcept {
    return a.dirty == b.dirty && a.type == b.type && a.flags == b.flags && a.rigid == b.rigid && a.disabled == b.disabled &&
        a.parent == b.parent && a.tail == b.tail && a.selector == b.selector &&
        !std::memcmp(a.position, b.position, sizeof(a.position)) && !std::memcmp(a.rotation, b.rotation, sizeof(a.rotation)) &&
        !std::memcmp(a.rest, b.rest, sizeof(a.rest)) && !std::memcmp(a.axis, b.axis, sizeof(a.axis));
}
struct MorphInput { std::array<char, 20> name; mdl::MorphPanel panel; unsigned char type; float value; };
bool ReadNumber(HWND dialog, unsigned id, float& value) noexcept {
    const auto control = GetDlgItem(dialog, int(id));
    if (!control) return false;
    char text[128]{};
    if (GetWindowTextLengthA(control) >= int(sizeof text)) return false;
    GetWindowTextA(control, text, int(sizeof text));
    char* end = nullptr; errno = 0;
    const auto parsed = std::strtod(text, &end);
    if (end == text || errno == ERANGE) return false;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    value = float(parsed); return !*end && std::isfinite(value);
}
}
struct NativeModelMultiplyPlan::State final : HistoryChange, std::enable_shared_from_this<State> {
    MMDApp& app;
    SharedHistory* history;
    ModelMultiplyKind kind;
    unsigned slot;
    unsigned char* model;
    std::shared_ptr<RetainedSceneObject> lifetime;
    std::uint64_t binding, revision = 0, poolIdentity, serial = 1;
    int frame;
    mdl::BoneRecord* bones;
    mdl::MorphRecord* morphs;
    unsigned boneCount, morphCount;
    unsigned char physicsMode;
    unsigned char* dirty;
    std::vector<mdl::BoneKey> boneKeys;
    std::vector<mdl::MorphKey> morphKeys;
    std::vector<BoneInput> bonePose;
    std::vector<MorphInput> morphPose;
    bone_scale::Values scalePose;
    bone_scale::AuthoringState scale;
    std::unique_ptr<bone_curve::AuthoringState> curves;
    morph_curve::AuthoredLayers morphLayers;
    std::shared_ptr<HistoryChange> inner;
    bool read = false, published = false, applied = false;
    State(MMDApp& a, SharedHistory& h, ModelMultiplyKind k)
        : app(a), history(&h), kind(k), slot(a.SelectedModelSlot()), model(a.ModelSlot(slot)),
          lifetime(RetainSceneObject(a, MMS_OBJECT_MODEL, slot)), binding(BoundNativePoseGeneration(&a)),
          poolIdentity(k == ModelMultiplyKind::Bone ? mdl::BoneKeyPoolIdentity(mdl::Mdl(model)->boneKeys) :
              mdl::MorphKeyPoolIdentity(mdl::Mdl(model)->morphKeys)), frame(a.CurrentFrame()),
          bones(mdl::Mdl(model)->boneTable), morphs(mdl::Mdl(model)->morphs),
          boneCount(mdl::Mdl(model)->boneCount), morphCount(mdl::Mdl(model)->morphCount),
          physicsMode(mdl::Mdl(model)->physicsMode), dirty(mdl::Mdl(model)->bonePhysicsState) {
        const auto& record = *mdl::Mdl(model);
        if (!poolIdentity || !lifetime) throw std::invalid_argument("model multiply owner");
        if (kind == ModelMultiplyKind::Bone) {
            const auto capacity = mdl::BoneKeyCapacity(record.boneKeys);
            if (!bones || !dirty || !boneCount || boneCount > capacity) throw std::invalid_argument("model multiply bone source");
            boneKeys.assign(record.boneKeys, record.boneKeys + capacity);
            bonePose.reserve(boneCount);
            for (unsigned bone = 0; bone < boneCount; ++bone) bonePose.push_back(Input(record, bone));
            scalePose = bone_scale::PoseValues(model);
            scale = bone_scale::AuthoringState::Capture(model); curves = bone_curve::AuthoringState::Capture(model);
        } else {
            const auto capacity = mdl::MorphKeyCapacity(record.morphKeys);
            if (!morphs || !morphCount || morphCount > capacity) throw std::invalid_argument("model multiply morph source");
            morphKeys.assign(record.morphKeys, record.morphKeys + capacity);
            morphPose.reserve(morphCount);
            for (unsigned morph = 0; morph < morphCount; ++morph) {
                const auto& source = morphs[morph]; MorphInput input{{}, source.panel, source.type, source.value};
                std::memcpy(input.name.data(), source.name, input.name.size()); morphPose.push_back(input);
            }
            std::string error;
            if (!morph_curve::CaptureLayers(model, morphLayers, error)) throw std::invalid_argument("model multiply morph authoring");
        }
    }
    bool Structure(bool initial) const noexcept {
        if (BoundNativePoseHistory(&app) != history || (initial && BoundNativePoseGeneration(&app) != binding) ||
            app.ModelSlot(slot) != model || !lifetime->Present() || !SceneObjectPresent(model, lifetime->Id())) return false;
        const auto& record = *mdl::Mdl(model);
        if (initial && record.physicsMode != physicsMode) return false;
        return kind == ModelMultiplyKind::Bone ? record.boneTable == bones && record.boneCount == boneCount &&
            record.bonePhysicsState == dirty && mdl::BoneKeyPoolIdentity(record.boneKeys) == poolIdentity &&
            mdl::BoneKeyCapacity(record.boneKeys) >= boneKeys.size() :
            record.morphs == morphs && record.morphCount == morphCount &&
            mdl::MorphKeyPoolIdentity(record.morphKeys) == poolIdentity && mdl::MorphKeyCapacity(record.morphKeys) >= morphKeys.size();
    }
    bool InitialCurrent(bool authors) const {
        if (!Structure(true) || app.SelectedModelSlot() != slot || app.CameraMode() || app.CurrentFrame() != frame) return false;
        std::uint64_t now = 0;
        if (history->Revision(now) != MMS_OK || now != revision) return false;
        const auto& record = *mdl::Mdl(model);
        if (kind == ModelMultiplyKind::Bone) {
            if (mdl::BoneKeyCapacity(record.boneKeys) != boneKeys.size() ||
                std::memcmp(record.boneKeys, boneKeys.data(), boneKeys.size() * sizeof(mdl::BoneKey)) ||
                !bone_scale::PoseValuesEqual(model, scalePose)) return false;
            for (unsigned bone = 0; bone < boneCount; ++bone) if (!Same(bonePose[bone], Input(record, bone))) return false;
            if (authors && (!bone_scale::AuthoringState::Capture(model).Equals(scale) ||
                !bone_curve::AuthoringState::Capture(model)->Equals(*curves))) return false;
        } else {
            if (mdl::MorphKeyCapacity(record.morphKeys) != morphKeys.size() ||
                std::memcmp(record.morphKeys, morphKeys.data(), morphKeys.size() * sizeof(mdl::MorphKey))) return false;
            for (unsigned morph = 0; morph < morphCount; ++morph) {
                const auto& source = morphs[morph]; const auto& before = morphPose[morph];
                if (source.panel != before.panel || source.type != before.type ||
                    std::memcmp(source.name, before.name.data(), before.name.size()) ||
                    std::memcmp(&source.value, &before.value, sizeof(float))) return false;
            }
            if (authors && !morph_curve::LayersUnchanged(model, morphLayers)) return false;
        }
        return true;
    }
    std::uint64_t OwnBytes() const noexcept {
        std::uint64_t bytes = sizeof(*this) + 4 * sizeof(void*);
        Count(bytes, boneKeys.capacity(), sizeof(mdl::BoneKey)); Count(bytes, morphKeys.capacity(), sizeof(mdl::MorphKey));
        Count(bytes, bonePose.capacity(), sizeof(BoneInput)); Count(bytes, morphPose.capacity(), sizeof(MorphInput));
        Count(bytes, scalePose.size() + 1, sizeof(bone_scale::Values::value_type) + 4 * sizeof(void*));
        bytes = Add(bytes, scale.RetainedBytes()); if (curves) bytes = Add(bytes, curves->RetainedBytes());
        return Add(bytes, morphLayers.RetainedBytes());
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<State> source;
        std::unique_ptr<PreparedHistoryChange> inner;
        mutable resource_budget::Collector resources;
        std::uint64_t retainedBytes = 0;
        std::uint64_t serial, binding;
        bool forward, first, consumed = false;
        Candidate(std::shared_ptr<State> s, std::unique_ptr<PreparedHistoryChange> value, bool f)
            : source(std::move(s)), inner(std::move(value)), serial(source->serial),
              binding(BoundNativePoseGeneration(&source->app)), forward(f), first(!source->published) {}
        bool IsCurrent() const noexcept override {
            return !consumed && source->serial == serial && forward != source->applied && source->Structure(first) &&
                BoundNativePoseGeneration(&source->app) == binding && Ready(source->app, true) &&
                (!first || source->InitialCurrent(false)) && source->lifetime->CoversBookkeeping(resources) && inner->IsCurrent();
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            inner->Publish(); source->published = true; source->applied = forward; ++source->serial; consumed = true;
        }
        bool TargetRetainedBytes(std::uint64_t& bytes) const noexcept override {
            bytes = retainedBytes; return true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!inner || forward == applied || !Structure(!published) || (!published && !InitialCurrent(true))) return MMS_E_STALE;
            if (!Ready(app, true)) return MMS_E_BUSY;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            std::unique_ptr<PreparedHistoryChange> prepared;
            if (auto result = inner->Prepare(forward, prepared)) return result;
            if (!prepared) return MMS_E_FAILED;
            auto next = std::make_unique<Candidate>(shared_from_this(), std::move(prepared), forward);
            if (auto result = lifetime->MeasureBookkeeping(next->resources)) return result;
            if (!next->resources.Result().Complete()) return MMS_E_NOT_READY;
            std::uint64_t innerBytes = 0;
            if (!next->inner->TargetRetainedBytes(innerBytes)) innerBytes = inner->RetainedBytes();
            next->retainedBytes = resource_budget::Collector::Sum(Add(innerBytes, OwnBytes()), next->resources.Result().totalBytes);
            if (!next->IsCurrent()) return MMS_E_STALE;
            out = std::move(next); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return Add(inner ? inner->RetainedBytes() : 0, OwnBytes()); }
    // The private candidate already samples clean values in this model. The
    // legacy multiply commands do not reset unrelated models' physics worlds.
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
MmsResult NativeModelMultiplyPlan::Begin(MMDApp& app, ModelMultiplyKind kind) noexcept {
    if (state_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app);
        if (!history) return MMS_E_NOT_READY;
        if (auto result = history->CheckNativeNavigation()) return result;
        if (auto result = history->CheckNativeEdit()) return result;
        if (!Ready(app, false) || app.CameraMode() || app.SelectedModelSlot() >= kModelSlotCount ||
            !app.SelectedModel()) return MMS_E_NOT_READY;
        if (app.CurrentFrame() < 0 || app.CurrentFrame() > frame_limits::maximum ||
            (kind != ModelMultiplyKind::Bone && kind != ModelMultiplyKind::Morph)) return MMS_E_ARGUMENT;
        auto next = std::make_shared<State>(app, *history, kind);
        if (auto result = history->Revision(next->revision)) return result;
        if (!next->InitialCurrent(true)) return MMS_E_STALE;
        state_ = std::move(next); return MMS_OK;
    });
}
MmsResult NativeModelMultiplyPlan::Read(const BoneMultiplyOptions& options) noexcept {
    if (!state_ || state_->kind != ModelMultiplyKind::Bone) return MMS_E_NOT_READY;
    state_->read = false; state_->inner.reset();
    return Boundary([&]() -> MmsResult {
        if (!state_->InitialCurrent(true)) return MMS_E_STALE;
        if (auto result = state_->history->CheckNativeNavigation()) return result;
        if (auto result = state_->history->CheckNativeEdit()) return result;
        if (!Ready(state_->app, false)) return MMS_E_BUSY;
        std::shared_ptr<HistoryChange> change;
        if (auto result = PlanBoneKeyMultiply(state_->app, state_->slot, options, change)) return result;
        if (!state_->InitialCurrent(true)) return MMS_E_STALE;
        state_->inner = std::move(change); state_->read = true; return MMS_OK;
    });
}
MmsResult NativeModelMultiplyPlan::Read(const MorphMultiplyOptions& options) noexcept {
    if (!state_ || state_->kind != ModelMultiplyKind::Morph) return MMS_E_NOT_READY;
    state_->read = false; state_->inner.reset();
    return Boundary([&]() -> MmsResult {
        if (!state_->InitialCurrent(true)) return MMS_E_STALE;
        if (auto result = state_->history->CheckNativeNavigation()) return result;
        if (auto result = state_->history->CheckNativeEdit()) return result;
        if (!Ready(state_->app, false)) return MMS_E_BUSY;
        std::shared_ptr<HistoryChange> change;
        if (auto result = PlanMorphKeyMultiply(state_->app, state_->slot, options, change)) return result;
        if (!state_->InitialCurrent(true)) return MMS_E_STALE;
        state_->inner = std::move(change); state_->read = true; return MMS_OK;
    });
}
MmsResult NativeModelMultiplyPlan::ReadDialog(HWND dialog) noexcept {
    if (!state_) return MMS_E_NOT_READY;
    state_->read = false; state_->inner.reset();
    if (state_->kind == ModelMultiplyKind::Bone) {
        BoneMultiplyOptions options;
        for (unsigned lane = 0; lane < 9; ++lane) for (unsigned pair = 0; pair < 2; ++pair) {
            const auto id = unsigned(panel::kBoneMulPosXScaleEdit) + 2 * lane + pair;
            if (!ReadNumber(dialog, id, pair ? options.offset[lane] : options.scale[lane])) return MMS_E_ARGUMENT;
        }
        return Read(options);
    }
    MorphMultiplyOptions options;
    for (unsigned pair = 0; pair < 2; ++pair) {
        const auto control = GetDlgItem(dialog, panel::kMorphMulScaleEdit + int(pair));
        if (!control) return MMS_E_ARGUMENT;
        // Preserve the ordinary morph dialog's bounded atof conversion; the
        // planner still rejects nonfinite inputs and overflow atomically.
        char text[20]{}; GetWindowTextA(control, text, int(sizeof text));
        (pair ? options.offset : options.scale) = float(std::atof(text));
    }
    return Read(options);
}
MmsResult NativeModelMultiplyPlan::Take(std::shared_ptr<HistoryChange>& out) noexcept {
    if (out) return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        if (!state_ || !state_->read) return MMS_E_NOT_READY;
        if (!state_->InitialCurrent(true)) return MMS_E_STALE;
        if (auto result = state_->history->CheckNativeNavigation()) return result;
        if (auto result = state_->history->CheckNativeEdit()) return result;
        if (!Ready(state_->app, false)) return MMS_E_BUSY;
        if (state_->inner) out = std::move(state_); else state_.reset();
        return MMS_OK;
    });
}
}
#endif
