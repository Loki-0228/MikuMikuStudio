namespace {
void SceneObjectBudgetChecks() {
    using namespace plugins;
    Fixture f;
    auto app = std::make_unique<MMDApp>(); ::new (static_cast<void*>(&app->state)) MMDAppState{};
    app->ModelSlot(0) = f.raw();
    auto object = RetainSceneObject(*app, MMS_OBJECT_MODEL, 0);
    auto removal = object->CaptureRemoval();
    StorageStore storage; SharedHistory journal(storage);
    std::unique_ptr<SharedHistory::NativeTransaction> tx;
    wchar_t unknown[] = L"unknown capacity"; f.m->pmxTextBuffers[0] = unknown;
    Check(journal.PrepareNative("measured model", removal, tx) == MMS_E_NOT_READY && !tx &&
        app->ModelSlot(0) == f.raw() && !journal.UndoCount(), "incomplete model budget refuses production deletion");
    f.m->pmxTextBuffers[0] = nullptr;
    const auto digest = f.Digest();
    bool prepared = false; unsigned failures = 0;
    for (long limit = 0; limit < 512; ++limit) {
        failAfter = limit; const auto result = journal.PrepareNative("measured model", removal, tx); failAfter = -1;
        Check(f.Digest() == digest && app->ModelSlot(0) == f.raw() && !journal.RetainedBytes(),
            "measured deletion preparation never changes model membership or budget");
        if (result == MMS_OK) { prepared = true; tx.reset(); break; }
        Check(result == MMS_E_MEMORY && !tx, "measured deletion reports each allocation failure"); ++failures;
    }
    Check(prepared && failures > 10, "measured deletion allocation sweep reaches all resource and history stages");
    Check(journal.PrepareNative("stale measured model", removal, tx) == MMS_OK, "prepare model before a resource grows");
    Check(mdl::BoneKeyPoolEnsure(*f.m, mdl::BoneKeyCapacity(*f.m) + 1), "grow a measured resource after prepare");
    Check(journal.CommitNative(std::move(tx)) == MMS_E_STALE && !journal.UndoCount() && !journal.RetainedBytes() &&
        app->ModelSlot(0) == f.raw(), "resource identity and capacity change invalidates a prepared deletion");
    Check(journal.PrepareNative("measured model", removal, tx) == MMS_OK, "prepare complete model budget");
    failAfter = 0; const auto committed = journal.CommitNative(std::move(tx)); failAfter = -1;
    Check(committed == MMS_OK && !app->ModelSlot(0), "measured model deletion publishes without allocation");
    const auto initialCharge = journal.RetainedBytes();
    Check(initialCharge > sizeof(mdl::ModelRecord), "journal uses resource charge instead of record-only placeholder");
    Check(journal.History(false) == MMS_OK && app->ModelSlot(0) == f.raw(), "measured model undo restores membership");
    Check(mdl::BoneKeyPoolEnsure(*f.m, mdl::BoneKeyCapacity(*f.m) + 1), "grow model pool before another deletion");
    Check(journal.History(true) == MMS_OK && journal.RetainedBytes() > initialCharge,
        "redo remeasures grown model resources");
    Check(journal.History(false) == MMS_OK, "restore measured fixture before owner release");
    Check(journal.Reset() == MMS_OK, "reset measured fixture history"); journal.Collect();
    removal.reset(); object.reset(); app->ModelSlot(0) = nullptr; ForgetSceneObject(f.raw());

    // A real Bullet target snapshot grows with the world even though the
    // retained object's own bodies and model storage remain unchanged.
    btDefaultCollisionConfiguration config; btCollisionDispatcher dispatcher(&config);
    history_physics::Broadphase broadphase(btVector3(-100,-100,-100), btVector3(100,100,100), 64);
    btSequentialImpulseConstraintSolver solver; DynamicsWorld world(&dispatcher, &broadphase, &solver, &config);
    PhysicsScene scene{}; scene.world = &world;
    btSphereShape shape(btScalar(.5)); btDefaultMotionState motion(btTransform::getIdentity());
    btRigidBody body(1, &motion, &shape); world.addRigidBody(&body, 1, -1);
    mdl::RigidRecord rigid{}; rigid.body = &body;
    f.m->scenePtr = &scene; f.m->rigidTable = &rigid; f.m->rigidCount = 1;
    app->ModelSlot(0) = f.raw(); object = RetainSceneObject(*app, MMS_OBJECT_MODEL, 0); removal = object->CaptureRemoval();
    Check(journal.PrepareNative("physical model", removal, tx) == MMS_OK &&
        journal.CommitNative(std::move(tx)) == MMS_OK, "measured Bullet deletion includes private body storage");
    const auto smallWorld = journal.RetainedBytes();
    Check(journal.History(false) == MMS_OK, "restore Bullet model for world growth");
    std::vector<std::unique_ptr<btRigidBody>> additional;
    for (int i = 0; i < 7; ++i) {
        additional.push_back(std::make_unique<btRigidBody>(0, nullptr, &shape)); world.addRigidBody(additional.back().get(), 1, -1);
    }
    Check(journal.History(true) == MMS_OK && journal.RetainedBytes() > smallWorld,
        "redo charges newly prepared world-order snapshot after world growth");
    Check(journal.History(false) == MMS_OK, "restore physical model before cleanup");
    Check(journal.Reset() == MMS_OK, "reset physical budget history"); journal.Collect();
    removal.reset(); object.reset();
    for (auto& other : additional) world.removeRigidBody(other.get());
    world.removeRigidBody(&body);
    f.m->scenePtr = nullptr; f.m->rigidTable = nullptr; f.m->rigidCount = 0;
    app->ModelSlot(0) = nullptr; ForgetSceneObject(f.raw());
}
}
