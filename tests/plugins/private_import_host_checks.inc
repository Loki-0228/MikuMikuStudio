namespace private_import_host_checks {
void Run(MMDApp* app) {
    using namespace plugins;
    wchar_t modelPath[256]{}, accessoryPath[256]{}, importedPath[256]{}, vacPath[256]{};
    Check(GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL", modelPath, 256) &&
          GetEnvironmentVariableW(L"MMS_SCENE_TEST_ACCESSORY", accessoryPath, 256) &&
          GetEnvironmentVariableW(L"MMS_IMPORT_TEST_MODEL", importedPath, 256) &&
          GetEnvironmentVariableW(L"MMS_IMPORT_TEST_VAC", vacPath, 256), "private import host fixtures supplied");
    LoadModelFile(app, modelPath, false); LoadAccessoryFile(accessoryPath);
    auto* oldModel = app->ModelSlot(0); auto* oldAccessory = app->AccessorySlot(0);
    Check(oldModel && oldAccessory, "load existing scene before import transaction binding");
    import_buffer_failure_checks::Run(app, importedPath);
    auto* world = app->Physics()->world;
    std::vector<std::pair<btCollisionObject*, btBroadphaseProxy*>> bodies;
    for (int i = 0; i < world->getNumCollisionObjects(); ++i) {
        auto* body = world->getCollisionObjectArray()[i]; bodies.emplace_back(body, body->getBroadphaseHandle());
    }
    const auto oldJointCount = world->getNumConstraints();
    Shutdown(); Check(InitializeHistoryTestRuntime(app), "bind private importer to application shared journal");
    auto* history = NativeHistory(app);
    import_picker_host_checks::Run(app);
    wchar_t cwd[32768]{}; GetCurrentDirectoryW(32768, cwd);
    const auto paths = app->PathWorkspace();
    LoadModelFile(app, importedPath, false);
    auto* model = app->ModelSlot(1);
    Check(model && model != oldModel && history->UndoCount() == 1, "actual model load entry commits one private import");
    Check(mdl::Mdl(model)->scenePtr == app->Physics(), "published imported model is rebound to live physics scene");
    for (const auto& item : bodies) Check(item.first->getBroadphaseHandle() == item.second, "import preserves existing broadphase handle addresses");
    LoadAccessoryFile(vacPath);
    auto* accessory = app->AccessorySlot(1); auto* keys = app->AccessoryKeys(1);
    Check(accessory && accessory->mesh && history->UndoCount() == 2 && accessory->order == 1,
          "actual VAC load entry appends one import to the same journal");
    Check(accessory->scale == 2.5f && keys[0].scale == 2.5f && keys[0].position[1] == -2.5f,
          "real VAC mesh and initial track preserve placement");
    wchar_t after[32768]{}; GetCurrentDirectoryW(32768, after);
    Check(!wcscmp(cwd, after) && !std::memcmp(&paths, &app->PathWorkspace(), sizeof paths),
          "controlled load entries leave process cwd and path workspace unchanged");
    Check(SendDlgItemMessageW(app->MainWindow(), 436, CB_GETCOUNT, 0, 0) == 3 &&
          SendDlgItemMessageW(app->MainWindow(), 471, CB_GETCOUNT, 0, 0) == 2 && !NativeObjectUiPending(app),
          "both successful imports publish native control lists");
    const auto modelId = SceneObjectIdentity(model), accessoryId = SceneObjectIdentity(accessory);
    auto* vertexBuffer = mdl::Mdl(model)->vertexBuffer; auto* mesh = accessory->mesh;
    const auto totalBodies = world->getNumCollisionObjects(), totalJoints = world->getNumConstraints();
    const auto directory = std::filesystem::path(importedPath).parent_path();
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.is_regular_file()) std::filesystem::rename(entry.path(), entry.path().wstring() + L".offline");
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        edit_history_host_checks::Menu(app, L"Edit probe undo"); edit_history_host_checks::FlushObjectUi(app);
        Check(!app->AccessorySlot(1) && app->ModelSlot(1) == model, "plugin undo removes last imported VAC");
        SendMessageW(app->MainWindow(), WM_COMMAND, 400, 0);
        Check(!app->ModelSlot(1) && app->ModelSlot(0) == oldModel && app->AccessorySlot(0) == oldAccessory &&
              world->getNumCollisionObjects() == int(bodies.size()) && world->getNumConstraints() == oldJointCount,
              "native undo removes imported model and physics while preserving previous scene");
        edit_history_host_checks::Menu(app, L"Edit probe redo"); edit_history_host_checks::FlushObjectUi(app);
        Check(app->ModelSlot(1) == model && mdl::Mdl(model)->vertexBuffer == vertexBuffer &&
              SceneObjectPresent(model, modelId.id) && SceneObjectIdentity(model).generation != modelId.generation,
              "plugin redo restores same model buffers and stable ID with source files offline");
        SendMessageW(app->MainWindow(), WM_COMMAND, 401, 0);
        Check(app->AccessorySlot(1) == accessory && accessory->mesh == mesh && app->AccessoryKeys(1) == keys &&
              SceneObjectPresent(accessory, accessoryId.id) && world->getNumCollisionObjects() == totalBodies &&
              world->getNumConstraints() == totalJoints, "native redo restores original accessory resources and imported physics");
        RenderFrameSceneForOutput(app);
    }
    edit_history_host_checks::Menu(app, L"Edit probe undo"); edit_history_host_checks::FlushObjectUi(app);
    edit_history_host_checks::Menu(app, L"Edit probe undo"); edit_history_host_checks::FlushObjectUi(app);
    const auto revision = [&] { uint64_t value = 0; Check(history->Revision(value) == MMS_OK, "read import journal revision"); return value; }();
    const auto dirty = app->SceneModified();
    LoadModelFile(app, importedPath, false); LoadAccessoryFile(vacPath);
    uint64_t unchanged = 0;
    Check(history->Revision(unchanged) == MMS_OK && unchanged == revision && history->RedoCount() == 2 &&
          app->SceneModified() == dirty && !app->ModelSlot(1) && !app->AccessorySlot(1),
          "failed actual imports preserve redo branch revision dirty flag and slots");
    Check(history->Reset() == MMS_OK, "clear retained private imports"); history->Collect();
    Check(world->getNumCollisionObjects() == int(bodies.size()) && world->getNumConstraints() == oldJointCount,
          "evicting absent imports releases only their physics resources");
    for (const auto& item : bodies) Check(item.first->getBroadphaseHandle() == item.second, "old physics handles survive import history eviction");
    std::puts("PASS private PMX/PMD/VAC import commands, offline native/plugin history and cancellation");
}
}
