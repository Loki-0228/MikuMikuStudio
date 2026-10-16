#include "native_model_keys.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_bone_registration.hpp"
#include "native_display_keys.hpp"
#include "native_morph_keys.hpp"
#include "native_range_scale.hpp"
#include "scene_object_history.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include <array>
#include <cmath>
#include <vector>

namespace mikudancestudio::plugins {
namespace {
struct Range {
    std::uintptr_t begin, end;
    bool operator==(const Range& b) const noexcept { return begin == b.begin && end == b.end; }
};
// Includes read and write storage across all families. Per-family ownership
// checks alone cannot detect a display payload pointing into another pool.
template<class Visitor> bool VisitStorage(const mdl::ModelRecord& m, Visitor&& visit) {
    const auto add = [&](const void* pointer, std::size_t count, std::size_t stride) {
        const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
        if (count > (UINTPTR_MAX - begin) / stride) return false;
        return visit(Range{begin, pointer ? begin + count * stride : begin});
    };
    if (!add(&m, 1, sizeof(m)) ||
        !add(m.boneKeys, mdl::BoneKeyCapacity(m.boneKeys), sizeof(mdl::BoneKey)) ||
        !add(m.morphKeys, mdl::MorphKeyCapacity(m.morphKeys), sizeof(mdl::MorphKey)) ||
        !add(m.displayKeys, mdl::KeyPoolCapacity(m.displayKeys), sizeof(mdl::DisplayKey)) ||
        !add(m.boneTable, m.boneCount, sizeof(mdl::BoneRecord)) ||
        !add(m.morphs, m.morphCount, sizeof(mdl::MorphRecord)) ||
        !add(m.ikChains, m.ikChainCount, sizeof(mdl::IkChain)) ||
        !add(m.boneOrderTable, m.boneOrderCount, sizeof(mdl::BoneOrderEntry)) ||
        !add(m.bonePhysicsState, m.boneCount, 1) ||
        !add(m.boneSelection, m.boneCount, 1)) return false;
    for (std::size_t index = 0; index < mdl::KeyPoolCapacity(m.displayKeys); ++index)
        if (!add(m.displayKeys[index].ikStates, m.ikChainCount, 1) ||
            !add(m.displayKeys[index].selectorStates, m.boneOrderCount, sizeof(mdl::BoneReference))) return false;
    return true;
}
struct Layout {
    std::vector<Range> ranges;
    explicit Layout(const mdl::ModelRecord& model) {
        if (!VisitStorage(model, [&](const Range& range) { ranges.push_back(range); return true; }))
            throw std::invalid_argument("model key storage overflow");
        auto sorted = ranges;
        sorted.erase(std::remove_if(sorted.begin(), sorted.end(), [](const Range& r) { return r.begin == r.end; }), sorted.end());
        std::sort(sorted.begin(), sorted.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
        for (std::size_t index = 1; index < sorted.size(); ++index)
            if (sorted[index].begin < sorted[index - 1].end)
                throw std::invalid_argument("aliased model key storage");
    }
    bool IsCurrent(const mdl::ModelRecord& model) const noexcept {
        std::size_t index = 0;
        return VisitStorage(model, [&](const Range& range) {
            return index < ranges.size() && ranges[index++] == range;
        }) && index == ranges.size();
    }
};
struct ModelKeyChange final : HistoryChange {
    MMDApp& app;
    unsigned slot;
    unsigned char* model;
    // Bone publication must precede display: its prepared pose uses target
    // windows but its final guard still checks the live source selectors.
    std::array<std::shared_ptr<HistoryChange>, 4> parts;
    ModelKeyChange(MMDApp& a, unsigned s) : app(a), slot(s), model(a.ModelSlot(s)) {}
    struct Candidate final : PreparedHistoryChange {
        MMDApp& app; unsigned slot; unsigned char* model;
        Layout layout;
        std::array<std::unique_ptr<PreparedHistoryChange>, 4> parts;
        bool published = false;
        Candidate(MMDApp& a, unsigned s) : app(a), slot(s), model(a.ModelSlot(s)), layout(*mdl::Mdl(model)) {}
        bool IsCurrent() const noexcept override {
            if (published || app.ModelSlot(slot) != model) return false;
            for (const auto& part : parts) if (part && !part->IsCurrent()) return false;
            return layout.IsCurrent(*mdl::Mdl(model));
        }
        void Publish() noexcept override {
            // Validate the entire write set once, before the first write.
            if (!IsCurrent()) std::terminate();
            for (const auto& part : parts) if (part) part->Publish();
            published = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();
        if (app.ModelSlot(slot) != model) return MMS_E_STALE;
        // Unknown legacy pools have assumed capacities. Reject them before
        // Layout reads nested display buffers, rather than relying on a later
        // participant's identity check to discover the replacement.
        const auto& live = *mdl::Mdl(model);
        if ((live.boneKeys && !mdl::BoneKeyPoolIdentity(live.boneKeys)) ||
            (live.morphKeys && !mdl::MorphKeyPoolIdentity(live.morphKeys)) ||
            (live.displayKeys && !mdl::KeyPoolIdentity(live.displayKeys))) return MMS_E_STALE;
        return Boundary([&]() -> MmsResult {
            auto candidate = std::make_unique<Candidate>(app, slot);
            for (std::size_t index = 0; index < parts.size(); ++index) if (parts[index])
                if (auto result = parts[index]->Prepare(forward, candidate->parts[index])) return result;
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t bytes = sizeof(*this) + 4 * sizeof(void*);
        for (const auto& part : parts) if (part) {
            const auto amount = part->RetainedBytes();
            bytes = amount > UINT64_MAX - bytes ? UINT64_MAX : bytes + amount;
        }
        return bytes;
    }
};
struct RangeMetadata final : HistoryChange, std::enable_shared_from_this<RangeMetadata> {
    MMDApp& app; unsigned slot; unsigned char* model;
    std::shared_ptr<RetainedSceneObject> owner;
    std::uint32_t maxBefore, maxAfter, lastBefore, lastAfter;
    int cursorBefore, cursorAfter;
    std::array<std::uint64_t, 3> identities;
    std::array<std::vector<unsigned char>, 3> before, after;
    explicit RangeMetadata(MMDApp& a, unsigned s) : app(a), slot(s), model(a.ModelSlot(s)),
        owner(RetainSceneObject(a, MMS_OBJECT_MODEL, s)), maxBefore(mdl::Mdl(model)->maxFrame), maxAfter(maxBefore),
        lastBefore(a.state.lastRegisteredFrame), lastAfter(lastBefore),
        cursorBefore(mdl::Mdl(model)->searchCursor), cursorAfter(cursorBefore) {
        const auto& m = *mdl::Mdl(model);
        identities = {mdl::BoneKeyPoolIdentity(m.boneKeys), mdl::MorphKeyPoolIdentity(m.morphKeys), mdl::KeyPoolIdentity(m.displayKeys)};
        Capture(before); after = before;
        for (auto& marks : after) std::fill(marks.begin(), marks.end(), 0);
    }
    void Capture(std::array<std::vector<unsigned char>, 3>& marks) const {
        const auto& m = *mdl::Mdl(model);
        const auto copy = [](const auto* keys, std::size_t count, auto& out) {
            out.reserve(count); for (std::size_t i = 0; i < count; ++i) out.push_back(keys[i].allocated);
        };
        copy(m.boneKeys, mdl::BoneKeyCapacity(m.boneKeys), marks[0]);
        copy(m.morphKeys, mdl::MorphKeyCapacity(m.morphKeys), marks[1]);
        copy(m.displayKeys, mdl::KeyPoolCapacity(m.displayKeys), marks[2]);
    }
    bool Structure() const noexcept {
        if (!owner->Present() || app.ModelSlot(slot) != model) return false;
        const auto& m = *mdl::Mdl(model);
        return mdl::BoneKeyPoolIdentity(m.boneKeys) == identities[0] && mdl::BoneKeyCapacity(m.boneKeys) >= before[0].size() &&
            mdl::MorphKeyPoolIdentity(m.morphKeys) == identities[1] && mdl::MorphKeyCapacity(m.morphKeys) >= before[1].size() &&
            mdl::KeyPoolIdentity(m.displayKeys) == identities[2] && mdl::KeyPoolCapacity(m.displayKeys) >= before[2].size();
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<RangeMetadata> source; bool forward, published = false;
        std::array<std::vector<unsigned char>, 3> current;
        std::array<const void*, 3> pools;
        Candidate(std::shared_ptr<RangeMetadata> s, bool f) : source(std::move(s)), forward(f) {
            const auto& m = *mdl::Mdl(source->model); pools = {m.boneKeys, m.morphKeys, m.displayKeys}; source->Capture(current);
        }
        bool IsCurrent() const noexcept override {
            if (published || !source->Structure()) return false;
            const auto& m = *mdl::Mdl(source->model);
            if (m.maxFrame != (forward ? source->maxBefore : source->maxAfter) ||
                m.searchCursor != (forward ? source->cursorBefore : source->cursorAfter) ||
                source->app.state.lastRegisteredFrame != (forward ? source->lastBefore : source->lastAfter)) return false;
            if (pools != std::array<const void*, 3>{m.boneKeys, m.morphKeys, m.displayKeys}) return false;
            const auto same = [](const auto* keys, std::size_t count, const auto& marks) {
                if (count != marks.size()) return false;
                for (std::size_t i = 0; i < count; ++i) if (keys[i].allocated != marks[i]) return false;
                return true;
            };
            return same(m.boneKeys, mdl::BoneKeyCapacity(m.boneKeys), current[0]) &&
                same(m.morphKeys, mdl::MorphKeyCapacity(m.morphKeys), current[1]) &&
                same(m.displayKeys, mdl::KeyPoolCapacity(m.displayKeys), current[2]);
        }
        void Publish() noexcept override {
            // Earlier family participants have already written their own key
            // marks. All guards ran before that first write; never recheck here.
            auto& m = *mdl::Mdl(source->model);
            const auto& target = forward ? source->after : source->before;
            const auto write = [](auto* keys, const auto& marks) {
                for (std::size_t i = 0; i < marks.size(); ++i) keys[i].allocated = marks[i];
            };
            write(m.boneKeys, target[0]); write(m.morphKeys, target[1]); write(m.displayKeys, target[2]);
            m.maxFrame = forward ? source->maxAfter : source->maxBefore;
            m.searchCursor = forward ? source->cursorAfter : source->cursorBefore;
            source->app.state.lastRegisteredFrame = forward ? source->lastAfter : source->lastBefore;
            published = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); if (!Structure()) return MMS_E_STALE;
        return Boundary([&]() -> MmsResult {
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t total = sizeof(*this) + 4 * sizeof(void*);
        for (const auto* side : {&before, &after}) for (const auto& marks : *side) {
            const auto bytes = marks.capacity(); total = bytes > UINT64_MAX - total ? UINT64_MAX : total + bytes;
        }
        return total;
    }
};
template<class Key> bool Marked(const Key* keys, std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) if (keys[index].allocated) return true;
    return false;
}
}
MmsResult PlanModelKeyDeletion(MMDApp& app, unsigned slot, bool automatic,
    std::shared_ptr<HistoryChange>& out, bool fallback) noexcept {
    out.reset();
    if (slot >= kModelSlotCount || !app.ModelSlot(slot) || app.PlaybackActive() || app.FrameStepPlayback() ||
        app.state.recordPlaybackStartPending || app.SceneMutationInProgress()) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        if ((model.boneKeys && !mdl::BoneKeyPoolIdentity(model.boneKeys)) ||
            (model.morphKeys && !mdl::MorphKeyPoolIdentity(model.morphKeys)) ||
            (model.displayKeys && !mdl::KeyPoolIdentity(model.displayKeys))) return MMS_E_NOT_READY;
        Layout check(model);
        const bool bones = Marked(model.boneKeys, mdl::BoneKeyCapacity(model.boneKeys));
        const bool morphs = Marked(model.morphKeys, mdl::MorphKeyCapacity(model.morphKeys));
        const bool displays = Marked(model.displayKeys, mdl::KeyPoolCapacity(model.displayKeys));
        auto change = std::make_shared<ModelKeyChange>(app, slot);
        std::shared_ptr<DisplayKeyDeletion> display;
        if (displays) {
            if (auto result = PlanDisplayKeyDeletion(app, slot, change->parts[2], true, false)) return result;
            display = std::static_pointer_cast<DisplayKeyDeletion>(change->parts[2]);
        }
        if (bones || (model.boneCount && display) || (fallback && !morphs && !displays && model.boneCount)) {
            if (auto result = PlanBoneKeyDeletion(app, slot, automatic, change->parts[0], fallback, true, display)) return result;
        }
        if (morphs) if (auto result = PlanMorphKeyDeletion(app, slot, change->parts[1], true)) return result;
        for (const auto& part : change->parts) if (part) { out = std::move(change); break; }
        return MMS_OK;
    });
}
MmsResult PlanModelRangeScale(MMDApp& app, unsigned slot, const ModelRangeScaleOptions& options,
    std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if (slot >= kModelSlotCount || !app.ModelSlot(slot) || app.PlaybackActive() || app.FrameStepPlayback() ||
        app.state.recordPlaybackStartPending || app.SceneMutationInProgress()) return MMS_E_NOT_READY;
    if (options.start >= options.end || options.end > unsigned(frame_limits::maximum) ||
        !std::isfinite(options.scale) || options.scale <= 0) return MMS_E_ARGUMENT;
    if (options.scale == 1 || (!options.bones && !options.morphs && !options.display)) return MMS_OK;
    return Boundary([&]() -> MmsResult {
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        if ((model.boneKeys && !mdl::BoneKeyPoolIdentity(model.boneKeys)) ||
            (model.morphKeys && !mdl::MorphKeyPoolIdentity(model.morphKeys)) ||
            (model.displayKeys && !mdl::KeyPoolIdentity(model.displayKeys))) return MMS_E_NOT_READY;
        Layout check(model);
        const BoneRangeScaleOptions family{options.start, options.end, options.scale, options.automaticInterpolation};
        std::array<RangeScaleResult, 3> results;
        std::shared_ptr<DisplayKeyDeletion> display;
        if (options.display) {
            if (auto result = PlanDisplayRangeScale(app, slot, family, results[2])) return result;
            display = std::static_pointer_cast<DisplayKeyDeletion>(results[2].change);
        }
        if (options.bones && model.boneCount) {
            std::shared_ptr<HistoryChange> bones;
            if (auto result = PlanBoneRangeScale(app, slot, family, bones, &results[0], display)) return result;
        } else if (display && model.boneCount) {
            if (auto result = PlanBoneDisplayEvaluation(app, slot, display, results[0].change)) return result;
        }
        if (options.morphs) if (auto result = PlanMorphRangeScale(app, slot, family, results[1])) return result;
        if (!results[0].moved && !results[1].moved && !results[2].moved) return MMS_OK;
        auto change = std::make_shared<ModelKeyChange>(app, slot);
        auto metadata = std::make_shared<RangeMetadata>(app, slot);
        int markedFamily = -1;
        for (unsigned i = 0; i < results.size(); ++i) {
            change->parts[i] = std::move(results[i].change);
            if (!results[i].hasKeys) continue;
            markedFamily = int(i); metadata->maxAfter = (std::max)(metadata->maxAfter, results[i].maxFrame);
            metadata->cursorAfter = results[i].searchCursor;
        }
        if (markedFamily >= 0) for (auto key : results[markedFamily].markedSlots) {
            if (key >= metadata->after[markedFamily].size()) return MMS_E_NOT_READY;
            metadata->after[markedFamily][key] = 1;
        }
        metadata->lastAfter = (std::max)(metadata->lastBefore, metadata->maxAfter);
        change->parts[3] = std::move(metadata); out = std::move(change); return MMS_OK;
    });
}
}
#endif
