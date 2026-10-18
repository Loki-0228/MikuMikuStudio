// Include after the shared Fixture/Check definitions in native_edit_history_tests.
// These checks exercise the retained-owner API and its real SharedHistory budget
// protocol. Camera-follow's production integration has its own dedicated suite.
namespace {
namespace retained_bookkeeping_checks {
namespace rb = plugins::resource_budget;
bool SameReport(const rb::Report& a, const rb::Report& b) {
    return a.cpuBytes == b.cpuBytes && a.d3d9Bytes == b.d3d9Bytes &&
        a.totalBytes == b.totalBytes && a.missing == b.missing;
}
struct AccessoryFixture {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    mdl::AccessoryRecord object{};
    std::shared_ptr<plugins::RetainedSceneObject> owner;
    AccessoryFixture() {
        ::new (static_cast<void*>(&app->state)) MMDAppState{};
        object.scale = object.opacity = 1; object.parentModel = -1;
        app->AccessorySlot(3) = &object;
        app->AccessoryKeys(3) = mdl::KeyPoolCreate<mdl::AccessoryKey>(3);
        auto& root = app->AccessoryKeys(3)[0];
        root.visible = 1; root.parentModel = -1; root.scale = root.opacity = 1;
        owner = plugins::RetainSceneObject(*app, MMS_OBJECT_ACCESSORY, 3);
    }
    ~AccessoryFixture() {
        // Every test restores presence before releasing this stack-owned object.
        owner.reset(); curve_host::Reset(app.get());
        plugins::ForgetSceneObject(&object); app->AccessorySlot(3) = nullptr;
        mdl::KeyPoolDestroy(app->AccessoryKeys(3));
    }
};

// An ordinary HistoryChange using the public retained-owner ledger contract.
// The bool is an independent payload so budget/stale refusal can prove that no
// publication happened, rather than merely inspecting a pure size calculation.
struct BudgetProbe final : plugins::HistoryChange {
    std::shared_ptr<plugins::RetainedSceneObject> owner;
    bool& payload;
    explicit BudgetProbe(std::shared_ptr<plugins::RetainedSceneObject> value, bool& target)
        : owner(std::move(value)), payload(target) {}
    struct Candidate final : plugins::PreparedHistoryChange {
        std::shared_ptr<plugins::RetainedSceneObject> owner;
        mutable rb::Collector resources;
        bool& payload;
        bool before, after;
        std::uint64_t bytes = 0;
        Candidate(std::shared_ptr<plugins::RetainedSceneObject> value, bool& target, bool forward)
            : owner(std::move(value)), payload(target), before(target), after(forward) {}
        bool IsCurrent() const noexcept override {
            // Covers mutates only its private verification marker. No node or
            // retained allocation is inserted in this final publication check.
            return payload == before && owner->CoversBookkeeping(resources);
        }
        void Publish() noexcept override { payload = after; }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<plugins::PreparedHistoryChange>& out) override {
        out.reset(); return plugins::Boundary([&]() -> MmsResult {
            auto next = std::make_unique<Candidate>(owner, payload, forward);
            if (auto result = owner->MeasureBookkeeping(next->resources)) return result;
            if (!next->resources.Result().Complete()) return MMS_E_NOT_READY;
            next->bytes = rb::Collector::Sum(sizeof(*this), next->resources.Result().totalBytes);
            out = std::move(next); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this); }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};

void AccessoryAllocations() {
    AccessoryFixture f;
    auto* active = f.app->AccessoryKeys(3);
    const auto before = f.object;
    const auto root = active[0];
    rb::Collector measured;
    // No renderer query is permissible merely to retain an active object. An
    // opaque live mesh must remain untouched by this bookkeeping-only API.
    auto* mesh = f.object.mesh;
    f.object.mesh = reinterpret_cast<decltype(f.object.mesh)>(std::uintptr_t(1));
    const auto result = f.owner->MeasureBookkeeping(measured);
    f.object.mesh = mesh;
    const auto first = measured.Result();
    Check(result == MMS_OK && first.Complete() && first.d3d9Bytes == 0 &&
        first.cpuBytes >= mdl::KeyPoolStorage<mdl::AccessoryKey>::initial * sizeof(mdl::AccessoryKey),
        "retained accessory bookkeeping counts its actual 600KB parked pool without querying live graphics");
    Check(std::memcmp(&f.object, &before, sizeof(before)) == 0 && f.app->AccessoryKeys(3) == active &&
        std::memcmp(&active[0], &root, sizeof(root)) == 0 && f.owner->Present(),
        "bookkeeping measurement preserves the active object pool payload and membership");
    auto duplicate = plugins::RetainSceneObject(*f.app, MMS_OBJECT_ACCESSORY, 3);
    Check(duplicate == f.owner && duplicate->MeasureBookkeeping(measured) == MMS_OK && SameReport(first, measured.Result()),
        "a shared owner and all its allocations are charged once in one collector");
    duplicate.reset();
    failAfter = 0;
    const bool covered = f.owner->CoversBookkeeping(measured);
    const auto repeated = f.owner->MeasureBookkeeping(measured);
    failAfter = -1;
    Check(covered && repeated == MMS_OK && SameReport(first, measured.Result()),
        "retained bookkeeping validates and deduplicates an existing ledger without allocation");

    rb::Collector empty;
    failAfter = 0; const bool missing = f.owner->CoversBookkeeping(empty); failAfter = -1;
    Check(!missing && empty.Result().cpuBytes == 0 && empty.Result().Complete(),
        "an unmeasured owner is refused without inserting identities into the validation ledger");
    auto* occupied = f.app->AccessorySlot(3); f.app->AccessorySlot(3) = nullptr;
    failAfter = 0; const bool stale = f.owner->CoversBookkeeping(measured); failAfter = -1;
    f.app->AccessorySlot(3) = occupied;
    Check(!stale && f.owner->CoversBookkeeping(measured) && SameReport(first, measured.Result()),
        "stale live membership fails without consuming the ledger and can recover");

    bool reached = false; unsigned failures = 0;
    for (long limit = 0; limit < 128; ++limit) {
        rb::Collector attempt;
        failAfter = limit; const auto status = f.owner->MeasureBookkeeping(attempt); failAfter = -1;
        Check(f.owner->Present() && f.app->AccessoryKeys(3) == active && f.app->AccessorySlot(3) == &f.object &&
            std::memcmp(&f.object, &before, sizeof(before)) == 0 && std::memcmp(&active[0], &root, sizeof(root)) == 0,
            "every bookkeeping allocation failure leaves source storage and membership unchanged");
        if (status == MMS_OK) { reached = true; Check(SameReport(first, attempt.Result()), "complete retry has the same retained charge"); break; }
        Check(status == MMS_E_MEMORY, "bookkeeping collector allocation failures report memory refusal"); ++failures;
    }
    Check(reached && failures >= 5, "bookkeeping allocation sweep reaches all distinct real owner allocations");
}

void ParkedPoolExchangeAndGrowth() {
    AccessoryFixture f;
    rb::Collector original; Check(f.owner->MeasureBookkeeping(original) == MMS_OK, "measure parked pool before exchange");
    const auto first = original.Result(); auto* originalActive = f.app->AccessoryKeys(3);
    auto remove = f.owner->PreparePresence(false);
    Check(remove && remove->IsCurrent(), "prepare real accessory membership exchange for bookkeeping validation");
    failAfter = 0; remove->Publish(); failAfter = -1; remove.reset();
    failAfter = 0; const bool exchanged = f.owner->CoversBookkeeping(original); failAfter = -1;
    Check(!exchanged && !f.owner->Present() && f.app->AccessorySlot(3) == nullptr && SameReport(first, original.Result()),
        "swapping a newly parked native pool invalidates the previous retained ledger without allocation");
    const auto oldCapacity = mdl::KeyPoolCapacity(f.app->AccessoryKeys(3));
    Check(mdl::KeyPoolEnsure(f.app->AccessoryKeys(3), oldCapacity + 1), "grow the actual blank destination pool while its object is absent");
    const auto grownCapacity = mdl::KeyPoolCapacity(f.app->AccessoryKeys(3));
    auto restore = f.owner->PreparePresence(true);
    Check(restore && restore->IsCurrent(), "prepare original accessory and grown parked destination restoration");
    failAfter = 0; restore->Publish(); failAfter = -1; restore.reset();
    failAfter = 0; const bool grownCovered = f.owner->CoversBookkeeping(original); failAfter = -1;
    Check(!grownCovered && f.owner->Present() && f.app->AccessoryKeys(3) == originalActive && SameReport(first, original.Result()),
        "a grown replacement parked pool cannot pass a previous allocation-identity ledger");
    rb::Collector updated;
    Check(f.owner->MeasureBookkeeping(updated) == MMS_OK && updated.Result().Complete() &&
        updated.Result().cpuBytes >= first.cpuBytes + (grownCapacity - oldCapacity) * sizeof(mdl::AccessoryKey),
        "retained bookkeeping remeasures the actual grown parked-pool capacity");
    failAfter = 0; const bool renewed = f.owner->CoversBookkeeping(updated); failAfter = -1;
    Check(renewed, "a freshly measured grown owner is again verifiable without allocation");
}

void CurveAndPhysicsContainers() {
    AccessoryFixture f;
    rb::Collector empty; Check(f.owner->MeasureBookkeeping(empty) == MMS_OK, "measure owner before authored curves exist");
    std::vector<curve_editor::Channel> channels; std::wstring error;
    Check(curve_host::Read(*f.app, curve_host::Kind::Accessory, 3, channels, error) && !channels.empty(),
        "read actual accessory authoring channels before retaining changed containers");
    auto key = channels[0].track.keys[0]; key.frame = 37; key.value = 9;
    key.interpolation = animation_curve::Interpolation::Bezier; key.inSlope = .375; key.inWeight = .625;
    channels[0].track.keys.push_back(key);
    Check(curve_host::Write(*f.app, curve_host::Kind::Accessory, 3, channels, error), "write authored accessory data into real host storage");
    failAfter = 0; const bool prior = f.owner->CoversBookkeeping(empty); failAfter = -1;
    rb::Collector authored;
    Check(!prior && f.owner->MeasureBookkeeping(authored) == MMS_OK && authored.Result().Complete() &&
        authored.Result().cpuBytes > empty.Result().cpuBytes,
        "new authoring nodes and vector capacities cannot escape an earlier owner charge");
    failAfter = 0; const bool curvesCovered = f.owner->CoversBookkeeping(authored); failAfter = -1;
    Check(curvesCovered, "actual retained authoring containers validate without allocation");

    Fixture model;
    auto app = std::make_unique<MMDApp>(); ::new (static_cast<void*>(&app->state)) MMDAppState{};
    btDefaultCollisionConfiguration config; btCollisionDispatcher dispatcher(&config);
    history_physics::Broadphase broadphase(btVector3(-100,-100,-100), btVector3(100,100,100), 64);
    btSequentialImpulseConstraintSolver solver; DynamicsWorld world(&dispatcher, &broadphase, &solver, &config);
    PhysicsScene scene{}; scene.world = &world;
    btSphereShape shape(btScalar(.5)); btDefaultMotionState motion(btTransform::getIdentity());
    btRigidBody body(1, &motion, &shape); world.addRigidBody(&body, 1, -1);
    mdl::RigidRecord rigid{}; rigid.body = &body;
    model.m->scenePtr = &scene; model.m->rigidTable = &rigid; model.m->rigidCount = 1; app->ModelSlot(0) = model.raw();
    auto owner = plugins::RetainSceneObject(*app, MMS_OBJECT_MODEL, 0);
    rb::Collector directPhysics;
    // This separately allocated owner is a lower bound for the same known
    // wrapper/Impl/bodies/constraints retained by the production scene owner.
    history_physics::Retained comparison(world, {&body}, {}); comparison.MeasureResources(directPhysics);
    rb::Collector physical;
    Check(owner->MeasureBookkeeping(physical) == MMS_OK && physical.Result().Complete() && physical.Result().d3d9Bytes == 0 &&
        physical.Result().cpuBytes > directPhysics.Result().cpuBytes,
        "retained model bookkeeping includes real physics keepalive containers without charging body or mesh resources");
    failAfter = 0; const bool physicsCovered = owner->CoversBookkeeping(physical); failAfter = -1;
    Check(physicsCovered, "real physics lifetime bookkeeping validates without allocation");
    auto remove = owner->PreparePresence(false); Check(remove && remove->IsCurrent(), "prepare physical model parking");
    failAfter = 0; remove->Publish(); failAfter = -1; remove.reset();
    failAfter = 0; const bool previous = owner->CoversBookkeeping(physical); failAfter = -1;
    rb::Collector parked;
    Check(!previous && owner->MeasureBookkeeping(parked) == MMS_OK && parked.Result().Complete() &&
        parked.Result().cpuBytes > physical.Result().cpuBytes,
        "parking adds measured Saved and world-order containers and invalidates a smaller prior ledger");
    auto restore = owner->PreparePresence(true); Check(restore && restore->IsCurrent(), "prepare physical fixture restoration");
    failAfter = 0; restore->Publish(); failAfter = -1; restore.reset();
    owner.reset(); world.removeRigidBody(&body);
    app->ModelSlot(0) = nullptr; plugins::ForgetSceneObject(model.raw());
    model.m->scenePtr = nullptr; model.m->rigidTable = nullptr; model.m->rigidCount = 0;
}

void BudgetAndPreparedFailure() {
    using namespace plugins;
    AccessoryFixture f;
    StorageStore storage; bool published = false;
    auto change = std::make_shared<BudgetProbe>(f.owner, published);
    SharedHistory reducedBudget(storage, {}, 16, 128 * 1024);
    std::unique_ptr<SharedHistory::NativeTransaction> tx;
    Check(reducedBudget.PrepareNative("retained owner budget", change, tx) == MMS_E_MEMORY && !tx && !published &&
        !reducedBudget.UndoCount() && !reducedBudget.RedoCount() && !reducedBudget.RetainedBytes() && f.owner->Present(),
        "actual parked owner storage refuses an insufficient journal budget before publication");
    SharedHistory journal(storage, {}, 16, 4 * 1024 * 1024);
    Check(journal.PrepareNative("retained owner budget", change, tx) == MMS_OK && tx && !published && !journal.UndoCount(),
        "retained bookkeeping admits a sufficiently budgeted real journal candidate");
    tx.reset();
    Check(!published && !journal.UndoCount() && !journal.RetainedBytes(), "cancelling an owner-budget candidate publishes nothing");
    Check(journal.PrepareNative("stale owner budget", change, tx) == MMS_OK, "prepare owner ledger before authoring storage grows");
    std::vector<curve_editor::Channel> channels; std::wstring error;
    Check(curve_host::Read(*f.app, curve_host::Kind::Accessory, 3, channels, error) && !channels.empty(),
        "read real accessory channels after budget preparation");
    Check(curve_host::Write(*f.app, curve_host::Kind::Accessory, 3, channels, error),
        "create a real authoring allocation after budget preparation without changing native values");
    failAfter = 0; const auto stale = journal.CommitNative(std::move(tx)); failAfter = -1;
    Check(stale == MMS_E_STALE && !published && !journal.UndoCount() && !journal.RedoCount() && !journal.RetainedBytes(),
        "new retained storage invalidates a prepared publication with no journal or payload changes");
    Check(journal.PrepareNative("remeasured owner budget", change, tx) == MMS_OK, "remeasure the owner before retry");
    failAfter = 0; const auto committed = journal.CommitNative(std::move(tx)); failAfter = -1;
    Check(committed == MMS_OK && published && journal.UndoCount() == 1 && journal.RetainedBytes() >=
        mdl::KeyPoolStorage<mdl::AccessoryKey>::initial * sizeof(mdl::AccessoryKey),
        "journal publication is allocation-free and retains the complete measured owner payload charge");
    const auto charged = journal.RetainedBytes();
    failAfter = 0; const auto refusedUndo = journal.History(false); failAfter = -1;
    Check(refusedUndo == MMS_E_MEMORY && published && journal.UndoCount() == 1 && !journal.RedoCount() &&
        journal.RetainedBytes() == charged && f.owner->Present(),
        "owner-budget history preparation failure preserves the existing node charge and published payload");
    Check(journal.History(false) == MMS_OK && !published && !journal.UndoCount() && journal.RedoCount() == 1 &&
        journal.History(true) == MMS_OK && published && journal.UndoCount() == 1,
        "both history directions reuse the retained owner budget contract");
    Check(journal.Reset() == MMS_OK, "reset bookkeeping budget journal before fixture destruction"); journal.Collect();
}
} // namespace retained_bookkeeping_checks
void RetainedBookkeepingChecks() {
    retained_bookkeeping_checks::AccessoryAllocations();
    retained_bookkeeping_checks::ParkedPoolExchangeAndGrowth();
    retained_bookkeeping_checks::CurveAndPhysicsContainers();
    retained_bookkeeping_checks::BudgetAndPreparedFailure();
}
} // namespace
