#include "BulletDynamics/Dynamics/btActionInterface.h"
#include "mikudancestudio/bullet275_storage.hpp"
#include "../src/plugins/scene_resource_budget.hpp"
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {
bool trackBulletAlloc = false;
unsigned bulletAllocations = 0;
bool observeBulletStorage = false;
unsigned observedStorageAllocations = 0;
void* observedStoragePointer = nullptr;
size_t observedStorageBytes = 0;
bool observedStorageRaw = false;
void* HistoryAlloc(size_t size) {
    if (trackBulletAlloc) ++bulletAllocations;
    void* const pointer = std::malloc(size);
    if (observeBulletStorage) {
        ++observedStorageAllocations; observedStoragePointer = pointer; observedStorageBytes = size;
        observedStorageRaw = true;
    }
    return pointer;
}
void HistoryFree(void* p) { std::free(p); }
void* HistoryAlignedAlloc(size_t size, int alignment) {
    if (trackBulletAlloc) ++bulletAllocations;
    void* const pointer = _aligned_malloc(size, alignment);
    if (observeBulletStorage) {
        ++observedStorageAllocations; observedStoragePointer = pointer; observedStorageBytes = size;
        observedStorageRaw = false;
    }
    return pointer;
}
void HistoryAlignedFree(void* p) { _aligned_free(p); }
struct PhysicsFixture {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
    history_physics::Broadphase broadphase{btVector3(-100, -100, -100), btVector3(100, 100, 100), 256};
    btSequentialImpulseConstraintSolver solver;
    DynamicsWorld world{&dispatcher, &broadphase, &solver, &config};
    btSphereShape shape{btScalar(0.5)};
    std::vector<std::unique_ptr<btRigidBody>> bodies;
    std::unique_ptr<btGeneric6DofConstraint> joint;
    explicit PhysicsFixture(bool detectCollisions = true) {
        world.setGravity(btVector3(0, -10, 0));
        for (const auto& pos : {btVector3(0, 5, 0), btVector3(0, 7, 0), btVector3(0, 5.8f, 0),
                              btVector3(10, 5, 0), btVector3(10, 5.8f, 0)}) {
            btVector3 inertia; shape.calculateLocalInertia(1, inertia);
            auto body = std::make_unique<btRigidBody>(1, nullptr, &shape, inertia);
            btTransform transform; transform.setIdentity(); transform.setOrigin(pos);
            body->setWorldTransform(transform); body->setInterpolationWorldTransform(transform);
            body->forceActivationState(DISABLE_DEACTIVATION);
            world.addRigidBody(body.get(), 1, -1); bodies.push_back(std::move(body));
        }
        auto frameA = btTransform::getIdentity(); frameA.setOrigin(btVector3(0, 2, 0));
        joint = std::make_unique<btGeneric6DofConstraint>(*bodies[0], *bodies[1], frameA, btTransform::getIdentity(), true);
        world.addConstraint(joint.get(), false);
        if (detectCollisions) world.performDiscreteCollisionDetection();
    }
    ~PhysicsFixture() {
        world.removeConstraint(joint.get());
        // removeRigidBody also releases proxies retained outside active arrays.
        for (auto& body : bodies) world.removeRigidBody(body.get());
    }
    std::unique_ptr<history_physics::Retained> Capture() {
        return std::make_unique<history_physics::Retained>(world,
            std::vector<btRigidBody*>{bodies[0].get(), bodies[1].get()},
            std::vector<btTypedConstraint*>{joint.get()});
    }
    bool RayHitsFirst() {
        const btVector3 from(-3, 5, 0), to(3, 5, 0);
        struct Hits final : btCollisionWorld::RayResultCallback {
            btCollisionObject* target; bool hit = false;
            explicit Hits(btCollisionObject* p) : target(p) {}
            btScalar addSingleResult(btCollisionWorld::LocalRayResult& result, bool) override {
                if (result.m_collisionObject == target) hit = true;
                return 1;
            }
        } ray(bodies[0].get());
        world.rayTest(from, to, ray); return ray.hit;
    }
    bool ContactsFirst() {
        for (int i = 0; i < dispatcher.getNumManifolds(); ++i) {
            auto* manifold = dispatcher.getManifoldByIndexInternal(i);
            if (manifold->getBody0() == bodies[0].get() || manifold->getBody1() == bodies[0].get()) return true;
        }
        return false;
    }
};
struct HistoryAction final : btActionInterface {
    unsigned calls = 0;
    void updateAction(btCollisionWorld*, btScalar) override { ++calls; }
    void debugDraw(btIDebugDraw*) override { ++calls; }
};
struct HistoryOverlapFilter final : btOverlapFilterCallback {
    mutable unsigned calls = 0;
    bool needBroadphaseCollision(btBroadphaseProxy*, btBroadphaseProxy*) const override { ++calls; return true; }
};
struct HistoryPairCallback final : btOverlappingPairCallback {
    unsigned calls = 0;
    btBroadphasePair* addOverlappingPair(btBroadphaseProxy*, btBroadphaseProxy*) override { ++calls; return nullptr; }
    void* removeOverlappingPair(btBroadphaseProxy*, btBroadphaseProxy*, btDispatcher*) override { ++calls; return nullptr; }
    void removeOverlappingPairsContainingProxy(btBroadphaseProxy*, btDispatcher*) override { ++calls; }
};
bool RejectPhysicsPrepare(history_physics::Retained& retained, bool present) {
    try { auto candidate = retained.Prepare(present); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}
bool RejectPhysicsBatch(const std::vector<history_physics::Retained*>& retained, bool present) {
    try { auto candidate = history_physics::Retained::PrepareBatch(retained, present); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}
void CheckPhysicsOrder(PhysicsFixture& f, const std::vector<unsigned>& indices) {
    Check(f.world.getNumCollisionObjects() == static_cast<int>(indices.size()) &&
          f.world.HistoryBodies().size() == static_cast<int>(indices.size()), "batch physics array counts");
    for (size_t i = 0; i < indices.size(); ++i) {
        Check(f.world.getCollisionObjectArray()[static_cast<int>(i)] == f.bodies[indices[i]].get(), "batch collision order");
        Check(f.world.HistoryBodies()[static_cast<int>(i)] == f.bodies[indices[i]].get(), "batch moving-body order");
    }
}
void PublishPhysicsBatch(std::unique_ptr<history_physics::Retained::Transition> candidate) {
    bulletAllocations = 0; trackBulletAlloc = true; failAfter = 0;
    const bool published = candidate->Publish();
    failAfter = -1; trackBulletAlloc = false;
    Check(published && bulletAllocations == 0, "batch publishes without C++ or Bullet allocation");
    Check(!candidate->IsCurrent() && !candidate->Publish(), "batch publication consumes the entire candidate");
}
void PhysicsGuardCoordinates() {
    const auto nan = std::numeric_limits<btScalar>::quiet_NaN();
    const auto sameCoordinates = [](const btVector3& a, const btVector3& b) {
        return a.x() == b.x() && a.y() == b.y() && a.z() == b.z();
    };
    {
        PhysicsFixture f(false);
        for (const auto& body : f.bodies) {
            btVector3 lo, hi; f.shape.getAabb(body->getWorldTransform(), lo, hi);
            auto* proxy = body->getBroadphaseHandle();
            Check(sameCoordinates(proxy->m_aabbMin, lo) && sameCoordinates(proxy->m_aabbMax, hi),
                  "new sweep proxy has initialized AABB before the first collision pass");
            Check(proxy->m_aabbMin.w() == 0 && proxy->m_aabbMax.w() == 0, "new sweep proxy initializes unused AABB components");
        }
        auto* body = f.bodies[0].get(); auto* oldProxy = body->getBroadphaseHandle();
        f.world.removeConstraint(f.joint.get()); f.world.removeRigidBody(body);
        // The sweep handle pool keeps this object alive and reuses it first.
        // Poison all coordinates, not only W, to reproduce the Bullet 2.75
        // createProxy gap independently of allocator or stack contents.
        oldProxy->m_aabbMin.setValue(nan, nan, nan); oldProxy->m_aabbMax.setValue(nan, nan, nan);
        f.world.addRigidBody(body, 1, -1); f.world.addConstraint(f.joint.get(), false);
        btVector3 lo, hi; f.shape.getAabb(body->getWorldTransform(), lo, hi);
        Check(body->getBroadphaseHandle() == oldProxy && sameCoordinates(oldProxy->m_aabbMin, lo) && sameCoordinates(oldProxy->m_aabbMax, hi),
              "reused sweep handle replaces poisoned bounds immediately");
        Check(oldProxy->m_aabbMin.w() == 0 && oldProxy->m_aabbMax.w() == 0, "reused sweep handle initializes unused AABB components");
        auto retained = f.Capture();
        PublishPhysicsBatch(retained->Prepare(false));
        PublishPhysicsBatch(retained->Prepare(true));
        Check(retained->Present() && f.world.getNumCollisionObjects() == 5,
              "physics history works before any simulation or collision pass");
    }
    {
        PhysicsFixture f; auto retained = f.Capture();
        auto* body = f.bodies[0].get(); auto* proxy = body->getBroadphaseHandle();
        const auto unused = [&](btScalar value) {
            auto transform = body->getWorldTransform(); transform.getOrigin()[3] = value;
            auto interpolation = body->getInterpolationWorldTransform(); interpolation.getOrigin()[3] = value;
            for (int i = 0; i < 3; ++i) {
                transform.getBasis()[i][3] = value; interpolation.getBasis()[i][3] = value;
            }
            body->setWorldTransform(transform); body->setInterpolationWorldTransform(interpolation);
            auto velocity = body->getLinearVelocity(); velocity[3] = value; body->setLinearVelocity(velocity);
            auto spin = body->getAngularVelocity(); spin[3] = value; body->setAngularVelocity(spin);
            const_cast<btVector3&>(body->getTotalForce())[3] = value;
            const_cast<btVector3&>(body->getTotalTorque())[3] = value;
            proxy->m_aabbMin[3] = value; proxy->m_aabbMax[3] = value;
            f.bodies[2]->getBroadphaseHandle()->m_aabbMin[3] = value;
            f.bodies[2]->getBroadphaseHandle()->m_aabbMax[3] = value;
        };
        unused(nan);
        auto candidate = retained->Prepare(false);
        Check(candidate->IsCurrent(), "NaN in unused vector and transform components does not stale physics history");
        unused(btScalar(17));
        Check(candidate->IsCurrent(), "changes to unused W components do not stale physics history");
        const auto loX = proxy->m_aabbMin.x(); proxy->m_aabbMin[0] += btScalar(1);
        Check(!candidate->IsCurrent(), "physical AABB coordinate mutation still invalidates history");
        proxy->m_aabbMin[0] = loX;
        auto transform = body->getWorldTransform(); transform.getBasis()[0][0] += btScalar(1);
        body->setWorldTransform(transform);
        Check(!candidate->IsCurrent(), "physical basis coordinate mutation still invalidates history");
        transform.getBasis()[0][0] -= btScalar(1); body->setWorldTransform(transform);
        auto interpolation = body->getInterpolationWorldTransform(); interpolation.getOrigin()[0] += btScalar(1);
        body->setInterpolationWorldTransform(interpolation);
        Check(!candidate->IsCurrent(), "physical interpolation coordinate mutation still invalidates history");
        interpolation.getOrigin()[0] -= btScalar(1); body->setInterpolationWorldTransform(interpolation);
        PublishPhysicsBatch(std::move(candidate));
        unused(nan);
        PublishPhysicsBatch(retained->Prepare(true));
        Check(retained->Present(), "parked-state validation also ignores unused components");
    }
}
void PhysicsBatchRetention() {
    using Retained = history_physics::Retained;
    {
        PhysicsFixture f;
        auto a = f.Capture();
        auto b = std::make_unique<Retained>(f.world, std::vector<btRigidBody*>{f.bodies[2].get()}, std::vector<btTypedConstraint*>{});
        auto* cache = f.broadphase.Cache();
        auto* unrelated = cache->findPair(f.bodies[3]->getBroadphaseHandle(), f.bodies[4]->getBroadphaseHandle())->m_algorithm;
        auto cancel = Retained::PrepareBatch({b.get(), a.get()}, false);
        Check(cancel->IsCurrent() && a->Present() && b->Present() && f.broadphase.Cache() == cache,
              "batch preparation leaves every owner and the live cache unchanged");
        cancel.reset();
        Check(f.ContactsFirst() && f.RayHitsFirst(), "batch cancellation retains contact and ray state");
        auto oldSingle = a->Prepare(false);
        PublishPhysicsBatch(Retained::PrepareBatch({b.get(), a.get()}, false));
        Check(!a->Present() && !b->Present() && !f.ContactsFirst() && !f.RayHitsFirst() && f.world.getNumConstraints() == 0,
              "one batch removes both owners and their joint");
        Check(!oldSingle->IsCurrent() && !oldSingle->Publish(), "batch invalidates outstanding single-owner candidate");
        oldSingle.reset();
        CheckPhysicsOrder(f, {3, 4});
        Check(f.broadphase.Cache()->findPair(f.bodies[3]->getBroadphaseHandle(), f.bodies[4]->getBroadphaseHandle())->m_algorithm == unrelated,
              "batch preserves unrelated contact algorithm ownership");
        PublishPhysicsBatch(Retained::PrepareBatch({a.get(), b.get()}, true));
        Check(a->Present() && b->Present() && f.world.getConstraint(0) == f.joint.get(), "batch restores both owners and exact joint");
        CheckPhysicsOrder(f, {0, 1, 2, 3, 4});
        f.world.performDiscreteCollisionDetection();
        Check(f.ContactsFirst(), "batch restoration regenerates contacts between restored owners");
        // Independent old deletion histories can be restored together in either
        // caller order, despite having captured different live array indices.
        PublishPhysicsBatch(a->Prepare(false));
        PublishPhysicsBatch(b->Prepare(false));
        PublishPhysicsBatch(Retained::PrepareBatch({b.get(), a.get()}, true));
        CheckPhysicsOrder(f, {0, 1, 2, 3, 4});
        // Owners from one batch remain independently usable. Restoring the
        // later-positioned owner first must still place it before live peers.
        PublishPhysicsBatch(Retained::PrepareBatch({a.get(), b.get()}, false));
        PublishPhysicsBatch(b->Prepare(true));
        CheckPhysicsOrder(f, {2, 3, 4});
        PublishPhysicsBatch(a->Prepare(true));
        CheckPhysicsOrder(f, {0, 1, 2, 3, 4});
        for (unsigned i = 0; i < 12; ++i) {
            PublishPhysicsBatch(Retained::PrepareBatch({b.get(), a.get()}, false));
            PublishPhysicsBatch(Retained::PrepareBatch({a.get(), b.get()}, true));
            CheckPhysicsOrder(f, {0, 1, 2, 3, 4});
        }
        auto stale = Retained::PrepareBatch({a.get(), b.get()}, false);
        f.bodies[2]->setLinearVelocity(btVector3(1, 2, 3));
        Check(!stale->IsCurrent() && !stale->Publish() && a->Present() && b->Present(),
              "one owner mutation rejects the whole batch before any publication");
        stale.reset(); f.bodies[2]->setLinearVelocity(btVector3(0, 0, 0));
        stale = Retained::PrepareBatch({a.get(), b.get()}, false);
        history_physics::Broadphase replacement(btVector3(-100, -100, -100), btVector3(100, 100, 100), 256);
        f.world.setBroadphase(&replacement);
        Check(!stale->IsCurrent() && !stale->Publish() && RejectPhysicsBatch({a.get(), b.get()}, false) && a->Present() && b->Present(),
              "replaced world broadphase rejects batch preparation and publication");
        f.world.setBroadphase(&f.broadphase); stale.reset();
        CheckPhysicsOrder(f, {0, 1, 2, 3, 4});
        Check(RejectPhysicsBatch({}, false) && RejectPhysicsBatch({nullptr}, false) &&
              RejectPhysicsBatch({a.get(), a.get()}, false), "empty null and duplicate-owner batches are rejected");
        Retained overlap(f.world, {f.bodies[0].get()}, {});
        Check(RejectPhysicsBatch({a.get(), &overlap}, false), "overlapping body ownership rejects the whole batch");
        auto cross = std::make_unique<btPoint2PointConstraint>(*f.bodies[0], *f.bodies[2], btVector3(0,0,0), btVector3(0,0,0));
        f.world.addConstraint(cross.get(), false);
        Check(RejectPhysicsBatch({a.get(), b.get()}, false) && a->Present() && b->Present(),
              "cross-owner joint is explicitly rejected even when both owners are requested");
        f.world.removeConstraint(cross.get());
        Retained empty(f.world, {}, {});
        PublishPhysicsBatch(Retained::PrepareBatch({a.get(), &empty}, false));
        Check(!a->Present() && !empty.Present(), "batch includes an empty retained object");
        Check(RejectPhysicsBatch({a.get(), b.get()}, true), "mixed initial presence rejects the whole batch");
        PublishPhysicsBatch(Retained::PrepareBatch({&empty, a.get()}, true));
        CheckPhysicsOrder(f, {0, 1, 2, 3, 4});
        PhysicsFixture second;
        auto differentWorld = second.Capture();
        Check(RejectPhysicsBatch({a.get(), differentWorld.get()}, false) && a->Present() && differentWorld->Present(),
              "mixed worlds are rejected without changing either world");
    }
    {
        PhysicsFixture f;
        auto a = f.Capture();
        auto b = std::make_unique<Retained>(f.world, std::vector<btRigidBody*>{f.bodies[2].get()}, std::vector<btTypedConstraint*>{});
        for (const bool restore : {false, true}) {
            bool reached = false; unsigned failures = 0;
            auto* cache = f.broadphase.Cache();
            for (long fail = 0; fail < 512; ++fail) {
                failAfter = fail;
                try { auto candidate = Retained::PrepareBatch({b.get(), a.get()}, restore); failAfter = -1; reached = true; }
                catch (const std::bad_alloc&) { failAfter = -1; ++failures; }
                Check(a->Present() == !restore && b->Present() == !restore && f.broadphase.Cache() == cache &&
                      f.world.getNumCollisionObjects() == (restore ? 2 : 5) && f.world.getNumConstraints() == (restore ? 0 : 1),
                      "every batch preparation allocation failure preserves all owners and live membership");
                if (reached) break;
            }
            Check(reached && failures > 15, "batch allocation failure sweep reaches full preparation");
            PublishPhysicsBatch(Retained::PrepareBatch({a.get(), b.get()}, restore));
        }
        CheckPhysicsOrder(f, {0, 1, 2, 3, 4});
        auto batch = Retained::PrepareBatch({a.get(), b.get()}, false);
        a.reset(); b.reset();
        Check(batch->IsCurrent(), "batch keeps all owner internals alive");
        PublishPhysicsBatch(std::move(batch));
        CheckPhysicsOrder(f, {3, 4});
        Check(!f.ContactsFirst(), "batch owner release still retires removed contact algorithms");
    }
    {
        PhysicsFixture f;
        auto a = f.Capture();
        PublishPhysicsBatch(a->Prepare(false));
        // An unmanaged reorder that contradicts the historical order must be
        // rejected during preparation, rather than silently changing live peers.
        std::swap(f.world.getCollisionObjectArray()[0], f.world.getCollisionObjectArray()[1]);
        Check(RejectPhysicsPrepare(*a, true) && !a->Present() && f.world.getNumCollisionObjects() == 3,
              "conflicting live and historical order rejects restoration atomically");
        std::swap(f.world.getCollisionObjectArray()[0], f.world.getCollisionObjectArray()[1]);
        PublishPhysicsBatch(a->Prepare(true));
        CheckPhysicsOrder(f, {0, 1, 2, 3, 4});
    }
    {
        PhysicsFixture f;
        auto a = f.Capture();
        auto joint = std::make_unique<btPoint2PointConstraint>(*f.bodies[3], *f.bodies[4], btVector3(0,0,0), btVector3(0,0,0));
        f.world.addConstraint(joint.get(), false);
        Retained b(f.world, {f.bodies[3].get(), f.bodies[4].get()}, {joint.get()});
        PublishPhysicsBatch(a->Prepare(false));
        PublishPhysicsBatch(b.Prepare(false));
        PublishPhysicsBatch(Retained::PrepareBatch({&b, a.get()}, true));
        CheckPhysicsOrder(f, {0, 1, 2, 3, 4});
        Check(f.world.getNumConstraints() == 2 && f.world.getConstraint(0) == f.joint.get() && f.world.getConstraint(1) == joint.get(),
              "independent joint histories restore exact constraint order in one batch");
        f.world.removeConstraint(joint.get());
    }
}
bool SamePhysicsResources(const plugins::resource_budget::Report& a, const plugins::resource_budget::Report& b) {
    return a.cpuBytes == b.cpuBytes && a.d3d9Bytes == b.d3d9Bytes &&
        a.totalBytes == b.totalBytes && a.missing == b.missing;
}
void PhysicsConstraintStorageChecks() {
    namespace rb = plugins::resource_budget;
    namespace storage = bullet275_storage;
    PhysicsFixture f; auto* body = f.bodies[0].get();
    const btRigidBody& readOnlyBody = *body;
    storage::Allocation initial{body, 999};
    failAfter = 0;
    const bool empty = storage::ConstraintReferences(readOnlyBody, initial);
    failAfter = -1;
    Check(empty && !initial.identity && initial.capacity == 0,
          "fresh rigid-body reference storage reads zero capacity without allocation or element access");
    rb::Collector baseline; rb::Body(body, baseline);
    Check(baseline.Result().missing == 0 && baseline.Result().cpuBytes == sizeof(btRigidBody) + sizeof(btSphereShape),
          "exact supported rigid body with an empty reference array has a complete payload estimate");

    std::vector<std::unique_ptr<btPoint2PointConstraint>> refs;
    for (unsigned i = 0; i < 9; ++i)
        refs.push_back(std::make_unique<btPoint2PointConstraint>(*body, *f.bodies[1], btVector3(0, 0, 0), btVector3(0, 0, 0)));
    storage::Allocation previous;
    for (unsigned i = 0; i < refs.size(); ++i) {
        observedStorageAllocations = 0; observedStoragePointer = nullptr; observedStorageBytes = 0;
        observeBulletStorage = true; body->addConstraintRef(refs[i].get()); observeBulletStorage = false;
        storage::Allocation allocation;
        Check(storage::ConstraintReferences(readOnlyBody, allocation) && allocation.capacity >= i + 1,
              "reference-array measurement follows multiple actual growth operations");
        if (allocation.capacity != previous.capacity) {
            const auto payload = allocation.capacity * sizeof(btTypedConstraint*);
            if (observedStorageRaw) {
                // Bullet 2.75 without BT_HAS_ALIGNED_ALLOCATOR bypasses the
                // aligned callback. Its ordinary allocator reserves a base
                // pointer and 15 padding bytes for this array's alignment 16.
                const auto base = reinterpret_cast<std::uintptr_t>(observedStoragePointer);
                const auto aligned = (base + sizeof(void*) + 15) & ~std::uintptr_t(15);
                Check(observedStorageAllocations == 1 && observedStoragePointer &&
                      allocation.identity == reinterpret_cast<const void*>(aligned) &&
                      observedStorageBytes == payload + sizeof(void*) + 15 &&
                      static_cast<void* const*>(allocation.identity)[-1] == observedStoragePointer,
                      "measured data identity and capacity match Bullet's exact raw allocation and alignment header");
            } else {
                Check(observedStorageAllocations == 1 && allocation.identity == observedStoragePointer &&
                      payload == observedStorageBytes,
                      "measured identity and capacity match the actual Bullet aligned allocation request");
            }
        } else {
            Check(observedStorageAllocations == 0 && allocation.identity == previous.identity,
                  "in-capacity append preserves allocation identity");
        }
        rb::Collector measured; rb::Body(body, measured);
        const auto expectedBytes = baseline.Result().cpuBytes + allocation.capacity * sizeof(btTypedConstraint*);
        Check(measured.Result().missing == 0 && measured.Result().cpuBytes == expectedBytes,
              "body resource charge includes the complete reference capacity rather than only its size");
        measured.Add(allocation.identity, allocation.capacity, sizeof(btTypedConstraint*)); rb::Body(body, measured);
        Check(measured.Result().cpuBytes == expectedBytes,
              "independent allocation observation and repeated body measurement share the real storage identity");
        previous = allocation;
    }
    observedStorageAllocations = 0; observeBulletStorage = true;
    body->addConstraintRef(refs.front().get()); observeBulletStorage = false;
    Check(observedStorageAllocations == 0 && body->getNumConstraintRefs() == static_cast<int>(refs.size()),
          "duplicate constraint reference changes neither storage nor count");
    for (const auto& ref : refs) body->removeConstraintRef(ref.get());
    storage::Allocation retained;
    Check(body->getNumConstraintRefs() == 0 && storage::ConstraintReferences(readOnlyBody, retained) &&
          retained.capacity == previous.capacity && retained.identity == previous.identity && retained.capacity > 0,
          "removing every reference preserves and measures its nonempty retained allocation");
    rb::Collector removed; rb::Body(body, removed);
    Check(removed.Result().missing == 0 && removed.Result().cpuBytes == baseline.Result().cpuBytes +
          retained.capacity * sizeof(btTypedConstraint*), "zero reference count does not erase the retained storage charge");

    // Exercise the ordinary virtual world route as well as direct body calls.
    f.world.addConstraint(refs.front().get(), true);
    Check(body->getNumConstraintRefs() == 1 && f.bodies[1]->getNumConstraintRefs() == 1,
          "world collision suppression populates both body reference arrays");
    f.world.removeConstraint(refs.front().get());
    rb::Collector worldPayload;
    for (const auto& item : f.bodies) rb::Body(item.get(), worldPayload);
    rb::Joint(f.joint.get(), worldPayload);
    const auto worldBytes = worldPayload.Result();
    rb::Body(body, worldPayload);
    worldPayload.Add(retained.identity, retained.capacity, sizeof(btTypedConstraint*));
    Check(SamePhysicsResources(worldBytes, worldPayload.Result()),
          "world and object accounting deduplicate shared shapes and the body reference allocation");
    auto owner = f.Capture(); auto candidate = owner->Prepare(false);
    rb::Collector target; Check(candidate->MeasureTargetResources(target), "prepare history after references were removed");
    Check(candidate->MeasureTargetResources(worldPayload) && worldPayload.Result().cpuBytes ==
          worldBytes.cpuBytes + target.Result().cpuBytes, "world payload and target history bookkeeping are separate allocations");
    const auto combined = worldPayload.Result(); owner->MeasureResources(worldPayload);
    Check(candidate->MeasureTargetResources(worldPayload) && SamePhysicsResources(combined, worldPayload.Result()),
          "body and retained-candidate accounting remain deduplicated in one batch collector");
    PublishPhysicsBatch(std::move(candidate));
    rb::Collector actual;
    for (const auto& item : f.bodies) rb::Body(item.get(), actual);
    rb::Joint(f.joint.get(), actual); owner->MeasureResources(actual);
    Check(SamePhysicsResources(combined, actual.Result()),
          "parked physics payload and target snapshot equal their pre-publication resource estimate");
    PublishPhysicsBatch(owner->Prepare(true));

    struct DerivedBody final : btRigidBody {
        char extra[32]{};
        explicit DerivedBody(btCollisionShape* shape) : btRigidBody(0, nullptr, shape) {}
    } unknown(&f.shape);
    rb::Collector incomplete; rb::Body(&unknown, incomplete);
    Check(incomplete.Result().missing & rb::PhysicsStorage,
          "reading known base storage never treats an unknown derived rigid body as fully measured");

    // No side registry survives destruction: reconstructing at the same body
    // address must read the new object's empty array, not the old high-water mark.
    alignas(btRigidBody) unsigned char memory[sizeof(btRigidBody)];
    auto* first = ::new(static_cast<void*>(memory)) btRigidBody(0, nullptr, &f.shape);
    first->addConstraintRef(refs.front().get()); first->removeConstraintRef(refs.front().get());
    storage::Allocation oldAllocation;
    Check(storage::ConstraintReferences(*first, oldAllocation) && oldAllocation.capacity > 0,
          "same-address lifetime fixture starts with retained reference storage");
    first->~btRigidBody();
    auto* second = ::new(static_cast<void*>(memory)) btRigidBody(0, nullptr, &f.shape);
    storage::Allocation newAllocation{oldAllocation.identity, oldAllocation.capacity};
    Check(storage::ConstraintReferences(*second, newAllocation) && !newAllocation.identity && !newAllocation.capacity,
          "same-address reconstruction cannot inherit a stale storage identity or capacity");
    second->~btRigidBody();
}
void PhysicsTargetResourceChecks() {
    namespace rb = plugins::resource_budget;
    using history_physics::Retained;
    {
        PhysicsFixture f; auto owner = f.Capture();
        rb::Collector initial; owner->MeasureResources(initial);
        auto candidate = owner->Prepare(false);
        rb::Collector target;
        Check(candidate->MeasureTargetResources(target) && target.Result().cpuBytes > initial.Result().cpuBytes &&
              target.Result().d3d9Bytes == 0 && target.Result().missing == 0,
              "prepared removal measures its future Saved state before the first publication");
        const auto expected = target.Result();
        owner->MeasureResources(target);
        Check(candidate->MeasureTargetResources(target) && SamePhysicsResources(target.Result(), expected),
              "owner and candidate share accounting identities and repeated measurement is deduplicated");
        rb::Collector stillLive; owner->MeasureResources(stillLive);
        Check(SamePhysicsResources(initial.Result(), stillLive.Result()) && candidate->IsCurrent() && owner->Present() &&
              f.world.getNumCollisionObjects() == 5 && f.world.getNumConstraints() == 1 && f.ContactsFirst() && f.RayHitsFirst(),
              "target measurement neither installs Saved storage nor changes simulation contacts or ray filtering");
        candidate.reset();
        rb::Collector canceled; owner->MeasureResources(canceled);
        Check(SamePhysicsResources(initial.Result(), canceled.Result()) && owner->Present(),
              "canceling a measured candidate releases its uncommitted storage");

        candidate = owner->Prepare(false);
        rb::Collector planned; Check(candidate->MeasureTargetResources(planned), "measure replacement removal candidate");
        bulletAllocations = 0; trackBulletAlloc = true; failAfter = 0;
        const bool published = candidate->Publish();
        failAfter = -1; trackBulletAlloc = false;
        Check(published && bulletAllocations == 0, "measurement preserves allocation-free publication");
        rb::Collector consumed; int sentinel = 0; consumed.Add(&sentinel, 1, 19);
        const auto unchanged = consumed.Result();
        failAfter = 0;
        const bool measuredConsumed = candidate->MeasureTargetResources(consumed);
        failAfter = -1;
        Check(!measuredConsumed && SamePhysicsResources(consumed.Result(), unchanged),
              "consumed candidate rejects measurement without touching the collector");
        rb::Collector parked; owner->MeasureResources(parked);
        Check(SamePhysicsResources(planned.Result(), parked.Result()),
              "published retained state exactly matches the prepared target charge without candidate scratch storage");
        candidate.reset();

        auto restore = owner->Prepare(true); rb::Collector restoredTarget;
        Check(restore->MeasureTargetResources(restoredTarget) && SamePhysicsResources(parked.Result(), restoredTarget.Result()),
              "restoration retains its existing Saved state and reports the same allocation charge");
        owner->MeasureResources(restoredTarget);
        Check(SamePhysicsResources(parked.Result(), restoredTarget.Result()),
              "restoration and current-owner measurement do not duplicate the same saved snapshot");
        PublishPhysicsBatch(std::move(restore));

        // Grow unrelated world membership while the restored owner stays alive.
        // Its next removal really retains a larger world-order vector.
        for (unsigned i = 0; i < 7; ++i) {
            btVector3 inertia; f.shape.calculateLocalInertia(1, inertia);
            auto body = std::make_unique<btRigidBody>(1, nullptr, &f.shape, inertia);
            btTransform t; t.setIdentity(); t.setOrigin(btVector3(20 + btScalar(i) * 2, 5, 0));
            body->setWorldTransform(t); body->setInterpolationWorldTransform(t);
            body->forceActivationState(DISABLE_DEACTIVATION);
            f.world.addRigidBody(body.get(), 1, -1); f.bodies.push_back(std::move(body));
        }
        rb::Collector oldSaved; owner->MeasureResources(oldSaved);
        Check(SamePhysicsResources(parked.Result(), oldSaved.Result()), "live world growth does not rewrite the old Saved charge");
        auto removeAgain = owner->Prepare(false); rb::Collector grownTarget;
        Check(removeAgain->MeasureTargetResources(grownTarget) && grownTarget.Result().cpuBytes > oldSaved.Result().cpuBytes,
              "a later removal accounts for its newly enlarged world-order snapshot");
        PublishPhysicsBatch(std::move(removeAgain));
        rb::Collector grownActual; owner->MeasureResources(grownActual);
        Check(SamePhysicsResources(grownTarget.Result(), grownActual.Result()),
              "repeat removal measures the replacement Saved state rather than also charging the discarded old one");
        PublishPhysicsBatch(owner->Prepare(true));
    }
    {
        PhysicsFixture f; auto a = f.Capture();
        Retained b(f.world, {f.bodies[3].get(), f.bodies[4].get()}, {});
        auto candidate = Retained::PrepareBatch({a.get(), &b}, false);
        rb::Collector target; Check(candidate->MeasureTargetResources(target), "measure a multi-owner prepared target");
        PublishPhysicsBatch(std::move(candidate));
        rb::Collector separateA, separateB, together;
        a->MeasureResources(separateA); b.MeasureResources(separateB);
        a->MeasureResources(together); b.MeasureResources(together);
        Check(SamePhysicsResources(target.Result(), together.Result()) &&
              together.Result().cpuBytes < separateA.Result().cpuBytes + separateB.Result().cpuBytes,
              "batch target counts its shared full-world OrderSnapshot once by actual allocation identity");
        auto restore = Retained::PrepareBatch({&b, a.get()}, true); rb::Collector reverse;
        Check(restore->MeasureTargetResources(reverse) && SamePhysicsResources(reverse.Result(), together.Result()),
              "batch restoration order does not change resource deduplication");
        PublishPhysicsBatch(std::move(restore));
    }
    {
        PhysicsFixture f; auto owner = f.Capture();
        auto candidate = owner->Prepare(false);
        rb::Collector target; Check(candidate->MeasureTargetResources(target), "measure a candidate before fault injection");
        bool sawFailure = false, completed = false;
        for (long failure = 0; failure < 64 && !completed; ++failure) {
            rb::Collector output; bool oom = false, measured = false;
            failAfter = failure;
            try { measured = candidate->MeasureTargetResources(output); }
            catch (const std::bad_alloc&) { oom = true; }
            failAfter = -1;
            sawFailure = sawFailure || oom;
            Check(candidate->IsCurrent() && owner->Present() && f.world.getNumCollisionObjects() == 5 &&
                  f.world.getNumConstraints() == 1 && f.ContactsFirst() && f.RayHitsFirst(),
                  "every collector allocation failure leaves the prepared candidate and scene unchanged");
            if (!oom) {
                Check(measured && SamePhysicsResources(output.Result(), target.Result()),
                      "measurement retries after allocation failure produce the complete target charge");
                completed = true;
            }
        }
        Check(sawFailure && completed, "target resource measurement exercises both allocation failure and success");
        rb::Collector full; int overflowSentinel = 0; full.Add(&overflowSentinel, 1, UINT64_MAX);
        const auto fullReport = full.Result(); bool overflow = false;
        try { candidate->MeasureTargetResources(full); }
        catch (const std::bad_alloc&) { overflow = true; }
        Check(overflow && SamePhysicsResources(full.Result(), fullReport) && candidate->IsCurrent(),
              "checked budget overflow preserves the candidate and the collector report");
        rb::Collector stale; int sentinel = 0; stale.Add(&sentinel, 1, 23);
        const auto unchanged = stale.Result();
        const auto velocity = f.bodies[0]->getLinearVelocity(); f.bodies[0]->setLinearVelocity(btVector3(1, 2, 3));
        failAfter = 0;
        const bool measuredStale = candidate->MeasureTargetResources(stale);
        failAfter = -1;
        Check(!measuredStale && SamePhysicsResources(stale.Result(), unchanged) && owner->Present(),
              "stale candidate rejects before any collector allocation or write");
        f.bodies[0]->setLinearVelocity(velocity);
        PublishPhysicsBatch(std::move(candidate));
        PublishPhysicsBatch(owner->Prepare(true));
        f.world.performDiscreteCollisionDetection();

        bool failedPrepare = false;
        failAfter = 0;
        try { candidate = owner->Prepare(false); }
        catch (const std::bad_alloc&) { failedPrepare = true; }
        failAfter = -1;
        Check(failedPrepare && !candidate && owner->Present() && f.world.getNumCollisionObjects() == 5 &&
              f.world.getNumConstraints() == 1 && f.ContactsFirst() && f.RayHitsFirst(),
              "failed preparation creates no measurable target and preserves the live scene");
        Check(RejectPhysicsPrepare(*owner, true) && RejectPhysicsBatch({owner.get(), nullptr}, false),
              "unsupported presence and unknown batch owners cannot create a measurement target");
    }
    {
        PhysicsFixture f; auto owner = f.Capture(); auto candidate = owner->Prepare(false);
        rb::Collector before; Check(candidate->MeasureTargetResources(before), "measure candidate with its wrapper alive");
        owner.reset(); rb::Collector after;
        Check(candidate->MeasureTargetResources(after) && SamePhysicsResources(before.Result(), after.Result()),
              "target accounting remains valid when the candidate alone retains its owner Impl");
        PublishPhysicsBatch(std::move(candidate));
    }
}
void PhysicsRetention() {
    btAlignedAllocSetCustom(HistoryAlloc, HistoryFree);
    btAlignedAllocSetCustomAligned(HistoryAlignedAlloc, HistoryAlignedFree);
    {
        PhysicsFixture f;
        auto retained = f.Capture();
        auto* proxy = f.bodies[0]->getBroadphaseHandle();
        auto* untouched = f.broadphase.Cache()->findPair(f.bodies[3]->getBroadphaseHandle(), f.bodies[4]->getBroadphaseHandle());
        Check(untouched && untouched->m_algorithm && f.ContactsFirst() && f.RayHitsFirst(), "real Bullet initial contacts and ray hit");
        auto* untouchedAlgorithm = untouched->m_algorithm;
        auto before = f.bodies[0]->getWorldTransform();
        f.bodies[0]->setLinearVelocity(btVector3(1, 2, 3));
        f.bodies[0]->setAngularVelocity(btVector3(0.1f, 0.2f, 0.3f));
        auto velocity = f.bodies[0]->getLinearVelocity();
        auto spin = f.bodies[0]->getAngularVelocity();
        auto cancel = retained->Prepare(false);
        Check(cancel->IsCurrent() && f.world.getNumCollisionObjects() == 5 && retained->Present(), "physics prepare leaves scene live");
        cancel.reset();
        Check(f.ContactsFirst() && f.world.getNumConstraints() == 1, "physics cancel keeps contacts and joints");
        auto park = retained->Prepare(false);
        bulletAllocations = 0; trackBulletAlloc = true; failAfter = 0;
        const bool published = park->Publish();
        failAfter = -1; trackBulletAlloc = false;
        Check(published && bulletAllocations == 0, "park publishes without C++ or Bullet allocation");
        Check(!park->IsCurrent() && !park->Publish(), "physics candidate is consumed");
        park.reset();
        Check(!retained->Present() && f.world.getNumCollisionObjects() == 3 && f.world.HistoryBodies().size() == 3 && f.world.getNumConstraints() == 0, "park removes simulation members");
        Check(!f.RayHitsFirst() && !f.ContactsFirst(), "park removes ray hits and contact manifolds");
        untouched = f.broadphase.Cache()->findPair(f.bodies[3]->getBroadphaseHandle(), f.bodies[4]->getBroadphaseHandle());
        Check(untouched && untouched->m_algorithm == untouchedAlgorithm, "unrelated contact algorithm is retained");
        for (int i = 0; i < 15; ++i) f.world.stepSimulation(btScalar(1) / 60, 1);
        Check(f.bodies[0]->getWorldTransform() == before && f.bodies[0]->getLinearVelocity() == velocity && f.bodies[0]->getAngularVelocity() == spin,
              "parked body does not integrate or lose velocity");
        Check(f.bodies[2]->getWorldTransform().getOrigin().y() < btScalar(5.8), "active peer continues falling");
        auto restore = retained->Prepare(true);
        bulletAllocations = 0; trackBulletAlloc = true; failAfter = 0;
        const bool restored = restore->Publish();
        failAfter = -1; trackBulletAlloc = false;
        Check(restored && bulletAllocations == 0, "restore publishes without C++ or Bullet allocation");
        restore.reset();
        Check(retained->Present() && f.world.getNumCollisionObjects() == 5 && f.world.getNumConstraints() == 1 &&
            f.world.getConstraint(0) == f.joint.get() && f.bodies[0]->getBroadphaseHandle() == proxy, "restore reuses exact bodies proxies and joint");
        for (int i = 0; i < 5; ++i) Check(f.world.getCollisionObjectArray()[i] == f.bodies[i].get(), "restore preserves simulation order");
        Check(f.RayHitsFirst() && proxy->m_collisionFilterMask == -1 && f.bodies[0]->getActivationState() == DISABLE_DEACTIVATION,
              "restore restores filters and activation");
        for (int i = 0; i < 30; ++i) {
            auto remove = retained->Prepare(false); Check(remove->Publish(), "repeat park"); remove.reset();
            auto add = retained->Prepare(true); Check(add->Publish(), "repeat restore"); add.reset();
            Check(f.world.getNumCollisionObjects() == 5 && f.world.getNumConstraints() == 1, "repeat leaves no duplicate members");
        }
        auto stale = retained->Prepare(false);
        f.world.stepSimulation(btScalar(1) / 60, 1);
        Check(!stale->IsCurrent() && !stale->Publish() && retained->Present(), "physics step rejects old candidate");
        stale.reset();
        auto changed = retained->Prepare(false); proxy->m_collisionFilterMask = 2;
        Check(!changed->IsCurrent() && !changed->Publish(), "filter mutation rejects candidate");
        proxy->m_collisionFilterMask = -1; changed.reset();
        auto collision = retained->Prepare(false); f.world.performDiscreteCollisionDetection();
        Check(!collision->IsCurrent(), "direct collision pass invalidates candidate"); collision.reset();
        auto velocityChange = retained->Prepare(false); f.bodies[0]->setLinearVelocity(btVector3(19, 20, 21));
        Check(!velocityChange->IsCurrent(), "direct rigid velocity edit invalidates candidate"); velocityChange.reset();
    }
    {
        PhysicsFixture f; auto retained = f.Capture(); bool reached = false; unsigned failures = 0;
        for (long fail = 0; fail < 200; ++fail) {
            failAfter = fail;
            try { auto candidate = retained->Prepare(false); failAfter = -1; reached = true; }
            catch (const std::bad_alloc&) { failAfter = -1; ++failures; }
            Check(retained->Present() && f.world.getNumCollisionObjects() == 5 && f.world.getNumConstraints() == 1 && f.ContactsFirst() && f.RayHitsFirst(),
                  "allocation failure preserves active physics");
            if (reached) break;
        }
        Check(reached && failures > 10, "sweep all C++ physics preparation allocations");
        // Immediate restore must regenerate overlapping pairs even with no
        // AABB edge crossings after restoring the old collision masks.
        auto remove = retained->Prepare(false); Check(remove->Publish(), "park for stationary restoration"); remove.reset();
        auto add = retained->Prepare(true); Check(add->Publish(), "restore stationary overlaps"); add.reset();
        f.world.performDiscreteCollisionDetection();
        Check(f.ContactsFirst(), "stationary restore regenerates contacts");
        auto external = std::make_unique<btPoint2PointConstraint>(*f.bodies[0], *f.bodies[2], btVector3(0,0,0), btVector3(0,0,0));
        f.world.addConstraint(external.get(), false);
        bool rejected = false;
        try { auto candidate = retained->Prepare(false); } catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected && retained->Present(), "unretained external joint is rejected");
        f.world.removeConstraint(external.get());
    }
    {
        PhysicsFixture f;
        auto retained = f.Capture();
        auto cancel = retained->Prepare(false);
        retained.reset();
        Check(cancel->IsCurrent(), "physics cancellation keeps its owner alive");
        cancel.reset();
        Check(f.ContactsFirst() && f.RayHitsFirst() && f.world.getNumCollisionObjects() == 5,
              "owner released before cancellation leaves the live world intact");
        retained = f.Capture();
        auto remove = retained->Prepare(false);
        retained.reset();
        bulletAllocations = 0; trackBulletAlloc = true; failAfter = 0;
        const bool removed = remove->Publish();
        failAfter = -1; trackBulletAlloc = false;
        Check(removed && bulletAllocations == 0, "physics publication keeps its owner alive without allocations");
        remove.reset();
        Check(!f.ContactsFirst() && !f.RayHitsFirst() && f.world.getNumCollisionObjects() == 3,
              "released physics owner still disposes old contacts");
    }
    {
        PhysicsFixture f; auto retained = f.Capture();
        auto remove = retained->Prepare(false); Check(remove->Publish(), "park before checking retained-state guards"); remove.reset();
        const auto flags = f.bodies[0]->getCollisionFlags();
        f.bodies[0]->setCollisionFlags(flags | btCollisionObject::CF_STATIC_OBJECT);
        Check(RejectPhysicsPrepare(*retained, true), "parked static-mode change cannot restore an obsolete moving-body list");
        f.bodies[0]->setCollisionFlags(flags);
        btSphereShape changedShape(btScalar(2));
        f.bodies[0]->setCollisionShape(&changedShape);
        Check(RejectPhysicsPrepare(*retained, true), "parked shape replacement is rejected");
        f.bodies[0]->setCollisionShape(&f.shape);
        f.bodies[0]->setLinearVelocity(btVector3(3, 4, 5));
        Check(RejectPhysicsPrepare(*retained, true), "parked body mutation is rejected");
        f.bodies[0]->setLinearVelocity(btVector3(0, 0, 0));
        auto* proxy = f.bodies[0]->getBroadphaseHandle();
        const auto lo = proxy->m_aabbMin;
        proxy->m_aabbMin += btVector3(1, 0, 0);
        Check(RejectPhysicsPrepare(*retained, true), "parked broadphase bounds mutation is rejected");
        proxy->m_aabbMin = lo;
        auto restore = retained->Prepare(true);
        auto* ray = history_physics::Broadphase::RayProxy(proxy);
        Check(ray != nullptr, "axis-sweep ray accelerator proxy exists");
        ray->m_collisionFilterMask = -1;
        Check(!restore->IsCurrent() && !restore->Publish(), "ray filter mutation rejects restore candidate");
        ray->m_collisionFilterMask = 0;
        Check(restore->Publish(), "unchanged parked state can be restored after rejected mutations");
    }
    {
        PhysicsFixture f; auto retained = f.Capture();
        HistoryAction action;
        auto candidate = retained->Prepare(false);
        f.world.addAction(&action);
        Check(!candidate->IsCurrent() && !candidate->Publish() && RejectPhysicsPrepare(*retained, false) && action.calls == 0,
              "physics action rejects both preparation and stale publication without callback");
        f.world.removeAction(&action); candidate.reset();
        HistoryOverlapFilter filter;
        candidate = retained->Prepare(false);
        f.broadphase.Cache()->setOverlapFilterCallback(&filter);
        Check(!candidate->IsCurrent() && !candidate->Publish() && RejectPhysicsPrepare(*retained, false) && filter.calls == 0,
              "custom overlap filter rejects history without callback");
        f.broadphase.Cache()->setOverlapFilterCallback(nullptr); candidate.reset();
        HistoryPairCallback callback;
        candidate = retained->Prepare(false);
        static_cast<btOverlappingPairCache*>(f.broadphase.Cache())->setInternalGhostPairCallback(&callback);
        Check(!candidate->IsCurrent() && !candidate->Publish() && RejectPhysicsPrepare(*retained, false) && callback.calls == 0,
              "ghost pair callback rejects history without callback");
        static_cast<btOverlappingPairCache*>(f.broadphase.Cache())->setInternalGhostPairCallback(nullptr); candidate.reset();
        candidate = retained->Prepare(false);
        f.broadphase.setOverlappingPairUserCallback(&callback);
        Check(!candidate->IsCurrent() && !candidate->Publish() && RejectPhysicsPrepare(*retained, false) && callback.calls == 0,
              "broadphase user callback rejects history without callback");
        f.broadphase.setOverlappingPairUserCallback(nullptr); candidate.reset();
        candidate = retained->Prepare(false);
        f.bodies[0]->addConstraintRef(f.joint.get());
        Check(!candidate->IsCurrent() && !candidate->Publish() && RejectPhysicsPrepare(*retained, false),
              "constraint filter reference rejects history");
        f.bodies[0]->removeConstraintRef(f.joint.get()); candidate.reset();
        bool rejected = false;
        try {
            history_physics::Retained partial(f.world, {f.bodies[0].get()}, {f.joint.get()});
        } catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected && retained->Present() && f.world.getNumConstraints() == 1,
              "joint cannot retain an independently owned body through a raw pointer");
    }
    PhysicsGuardCoordinates();
    PhysicsBatchRetention();
    PhysicsTargetResourceChecks();
    PhysicsConstraintStorageChecks();
    btAlignedAllocSetCustomAligned(nullptr, nullptr);
    btAlignedAllocSetCustom(nullptr, nullptr);
}
}
