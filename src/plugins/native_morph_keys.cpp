#include "native_morph_keys.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "scene_object_history.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/morph_curve.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include <array>
#include <cmath>
#include <exception>
#include <map>
#include <set>

namespace mikudancestudio::plugins {
namespace {
struct Patch { unsigned slot; mdl::MorphKey before, after; };
struct Dependency { unsigned slot; mdl::MorphKey key; };
struct Rule {
    unsigned morph;
    std::array<char, 20> name;
    mdl::MorphPanel panel;
    unsigned char type;
};
using Owners = std::map<unsigned, unsigned>;
bool SameKey(mdl::MorphKey a, mdl::MorphKey b, bool marks) noexcept {
    if (!marks) a.allocated = b.allocated = 0;
    return !std::memcmp(&a, &b, sizeof(a));
}
bool SameFloat(float a, float b) noexcept { return !std::memcmp(&a, &b, sizeof(a)); }
template<class Key> bool Marked(const Key* keys, std::size_t capacity) noexcept {
    for (std::size_t slot = 0; keys && slot < capacity; ++slot) if (keys[slot].allocated) return true;
    return false;
}
// Follow every root so shared slots and malformed unrelated tracks cannot
// make a deletion silently unlink somebody else's key. Frame-zero heads may
// carry a nonzero previous sentinel after the legacy last-key deletion.
std::vector<Dependency> Chains(const mdl::ModelRecord& model, Owners& owners) {
    const auto capacity = mdl::MorphKeyCapacity(model.morphKeys);
    if (!model.morphs || !model.morphKeys || model.morphCount > capacity)
        throw std::invalid_argument("missing morph tracks");
    owners.clear(); std::vector<Dependency> dependencies;
    for (unsigned morph = 0; morph < model.morphCount; ++morph) {
        unsigned slot = morph, previous = 0, frame = 0; bool ended = false;
        for (std::size_t hop = 0; hop < capacity; ++hop) {
            if (slot >= capacity || (hop && slot < model.morphCount) || !owners.emplace(slot, morph).second)
                throw std::invalid_argument("shared or invalid morph key slot");
            const auto key = model.morphKeys[slot];
            if (key.previous >= capacity || key.next >= capacity || key.frame > unsigned(frame_limits::maximum) ||
                !std::isfinite(key.value) || (!hop && key.frame) || (hop && (key.previous != previous || key.frame <= frame)))
                throw std::invalid_argument("invalid morph key link frame or value");
            dependencies.push_back({slot, key});
            if (!key.next) { ended = true; break; }
            previous = slot; frame = key.frame; slot = key.next;
        }
        if (!ended) throw std::invalid_argument("unterminated morph key track");
    }
    return dependencies;
}
void AddBytes(std::uint64_t& bytes, std::size_t count, std::size_t stride) noexcept {
    if (count > (UINT64_MAX - bytes) / stride) bytes = UINT64_MAX;
    else bytes += std::uint64_t(count) * stride;
}
struct MorphDeletion final : HistoryChange, std::enable_shared_from_this<MorphDeletion> {
    MMDApp& app;
    unsigned modelSlot;
    unsigned char* model;
    std::shared_ptr<RetainedSceneObject> lifetime;
    mdl::MorphRecord* morphs;
    unsigned count;
    std::uint64_t poolIdentity;
    std::size_t capacity;
    std::vector<Patch> patches;
    std::vector<Rule> rules;
    MorphDeletion(MMDApp& owner, unsigned slot) : app(owner), modelSlot(slot), model(app.ModelSlot(slot)),
        lifetime(RetainSceneObject(app, MMS_OBJECT_MODEL, slot)), morphs(mdl::Mdl(model)->morphs),
        count(mdl::Mdl(model)->morphCount), poolIdentity(mdl::MorphKeyPoolIdentity(mdl::Mdl(model)->morphKeys)),
        capacity(mdl::MorphKeyCapacity(mdl::Mdl(model)->morphKeys)) {}
    bool Structure() const noexcept {
        if (!lifetime || !lifetime->Present() || app.ModelSlot(modelSlot) != model || !SceneObjectPresent(model, lifetime->Id())) return false;
        const auto& live = *mdl::Mdl(model);
        if (live.morphs != morphs || live.morphCount != count || mdl::MorphKeyPoolIdentity(live.morphKeys) != poolIdentity ||
            mdl::MorphKeyCapacity(live.morphKeys) < capacity) return false;
        for (const auto& rule : rules) {
            const auto& morph = morphs[rule.morph];
            if (morph.panel != rule.panel || morph.type != rule.type || std::memcmp(morph.name, rule.name.data(), rule.name.size())) return false;
        }
        return true;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<MorphDeletion> source;
        bool forward, published = false;
        mdl::MorphKey* pool;
        std::size_t capacity;
        int frame;
        morph_curve::AuthoredLayers layers;
        std::vector<mdl::MorphKey> current;
        std::vector<Dependency> dependencies;
        std::vector<float> beforeValues, afterValues;
        Candidate(std::shared_ptr<MorphDeletion> owner, bool direction) : source(std::move(owner)), forward(direction),
            pool(mdl::Mdl(source->model)->morphKeys), capacity(mdl::MorphKeyCapacity(pool)), frame(source->app.CurrentFrame()) {}
        const mdl::MorphKey* Target(unsigned slot) const noexcept {
            const auto& patches = source->patches;
            const auto found = std::lower_bound(patches.begin(), patches.end(), slot,
                [](const Patch& patch, unsigned index) { return patch.slot < index; });
            if (found != patches.end() && found->slot == slot) return forward ? &found->after : &found->before;
            return slot < capacity ? &pool[slot] : nullptr;
        }
        bool IsCurrent() const noexcept override {
            if (published || !source->Structure() || source->app.CurrentFrame() != frame ||
                mdl::Mdl(source->model)->morphKeys != pool || mdl::MorphKeyCapacity(pool) != capacity) return false;
            for (std::size_t i = 0; i < source->patches.size(); ++i)
                if (!SameKey(current[i], pool[source->patches[i].slot], true)) return false;
            for (const auto& dependency : dependencies)
                if (!SameKey(dependency.key, pool[dependency.slot], true)) return false;
            for (std::size_t i = 0; i < source->rules.size(); ++i)
                if (!SameFloat(beforeValues[i], source->morphs[source->rules[i].morph].value)) return false;
            return morph_curve::LayersUnchanged(source->model, layers);
        }
        void Publish() noexcept override {
            // The enclosing journal/aggregate validates every participant
            // before publication; other families must not write these fields.
            if (!IsCurrent()) std::terminate();
            for (const auto& patch : source->patches) pool[patch.slot] = forward ? patch.after : patch.before;
            for (std::size_t i = 0; i < source->rules.size(); ++i)
                source->morphs[source->rules[i].morph].value = afterValues[i];
            published = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset(); if (!Structure()) return MMS_E_STALE;
        if (app.CurrentFrame() < 0 || app.CurrentFrame() > frame_limits::maximum) return MMS_E_ARGUMENT;
        return Boundary([&]() -> MmsResult {
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            Owners owners; candidate->dependencies = Chains(*mdl::Mdl(model), owners);
            candidate->current.reserve(patches.size());
            for (const auto& patch : patches) {
                const auto live = candidate->pool[patch.slot];
                if (!SameKey(live, forward ? patch.before : patch.after, false)) return MMS_E_STALE;
                candidate->current.push_back(live);
            }
            std::string error;
            if (!morph_curve::CaptureLayers(model, candidate->layers, error)) return MMS_E_ARGUMENT;
            morph_curve::NativeKeyView view;
            view.context = candidate.get(); view.capacity = candidate->capacity;
            view.key = [](const void* context, std::uint32_t slot) noexcept {
                return static_cast<const Candidate*>(context)->Target(slot);
            };
            candidate->beforeValues.reserve(rules.size()); candidate->afterValues.reserve(rules.size());
            for (const auto& rule : rules) {
                float value = 0;
                if (!morph_curve::SampleLayersTarget(candidate->layers, view, int(rule.morph), candidate->frame, value, error))
                    return MMS_E_ARGUMENT;
                candidate->beforeValues.push_back(morphs[rule.morph].value); candidate->afterValues.push_back(value);
            }
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t bytes = sizeof(*this) + 4 * sizeof(void*);
        AddBytes(bytes, patches.capacity(), sizeof(Patch)); AddBytes(bytes, rules.capacity(), sizeof(Rule)); return bytes;
    }
};
}
MmsResult PlanMorphKeyDeletion(MMDApp& app, unsigned slot, std::shared_ptr<HistoryChange>& out, bool allowOtherMarks) noexcept {
    out.reset();
    if (slot >= kModelSlotCount || !app.ModelSlot(slot) || app.PlaybackActive() || app.FrameStepPlayback() ||
        app.state.recordPlaybackStartPending || app.SceneMutationInProgress()) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        if (!mdl::MorphKeyPoolIdentity(model.morphKeys)) return MMS_E_NOT_READY;
        if (!allowOtherMarks) {
            if ((model.boneKeys && !mdl::BoneKeyPoolIdentity(model.boneKeys)) ||
                (model.displayKeys && !mdl::KeyPoolIdentity(model.displayKeys))) return MMS_E_NOT_READY;
            if (Marked(model.boneKeys, mdl::BoneKeyCapacity(model.boneKeys)) ||
                Marked(model.displayKeys, mdl::KeyPoolCapacity(model.displayKeys))) return MMS_E_NOT_READY;
        }
        const auto capacity = mdl::MorphKeyCapacity(model.morphKeys);
        if (!Marked(model.morphKeys, capacity)) return MMS_OK;
        Owners owners; Chains(model, owners);
        std::map<unsigned, Patch> planned; std::set<unsigned> affected;
        const auto write = [&](unsigned index) -> mdl::MorphKey& {
            auto found = planned.find(index);
            if (found == planned.end()) {
                const auto key = model.morphKeys[index]; found = planned.emplace(index, Patch{index, key, key}).first;
            }
            return found->second.after;
        };
        for (std::size_t index = 0; index < capacity; ++index) if (model.morphKeys[index].allocated) {
            const auto owner = owners.find(unsigned(index)); if (owner == owners.end()) return MMS_E_ARGUMENT;
            affected.insert(owner->second);
            auto& key = write(unsigned(index)); const auto previous = key.previous, next = key.next;
            key.frame = 0;
            if (index >= model.morphCount) { write(previous).next = next; write(next).previous = previous; }
            key.allocated = 0; key.value = 0;
            if (index >= model.morphCount) key.next = 0;
            key.previous = 0;
        }
        auto change = std::make_shared<MorphDeletion>(app, slot); change->patches.reserve(planned.size());
        for (const auto& patch : planned) change->patches.push_back(patch.second);
        change->rules.reserve(affected.size());
        for (auto morph : affected) {
            const auto& record = model.morphs[morph]; Rule rule{morph, {}, record.panel, record.type};
            std::memcpy(rule.name.data(), record.name, rule.name.size()); change->rules.push_back(rule);
        }
        out = std::move(change); return MMS_OK;
    });
}
}
#endif
