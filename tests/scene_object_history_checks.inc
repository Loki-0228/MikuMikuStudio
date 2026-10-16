namespace {
struct CountedShape : btSphereShape {
    int& destroyed;
    explicit CountedShape(int& count) : btSphereShape(btScalar(.5)), destroyed(count) {}
    ~CountedShape() override { ++destroyed; }
};
void ObjectRetention() {
    {
        auto app = std::make_unique<MMDApp>(); app->state = MMDAppState{};
        btDefaultCollisionConfiguration config;
        btCollisionDispatcher dispatcher(&config);
        history_physics::Broadphase broadphase(btVector3(-100,-100,-100), btVector3(100,100,100), 64);
        btSequentialImpulseConstraintSolver solver;
        DynamicsWorld world(&dispatcher, &broadphase, &solver, &config);
        PhysicsScene scene{}; scene.world = &world;
        int destroyed = 0;
        auto* model = static_cast<mdl::ModelRecord*>(std::calloc(1, sizeof(mdl::ModelRecord)));
        model->scenePtr = &scene; model->rigidCount = 2; model->jointCount = 1;
        model->rigidTable = static_cast<mdl::RigidRecord*>(std::calloc(2, sizeof(mdl::RigidRecord)));
        model->jointTable = static_cast<mdl::JointRecord*>(std::calloc(1, sizeof(mdl::JointRecord)));
        model->boneKeys = mdl::BoneKeyPoolCreate(8); model->boneKeys[3].frame = 23;
        model->morphKeys = mdl::MorphKeyPoolCreate(8); model->morphKeys[2].value = .75f;
        std::wcscpy(model->path, L"missing-source-do-not-reload.pmx");
        auto* raw = reinterpret_cast<unsigned char*>(model); app->ModelSlot(4) = raw;
        for (int i = 0; i < 2; ++i) {
            auto* shape = new CountedShape(destroyed); btVector3 inertia; shape->calculateLocalInertia(1, inertia);
            auto* body = new btRigidBody(1, new btDefaultMotionState(btTransform::getIdentity()), shape, inertia);
            model->rigidTable[i].body = body; world.addRigidBody(body, 1, -1);
        }
        auto* joint = new btGeneric6DofConstraint(*static_cast<btRigidBody*>(model->rigidTable[0].body),
            *static_cast<btRigidBody*>(model->rigidTable[1].body), btTransform::getIdentity(), btTransform::getIdentity(), true);
        joint->setUserConstraintId(42); world.addConstraint(joint, false); model->jointTable[0].constraint = 42;
        const auto identity = plugins::SceneObjectIdentity(raw);
        auto object = plugins::RetainSceneObject(*app, MMS_OBJECT_MODEL, 4);
        Check(plugins::RetainSceneObject(*app, MMS_OBJECT_MODEL, 4) == object, "same native model has one retained owner");
        app->CameraParentModel() = 4; app->CameraParentBone() = 6;
        plugins::StorageStore storage; plugins::SharedHistory journal(storage);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        std::unique_ptr<plugins::PreparedHistoryChange> staged;
        std::shared_ptr<plugins::HistoryChange> insertion;
        Check(object->PrepareInsertion(1024 * 1024, insertion, staged) == MMS_OK && staged->IsCurrent(), "prepare newly loaded model staging");
        staged->Publish(); staged.reset();
        Check(journal.PrepareNative("import model", std::move(insertion), tx) == MMS_OK &&
            journal.CommitNative(std::move(tx)) == MMS_OK && app->ModelSlot(4) == raw, "publish staged model import into common history");
        Check(journal.PrepareNative("delete model", object->CaptureRemoval(1024 * 1024), tx) == MMS_OK, "prepare real model removal");
        failAfter = 0; const auto result = journal.CommitNative(std::move(tx)); failAfter = -1;
        Check(result == MMS_OK && !app->ModelSlot(4) && !object->Present() && !plugins::SceneObjectPresent(raw, identity.id), "model membership and identity publish without allocation");
        Check(world.getNumCollisionObjects() == 0 && world.getNumConstraints() == 0 && destroyed == 0 && app->CameraParentModel() == -1, "model retained while physics and incoming binding removed");
        Check(journal.History(false) == MMS_OK && app->ModelSlot(4) == raw && world.getNumConstraints() == 1, "common undo restores real model and physics");
        auto restored = plugins::SceneObjectIdentity(raw);
        Check(restored.id.lo == identity.id.lo && restored.id.hi == identity.id.hi && restored.generation != identity.generation, "restored model stable ID and fresh generation");
        Check(model->boneKeys[3].frame == 23 && model->morphKeys[2].value == .75f && app->CameraParentModel() == 4 && app->CameraParentBone() == 6, "model keys and incoming binding restored without source file");
        Check(journal.History(false) == MMS_OK && !app->ModelSlot(4) && destroyed == 0 && world.getNumCollisionObjects() == 0, "undo earlier import retains model and removes physics");
        Check(journal.History(true) == MMS_OK && app->ModelSlot(4) == raw && model->boneKeys[3].frame == 23 && world.getNumCollisionObjects() == 2,
              "redo import restores original data without reading missing source");
        Check(journal.History(true) == MMS_OK && !app->ModelSlot(4), "common redo deletes real model again");
        object.reset(); Check(destroyed == 0, "history owns absent model");
        Check(journal.Reset() == MMS_OK && destroyed == 0, "history reset defers resource disposal");
        journal.Collect();
        Check(destroyed == 2 && broadphase.getNumHandles() == 0 && world.getNumCollisionObjects() == 0, "idle collection releases parked model bodies and handles");
    }
    {
        auto app = std::make_unique<MMDApp>(); app->state = MMDAppState{}; app->EnglishUI() = 2;
        auto* accessory = new mdl::AccessoryRecord{}; accessory->scale = accessory->opacity = 1; accessory->parentModel = -1;
        app->AccessorySlot(3) = accessory; app->AccessoryKeys(3) = mdl::KeyPoolCreate<mdl::AccessoryKey>(8);
        app->AccessoryKeys(3)[0].scale = app->AccessoryKeys(3)[0].opacity = 1; app->AccessoryKeys(3)[0].parentModel = -1;
        std::vector<curve_editor::Channel> channels; std::wstring error;
        Check(curve_host::Read(*app, curve_host::Kind::Accessory, 3, channels, error), "read retained accessory curves");
        auto key = channels[0].track.keys[0]; key.frame = 20; key.value = 7;
        key.interpolation = animation_curve::Interpolation::Bezier; key.inSlope = .25; key.inWeight = .4;
        channels[0].track.keys.push_back(key);
        Check(curve_host::Write(*app, curve_host::Kind::Accessory, 3, channels, error), "write accessory custom tangent before deletion");
        app->AccessoryTrackCursor(3) = 1; app->AccessoryTrackActive(3) = 1;
        app->AccessoryRenderSplitOrder() = 1;
        auto* keys = app->AccessoryKeys(3);
        const auto identity = plugins::SceneObjectIdentity(accessory);
        auto object = plugins::RetainSceneObject(*app, MMS_OBJECT_ACCESSORY, 3);
        plugins::StorageStore storage; plugins::SharedHistory journal(storage);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(journal.PrepareNative("delete accessory", object->CaptureRemoval(1024 * 1024), tx) == MMS_OK, "prepare real accessory removal");
        failAfter = 0; const auto result = journal.CommitNative(std::move(tx)); failAfter = -1;
        Check(result == MMS_OK && !app->AccessorySlot(3) && app->AccessoryKeys(3) != keys && app->AccessoryKeys(3)[0].scale == 1,
              "accessory removal publishes independent empty track without allocation");
        Check(!curve_host::Has(app.get(), curve_host::Kind::Accessory, 3) && app->AccessoryTrackCursor(3) == 0 && app->AccessoryTrackActive(3) == 0 && app->AccessoryRenderSplitOrder() == 0,
              "accessory authoring cursor and render split leave active scene");
        // A later import can use the old slot and its replacement key pool.
        auto* replacement = new mdl::AccessoryRecord{}; app->AccessorySlot(3) = replacement;
        Check(journal.History(false) != MMS_OK && journal.UndoCount() == 1 && app->AccessorySlot(3) == replacement,
              "occupied accessory slot blocks undo without overwriting newer object");
        app->AccessorySlot(3) = nullptr; delete replacement;
        Check(journal.History(false) == MMS_OK && app->AccessorySlot(3) == accessory && app->AccessoryKeys(3) == keys &&
            app->AccessoryTrackCursor(3) == 1 && app->AccessoryTrackActive(3) == 1 && app->AccessoryRenderSplitOrder() == 1, "accessory undo restores original pool cursor and split");
        Check(curve_host::Read(*app, curve_host::Kind::Accessory, 3, channels, error) &&
            channels[0].track.keys.back().inSlope == .25 && channels[0].track.keys.back().inWeight == .4, "accessory undo retains authored tangent");
        const auto restored = plugins::SceneObjectIdentity(accessory);
        Check(restored.id.lo == identity.id.lo && restored.id.hi == identity.id.hi && restored.generation != identity.generation, "accessory identity remains stable across undo");
        Check(journal.History(true) == MMS_OK, "redo real accessory deletion");
        object.reset(); Check(journal.Reset() == MMS_OK, "reset accessory history"); journal.Collect();
        Check(!curve_host::Has(app.get(), curve_host::Kind::Accessory, 3) && !app->AccessorySlot(3), "collected accessory stays absent");
        mdl::KeyPoolDestroy(app->AccessoryKeys(3)); curve_host::Reset(app.get());
    }
    {
        // Import, delete, then reuse the same slot. Each object keeps its own
        // track and authored tangents through both directions of the journal.
        auto app = std::make_unique<MMDApp>(); app->state = MMDAppState{}; app->EnglishUI() = 2;
        app->AccessoryKeys(3) = mdl::KeyPoolCreate<mdl::AccessoryKey>(8);
        plugins::StorageStore storage; plugins::SharedHistory journal(storage);
        const auto import = [&](float value) {
            auto* accessory = new mdl::AccessoryRecord{};
            accessory->scale = accessory->opacity = 1; accessory->parentModel = -1;
            app->AccessorySlot(3) = accessory; app->AccessoryRenderSplitOrder() = 1;
            auto& head = app->AccessoryKeys(3)[0];
            head.scale = head.opacity = 1; head.visible = 1; head.parentModel = -1;
            std::vector<curve_editor::Channel> channels; std::wstring error;
            Check(curve_host::Read(*app, curve_host::Kind::Accessory, 3, channels, error), "read slot-reuse accessory authoring");
            auto key = channels[0].track.keys[0]; key.frame = 20; key.value = value;
            key.interpolation = animation_curve::Interpolation::Bezier; key.inSlope = value; key.inWeight = .4;
            channels[0].track.keys.push_back(key);
            Check(curve_host::Write(*app, curve_host::Kind::Accessory, 3, channels, error), "write distinct slot-reuse tangents");
            auto object = plugins::RetainSceneObject(*app, MMS_OBJECT_ACCESSORY, 3);
            std::shared_ptr<plugins::HistoryChange> change; std::unique_ptr<plugins::PreparedHistoryChange> stage;
            Check(object->PrepareInsertion(1024 * 1024, change, stage) == MMS_OK && stage->IsCurrent(), "stage same-slot import");
            stage->Publish(); stage.reset();
            std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
            Check(journal.PrepareNative("import accessory", std::move(change), tx) == MMS_OK && journal.CommitNative(std::move(tx)) == MMS_OK,
                  "record same-slot import");
            return object;
        };
        const auto remove = [&](const std::shared_ptr<plugins::RetainedSceneObject>& object) {
            std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
            Check(journal.PrepareNative("delete accessory", object->CaptureRemoval(1024 * 1024), tx) == MMS_OK &&
                  journal.CommitNative(std::move(tx)) == MMS_OK, "record same-slot deletion");
        };
        const auto restored = [&](mdl::AccessoryRecord* object, mdl::AccessoryKey* keys, double slope) {
            std::vector<curve_editor::Channel> channels; std::wstring error;
            return app->AccessorySlot(3) == object && app->AccessoryKeys(3) == keys && app->AccessoryRenderSplitOrder() == 1 &&
                curve_host::Read(*app, curve_host::Kind::Accessory, 3, channels, error) && channels[0].track.keys.back().inSlope == slope;
        };
        auto first = import(2); auto* a = app->AccessorySlot(3); auto* aKeys = app->AccessoryKeys(3);
        app->AccessorySlot(4) = a; bool rejectedAlias = false;
        try { plugins::RetainSceneObject(*app, MMS_OBJECT_ACCESSORY, 4); } catch (const std::invalid_argument&) { rejectedAlias = true; }
        app->AccessorySlot(4) = nullptr;
        Check(rejectedAlias, "same native pointer cannot acquire an owner for another slot");
        remove(first);
        Check(mdl::KeyPoolEnsure(app->AccessoryKeys(3), mdl::KeyPoolCapacity(app->AccessoryKeys(3)) + 1), "later import can grow vacant slot track");
        auto second = import(5); auto* b = app->AccessorySlot(3); auto* bKeys = app->AccessoryKeys(3);
        remove(second);
        for (int cycle = 0; cycle < 3; ++cycle) {
            Check(journal.History(false) == MMS_OK && restored(b, bKeys, 5), "undo later deletion restores new occupant and its curve");
            Check(journal.History(false) == MMS_OK && !app->AccessorySlot(3) && app->AccessoryRenderSplitOrder() == 0, "undo new occupant import empties slot");
            Check(journal.History(false) == MMS_OK && restored(a, aKeys, 2), "undo earlier deletion restores old occupant and its curve");
            Check(journal.History(false) == MMS_OK && !app->AccessorySlot(3), "undo old occupant import empties slot");
            Check(journal.History(true) == MMS_OK && restored(a, aKeys, 2), "redo old occupant import preserves exact track");
            Check(journal.History(true) == MMS_OK && !app->AccessorySlot(3), "redo earlier deletion empties slot");
            Check(journal.History(true) == MMS_OK && restored(b, bKeys, 5), "redo new occupant import preserves exact track");
            Check(journal.History(true) == MMS_OK && !app->AccessorySlot(3), "redo later deletion empties slot");
        }
        auto pending = first->PreparePresence(true); std::weak_ptr<plugins::RetainedSceneObject> weak = first;
        first.reset(); second.reset();
        Check(journal.Reset() == MMS_OK, "retire both slot occupants"); journal.Collect();
        Check(!weak.expired() && pending->IsCurrent(), "prepared presence owns absent object after history collection");
        pending.reset();
        Check(weak.expired() && !app->AccessorySlot(3), "canceling final candidate releases absent object");
        mdl::KeyPoolDestroy(app->AccessoryKeys(3)); curve_host::Reset(app.get());
    }
    {
        auto app = std::make_unique<MMDApp>(); app->state = MMDAppState{};
        auto* accessory = new mdl::AccessoryRecord{};
        accessory->scale = accessory->opacity = 1; accessory->parentModel = -1;
        app->AccessorySlot(3) = accessory; app->AccessoryKeys(3) = mdl::KeyPoolCreate<mdl::AccessoryKey>(8);
        app->AccessoryRenderSplitOrder() = 1;
        auto object = plugins::RetainSceneObject(*app, MMS_OBJECT_ACCESSORY, 3);
        auto* keys = app->AccessoryKeys(3); const auto identity = plugins::SceneObjectIdentity(accessory);
        unsigned failures = 0; bool prepared = false;
        for (long limit = 0; limit < 256; ++limit) {
            std::shared_ptr<plugins::HistoryChange> change; std::unique_ptr<plugins::PreparedHistoryChange> stage;
            failAfter = limit; const auto result = object->PrepareInsertion(1024 * 1024, change, stage); failAfter = -1;
            Check(app->AccessorySlot(3) == accessory && app->AccessoryKeys(3) == keys && app->AccessoryRenderSplitOrder() == 1 &&
                  object->Present() && plugins::SceneObjectIdentity(accessory).generation == identity.generation,
                  "failed or canceled import preparation preserves live source");
            if (result == MMS_OK) { prepared = true; Check(stage->IsCurrent() && failures > 2, "import staging survives allocation failure sweep"); break; }
            Check(result == MMS_E_MEMORY && !change && !stage, "import preparation failure publishes neither output"); ++failures;
        }
        Check(prepared, "import allocation sweep terminates");
        std::shared_ptr<plugins::HistoryChange> change; std::unique_ptr<plugins::PreparedHistoryChange> stage;
        Check(object->PrepareInsertion(1024 * 1024, change, stage) == MMS_OK && stage->IsCurrent(), "prepare over-budget import");
        // Staging is valid independently of the insertion record's lifetime.
        change.reset(); object.reset();
        Check(stage->IsCurrent(), "loader staging retains object and binding owners");
        object = plugins::RetainSceneObject(*app, MMS_OBJECT_ACCESSORY, 3);
        stage.reset();
        Check(object->PrepareInsertion(1024 * 1024, change, stage) == MMS_OK && stage->IsCurrent(), "reprepare canceled independent staging");
        stage->Publish(); stage.reset();
        plugins::StorageStore storage; plugins::SharedHistory journal(storage, {}, 128, 1);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(journal.PrepareNative("over-budget import", change, tx) == MMS_E_MEMORY && !tx && journal.UndoCount() == 0 &&
              !app->AccessorySlot(3) && app->AccessoryRenderSplitOrder() == 0, "rejected import reservation leaves object absent and history empty");
        std::weak_ptr<plugins::RetainedSceneObject> weak = object; object.reset(); change.reset();
        Check(weak.expired(), "rejected import releases retained resource owner");
        mdl::KeyPoolDestroy(app->AccessoryKeys(3)); curve_host::Reset(app.get());
    }
}
}
