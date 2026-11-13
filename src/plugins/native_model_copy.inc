// Include at file scope at the end of native_model_keys.cpp.
#include "native_model_copy.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/app_layout.hpp"
#include "mikumikustudio/frame_clipboard.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace mikumikustudio::plugins {
namespace {
namespace model_copy_detail {
static_assert(kModelSlotCount <= 255, "model copy reference capacity");

bool SameIdentity(const MmsObjectRef& a, const MmsObjectRef& b) noexcept {
    return a.id.lo == b.id.lo && a.id.hi == b.id.hi && a.generation == b.generation;
}

void Model(const MMDApp& app, ModelCopySnapshot& snapshot, int slot, bool tracks = false) noexcept {
    if (slot < 0 || slot >= kModelSlotCount) {
        snapshot.referencesAvailable = false;
        return;
    }
    auto& ref = snapshot.models[slot];
    ref.tracks |= tracks;
    if (ref.referenced) return;
    ref.referenced = true;
    ref.slot = slot;
    ref.pointer = app.ModelSlot(slot);
    ref.available = ref.pointer && FindSceneObjectIdentity(ref.pointer, ref.identity);
    if (!ref.available) {
        snapshot.referencesAvailable = false;
        return;
    }
    // The pointer came from a live slot and has an existing identity.
    const auto& model = *reinterpret_cast<const mdl::ModelRecord*>(ref.pointer);
    ref.bones = model.boneTable; ref.boneCount = model.boneCount;
    ref.morphs = model.morphs; ref.morphCount = model.morphCount;
    ref.ikChains = model.ikChains; ref.ikCount = model.ikChainCount;
    ref.selectors = model.boneOrderTable; ref.selectorCount = model.boneOrderCount;
}

template<class Entry, class Metadata>
bool Name(const Entry* table, std::uint32_t count, int index, Metadata& metadata) noexcept {
    if (!table || index < 0 || std::uint32_t(index) >= count) return false;
    std::copy_n(table[index].name, metadata.name.size(), metadata.name.begin());
    // Ordinal in the complete source table, including unselected predecessors.
    for (int i = 0; i < index; ++i)
        if (!std::strncmp(table[i].name, metadata.name.data(), metadata.name.size())) ++metadata.occurrence;
    return true;
}

ModelCopyTrack Track(const MMDApp& app, const ModelCopyModelReference& ref,
                     std::size_t index, bool morph) {
    ModelCopyTrack track;
    if (!ref.available) return track;
    const auto& model = *reinterpret_cast<const mdl::ModelRecord*>(ref.pointer);
    track.index = morph ? frame_clipboard::MorphTarget(&app, ref.pointer, index) :
                         frame_clipboard::BoneTarget(&app, ref.pointer, index);
    // In particular, -1 must remain unavailable, never guess the first name.
    track.available = morph ? Name(model.morphs, model.morphCount, track.index, track) :
                              Name(model.boneTable, model.boneCount, track.index, track);
    return track;
}

ModelCopyMapping Mapping(const ModelCopyModelReference& ref, std::size_t index, bool selector) noexcept {
    ModelCopyMapping mapping;
    mapping.tableIndex = std::uint32_t(index);
    if (!ref.available) return mapping;
    const auto& model = *reinterpret_cast<const mdl::ModelRecord*>(ref.pointer);
    if (selector) {
        if (!model.boneOrderTable || index >= model.boneOrderCount) return mapping;
        mapping.mappedBone = static_cast<const mdl::BoneOrderEntry*>(model.boneOrderTable)[index].boneIndex;
        if (!index && mapping.mappedBone == -1) {
            mapping.available = true;
            return mapping;
        }
    } else {
        if (!model.ikChains || index >= model.ikChainCount) return mapping;
        mapping.mappedBone = model.ikChains[index].boneIndex;
    }
    mapping.available = Name(model.boneTable, model.boneCount, mapping.mappedBone, mapping);
    return mapping;
}

bool ReferenceCurrent(const MMDApp& app, const ModelCopyModelReference& ref) noexcept {
    if (!ref.available || !ref.pointer || ref.slot < 0 || ref.slot >= kModelSlotCount ||
        app.ModelSlot(ref.slot) != ref.pointer) return false;
    MmsObjectRef identity{};
    if (!FindSceneObjectIdentity(ref.pointer, identity) || !SameIdentity(identity, ref.identity)) return false;
    // Slot and identity are checked before dereferencing borrowed storage.
    const auto& model = *reinterpret_cast<const mdl::ModelRecord*>(ref.pointer);
    return model.boneTable == ref.bones && model.boneCount == ref.boneCount &&
        (!ref.tracks || (model.morphs == ref.morphs && model.morphCount == ref.morphCount &&
         model.ikChains == ref.ikChains && model.ikChainCount == ref.ikCount &&
         model.boneOrderTable == ref.selectors && model.boneOrderCount == ref.selectorCount));
}
}
}

MmsResult CaptureModelCopySnapshot(MMDApp& app, std::uint32_t nativeFirst,
    std::shared_ptr<const ModelCopySnapshot>& out) noexcept {
    out.reset();
    const auto slot = app.SelectedModelSlot();
    if (slot >= kModelSlotCount || !app.ModelSlot(slot)) return MMS_E_NOT_READY;
    try {
        auto snapshot = std::make_shared<ModelCopySnapshot>();
        snapshot->owner = &app;
        snapshot->sceneGeneration = app.SceneGeneration();
        snapshot->nativeFirst = nativeFirst;
        snapshot->modelSlot = int(slot);
        if (const auto result = CaptureModelPastePayload(app, snapshot->payload)) return result;
        model_copy_detail::Model(app, *snapshot, int(slot), true);
        const auto& ref = snapshot->models[slot];
        snapshot->bones.reserve(snapshot->payload.bones.size());
        for (std::size_t i = 0; i < snapshot->payload.bones.size(); ++i) {
            auto track = model_copy_detail::Track(app, ref, i, false);
            snapshot->referencesAvailable &= track.available;
            snapshot->bones.push_back(track);
        }
        snapshot->morphs.reserve(snapshot->payload.morphs.size());
        for (std::size_t i = 0; i < snapshot->payload.morphs.size(); ++i) {
            auto track = model_copy_detail::Track(app, ref, i, true);
            snapshot->referencesAvailable &= track.available;
            snapshot->morphs.push_back(track);
        }
        snapshot->displays.reserve(snapshot->payload.displays.size());
        for (const auto& display : snapshot->payload.displays) {
            ModelCopyDisplaySource metadata;
            metadata.ik.reserve(display.ik.size());
            for (std::size_t i = 0; i < display.ik.size(); ++i) {
                auto mapping = model_copy_detail::Mapping(ref, i, false);
                snapshot->referencesAvailable &= mapping.available;
                metadata.ik.push_back(mapping);
            }
            metadata.selectors.reserve(display.selectors.size());
            for (std::size_t i = 0; i < display.selectors.size(); ++i) {
                // Parent references belong to the raw payload even if the
                // source identity/table/mapping is unavailable.
                const int parent = display.selectors[i].modelIndex;
                if (parent != -1) model_copy_detail::Model(app, *snapshot, parent);
                auto mapping = model_copy_detail::Mapping(ref, i, true);
                snapshot->referencesAvailable &= mapping.available;
                metadata.selectors.push_back(mapping);
            }
            snapshot->displays.push_back(std::move(metadata));
        }
        if (ref.available) {
            std::vector<int> bones, morphs;
            const auto selected = [](const auto& tracks, auto& indices) {
                for (const auto& track : tracks)
                    if (track.available && std::find(indices.begin(), indices.end(), track.index) == indices.end())
                        indices.push_back(track.index);
            };
            selected(snapshot->bones, bones); selected(snapshot->morphs, morphs);
            std::string error;
            if ((!bones.empty() && !bone_curve::CaptureCopyAuthoring(ref.pointer, bones, snapshot->boneAuthoring, error)) ||
                (!morphs.empty() && !morph_curve::CaptureCopyAuthoring(ref.pointer, morphs, snapshot->morphAuthoring, error)))
                return MMS_E_ARGUMENT;
        }
        // No mutable alias escapes; conversion/publication does not allocate.
        out = std::move(snapshot);
        return MMS_OK;
    } catch (const std::bad_alloc&) {
        return MMS_E_MEMORY;
    } catch (...) {
        return MMS_E_FAILED;
    }
}

bool ModelCopySnapshotCurrent(const MMDApp& app, const ModelCopySnapshot& snapshot) noexcept {
    if (snapshot.owner != &app || snapshot.sceneGeneration != app.SceneGeneration() ||
        !snapshot.referencesAvailable || snapshot.modelSlot < 0 || snapshot.modelSlot >= kModelSlotCount) return false;
    const auto& source = snapshot.models[snapshot.modelSlot];
    if (!source.referenced || !source.tracks || source.slot != snapshot.modelSlot) return false;
    for (const auto& ref : snapshot.models)
        if (ref.referenced && !model_copy_detail::ReferenceCurrent(app, ref)) return false;
    const auto& model = *reinterpret_cast<const mdl::ModelRecord*>(source.pointer);
    // Only copy-time mapping dependencies are read, never current key values,
    // parent-bone values, names, selection or mutable clipboard buffers.
    for (const auto& display : snapshot.displays) {
        for (const auto& mapping : display.ik)
            if (!mapping.available || !model.ikChains || mapping.tableIndex >= model.ikChainCount ||
                model.ikChains[mapping.tableIndex].boneIndex != mapping.mappedBone) return false;
        for (const auto& mapping : display.selectors)
            if (!mapping.available || !model.boneOrderTable || mapping.tableIndex >= model.boneOrderCount ||
                static_cast<const mdl::BoneOrderEntry*>(model.boneOrderTable)[mapping.tableIndex].boneIndex != mapping.mappedBone)
                return false;
    }
    return true;
}

}
#endif
