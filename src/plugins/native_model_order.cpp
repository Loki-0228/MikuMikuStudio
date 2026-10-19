#include "native_model_order.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_object_commands.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <array>
#include <algorithm>
#include <exception>

namespace mikumikustudio::plugins {
namespace {
bool Ready(MMDApp& app, bool replay) noexcept {
    const bool authorized = replay && NativeObjectPublicationAuthorized(&app);
    return (!bake::UiLocked() || authorized) && (!app.SceneMutationInProgress() || authorized) &&
        !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        !SceneSaveInProgress() && !GetCapture() && !NativeObjectUiPending(&app) &&
        !app.FrameRangeDialog() && !app.state.frameCopyDialog && !app.RecordingWindow() &&
        !app.GravitySettingDialog() && !app.GroundShadowColorDialog() &&
        !app.state.accessoryEditDialogOpen && !app.state.edgeThicknessDialog;
}
}
struct NativeModelOrderPlan::State final : HistoryChange, std::enable_shared_from_this<State> {
    struct Value { std::uint8_t draw = 0, calculate = 0; };
    struct Entry {
        unsigned slot;
        unsigned char* object;
        std::shared_ptr<RetainedSceneObject> owner;
        MmsObjectRef initial;
        Value before, after;
    };
    MMDApp& app;
    SharedHistory& history;
    const ModelOrderKind kind;
    const std::uint64_t binding;
    std::vector<Entry> entries;
    std::uint64_t serial = 1, initialBookkeeping = 0;
    bool read = false, applied = false, published = false;
    State(MMDApp& value, SharedHistory& journal, ModelOrderKind order)
        : app(value), history(journal), kind(order), binding(BoundNativePoseGeneration(&value)) {}
    bool Members() const noexcept {
        std::size_t index = 0;
        for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (auto* object = app.ModelSlot(slot)) {
            if (index == entries.size()) return false;
            const auto& e = entries[index++];
            if (slot != e.slot || object != e.object || !e.owner->Present() ||
                !SceneObjectPresent(object, e.initial.id)) return false;
        }
        return index == entries.size();
    }
    bool Source(bool forward, bool initial) const noexcept {
        if (BoundNativePoseHistory(&app) != &history || !Members()) return false;
        if (initial && BoundNativePoseGeneration(&app) != binding) return false;
        for (const auto& e : entries) {
            const auto& expected = forward ? e.before : e.after;
            const auto& live = *mdl::Mdl(e.object);
            if ((initial || kind == ModelOrderKind::Draw) && live.comboSelIndex != expected.draw) return false;
            if ((initial || kind == ModelOrderKind::Calculate) && live.comboSelIndex2 != expected.calculate) return false;
            if (initial && SceneObjectIdentity(e.object).generation != e.initial.generation) return false;
        }
        return true;
    }
    std::uint64_t RecordBytes() const noexcept {
        return sizeof(State) + 4 * sizeof(void*) + entries.capacity() * sizeof(Entry);
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<State> state;
        const bool forward, initial;
        const std::uint64_t serial, binding;
        std::vector<std::uint64_t> generations;
        mutable resource_budget::Collector resources;
        std::uint64_t bytes = 0;
        bool consumed = false;
        Candidate(std::shared_ptr<State> value, bool direction)
            : state(std::move(value)), forward(direction), initial(!state->published), serial(state->serial),
              binding(BoundNativePoseGeneration(&state->app)) {
            generations.reserve(state->entries.size());
            for (const auto& e : state->entries) generations.push_back(SceneObjectIdentity(e.object).generation);
        }
        bool IsCurrent() const noexcept override {
            if (consumed || state->serial != serial || forward == state->applied ||
                !Ready(state->app, true) || state->app.AccessoryOrderArray() ||
                BoundNativePoseGeneration(&state->app) != binding || !state->Source(forward, initial)) return false;
            for (std::size_t i = 0; i < state->entries.size(); ++i) {
                const auto& e = state->entries[i];
                if (SceneObjectIdentity(e.object).generation != generations[i] || !e.owner->CoversBookkeeping(resources)) return false;
            }
            return true;
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            for (const auto& e : state->entries) {
                auto& live = *mdl::Mdl(e.object);
                const auto& target = forward ? e.after : e.before;
                if (state->kind == ModelOrderKind::Draw) live.comboSelIndex = target.draw;
                else live.comboSelIndex2 = target.calculate;
            }
            state->applied = forward; state->published = true; ++state->serial; consumed = true;
        }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); return Boundary([&]() -> MmsResult {
            if (!read || forward == applied || !Source(forward, !published)) return MMS_E_STALE;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            if (!Ready(app, true) || app.AccessoryOrderArray()) return MMS_E_BUSY;
            auto next = std::make_unique<Candidate>(shared_from_this(), forward);
            for (const auto& e : entries) if (auto result = e.owner->MeasureBookkeeping(next->resources)) return result;
            if (!next->resources.Result().Complete()) return MMS_E_NOT_READY;
            next->bytes = resource_budget::Collector::Sum(RecordBytes(), next->resources.Result().totalBytes);
            if (!next->IsCurrent()) return MMS_E_STALE;
            out = std::move(next); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        return initialBookkeeping > UINT64_MAX - RecordBytes() ? UINT64_MAX : RecordBytes() + initialBookkeeping;
    }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
MmsResult NativeModelOrderPlan::Begin(MMDApp& app, ModelOrderKind kind) noexcept {
    if (state_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app);
        if (!history) return MMS_E_NOT_READY;
        if (kind != ModelOrderKind::Draw && kind != ModelOrderKind::Calculate) return MMS_E_ARGUMENT;
        if (const auto result = history->CheckNativeEdit()) return result;
        if (!Ready(app, false) || app.AccessoryOrderArray()) return MMS_E_BUSY;
        auto state = std::make_shared<State>(app, *history, kind);
        std::array<bool, kModelSlotCount + 1> draw{}, calculate{};
        for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (auto* object = app.ModelSlot(slot)) {
            const auto& model = *mdl::Mdl(object);
            if (!model.comboSelIndex || !model.comboSelIndex2 || draw[model.comboSelIndex] || calculate[model.comboSelIndex2]) return MMS_E_STALE;
            draw[model.comboSelIndex] = calculate[model.comboSelIndex2] = true;
            State::Entry entry{}; entry.slot = slot; entry.object = object;
            entry.owner = RetainSceneObject(app, MMS_OBJECT_MODEL, slot); entry.initial = SceneObjectIdentity(object);
            entry.before = {model.comboSelIndex, model.comboSelIndex2}; entry.after = entry.before;
            state->entries.push_back(std::move(entry));
        }
        for (std::size_t i = 1; i <= state->entries.size(); ++i) if (!draw[i] || !calculate[i]) return MMS_E_STALE;
        resource_budget::Collector resources;
        for (const auto& e : state->entries) if (auto result = e.owner->MeasureBookkeeping(resources)) return result;
        if (!resources.Result().Complete()) return MMS_E_NOT_READY;
        state->initialBookkeeping = resources.Result().totalBytes;
        if (!state->Source(true, true)) return MMS_E_STALE;
        state_ = std::move(state); return MMS_OK;
    });
}
MmsResult NativeModelOrderPlan::Read(const std::int32_t* order, std::size_t count) noexcept {
    if (!state_) return MMS_E_NOT_READY;
    state_->read = false;
    if (!order || count != state_->entries.size()) return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        // The binding may have been removed together with its old journal
        // while the editor was open. Check identity before dereferencing it.
        if (!state_->Source(true, true)) return MMS_E_STALE;
        if (const auto result = state_->history.CheckNativeEdit()) return result;
        if (!Ready(state_->app, false)) return MMS_E_BUSY;
        std::array<bool, kModelSlotCount> used{};
        for (std::size_t row = 1; row <= count; ++row) {
            const auto stored = order[row];
            if (stored <= 0 || stored > kModelSlotCount) return MMS_E_ARGUMENT;
            const auto slot = static_cast<unsigned>(stored - 1);
            if (used[slot]) return MMS_E_ARGUMENT;
            used[slot] = true;
            const auto entry = std::find_if(state_->entries.begin(), state_->entries.end(),
                [&](const auto& value) { return value.slot == slot; });
            if (entry == state_->entries.end()) return MMS_E_ARGUMENT;
            entry->after = entry->before;
            (state_->kind == ModelOrderKind::Draw ? entry->after.draw : entry->after.calculate) = static_cast<std::uint8_t>(row);
        }
        if (!state_->Source(true, true)) return MMS_E_STALE;
        state_->read = true; return MMS_OK;
    });
}
MmsResult NativeModelOrderPlan::Take(std::shared_ptr<HistoryChange>& out) noexcept {
    if (out) return MMS_E_ARGUMENT;
    if (!state_ || !state_->read) return MMS_E_NOT_READY;
    if (!state_->Source(true, true)) return MMS_E_STALE;
    const bool changed = std::any_of(state_->entries.begin(), state_->entries.end(), [](const auto& e) {
        return e.before.draw != e.after.draw || e.before.calculate != e.after.calculate;
    });
    if (changed) out = std::move(state_); else state_.reset();
    return MMS_OK;
}
}
#endif
