namespace object_history_checks {
void ProjectBoundary(MMDApp* app, const wchar_t* modelPath, const wchar_t* accessoryPath) {
    using namespace plugins;
    Initialize(app);
    auto* journal = NativeHistory(app);
    Check(journal != nullptr, "application owns a native history journal");
    // Earlier component probes intentionally bypassed UI publication. The
    // legacy PMM writer reads combo counts, so start this host-boundary probe
    // from a complete native reset before creating its project fixture.
    ResetAppState(app);
    LoadModelFile(app,modelPath,false); LoadAccessoryFile(accessoryPath);
    Check(SendDlgItemMessageW(app->MainWindow(),panel::kMainComboModel,CB_GETCOUNT,0,0)==2 &&
          SendDlgItemMessageW(app->MainWindow(),panel::kAccessoryCombo,CB_GETCOUNT,0,0)==1,
          "project fixture controls match native object counts");
    const auto project = std::filesystem::absolute(L"history-boundary.pmm");
    Check(SaveSceneCopy(app,project.c_str()), "save native resources before history removal");
    for (bool reopen : {true, false}) {
        RetainedSceneObject::Objects owners{
            RetainSceneObject(*app,MMS_OBJECT_MODEL,0), RetainSceneObject(*app,MMS_OBJECT_ACCESSORY,0)};
        const std::weak_ptr<RetainedSceneObject> modelOwner = owners[0], accessoryOwner = owners[1];
        std::unique_ptr<SharedHistory::NativeTransaction> tx;
        Check(journal->PrepareNative("project boundary removal",
            RetainedSceneObject::CaptureRemovalBatch(owners,128ull*1024*1024),tx)==MMS_OK &&
            journal->CommitNative(std::move(tx))==MMS_OK, "application journal retains removed native resources");
        owners.clear();
        Check(!modelOwner.expired() && !accessoryOwner.expired(), "history is the final retained object owner");
        Check(PrepareProjectLoad(app,project.c_str()), "prepare project serializers with existing history");
        Check(journal->UndoCount()==1 && !modelOwner.expired() && !accessoryOwner.expired(),
              "project preparation does not release old history");
        CancelProjectLoad(app);
        Check(journal->UndoCount()==1 && !modelOwner.expired() && !accessoryOwner.expired(),
              "cancelled project load preserves old history and resources");
        if (reopen) {
            wcscpy_s(app->state.envFileName,project.c_str()); LoadSceneFile();
            Check(app->ModelSlot(0) && app->AccessorySlot(0), "actual PMM loader replaces the scene");
        } else ResetAppState(app);
        Check(modelOwner.expired() && accessoryOwner.expired() && !journal->UndoCount() && !journal->RedoCount(),
              "project boundary reclaims absent objects before native scene reset");
        Check(journal->History(false)==MMS_E_NOT_FOUND, "project boundary releases the journal lock");
    }
    Check(app->Physics()->world->getNumCollisionObjects()==1 && app->Physics()->world->getNumConstraints()==0,
          "new scene leaves no retained bodies or constraints");
    LoadModelFile(app,modelPath,false);
    auto holder = RetainSceneObject(*app,MMS_OBJECT_MODEL,0);
    const std::weak_ptr<RetainedSceneObject> shutdownOwner = holder;
    std::unique_ptr<SharedHistory::NativeTransaction> pending;
    Check(journal->PrepareNative("pending at shutdown",holder->CaptureRemoval(64ull*1024*1024),pending)==MMS_OK,
          "prepare native candidate before closing");
    holder.reset();
    Check(PrepareProjectLoad(app,project.c_str()), "prepare serializers before closing");
    Shutdown();
    Check(NativeHistory(app)==nullptr && shutdownOwner.expired(),
          "shutdown cancels prepared project and releases native candidates while world lives");
    pending.reset();
    ResetAppState(app);
    std::puts("PASS application history project boundaries");
}
void RunBatch(MMDApp* app, const wchar_t* modelPath, const wchar_t* accessoryPath) {
    using namespace plugins;
    LoadModelFile(app,modelPath,false);LoadModelFile(app,modelPath,false);
    LoadAccessoryFile(accessoryPath);LoadAccessoryFile(accessoryPath);
    auto* model0=app->ModelSlot(0);auto* model1=app->ModelSlot(1);
    auto* accessory0=app->AccessorySlot(0);auto* accessory1=app->AccessorySlot(1);
    Check(model0 && model1 && accessory0 && accessory1,"load two models and two accessories for atomic batch");
    auto* world=app->Physics()->world;
    std::vector<btCollisionObject*> originalBodies;
    for(int i=0;i<world->getNumCollisionObjects();++i)originalBodies.push_back(world->getCollisionObjectArray()[i]);
    const auto originalJoints=world->getNumConstraints();
    accessory0->parentModel=1;accessory0->parentBone=0;app->AccessoryKeys(0)[0].parentModel=1;
    accessory1->parentModel=0;accessory1->parentBone=0;app->AccessoryKeys(1)[0].parentModel=0;
    RetainedSceneObject::Objects owners{
        RetainSceneObject(*app,MMS_OBJECT_ACCESSORY,1),RetainSceneObject(*app,MMS_OBJECT_MODEL,0),
        RetainSceneObject(*app,MMS_OBJECT_ACCESSORY,0),RetainSceneObject(*app,MMS_OBJECT_MODEL,1)};
    StorageStore storage;SharedHistory journal(storage,{},128,1024ull*1024*1024);
    std::shared_ptr<HistoryChange> insertion;std::unique_ptr<PreparedHistoryChange> staging;
    Check(RetainedSceneObject::PrepareInsertionBatch(owners,256ull*1024*1024,insertion,staging)==MMS_OK && staging->IsCurrent(),"prepare complete loader batch");
    staging->Publish();staging.reset();
    std::unique_ptr<SharedHistory::NativeTransaction> tx;
    Check(journal.PrepareNative("import four objects",insertion,tx)==MMS_OK && journal.CommitNative(std::move(tx))==MMS_OK,"one journal node imports all four resources");
    auto removal=RetainedSceneObject::CaptureRemovalBatch(owners,256ull*1024*1024);
    Check(journal.PrepareNative("delete four objects",removal,tx)==MMS_OK && journal.CommitNative(std::move(tx))==MMS_OK,"one journal node deletes all four resources");
    const auto absent=[&] { return !app->ModelSlot(0) && !app->ModelSlot(1) && !app->AccessorySlot(0) && !app->AccessorySlot(1) && world->getNumCollisionObjects()==1 && world->getNumConstraints()==0; };
    Check(absent(),"batch removes every object and leaves only ground");
    const auto modelOffline=std::filesystem::path(modelPath).wstring()+L".batch-offline";
    const auto accessoryOffline=std::filesystem::path(accessoryPath).wstring()+L".batch-offline";
    std::filesystem::rename(modelPath,modelOffline);std::filesystem::rename(accessoryPath,accessoryOffline);
    app->CameraMode()=1;Check(PostDeviceReset(app),"device reset with entire batch retained");
    for(int cycle=0;cycle<3;++cycle) {
        Check(journal.History(false)==MMS_OK,"single undo restores all four deleted objects");
        Check(app->ModelSlot(0)==model0 && app->ModelSlot(1)==model1 && app->AccessorySlot(0)==accessory0 && app->AccessorySlot(1)==accessory1,"batch undo reuses original resource owners");
        Check(world->getNumCollisionObjects()==int(originalBodies.size()) && world->getNumConstraints()==originalJoints,"batch restores live bodies and joints");
        for(size_t i=0;i<originalBodies.size();++i)Check(world->getCollisionObjectArray()[int(i)]==originalBodies[i],"batch retains physical ordering");
        Check(accessory0->parentModel==1 && accessory1->parentModel==0 && app->AccessoryKeys(0)[0].parentModel==1 && app->AccessoryKeys(1)[0].parentModel==0,"bindings internal to removed batch survive");
        RenderFrameSceneForOutput(app);
        Check(journal.History(false)==MMS_OK && absent(),"single undo reverses complete import");
        Check(journal.History(true)==MMS_OK && app->ModelSlot(0)==model0 && app->ModelSlot(1)==model1 && app->AccessorySlot(0)==accessory0 && app->AccessorySlot(1)==accessory1,"single redo repeats complete import offline");
        Check(journal.History(true)==MMS_OK && absent(),"single redo repeats complete deletion");
    }
    insertion.reset();removal.reset();owners.clear();Check(journal.Reset()==MMS_OK,"reset batch resource journal");journal.Collect();
    std::filesystem::rename(modelOffline,modelPath);std::filesystem::rename(accessoryOffline,accessoryPath);
    app->SceneModified()=0;
    std::puts("PASS production four-object atomic resource history");
}
void Run(MMDApp* app) {
    using namespace plugins;
    wchar_t modelPath[32768]{}, accessoryPath[32768]{};
    Check(GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL",modelPath,32768)>0 &&
          GetEnvironmentVariableW(L"MMS_SCENE_TEST_ACCESSORY",accessoryPath,32768)>0,"original resource fixtures supplied");
    const auto root=std::filesystem::current_path();
    Check(std::filesystem::equivalent(std::filesystem::path(modelPath).parent_path(),root) &&
          std::filesystem::equivalent(std::filesystem::path(accessoryPath).parent_path(),root),"source-file test stays inside isolated fixture directory");
    LoadModelFile(app,modelPath,false);
    auto* raw=app->ModelSlot(0);
    Check(raw!=nullptr,"load production PMX/PMD resource");
    auto& model=*mdl::Mdl(raw);
    auto* vertexBuffer=static_cast<IDirect3DVertexBuffer9*>(model.vertexBuffer);
    D3DVERTEXBUFFER_DESC descriptor{};
    Check(vertexBuffer && SUCCEEDED(vertexBuffer->GetDesc(&descriptor)) && descriptor.Pool==D3DPOOL_MANAGED,
          "retain real managed model vertex buffer");
    auto* world=app->Physics()->world;
    const int bodyCount=world->getNumCollisionObjects(), jointCount=world->getNumConstraints();
    auto modelOwner=RetainSceneObject(*app,MMS_OBJECT_MODEL,0);
    resource_budget::Report modelBudget{};
    Check(modelOwner->MeasureResources(modelBudget)==MMS_OK && modelBudget.cpuBytes>=sizeof(mdl::ModelRecord) &&
          modelBudget.d3d9Bytes>=descriptor.Size && modelBudget.totalBytes==modelBudget.cpuBytes+modelBudget.d3d9Bytes,
          "measure live native model allocations and real DX9 buffer sizes");
    Check(!(modelBudget.missing & resource_budget::PhysicsStorage),
          "supported Bullet private storage capacity is included in measurement");
    Check(!(modelBudget.missing & resource_budget::RawTextCapacity),
          "production PMX and PMD loaders account for every retained text allocation");
    resource_budget::Collector buffers;
    buffers.VertexBuffer(vertexBuffer); const auto bufferBytes=buffers.Result().d3d9Bytes;
    buffers.VertexBuffer(vertexBuffer);
    Check(bufferBytes>=descriptor.Size && buffers.Result().d3d9Bytes==bufferBytes,
          "shared DX9 COM identity is charged once");
    StorageStore storage;SharedHistory journal(storage,{},128,512ull*1024*1024);
    // Fixture resource charges are conservative. The public host still needs
    // its complete live resource accounting before enabling these commands.
    const auto insert=[&](const std::shared_ptr<RetainedSceneObject>& object,const char* label) {
        std::shared_ptr<HistoryChange> change;std::unique_ptr<PreparedHistoryChange> stage;
        Check(object->PrepareInsertion(64ull*1024*1024,change,stage)==MMS_OK && stage->IsCurrent(),"prepare real loader result for history");
        stage->Publish();stage.reset();
        std::unique_ptr<SharedHistory::NativeTransaction> tx;
        Check(journal.PrepareNative(label,std::move(change),tx)==MMS_OK && journal.CommitNative(std::move(tx))==MMS_OK,"publish real import in shared journal");
    };
    insert(modelOwner,"import model");
    LoadAccessoryFile(accessoryPath);auto* accessory=app->AccessorySlot(0);
    Check(accessory && accessory->mesh,"load production X mesh");auto* mesh=accessory->mesh;
    auto accessoryOwner=RetainSceneObject(*app,MMS_OBJECT_ACCESSORY,0);insert(accessoryOwner,"import accessory");
    resource_budget::Report accessoryBudget{}, duplicateBudget{};
    Check(accessoryOwner->MeasureResources(accessoryBudget)==MMS_OK && accessoryBudget.d3d9Bytes>0,
          "measure real X mesh buffers and retained accessory track");
    Check(RetainedSceneObject::MeasureResourcesBatch({accessoryOwner,accessoryOwner},duplicateBudget)==MMS_OK &&
          duplicateBudget.totalBytes==accessoryBudget.totalBytes && duplicateBudget.missing==accessoryBudget.missing,
          "duplicate owner in a measurement batch is charged once");
    accessory->parentModel=0;accessory->parentBone=0;
    app->AccessoryKeys(0)[0].parentModel=0;app->AccessoryKeys(0)[0].parentBone=0;
    auto store=CreateSceneStore(app);SceneClient client;auto api=([&]{ MmsSceneApiV1 value{}; if (store->Api(client, value) != MMS_OK) throw std::runtime_error("scene API binding failed"); return value; }());
    MmsSceneQueryV1 query{};query.h=Header<MmsSceneQueryV1>();query.channels=MMS_CHANNEL_MESH|MMS_CHANNEL_BONES;query.time={0,1};
    MmsSnapshotV1 snapshot{};MmsSpan objects{};
    Check(api.acquire(api.context,&query,&snapshot)==MMS_OK && api.objects(api.context,snapshot.handle,&objects)==MMS_OK && objects.count==2,
          "scene ABI sees both imported objects");
    const auto oldModel=SceneObjectIdentity(raw),oldAccessory=SceneObjectIdentity(accessory);
    const auto remove=[&](const std::shared_ptr<RetainedSceneObject>& object,const char* label) {
        std::unique_ptr<SharedHistory::NativeTransaction> tx;
        Check(journal.PrepareNative(label,object->CaptureRemoval(64ull*1024*1024),tx)==MMS_OK && journal.CommitNative(std::move(tx))==MMS_OK,"publish real object deletion");
    };
    remove(modelOwner,"delete model");remove(accessoryOwner,"delete accessory");
    resource_budget::Report parkedModel{}, parkedAccessory{};
    Check(modelOwner->MeasureResources(parkedModel)==MMS_OK && accessoryOwner->MeasureResources(parkedAccessory)==MMS_OK &&
          parkedModel.d3d9Bytes==modelBudget.d3d9Bytes && parkedAccessory.d3d9Bytes==accessoryBudget.d3d9Bytes &&
          parkedAccessory.cpuBytes>=sizeof(mdl::AccessoryRecord),
          "measure removed objects through retained owners without live scene slots");
    Check(!(parkedModel.missing & resource_budget::RawTextCapacity),
          "removed model retains its registered text capacities for undo");
    Check(!app->ModelSlot(0) && !app->AccessorySlot(0) && world->getNumCollisionObjects()==1 && world->getNumConstraints()==0,
          "only ground physics remains after both deletions");
    Check(api.objects(api.context,snapshot.handle,&objects)==MMS_OK && objects.count==2,"old ABI snapshot survives real object deletion");
    auto selected=query;selected.objects={&oldModel,1,sizeof(MmsObjectRef),1};MmsSnapshotV1 stale{};
    Check(api.acquire(api.context,&selected,&stale)==MMS_E_STALE,"deleted model generation cannot be acquired");
    const auto modelOffline=std::filesystem::path(modelPath).wstring()+L".offline";
    const auto accessoryOffline=std::filesystem::path(accessoryPath).wstring()+L".offline";
    std::filesystem::rename(modelPath,modelOffline);std::filesystem::rename(accessoryPath,accessoryOffline);
    app->CameraMode()=1;
    Check(PostDeviceReset(app),"reset DX9 device with objects held only by history");
    Check(journal.History(false)==MMS_OK && journal.History(false)==MMS_OK,"undo both deletions while source files are unavailable");
    Check(app->ModelSlot(0)==raw && app->AccessorySlot(0)==accessory && model.vertexBuffer==vertexBuffer && accessory->mesh==mesh,
          "undo reuses original model texture/mesh owners");
    Check(world->getNumCollisionObjects()==bodyCount && world->getNumConstraints()==jointCount && accessory->parentModel==0 && app->AccessoryKeys(0)[0].parentModel==0,
          "restore live physics and accessory binding");
    Check(SUCCEEDED(vertexBuffer->GetDesc(&descriptor)),"retained model buffer remains valid after reset");
    RenderFrameSceneForOutput(app);
    const auto restoredModel=SceneObjectIdentity(raw),restoredAccessory=SceneObjectIdentity(accessory);
    Check(Same(oldModel.id,restoredModel.id) && Same(oldAccessory.id,restoredAccessory.id) &&
          oldModel.generation!=restoredModel.generation && oldAccessory.generation!=restoredAccessory.generation,"real object restore renews generations and keeps IDs");
    Check(journal.History(false)==MMS_OK && journal.History(false)==MMS_OK && !app->ModelSlot(0) && !app->AccessorySlot(0),
          "shared history undoes both earlier imports");
    Check(journal.History(true)==MMS_OK && journal.History(true)==MMS_OK && app->ModelSlot(0)==raw && app->AccessorySlot(0)==accessory,
          "redo both imports without reopening source files");
    Check(journal.History(true)==MMS_OK && journal.History(true)==MMS_OK && !app->ModelSlot(0) && !app->AccessorySlot(0),"redo both real deletions");
    api.release(api.context,snapshot.handle);
    modelOwner.reset();accessoryOwner.reset();Check(journal.Reset()==MMS_OK,"reset resource history");journal.Collect();
    Check(world->getNumCollisionObjects()==1 && world->getNumConstraints()==0,"collect resources before production shutdown");
    std::filesystem::rename(modelOffline,modelPath);std::filesystem::rename(accessoryOffline,accessoryPath);
    app->SceneModified()=0;
    std::puts("PASS production object resource history");
    RunBatch(app,modelPath,accessoryPath);
    ProjectBoundary(app,modelPath,accessoryPath);
}
}
