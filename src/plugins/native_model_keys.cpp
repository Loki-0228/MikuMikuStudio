#include "native_model_keys.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_bone_registration.hpp"
#include "native_display_keys.hpp"
#include "native_morph_keys.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include <array>
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
struct ModelDeletion final : HistoryChange {
    MMDApp& app;
    unsigned slot;
    unsigned char* model;
    // Bone publication must precede display: its prepared pose uses target
    // windows but its final guard still checks the live source selectors.
    std::array<std::shared_ptr<HistoryChange>, 3> parts;
    ModelDeletion(MMDApp& a, unsigned s) : app(a), slot(s), model(a.ModelSlot(s)) {}
    struct Candidate final : PreparedHistoryChange {
        MMDApp& app; unsigned slot; unsigned char* model;
        Layout layout;
        std::array<std::unique_ptr<PreparedHistoryChange>, 3> parts;
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
        auto change = std::make_shared<ModelDeletion>(app, slot);
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
}
#endif
