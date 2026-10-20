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
void PhysicsImportReplaceChecks();
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
    PhysicsImportReplaceChecks();
    btAlignedAllocSetCustomAligned(nullptr, nullptr); btAlignedAllocSetCustom(nullptr, nullptr);
}
namespace physics_import_replace_checks {
using history_physics::Retained;
namespace rb = plugins::resource_budget;
// Match the production UID strategy: the parity build has a separate solver
// UID; stock Bullet 2.75's getUid() aliases the user constraint ID instead.
template <typename Constraint>
auto SetUid(Constraint* joint,int uid,int) -> decltype(joint->setUid(uid),void()) {
    joint->setUid(uid);
}
template <typename Constraint>
void SetUid(Constraint* joint,int uid,long) {
    joint->setUserConstraintId(uid);
}
struct Fixture {
    PhysicsFixture physics;
    std::unique_ptr<Retained> old = physics.Capture();
    std::unique_ptr<Retained> second;
    PhysicsImportFixture added{physics};
    std::shared_ptr<Retained> incoming;
    std::unique_ptr<btGeneric6DofConstraint> peerJoint;
    explicit Fixture(bool multiple=false) {
        SetUid(physics.joint.get(),3,0);
        peerJoint=std::make_unique<btGeneric6DofConstraint>(*physics.bodies[3],*physics.bodies[4],
            btTransform::getIdentity(),btTransform::getIdentity(),true);
        SetUid(peerJoint.get(),4,0);peerJoint->setLinearLowerLimit(btVector3(.1f,.2f,.3f));physics.world.addConstraint(peerJoint.get(),false);
        if(multiple)second=std::make_unique<Retained>(physics.world,std::vector<btRigidBody*>{physics.bodies[2].get()},std::vector<btTypedConstraint*>{});
    }
    ~Fixture(){physics.world.removeConstraint(peerJoint.get());}
    std::vector<Retained*> Outgoing(bool reverse=false) {
        std::vector<Retained*> result{old.get()};if(second)result.push_back(second.get());
        if(reverse)std::reverse(result.begin(),result.end());return result;
    }
    void Order(bool replaced) {
        std::vector<btRigidBody*> expected;
        if(!replaced){for(auto& body:physics.bodies)expected.push_back(body.get());}
        else {
            for(unsigned i=second?3:2;i<physics.bodies.size();++i)expected.push_back(physics.bodies[i].get());
            for(auto& body:added.bodies)expected.push_back(body.get());
        }
        Check(physics.world.getNumCollisionObjects()==int(expected.size())&&physics.world.HistoryBodies().size()==int(expected.size()),
            "replacement changes active objects and moving bodies in the same publication");
        for(unsigned i=0;i<expected.size();++i)Check(physics.world.getCollisionObjectArray()[i]==expected[i]&&physics.world.HistoryBodies()[i]==expected[i],
            "replacement preserves every peer position and uses deterministic private order");
        Check(physics.world.getNumConstraints()==2&&physics.world.getConstraint(replaced?1:0)==(replaced?added.joint:physics.joint.get())&&
            physics.world.getConstraint(replaced?0:1)==peerJoint.get()&&peerJoint->getUid()==4&&
            peerJoint->getTranslationalLimitMotor()->m_lowerLimit==btVector3(.1f,.2f,.3f)&&
            old->Present()!=replaced&&(!second||second->Present()!=replaced),"replacement selects the matching constraint and all outgoing owner directions");
        if(incoming)Check(incoming->Present()==replaced,"new owner presence follows its complete resource bundle");
    }
    btCollisionAlgorithm* PeerAlgorithm() {
        auto* pair=physics.broadphase.Cache()->findPair(physics.bodies[3]->getBroadphaseHandle(),physics.bodies[4]->getBroadphaseHandle());
        Check(pair&&pair->m_algorithm,"replacement retains a real unrelated peer contact algorithm");return pair->m_algorithm;
    }
    Retained::PresenceChange ChangeOld(Retained* value,bool forward) {return {value,!forward};}
    std::unique_ptr<Retained::Transition> Exchange(bool forward) {
        std::vector<Retained::PresenceChange> changes{ChangeOld(old.get(),forward),{incoming.get(),forward}};
        if(second)changes.push_back(ChangeOld(second.get(),forward));return Retained::PrepareExchange(changes);
    }
};
struct Storage {
    struct Array {const void* pointer;int size,capacity;};
    Array objects,moving,constraints;
    static Storage Read(DynamicsWorld& world) {
        const auto get=[](auto& array)->Array{return {array.size()?&array[0]:nullptr,array.size(),array.capacity()};};
        return {get(world.getCollisionObjectArray()),get(world.HistoryBodies()),get(world.HistoryConstraints())};
    }
    bool Same(DynamicsWorld& world)const {
        const auto now=Read(world);
        const auto equal=[](const auto& a,const auto& b){return a.pointer==b.pointer&&a.size==b.size&&a.capacity==b.capacity;};
        return equal(objects,now.objects)&&equal(moving,now.moving)&&equal(constraints,now.constraints);
    }
};
bool Reject(Fixture& f,const std::vector<Retained*>& removed) {
    try{auto publication=f.added.scene->PrepareReplacing(removed);return false;}
    catch(const std::invalid_argument&){return true;}
}
void AtomicDirections() {
    for(bool multiple:{false,true})for(bool reverse:{false,true}) {
        Fixture f(multiple);const auto live=Storage::Read(f.physics.world),privateStorage=Storage::Read(static_cast<DynamicsWorld&>(*f.added.scene->Scene().world));
        const auto* source=f.physics.broadphase.Cache();const auto* algorithm=f.PeerAlgorithm();
        const auto transform=f.physics.bodies[0]->getWorldTransform(),privateTransform=f.added.bodies[0]->getWorldTransform();
        const auto velocity=btVector3(.125f,-.25f,.5f);f.physics.bodies[3]->setLinearVelocity(velocity);
        auto* oldProxy=f.physics.bodies[0]->getBroadphaseHandle();
        auto staleOld=f.old->Prepare(false);
        {
            auto canceled=f.added.scene->PrepareReplacing(f.Outgoing(reverse));auto pending=canceled->RetainedOwner();
            Check(canceled->IsCurrent()&&!pending->Present()&&RejectPhysicsPrepare(*pending,true)&&RejectPhysicsPrepare(*pending,false)&&
                live.Same(f.physics.world)&&privateStorage.Same(static_cast<DynamicsWorld&>(*f.added.scene->Scene().world))&&
                f.physics.broadphase.Cache()==source&&f.PeerAlgorithm()==algorithm&&f.physics.RayHitsFirst()&&
                !ImportRayHit(f.physics.world,f.added.bodies[0].get()),"replacement preparation and pending ownership change neither world, storage, contacts nor rays");f.Order(false);
        }
        auto candidate=f.added.scene->PrepareReplacing(f.Outgoing(reverse));f.incoming=candidate->RetainedOwner();rb::Collector target;
        Check(candidate->MeasureTargetResources(target)&&target.Result().Complete(),"replacement target includes old Saved and the new retained owner before publication");
        const auto expected=target.Result();Check(candidate->MeasureTargetResources(target)&&SamePhysicsResources(expected,target.Result()),
            "replacement owner and shared order resources deduplicate on repeated measurement");
        PublishImportWithoutAllocation(*candidate);f.added.published=true;f.Order(true);
        Check(!staleOld->IsCurrent()&&f.physics.bodies[0]->getBroadphaseHandle()==oldProxy&&!f.physics.RayHitsFirst()&&
            ImportRayHit(f.physics.world,f.added.bodies[0].get())&&f.PeerAlgorithm()==algorithm&&f.added.live.constraintId==18&&
            f.added.scene->Scene().world->getNumCollisionObjects()==0,"one replacement preserves old handles and peer algorithms while switching masks, arrays and constraint counter");
        rb::Collector actual;f.incoming->MeasureResources(actual);f.old->MeasureResources(actual);if(f.second)f.second->MeasureResources(actual);
        Check(SamePhysicsResources(actual.Result(),expected),"published replacement bookkeeping exactly matches the pre-admission target");
        candidate.reset();staleOld.reset();
        Check(!f.physics.ContactsFirst()&&f.PeerAlgorithm()==algorithm,"retired outgoing contacts are released after publication without disposing peer algorithms");
        f.physics.world.performDiscreteCollisionDetection();
        if(!multiple)Check(physics_exchange_checks::Contact(f.physics,f.added.bodies[0].get()),"incoming resources generate contacts against unchanged peers");
        for(unsigned iteration=0;iteration<12;++iteration) {
            PublishPhysicsBatch(f.Exchange(false));f.Order(false);
            Check(f.physics.RayHitsFirst()&&!ImportRayHit(f.physics.world,f.added.bodies[0].get()),"replacement undo restores old masks and parks imported resources");
            PublishPhysicsBatch(f.Exchange(true));f.Order(true);
            Check(f.PeerAlgorithm()==algorithm&&f.physics.bodies[3]->getLinearVelocity()==velocity&&f.physics.bodies[0]->getWorldTransform()==transform&&
                f.added.bodies[0]->getWorldTransform()==privateTransform&&f.added.live.constraintId==18,
                "replacement replay preserves peer physics state, both retained transforms and monotonic constraint IDs without reimport");
        }
    }
}
void InvalidAndStale() {
    {
        Fixture f;PhysicsFixture foreign;auto other=foreign.Capture();
        Retained overlap(f.physics.world,{f.physics.bodies[0].get()},{});
        Check(Reject(f,{nullptr})&&Reject(f,{f.old.get(),f.old.get()})&&Reject(f,{other.get()})&&Reject(f,{f.old.get(),&overlap}),
            "replacement rejects null, repeated, foreign and overlapping outgoing ownership");
        auto pending=f.added.scene->Prepare()->RetainedOwner();
        Check(Reject(f,{pending.get()}),"an importPending owner cannot be reused as outgoing physics");
        PublishPhysicsBatch(f.old->Prepare(false));Check(Reject(f,{f.old.get()}),"outgoing replacement owner must be present");
        PublishPhysicsBatch(f.old->Prepare(true));
        auto cross=std::make_unique<btPoint2PointConstraint>(*f.physics.bodies[0],*f.physics.bodies[3],btVector3(0,0,0),btVector3(0,0,0));
        f.physics.world.addConstraint(cross.get(),false);Check(Reject(f,f.Outgoing()),"external active joints cannot retain pointers to a parked outgoing body");f.physics.world.removeConstraint(cross.get());
    }
    for(unsigned mutation=0;mutation<9;++mutation) {
        Fixture f;auto candidate=f.added.scene->PrepareReplacing(f.Outgoing());HistoryOverlapFilter filter;
        const auto uid=f.physics.joint->getUid();
        if(mutation==0)f.physics.bodies[0]->setLinearVelocity(btVector3(1,2,3));
        if(mutation==1)f.added.bodies[0]->setLinearVelocity(btVector3(1,2,3));
        if(mutation==2){SetUid(f.physics.joint.get(),uid+1,0);Check(f.physics.joint->getUid()==uid+1,"constraint UID mutation reaches Bullet's actual history identity");}
        if(mutation==3)++f.added.live.constraintId;
        if(mutation==4)++f.added.scene->Scene().constraintId;
        if(mutation==5)f.added.scene->Scene().world->performDiscreteCollisionDetection();
        if(mutation==6)f.physics.broadphase.Cache()->setOverlapFilterCallback(&filter);
        if(mutation==7){PublishPhysicsBatch(f.old->Prepare(false));PublishPhysicsBatch(f.old->Prepare(true));}
        if(mutation==8)f.physics.world.stepSimulation(btScalar(1.f/60.f));
        rb::Collector untouched;int sentinel=0;untouched.Add(&sentinel,1,17);const auto expected=untouched.Result();
        failAfter=0;const auto current=candidate->IsCurrent(),measured=candidate->MeasureTargetResources(untouched),published=candidate->Publish();failAfter=-1;
        Check(!current&&!measured&&!published&&SamePhysicsResources(untouched.Result(),expected)&&f.old->Present()&&
            f.added.scene->Scene().world->getNumCollisionObjects()==2,"stale body, UID, reservation, world-step, callbacks or owner ABA rejects the whole replacement and leaves accounting untouched");
        if(mutation==6)f.physics.broadphase.Cache()->setOverlapFilterCallback(nullptr);
        if(mutation==2)SetUid(f.physics.joint.get(),uid,0);
    }
    {
        // Removing all model physics needs an empty new version, not a dummy
        // native body. It still shares the one publication and replay path.
        PhysicsFixture f;SetUid(f.joint.get(),3,0);auto old=f.Capture();PhysicsScene live{};live.world=&f.world;live.dispatcher=&f.dispatcher;live.constraintId=17;
        history_physics::ImportScene scene(live);auto candidate=scene.PrepareReplacing({old.get()});auto empty=candidate->RetainedOwner();
        PublishImportWithoutAllocation(*candidate);candidate.reset();
        Check(!old->Present()&&empty->Present()&&f.world.getNumCollisionObjects()==3&&f.world.getNumConstraints()==0,"an empty incoming physics version parks old bodies and joints atomically");
        PublishPhysicsBatch(Retained::PrepareExchange({{old.get(),true},{empty.get(),false}}));
        CheckPhysicsOrder(f,{0,1,2,3,4});
    }
    {
        Fixture f;const auto freed=f.physics.bodies[2]->getBroadphaseHandle()->m_uniqueId;
        f.physics.world.removeRigidBody(f.physics.bodies[2].get());
        // Bullet removes a non-tail member by swapping in its last member.
        // The version source is this real post-removal order, not a sorted
        // projection of the fixture's original body indices.
        CheckPhysicsOrder(f.physics,{0,1,4,3});
        auto* preserved=f.physics.bodies[0]->getBroadphaseHandle();
        auto candidate=f.added.scene->PrepareReplacing(f.Outgoing());f.incoming=candidate->RetainedOwner();
        PublishImportWithoutAllocation(*candidate);f.added.published=true;candidate.reset();
        Check(f.added.bodies[0]->getBroadphaseHandle()->m_uniqueId==freed&&f.physics.bodies[0]->getBroadphaseHandle()==preserved&&
            ImportRayHit(f.physics.world,f.added.bodies[0].get()),"replacement reuses a non-tail free handle while preserving parked outgoing handles and valid pair hashes");
        const auto checkImportedOrder=[&] {
            const std::array<btRigidBody*,4> expected{f.physics.bodies[4].get(),f.physics.bodies[3].get(),f.added.bodies[0].get(),f.added.bodies[1].get()};
            Check(f.physics.world.getNumCollisionObjects()==int(expected.size())&&f.physics.world.HistoryBodies().size()==int(expected.size()),
                "non-tail handle replacement keeps complete collision and moving membership");
            for(size_t i=0;i<expected.size();++i)Check(f.physics.world.getCollisionObjectArray()[int(i)]==expected[i]&&f.physics.world.HistoryBodies()[int(i)]==expected[i],
                "non-tail handle replacement preserves the real peer order before appending the imported bundle");
            Check(f.physics.world.getNumConstraints()==2&&f.physics.world.getConstraint(0)==f.peerJoint.get()&&f.physics.world.getConstraint(1)==f.added.joint,
                "non-tail handle replacement preserves peer and imported constraint order");
        };
        checkImportedOrder();
        for(unsigned iteration=0;iteration<12;++iteration) {
            PublishPhysicsBatch(f.Exchange(false));CheckPhysicsOrder(f.physics,{0,1,4,3});
            Check(f.physics.world.getNumConstraints()==2&&f.physics.world.getConstraint(0)==f.physics.joint.get()&&f.physics.world.getConstraint(1)==f.peerJoint.get(),
                "non-tail handle replacement undo restores the exact source joint order");
            PublishPhysicsBatch(f.Exchange(true));checkImportedOrder();
        }
        PublishPhysicsBatch(f.Exchange(false));CheckPhysicsOrder(f.physics,{0,1,4,3});
    }
    {
        Fixture f;std::vector<std::unique_ptr<btRigidBody>> extra;
        for(unsigned i=0;i<250;++i) {
            auto body=std::make_unique<btRigidBody>(0,nullptr,&f.physics.shape);
            f.physics.world.addRigidBody(body.get(),1,0);extra.push_back(std::move(body));
        }
        const auto live=Storage::Read(f.physics.world);auto* cache=f.physics.broadphase.Cache();
        Check(f.physics.broadphase.getNumHandles()==255&&Reject(f,f.Outgoing())&&live.Same(f.physics.world)&&
            f.physics.broadphase.Cache()==cache&&f.old->Present(),"outgoing handles remain reserved for undo so insufficient occupied-plus-private capacity rejects atomically");
        for(auto& body:extra)f.physics.world.removeRigidBody(body.get());
    }
    {
        Fixture f;auto candidate=f.added.scene->PrepareReplacing(f.Outgoing());f.incoming=candidate->RetainedOwner();
        f.old.reset();rb::Collector target;
        Check(candidate->IsCurrent()&&candidate->MeasureTargetResources(target),"replacement candidate keeps outgoing owner internals after its wrapper is released");
        PublishImportWithoutAllocation(*candidate);f.added.published=true;candidate.reset();
        Check(f.incoming->Present()&&!f.physics.RayHitsFirst(),"wrapper release does not break the pending one-world publication or outgoing masks");
    }
    {
        Fixture f;auto& moving=static_cast<DynamicsWorld&>(*f.added.scene->Scene().world).HistoryBodies();
        auto* second=moving[1];moving.pop_back();
        Check(Reject(f,f.Outgoing()),"missing private moving-body membership cannot publish an imported bundle which undo cannot park");
        moving.push_back(second);moving[1]=moving[0];
        Check(Reject(f,f.Outgoing()),"duplicate private moving-body membership refuses before a live-array exchange");moving[1]=second;
        auto candidate=f.added.scene->PrepareReplacing(f.Outgoing());
        Check(candidate->IsCurrent()&&f.old->Present(),"restoring complete private membership permits a fresh reusable candidate");
    }
}
void FailureAndMeasurement() {
    Fixture f(true);const auto live=Storage::Read(f.physics.world),privateStorage=Storage::Read(static_cast<DynamicsWorld&>(*f.added.scene->Scene().world));
    auto* cache=f.physics.broadphase.Cache();auto* algorithm=f.PeerAlgorithm();bool reached=false;unsigned failures=0;
    for(long limit=0;limit<2048&&!reached;++limit) {
        std::unique_ptr<history_physics::ImportScene::Publication> candidate;
        failAfter=limit;
        try{candidate=f.added.scene->PrepareReplacing(f.Outgoing(true));reached=true;}catch(const std::bad_alloc&){++failures;}
        failAfter=-1;
        Check(live.Same(f.physics.world)&&privateStorage.Same(static_cast<DynamicsWorld&>(*f.added.scene->Scene().world))&&
            f.physics.broadphase.Cache()==cache&&f.PeerAlgorithm()==algorithm&&f.added.live.constraintId==17&&f.old->Present()&&f.second->Present(),
            "every C++ allocation failure preserves complete live/private array storage, owners, counter and source cache");candidate.reset();
    }
    Check(reached&&failures>30,"replacement allocation sweep reaches old Saved, target arrays, sweep/ray caches and pending owner");
    auto candidate=f.added.scene->PrepareReplacing(f.Outgoing());rb::Collector target;
    Check(candidate->MeasureTargetResources(target)&&target.Result().Complete(),"replacement target resource measurement succeeds before admission");const auto expected=target.Result();
    reached=false;failures=0;
    for(long limit=0;limit<256&&!reached;++limit) {
        rb::Collector measured;bool ok=false;failAfter=limit;
        try{ok=candidate->MeasureTargetResources(measured);reached=true;}catch(const std::bad_alloc&){++failures;}
        failAfter=-1;Check(candidate->IsCurrent()&&f.old->Present()&&f.second->Present()&&live.Same(f.physics.world),"failed measurement preserves a reusable unpublished replacement");
        if(reached)Check(ok&&SamePhysicsResources(measured.Result(),expected),"replacement measurement retry produces identical complete target accounting");
    }
    Check(reached&&failures>5,"replacement measurement fault sweep reaches all retained owners and their shared order");
    rb::Collector overflow;int sentinel=0;overflow.Add(&sentinel,1,UINT64_MAX);const auto before=overflow.Result();bool threw=false;
    try{candidate->MeasureTargetResources(overflow);}catch(const std::bad_alloc&){threw=true;}
    Check(threw&&SamePhysicsResources(overflow.Result(),before)&&candidate->IsCurrent(),"checked target budget overflow does not consume or publish the replacement");
    f.incoming=candidate->RetainedOwner();PublishImportWithoutAllocation(*candidate);f.added.published=true;candidate.reset();
    rb::Collector actual;f.old->MeasureResources(actual);f.second->MeasureResources(actual);f.incoming->MeasureResources(actual);
    Check(SamePhysicsResources(actual.Result(),expected),"all outgoing Saved states and the incoming owner retain the exact measured identities");
}
struct JournalChange final : plugins::HistoryChange, std::enable_shared_from_this<JournalChange> {
    Fixture& fixture;bool published=false;
    explicit JournalChange(Fixture& f):fixture(f){}
    struct Prepared final : plugins::PreparedHistoryChange {
        std::shared_ptr<JournalChange> owner;
        std::unique_ptr<history_physics::ImportScene::Publication> initial;
        std::shared_ptr<Retained> incoming;
        std::unique_ptr<Retained::Transition> replay;
        std::uint64_t bytes=0;bool consumed=false;
        bool IsCurrent()const noexcept override{return !consumed&&(initial?!owner->published&&initial->IsCurrent():owner->published&&replay&&replay->IsCurrent());}
        void Publish()noexcept override {
            if(!IsCurrent())std::terminate();
            if(initial){if(!initial->Publish())std::terminate();owner->fixture.incoming=std::move(incoming);owner->fixture.added.published=true;owner->published=true;}
            else if(!replay->Publish())std::terminate();consumed=true;
        }
        bool TargetRetainedBytes(std::uint64_t& out)const noexcept override{out=bytes;return true;}
    };
    MmsResult Prepare(bool forward,std::unique_ptr<plugins::PreparedHistoryChange>& out)override {
        return plugins::Boundary([&]() -> MmsResult {
            auto next=std::make_unique<Prepared>();next->owner=shared_from_this();rb::Collector measured;
            if(!published){if(!forward)return MMS_E_STALE;next->initial=fixture.added.scene->PrepareReplacing(fixture.Outgoing());next->incoming=next->initial->RetainedOwner();
                if(!next->initial->MeasureTargetResources(measured))return MMS_E_STALE;}
            else {next->replay=fixture.Exchange(forward);if(!next->replay->MeasureTargetResources(measured))return MMS_E_STALE;}
            // The outer resource-version owner must charge both native sets,
            // not just the presence bookkeeping. This test owns them in Fixture.
            for(unsigned i=0;i<2;++i)rb::Body(fixture.physics.bodies[i].get(),measured);
            for(auto& body:fixture.added.bodies)rb::Body(body.get(),measured);
            rb::Joint(fixture.physics.joint.get(),measured);rb::Joint(fixture.added.joint,measured);
            measured.Array(this,1);measured.Array(next.get(),1);
            if(!measured.Result().Complete())return MMS_E_NOT_READY;
            next->bytes=measured.Result().totalBytes;if(!next->IsCurrent())return MMS_E_STALE;out=std::move(next);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes()const noexcept override{return 0;}
};
void JournalBudget() {
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}) {
        Fixture f;plugins::StorageStore storage;plugins::SharedHistory history(storage,{},128,1);
        Check(history.SetMode(mode)==MMS_OK,"first-import replacement honors either common history recording mode");
        auto change=std::make_shared<JournalChange>(f);std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        const auto live=Storage::Read(f.physics.world),privateStorage=Storage::Read(static_cast<DynamicsWorld&>(*f.added.scene->Scene().world));
        Check(history.PrepareNative("replace private physics",change,token)==MMS_E_MEMORY&&!token&&!history.UndoCount()&&
            live.Same(f.physics.world)&&privateStorage.Same(static_cast<DynamicsWorld&>(*f.added.scene->Scene().world)),
            "complete native-resource budget rejection publishes neither old removal nor new import");
        Check(history.Configure(mode,128,1024*1024)==MMS_OK&&history.PrepareNative("replace private physics",change,token)==MMS_OK,
            "history admits one complete replacement node after preparation");
        bulletAllocations=0;trackBulletAlloc=true;failAfter=0;const auto result=history.CommitNative(std::move(token));failAfter=-1;trackBulletAlloc=false;
        Check(result==MMS_OK&&!bulletAllocations&&history.UndoCount()==1&&history.RetainedBytes()>sizeof(btRigidBody)*4,
            "one explicit first-import replacement commits without C++ or Bullet allocation and charges native resources");f.Order(true);
        Check(history.History(false)==MMS_OK&&history.RedoCount()==1,"common undo exchanges the original and imported bundles");f.Order(false);
        Check(history.History(true)==MMS_OK&&history.UndoCount()==1,"common redo uses retained imported resources rather than a second import");f.Order(true);
    }
}
}
void PhysicsImportReplaceChecks() {
    btAlignedAllocSetCustom(HistoryAlloc,HistoryFree);btAlignedAllocSetCustomAligned(HistoryAlignedAlloc,HistoryAlignedFree);
    physics_import_replace_checks::AtomicDirections();physics_import_replace_checks::InvalidAndStale();
    physics_import_replace_checks::FailureAndMeasurement();physics_import_replace_checks::JournalBudget();
    btAlignedAllocSetCustomAligned(nullptr,nullptr);btAlignedAllocSetCustom(nullptr,nullptr);
}
}
