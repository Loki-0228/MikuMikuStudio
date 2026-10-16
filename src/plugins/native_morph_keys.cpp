#include "native_morph_keys.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_bone_registration.hpp"
#include "native_range_scale.hpp"
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
template<class Read> std::vector<Dependency> TrackChains(unsigned count, std::size_t capacity, Read read, Owners& owners) {
    owners.clear(); std::vector<Dependency> dependencies;
    for (unsigned morph = 0; morph < count; ++morph) {
        unsigned slot = morph, previous = 0, frame = 0; bool ended = false;
        for (std::size_t hop = 0; hop < capacity; ++hop) {
            if (slot >= capacity || (hop && slot < count) || !owners.emplace(slot, morph).second)
                throw std::invalid_argument("shared or invalid morph key slot");
            const auto key = read(slot);
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
std::vector<Dependency> Chains(const mdl::ModelRecord& model, Owners& owners) {
    const auto capacity = mdl::MorphKeyCapacity(model.morphKeys);
    if (!model.morphs || !model.morphKeys || model.morphCount > capacity)
        throw std::invalid_argument("missing morph tracks");
    return TrackChains(model.morphCount, capacity, [&](unsigned slot) { return model.morphKeys[slot]; }, owners);
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
struct RegistrationPatch { Patch key; bool data = false; };
struct RegistrationFlag { unsigned slot; unsigned char before; };
struct MorphRegistration final : HistoryChange, std::enable_shared_from_this<MorphRegistration> {
    MMDApp& app;
    unsigned modelSlot, morph;
    unsigned char* model;
    std::shared_ptr<RetainedSceneObject> lifetime;
    mdl::MorphRecord* morphs;
    unsigned count;
    unsigned char physicsMode;
    Rule rule;
    std::uint64_t identity, boneIdentity, displayIdentity;
    std::size_t capacity, boneCapacity, displayCapacity;
    std::vector<RegistrationPatch> patches;
    std::vector<RegistrationFlag> boneFlags, displayFlags;
    std::uint32_t maxBefore, maxAfter, lastBefore, lastAfter;
    int search, playheadBefore;
    float value;
    bool published = false;
    MorphRegistration(MMDApp& a, unsigned slot, unsigned index, int frame)
        : app(a), modelSlot(slot), morph(index), model(a.ModelSlot(slot)),
          lifetime(RetainSceneObject(a, MMS_OBJECT_MODEL, slot)), morphs(mdl::Mdl(model)->morphs),
          count(mdl::Mdl(model)->morphCount), physicsMode(mdl::Mdl(model)->physicsMode),
          rule{index, {}, morphs[index].panel, morphs[index].type},
          identity(mdl::MorphKeyPoolIdentity(mdl::Mdl(model)->morphKeys)),
          boneIdentity(mdl::BoneKeyPoolIdentity(mdl::Mdl(model)->boneKeys)),
          displayIdentity(mdl::KeyPoolIdentity(mdl::Mdl(model)->displayKeys)),
          capacity(mdl::MorphKeyCapacity(mdl::Mdl(model)->morphKeys)),
          boneCapacity(mdl::BoneKeyCapacity(mdl::Mdl(model)->boneKeys)),
          displayCapacity(mdl::KeyPoolCapacity(mdl::Mdl(model)->displayKeys)),
          maxBefore(mdl::Mdl(model)->maxFrame), maxAfter(maxBefore),
          lastBefore(a.state.lastRegisteredFrame), lastAfter(lastBefore),
          search(mdl::Mdl(model)->searchCursor), playheadBefore(a.CurrentFrame()), value(morphs[index].value) {
        (void)frame;
        std::memcpy(rule.name.data(), morphs[index].name, rule.name.size());
    }
    bool Structure() const noexcept {
        if (!lifetime->Present() || app.ModelSlot(modelSlot) != model || !SceneObjectPresent(model, lifetime->Id())) return false;
        const auto& m = *mdl::Mdl(model);
        return m.morphs == morphs && m.morphCount == count && m.physicsMode == physicsMode &&
            mdl::MorphKeyPoolIdentity(m.morphKeys) == identity && mdl::MorphKeyCapacity(m.morphKeys) >= capacity &&
            (boneIdentity ? mdl::BoneKeyPoolIdentity(m.boneKeys) == boneIdentity && mdl::BoneKeyCapacity(m.boneKeys) >= boneCapacity : !m.boneKeys) &&
            (displayIdentity ? mdl::KeyPoolIdentity(m.displayKeys) == displayIdentity && mdl::KeyPoolCapacity(m.displayKeys) >= displayCapacity : !m.displayKeys) &&
            morphs[morph].panel == rule.panel && morphs[morph].type == rule.type &&
            !std::memcmp(morphs[morph].name, rule.name.data(), rule.name.size());
    }
    bool InitialMarksCurrent() const noexcept {
        const auto& m = *mdl::Mdl(model);
        if (mdl::MorphKeyCapacity(m.morphKeys) != capacity || mdl::BoneKeyCapacity(m.boneKeys) != boneCapacity ||
            mdl::KeyPoolCapacity(m.displayKeys) != displayCapacity) return false;
        std::size_t patch = 0, bone = 0, display = 0;
        for (std::size_t slot = 0; slot < capacity; ++slot) {
            while (patch < patches.size() && patches[patch].key.slot < slot) ++patch;
            const auto mark = patch < patches.size() && patches[patch].key.slot == slot ? patches[patch].key.before.allocated : 0;
            if (m.morphKeys[slot].allocated != mark) return false;
        }
        for (std::size_t slot = 0; slot < boneCapacity; ++slot) {
            const auto mark = bone < boneFlags.size() && boneFlags[bone].slot == slot ? boneFlags[bone++].before : 0;
            if (m.boneKeys[slot].allocated != mark) return false;
        }
        for (std::size_t slot = 0; slot < displayCapacity; ++slot) {
            const auto mark = display < displayFlags.size() && displayFlags[display].slot == slot ? displayFlags[display++].before : 0;
            if (m.displayKeys[slot].allocated != mark) return false;
        }
        return true;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<MorphRegistration> source;
        bool forward, first, consumed = false;
        int frame;
        mdl::MorphKey* pool;
        mdl::BoneKey* bones;
        mdl::DisplayKey* displays;
        std::size_t capacity, boneCapacity, displayCapacity;
        std::vector<mdl::MorphKey> current;
        std::vector<Dependency> dependencies;
        std::vector<unsigned char> morphMarks, boneMarks, displayMarks;
        morph_curve::AuthoredLayers layers;
        std::unique_ptr<mdl::MorphKeyPoolGrowth> growth;
        float beforeValue, afterValue;
        Candidate(std::shared_ptr<MorphRegistration> s, bool f)
            : source(std::move(s)), forward(f), first(!source->published), frame(source->app.CurrentFrame()),
              pool(mdl::Mdl(source->model)->morphKeys), bones(mdl::Mdl(source->model)->boneKeys),
              displays(mdl::Mdl(source->model)->displayKeys), capacity(mdl::MorphKeyCapacity(pool)),
              boneCapacity(mdl::BoneKeyCapacity(bones)), displayCapacity(mdl::KeyPoolCapacity(displays)),
              beforeValue(source->morphs[source->morph].value), afterValue(beforeValue) {}
        const mdl::MorphKey* Target(unsigned slot) const noexcept {
            const auto& patches = source->patches;
            const auto found = std::lower_bound(patches.begin(), patches.end(), slot,
                [](const RegistrationPatch& patch, unsigned index) { return patch.key.slot < index; });
            if (found != patches.end() && found->key.slot == slot && found->data)
                return forward ? &found->key.after : &found->key.before;
            return slot < capacity ? &pool[slot] : nullptr;
        }
        bool IsCurrent() const noexcept override {
            if (consumed || !source->Structure() || first == source->published || !growth || !growth->IsCurrent() ||
                source->app.CurrentFrame() != frame || !SameFloat(beforeValue, source->morphs[source->morph].value)) return false;
            const auto& m = *mdl::Mdl(source->model);
            if (m.morphKeys != pool || m.boneKeys != bones || m.displayKeys != displays ||
                mdl::MorphKeyCapacity(pool) != capacity || mdl::BoneKeyCapacity(bones) != boneCapacity ||
                mdl::KeyPoolCapacity(displays) != displayCapacity || m.searchCursor != source->search ||
                m.maxFrame != (forward ? source->maxBefore : source->maxAfter) ||
                source->app.state.lastRegisteredFrame != (forward ? source->lastBefore : source->lastAfter)) return false;
            for (std::size_t i = 0; i < source->patches.size(); ++i) {
                const auto slot = source->patches[i].key.slot;
                const auto live = slot < capacity ? pool[slot] : mdl::MorphKey{};
                if (!SameKey(current[i], live, true)) return false;
            }
            for (const auto& dependency : dependencies)
                if (!SameKey(dependency.key, pool[dependency.slot], true)) return false;
            for (std::size_t i = 0; i < morphMarks.size(); ++i) if (pool[i].allocated != morphMarks[i]) return false;
            for (std::size_t i = 0; i < boneMarks.size(); ++i) if (bones[i].allocated != boneMarks[i]) return false;
            for (std::size_t i = 0; i < displayMarks.size(); ++i) if (displays[i].allocated != displayMarks[i]) return false;
            return morph_curve::LayersUnchanged(source->model, layers);
        }
        void Publish() noexcept override {
            if (!IsCurrent() || !growth->Publish()) std::terminate();
            auto& m = *mdl::Mdl(source->model);
            // Match display registration's complete selection snapshot over
            // the pools captured by this record. Sparse flags retain nonzero
            // values, but newly selected originally-zero slots must also clear.
            // Later unrelated growth tails remain outside this record; its own
            // newly inserted slot is restored by the data patch below.
            for (std::size_t i = 0; i < source->capacity; ++i) m.morphKeys[i].allocated = 0;
            for (std::size_t i = 0; i < source->boneCapacity; ++i) bones[i].allocated = 0;
            for (std::size_t i = 0; i < source->displayCapacity; ++i) displays[i].allocated = 0;
            for (const auto& patch : source->patches) {
                const auto& target = forward ? patch.key.after : patch.key.before;
                if (patch.data) m.morphKeys[patch.key.slot] = target;
                else m.morphKeys[patch.key.slot].allocated = target.allocated;
            }
            for (const auto& flag : source->boneFlags) bones[flag.slot].allocated = forward ? 0 : flag.before;
            for (const auto& flag : source->displayFlags) displays[flag.slot].allocated = forward ? 0 : flag.before;
            m.maxFrame = forward ? source->maxAfter : source->maxBefore;
            source->app.state.lastRegisteredFrame = forward ? source->lastAfter : source->lastBefore;
            // The current-value registrar never seeks. Subsequent history
            // traversal evaluates only its own channel at the current frame.
            if (!first) source->morphs[source->morph].value = afterValue;
            source->published = true; consumed = true;
            if (!growth->Accept()) std::terminate();
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();
        if (!Structure()) return MMS_E_STALE;
        if (!published && (!forward || app.CurrentFrame() != playheadBefore || !SameFloat(value, morphs[morph].value) || !InitialMarksCurrent()))
            return MMS_E_STALE;
        if (app.CurrentFrame() < 0 || app.CurrentFrame() > frame_limits::maximum) return MMS_E_ARGUMENT;
        return Boundary([&]() -> MmsResult {
            auto& m = *mdl::Mdl(model);
            if (m.maxFrame != (forward ? maxBefore : maxAfter) || m.searchCursor != search ||
                app.state.lastRegisteredFrame != (forward ? lastBefore : lastAfter)) return MMS_E_STALE;
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            Owners owners; candidate->dependencies = Chains(m, owners);
            std::uint64_t needed = count;
            for (const auto& patch : patches) {
                const auto live = patch.key.slot < candidate->capacity ? m.morphKeys[patch.key.slot] : mdl::MorphKey{};
                if (patch.data && !SameKey(live, forward ? patch.key.before : patch.key.after, false)) return MMS_E_STALE;
                candidate->current.push_back(live); needed = (std::max)(needed, std::uint64_t(patch.key.slot) + 1);
            }
            candidate->morphMarks.reserve(candidate->capacity);
            candidate->boneMarks.reserve(candidate->boneCapacity);
            candidate->displayMarks.reserve(candidate->displayCapacity);
            for (std::size_t i = 0; i < candidate->capacity; ++i) candidate->morphMarks.push_back(m.morphKeys[i].allocated);
            for (std::size_t i = 0; i < candidate->boneCapacity; ++i) candidate->boneMarks.push_back(m.boneKeys[i].allocated);
            for (std::size_t i = 0; i < candidate->displayCapacity; ++i) candidate->displayMarks.push_back(m.displayKeys[i].allocated);
            if (!mdl::MorphKeyPoolGrowth::Prepare(m, needed, candidate->growth)) return MMS_E_MEMORY;
            std::string error;
            if (!morph_curve::CaptureLayers(model, candidate->layers, error)) return MMS_E_ARGUMENT;
            morph_curve::NativeKeyView view;
            view.context = candidate.get(); view.capacity = candidate->growth->CandidateCapacity();
            view.key = [](const void* context, std::uint32_t slot) noexcept {
                return static_cast<const Candidate*>(context)->Target(slot);
            };
            if (!candidate->first && !morph_curve::SampleLayersTarget(candidate->layers, view, int(morph),
                candidate->frame, candidate->afterValue, error)) return MMS_E_ARGUMENT;
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t bytes = sizeof(*this) + 4 * sizeof(void*);
        AddBytes(bytes, patches.capacity(), sizeof(RegistrationPatch));
        AddBytes(bytes, boneFlags.capacity(), sizeof(RegistrationFlag));
        AddBytes(bytes, displayFlags.capacity(), sizeof(RegistrationFlag)); return bytes;
    }
};
}
MmsResult PlanMorphRegistration(MMDApp& app, unsigned slot, int morph, int frame,
    std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if (frame < 0 || frame > frame_limits::maximum || morph < 0) return MMS_E_ARGUMENT;
    if (slot >= kModelSlotCount || !app.ModelSlot(slot) || app.PlaybackActive() || app.FrameStepPlayback() ||
        app.state.recordPlaybackStartPending || app.SceneMutationInProgress()) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        const auto& m = *mdl::Mdl(app.ModelSlot(slot));
        if (!m.morphs || unsigned(morph) >= m.morphCount) return MMS_E_ARGUMENT;
        if ((m.physicsMode != 2 && morph == 0) || !mdl::MorphKeyPoolIdentity(m.morphKeys) ||
            (m.boneKeys && !mdl::BoneKeyPoolIdentity(m.boneKeys)) ||
            (m.displayKeys && !mdl::KeyPoolIdentity(m.displayKeys))) return MMS_E_NOT_READY;
        if (!std::isfinite(m.morphs[morph].value)) return MMS_E_ARGUMENT;
        Owners owners; Chains(m, owners);
        const auto capacity = mdl::MorphKeyCapacity(m.morphKeys);
        unsigned current = unsigned(morph);
        while (m.morphKeys[current].frame < unsigned(frame) && m.morphKeys[current].next)
            current = m.morphKeys[current].next; // Chains validated every link.
        if (m.morphKeys[current].frame == unsigned(frame) && SameFloat(m.morphKeys[current].value, m.morphs[morph].value))
            return MMS_OK;
        auto change = std::make_shared<MorphRegistration>(app, slot, unsigned(morph), frame);
        std::map<unsigned, RegistrationPatch> planned;
        const auto write = [&](unsigned index, bool data) -> mdl::MorphKey& {
            auto found = planned.find(index);
            if (found == planned.end()) {
                const auto key = index < capacity ? m.morphKeys[index] : mdl::MorphKey{};
                found = planned.emplace(index, RegistrationPatch{{index, key, key}, data}).first;
            }
            found->second.data |= data; return found->second.key.after;
        };
        for (std::size_t index = 0; index < capacity; ++index)
            if (m.morphKeys[index].allocated) write(unsigned(index), false).allocated = 0;
        if (m.morphKeys[current].frame == unsigned(frame)) {
            auto& key = write(current, true); key.value = change->value; key.allocated = 1;
        } else {
            std::size_t free = m.morphCount;
            while (free < capacity && m.morphKeys[free].frame) ++free;
            if (free >= mdl::kMorphKeyCapacityLimit) return MMS_E_ARGUMENT;
            auto& key = write(unsigned(free), true);
            if (m.morphKeys[current].frame < unsigned(frame)) {
                write(current, true).next = unsigned(free); key.previous = current;
            } else {
                const auto previous = m.morphKeys[current].previous;
                write(previous, true).next = unsigned(free); key.previous = previous;
                write(current, true).previous = unsigned(free); key.next = current;
            }
            key.frame = unsigned(frame); key.value = change->value; key.allocated = 1;
            change->maxAfter = (std::max)(change->maxBefore, unsigned(frame));
        }
        const auto targetCapacity = (std::max)(capacity, std::size_t(planned.rbegin()->first) + 1);
        Owners targetOwners;
        TrackChains(m.morphCount, targetCapacity, [&](unsigned index) {
            const auto found = planned.find(index);
            return found == planned.end() ? m.morphKeys[index] : found->second.key.after;
        }, targetOwners);
        for (const auto& item : planned)
            if (!SameKey(item.second.key.before, item.second.key.after, true)) change->patches.push_back(item.second);
        for (std::size_t index = 0; index < change->boneCapacity; ++index)
            if (m.boneKeys[index].allocated) change->boneFlags.push_back({unsigned(index), m.boneKeys[index].allocated});
        for (std::size_t index = 0; index < change->displayCapacity; ++index)
            if (m.displayKeys[index].allocated) change->displayFlags.push_back({unsigned(index), m.displayKeys[index].allocated});
        change->lastAfter = (std::max)(change->lastBefore, change->maxAfter);
        out = std::move(change); return MMS_OK;
    });
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
MmsResult PlanMorphRangeScale(MMDApp& app, unsigned slot, const BoneRangeScaleOptions& options,
    RangeScaleResult& out) noexcept {
    out = {};
    if (options.start >= options.end || options.end > unsigned(frame_limits::maximum) ||
        !std::isfinite(options.scale) || options.scale <= 0) return MMS_E_ARGUMENT;
    if (slot >= kModelSlotCount || !app.ModelSlot(slot) || app.PlaybackActive() || app.FrameStepPlayback() ||
        app.state.recordPlaybackStartPending || app.SceneMutationInProgress()) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        const auto& model = *mdl::Mdl(app.ModelSlot(slot));
        if (!mdl::MorphKeyPoolIdentity(model.morphKeys)) return MMS_E_NOT_READY;
        const auto capacity = mdl::MorphKeyCapacity(model.morphKeys);
        RangeScaleResult result; result.maxFrame = model.maxFrame; result.searchCursor = model.searchCursor;
        if (!model.morphCount) {
            for (std::size_t index = 0; index < capacity; ++index)
                if (model.morphKeys[index].frame) return MMS_E_ARGUMENT;
            out = std::move(result); return MMS_OK;
        }
        Owners owners; Chains(model, owners);
        struct Copy { unsigned slot, owner, frame; float value; };
        std::vector<Copy> copies; std::set<unsigned> affected;
        for (std::size_t index = 0; index < capacity; ++index) {
            const auto& key = model.morphKeys[index];
            if ((!key.frame && index >= model.morphCount) || key.frame < options.start) continue;
            const auto owner = owners.find(unsigned(index)); if (owner == owners.end()) return MMS_E_ARGUMENT;
            const double scaled = double(key.frame <= options.end ? key.frame - options.start : options.end - options.start) * options.scale;
            // Truncate only after the bound check, without legacy uint32 wrap.
            if (!std::isfinite(scaled) || scaled < 0 || scaled > double(frame_limits::maximum)) return MMS_E_ARGUMENT;
            const auto target = std::uint64_t(options.start) + std::uint64_t(scaled) +
                (key.frame > options.end ? std::uint64_t(key.frame - options.end) : 0);
            if (target > std::uint64_t(frame_limits::maximum)) return MMS_E_ARGUMENT;
            result.moved |= target != key.frame;
            copies.push_back({unsigned(index), owner->second, unsigned(target), key.value}); affected.insert(owner->second);
        }
        result.hasKeys = !copies.empty();
        if (!result.hasKeys) { out = std::move(result); return MMS_OK; }
        std::map<unsigned, Patch> planned;
        const auto read = [&](unsigned index) -> const mdl::MorphKey& {
            if (index >= capacity) throw std::invalid_argument("morph range read outside pool");
            const auto found = planned.find(index);
            return found == planned.end() ? model.morphKeys[index] : found->second.after;
        };
        const auto write = [&](unsigned index) -> mdl::MorphKey& {
            if (index >= capacity) throw std::invalid_argument("morph range write outside pool");
            auto found = planned.find(index);
            if (found == planned.end()) {
                const auto key = model.morphKeys[index]; found = planned.emplace(index, Patch{index, key, key}).first;
            }
            return found->second.after;
        };
        for (std::size_t index = 0; index < capacity; ++index)
            if (model.morphKeys[index].allocated) write(unsigned(index)).allocated = 0;
        // Deletion follows the ascending original pool slots. Adjacent removals
        // use links changed by previous removals, including root-zero sentinel.
        for (const auto& copy : copies) {
            auto& key = write(copy.slot); const auto previous = key.previous, next = key.next;
            key.frame = 0;
            if (copy.slot >= model.morphCount) { write(previous).next = next; write(next).previous = previous; }
            key.allocated = 0; key.value = 0;
            if (copy.slot >= model.morphCount) key.next = 0;
            key.previous = 0;
        }
        unsigned cursor = model.morphCount;
        while (cursor < capacity && read(cursor).frame) ++cursor;
        const auto allocate = [&]() -> unsigned {
            if (cursor < model.morphCount || cursor >= capacity) cursor = model.morphCount;
            while (cursor < capacity && read(cursor).frame) ++cursor;
            return cursor;
        };
        for (const auto& copy : copies) {
            unsigned current = copy.owner; std::size_t hops = 0;
            while (read(current).frame < copy.frame && read(current).next) {
                if (++hops >= capacity) throw std::invalid_argument("cyclic private morph range track");
                current = read(current).next;
            }
            if (read(current).frame == copy.frame) {
                auto& key = write(current); key.value = copy.value; key.allocated = 1;
                continue; // The native overwrite branch does not raise maxFrame.
            }
            const auto fresh = allocate();
            // Retime cannot require more live non-root keys than it removed.
            // Reject corrupt/full storage rather than allocate during publish.
            if (fresh >= capacity) return MMS_E_NOT_READY;
            auto& key = write(fresh);
            if (read(current).frame < copy.frame) {
                write(current).next = fresh; key.previous = current;
                // Preserve the old registrar's append behavior and reserved bytes.
            } else {
                const auto previous = read(current).previous;
                write(previous).next = fresh; key.previous = previous;
                write(current).previous = fresh; key.next = current;
            }
            key.frame = copy.frame; key.value = copy.value; key.allocated = 1;
            result.maxFrame = (std::max)(result.maxFrame, copy.frame);
        }
        Owners targetOwners; TrackChains(model.morphCount, capacity, read, targetOwners);
        auto change = std::make_shared<MorphDeletion>(app, slot); change->patches.reserve(planned.size());
        for (const auto& patch : planned) {
            if (!SameKey(patch.second.before, patch.second.after, true)) change->patches.push_back(patch.second);
        }
        change->rules.reserve(affected.size());
        for (auto morph : affected) {
            const auto& record = model.morphs[morph]; Rule rule{morph, {}, record.panel, record.type};
            std::memcpy(rule.name.data(), record.name, rule.name.size()); change->rules.push_back(rule);
        }
        for (std::size_t index = 0; index < capacity; ++index)
            if (read(unsigned(index)).allocated) result.markedSlots.push_back(unsigned(index));
        result.searchCursor = int(cursor); result.change = std::move(change);
        out = std::move(result); return MMS_OK;
    });
}
}
#endif
