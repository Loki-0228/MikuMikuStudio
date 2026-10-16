#include "BulletDynamics/Dynamics/btActionInterface.h"
#include <limits>
#include <stdexcept>

namespace {
bool trackBulletAlloc = false;
unsigned bulletAllocations = 0;
void* HistoryAlignedAlloc(size_t size, int alignment) {
    if (trackBulletAlloc) ++bulletAllocations;
    return _aligned_malloc(size, alignment);
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
void PhysicsRetention() {
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
    btAlignedAllocSetCustomAligned(nullptr, nullptr);
}
}
