// Include after history_physics_checks.inc. No renderer or user scene is used.
namespace {
struct PhysicsImportFixture {
    PhysicsFixture& host;
    PhysicsScene live{};
    std::unique_ptr<history_physics::ImportScene> scene;
    btSphereShape shape{btScalar(0.5)};
    std::vector<std::unique_ptr<btRigidBody>> bodies;
    btTypedConstraint* joint = nullptr;
    bool published = false;
    explicit PhysicsImportFixture(PhysicsFixture& f, int base = 17, bool withJoint = true) : host(f) {
        live.world = &f.world; live.broadphase = &f.broadphase; live.dispatcher = &f.dispatcher;
        live.collisionConfig = &f.config; live.solver = &f.solver; live.constraintId = base;
        scene = std::make_unique<history_physics::ImportScene>(live);
        for (const auto& p : {btVector3(0, 5.3f, 0), btVector3(0, 7.3f, 0)}) {
            btVector3 inertia; shape.calculateLocalInertia(1, inertia);
            auto body = std::make_unique<btRigidBody>(1, nullptr, &shape, inertia);
            auto transform = btTransform::getIdentity(); transform.setOrigin(p);
            body->setWorldTransform(transform); body->setInterpolationWorldTransform(transform);
            body->forceActivationState(DISABLE_DEACTIVATION);
            scene->Scene().world->addRigidBody(body.get(), 1, -1); bodies.push_back(std::move(body));
        }
        if (withJoint) {
            const auto id = CreatePhysJoint(&scene->Scene(), bodies[0].get(), bodies[1].get(),
                0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            joint = scene->Scene().world->getConstraint(0);
            Check(id == base && joint->getUid() == base, "private import joint receives the reserved live ID without publishing it");
        }
    }
    ~PhysicsImportFixture() {
        auto* world = published ? &host.world : scene->Scene().world;
        if (joint) { world->removeConstraint(joint); delete joint; }
        for (auto& body : bodies) world->removeRigidBody(body.get());
    }
};
bool ImportRayHit(DynamicsWorld& world, btRigidBody* target) {
    struct Ray final : btCollisionWorld::RayResultCallback {
        btRigidBody* target; bool hit = false;
        explicit Ray(btRigidBody* t) : target(t) {}
        btScalar addSingleResult(btCollisionWorld::LocalRayResult& result, bool) override {
            hit = hit || result.m_collisionObject == target; return 1;
        }
    } ray(target);
    world.rayTest(btVector3(-3, 5.3f, 0), btVector3(3, 5.3f, 0), ray); return ray.hit;
}
void PublishImportWithoutAllocation(history_physics::ImportScene::Publication& candidate) {
    bulletAllocations = 0; trackBulletAlloc = true; failAfter = 0;
    const bool current = candidate.IsCurrent(); const bool published = candidate.Publish();
    failAfter = -1; trackBulletAlloc = false;
    Check(current && published && bulletAllocations == 0, "import validation and publication allocate neither C++ nor Bullet storage");
}
bool RejectImportPrepare(history_physics::ImportScene& scene) {
    try { auto candidate = scene.Prepare(); return false; }
    catch (const std::invalid_argument&) { return true; }
}
void ImportTick(btDynamicsWorld*, btScalar) {}
void PhysicsImportChecks() {
    using namespace history_physics;
    namespace budget = plugins::resource_budget;
    btAlignedAllocSetCustom(HistoryAlloc, HistoryFree);
    btAlignedAllocSetCustomAligned(HistoryAlignedAlloc, HistoryAlignedFree);
    {
        PhysicsFixture f; auto oldOwner = f.Capture(); PhysicsImportFixture added(f);
        auto* oldCache = f.broadphase.Cache(); auto* oldObjects = &f.world.getCollisionObjectArray()[0];
        const int oldCapacity = f.world.getCollisionObjectArray().capacity();
        std::vector<btBroadphaseProxy*> oldProxies;
        for (auto& body : f.bodies) oldProxies.push_back(body->getBroadphaseHandle());
        std::vector<btBroadphasePair> oldPairs;
        const auto& pairs = oldCache->getOverlappingPairArray();
        for (int i = 0; i < pairs.size(); ++i) oldPairs.push_back(pairs[i]);
        Check(!ImportRayHit(f.world, added.bodies[0].get()) && added.live.constraintId == 17 &&
            added.scene->Scene().constraintId == 18, "private body creation changes neither live ray queries nor constraint counter");
        auto candidate = added.scene->Prepare(); auto retained = candidate->RetainedOwner();
        Check(candidate->IsCurrent() && !retained->Present() && RejectPhysicsPrepare(*retained, true) &&
            RejectPhysicsPrepare(*retained, false), "prepared import owner remains pending and rejects ordinary presence transitions");
        Check(f.world.getNumCollisionObjects() == 5 && f.world.getNumConstraints() == 1 &&
            f.broadphase.Cache() == oldCache && &f.world.getCollisionObjectArray()[0] == oldObjects &&
            f.world.getCollisionObjectArray().capacity() == oldCapacity,
            "private preparation preserves live members, pair cache, array allocation and capacity");
        budget::Collector target; Check(candidate->MeasureTargetResources(target), "import target accounting is available before publication");
        const auto bytes = target.Result().totalBytes; retained->MeasureResources(target);
        Check(target.Result().Complete() && target.Result().totalBytes == bytes && bytes > 0,
            "pending owner and publication share one exact target bookkeeping identity");
        PublishImportWithoutAllocation(*candidate); added.published = true;
        Check(retained->Present() && f.world.getNumCollisionObjects() == 7 && f.world.getNumConstraints() == 2 &&
            added.scene->Scene().world->getNumCollisionObjects() == 0 && added.live.constraintId == 18,
            "one publication transfers both private bodies and joint and advances the live counter once");
        for (std::size_t i = 0; i < oldProxies.size(); ++i)
            Check(f.bodies[i]->getBroadphaseHandle() == oldProxies[i], "publication preserves every existing sweep proxy address");
        for (const auto& old : oldPairs) {
            auto* now = f.broadphase.Cache()->findPair(old.m_pProxy0, old.m_pProxy1);
            Check(now && now->m_algorithm == old.m_algorithm && now->m_internalInfo1 == old.m_internalInfo1,
                "publication preserves existing contact algorithms and user payload");
        }
        Check(ImportRayHit(f.world, added.bodies[0].get()) && !candidate->Publish(),
            "published import is visible to rays and cannot publish twice");
        candidate.reset();
        Check(ImportRayHit(f.world, added.bodies[0].get()) && f.RayHitsFirst(),
            "candidate cleanup removes retired ray trees and private proxies without deleting live proxies");
        f.world.performDiscreteCollisionDetection();
        PublishPhysicsBatch(retained->Prepare(false));
        Check(!ImportRayHit(f.world, added.bodies[0].get()) && f.world.getNumCollisionObjects() == 5,
            "imported retained owner can be removed through ordinary history");
        PublishPhysicsBatch(retained->Prepare(true));
        PublishPhysicsBatch(oldOwner->Prepare(false)); PublishPhysicsBatch(oldOwner->Prepare(true));
        Check(f.world.getNumCollisionObjects() == 7 && f.world.getCollisionObjectArray()[0] == f.bodies[0].get() &&
            ImportRayHit(f.world, added.bodies[0].get()), "pre-import owner history remains usable after import remove and restore");
    }
    {
        PhysicsFixture f; auto oldOwner = f.Capture();
        auto* parkedProxy = f.bodies[0]->getBroadphaseHandle(); PublishPhysicsBatch(oldOwner->Prepare(false));
        PhysicsImportFixture added(f); auto candidate = added.scene->Prepare();
        PublishImportWithoutAllocation(*candidate); added.published = true; candidate.reset();
        Check(f.bodies[0]->getBroadphaseHandle() == parkedProxy && !f.RayHitsFirst(),
            "import preserves parked handles and their disabled ray mask");
        PublishPhysicsBatch(oldOwner->Prepare(true));
        Check(f.RayHitsFirst() && f.world.getNumCollisionObjects() == 7,
            "an owner removed before import restores with the same stable handles");
    }
    {
        PhysicsFixture f; PhysicsImportFixture added(f);
        auto* cache = f.broadphase.Cache(); auto* proxy = f.bodies[0]->getBroadphaseHandle();
        auto candidate = added.scene->Prepare();
        ++added.live.constraintId;
        budget::Collector unchanged; const auto before = unchanged.Result().totalBytes;
        Check(!candidate->IsCurrent() && !candidate->Publish() && !candidate->MeasureTargetResources(unchanged) &&
            unchanged.Result().totalBytes == before, "changed constraint reservation rejects publication and leaves target collector untouched");
        --added.live.constraintId;
        auto transform = added.bodies[0]->getWorldTransform(); auto changed = transform;
        changed.setOrigin(changed.getOrigin() + btVector3(1, 0, 0)); added.bodies[0]->setWorldTransform(changed);
        Check(!candidate->IsCurrent() && !candidate->Publish(), "private body mutation invalidates its prepared import");
        added.bodies[0]->setWorldTransform(transform);
        f.world.setInternalTickCallback(ImportTick);
        Check(!candidate->IsCurrent() && !candidate->Publish() && RejectImportPrepare(*added.scene),
            "external world callbacks reject both new preparation and a prepared import");
        f.world.setInternalTickCallback(nullptr);
        HistoryOverlapFilter filter; f.broadphase.Cache()->setOverlapFilterCallback(&filter);
        Check(!candidate->IsCurrent() && !candidate->Publish() && RejectImportPrepare(*added.scene) && filter.calls == 0,
            "external pair filtering is rejected without invoking it");
        f.broadphase.Cache()->setOverlapFilterCallback(nullptr);
        candidate.reset();
        Check(f.broadphase.Cache() == cache && f.bodies[0]->getBroadphaseHandle() == proxy &&
            f.world.getNumCollisionObjects() == 5 && f.world.getNumConstraints() == 1 && added.live.constraintId == 17,
            "cancellation and stale failure preserve the complete live membership and counter");
    }
    {
        PhysicsFixture f;
        const int freed = f.bodies[2]->getBroadphaseHandle()->m_uniqueId;
        f.world.removeRigidBody(f.bodies[2].get());
        PhysicsImportFixture added(f); auto candidate = added.scene->Prepare();
        PublishImportWithoutAllocation(*candidate); added.published = true; candidate.reset();
        Check(added.bodies[0]->getBroadphaseHandle()->m_uniqueId == freed &&
            ImportRayHit(f.world, added.bodies[0].get()), "import safely reuses a non-tail free handle with its final UID");
        auto* pair = f.broadphase.Cache()->findPair(f.bodies[0]->getBroadphaseHandle(), added.bodies[0]->getBroadphaseHandle());
        Check(pair != nullptr, "prepared pair hash remains valid when final live handle IDs differ from compact shadow IDs");
        f.world.performDiscreteCollisionDetection();
    }
    {
        PhysicsFixture f; PhysicsImportFixture added(f);
        auto* cache = f.broadphase.Cache(); auto* objects = &f.world.getCollisionObjectArray()[0];
        const auto capacity = f.world.getCollisionObjectArray().capacity();
        bool reached = false; unsigned failures = 0;
        for (long fail = 0; fail < 512; ++fail) {
            failAfter = fail;
            try { auto candidate = added.scene->Prepare(); failAfter = -1; reached = true; }
            catch (const std::bad_alloc&) { failAfter = -1; ++failures; }
            Check(f.broadphase.Cache() == cache && &f.world.getCollisionObjectArray()[0] == objects &&
                f.world.getCollisionObjectArray().capacity() == capacity && f.world.getNumCollisionObjects() == 5 &&
                added.scene->Scene().world->getNumCollisionObjects() == 2 && added.live.constraintId == 17,
                "every injected import preparation allocation failure preserves both worlds and live storage");
            if (reached) break;
        }
        Check(reached && failures > 20, "import allocation fault sweep reaches complete preparation");
        auto candidate = added.scene->Prepare(); budget::Collector target;
        failAfter = 0; bool failed = false;
        try { candidate->MeasureTargetResources(target); } catch (const std::bad_alloc&) { failed = true; }
        failAfter = -1;
        Check(failed && candidate->IsCurrent(), "failed target accounting leaves a prepared import usable");
        PublishImportWithoutAllocation(*candidate); added.published = true;
    }
    {
        PhysicsFixture f; PhysicsImportFixture added(f, (std::numeric_limits<int>::max)(), false);
        bool rejected = false;
        try {
            CreatePhysJoint(&added.scene->Scene(), added.bodies[0].get(), added.bodies[1].get(),
                0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        } catch (const std::overflow_error&) { rejected = true; }
        Check(rejected && added.scene->Scene().world->getNumConstraints() == 0 &&
            added.live.constraintId == (std::numeric_limits<int>::max)(),
            "constraint ID exhaustion rejects before adding a joint or changing either scene counter");
        for (const char group : {char(16), char(-1)}) {
            void* output[2]{nullptr, reinterpret_cast<void*>(1)};
            CreateRigidBody(&added.scene->Scene(), output, 0, 1, 1, 1, nullptr,
                1, 1, 0, 0, 0, 0, group, 0xffff);
            Check(output[0] == reinterpret_cast<void*>(static_cast<std::intptr_t>(-1)) && !output[1] &&
                added.scene->Scene().world->getNumCollisionObjects() == 2 && f.world.getNumCollisionObjects() == 5,
                "invalid collision group is rejected before shifting or allocating a body");
        }
    }
    btAlignedAllocSetCustomAligned(nullptr, nullptr); btAlignedAllocSetCustom(nullptr, nullptr);
}
}
