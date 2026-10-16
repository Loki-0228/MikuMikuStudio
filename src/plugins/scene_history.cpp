#include "scene_history.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include <array>

namespace mikudancestudio::plugins {
namespace {
enum class Field {
    ModelOrder, ModelOrder2, ModelParent, ModelBone, SelectorParent, SelectorBone,
    CameraParent, CameraBone, CameraKeyParent, CameraKeyBone,
    AccessoryParent, AccessoryBone, AccessoryKeyParent, AccessoryKeyBone,
    AccessoryKeyVisible, AccessoryOrder, AccessorySplit
};
// History stores logical locations. Only a prepared candidate holds addresses,
// and it invalidates them if a pool moves before publication.
struct Patch {
    Field field;
    unsigned slot;
    size_t key;
    uint32_t element;
    std::array<unsigned char, 4> before{}, after{};
    size_t bytes;
    template<class T> Patch(Field f, unsigned s, size_t k, uint32_t e, T value, T next)
        : field(f), slot(s), key(k), element(e), bytes(sizeof(T)) {
        static_assert(std::is_integral_v<T> && sizeof(T) <= 4);
        std::memcpy(before.data(), &value, bytes); std::memcpy(after.data(), &next, bytes);
    }
    void* Resolve(MMDApp& app) const noexcept {
        switch (field) {
        case Field::ModelOrder: return &mdl::Mdl(app.ModelSlot(slot))->comboSelIndex;
        case Field::ModelOrder2: return &mdl::Mdl(app.ModelSlot(slot))->comboSelIndex2;
        case Field::ModelParent: return &mdl::BoneOrder(app.ModelSlot(slot))[key].linkedModel;
        case Field::ModelBone: return &mdl::BoneOrder(app.ModelSlot(slot))[key].linkedBone;
        case Field::SelectorParent: return &mdl::SelectorStates(mdl::Mdl(app.ModelSlot(slot))->displayKeys[key])[element].modelIndex;
        case Field::SelectorBone: return &mdl::SelectorStates(mdl::Mdl(app.ModelSlot(slot))->displayKeys[key])[element].boneIndex;
        case Field::CameraParent: return &app.CameraParentModel();
        case Field::CameraBone: return &app.CameraParentBone();
        case Field::CameraKeyParent: return &app.CameraKeys()[key].parentModel;
        case Field::CameraKeyBone: return &app.CameraKeys()[key].parentBone;
        case Field::AccessoryParent: return &app.AccessorySlot(slot)->parentModel;
        case Field::AccessoryBone: return &app.AccessorySlot(slot)->parentBone;
        case Field::AccessoryKeyParent: return &app.AccessoryKeys(slot)[key].parentModel;
        case Field::AccessoryKeyBone: return &app.AccessoryKeys(slot)[key].parentBone;
        case Field::AccessoryKeyVisible: return &app.AccessoryKeys(slot)[key].visible;
        case Field::AccessoryOrder: return &app.AccessorySlot(slot)->order;
        case Field::AccessorySplit: return &app.AccessoryRenderSplitOrder();
        }
        return nullptr;
    }
    bool Current(const void* address, bool forward) const noexcept {
        return std::memcmp(address, (forward ? before : after).data(), bytes) == 0;
    }
    void Publish(void* address, bool forward) const noexcept {
        std::memcpy(address, (forward ? after : before).data(), bytes);
    }
};
template<class Key> struct PoolGuard {
    const Key* pointer;
    size_t capacity;
    uint64_t identity;
    explicit PoolGuard(const Key* pool = nullptr)
        : pointer(pool), capacity(mdl::KeyPoolCapacity(pool)), identity(mdl::KeyPoolIdentity(pool)) {}
    bool Current(const Key* pool) const noexcept {
        if (identity)
            return mdl::KeyPoolIdentity(pool) == identity && mdl::KeyPoolCapacity(pool) >= capacity;
        return pool == pointer && !mdl::KeyPoolIdentity(pool) && mdl::KeyPoolCapacity(pool) == capacity;
    }
};
struct ModelGuard {
    unsigned slot;
    const unsigned char* model;
    MmsId id;
    const mdl::BoneOrderEntry* order;
    uint32_t count;
    PoolGuard<mdl::DisplayKey> displays;
    std::vector<std::pair<size_t, const mdl::BoneReference*>> selectors;
    bool Current(MMDApp& app) const noexcept {
        if (app.ModelSlot(slot) != model || !SceneObjectPresent(model, id)) return false;
        const auto& m = *mdl::Mdl(model);
        if (m.boneOrderTable != order || m.boneOrderCount != count || !displays.Current(m.displayKeys)) return false;
        for (const auto& [index, pointer] : selectors)
            if (mdl::SelectorStates(m.displayKeys[index]) != pointer) return false;
        return true;
    }
};
struct AccessoryGuard {
    unsigned slot;
    const mdl::AccessoryRecord* object;
    MmsId id;
    PoolGuard<mdl::AccessoryKey> keys;
    bool Current(MMDApp& app) const noexcept {
        return app.AccessorySlot(slot) == object && SceneObjectPresent(object, id) && keys.Current(app.AccessoryKeys(slot));
    }
};
class BindingChange final : public HistoryChange, public std::enable_shared_from_this<BindingChange> {
public:
    MMDApp& app;
    std::vector<Patch> patches;
    std::vector<ModelGuard> models;
    std::vector<AccessoryGuard> accessories;
    PoolGuard<mdl::CameraKey> camera;
    bool guardCamera = false;
    explicit BindingChange(MMDApp& a) : app(a) {}
    bool OwnersCurrent() const noexcept {
        if (guardCamera && !camera.Current(app.CameraKeys())) return false;
        // Validate ownership and nested allocations before resolving a field.
        for (const auto& guard : models) if (!guard.Current(app)) return false;
        for (const auto& guard : accessories) if (!guard.Current(app)) return false;
        return true;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<BindingChange> owner;
        bool forward, consumed = false;
        std::vector<void*> addresses;
        Candidate(std::shared_ptr<BindingChange> value, bool direction) : owner(std::move(value)), forward(direction) {
            addresses.reserve(owner->patches.size());
            for (const auto& patch : owner->patches) addresses.push_back(patch.Resolve(owner->app));
        }
        bool IsCurrent() const noexcept override {
            if (consumed || !owner->OwnersCurrent()) return false;
            for (size_t i = 0; i < addresses.size(); ++i) {
                const auto& patch = owner->patches[i];
                if (patch.Resolve(owner->app) != addresses[i] || !patch.Current(addresses[i], forward)) return false;
            }
            return true;
        }
        void Publish() noexcept override {
            for (size_t i = 0; i < addresses.size(); ++i) owner->patches[i].Publish(addresses[i], forward);
            consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        if (!OwnersCurrent()) return MMS_E_STALE;
        auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
        if (!candidate->IsCurrent()) return MMS_E_STALE;
        out = std::move(candidate); return MMS_OK;
    }
    uint64_t RetainedBytes() const noexcept override {
        uint64_t size = sizeof(*this) + patches.capacity() * sizeof(Patch) + models.capacity() * sizeof(ModelGuard) + accessories.capacity() * sizeof(AccessoryGuard);
        for (const auto& model : models) size += model.selectors.capacity() * sizeof(model.selectors.front());
        return size;
    }
    template<class T> void Set(Field field, unsigned slot, size_t key, uint32_t element, T value, T next) {
        if (value != next) patches.emplace_back(field, slot, key, element, value, next);
    }
};
}
std::shared_ptr<HistoryChange> CaptureObjectsRemovalBindings(MMDApp& app,
    const std::vector<unsigned>& modelSlots, const std::vector<unsigned>& accessorySlots) {
    if (modelSlots.empty() && accessorySlots.empty()) throw std::invalid_argument("empty object removal");
    std::array<bool, kModelSlotCount> removedModels{};
    std::array<bool, 255> removedAccessories{};
    for (auto slot : modelSlots) {
        if (slot >= removedModels.size() || removedModels[slot] || !app.ModelSlot(slot))
            throw std::invalid_argument("deleted model slot");
        removedModels[slot] = true;
    }
    for (auto slot : accessorySlots) {
        if (slot >= removedAccessories.size() || removedAccessories[slot] || !app.AccessorySlot(slot))
            throw std::invalid_argument("deleted accessory slot");
        removedAccessories[slot] = true;
    }
    const auto removesModel = [&](int slot) { return slot >= 0 && size_t(slot) < removedModels.size() && removedModels[slot]; };
    auto change = std::make_shared<BindingChange>(app);
    for (unsigned i = 0; i < kModelSlotCount; ++i) {
        if (modelSlots.empty()) break;
        auto* pointer = app.ModelSlot(i); if (!pointer || removedModels[i]) continue;
        auto& model = *mdl::Mdl(pointer);
        if (model.boneOrderCount && !model.boneOrderTable) throw std::invalid_argument("model parent table");
        const auto begin = change->patches.size();
        ModelGuard guard{i, pointer, SceneObjectIdentity(pointer).id, mdl::BoneOrder(pointer),
            model.boneOrderCount, PoolGuard<mdl::DisplayKey>(model.displayKeys), {}};
        unsigned earlier = 0, earlier2 = 0;
        for (auto slot : modelSlots) {
            const auto& deleted = *mdl::Mdl(app.ModelSlot(slot));
            earlier += deleted.comboSelIndex < model.comboSelIndex;
            earlier2 += deleted.comboSelIndex2 < model.comboSelIndex2;
        }
        if (earlier > model.comboSelIndex || earlier2 > model.comboSelIndex2) throw std::invalid_argument("model removal order");
        change->Set(Field::ModelOrder, i, 0, 0, model.comboSelIndex, uint8_t(model.comboSelIndex - earlier));
        change->Set(Field::ModelOrder2, i, 0, 0, model.comboSelIndex2, uint8_t(model.comboSelIndex2 - earlier2));
        for (uint32_t n = 0; n < model.boneOrderCount; ++n) {
            auto& order = mdl::BoneOrder(pointer)[n];
            if (removesModel(order.linkedModel)) {
                change->Set(Field::ModelParent, i, n, 0, order.linkedModel, int32_t(-1));
                change->Set(Field::ModelBone, i, n, 0, order.linkedBone, int32_t(0));
            }
        }
        for (size_t n = 0; n < guard.displays.capacity; ++n) {
            auto* states = mdl::SelectorStates(model.displayKeys[n]);
            if (!states) continue;
            const auto first = change->patches.size();
            for (uint32_t bone = 0; bone < model.boneOrderCount; ++bone) {
                if (removesModel(states[bone].modelIndex)) {
                    change->Set(Field::SelectorParent, i, n, bone, states[bone].modelIndex, int32_t(-1));
                    change->Set(Field::SelectorBone, i, n, bone, states[bone].boneIndex, int32_t(0));
                }
            }
            if (change->patches.size() != first) guard.selectors.emplace_back(n, states);
        }
        if (change->patches.size() != begin) change->models.push_back(std::move(guard));
    }
    if (removesModel(app.CameraParentModel())) {
        change->Set(Field::CameraParent, 0, 0, 0, app.CameraParentModel(), int32_t(-1));
        change->Set(Field::CameraBone, 0, 0, 0, app.CameraParentBone(), int32_t(0));
    }
    if (!modelSlots.empty()) change->camera = PoolGuard<mdl::CameraKey>(app.CameraKeys());
    for (size_t i = 0; i < change->camera.capacity; ++i) {
        auto& key = app.CameraKeys()[i];
        if (removesModel(key.parentModel)) {
            change->guardCamera = true;
            change->Set(Field::CameraKeyParent, 0, i, 0, key.parentModel, int32_t(-1));
            change->Set(Field::CameraKeyBone, 0, i, 0, key.parentBone, int32_t(0));
        }
    }
    unsigned beforeSplit = 0;
    for (auto slot : accessorySlots) beforeSplit += app.AccessorySlot(slot)->order < app.AccessoryRenderSplitOrder();
    change->Set(Field::AccessorySplit, 0, 0, 0, app.AccessoryRenderSplitOrder(),
        int32_t(app.AccessoryRenderSplitOrder() - int32_t(beforeSplit)));
    for (unsigned i = 0; i < 255; ++i) {
        auto* object = app.AccessorySlot(i); if (!object || removedAccessories[i]) continue;
        const auto begin = change->patches.size();
        unsigned earlier = 0;
        for (auto slot : accessorySlots) earlier += app.AccessorySlot(slot)->order < object->order;
        if (earlier > object->order) throw std::invalid_argument("accessory removal order");
        change->Set(Field::AccessoryOrder, i, 0, 0, object->order, uint8_t(object->order - earlier));
        if (removesModel(object->parentModel)) {
            change->Set(Field::AccessoryParent, i, 0, 0, object->parentModel, int32_t(-1));
            change->Set(Field::AccessoryBone, i, 0, 0, object->parentBone, int32_t(0));
        }
        auto* keys = app.AccessoryKeys(i); const auto count = mdl::KeyPoolCapacity(keys);
        for (size_t n = 0; !modelSlots.empty() && n < count; ++n) {
            auto& key = keys[n];
            if (removesModel(key.parentModel)) {
                change->Set(Field::AccessoryKeyParent, i, n, 0, key.parentModel, int32_t(-1));
                change->Set(Field::AccessoryKeyBone, i, n, 0, key.parentBone, int32_t(0));
                change->Set(Field::AccessoryKeyVisible, i, n, 0, key.visible, uint8_t(0));
            }
        }
        if (change->patches.size() != begin)
            change->accessories.push_back({i, object, SceneObjectIdentity(object).id, PoolGuard<mdl::AccessoryKey>(keys)});
    }
    return change;
}
std::shared_ptr<HistoryChange> CaptureModelRemovalBindings(MMDApp& app, unsigned slot) {
    return CaptureObjectsRemovalBindings(app, {slot}, {});
}
std::shared_ptr<HistoryChange> CaptureAccessoryRemovalOrder(MMDApp& app, unsigned slot) {
    return CaptureObjectsRemovalBindings(app, {}, {slot});
}
}
