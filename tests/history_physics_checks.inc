#include "BulletDynamics/Dynamics/btActionInterface.h"
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
    PhysicsFixture() {
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
        world.performDiscreteCollisionDetection();
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
    btAlignedAllocSetCustomAligned(nullptr, nullptr);
}
}
