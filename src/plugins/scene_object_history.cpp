#include "scene_object_history.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/dynamics_world.hpp"
#include "mikudancestudio/history_physics.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/physics_scale.hpp"
#include "mikudancestudio/physics_scene.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "BulletDynamics/ConstraintSolver/btTypedConstraint.h"
#include <map>

namespace mikudancestudio {
void DeleteModel(unsigned char*, int);
void DeleteAccessory(mdl::AccessoryRecord*, int);
}
namespace mikudancestudio::plugins {
namespace {
using Key = std::pair<MMDApp*, const void*>;
std::map<Key, std::weak_ptr<RetainedSceneObject>> retainedObjects;
}
struct RetainedSceneObject::Impl {
    MMDApp& app;
    uint32_t kind;
    unsigned slot;
    void* object;
    MmsId id;
    bool present = true;
    std::unique_ptr<history_physics::Retained> physics;
    std::vector<btTypedConstraint*> joints;
    std::unique_ptr<curve_host::AccessoryAuthoring> curves;
    mdl::AccessoryKey* inactiveKeys = nullptr;
    uint32_t inactiveCursor = 0;
    uint8_t inactiveActive = 0;
    const mdl::RigidRecord* rigidTable = nullptr;
    const mdl::JointRecord* jointTable = nullptr;
    uint32_t rigidCount = 0, jointCount = 0;
    Impl(MMDApp& a, uint32_t k, unsigned s, void* p) : app(a), kind(k), slot(s), object(p), id(SceneObjectIdentity(p).id) {
        if (kind == MMS_OBJECT_ACCESSORY) {
            curves = std::make_unique<curve_host::AccessoryAuthoring>(app, slot);
            inactiveKeys = mdl::KeyPoolCreate<mdl::AccessoryKey>();
            if (!inactiveKeys) throw std::bad_alloc();
            inactiveKeys[0].visible = 1; inactiveKeys[0].parentModel = -1;
            inactiveKeys[0].scale = inactiveKeys[0].opacity = 1;
        } else {
            const auto& model = *mdl::Mdl(static_cast<unsigned char*>(object));
            rigidTable = model.rigidTable; jointTable = model.jointTable;
            rigidCount = model.rigidCount; jointCount = model.jointCount;
            if (rigidCount || jointCount) {
                if (!model.scenePtr || !model.scenePtr->world || (rigidCount && !rigidTable) || (jointCount && !jointTable))
                    throw std::invalid_argument("model physics ownership");
                auto& world = dynamic_cast<DynamicsWorld&>(*model.scenePtr->world);
                std::vector<btRigidBody*> bodies;
                for (uint32_t i = 0; i < rigidCount; ++i) {
                    if (rigidTable[i].body) bodies.push_back(static_cast<btRigidBody*>(rigidTable[i].body));
                }
                for (uint32_t i = 0; i < jointCount; ++i) {
                    // Missing-body joints are not constructed by the importer.
                    for (int n = 0; n < world.getNumConstraints(); ++n)
                        if (world.getConstraint(n)->getUid() == jointTable[i].constraint) {
                            joints.push_back(world.getConstraint(n)); break;
                        }
                }
                physics = std::make_unique<history_physics::Retained>(world, std::move(bodies), joints);
            }
        }
    }
    ~Impl() {
        retainedObjects.erase({&app, object});
        // The inactive track is always ours: either the blank replacement or
        // the deleted object's original key pool, including all grown slots.
        mdl::KeyPoolDestroy(inactiveKeys);
        if (present) return;
        if (kind == MMS_OBJECT_ACCESSORY) DeleteAccessory(static_cast<mdl::AccessoryRecord*>(object), 1);
        else {
            auto* model = static_cast<unsigned char*>(object);
            // Scale authoring holds joint pointers; forget it while the
            // retained joints are alive, before normal model disposal.
            physics_scale::Forget(model);
            for (auto* joint : joints) delete joint;
            DeleteModel(model, 1);
        }
    }
    bool MembershipCurrent() const noexcept {
        if (kind == MMS_OBJECT_ACCESSORY)
            return app.AccessorySlot(slot) == (present ? object : nullptr);
        if (app.ModelSlot(slot) != (present ? object : nullptr)) return false;
        const auto& m = *mdl::Mdl(static_cast<unsigned char*>(object));
        return m.rigidTable == rigidTable && m.jointTable == jointTable && m.rigidCount == rigidCount && m.jointCount == jointCount;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<RetainedSceneObject> lifetime;
        Impl& owner;
        bool present;
        std::unique_ptr<PreparedHistoryChange> identity;
        std::unique_ptr<history_physics::Retained::Transition> physics;
        std::unique_ptr<curve_host::AccessoryAuthoring::Exchange> curves;
        mdl::AccessoryKey* keys;
        size_t keyCapacity;
        Candidate(std::shared_ptr<RetainedSceneObject> value, bool p, bool preparePhysics = true)
            : lifetime(std::move(value)), owner(*lifetime->impl_), present(p), keys(nullptr), keyCapacity(0) {
            if (owner.present == p || !owner.MembershipCurrent()) throw std::invalid_argument("scene object membership");
            identity = PrepareObjectPresence(owner.object, owner.id, p);
            if (preparePhysics && owner.physics) physics = owner.physics->Prepare(p);
            if (owner.curves) {
                curves = owner.curves->Prepare(p); keys = owner.app.AccessoryKeys(owner.slot);
                keyCapacity = mdl::KeyPoolCapacity(keys);
            }
        }
        bool IsCurrent() const noexcept override {
            return owner.present != present && owner.MembershipCurrent() && identity->IsCurrent() &&
                (!physics || physics->IsCurrent()) && (!curves || (curves->IsCurrent() &&
                    owner.app.AccessoryKeys(owner.slot) == keys && mdl::KeyPoolCapacity(keys) == keyCapacity));
        }
        void Publish() noexcept override {
            if (physics) physics->Publish();
            if (curves) curves->Publish();
            identity->Publish();
            if (owner.kind == MMS_OBJECT_MODEL)
                owner.app.ModelSlot(owner.slot) = present ? static_cast<unsigned char*>(owner.object) : nullptr;
            else {
                std::swap(owner.app.AccessoryKeys(owner.slot), owner.inactiveKeys);
                std::swap(owner.app.AccessoryTrackCursor(owner.slot), owner.inactiveCursor);
                std::swap(owner.app.AccessoryTrackActive(owner.slot), owner.inactiveActive);
                owner.app.AccessorySlot(owner.slot) = present ? static_cast<mdl::AccessoryRecord*>(owner.object) : nullptr;
            }
            owner.present = present;
        }
    };
};
RetainedSceneObject::RetainedSceneObject(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RetainedSceneObject::~RetainedSceneObject() = default;
bool RetainedSceneObject::Present() const noexcept { return impl_->present; }
MmsId RetainedSceneObject::Id() const noexcept { return impl_->id; }
std::unique_ptr<PreparedHistoryChange> RetainedSceneObject::PreparePresence(bool present) {
    return std::make_unique<Impl::Candidate>(shared_from_this(), present);
}
std::unique_ptr<PreparedHistoryChange> RetainedSceneObject::PreparePresenceBatch(const Objects& objects, bool present) {
    if (objects.empty()) throw std::invalid_argument("empty object batch");
    struct Candidate final : PreparedHistoryChange {
        // Destroy physics contacts before releasing the native resource owners.
        std::vector<std::unique_ptr<PreparedHistoryChange>> members;
        std::unique_ptr<history_physics::Retained::Transition> physics;
        bool IsCurrent() const noexcept override {
            if (physics && !physics->IsCurrent()) return false;
            for (const auto& member : members) if (!member->IsCurrent()) return false;
            return true;
        }
        void Publish() noexcept override {
            if (physics) physics->Publish();
            for (const auto& member : members) member->Publish();
        }
    };
    auto next = std::make_unique<Candidate>();
    std::vector<history_physics::Retained*> physics;
    MMDApp* app = nullptr;
    bool modelSlots[255]{}, accessorySlots[255]{};
    for (size_t i = 0; i < objects.size(); ++i) {
        if (!objects[i] || std::find(objects.begin(), objects.begin() + i, objects[i]) != objects.begin() + i)
            throw std::invalid_argument("duplicate/null object batch member");
        auto& owner = *objects[i]->impl_;
        if (app && app != &owner.app) throw std::invalid_argument("object batch spans applications");
        app = &owner.app;
        auto& occupied = owner.kind == MMS_OBJECT_MODEL ? modelSlots[owner.slot] : accessorySlots[owner.slot];
        if (occupied) throw std::invalid_argument("object batch repeats a kind/slot");
        occupied = true;
    }
    // Validate the complete destination set before reserving identity tickets
    // or preparing curves: separate absent owners can have reused one slot.
    for (const auto& object : objects) {
        auto& owner = *object->impl_;
        if (owner.physics) physics.push_back(owner.physics.get());
        next->members.push_back(std::make_unique<Impl::Candidate>(object, present, false));
    }
    if (!physics.empty()) next->physics = history_physics::Retained::PrepareBatch(physics, present);
    return next;
}
std::shared_ptr<RetainedSceneObject> RetainSceneObject(MMDApp& app, uint32_t kind, unsigned slot) {
    if (slot >= 255 || (kind != MMS_OBJECT_MODEL && kind != MMS_OBJECT_ACCESSORY)) throw std::invalid_argument("retained object slot/kind");
    void* object = kind == MMS_OBJECT_MODEL ? static_cast<void*>(app.ModelSlot(slot)) : app.AccessorySlot(slot);
    if (!object) throw std::invalid_argument("retained object missing");
    const Key key{&app, object};
    if (auto found = retainedObjects.find(key); found != retainedObjects.end())
        if (auto retained = found->second.lock()) {
            if (retained->impl_->kind != kind || retained->impl_->slot != slot || !retained->impl_->present)
                throw std::invalid_argument("retained object alias");
            return retained;
        }
    auto result = std::shared_ptr<RetainedSceneObject>(new RetainedSceneObject(std::make_unique<RetainedSceneObject::Impl>(app, kind, slot, object)));
    retainedObjects[key] = result; return result;
}
std::shared_ptr<HistoryChange> RetainedSceneObject::CaptureRemoval(uint64_t resourceBytes) {
    return CaptureRemovalBatch({shared_from_this()}, resourceBytes);
}
std::shared_ptr<HistoryChange> RetainedSceneObject::CaptureRemovalBatch(const Objects& objects, uint64_t resourceBytes) {
    if (objects.empty()) throw std::invalid_argument("empty object removal batch");
    struct Change final : HistoryChange {
        Objects objects;
        std::shared_ptr<HistoryChange> bindings;
        uint64_t bytes;
        struct Candidate final : PreparedHistoryChange {
            std::unique_ptr<PreparedHistoryChange> presence, bindings;
            bool IsCurrent() const noexcept override { return presence->IsCurrent() && bindings->IsCurrent(); }
            void Publish() noexcept override { bindings->Publish(); presence->Publish(); }
        };
        MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
            return Boundary([&]() -> MmsResult {
                auto next = std::make_unique<Candidate>();
                next->presence = RetainedSceneObject::PreparePresenceBatch(objects, !forward);
                if (auto r = bindings->Prepare(forward, next->bindings)) return r;
                out = std::move(next); return MMS_OK;
            });
        }
        uint64_t RetainedBytes() const noexcept override { return bytes; }
    };
    auto next = std::make_shared<Change>();
    MMDApp* app = nullptr; std::vector<unsigned> models, accessories;
    for (size_t i = 0; i < objects.size(); ++i) {
        if (!objects[i] || std::find(objects.begin(), objects.begin() + i, objects[i]) != objects.begin() + i)
            throw std::invalid_argument("duplicate/null object removal batch member");
        auto& owner = *objects[i]->impl_;
        if (!owner.present || !owner.MembershipCurrent() || (app && app != &owner.app))
            throw std::invalid_argument("removed/foreign scene object");
        app = &owner.app;
        (owner.kind == MMS_OBJECT_MODEL ? models : accessories).push_back(owner.slot);
    }
    next->objects = objects;
    next->bindings = CaptureObjectsRemovalBindings(*app, models, accessories);
    const auto overhead = uint64_t(sizeof(Change)) + next->objects.capacity() * sizeof(Objects::value_type) +
        objects.size() * sizeof(Impl) + next->bindings->RetainedBytes();
    if (resourceBytes > UINT64_MAX - overhead) throw std::overflow_error("object history resource budget");
    next->bytes = resourceBytes + overhead; return next;
}
MmsResult RetainedSceneObject::PrepareInsertion(uint64_t resourceBytes,
    std::shared_ptr<HistoryChange>& insertion, std::unique_ptr<PreparedHistoryChange>& staging) {
    return Boundary([&] { return PrepareInsertionBatch({shared_from_this()}, resourceBytes, insertion, staging); });
}
MmsResult RetainedSceneObject::PrepareInsertionBatch(const Objects& objects, uint64_t resourceBytes,
    std::shared_ptr<HistoryChange>& insertion, std::unique_ptr<PreparedHistoryChange>& staging) {
    return Boundary([&]() -> MmsResult {
        struct Insertion final : HistoryChange {
            std::shared_ptr<HistoryChange> removal;
            explicit Insertion(std::shared_ptr<HistoryChange> value) : removal(std::move(value)) {}
            MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
                return removal->Prepare(!forward, out);
            }
            uint64_t RetainedBytes() const noexcept override { return removal->RetainedBytes() + sizeof(*this); }
        };
        if (resourceBytes > UINT64_MAX - sizeof(Insertion)) return MMS_E_MEMORY;
        auto removal = CaptureRemovalBatch(objects, resourceBytes);
        if (removal->RetainedBytes() > UINT64_MAX - sizeof(Insertion)) return MMS_E_MEMORY;
        auto next = std::make_shared<Insertion>(removal);
        std::unique_ptr<PreparedHistoryChange> prepared;
        if (auto r = removal->Prepare(true, prepared)) return r;
        insertion = std::move(next); staging = std::move(prepared); return MMS_OK;
    });
}
}
