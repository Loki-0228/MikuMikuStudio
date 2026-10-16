#include "mikudancestudio/history_physics.hpp"
#include "mikudancestudio/dynamics_world.hpp"
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

namespace mikudancestudio::history_physics {
Broadphase::Broadphase(const btVector3& lo, const btVector3& hi, unsigned handles)
    : Broadphase(lo, hi, handles, std::make_unique<PairCache>()) {}
Broadphase::Broadphase(const btVector3& lo, const btVector3& hi, unsigned handles,
                       std::unique_ptr<PairCache> cache)
    : bt32BitAxisSweep3(lo, hi, handles, cache.get()) { cache.release(); }
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
bool Current(const BodyState& s, bool parked = false) noexcept {
    const auto mask = parked ? short(0) : s.mask;
    if (s.body->getBroadphaseHandle() != s.proxy || s.body->getActivationState() != (parked ? DISABLE_SIMULATION : s.activation) ||
        s.body->getCollisionFlags() != s.flags || s.proxy->m_collisionFilterGroup != s.group ||
        s.proxy->m_collisionFilterMask != mask || s.body->getNumConstraintRefs() ||
        !EqualTransform(s.body->getWorldTransform(), s.transform) || !EqualTransform(s.body->getInterpolationWorldTransform(), s.interpolation) ||
        !EqualVector(s.body->getLinearVelocity(), s.velocity) || !EqualVector(s.body->getAngularVelocity(), s.spin) ||
        !EqualVector(s.body->getTotalForce(), s.force) || !EqualVector(s.body->getTotalTorque(), s.torque) ||
        s.body->getCollisionShape() != s.shape || s.body->getDeactivationTime() != s.deactivation ||
        !EqualVector(s.proxy->m_aabbMin, s.aabbLo) || !EqualVector(s.proxy->m_aabbMax, s.aabbHi)) return false;
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
    bool present = true;
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
};
struct Retained::Transition::Impl {
    struct Entry {
        std::shared_ptr<Retained::Impl> owner;
        std::uint64_t generation;
        std::unique_ptr<Saved> saved;
        explicit Entry(std::shared_ptr<Retained::Impl> o) : owner(std::move(o)), generation(owner->generation) {}
        const Saved& State(bool present) const { return present ? *owner->saved : *saved; }
    };
    std::vector<Entry> entries;
    DynamicsWorld& world;
    Broadphase& broadphase;
    bool present, consumed = false;
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
    Impl(std::vector<std::shared_ptr<Retained::Impl>> retained, bool p)
        : world(retained.front()->world), broadphase(retained.front()->broadphase), present(p),
          step(world.StepSerial()), source(broadphase.Cache()) {
        if (world.getBroadphase() != &broadphase || world.HasActions() || source->getOverlapFilterCallback() ||
            source->HasGhostCallback() || broadphase.getOverlappingPairUserCallback())
            throw std::invalid_argument("unsupported physics membership transition");
        std::unordered_set<Retained::Impl*> owners;
        std::unordered_set<btRigidBody*> ownedBodies;
        std::unordered_set<btTypedConstraint*> ownedConstraints;
        entries.reserve(retained.size());
        for (auto& owner : retained) {
            if (&owner->world != &world || owner->present == p || !owners.insert(owner.get()).second)
                throw std::invalid_argument("incompatible physics membership batch");
            for (auto* body : owner->bodies)
                if (!ownedBodies.insert(body).second) throw std::invalid_argument("overlapping retained bodies");
            for (auto* constraint : owner->constraints)
                if (!ownedConstraints.insert(constraint).second) throw std::invalid_argument("overlapping retained constraints");
            entries.emplace_back(std::move(owner));
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
        if (!p) order = std::make_shared<OrderSnapshot>(OrderSnapshot{
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
            if (!p) {
                entry.saved = std::make_unique<Saved>(); entry.saved->bodies = std::move(currentBodies);
                entry.saved->order = order;
                entry.saved->objects.Capture(beforeObjects, objects); entry.saved->moving.Capture(beforeMoving, moving);
                entry.saved->constraints.Capture(beforeConstraints, o.constraints);
            } else {
                if (!o.saved || !o.saved->order) throw std::invalid_argument("missing parked physics state");
                for (const auto& body : o.saved->bodies)
                    if (!history_physics::Current(body, true)) throw std::invalid_argument("parked physics body was changed");
                restored.insert(restored.end(), o.saved->bodies.begin(), o.saved->bodies.end());
            }
            const auto& state = entry.State(p);
            objectGroups.push_back(&state.objects); movingGroups.push_back(&state.moving);
            constraintGroups.push_back(&state.constraints); orders.push_back(state.order.get());
        }
        std::sort(orders.begin(), orders.end(), [](const auto* a, const auto* b) { return a->serial < b->serial; });
        orders.erase(std::unique(orders.begin(), orders.end()), orders.end());
        if (p) {
            std::vector<const std::vector<btCollisionObject*>*> objectOrders;
            std::vector<const std::vector<btRigidBody*>*> movingOrders;
            std::vector<const std::vector<btTypedConstraint*>*> constraintOrders;
            for (const auto* savedOrder : orders) {
                objectOrders.push_back(&savedOrder->objects); movingOrders.push_back(&savedOrder->moving);
                constraintOrders.push_back(&savedOrder->constraints);
            }
            afterObjects = RestoreOrder(beforeObjects, objectGroups, objectOrders);
            afterMoving = RestoreOrder(beforeMoving, movingGroups, movingOrders);
            afterConstraints = RestoreOrder(beforeConstraints, constraintGroups, constraintOrders);
        } else {
            afterObjects = beforeObjects; afterMoving = beforeMoving; afterConstraints = beforeConstraints;
            for (const auto& entry : entries) {
                afterObjects = entry.saved->objects.Apply(afterObjects, false);
                afterMoving = entry.saved->moving.Apply(afterMoving, false);
                afterConstraints = entry.saved->constraints.Apply(afterConstraints, false);
            }
        }
        cache = std::make_unique<PairCache>();
        pairs = Copy(source->getOverlappingPairArray());
        Filter filter(restored);
        if (p) cache->setOverlapFilterCallback(&filter);
        for (const auto& pair : pairs) {
            const bool target = ownedBodies.count(btRigidBody::upcast(static_cast<btCollisionObject*>(pair.m_pProxy0->m_clientObject))) ||
                                ownedBodies.count(btRigidBody::upcast(static_cast<btCollisionObject*>(pair.m_pProxy1->m_clientObject)));
            if (p || !target) cache->addOverlappingPair(pair.m_pProxy0, pair.m_pProxy1);
        }
        if (p) for (const auto& body : restored) for (auto* other : afterObjects) {
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
            source->getNumOverlappingPairs() != static_cast<int>(pairs.size())) return false;
        for (const auto& entry : entries)
            if (entry.owner->generation != entry.generation || entry.owner->present == present) return false;
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
        for (const auto& entry : entries) for (const auto& body : entry.State(present).bodies) {
            Broadphase::SetMask(body.proxy, present ? body.mask : 0);
            body.body->forceActivationState(present ? body.activation : DISABLE_SIMULATION);
        }
        Replace(world.getCollisionObjectArray(), afterObjects);
        Replace(world.HistoryBodies(), afterMoving); Replace(world.HistoryConstraints(), afterConstraints);
        cache.reset(broadphase.Exchange(cache.release()));
        for (auto& entry : entries) {
            if (!present) entry.owner->saved.swap(entry.saved);
            entry.owner->present = present; ++entry.owner->generation;
        }
        consumed = true;
        return true;
    }
};
Retained::Retained(DynamicsWorld& world, std::vector<btRigidBody*> bodies, std::vector<btTypedConstraint*> constraints)
    : impl_(std::make_shared<Impl>(world, std::move(bodies), std::move(constraints))) {}
Retained::~Retained() = default;
bool Retained::Present() const noexcept { return impl_->present; }
std::unique_ptr<Retained::Transition> Retained::Prepare(bool present) {
    return PrepareBatch({this}, present);
}
std::unique_ptr<Retained::Transition> Retained::PrepareBatch(const std::vector<Retained*>& objects, bool present) {
    if (objects.empty()) throw std::invalid_argument("empty physics membership batch");
    std::vector<std::shared_ptr<Impl>> retained; retained.reserve(objects.size());
    for (auto* object : objects) {
        if (!object) throw std::invalid_argument("null retained physics object");
        retained.push_back(object->impl_);
    }
    return std::unique_ptr<Transition>(new Transition(std::make_unique<Transition::Impl>(std::move(retained), present)));
}
Retained::Transition::Transition(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Retained::Transition::~Transition() = default;
bool Retained::Transition::IsCurrent() const noexcept { return impl_->Current(); }
bool Retained::Transition::Publish() noexcept { return impl_->Publish(); }
}
