// Include after history_physics_checks.inc. This exercises already-retained
// present/parked bundles, not private ImportScene publication or menu 262.
#include "../src/physics/bullet_import_storage.hpp"
namespace {
namespace physics_exchange_checks {
using Retained = history_physics::Retained;
using Changes = std::vector<Retained::PresenceChange>;
namespace rb = plugins::resource_budget;

struct Fixture {
    PhysicsFixture physics;
    std::unique_ptr<btGeneric6DofConstraint> incomingJoint, peerFirst, peerSecond;
    std::unique_ptr<Retained> oldBundle, newBundle;
    Fixture() {
        for (const auto& position : {btVector3(0, 7.8f, 0), btVector3(.3f, 5.7f, 0)}) {
            btVector3 inertia; physics.shape.calculateLocalInertia(1, inertia);
            auto body = std::make_unique<btRigidBody>(1, nullptr, &physics.shape, inertia);
            auto transform = btTransform::getIdentity(); transform.setOrigin(position);
            body->setWorldTransform(transform); body->setInterpolationWorldTransform(transform);
            body->forceActivationState(DISABLE_DEACTIVATION);
            physics.world.addRigidBody(body.get(), 1, -1); physics.bodies.push_back(std::move(body));
        }
        const auto joint = [&](unsigned a, unsigned b) {
            auto result = std::make_unique<btGeneric6DofConstraint>(*physics.bodies[a], *physics.bodies[b],
                btTransform::getIdentity(), btTransform::getIdentity(), true);
            physics.world.addConstraint(result.get(), false); return result;
        };
        incomingJoint = joint(2, 5); peerFirst = joint(3, 4); peerSecond = joint(3, 4);
        oldBundle = physics.Capture();
        newBundle = std::make_unique<Retained>(physics.world,
            std::vector<btRigidBody*>{physics.bodies[2].get(), physics.bodies[5].get()},
            std::vector<btTypedConstraint*>{incomingJoint.get()});
        physics.world.performDiscreteCollisionDetection();
        PublishPhysicsBatch(newBundle->Prepare(false));
        Order(false);
    }
    ~Fixture() {
        oldBundle.reset(); newBundle.reset();
        physics.world.removeConstraint(incomingJoint.get());
        physics.world.removeConstraint(peerFirst.get()); physics.world.removeConstraint(peerSecond.get());
    }
    Changes Exchange(bool forward, bool reverse = false) {
        Changes changes{{oldBundle.get(), !forward}, {newBundle.get(), forward}};
        if (reverse) std::reverse(changes.begin(), changes.end());
        return changes;
    }
    void Order(bool incoming) {
        CheckPhysicsOrder(physics, incoming ? std::vector<unsigned>{2, 3, 4, 5, 6} : std::vector<unsigned>{0, 1, 3, 4, 6});
        Check(physics.world.getNumConstraints() == 3 &&
            physics.world.getConstraint(0) == (incoming ? incomingJoint.get() : physics.joint.get()) &&
            physics.world.getConstraint(1) == peerFirst.get() && physics.world.getConstraint(2) == peerSecond.get(),
            "mixed exchange preserves exact unrelated constraint order and the active bundle joint");
        Check(oldBundle->Present() == !incoming && newBundle->Present() == incoming,
            "mixed exchange publishes both owner directions together");
    }
    btCollisionAlgorithm* PeerAlgorithm() {
        auto* pair = physics.broadphase.Cache()->findPair(physics.bodies[3]->getBroadphaseHandle(), physics.bodies[4]->getBroadphaseHandle());
        Check(pair && pair->m_algorithm, "unrelated overlapping bodies retain a real contact algorithm");
        return pair->m_algorithm;
    }
};
bool Reject(const Changes& changes) {
    try { auto candidate = Retained::PrepareExchange(changes); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}
bool Ray(DynamicsWorld& world, btRigidBody* target) {
    struct Hits final : btCollisionWorld::RayResultCallback {
        btRigidBody* target; bool hit = false;
        explicit Hits(btRigidBody* value): target(value) {}
        btScalar addSingleResult(btCollisionWorld::LocalRayResult& result, bool) override {
            hit = hit || result.m_collisionObject == target; return 1;
        }
    } hits(target);
    const auto origin = target->getWorldTransform().getOrigin();
    world.rayTest(origin - btVector3(3, 0, 0), origin + btVector3(3, 0, 0), hits); return hits.hit;
}
bool Contact(PhysicsFixture& f, btRigidBody* target) {
    for (int i = 0; i < f.dispatcher.getNumManifolds(); ++i) {
        const auto* manifold = f.dispatcher.getManifoldByIndexInternal(i);
        if (manifold->getBody0() == target || manifold->getBody1() == target) return true;
    }
    return false;
}
void DirectionsAndCancellation() {
    for (bool reverse : {false, true}) {
        Fixture f; const auto* algorithm = f.PeerAlgorithm(); const auto* cache = f.physics.broadphase.Cache();
        const auto oldTransform = f.physics.bodies[0]->getWorldTransform();
        const auto newTransform = f.physics.bodies[2]->getWorldTransform();
        const auto velocity = btVector3(.25f, -.5f, .125f); f.physics.bodies[3]->setLinearVelocity(velocity);
        auto canceled = Retained::PrepareExchange(f.Exchange(true, reverse));
        Check(canceled->IsCurrent() && f.physics.broadphase.Cache() == cache &&
            Ray(f.physics.world, f.physics.bodies[0].get()) && !Ray(f.physics.world, f.physics.bodies[2].get()),
            "mixed prepare leaves source masks, rays and live cache unchanged");
        f.Order(false); canceled.reset();
        Check(f.PeerAlgorithm() == algorithm && Contact(f.physics, f.physics.bodies[0].get()),
            "mixed cancellation retains source contacts and unrelated algorithm ownership");
        auto staleSingle = f.oldBundle->Prepare(false);
        for (unsigned i = 0; i != 12; ++i) {
            PublishPhysicsBatch(Retained::PrepareExchange(f.Exchange(true, reverse)));
            f.Order(true);
            Check(f.PeerAlgorithm() == algorithm && !Ray(f.physics.world, f.physics.bodies[0].get()) &&
                Ray(f.physics.world, f.physics.bodies[2].get()), "mixed publication parks outgoing ray proxies and restores incoming masks");
            Check(!Contact(f.physics, f.physics.bodies[0].get()), "retired outgoing contact algorithms are cleaned before the next physics pass");
            f.physics.world.performDiscreteCollisionDetection();
            Check(Contact(f.physics, f.physics.bodies[2].get()), "incoming bundle contacts are regenerated against unchanged live peers");
            PublishPhysicsBatch(Retained::PrepareExchange(f.Exchange(false, !reverse)));
            f.Order(false);
            Check(f.PeerAlgorithm() == algorithm && f.physics.bodies[3]->getLinearVelocity() == velocity &&
                f.physics.bodies[0]->getWorldTransform() == oldTransform && f.physics.bodies[2]->getWorldTransform() == newTransform,
                "bidirectional mixed exchange preserves other body velocity and both bundle transforms");
        }
        Check(!staleSingle->IsCurrent() && !staleSingle->Publish(), "mixed exchange invalidates earlier single-owner candidates after an ABA cycle");
        staleSingle.reset();
    }
}
void InvalidAndConflictingSources() {
    Fixture f;
    Check(Reject({}) && Reject({{nullptr, true}}) && Reject({{f.oldBundle.get(), true}}) &&
        Reject({{f.newBundle.get(), false}}) && Reject({{f.oldBundle.get(), false}, {f.oldBundle.get(), false}}),
        "mixed exchange rejects empty/null, same-state and duplicate-owner requests");
    Retained overlap(f.physics.world, {f.physics.bodies[0].get()}, {});
    Check(Reject({{f.oldBundle.get(), false}, {&overlap, false}, {f.newBundle.get(), true}}),
        "mixed exchange rejects overlapping owners before changing either bundle");
    PhysicsFixture foreign; auto foreignOwner = foreign.Capture();
    Check(Reject({{f.oldBundle.get(), false}, {foreignOwner.get(), false}, {f.newBundle.get(), true}}),
        "mixed exchange rejects different worlds without publishing any requested direction");
    auto cross = std::make_unique<btPoint2PointConstraint>(*f.physics.bodies[0], *f.physics.bodies[3], btVector3(0,0,0), btVector3(0,0,0));
    f.physics.world.addConstraint(cross.get(), false);
    Check(Reject(f.Exchange(true)), "mixed exchange rejects external joints referencing an outgoing body");
    f.physics.world.removeConstraint(cross.get()); f.Order(false);
    for (unsigned kind = 0; kind != 3; ++kind) {
        Fixture conflict; auto* cache = conflict.physics.broadphase.Cache();
        if (kind == 0) std::swap(conflict.physics.world.getCollisionObjectArray()[2], conflict.physics.world.getCollisionObjectArray()[3]);
        if (kind == 1) std::swap(conflict.physics.world.HistoryBodies()[2], conflict.physics.world.HistoryBodies()[3]);
        if (kind == 2) std::swap(conflict.physics.world.HistoryConstraints()[1], conflict.physics.world.HistoryConstraints()[2]);
        Check(Reject(conflict.Exchange(true)) && conflict.oldBundle->Present() && !conflict.newBundle->Present() &&
            conflict.physics.broadphase.Cache() == cache, "conflicting incoming historical anchors reject in prepare without silently reordering peers");
        if (kind == 0) std::swap(conflict.physics.world.getCollisionObjectArray()[2], conflict.physics.world.getCollisionObjectArray()[3]);
        if (kind == 1) std::swap(conflict.physics.world.HistoryBodies()[2], conflict.physics.world.HistoryBodies()[3]);
        if (kind == 2) std::swap(conflict.physics.world.HistoryConstraints()[1], conflict.physics.world.HistoryConstraints()[2]);
        conflict.Order(false);
    }
    for (unsigned mutation = 0; mutation != 7; ++mutation) {
        Fixture source; auto candidate = Retained::PrepareExchange(source.Exchange(true));
        HistoryOverlapFilter filter;
        if (mutation == 0) source.physics.bodies[0]->setLinearVelocity(btVector3(1, 2, 3));
        if (mutation == 1) source.physics.bodies[2]->setLinearVelocity(btVector3(1, 2, 3));
        if (mutation == 2) source.physics.bodies[2]->forceActivationState(ACTIVE_TAG);
        if (mutation == 3) std::swap(source.physics.world.getCollisionObjectArray()[2], source.physics.world.getCollisionObjectArray()[3]);
        if (mutation == 4) source.physics.broadphase.Cache()->setOverlapFilterCallback(&filter);
        if (mutation == 5) source.physics.world.stepSimulation(btScalar(1.f / 60.f));
        if (mutation == 6) {
            PublishPhysicsBatch(Retained::PrepareExchange(source.Exchange(true)));
            PublishPhysicsBatch(Retained::PrepareExchange(source.Exchange(false)));
        }
        rb::Collector target; int sentinel = 0; target.Add(&sentinel, 1, 17); const auto untouched = target.Result();
        failAfter = 0;
        const bool current = candidate->IsCurrent(), measured = candidate->MeasureTargetResources(target), published = candidate->Publish();
        failAfter = -1;
        Check(!current && !measured && !published && SamePhysicsResources(target.Result(), untouched) &&
            source.oldBundle->Present() && !source.newBundle->Present(),
            "source mutation rejects every mixed direction and target measurement before any allocation or publication");
        if (mutation == 4) source.physics.broadphase.Cache()->setOverlapFilterCallback(nullptr);
        if (mutation == 3) std::swap(source.physics.world.getCollisionObjectArray()[2], source.physics.world.getCollisionObjectArray()[3]);
        candidate.reset();
    }
}
void FailureAndTargetResources() {
    Fixture f; rb::Collector before; f.oldBundle->MeasureResources(before); f.newBundle->MeasureResources(before);
    const auto* cache = f.physics.broadphase.Cache(); const auto* algorithm = f.PeerAlgorithm();
    bool reached = false; unsigned failures = 0;
    for (long limit = 0; limit != 1024 && !reached; ++limit) {
        std::unique_ptr<Retained::Transition> candidate;
        failAfter = limit;
        try { candidate = Retained::PrepareExchange(f.Exchange(true)); reached = true; }
        catch (const std::bad_alloc&) { ++failures; }
        failAfter = -1;
        f.Order(false);
        Check(f.physics.broadphase.Cache() == cache && f.PeerAlgorithm() == algorithm &&
            Ray(f.physics.world, f.physics.bodies[0].get()) && !Ray(f.physics.world, f.physics.bodies[2].get()),
            "every mixed preparation allocation failure preserves both owner directions, contacts and rays");
        candidate.reset();
    }
    Check(reached && failures > 20, "mixed allocation sweep reaches complete private preparation");
    auto candidate = Retained::PrepareExchange(f.Exchange(true)); rb::Collector target;
    Check(candidate->MeasureTargetResources(target) && target.Result().Complete() &&
        target.Result().cpuBytes > before.Result().cpuBytes, "mixed target measures outgoing new Saved plus incoming retained Saved and shared order resources");
    const auto expected = target.Result();
    Check(candidate->MeasureTargetResources(target) && SamePhysicsResources(target.Result(), expected),
        "repeated mixed target measurement deduplicates actual owner and order identities");
    reached = false; failures = 0;
    for (long limit = 0; limit != 128 && !reached; ++limit) {
        rb::Collector measured; bool ok = false;
        failAfter = limit;
        try { ok = candidate->MeasureTargetResources(measured); reached = true; }
        catch (const std::bad_alloc&) { ++failures; }
        failAfter = -1;
        Check(candidate->IsCurrent() && f.oldBundle->Present() && !f.newBundle->Present(),
            "mixed measurement allocation failures preserve the reusable candidate and live scene");
        if (reached) Check(ok && SamePhysicsResources(measured.Result(), expected), "mixed measurement retry retains its complete target budget");
    }
    Check(reached && failures, "mixed target measurement covers failure and recovery");
    rb::Collector overflow; int sentinel = 0; overflow.Add(&sentinel, 1, UINT64_MAX); const auto unchanged = overflow.Result();
    bool overflowed = false;
    try { candidate->MeasureTargetResources(overflow); } catch (const std::bad_alloc&) { overflowed = true; }
    Check(overflowed && SamePhysicsResources(overflow.Result(), unchanged) && candidate->IsCurrent(),
        "mixed checked budget overflow refuses without touching owners or consuming the candidate");
    PublishPhysicsBatch(std::move(candidate)); f.Order(true);
    rb::Collector actual; f.oldBundle->MeasureResources(actual); f.newBundle->MeasureResources(actual);
    Check(SamePhysicsResources(actual.Result(), expected), "published mixed Saved bookkeeping exactly matches the prepared resource charge");
    auto reverse = Retained::PrepareExchange(f.Exchange(false));
    f.oldBundle.reset(); f.newBundle.reset();
    Check(reverse->IsCurrent(), "mixed candidate retains both owner internals when wrappers are released");
    PublishPhysicsBatch(std::move(reverse));
    CheckPhysicsOrder(f.physics, {0, 1, 3, 4, 6});
}
// The fixture owns the native Bullet allocations. This adapter admits the
// exchange's actual owner/Saved bookkeeping to the existing journal budget;
// production model bundles must separately charge their native resources.
struct JournalChange final : plugins::HistoryChange {
    Retained& oldBundle;
    Retained& newBundle;
    // A separately retained test payload can grow before journal admission.
    // This forces the real undo budget gate after native preparation succeeds.
    std::uint64_t additionalTargetBytes = 0;
    JournalChange(Retained& a, Retained& b): oldBundle(a), newBundle(b) {}
    struct Prepared final : plugins::PreparedHistoryChange {
        std::unique_ptr<Retained::Transition> transition;
        std::uint64_t bytes;
        Prepared(std::unique_ptr<Retained::Transition> value, std::uint64_t charge): transition(std::move(value)), bytes(charge) {}
        bool IsCurrent() const noexcept override { return transition->IsCurrent(); }
        void Publish() noexcept override { if (!transition->Publish()) std::terminate(); }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<plugins::PreparedHistoryChange>& out) override {
        return plugins::Boundary([&]() -> MmsResult {
            auto transition = Retained::PrepareExchange({{&oldBundle, !forward}, {&newBundle, forward}});
            rb::Collector target; if (!transition->MeasureTargetResources(target)) return MMS_E_STALE;
            if (!target.Result().Complete()) return MMS_E_NOT_READY;
            const auto bytes = rb::Collector::Sum(rb::Collector::Sum(target.Result().totalBytes,
                sizeof(JournalChange) + sizeof(Prepared)), additionalTargetBytes);
            auto prepared = std::make_unique<Prepared>(std::move(transition), bytes); out = std::move(prepared); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return 0; }
};
void JournalBudget() {
    using Mode = plugins::SharedHistory::RecordingMode;
    for (Mode mode : {Mode::AllOperations, Mode::ExplicitOnly}) {
        Fixture f; plugins::StorageStore storage; plugins::SharedHistory history(storage, {}, 128, 1);
        Check(history.SetMode(mode) == MMS_OK, "mixed exchange journal recording mode");
        auto change = std::make_shared<JournalChange>(*f.oldBundle, *f.newBundle);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(history.PrepareNative("mixed physics exchange", change, token) == MMS_E_MEMORY && !token && !history.UndoCount(),
            "journal budget refusal publishes neither mixed direction nor a history node"); f.Order(false);
        Check(history.Configure(mode, 128, 1024 * 1024) == MMS_OK &&
            history.PrepareNative("mixed physics exchange", change, token) == MMS_OK && token,
            "journal reserves a measured mixed exchange before the first publication");
        bulletAllocations = 0; trackBulletAlloc = true; failAfter = 0;
        const auto result = history.CommitNative(std::move(token));
        failAfter = -1; trackBulletAlloc = false;
        Check(result == MMS_OK && !bulletAllocations && history.UndoCount() == 1 && history.RetainedBytes(),
            "both recording modes commit one explicit mixed exchange without C++ or Bullet allocation"); f.Order(true);
        Check(history.History(false) == MMS_OK && history.RedoCount() == 1, "common undo reverses both mixed bundle directions"); f.Order(false);
        Check(history.History(true) == MMS_OK && history.UndoCount() == 1, "common redo restores both mixed bundle directions"); f.Order(true);
    }
}
struct WorldStorage {
    struct Array { const void* pointer; int size, capacity; };
    Array objects, moving, constraints;
    static WorldStorage Read(DynamicsWorld& world) noexcept {
        namespace storage = history_physics::import_storage;
        auto& objects = world.getCollisionObjectArray(); auto& moving = world.HistoryBodies();
        auto& constraints = world.HistoryConstraints();
        // Read storage even at size zero, without indexing an empty array.
        return {{objects.*Access(storage::ObjectsData{}), objects.size(), objects.capacity()},
            {moving.*Access(storage::BodiesData{}), moving.size(), moving.capacity()},
            {constraints.*Access(storage::ConstraintsData{}), constraints.size(), constraints.capacity()}};
    }
    bool Same(DynamicsWorld& world) const noexcept {
        const auto now = Read(world);
        const auto equal = [](const Array& a, const Array& b) {
            return a.pointer == b.pointer && a.size == b.size && a.capacity == b.capacity;
        };
        return equal(objects, now.objects) && equal(moving, now.moving) && equal(constraints, now.constraints);
    }
};
struct ShrunkFixture {
    PhysicsFixture physics;
    std::unique_ptr<btGeneric6DofConstraint> peerJoint;
    std::unique_ptr<Retained> oldBundle;
    PhysicsScene live{};
    std::unique_ptr<history_physics::ImportScene> scene;
    std::shared_ptr<Retained> emptyBundle;
    bool peers;
    explicit ShrunkFixture(bool keepPeers, bool importEmpty = true): peers(keepPeers) {
        if (!peers) for (unsigned i = 2; i != 5; ++i) physics.world.removeRigidBody(physics.bodies[i].get());
        else {
            peerJoint = std::make_unique<btGeneric6DofConstraint>(*physics.bodies[3], *physics.bodies[4],
                btTransform::getIdentity(), btTransform::getIdentity(), true);
            physics.world.addConstraint(peerJoint.get(), false);
        }
        oldBundle = physics.Capture();
        if (importEmpty) {
            live.world = &physics.world; live.dispatcher = &physics.dispatcher; live.constraintId = 17;
            scene = std::make_unique<history_physics::ImportScene>(live);
            auto publication = scene->PrepareReplacing({oldBundle.get()}); emptyBundle = publication->RetainedOwner();
            bulletAllocations = 0; trackBulletAlloc = true; failAfter = 0;
            const bool published = publication->Publish();
            failAfter = -1; trackBulletAlloc = false;
            Check(published && !bulletAllocations, "empty first import publishes without allocation before replay storage checks");
            publication.reset(); Order(false);
            const auto arrays = WorldStorage::Read(physics.world);
            Check(arrays.objects.capacity == (peers ? 3 : 0) && arrays.moving.capacity == (peers ? 3 : 0) &&
                arrays.constraints.capacity == (peers ? 1 : 0), "first empty import leaves exactly the surviving world array capacities");
        } else {
            emptyBundle = std::make_shared<Retained>(physics.world, std::vector<btRigidBody*>{}, std::vector<btTypedConstraint*>{});
            PublishPhysicsBatch(emptyBundle->Prepare(false)); Order(true);
        }
    }
    ~ShrunkFixture() {
        emptyBundle.reset(); oldBundle.reset(); scene.reset();
        if (peerJoint) physics.world.removeConstraint(peerJoint.get());
    }
    Changes Restore() { return {{oldBundle.get(), true}, {emptyBundle.get(), false}}; }
    Changes Shrink() { return {{oldBundle.get(), false}, {emptyBundle.get(), true}}; }
    void Order(bool restored) {
        CheckPhysicsOrder(physics, restored ? (peers ? std::vector<unsigned>{0, 1, 2, 3, 4} : std::vector<unsigned>{0, 1}) :
            (peers ? std::vector<unsigned>{2, 3, 4} : std::vector<unsigned>{}));
        Check(physics.world.getNumConstraints() == (restored ? 1 : 0) + (peers ? 1 : 0) &&
            (!restored || physics.world.getConstraint(0) == physics.joint.get()) &&
            (!peers || physics.world.getConstraint(restored ? 1 : 0) == peerJoint.get()) &&
            oldBundle->Present() == restored && emptyBundle->Present() != restored,
            "zero-member replay restores exact owner, joint and unaffected peer order");
    }
    btCollisionAlgorithm* PeerAlgorithm() {
        if (!peers) return nullptr;
        auto* pair = physics.broadphase.Cache()->findPair(physics.bodies[3]->getBroadphaseHandle(), physics.bodies[4]->getBroadphaseHandle());
        Check(pair && pair->m_algorithm, "zero-member replay fixture retains a real peer contact algorithm"); return pair->m_algorithm;
    }
};
long privateArrayFailAfter = -1;
unsigned privateArrayAllocations = 0;
bool privateArrayFailed = false;
void PrivateArrayAllocation() {
    ++privateArrayAllocations;
    if (privateArrayFailAfter == 0) { privateArrayFailed = true; throw std::bad_alloc(); }
    if (privateArrayFailAfter > 0) --privateArrayFailAfter;
}
void* PrivateArrayAlloc(size_t bytes) { PrivateArrayAllocation(); return HistoryAlloc(bytes); }
void* PrivateArrayAlignedAlloc(size_t bytes, int alignment) { PrivateArrayAllocation(); return HistoryAlignedAlloc(bytes, alignment); }
struct PrivateArrayAllocator {
    PrivateArrayAllocator() {
        btAlignedAllocSetCustom(PrivateArrayAlloc, HistoryFree);
        btAlignedAllocSetCustomAligned(PrivateArrayAlignedAlloc, HistoryAlignedFree);
    }
    ~PrivateArrayAllocator() {
        privateArrayFailAfter = -1;
        btAlignedAllocSetCustom(HistoryAlloc, HistoryFree);
        btAlignedAllocSetCustomAligned(HistoryAlignedAlloc, HistoryAlignedFree);
    }
};
void ShrunkWorldPreparation() {
    for (bool peers : {false, true}) {
        ShrunkFixture f(peers); const auto storage = WorldStorage::Read(f.physics.world);
        auto* const cache = f.physics.broadphase.Cache(); auto* const algorithm = f.PeerAlgorithm();
        const auto peerVelocity = f.physics.bodies[3]->getLinearVelocity();
        const auto peerTransform = f.physics.bodies[3]->getWorldTransform();
        rb::Collector baseline; f.oldBundle->MeasureResources(baseline); f.emptyBundle->MeasureResources(baseline);
        auto canceled = Retained::PrepareExchange(f.Restore());
        Check(canceled->IsCurrent() && storage.Same(f.physics.world), "undo preparation after an empty import leaves live storage and capacities unchanged");
        canceled.reset();
        Check(storage.Same(f.physics.world) && cache == f.physics.broadphase.Cache() && algorithm == f.PeerAlgorithm(),
            "canceling zero-member undo releases private arrays without changing live contacts or storage");
        bool reached = false; unsigned failures = 0;
        for (long limit = 0; limit != 1024 && !reached; ++limit) {
            std::unique_ptr<Retained::Transition> candidate; failAfter = limit;
            try { candidate = Retained::PrepareExchange(f.Restore()); reached = true; }
            catch (const std::bad_alloc&) { ++failures; }
            failAfter = -1; candidate.reset();
            Check(storage.Same(f.physics.world) && cache == f.physics.broadphase.Cache() && algorithm == f.PeerAlgorithm() &&
                !f.physics.RayHitsFirst(), "each zero-member undo allocation failure preserves storage, capacities, cache and parked masks"); f.Order(false);
        }
        Check(reached && failures > 20, "zero-member undo allocation sweep reaches complete preparation without live reserve");
        {
            PrivateArrayAllocator allocations; privateArrayAllocations = 0;
            auto candidate = Retained::PrepareExchange(f.Restore()); const auto count = privateArrayAllocations; candidate.reset();
            Check(count >= 3, "restoration prepares three nonempty private Bullet world arrays");
            // These are the final three Bullet allocations in Transition prepare;
            // fail each array after every preceding cache/allocation has succeeded.
            for (unsigned index = count - 3; index != count; ++index) {
                privateArrayAllocations = 0; privateArrayFailed = false; privateArrayFailAfter = index;
                bool rejected = false;
                try { candidate = Retained::PrepareExchange(f.Restore()); }
                catch (const std::bad_alloc&) { rejected = true; }
                privateArrayFailAfter = -1; candidate.reset();
                Check(rejected && privateArrayFailed && privateArrayAllocations == index + 1 && storage.Same(f.physics.world) &&
                    cache == f.physics.broadphase.Cache() && algorithm == f.PeerAlgorithm(),
                    "failure preparing each private Bullet array retains exact zero-member source storage and algorithms"); f.Order(false);
            }
        }
        rb::Collector canceledResources; f.oldBundle->MeasureResources(canceledResources); f.emptyBundle->MeasureResources(canceledResources);
        Check(SamePhysicsResources(baseline.Result(), canceledResources.Result()), "failed restoration retains the same owner and saved-state budget identities");
        for (unsigned iteration = 0; iteration != 12; ++iteration) {
            auto candidate = Retained::PrepareExchange(f.Restore()); rb::Collector target;
            Check(candidate->MeasureTargetResources(target) && target.Result().Complete(), "zero-member undo has a complete prepared bookkeeping charge");
            PublishPhysicsBatch(std::move(candidate)); f.Order(true);
            rb::Collector actual; f.oldBundle->MeasureResources(actual); f.emptyBundle->MeasureResources(actual);
            Check(SamePhysicsResources(target.Result(), actual.Result()), "zero-member restore publishes exactly its measured bookkeeping identities");
            Check(f.physics.RayHitsFirst() && algorithm == f.PeerAlgorithm(), "restoration preserves peer contact algorithms and restores parked ray masks");
            PublishPhysicsBatch(Retained::PrepareExchange(f.Shrink())); f.Order(false);
            const auto arrays = WorldStorage::Read(f.physics.world);
            Check(arrays.objects.capacity == (peers ? 3 : 0) && arrays.moving.capacity == (peers ? 3 : 0) &&
                arrays.constraints.capacity == (peers ? 1 : 0) && algorithm == f.PeerAlgorithm() &&
                f.physics.bodies[3]->getLinearVelocity() == peerVelocity && f.physics.bodies[3]->getWorldTransform() == peerTransform,
                "repeated empty-version replay retains exact smaller capacities and unparticipating peer state");
        }
    }
}
void ShrunkWorldJournalBudget() {
    using Mode = plugins::SharedHistory::RecordingMode;
    for (Mode mode : {Mode::AllOperations, Mode::ExplicitOnly}) for (bool peers : {false, true}) {
        ShrunkFixture f(peers, false); plugins::StorageStore storage; plugins::SharedHistory history(storage, {}, 128, 1024 * 1024);
        Check(history.SetMode(mode) == MMS_OK, "zero-member exchange uses both common history recording modes");
        auto change = std::make_shared<JournalChange>(*f.oldBundle, *f.emptyBundle);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(history.PrepareNative("empty physics version", change, token) == MMS_OK &&
            history.CommitNative(std::move(token)) == MMS_OK, "one explicit history node records shrinking the physics version to no members"); f.Order(false);
        const auto source = WorldStorage::Read(f.physics.world); const auto* cache = f.physics.broadphase.Cache();
        const auto* algorithm = f.PeerAlgorithm(); const auto retained = history.RetainedBytes();
        uint64_t revision = 0, after = 0; Check(history.Revision(revision) == MMS_OK, "read zero-member history source revision");
        change->additionalTargetBytes = history.MaxBytes();
        Check(history.History(false) == MMS_E_MEMORY && source.Same(f.physics.world) && cache == f.physics.broadphase.Cache() &&
            algorithm == f.PeerAlgorithm() && history.UndoCount() == 1 && !history.RedoCount() && history.RetainedBytes() == retained &&
            history.Revision(after) == MMS_OK && after == revision,
            "actual undo budget refusal after restoration preparation leaves source arrays, capacities, contacts and journal unchanged"); f.Order(false);
        change->additionalTargetBytes = 0;
        Check(history.History(false) == MMS_OK, "zero-member undo recovers after budget admission is available"); f.Order(true);
        const auto restored = WorldStorage::Read(f.physics.world);
        Check(history.PrepareNative("cancel empty version", change, token) == MMS_OK && token,
            "prepare a new shrink while the previous version is available in redo");
        token.reset();
        Check(restored.Same(f.physics.world) && !history.UndoCount() && history.RedoCount() == 1,
            "canceling a prepared empty-version transaction preserves storage and the existing redo branch");
        Check(history.History(true) == MMS_OK, "zero-member redo remains replayable after cancellation"); f.Order(false);
    }
}
} // namespace physics_exchange_checks
void PhysicsExchangeChecks() {
    btAlignedAllocSetCustom(HistoryAlloc, HistoryFree);
    btAlignedAllocSetCustomAligned(HistoryAlignedAlloc, HistoryAlignedFree);
    physics_exchange_checks::DirectionsAndCancellation();
    physics_exchange_checks::InvalidAndConflictingSources();
    physics_exchange_checks::FailureAndTargetResources();
    physics_exchange_checks::JournalBudget();
    physics_exchange_checks::ShrunkWorldPreparation();
    physics_exchange_checks::ShrunkWorldJournalBudget();
    btAlignedAllocSetCustomAligned(nullptr, nullptr); btAlignedAllocSetCustom(nullptr, nullptr);
}
} // namespace
