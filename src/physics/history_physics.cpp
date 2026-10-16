#include "mikudancestudio/history_physics.hpp"
#include "mikudancestudio/dynamics_world.hpp"
#include "BulletDynamics/Dynamics/btRigidBody.h"
#include "BulletDynamics/ConstraintSolver/btTypedConstraint.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mikudancestudio::history_physics {
Broadphase::Broadphase(const btVector3& lo, const btVector3& hi, unsigned handles)
    : Broadphase(lo, hi, handles, std::make_unique<PairCache>()) {}
Broadphase::Broadphase(const btVector3& lo, const btVector3& hi, unsigned handles,
                       std::unique_ptr<PairCache> cache)
    : bt32BitAxisSweep3(lo, hi, handles, cache.get()) { cache.release(); }
Broadphase::~Broadphase() { delete m_pairCache; m_pairCache = nullptr; }
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
        body->getActivationState(), body->getCollisionFlags(), body->getWorldTransform(), body->getInterpolationWorldTransform(),
        body->getLinearVelocity(), body->getAngularVelocity(), body->getTotalForce(), body->getTotalTorque(),
        body->getCollisionShape(), body->getDeactivationTime(), proxy->m_aabbMin, proxy->m_aabbMax};
}
bool Current(const BodyState& s, bool parked = false) noexcept {
    const auto mask = parked ? short(0) : s.mask;
    if (s.body->getBroadphaseHandle() != s.proxy || s.body->getActivationState() != (parked ? DISABLE_SIMULATION : s.activation) ||
        s.body->getCollisionFlags() != s.flags || s.proxy->m_collisionFilterGroup != s.group ||
        s.proxy->m_collisionFilterMask != mask || s.body->getNumConstraintRefs() ||
        !(s.body->getWorldTransform() == s.transform) || !(s.body->getInterpolationWorldTransform() == s.interpolation) ||
        s.body->getLinearVelocity() != s.velocity || s.body->getAngularVelocity() != s.spin ||
        s.body->getTotalForce() != s.force || s.body->getTotalTorque() != s.torque ||
        s.body->getCollisionShape() != s.shape || s.body->getDeactivationTime() != s.deactivation ||
        s.proxy->m_aabbMin != s.aabbLo || s.proxy->m_aabbMax != s.aabbHi) return false;
    auto* ray = Broadphase::RayProxy(s.proxy);
    return !ray || (ray->m_collisionFilterGroup == s.group && ray->m_collisionFilterMask == mask);
}
struct ProxyState {
    btCollisionObject* object;
    btBroadphaseProxy* proxy;
    btVector3 lo, hi;
    short group, mask;
    explicit ProxyState(btCollisionObject* obj) : object(obj), proxy(obj->getBroadphaseHandle()),
        lo(proxy->m_aabbMin), hi(proxy->m_aabbMax), group(proxy->m_collisionFilterGroup), mask(proxy->m_collisionFilterMask) {}
    bool Current() const noexcept {
        return object->getBroadphaseHandle() == proxy && proxy->m_aabbMin == lo && proxy->m_aabbMax == hi &&
            proxy->m_collisionFilterGroup == group && proxy->m_collisionFilterMask == mask;
    }
};
struct Saved {
    Removed<btCollisionObject*> objects;
    Removed<btRigidBody*> moving;
    Removed<btTypedConstraint*> constraints;
    std::vector<BodyState> bodies;
};
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
    std::shared_ptr<Retained::Impl> lifetime;
    Retained::Impl& owner;
    bool present, consumed = false;
    std::uint64_t generation, step;
    PairCache* source;
    std::unique_ptr<PairCache> cache;
    std::vector<btBroadphasePair> pairs;
    std::vector<std::pair<size_t, size_t>> transfers;
    std::vector<btCollisionObject*> beforeObjects, afterObjects;
    std::vector<btRigidBody*> beforeMoving, afterMoving;
    std::vector<btTypedConstraint*> beforeConstraints, afterConstraints;
    std::vector<ProxyState> proxies;
    std::vector<BodyState> bodies;
    std::unique_ptr<Saved> saved;
    Impl(std::shared_ptr<Retained::Impl> retained, bool p)
        : lifetime(std::move(retained)), owner(*lifetime), present(p), generation(owner.generation),
          step(owner.world.StepSerial()), source(owner.broadphase.Cache()) {
        auto& o = owner;
        if (o.present == p || o.world.HasActions() || source->getOverlapFilterCallback() ||
            source->HasGhostCallback() || o.broadphase.getOverlappingPairUserCallback())
            throw std::invalid_argument("unsupported physics membership transition");
        beforeObjects = Copy(o.world.getCollisionObjectArray());
        beforeMoving = Copy(o.world.HistoryBodies()); beforeConstraints = Copy(o.world.HistoryConstraints());
        for (auto* object : beforeObjects) {
            if (!object->getBroadphaseHandle()) throw std::invalid_argument("missing active proxy");
            proxies.emplace_back(object);
        }
        std::vector<btCollisionObject*> objects;
        std::vector<btRigidBody*> moving;
        for (auto* body : o.bodies) {
            if (body->getNumConstraintRefs()) throw std::invalid_argument("filtered constraint references");
            bodies.push_back(Read(body));
            objects.push_back(body);
            if (!body->isStaticObject()) moving.push_back(body);
        }
        // An unretained joint must never keep a parked body in an active island.
        for (auto* joint : beforeConstraints) {
            if ((Contains(o.bodies, &joint->getRigidBodyA()) || Contains(o.bodies, &joint->getRigidBodyB())) &&
                !Contains(o.constraints, joint)) throw std::invalid_argument("external retained constraint");
        }
        if (!p) {
            saved = std::make_unique<Saved>(); saved->bodies = bodies;
            saved->objects.Capture(beforeObjects, objects); saved->moving.Capture(beforeMoving, moving);
            saved->constraints.Capture(beforeConstraints, o.constraints);
        } else {
            if (!o.saved) throw std::invalid_argument("missing parked physics state");
            for (const auto& body : o.saved->bodies)
                if (!history_physics::Current(body, true)) throw std::invalid_argument("parked physics body was changed");
        }
        const auto& state = p ? *o.saved : *saved;
        afterObjects = state.objects.Apply(beforeObjects, p);
        afterMoving = state.moving.Apply(beforeMoving, p);
        afterConstraints = state.constraints.Apply(beforeConstraints, p);
        cache = std::make_unique<PairCache>();
        pairs = Copy(source->getOverlappingPairArray());
        Filter filter(state.bodies);
        if (p) cache->setOverlapFilterCallback(&filter);
        for (const auto& pair : pairs) {
            const bool target = Contains(objects, static_cast<btCollisionObject*>(pair.m_pProxy0->m_clientObject)) ||
                                Contains(objects, static_cast<btCollisionObject*>(pair.m_pProxy1->m_clientObject));
            if (p || !target) cache->addOverlappingPair(pair.m_pProxy0, pair.m_pProxy1);
        }
        if (p) for (const auto& body : state.bodies) for (auto* other : afterObjects) {
            auto* proxy = other->getBroadphaseHandle();
            if (proxy != body.proxy && o.broadphase.testAabbOverlap(body.proxy, proxy))
                cache->addOverlappingPair(body.proxy, proxy);
        }
        cache->setOverlapFilterCallback(nullptr);
        for (size_t i = 0; i < pairs.size(); ++i) {
            if (auto* target = cache->findPair(pairs[i].m_pProxy0, pairs[i].m_pProxy1))
                transfers.emplace_back(i, target - cache->getOverlappingPairArrayPtr());
        }
        // These operations may allocate; publication only writes reserved slots.
        o.world.getCollisionObjectArray().reserve(static_cast<int>(afterObjects.size()));
        o.world.HistoryBodies().reserve(static_cast<int>(afterMoving.size()));
        o.world.HistoryConstraints().reserve(static_cast<int>(afterConstraints.size()));
    }
    ~Impl() {
        // After publication this is the old cache. Surviving algorithms were
        // moved to the new cache; only deleted contacts are destroyed here.
        if (cache) for (int i = 0; i < cache->getNumOverlappingPairs(); ++i)
            cache->cleanOverlappingPair(cache->getOverlappingPairArray()[i], owner.world.getDispatcher());
    }
    bool Current() const noexcept {
        if (consumed || owner.generation != generation || owner.world.StepSerial() != step ||
            owner.present == present || owner.broadphase.Cache() != source || owner.world.HasActions() ||
            source->getOverlapFilterCallback() || source->HasGhostCallback() || owner.broadphase.getOverlappingPairUserCallback() ||
            !Equal(owner.world.getCollisionObjectArray(), beforeObjects) ||
            !Equal(owner.world.HistoryBodies(), beforeMoving) || !Equal(owner.world.HistoryConstraints(), beforeConstraints) ||
            source->getNumOverlappingPairs() != static_cast<int>(pairs.size())) return false;
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
        const auto& state = present ? *owner.saved : *saved;
        for (const auto& body : state.bodies) {
            Broadphase::SetMask(body.proxy, present ? body.mask : 0);
            body.body->forceActivationState(present ? body.activation : DISABLE_SIMULATION);
        }
        Replace(owner.world.getCollisionObjectArray(), afterObjects);
        Replace(owner.world.HistoryBodies(), afterMoving); Replace(owner.world.HistoryConstraints(), afterConstraints);
        cache.reset(owner.broadphase.Exchange(cache.release()));
        if (!present) owner.saved.swap(saved);
        owner.present = present; ++owner.generation; consumed = true;
        return true;
    }
};
Retained::Retained(DynamicsWorld& world, std::vector<btRigidBody*> bodies, std::vector<btTypedConstraint*> constraints)
    : impl_(std::make_shared<Impl>(world, std::move(bodies), std::move(constraints))) {}
Retained::~Retained() = default;
bool Retained::Present() const noexcept { return impl_->present; }
std::unique_ptr<Retained::Transition> Retained::Prepare(bool present) {
    return std::unique_ptr<Transition>(new Transition(std::make_unique<Transition::Impl>(impl_, present)));
}
Retained::Transition::Transition(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Retained::Transition::~Transition() = default;
bool Retained::Transition::IsCurrent() const noexcept { return impl_->Current(); }
bool Retained::Transition::Publish() noexcept { return impl_->Publish(); }
}
