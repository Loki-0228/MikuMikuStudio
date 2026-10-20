#include "native_morph_category.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_history_commands.hpp"
#include "native_object_commands.hpp"
#include "native_pose_scope.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/ui_language.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace mikumikustudio::plugins {
namespace {
using Key = mdl::MorphKey;
using Pool = std::vector<Key>;
thread_local bool confirmationActive = false;
int ConfirmPurge(MMDApp& app, unsigned panel) {
    struct Message { const wchar_t* title; const wchar_t* body; };
    // Bound commits are reversible. Keep the original non-reversible message
    // exclusively on the unbound legacy route.
    static constexpr Message messages[3][3] = {
        {{L"眉フレーム削除", L"すべての眉フレームを削除し、現在のウェイトを 0 にしますか？\nこの操作は元に戻せます。"},
         {L"Delete eyebrow frames", L"Delete all eyebrow frames and reset the current weights to 0?\nYou can undo this operation."},
         {L"删除眉毛帧", L"确定删除全部眉毛帧，并将当前权重清零吗？\n此操作可以撤销。"}},
        {{L"目フレーム削除", L"すべての目のフレームを削除し、現在のウェイトを 0 にしますか？\nこの操作は元に戻せます。"},
         {L"Delete eye frames", L"Delete all eye frames and reset the current weights to 0?\nYou can undo this operation."},
         {L"删除眼部帧", L"确定删除全部眼部帧，并将当前权重清零吗？\n此操作可以撤销。"}},
        {{L"リップフレーム削除", L"すべてのリップフレームを削除し、現在のウェイトを 0 にしますか？\nこの操作は元に戻せます。"},
         {L"Delete lip frames", L"Delete all lip frames and reset the current weights to 0?\nYou can undo this operation."},
         {L"删除口型帧", L"确定删除全部口型帧，并将当前权重清零吗？\n此操作可以撤销。"}}
    };
    const auto language = app.EnglishUI() <= kUiChinese ? app.EnglishUI() : kUiEnglish;
    const auto& message = messages[panel - 1][language];
    return MessageBoxW(app.MainWindow(), message.body, message.title, MB_OKCANCEL | MB_TOPMOST);
}
void Add(std::uint64_t& bytes, std::uint64_t count, std::uint64_t stride = 1) noexcept {
    bytes = stride && count > (UINT64_MAX - bytes) / stride ? UINT64_MAX : bytes + count * stride;
}
std::uint32_t WeightBits(const mdl::MorphRecord& morph) noexcept {
    std::uint32_t bits; std::memcpy(&bits, &morph.value, sizeof bits); return bits;
}
bool Same(Key first, Key second, bool marks = true) noexcept {
    if (!marks) first.allocated = second.allocated = 0;
    return std::memcmp(&first, &second, sizeof first) == 0;
}
bool Ready(MMDApp& app, bool replay = false) noexcept {
    const bool ownReplay = replay && NativeObjectPublicationAuthorized(&app);
    return !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        (!app.SceneMutationInProgress() || ownReplay) && (!bake::UiLocked() || ownReplay) &&
        !SceneSaveBlocksEdits() && !GetCapture() && !app.FrameRangeDialog() && !app.state.frameCopyDialog &&
        !app.RecordingWindow() && !app.GravitySettingDialog() && !app.GroundShadowColorDialog() &&
        !app.AccessoryOrderArray() && !app.state.accessoryEditDialogOpen && !NativeModelOutlineDialogBlocks(app);
}
std::vector<std::vector<unsigned>> Chains(const Pool& keys, unsigned count) {
    if (!count || count > keys.size()) throw std::invalid_argument("missing morph category tracks");
    std::vector<std::vector<unsigned>> result(count);
    std::vector<unsigned char> owned(keys.size());
    for (unsigned morph = 0; morph < count; ++morph) {
        unsigned index = morph, previous = 0, frame = 0;
        for (;;) {
            if (index >= keys.size() || owned[index] || (index < count && index != morph))
                throw std::invalid_argument("shared morph category key");
            const auto& key = keys[index];
            if (key.next >= keys.size() || key.previous >= keys.size() || !std::isfinite(key.value) ||
                key.frame > unsigned(frame_limits::maximum) ||
                (index == morph ? key.frame != 0 : key.previous != previous || key.frame <= frame))
                throw std::invalid_argument("invalid morph category chain");
            owned[index] = 1; result[morph].push_back(index);
            if (!key.next) break;
            previous = index; frame = key.frame; index = key.next;
        }
    }
    for (std::size_t index = count; index < keys.size(); ++index)
        if (!owned[index] && keys[index].frame) throw std::invalid_argument("orphan morph category key");
    return result;
}
morph_curve::NativeKeyView View(const Pool& pool) noexcept {
    return {&pool, [](const void* context, std::uint32_t index) noexcept -> const Key* {
        const auto& value = *static_cast<const Pool*>(context);
        return index < value.size() ? &value[index] : nullptr;
    }, pool.size()};
}
// The affected buffers must belong to this model. In particular, undo may not
// overwrite another model through a malformed shared pool or cursor table.
bool Exclusive(MMDApp& app, unsigned slot, const Key* pool, std::size_t capacity) noexcept {
    struct Range { const void* data; std::size_t count, stride; };
    const auto separate = [](Range first, Range second) {
        if (!first.data || !first.count || !second.data || !second.count) return true;
        const auto a = reinterpret_cast<std::uintptr_t>(first.data), b = reinterpret_cast<std::uintptr_t>(second.data);
        return first.stride && second.stride && first.count <= (UINTPTR_MAX - a) / first.stride &&
            second.count <= (UINTPTR_MAX - b) / second.stride &&
            !(a < b + second.count * second.stride && b < a + first.count * first.stride);
    };
    if (!pool) return false;
    const auto& own = *mdl::Mdl(app.ModelSlot(slot));
    const Range native{pool, capacity, sizeof(Key)}, appState{&app.state, 1, sizeof app.state};
    const std::array<Range, 3> writes = {{{own.morphs, own.morphCount, sizeof(mdl::MorphRecord)},
        {own.morphKeyCursors, own.morphCount, sizeof(std::uint32_t)}, {own.morphTrackActive, own.morphCount, 1}}};
    if (!separate(native, appState)) return false;
    for (const auto& write : writes) if (!separate(write, appState)) return false;
    for (unsigned index = 0; index < kModelSlotCount; ++index) if (app.ModelSlot(index)) {
        const auto& record = *mdl::Mdl(app.ModelSlot(index));
        const std::array<Range, 16> protectedRanges = {{{&record, 1, sizeof record},
            {record.morphs, record.morphCount, sizeof(mdl::MorphRecord)},
            {record.boneTable, record.boneCount, sizeof(mdl::BoneRecord)},
            {record.boneKeys, mdl::BoneKeyCapacity(record.boneKeys), sizeof(mdl::BoneKey)},
            {record.displayKeys, mdl::KeyPoolCapacity(record.displayKeys), sizeof(mdl::DisplayKey)},
            {record.morphKeyCursors, record.morphCount, sizeof(std::uint32_t)}, {record.morphTrackActive, record.morphCount, 1},
            {record.boneKeyCursors, record.boneCount, sizeof(std::uint32_t)}, {record.boneTrackActive, record.boneCount, 1},
            {record.morphKeys, mdl::MorphKeyCapacity(record.morphKeys), sizeof(Key)},
            {record.boneSelection, record.boneCount, 1}, {record.bonePhysicsState, record.boneCount, 1},
            {record.boneKeyIndices, record.boneCount, sizeof(std::uint32_t)},
            {record.morphKeyIndices, record.morphCount, sizeof(std::uint32_t)},
            {record.ikChains, record.ikChainCount, sizeof(mdl::IkChain)},
            {record.boneOrderTable, record.boneOrderCount, sizeof(mdl::BoneOrderEntry)}}};
        for (unsigned range = 0; range < protectedRanges.size(); ++range) {
            if (!(index == slot && range == 9) && !separate(native, protectedRanges[range])) return false;
            for (unsigned write = 0; write < writes.size(); ++write)
                if (!(index == slot && range == (write == 0 ? 1 : write == 1 ? 5 : 6)) &&
                    !separate(writes[write], protectedRanges[range])) return false;
        }
        for (unsigned chain = 0; record.ikChains && chain < record.ikChainCount; ++chain) {
            const Range links{record.ikChains[chain].links, record.ikChains[chain].linkCount, sizeof(std::uint16_t)};
            if (!separate(native, links)) return false;
            for (const auto& write : writes) if (!separate(write, links)) return false;
        }
        for (std::size_t key = 0; record.displayKeys && key < mdl::KeyPoolCapacity(record.displayKeys); ++key) {
            const std::array<Range, 2> nested = {{{record.displayKeys[key].ikStates, record.ikChainCount, 1},
                {record.displayKeys[key].selectorStates, record.boneOrderCount, sizeof(mdl::BoneReference)}}};
            for (const auto& item : nested) {
                if (!separate(native, item)) return false;
                for (const auto& write : writes) if (!separate(write, item)) return false;
            }
        }
    }
    return true;
}
struct Rule {
    std::array<char, 20> name{};
    mdl::MorphPanel panel;
    std::uint8_t type;
    std::uint32_t weight, cursor = 0;
    unsigned char active = 0;
    Rule(const mdl::MorphRecord& morph) : panel(morph.panel), type(morph.type), weight(WeightBits(morph)) {
        std::memcpy(name.data(), morph.name, name.size());
    }
    bool Identity(const mdl::MorphRecord& morph) const noexcept {
        return morph.panel == panel && morph.type == type && !std::memcmp(name.data(), morph.name, name.size());
    }
};
std::vector<unsigned> CategoryMorphs(const mdl::ModelRecord& record, unsigned panel) {
    std::vector<unsigned> tracks;
    for (unsigned index = 0; index < record.morphCount; ++index) if (unsigned(record.morphs[index].panel) == panel) {
        tracks.push_back(index);
    }
    return tracks;
}
struct Purge final : HistoryChange, std::enable_shared_from_this<Purge> {
    MMDApp& app;
    SharedHistory& history;
    const unsigned slot, panel;
    unsigned char* const model;
    const MmsObjectRef object;
    std::shared_ptr<RetainedSceneObject> owner;
    mdl::MorphRecord* const morphs;
    const unsigned count;
    std::uint32_t* const cursors;
    unsigned char* const active;
    const std::uint64_t poolIdentity, binding;
    const int initialFrame;
    const std::size_t initialCapacity;
    std::vector<Rule> rules;
    std::vector<unsigned> affected;
    Pool original;
    struct Patch { unsigned slot; Key before, after; };
    std::vector<Patch> patches;
    morph_curve::AuthoredReplacement authoring;
    bool changed = false, published = false, applied = false;
    std::uint64_t generation = 1;
    Purge(MMDApp& a, SharedHistory& h, unsigned s, unsigned p)
        : app(a), history(h), slot(s), panel(p), model(a.ModelSlot(s)), object(SceneObjectIdentity(model)),
          owner(RetainSceneObject(a, MMS_OBJECT_MODEL, s)), morphs(mdl::Mdl(model)->morphs), count(mdl::Mdl(model)->morphCount),
          cursors(mdl::Mdl(model)->morphKeyCursors), active(mdl::Mdl(model)->morphTrackActive),
          poolIdentity(mdl::MorphKeyPoolIdentity(mdl::Mdl(model)->morphKeys)), binding(BoundNativePoseGeneration(&a)),
          initialFrame(a.CurrentFrame()), initialCapacity(mdl::MorphKeyCapacity(mdl::Mdl(model)->morphKeys)),
          original(mdl::Mdl(model)->morphKeys, mdl::Mdl(model)->morphKeys + initialCapacity),
          authoring(morph_curve::AuthoredReplacement::Erase(model, CategoryMorphs(*mdl::Mdl(model), p))) {
        const auto chains = Chains(original, count); rules.reserve(count);
        for (unsigned morph = 0; morph < count; ++morph) {
            rules.emplace_back(morphs[morph]);
            if (cursors) rules.back().cursor = cursors[morph];
            if (active) rules.back().active = active[morph];
            if (unsigned(morphs[morph].panel) != panel) continue;
            affected.push_back(morph); changed |= rules.back().weight != 0;
            for (unsigned index : chains[morph]) {
                patches.push_back({index, original[index], {}}); changed |= !Same(original[index], {});
            }
        }
        std::sort(patches.begin(), patches.end(), [](const Patch& a, const Patch& b) { return a.slot < b.slot; });
        changed |= authoring.Changed();
    }
    bool Structure(bool initial = false) const noexcept {
        if (app.ModelSlot(slot) != model || !owner->Present() || !SceneObjectPresent(model, object.id)) return false;
        const auto& record = *mdl::Mdl(model);
        if (record.morphs != morphs || record.morphCount != count || record.morphKeyCursors != cursors ||
            record.morphTrackActive != active || mdl::MorphKeyPoolIdentity(record.morphKeys) != poolIdentity ||
            mdl::MorphKeyCapacity(record.morphKeys) < initialCapacity ||
            !Exclusive(app, slot, record.morphKeys, mdl::MorphKeyCapacity(record.morphKeys))) return false;
        if (initial) {
            for (unsigned morph = 0; morph < count; ++morph) if (!rules[morph].Identity(morphs[morph])) return false;
        } else for (unsigned morph : affected) if (!rules[morph].Identity(morphs[morph])) return false;
        return true;
    }
    bool InitialCurrent() const noexcept {
        if (BoundNativePoseHistory(&app) != &history || BoundNativePoseGeneration(&app) != binding ||
            !Structure(true) || app.CameraMode() || app.SelectedModelSlot() != slot || app.CurrentFrame() != initialFrame ||
            SceneObjectIdentity(model).generation != object.generation) return false;
        const auto* keys = mdl::Mdl(model)->morphKeys;
        if (mdl::MorphKeyCapacity(keys) != original.size() || std::memcmp(keys, original.data(), original.size() * sizeof(Key))) return false;
        for (unsigned morph = 0; morph < count; ++morph)
            if (WeightBits(morphs[morph]) != rules[morph].weight ||
                (cursors && cursors[morph] != rules[morph].cursor) || (active && active[morph] != rules[morph].active)) return false;
        return true;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<Purge> source;
        const bool forward, initial;
        const std::uint64_t generation, binding, objectGeneration;
        const int frame;
        Key* const pool;
        const std::size_t capacity;
        Pool current, target;
        std::vector<std::uint32_t> weights, beforeCursors, afterCursors;
        std::vector<unsigned char> beforeActive, afterActive;
        std::unique_ptr<morph_curve::AuthoredReplacement::Target> layers;
        mutable resource_budget::Collector resources;
        std::uint64_t bytes = 0;
        bool consumed = false;
        Candidate(std::shared_ptr<Purge> value, bool direction)
            : source(std::move(value)), forward(direction), initial(!source->published), generation(source->generation),
              binding(BoundNativePoseGeneration(&source->app)), objectGeneration(SceneObjectIdentity(source->model).generation),
              frame(source->app.CurrentFrame()), pool(mdl::Mdl(source->model)->morphKeys),
              capacity(mdl::MorphKeyCapacity(pool)), current(pool, pool + capacity), target(current) {}
        bool IsCurrent() const noexcept override {
            const auto& s = *source;
            if (consumed || s.generation != generation || forward == s.applied || initial == s.published ||
                BoundNativePoseHistory(&s.app) != &s.history || BoundNativePoseGeneration(&s.app) != binding ||
                !Ready(s.app, !initial) || !s.Structure(initial) || s.app.CurrentFrame() != frame ||
                SceneObjectIdentity(s.model).generation != objectGeneration ||
                mdl::Mdl(s.model)->morphKeys != pool || mdl::MorphKeyCapacity(pool) != capacity ||
                std::memcmp(pool, current.data(), capacity * sizeof(Key)) || !layers || !layers->IsCurrent() ||
                !s.owner->CoversBookkeeping(resources)) return false;
            if (initial && !s.InitialCurrent()) return false;
            for (std::size_t i = 0; i < s.affected.size(); ++i) {
                const unsigned morph = s.affected[i];
                if (WeightBits(s.morphs[morph]) != weights[i] || (s.cursors && s.cursors[morph] != beforeCursors[i]) ||
                    (s.active && s.active[morph] != beforeActive[i])) return false;
            }
            return true;
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            for (const auto& patch : source->patches) pool[patch.slot] = forward ? patch.after : patch.before;
            layers->Publish();
            for (std::size_t i = 0; i < source->affected.size(); ++i) {
                const unsigned morph = source->affected[i];
                const std::uint32_t bits = forward ? 0 : source->rules[morph].weight;
                std::memcpy(&source->morphs[morph].value, &bits, sizeof bits);
                if (source->cursors) source->cursors[morph] = afterCursors[i];
                if (source->active) source->active[morph] = afterActive[i];
            }
            source->published = true; source->applied = forward; ++source->generation; consumed = true;
        }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();
        return Boundary([&]() -> MmsResult {
            if (BoundNativePoseHistory(&app) != &history || forward == applied || !Structure(!published) ||
                !authoring.IsCurrent() || (!published && !InitialCurrent())) return MMS_E_STALE;
            if (!Ready(app, published)) return MMS_E_BUSY;
            if (generation == UINT64_MAX) return MMS_E_NOT_READY;
            if (app.CurrentFrame() < 0 || app.CurrentFrame() > frame_limits::maximum) return MMS_E_ARGUMENT;
            auto next = std::make_unique<Candidate>(shared_from_this(), forward);
            for (const auto& patch : patches) {
                if (!Same(next->pool[patch.slot], forward ? patch.before : patch.after, !published)) return MMS_E_STALE;
                next->target[patch.slot] = forward ? patch.after : patch.before;
            }
            (void)Chains(next->current, count); (void)Chains(next->target, count);
            next->layers = authoring.Prepare(View(next->target), forward);
            for (unsigned morph : affected) {
                next->weights.push_back(WeightBits(morphs[morph]));
                unsigned index = morph;
                while (next->target[index].frame < unsigned(next->frame) && next->target[index].next) index = next->target[index].next;
                if (cursors) {
                    next->beforeCursors.push_back(cursors[morph]);
                    next->afterCursors.push_back(!forward && next->frame == initialFrame ? rules[morph].cursor : index);
                }
                if (active) {
                    next->beforeActive.push_back(active[morph]);
                    next->afterActive.push_back(!forward && next->frame == initialFrame ? rules[morph].active :
                        static_cast<unsigned char>(next->target[index].frame >= unsigned(next->frame)));
                }
                if (forward) {
                    float value = -1;
                    if (!next->layers->Sample(int(morph), next->frame, value) || value != 0) return MMS_E_ARGUMENT;
                }
            }
            if (auto result = owner->MeasureBookkeeping(next->resources)) return result;
            if (!next->resources.Result().Complete()) return MMS_E_NOT_READY;
            next->bytes = resource_budget::Collector::Sum(RetainedBytes(), next->resources.Result().totalBytes);
            if (!next->IsCurrent()) return MMS_E_STALE;
            out = std::move(next); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t bytes = sizeof(*this) + 4 * sizeof(void*);
        Add(bytes, rules.capacity(), sizeof(Rule)); Add(bytes, affected.capacity(), sizeof(unsigned));
        Add(bytes, original.capacity(), sizeof(Key)); Add(bytes, patches.capacity(), sizeof(Patch));
        Add(bytes, authoring.RetainedBytes()); return bytes;
    }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};
}
MmsResult PlanMorphCategoryPurge(MMDApp& app, unsigned slot, unsigned panel, std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if (panel < 1 || panel > 3) return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app); if (!history) return MMS_E_NOT_READY;
        if (auto result = history->CheckNativeEdit()) return result;
        if (!Ready(app) || app.CameraMode() || slot >= kModelSlotCount || app.SelectedModelSlot() != slot ||
            !app.ModelSlot(slot)) return MMS_E_NOT_READY;
        if (app.CurrentFrame() < 0 || app.CurrentFrame() > frame_limits::maximum) return MMS_E_ARGUMENT;
        const auto& record = *mdl::Mdl(app.ModelSlot(slot));
        if (!record.morphCount) return MMS_OK;
        if (!record.morphs || !mdl::MorphKeyPoolIdentity(record.morphKeys) ||
            record.morphCount > mdl::MorphKeyCapacity(record.morphKeys) ||
            !Exclusive(app, slot, record.morphKeys, mdl::MorphKeyCapacity(record.morphKeys))) return MMS_E_NOT_READY;
        bool present = false;
        for (unsigned morph = 0; morph < record.morphCount; ++morph) present |= unsigned(record.morphs[morph].panel) == panel;
        if (!present) return MMS_OK;
        auto change = std::make_shared<Purge>(app, *history, slot, panel);
        if (change->changed) out = std::move(change);
        return MMS_OK;
    });
}
bool TryPurgeBoundMorphCategory(MMDApp* app, unsigned panel) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app); if (!history) return false;
    if (confirmationActive) return true;
    const auto result = Boundary([&]() -> MmsResult {
        if (panel < 1 || panel > 3) return MMS_E_ARGUMENT;
        const auto binding = BoundNativePoseGeneration(app);
        std::uint64_t revision = 0;
        if (auto result = history->Revision(revision)) return result;
        std::shared_ptr<HistoryChange> change;
        if (auto result = PlanMorphCategoryPurge(*app, app->SelectedModelSlot(), panel, change)) return result;
        struct ConfirmationScope {
            ConfirmationScope() { confirmationActive = true; }
            ~ConfirmationScope() { confirmationActive = false; }
        } scope;
        app->state.enterKeyState = 1;
        if (ConfirmPurge(*app, panel) != IDOK) return MMS_OK;
        // The modal loop can replace a binding and destroy its old journal.
        // Never dereference the borrowed pointer before validating the binding.
        if (BoundNativePoseHistory(app) != history || BoundNativePoseGeneration(app) != binding) return MMS_E_STALE;
        std::uint64_t current = 0;
        if (auto result = history->Revision(current)) return result;
        if (current != revision) return MMS_E_STALE;
        if (!change) return MMS_OK;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (auto result = history->PrepareNative("Purge morph category frames", std::move(change), transaction,
            SharedHistory::NativeIntent::ExplicitCommit)) return result;
        if (auto result = history->CommitNative(std::move(transaction))) return result;
        bake::ScopedUiInputGate gate;
        PublishBoundPoseEdit(*app, *history, false);
        // UI-only best effort. This does not reconcile or replace authoring,
        // and a failed editor refresh never changes committed edit status.
        curve_editor::NotifyOwnedChanged(app->MainWindow());
        return MMS_OK;
    });
    if (result != MMS_OK) runtime_log::Write("PLUGIN_ABI morph category purge refused: %d", result);
    return true;
}
}
#endif
