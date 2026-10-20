#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "mikumikustudio/history_physics.hpp"
#include "../plugins/scene_resource_budget.hpp"
#include "mikumikustudio/dynamics_world.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "bullet_import_storage.hpp"
#include "btBulletDynamicsCommon.h"
#include "BulletCollision/CollisionDispatch/btManifoldResult.h"
#include "BulletDynamics/Dynamics/btRigidBody.h"
#include "BulletDynamics/ConstraintSolver/btTypedConstraint.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// Bullet 2.75 defines this callback beside the other two contact callbacks, but
// omits its extern declaration from btPersistentManifold.h.
extern ContactProcessedCallback gContactProcessedCallback;

namespace mikumikustudio::history_physics {
Broadphase::Broadphase(const btVector3& lo, const btVector3& hi, unsigned handles)
    : Broadphase(lo, hi, handles, std::make_unique<PairCache>()) {}
Broadphase::Broadphase(const btVector3& lo, const btVector3& hi, unsigned handles,
                       std::unique_ptr<PairCache> cache, bool disableRaycast)
    : bt32BitAxisSweep3(lo, hi, handles, cache.get(), disableRaycast) { cache.release(); }
Broadphase::~Broadphase() { delete m_pairCache; m_pairCache = nullptr; }
btBroadphaseProxy* Broadphase::createProxy(const btVector3& lo, const btVector3& hi, int shapeType,
    void* object, short group, short mask, btDispatcher* dispatcher, void* multiSapProxy) {
    auto* proxy = bt32BitAxisSweep3::createProxy(lo, hi, shapeType, object, group, mask, dispatcher, multiSapProxy);
    // Bullet 2.75 initializes the sweep edges, but leaves these public fields
    // untouched until setAabb. History can run before that first collision pass,
    // including after a freed handle is reused. Initialize only this MMS path.
    proxy->m_aabbMin.setValue(lo.x(), lo.y(), lo.z());
    proxy->m_aabbMax.setValue(hi.x(), hi.y(), hi.z());
    return proxy;
}
PairCache* Broadphase::Cache() const noexcept { return static_cast<PairCache*>(m_pairCache); }
PairCache* Broadphase::Exchange(PairCache* cache) noexcept {
    auto* old = Cache(); m_pairCache = cache; return old;
}
btBroadphaseProxy* Broadphase::RayProxy(btBroadphaseProxy* proxy) noexcept {
    return static_cast<Handle*>(proxy)->m_dbvtProxy;
}
void Broadphase::SetMask(btBroadphaseProxy* proxy, short mask) noexcept {
    proxy->m_collisionFilterMask = mask;
    if (auto* ray = RayProxy(proxy)) ray->m_collisionFilterMask = mask;
}

namespace {
btVector3 PhysicalVector(const btVector3& value) noexcept {
    return btVector3(value.x(), value.y(), value.z());
}
bool EqualVector(const btVector3& a, const btVector3& b) noexcept {
    // Bullet 2.75's vector operator== also reads the unused fourth component.
    // It is not a physics coordinate and may contain NaN or unrelated padding.
    return a.x() == b.x() && a.y() == b.y() && a.z() == b.z();
}
btTransform PhysicalTransform(const btTransform& value) noexcept {
    const auto& basis = value.getBasis();
    return btTransform(btMatrix3x3(basis[0][0], basis[0][1], basis[0][2],
        basis[1][0], basis[1][1], basis[1][2], basis[2][0], basis[2][1], basis[2][2]),
        PhysicalVector(value.getOrigin()));
}
bool EqualTransform(const btTransform& a, const btTransform& b) noexcept {
    for (int i = 0; i < 3; ++i) if (!EqualVector(a.getBasis()[i], b.getBasis()[i])) return false;
    return EqualVector(a.getOrigin(), b.getOrigin());
}
template<class T> bool Contains(const std::vector<T>& values, T p) noexcept {
    return std::find(values.begin(), values.end(), p) != values.end();
}
template<class T> std::vector<T> Copy(const btAlignedObjectArray<T>& source) {
    std::vector<T> out; out.reserve(source.size());
    for (int i = 0; i < source.size(); ++i) out.push_back(source[i]);
    return out;
}
template<class T> bool Equal(const btAlignedObjectArray<T>& actual, const std::vector<T>& expected) noexcept {
    if (actual.size() != static_cast<int>(expected.size())) return false;
    for (int i = 0; i < actual.size(); ++i) if (actual[i] != expected[i]) return false;
    return true;
}
template<class T> void Replace(btAlignedObjectArray<T>& target, const std::vector<T>& values) noexcept {
    // Capacity was reserved in prepare. T is a pointer, with no destructor.
    target.resize(static_cast<int>(values.size()));
    for (int i = 0; i < target.size(); ++i) target[i] = values[i];
}
template<class T> struct Removed {
    std::vector<T> values;
    std::vector<int> positions;
    void Capture(const std::vector<T>& before, const std::vector<T>& targets) {
        for (int i = 0; i < static_cast<int>(before.size()); ++i)
            if (Contains(targets, before[i])) { values.push_back(before[i]); positions.push_back(i); }
        if (values.size() != targets.size()) throw std::invalid_argument("missing physics member");
    }
    std::vector<T> Apply(const std::vector<T>& before, bool restore) const {
        auto result = before;
        if (restore) {
            for (size_t i = 0; i < values.size(); ++i) {
                if (Contains(result, values[i])) throw std::invalid_argument("duplicate physics member");
                result.insert(result.begin() + std::min<size_t>(positions[i], result.size()), values[i]);
            }
        } else {
            result.erase(std::remove_if(result.begin(), result.end(),
                [&](T p) { return Contains(values, p); }), result.end());
        }
        return result;
    }
};
struct BodyState {
    btRigidBody* body;
    btBroadphaseProxy* proxy;
    short group, mask;
    int activation, flags;
    btTransform transform, interpolation;
    btVector3 velocity, spin, force, torque;
    btCollisionShape* shape;
    btScalar deactivation;
    btVector3 aabbLo, aabbHi;
};
BodyState Read(btRigidBody* body) {
    auto* proxy = body->getBroadphaseHandle();
    if (!proxy) throw std::invalid_argument("missing physics proxy");
    return {body, proxy, proxy->m_collisionFilterGroup, proxy->m_collisionFilterMask,
        body->getActivationState(), body->getCollisionFlags(), PhysicalTransform(body->getWorldTransform()), PhysicalTransform(body->getInterpolationWorldTransform()),
        PhysicalVector(body->getLinearVelocity()), PhysicalVector(body->getAngularVelocity()), PhysicalVector(body->getTotalForce()), PhysicalVector(body->getTotalTorque()),
        body->getCollisionShape(), body->getDeactivationTime(), PhysicalVector(proxy->m_aabbMin), PhysicalVector(proxy->m_aabbMax)};
}
bool Current(const BodyState& s, bool parked = false, bool checkRay = true) noexcept {
    const auto mask = parked ? short(0) : s.mask;
    if (s.body->getBroadphaseHandle() != s.proxy || s.body->getActivationState() != (parked ? DISABLE_SIMULATION : s.activation) ||
        s.body->getCollisionFlags() != s.flags || s.proxy->m_collisionFilterGroup != s.group ||
        s.proxy->m_collisionFilterMask != mask || s.body->getNumConstraintRefs() ||
        !EqualTransform(s.body->getWorldTransform(), s.transform) || !EqualTransform(s.body->getInterpolationWorldTransform(), s.interpolation) ||
        !EqualVector(s.body->getLinearVelocity(), s.velocity) || !EqualVector(s.body->getAngularVelocity(), s.spin) ||
        !EqualVector(s.body->getTotalForce(), s.force) || !EqualVector(s.body->getTotalTorque(), s.torque) ||
        s.body->getCollisionShape() != s.shape || s.body->getDeactivationTime() != s.deactivation ||
        !EqualVector(s.proxy->m_aabbMin, s.aabbLo) || !EqualVector(s.proxy->m_aabbMax, s.aabbHi)) return false;
    if (!checkRay) return true;
    auto* ray = Broadphase::RayProxy(s.proxy);
    return !ray || (ray->m_collisionFilterGroup == s.group && ray->m_collisionFilterMask == mask);
}
struct ProxyState {
    btCollisionObject* object;
    btBroadphaseProxy* proxy;
    btVector3 lo, hi;
    short group, mask;
    explicit ProxyState(btCollisionObject* obj) : object(obj), proxy(obj->getBroadphaseHandle()),
        lo(PhysicalVector(proxy->m_aabbMin)), hi(PhysicalVector(proxy->m_aabbMax)), group(proxy->m_collisionFilterGroup), mask(proxy->m_collisionFilterMask) {}
    bool Current() const noexcept {
        return object->getBroadphaseHandle() == proxy && EqualVector(proxy->m_aabbMin, lo) && EqualVector(proxy->m_aabbMax, hi) &&
            proxy->m_collisionFilterGroup == group && proxy->m_collisionFilterMask == mask;
    }
};
struct OrderSnapshot {
    std::uint64_t serial;
    std::vector<btCollisionObject*> objects;
    std::vector<btRigidBody*> moving;
    std::vector<btTypedConstraint*> constraints;
};
std::atomic<std::uint64_t> nextOrderSerial{1};
struct Saved {
    Removed<btCollisionObject*> objects;
    Removed<btRigidBody*> moving;
    Removed<btTypedConstraint*> constraints;
    std::vector<BodyState> bodies;
    std::shared_ptr<const OrderSnapshot> order;
};
// Only pointer identities are compared here; historical peers may no longer
// exist. Project each saved order onto the current/restored members, then merge
// those relative orders with the existing live order. Integer insertion indices
// alone cannot restore separately removed or partially restored groups.
template<class T> std::vector<T> RestoreOrder(const std::vector<T>& before,
    const std::vector<const Removed<T>*>& removed, const std::vector<const std::vector<T>*>& orders) {
    constexpr auto missing = std::numeric_limits<size_t>::max();
    std::unordered_map<T, size_t> indices;
    for (auto value : before)
        if (!indices.emplace(value, missing).second) throw std::invalid_argument("duplicate live physics member");
    for (const auto* group : removed) for (auto value : group->values)
        if (!indices.emplace(value, missing).second) throw std::invalid_argument("duplicate restored physics member");
    std::vector<T> members; members.reserve(indices.size());
    const auto prefer = [&](const std::vector<T>& values) {
        for (auto value : values) {
            auto found = indices.find(value);
            if (found != indices.end() && found->second == missing) {
                found->second = members.size(); members.push_back(value);
            }
        }
    };
    // Older snapshots supply the stable tie-break for unrelated components.
    for (const auto* order : orders) prefer(*order);
    prefer(before);
    for (const auto* group : removed) prefer(group->values);
    std::vector<std::vector<size_t>> edges(members.size());
    std::vector<size_t> incoming(members.size(), 0);
    const auto constrain = [&](const std::vector<T>& values) {
        size_t previous = missing;
        for (auto value : values) {
            const auto found = indices.find(value);
            if (found == indices.end()) continue;
            const auto current = found->second;
            if (previous != missing && previous != current) {
                edges[previous].push_back(current); ++incoming[current];
            }
            previous = current;
        }
    };
    constrain(before);
    for (const auto* order : orders) constrain(*order);
    std::priority_queue<size_t, std::vector<size_t>, std::greater<size_t>> ready;
    for (size_t i = 0; i < members.size(); ++i) if (!incoming[i]) ready.push(i);
    std::vector<T> result; result.reserve(members.size());
    while (!ready.empty()) {
        const auto i = ready.top(); ready.pop(); result.push_back(members[i]);
        for (auto next : edges[i]) if (!--incoming[next]) ready.push(next);
    }
    if (result.size() != members.size()) throw std::invalid_argument("conflicting retained physics order");
    return result;
}
struct Filter final : btOverlapFilterCallback {
    const std::vector<BodyState>& restored;
    explicit Filter(const std::vector<BodyState>& b) : restored(b) {}
    short Mask(btBroadphaseProxy* p) const noexcept {
        for (const auto& b : restored) if (b.proxy == p) return b.mask;
        return p->m_collisionFilterMask;
    }
    bool needBroadphaseCollision(btBroadphaseProxy* a, btBroadphaseProxy* b) const override {
        return (a->m_collisionFilterGroup & Mask(b)) && (b->m_collisionFilterGroup & Mask(a));
    }
};
}
struct Retained::Impl {
    DynamicsWorld& world;
    Broadphase& broadphase;
    std::vector<btRigidBody*> bodies;
    std::vector<btTypedConstraint*> constraints;
    std::unique_ptr<Saved> saved;
    bool present = true, importPending = false;
    std::uint64_t generation = 1;
    Impl(DynamicsWorld& w, std::vector<btRigidBody*> b, std::vector<btTypedConstraint*> c)
        : world(w), broadphase(dynamic_cast<Broadphase&>(*w.getBroadphase())), bodies(std::move(b)), constraints(std::move(c)) {
        for (size_t i = 0; i < bodies.size(); ++i)
            if (!bodies[i] || std::find(bodies.begin(), bodies.begin() + i, bodies[i]) != bodies.begin() + i)
                throw std::invalid_argument("duplicate/null retained body");
        for (size_t i = 0; i < constraints.size(); ++i)
            if (!constraints[i] || std::find(constraints.begin(), constraints.begin() + i, constraints[i]) != constraints.begin() + i)
                throw std::invalid_argument("duplicate/null retained constraint");
        // A parked joint cannot safely retain a raw pointer to another owner's
        // body: that owner's history may release it while this joint is absent.
        for (auto* constraint : constraints)
            if (!Contains(bodies, &constraint->getRigidBodyA()) || !Contains(bodies, &constraint->getRigidBodyB()))
                throw std::invalid_argument("constraint outside retained bodies");
    }
    void MeasureResources(plugins::resource_budget::Collector& out, const Saved* state) const {
        // A transition retains this Impl even if its Retained wrapper has gone.
        // Bundle the wrapper allowance here so both public measurements use the
        // same stable identity without dereferencing a potentially dead wrapper.
        out.Add(this, 1, plugins::resource_budget::Collector::Sum(sizeof(Retained), sizeof(*this)));
        out.Vector(bodies); out.Vector(constraints);
        if (!state) return;
        out.Array(state, 1);
        out.Vector(state->objects.values); out.Vector(state->objects.positions);
        out.Vector(state->moving.values); out.Vector(state->moving.positions);
        out.Vector(state->constraints.values); out.Vector(state->constraints.positions);
        out.Vector(state->bodies);
        if (state->order) {
            out.Array(state->order.get(), 1); out.Vector(state->order->objects);
            out.Vector(state->order->moving); out.Vector(state->order->constraints);
        }
    }
};
struct Retained::Transition::Impl {
    struct Entry {
        std::shared_ptr<Retained::Impl> owner;
        std::uint64_t generation;
        bool present;
        std::unique_ptr<Saved> saved;
        Entry(std::shared_ptr<Retained::Impl> o, bool p) : owner(std::move(o)), generation(owner->generation), present(p) {}
        const Saved& State() const { return present ? *owner->saved : *saved; }
    };
    std::vector<Entry> entries;
    DynamicsWorld& world;
    Broadphase& broadphase;
    bool consumed = false;
    std::uint64_t step;
    PairCache* source;
    std::unique_ptr<PairCache> cache;
    std::vector<btBroadphasePair> pairs;
    std::vector<std::pair<size_t, size_t>> transfers;
    std::vector<btCollisionObject*> beforeObjects, afterObjects;
    std::vector<btRigidBody*> beforeMoving, afterMoving;
    std::vector<btTypedConstraint*> beforeConstraints, afterConstraints;
    std::vector<ProxyState> proxies;
    std::vector<BodyState> bodies;
    Impl(std::vector<std::pair<std::shared_ptr<Retained::Impl>, bool>> retained)
        : world(retained.front().first->world), broadphase(retained.front().first->broadphase),
          step(world.StepSerial()), source(broadphase.Cache()) {
        if (world.getBroadphase() != &broadphase || world.HasActions() || source->getOverlapFilterCallback() ||
            source->HasGhostCallback() || broadphase.getOverlappingPairUserCallback())
            throw std::invalid_argument("unsupported physics membership transition");
        std::unordered_set<Retained::Impl*> owners;
        std::unordered_set<btRigidBody*> ownedBodies;
        std::unordered_set<btRigidBody*> outgoingBodies;
        std::unordered_set<btTypedConstraint*> ownedConstraints;
        entries.reserve(retained.size());
        for (auto& change : retained) {
            auto& owner = change.first;
            if (&owner->world != &world || owner->importPending || owner->present == change.second || !owners.insert(owner.get()).second)
                throw std::invalid_argument("incompatible physics membership batch");
            for (auto* body : owner->bodies) {
                if (!ownedBodies.insert(body).second) throw std::invalid_argument("overlapping retained bodies");
                if (!change.second) outgoingBodies.insert(body);
            }
            for (auto* constraint : owner->constraints)
                if (!ownedConstraints.insert(constraint).second) throw std::invalid_argument("overlapping retained constraints");
            entries.emplace_back(std::move(owner), change.second);
        }
        beforeObjects = Copy(world.getCollisionObjectArray());
        beforeMoving = Copy(world.HistoryBodies()); beforeConstraints = Copy(world.HistoryConstraints());
        for (auto* object : beforeObjects) {
            if (!object->getBroadphaseHandle()) throw std::invalid_argument("missing active proxy");
            proxies.emplace_back(object);
        }
        // An unretained joint must never keep a parked body in an active island.
        for (auto* joint : beforeConstraints) {
            if ((ownedBodies.count(&joint->getRigidBodyA()) || ownedBodies.count(&joint->getRigidBodyB())) &&
                !ownedConstraints.count(joint)) throw std::invalid_argument("external retained constraint");
        }
        std::shared_ptr<OrderSnapshot> order;
        if (std::any_of(entries.begin(), entries.end(), [](const auto& entry) { return !entry.present; }))
            order = std::make_shared<OrderSnapshot>(OrderSnapshot{
            nextOrderSerial.fetch_add(1, std::memory_order_relaxed), beforeObjects, beforeMoving, beforeConstraints});
        std::vector<BodyState> restored;
        std::vector<const Removed<btCollisionObject*>*> objectGroups;
        std::vector<const Removed<btRigidBody*>*> movingGroups;
        std::vector<const Removed<btTypedConstraint*>*> constraintGroups;
        std::vector<const OrderSnapshot*> orders;
        for (auto& entry : entries) {
            auto& o = *entry.owner;
            std::vector<btCollisionObject*> objects;
            std::vector<btRigidBody*> moving;
            std::vector<BodyState> currentBodies;
            for (auto* body : o.bodies) {
                if (body->getNumConstraintRefs()) throw std::invalid_argument("filtered constraint references");
                currentBodies.push_back(Read(body));
                objects.push_back(body);
                if (!body->isStaticObject()) moving.push_back(body);
            }
            bodies.insert(bodies.end(), currentBodies.begin(), currentBodies.end());
            if (!entry.present) {
                entry.saved = std::make_unique<Saved>(); entry.saved->bodies = std::move(currentBodies);
                entry.saved->order = order;
                entry.saved->objects.Capture(beforeObjects, objects); entry.saved->moving.Capture(beforeMoving, moving);
                entry.saved->constraints.Capture(beforeConstraints, o.constraints);
            } else {
                if (!o.saved || !o.saved->order) throw std::invalid_argument("missing parked physics state");
                for (const auto& body : o.saved->bodies)
                    if (!history_physics::Current(body, true)) throw std::invalid_argument("parked physics body was changed");
                restored.insert(restored.end(), o.saved->bodies.begin(), o.saved->bodies.end());
                objectGroups.push_back(&o.saved->objects); movingGroups.push_back(&o.saved->moving);
                constraintGroups.push_back(&o.saved->constraints); orders.push_back(o.saved->order.get());
            }
        }
        std::sort(orders.begin(), orders.end(), [](const auto* a, const auto* b) { return a->serial < b->serial; });
        orders.erase(std::unique(orders.begin(), orders.end()), orders.end());
        // Project historical incoming anchors onto the final membership. An
        // outgoing peer can appear in an incoming Saved order, but must not
        // re-enter either the output or its relative-order constraints.
        afterObjects = beforeObjects; afterMoving = beforeMoving; afterConstraints = beforeConstraints;
        for (const auto& entry : entries) if (!entry.present) {
            afterObjects = entry.saved->objects.Apply(afterObjects, false);
            afterMoving = entry.saved->moving.Apply(afterMoving, false);
            afterConstraints = entry.saved->constraints.Apply(afterConstraints, false);
        }
        if (!orders.empty()) {
            std::vector<const std::vector<btCollisionObject*>*> objectOrders;
            std::vector<const std::vector<btRigidBody*>*> movingOrders;
            std::vector<const std::vector<btTypedConstraint*>*> constraintOrders;
            for (const auto* savedOrder : orders) {
                objectOrders.push_back(&savedOrder->objects); movingOrders.push_back(&savedOrder->moving);
                constraintOrders.push_back(&savedOrder->constraints);
            }
            afterObjects = RestoreOrder(afterObjects, objectGroups, objectOrders);
            afterMoving = RestoreOrder(afterMoving, movingGroups, movingOrders);
            afterConstraints = RestoreOrder(afterConstraints, constraintGroups, constraintOrders);
        }
        cache = std::make_unique<PairCache>();
        pairs = Copy(source->getOverlappingPairArray());
        Filter filter(restored);
        if (!restored.empty()) cache->setOverlapFilterCallback(&filter);
        for (const auto& pair : pairs) {
            const bool outgoing = outgoingBodies.count(btRigidBody::upcast(static_cast<btCollisionObject*>(pair.m_pProxy0->m_clientObject))) ||
                                  outgoingBodies.count(btRigidBody::upcast(static_cast<btCollisionObject*>(pair.m_pProxy1->m_clientObject)));
            if (!outgoing) cache->addOverlappingPair(pair.m_pProxy0, pair.m_pProxy1);
        }
        for (const auto& body : restored) for (auto* other : afterObjects) {
            auto* proxy = other->getBroadphaseHandle();
            if (proxy != body.proxy && broadphase.testAabbOverlap(body.proxy, proxy))
                cache->addOverlappingPair(body.proxy, proxy);
        }
        cache->setOverlapFilterCallback(nullptr);
        for (size_t i = 0; i < pairs.size(); ++i) {
            if (auto* target = cache->findPair(pairs[i].m_pProxy0, pairs[i].m_pProxy1))
                transfers.emplace_back(i, target - cache->getOverlappingPairArrayPtr());
        }
        // These operations may allocate; publication only writes reserved slots.
        const auto maximum = static_cast<std::size_t>((std::numeric_limits<int>::max)());
        if (afterObjects.size() > maximum || afterMoving.size() > maximum || afterConstraints.size() > maximum)
            throw std::bad_alloc();
        world.getCollisionObjectArray().reserve(static_cast<int>(afterObjects.size()));
        world.HistoryBodies().reserve(static_cast<int>(afterMoving.size()));
        world.HistoryConstraints().reserve(static_cast<int>(afterConstraints.size()));
    }
    ~Impl() {
        // After publication this is the old cache. Surviving algorithms were
        // moved to the new cache; only deleted contacts are destroyed here.
        if (cache) for (int i = 0; i < cache->getNumOverlappingPairs(); ++i)
            cache->cleanOverlappingPair(cache->getOverlappingPairArray()[i], world.getDispatcher());
    }
    bool Current() const noexcept {
        if (consumed || world.StepSerial() != step || world.getBroadphase() != &broadphase || broadphase.Cache() != source || world.HasActions() ||
            source->getOverlapFilterCallback() || source->HasGhostCallback() || broadphase.getOverlappingPairUserCallback() ||
            !Equal(world.getCollisionObjectArray(), beforeObjects) ||
            !Equal(world.HistoryBodies(), beforeMoving) || !Equal(world.HistoryConstraints(), beforeConstraints) ||
            world.getCollisionObjectArray().capacity() < static_cast<int>(afterObjects.size()) ||
            world.HistoryBodies().capacity() < static_cast<int>(afterMoving.size()) ||
            world.HistoryConstraints().capacity() < static_cast<int>(afterConstraints.size()) ||
            source->getNumOverlappingPairs() != static_cast<int>(pairs.size())) return false;
        for (const auto& entry : entries)
            if (entry.owner->generation != entry.generation || entry.owner->present == entry.present) return false;
        for (const auto& proxy : proxies) if (!proxy.Current()) return false;
        for (const auto& b : bodies) if (!history_physics::Current(b)) return false;
        for (size_t i = 0; i < pairs.size(); ++i) {
            const auto& now = source->getOverlappingPairArray()[static_cast<int>(i)]; const auto& old = pairs[i];
            if (now.m_pProxy0 != old.m_pProxy0 || now.m_pProxy1 != old.m_pProxy1 ||
                now.m_algorithm != old.m_algorithm || now.m_internalInfo1 != old.m_internalInfo1) return false;
        }
        return true;
    }
    bool Publish() noexcept {
        if (!Current()) return false;
        for (const auto& transfer : transfers) {
            auto& from = source->getOverlappingPairArray()[static_cast<int>(transfer.first)];
            auto& to = cache->getOverlappingPairArray()[static_cast<int>(transfer.second)];
            to.m_algorithm = from.m_algorithm; to.m_internalInfo1 = from.m_internalInfo1;
            from.m_algorithm = nullptr; from.m_internalInfo1 = nullptr;
        }
        for (const auto& entry : entries) for (const auto& body : entry.State().bodies) {
            Broadphase::SetMask(body.proxy, entry.present ? body.mask : 0);
            body.body->forceActivationState(entry.present ? body.activation : DISABLE_SIMULATION);
        }
        Replace(world.getCollisionObjectArray(), afterObjects);
        Replace(world.HistoryBodies(), afterMoving); Replace(world.HistoryConstraints(), afterConstraints);
        cache.reset(broadphase.Exchange(cache.release()));
        for (auto& entry : entries) {
            if (!entry.present) entry.owner->saved.swap(entry.saved);
            entry.owner->present = entry.present; ++entry.owner->generation;
        }
        consumed = true;
        return true;
    }
};
Retained::Retained(DynamicsWorld& world, std::vector<btRigidBody*> bodies, std::vector<btTypedConstraint*> constraints)
    : impl_(std::make_shared<Impl>(world, std::move(bodies), std::move(constraints))) {}
Retained::~Retained() = default;
bool Retained::Present() const noexcept { return impl_->present; }
void Retained::MeasureResources(plugins::resource_budget::Collector& out) const {
    impl_->MeasureResources(out, impl_->saved.get());
}
std::unique_ptr<Retained::Transition> Retained::Prepare(bool present) {
    return PrepareBatch({this}, present);
}
std::unique_ptr<Retained::Transition> Retained::PrepareBatch(const std::vector<Retained*>& objects, bool present) {
    std::vector<PresenceChange> changes; changes.reserve(objects.size());
    for (auto* object : objects) changes.push_back({object, present});
    return PrepareExchange(changes);
}
std::unique_ptr<Retained::Transition> Retained::PrepareExchange(const std::vector<PresenceChange>& changes) {
    if (changes.empty()) throw std::invalid_argument("empty physics membership batch");
    std::vector<std::pair<std::shared_ptr<Impl>, bool>> retained; retained.reserve(changes.size());
    for (const auto& change : changes) {
        if (!change.object) throw std::invalid_argument("null retained physics object");
        retained.emplace_back(change.object->impl_, change.present);
    }
    return std::unique_ptr<Transition>(new Transition(std::make_unique<Transition::Impl>(std::move(retained))));
}
Retained::Transition::Transition(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Retained::Transition::~Transition() = default;
bool Retained::Transition::IsCurrent() const noexcept { return impl_->Current(); }
bool Retained::Transition::MeasureTargetResources(plugins::resource_budget::Collector& out) const {
    if (!impl_->Current()) return false;
    // Validate the complete target before the first Collector write.
    for (const auto& entry : impl_->entries)
        if (!(entry.present ? entry.owner->saved : entry.saved)) return false;
    for (const auto& entry : impl_->entries)
        entry.owner->MeasureResources(out, &entry.State());
    return true;
}
bool Retained::Transition::Publish() noexcept { return impl_->Publish(); }

namespace {
bool ImportCallbacks(const DynamicsWorld& world, const btCollisionDispatcher& dispatcher) noexcept {
    return world.HasActions() || import_storage::HasTickCallbacks(world) ||
        dispatcher.getNearCallback() != btCollisionDispatcher::defaultNearCallback ||
        gContactAddedCallback || gContactDestroyedCallback || gContactProcessedCallback;
}
bool SamePairs(PairCache* cache, const std::vector<btBroadphasePair>& pairs) noexcept {
    if (cache->getNumOverlappingPairs() != static_cast<int>(pairs.size())) return false;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const auto& a = cache->getOverlappingPairArray()[static_cast<int>(i)]; const auto& b = pairs[i];
        if (a.m_pProxy0 != b.m_pProxy0 || a.m_pProxy1 != b.m_pProxy1 ||
            a.m_algorithm != b.m_algorithm || a.m_internalInfo1 != b.m_internalInfo1) return false;
    }
    return true;
}
struct KeepPair final : btOverlapFilterCallback {
    bool needBroadphaseCollision(btBroadphaseProxy*, btBroadphaseProxy*) const override { return true; }
};
}

// Prepare compact sweep/ray structures, then copy edge indices back into the
// fixed live handle pool. Existing handles never move, including parked ones.
// The shadow is sized by occupied handles, not the scene's 1.5M handle limit.
struct ImportBroadphaseState {
    using Handle = Broadphase::Handle;
    using Edge = Broadphase::Edge;
    struct Entry {
        unsigned id;
        Handle* target;
        Handle* shadow = nullptr;
        BodyState before;
        btBroadphaseProxy* oldRay;
        bool added;
    };
    Broadphase& live;
    btDispatcher* dispatcher;
    unsigned oldCount, firstFree, nextFree = 0;
    PairCache* source;
    std::unique_ptr<Broadphase> shadow;
    std::unique_ptr<PairCache> cache;
    std::vector<Entry> entries;
    std::vector<std::pair<unsigned, unsigned>> reserved;
    std::vector<Edge> beforeEdges[3], targetEdges[3];
    std::vector<btBroadphasePair> pairs;
    std::vector<std::pair<int, int>> transfers;
    bool published = false;

    ImportBroadphaseState(Broadphase& bp, btDispatcher* d, const std::vector<btRigidBody*>& added)
        : live(bp), dispatcher(d), oldCount(bp.m_numHandles), firstFree(bp.m_firstFreeHandle), source(bp.Cache()) {
        if (!bp.m_raycastAccelerator || !bp.m_nullPairCache || bp.m_invalidPair ||
            source->getOverlapFilterCallback() || source->HasGhostCallback() || bp.m_userPairCallback ||
            added.size() > bp.m_maxHandles - 1 - oldCount)
            throw std::invalid_argument("unsupported import broadphase");
        entries.reserve(oldCount + added.size());
        std::unordered_set<unsigned> used;
        for (unsigned i = 1; i < oldCount * 2 + 1; ++i) {
            const auto& edge = bp.m_pEdges[0][i];
            if (!edge.IsMax()) continue;
            const unsigned id = edge.m_handle;
            if (!id || id >= bp.m_maxHandles || !used.insert(id).second)
                throw std::invalid_argument("invalid live physics handle");
            auto* handle = bp.getHandle(id);
            auto* object = static_cast<btCollisionObject*>(handle->m_clientObject);
            auto* body = object ? btRigidBody::upcast(object) : nullptr;
            if (!body || body->getBroadphaseHandle() != handle || handle->m_uniqueId != static_cast<int>(id) ||
                handle->m_multiSapParentProxy || body->getNumConstraintRefs() || !handle->m_dbvtProxy ||
                handle->m_dbvtProxy->m_clientObject != body ||
                handle->m_dbvtProxy->m_collisionFilterGroup != handle->m_collisionFilterGroup ||
                handle->m_dbvtProxy->m_collisionFilterMask != handle->m_collisionFilterMask)
                throw std::invalid_argument("unsupported live physics proxy");
            entries.push_back({id, handle, nullptr, Read(body), handle->m_dbvtProxy, false});
        }
        if (entries.size() != oldCount) throw std::invalid_argument("invalid live handle count");
        auto next = firstFree;
        for (auto* body : added) {
            if (!next || next >= bp.m_maxHandles || !used.insert(next).second || !body ||
                !body->getBroadphaseHandle() || body->getNumConstraintRefs())
                throw std::invalid_argument("invalid import physics handle");
            auto* handle = bp.getHandle(next);
            const auto following = handle->GetNextFree();
            reserved.emplace_back(next, following);
            entries.push_back({next, handle, nullptr, Read(body), nullptr, true});
            next = following;
        }
        nextFree = next;
        // The stock constructor allocates ray structures before new Handle[].
        // If that C++ allocation throws, its incomplete base cannot release
        // those structures. Build the shadow's ray tree only after its sweep.
        shadow.reset(new Broadphase(bp.m_worldAabbMin, bp.m_worldAabbMax,
            (std::max)(1u, static_cast<unsigned>(entries.size())), std::make_unique<PairCache>(), true));
        void* nullStorage = btAlignedAlloc(sizeof(btNullPairCache), 16);
        if (!nullStorage) throw std::bad_alloc();
        auto* nullCache = new(nullStorage) btNullPairCache();
        void* rayStorage = nullptr;
        try { rayStorage = btAlignedAlloc(sizeof(btDbvtBroadphase), 16); }
        catch (...) { nullCache->~btNullPairCache(); btAlignedFree(nullStorage); throw; }
        if (!rayStorage) { nullCache->~btNullPairCache(); btAlignedFree(nullStorage); throw std::bad_alloc(); }
        shadow->m_nullPairCache = nullCache;
        shadow->m_raycastAccelerator = new(rayStorage) btDbvtBroadphase(nullCache);
        shadow->m_raycastAccelerator->m_deferedcollide = true;
        // From this point failures must clean the shadow's separately allocated
        // DBVT proxies: Bullet's broadphase destructor only destroys the trees.
        try {
            std::vector<btBroadphaseProxy> keys; keys.reserve(entries.size());
            std::unordered_map<btBroadphaseProxy*, std::size_t> oldIndex;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                auto& entry = entries[i]; const auto& b = entry.before;
                entry.shadow = static_cast<Handle*>(shadow->createProxy(b.aabbLo, b.aabbHi,
                    b.shape->getShapeType(), b.body, b.group, b.mask, d, nullptr));
                keys.push_back(*entry.shadow); keys.back().m_uniqueId = static_cast<int>(entry.id);
                if (!entry.added) oldIndex.emplace(entry.target, i);
            }
            for (int axis = 0; axis < 3; ++axis) {
                beforeEdges[axis].assign(bp.m_pEdges[axis], bp.m_pEdges[axis] + oldCount * 2 + 2);
                targetEdges[axis].assign(shadow->m_pEdges[axis], shadow->m_pEdges[axis] + entries.size() * 2 + 2);
                for (auto& edge : targetEdges[axis]) if (edge.m_handle)
                    edge.m_handle = entries[edge.m_handle - 1].id;
            }
            pairs = Copy(source->getOverlappingPairArray()); cache = std::make_unique<PairCache>();
            KeepPair keep;
            cache->setOverlapFilterCallback(&keep);
            for (const auto& pair : pairs) {
                const auto a = oldIndex.find(pair.m_pProxy0), b = oldIndex.find(pair.m_pProxy1);
                if (a == oldIndex.end() || b == oldIndex.end()) throw std::invalid_argument("foreign live pair");
                cache->addOverlappingPair(&keys[a->second], &keys[b->second]);
            }
            // Retain every old pair and algorithm. Only import-related pairs
            // come from the newly prepared sweep; no collision algorithms run.
            const auto& newPairs = shadow->Cache()->getOverlappingPairArray();
            for (int i = 0; i < newPairs.size(); ++i) {
                const auto a = static_cast<std::size_t>(newPairs[i].m_pProxy0->m_uniqueId - 1);
                const auto b = static_cast<std::size_t>(newPairs[i].m_pProxy1->m_uniqueId - 1);
                if (entries[a].added || entries[b].added) cache->addOverlappingPair(&keys[a], &keys[b]);
            }
            cache->setOverlapFilterCallback(nullptr);
            for (std::size_t i = 0; i < pairs.size(); ++i) {
                auto* target = cache->findPair(pairs[i].m_pProxy0, pairs[i].m_pProxy1);
                if (!target) throw std::invalid_argument("missing retained import pair");
                transfers.emplace_back(static_cast<int>(i), static_cast<int>(target - cache->getOverlappingPairArrayPtr()));
            }
            // Hashes use the final UIDs already stored in keys. Replacing the
            // pointers preserves hash buckets without reading free live slots.
            auto& finalPairs = cache->getOverlappingPairArray();
            for (int i = 0; i < finalPairs.size(); ++i) {
                finalPairs[i].m_pProxy0 = entries[finalPairs[i].m_pProxy0 - keys.data()].target;
                finalPairs[i].m_pProxy1 = entries[finalPairs[i].m_pProxy1 - keys.data()].target;
            }
        } catch (...) { ClearShadowRays(); throw; }
    }
    void ClearShadowRays() noexcept {
        if (!shadow) return;
        for (auto& entry : entries) {
            auto* ray = published ? entry.oldRay : (entry.shadow ? entry.shadow->m_dbvtProxy : nullptr);
            if (ray) shadow->m_raycastAccelerator->destroyProxy(ray, dispatcher);
            if (!published && entry.shadow) entry.shadow->m_dbvtProxy = nullptr;
        }
    }
    ~ImportBroadphaseState() { ClearShadowRays(); }
    bool Current() const noexcept {
        if (published || live.m_numHandles != oldCount || live.m_firstFreeHandle != firstFree ||
            live.Cache() != source || source->getOverlapFilterCallback() || source->HasGhostCallback() ||
            live.m_userPairCallback || !SamePairs(source, pairs)) return false;
        for (const auto& item : reserved) if (live.getHandle(item.first)->GetNextFree() != item.second) return false;
        for (int axis = 0; axis < 3; ++axis) for (std::size_t i = 0; i < beforeEdges[axis].size(); ++i) {
            const auto& a = live.m_pEdges[axis][i]; const auto& b = beforeEdges[axis][i];
            if (a.m_pos != b.m_pos || a.m_handle != b.m_handle) return false;
        }
        for (const auto& entry : entries) {
            if (!history_physics::Current(entry.before, false, !entry.added)) return false;
            if (!entry.added && (entry.target->m_dbvtProxy != entry.oldRay ||
                entry.target->m_uniqueId != static_cast<int>(entry.id))) return false;
        }
        return true;
    }
    void Publish() noexcept {
        for (const auto& transfer : transfers) {
            auto& from = source->getOverlappingPairArray()[transfer.first];
            auto& to = cache->getOverlappingPairArray()[transfer.second];
            to.m_algorithm = from.m_algorithm; to.m_internalInfo1 = from.m_internalInfo1;
            from.m_algorithm = nullptr; from.m_internalInfo1 = nullptr;
        }
        for (auto& entry : entries) {
            auto& target = *entry.target; const auto& from = *entry.shadow;
            if (entry.added) {
                static_cast<btBroadphaseProxy&>(target) = from;
                target.m_uniqueId = static_cast<int>(entry.id);
                entry.before.body->setBroadphaseHandle(&target);
            }
            for (int axis = 0; axis < 3; ++axis) {
                target.m_minEdges[axis] = from.m_minEdges[axis]; target.m_maxEdges[axis] = from.m_maxEdges[axis];
            }
            target.m_dbvtProxy = from.m_dbvtProxy;
        }
        for (int axis = 0; axis < 3; ++axis) {
            std::copy(targetEdges[axis].begin(), targetEdges[axis].end(), live.m_pEdges[axis]);
            live.m_pHandles[0].m_minEdges[axis] = 0;
            live.m_pHandles[0].m_maxEdges[axis] = static_cast<unsigned>(entries.size() * 2 + 1);
        }
        live.m_numHandles = static_cast<unsigned>(entries.size()); live.m_firstFreeHandle = nextFree;
        std::swap(live.m_raycastAccelerator, shadow->m_raycastAccelerator);
        std::swap(live.m_nullPairCache, shadow->m_nullPairCache);
        cache.reset(live.Exchange(cache.release()));
        published = true;
    }
};

struct ImportScene::Impl {
    PhysicsScene& live;
    DynamicsWorld& target;
    Broadphase& broadphase;
    const int baseId;
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
    PairCache privateCache;
    btDbvtBroadphase privateBroadphase{&privateCache};
    btSequentialImpulseConstraintSolver solver;
    DynamicsWorld world{&dispatcher, &privateBroadphase, &solver, &config};
    PhysicsScene scene{};
    bool published = false;
    explicit Impl(PhysicsScene& source)
        : live(source), target(dynamic_cast<DynamicsWorld&>(*source.world)),
          broadphase(dynamic_cast<Broadphase&>(*target.getBroadphase())), baseId(source.constraintId) {
        if (baseId < 0 || source.dispatcher != target.getDispatcher() ||
            ImportCallbacks(target, *source.dispatcher)) throw std::invalid_argument("unsupported import world");
        world.setGravity(target.getGravity()); world.getSolverInfo() = target.getSolverInfo();
        scene.owner = source.owner; scene.collisionConfig = &config; scene.dispatcher = &dispatcher;
        scene.constraintId = baseId; scene.solver = &solver; scene.world = &world;
    }
};

struct ImportScene::Publication::Impl {
    std::shared_ptr<ImportScene::Impl> owner;
    std::shared_ptr<Retained> retained;
    std::uint64_t step, privateStep;
    int endId;
    btVector3 gravity;
    std::vector<btCollisionObject*> beforeObjects, privateObjects;
    std::vector<btRigidBody*> beforeMoving, privateMoving, imported;
    std::vector<btTypedConstraint*> beforeConstraints, privateConstraints;
    std::vector<int> constraintIds;
    btAlignedObjectArray<btCollisionObject*> afterObjects, emptyObjects;
    btAlignedObjectArray<btRigidBody*> afterMoving, emptyMoving;
    btAlignedObjectArray<btTypedConstraint*> afterConstraints, emptyConstraints;
    std::unique_ptr<ImportBroadphaseState> broadphase;
    bool published = false;
    explicit Impl(std::shared_ptr<ImportScene::Impl> o) : owner(std::move(o)), step(owner->target.StepSerial()),
        privateStep(owner->world.StepSerial()), endId(owner->scene.constraintId), gravity(owner->target.getGravity()) {
        auto& state = *owner;
        if (state.published || privateStep || endId < state.baseId ||
            state.live.constraintId != state.baseId || ImportCallbacks(state.target, *state.live.dispatcher) ||
            ImportCallbacks(state.world, state.dispatcher) || state.privateCache.getOverlapFilterCallback() ||
            state.privateCache.HasGhostCallback() || state.dispatcher.getNumManifolds() ||
            !EqualVector(state.world.getGravity(), state.target.getGravity()))
            throw std::invalid_argument("changed import world");
        beforeObjects = Copy(state.target.getCollisionObjectArray()); privateObjects = Copy(state.world.getCollisionObjectArray());
        beforeMoving = Copy(state.target.HistoryBodies()); privateMoving = Copy(state.world.HistoryBodies());
        beforeConstraints = Copy(state.target.HistoryConstraints()); privateConstraints = Copy(state.world.HistoryConstraints());
        if (static_cast<std::uint64_t>(endId - state.baseId) != privateConstraints.size())
            throw std::invalid_argument("noncontiguous import constraint IDs");
        std::unordered_set<btRigidBody*> bodies;
        for (auto* object : privateObjects) {
            auto* body = object ? btRigidBody::upcast(object) : nullptr;
            if (!body || typeid(*body) != typeid(btRigidBody) || !body->getCollisionShape() ||
                !body->getBroadphaseHandle() || body->getNumConstraintRefs() || !bodies.insert(body).second ||
                Contains(beforeObjects, object)) throw std::invalid_argument("invalid import body");
            imported.push_back(body);
        }
        for (auto* body : privateMoving) if (!bodies.count(body)) throw std::invalid_argument("foreign import moving body");
        for (std::size_t i = 0; i < privateConstraints.size(); ++i) {
            auto* joint = privateConstraints[i];
            if (!joint || !bodies.count(&joint->getRigidBodyA()) || !bodies.count(&joint->getRigidBodyB()) ||
                joint->getUid() != state.baseId + static_cast<int>(i) || Contains(beforeConstraints, joint))
                throw std::invalid_argument("invalid import constraint");
            constraintIds.push_back(joint->getUid());
        }
        for (auto* joint : beforeConstraints) if (!joint || bodies.count(&joint->getRigidBodyA()) ||
            bodies.count(&joint->getRigidBodyB()) || (joint->getUid() >= state.baseId && joint->getUid() < endId))
            throw std::invalid_argument("external import constraint or reused ID");
        auto objects = beforeObjects; objects.insert(objects.end(), privateObjects.begin(), privateObjects.end());
        auto moving = beforeMoving; moving.insert(moving.end(), privateMoving.begin(), privateMoving.end());
        auto constraints = beforeConstraints; constraints.insert(constraints.end(), privateConstraints.begin(), privateConstraints.end());
        import_storage::Prepare(afterObjects, objects); import_storage::Prepare(afterMoving, moving);
        import_storage::Prepare(afterConstraints, constraints);
        broadphase = std::make_unique<ImportBroadphaseState>(state.broadphase, state.target.getDispatcher(), imported);
        retained = std::make_shared<Retained>(state.target, imported, privateConstraints);
        retained->impl_->present = false; retained->impl_->importPending = true;
    }
    ~Impl() {
        if (published) for (const auto& entry : broadphase->entries) if (entry.added)
            owner->privateBroadphase.destroyProxy(entry.before.proxy, &owner->dispatcher);
    }
    bool Current() const noexcept {
        auto& state = *owner;
        if (published || state.published || state.target.StepSerial() != step || state.world.StepSerial() != privateStep ||
            state.live.world != &state.target || state.target.getBroadphase() != &state.broadphase ||
            state.live.dispatcher != state.target.getDispatcher() || state.live.constraintId != state.baseId ||
            state.scene.world != &state.world || state.scene.constraintId != endId ||
            state.world.getBroadphase() != &state.privateBroadphase ||
            !EqualVector(state.target.getGravity(), gravity) || !EqualVector(state.world.getGravity(), gravity) ||
            ImportCallbacks(state.target, *state.live.dispatcher) || ImportCallbacks(state.world, state.dispatcher) ||
            state.privateCache.getOverlapFilterCallback() || state.privateCache.HasGhostCallback() ||
            state.dispatcher.getNumManifolds() || !Equal(state.target.getCollisionObjectArray(), beforeObjects) ||
            !Equal(state.target.HistoryBodies(), beforeMoving) || !Equal(state.target.HistoryConstraints(), beforeConstraints) ||
            !Equal(state.world.getCollisionObjectArray(), privateObjects) || !Equal(state.world.HistoryBodies(), privateMoving) ||
            !Equal(state.world.HistoryConstraints(), privateConstraints) || !broadphase->Current()) return false;
        for (std::size_t i = 0; i < privateConstraints.size(); ++i)
            if (privateConstraints[i]->getUid() != constraintIds[i] ||
                !Contains(imported, &privateConstraints[i]->getRigidBodyA()) ||
                !Contains(imported, &privateConstraints[i]->getRigidBodyB())) return false;
        return true;
    }
    bool Publish() noexcept {
        if (!Current()) return false;
        auto& state = *owner;
        broadphase->Publish();
        import_storage::Exchange(state.target.getCollisionObjectArray(), afterObjects);
        import_storage::Exchange(state.target.HistoryBodies(), afterMoving);
        import_storage::Exchange(state.target.HistoryConstraints(), afterConstraints);
        import_storage::Exchange(state.world.getCollisionObjectArray(), emptyObjects);
        import_storage::Exchange(state.world.HistoryBodies(), emptyMoving);
        import_storage::Exchange(state.world.HistoryConstraints(), emptyConstraints);
        state.live.constraintId = endId;
        retained->impl_->importPending = false; retained->impl_->present = true; ++retained->impl_->generation;
        state.published = true; published = true;
        return true;
    }
};

ImportScene::ImportScene(PhysicsScene& live) {
    if (!live.world || !live.dispatcher) throw std::invalid_argument("missing live import world");
    impl_ = std::make_shared<Impl>(live);
}
ImportScene::~ImportScene() = default;
PhysicsScene& ImportScene::Scene() noexcept { return impl_->scene; }
std::unique_ptr<ImportScene::Publication> ImportScene::Prepare() {
    return std::unique_ptr<Publication>(new Publication(std::make_unique<Publication::Impl>(impl_)));
}
ImportScene::Publication::Publication(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ImportScene::Publication::~Publication() = default;
bool ImportScene::Publication::IsCurrent() const noexcept { return impl_->Current(); }
std::shared_ptr<Retained> ImportScene::Publication::RetainedOwner() const noexcept { return impl_->retained; }
bool ImportScene::Publication::MeasureTargetResources(plugins::resource_budget::Collector& out) const {
    if (!impl_->Current()) return false;
    impl_->retained->MeasureResources(out); return true;
}
bool ImportScene::Publication::Publish() noexcept { return impl_->Publish(); }
}
