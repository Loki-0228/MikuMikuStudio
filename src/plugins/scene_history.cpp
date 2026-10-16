#include "scene_history.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include <array>

namespace mikudancestudio::plugins {
namespace {
struct Patch {
    void* address;
    std::array<unsigned char, 4> before{}, after{};
    size_t bytes;
    template<class T> Patch(T& value, T next) : address(&value), bytes(sizeof(T)) {
        static_assert(std::is_integral_v<T> && sizeof(T) <= 4);
        std::memcpy(before.data(), &value, bytes); std::memcpy(after.data(), &next, bytes);
    }
    bool Current(bool forward) const noexcept {
        return std::memcmp(address, (forward ? before : after).data(), bytes) == 0;
    }
    void Publish(bool forward) const noexcept {
        std::memcpy(address, (forward ? after : before).data(), bytes);
    }
};
struct ModelGuard {
    unsigned slot;
    const unsigned char* model;
    MmsId id;
    const mdl::BoneOrderEntry* order;
    uint32_t count;
    const mdl::DisplayKey* displays;
    size_t capacity;
    std::vector<std::pair<size_t, const mdl::BoneReference*>> selectors;
    bool Current(MMDApp& app) const noexcept {
        if (app.ModelSlot(slot) != model || !SceneObjectPresent(model, id)) return false;
        const auto& m = *mdl::Mdl(model);
        if (m.boneOrderTable != order || m.boneOrderCount != count || m.displayKeys != displays ||
            mdl::KeyPoolCapacity(displays) != capacity) return false;
        for (const auto& [index, pointer] : selectors)
            if (mdl::SelectorStates(displays[index]) != pointer) return false;
        return true;
    }
};
struct AccessoryGuard {
    unsigned slot;
    const mdl::AccessoryRecord* object;
    MmsId id;
    const mdl::AccessoryKey* keys;
    size_t capacity;
    bool Current(MMDApp& app) const noexcept {
        return app.AccessorySlot(slot) == object && SceneObjectPresent(object, id) &&
            app.AccessoryKeys(slot) == keys && mdl::KeyPoolCapacity(keys) == capacity;
    }
};
class BindingChange final : public HistoryChange, public std::enable_shared_from_this<BindingChange> {
public:
    MMDApp& app;
    std::vector<Patch> patches;
    std::vector<ModelGuard> models;
    std::vector<AccessoryGuard> accessories;
    const mdl::CameraKey* camera = nullptr;
    size_t cameraCapacity = 0;
    bool guardCamera = false;
    explicit BindingChange(MMDApp& a) : app(a) {}
    bool Current(bool forward) const noexcept {
        if (guardCamera && (app.CameraKeys() != camera || mdl::KeyPoolCapacity(camera) != cameraCapacity)) return false;
        // Check ownership and nested allocations before dereferencing patches.
        for (const auto& guard : models) if (!guard.Current(app)) return false;
        for (const auto& guard : accessories) if (!guard.Current(app)) return false;
        for (const auto& patch : patches) if (!patch.Current(forward)) return false;
        return true;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<BindingChange> owner; bool forward;
        Candidate(std::shared_ptr<BindingChange> value, bool direction) : owner(std::move(value)), forward(direction) {}
        bool IsCurrent() const noexcept override { return owner->Current(forward); }
        void Publish() noexcept override { for (const auto& patch : owner->patches) patch.Publish(forward); }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        if (!Current(forward)) return MMS_E_STALE;
        out = std::make_unique<Candidate>(shared_from_this(), forward); return MMS_OK;
    }
    uint64_t RetainedBytes() const noexcept override {
        uint64_t size = sizeof(*this) + patches.capacity() * sizeof(Patch) + models.capacity() * sizeof(ModelGuard) + accessories.capacity() * sizeof(AccessoryGuard);
        for (const auto& model : models) size += model.selectors.capacity() * sizeof(model.selectors.front());
        return size;
    }
    template<class T> void Set(T& value, T next) { if (value != next) patches.emplace_back(value, next); }
};
}
std::shared_ptr<HistoryChange> CaptureModelRemovalBindings(MMDApp& app, unsigned slot) {
    if (slot >= kModelSlotCount || !app.ModelSlot(slot)) throw std::invalid_argument("deleted model slot");
    auto change = std::make_shared<BindingChange>(app);
    const auto& deleted = *mdl::Mdl(app.ModelSlot(slot));
    for (unsigned i = 0; i < kModelSlotCount; ++i) {
        auto* pointer = app.ModelSlot(i); if (!pointer || i == slot) continue;
        auto& model = *mdl::Mdl(pointer);
        if (model.boneOrderCount && !model.boneOrderTable)
            throw std::invalid_argument("model parent table");
        const auto begin = change->patches.size();
        ModelGuard guard{i, pointer, SceneObjectIdentity(pointer).id, mdl::BoneOrder(pointer),
            model.boneOrderCount, model.displayKeys, mdl::KeyPoolCapacity(model.displayKeys), {}};
        if (model.comboSelIndex > deleted.comboSelIndex) change->Set(model.comboSelIndex, uint8_t(model.comboSelIndex - 1));
        if (model.comboSelIndex2 > deleted.comboSelIndex2) change->Set(model.comboSelIndex2, uint8_t(model.comboSelIndex2 - 1));
        for (uint32_t n = 0; n < model.boneOrderCount; ++n) {
            auto& order = mdl::BoneOrder(pointer)[n];
            if (order.linkedModel == int(slot)) { change->Set(order.linkedModel, int32_t(-1)); change->Set(order.linkedBone, int32_t(0)); }
        }
        for (size_t n = 0; n < guard.capacity; ++n) {
            auto* states = mdl::SelectorStates(model.displayKeys[n]);
            if (!states) continue; // unused records may have no selector array
            const auto first = change->patches.size();
            for (uint32_t bone = 0; bone < model.boneOrderCount; ++bone) {
                if (states[bone].modelIndex == int(slot)) {
                    change->Set(states[bone].modelIndex, int32_t(-1)); change->Set(states[bone].boneIndex, int32_t(0));
                }
            }
            if (change->patches.size() != first) guard.selectors.emplace_back(n, states);
        }
        if (change->patches.size() != begin) change->models.push_back(std::move(guard));
    }
    if (app.CameraParentModel() == int(slot)) {
        change->Set(app.CameraParentModel(), int32_t(-1)); change->Set(app.CameraParentBone(), int32_t(0));
    }
    change->camera = app.CameraKeys(); change->cameraCapacity = mdl::KeyPoolCapacity(change->camera);
    for (size_t i = 0; i < change->cameraCapacity; ++i) {
        auto& key = app.CameraKeys()[i];
        if (key.parentModel == int(slot)) {
            change->guardCamera = true;
            change->Set(key.parentModel, int32_t(-1)); change->Set(key.parentBone, int32_t(0));
        }
    }
    for (unsigned i = 0; i < 255; ++i) {
        auto* object = app.AccessorySlot(i); if (!object) continue;
        const auto begin = change->patches.size();
        if (object->parentModel == int(slot)) {
            change->Set(object->parentModel, int32_t(-1)); change->Set(object->parentBone, int32_t(0));
        }
        auto* keys = app.AccessoryKeys(i); const auto count = mdl::KeyPoolCapacity(keys);
        for (size_t n = 0; n < count; ++n) {
            auto& key = keys[n];
            if (key.parentModel == int(slot)) {
                change->Set(key.parentModel, int32_t(-1)); change->Set(key.parentBone, int32_t(0));
                change->Set(key.visible, uint8_t(0));
            }
        }
        if (change->patches.size() != begin)
            change->accessories.push_back({i, object, SceneObjectIdentity(object).id, keys, count});
    }
    return change;
}
std::shared_ptr<HistoryChange> CaptureAccessoryRemovalOrder(MMDApp& app, unsigned slot) {
    if (slot >= 255 || !app.AccessorySlot(slot)) throw std::invalid_argument("deleted accessory slot");
    auto change = std::make_shared<BindingChange>(app); const auto order = app.AccessorySlot(slot)->order;
    if (order < app.AccessoryRenderSplitOrder())
        change->Set(app.AccessoryRenderSplitOrder(), int32_t(app.AccessoryRenderSplitOrder() - 1));
    for (unsigned i = 0; i < 255; ++i) {
        auto* object = app.AccessorySlot(i); if (!object || i == slot || object->order <= order) continue;
        change->Set(object->order, uint8_t(object->order - 1));
        auto* keys = app.AccessoryKeys(i);
        change->accessories.push_back({i, object, SceneObjectIdentity(object).id, keys, mdl::KeyPoolCapacity(keys)});
    }
    return change;
}
}
