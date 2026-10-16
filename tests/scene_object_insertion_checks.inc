#include <limits>

namespace {
struct MeasuredInsertionScene {
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    plugins::RetainedSceneObject::Objects objects;
    std::vector<unsigned> accessorySlots;
    MeasuredInsertionScene() {
        ::new (static_cast<void*>(&app->state)) MMDAppState{};
        app->CameraParentModel() = -1;
    }
    std::shared_ptr<plugins::RetainedSceneObject> Model(unsigned slot, size_t capacity = 8) {
        auto* model = static_cast<mdl::ModelRecord*>(std::calloc(1, sizeof(mdl::ModelRecord)));
        Check(model != nullptr, "allocate measured insertion model");
        model->boneKeys = mdl::BoneKeyPoolCreate(capacity);
        model->boneKeys[3].frame = 23; model->boneKeys[3].position[0] = 7;
        std::wcscpy(model->path, L"missing-measured-insertion-source.pmx");
        app->ModelSlot(slot) = reinterpret_cast<unsigned char*>(model);
        auto owner = plugins::RetainSceneObject(*app, MMS_OBJECT_MODEL, slot);
        objects.push_back(owner); return owner;
    }
    std::shared_ptr<plugins::RetainedSceneObject> Accessory(unsigned slot) {
        auto* object = new mdl::AccessoryRecord{};
        object->scale = object->opacity = 1; object->parentModel = -1;
        app->AccessorySlot(slot) = object;
        app->AccessoryKeys(slot) = mdl::KeyPoolCreate<mdl::AccessoryKey>(8);
        auto& head = app->AccessoryKeys(slot)[0];
        head.scale = head.opacity = 1; head.parentModel = -1;
        accessorySlots.push_back(slot);
        auto owner = plugins::RetainSceneObject(*app, MMS_OBJECT_ACCESSORY, slot);
        objects.push_back(owner); return owner;
    }
    ~MeasuredInsertionScene() {
        // All journal/candidate variables are declared after this fixture and
        // have gone away. Park any remaining live objects before disposal.
        for (const auto& object : objects) if (object->Present()) {
            auto candidate = object->PreparePresence(false); candidate->Publish();
        }
        objects.clear();
        for (auto slot : accessorySlots) mdl::KeyPoolDestroy(app->AccessoryKeys(slot));
        curve_host::Reset(app.get());
    }
};

void MeasuredInsertionBudgetChecks() {
    using namespace plugins;
    MeasuredInsertionScene scene;
    auto object = scene.Model(4);
    auto* raw = scene.app->ModelSlot(4); auto& model = *mdl::Mdl(raw);
    const auto identity = SceneObjectIdentity(raw);
    std::shared_ptr<HistoryChange> insertion;
    std::unique_ptr<PreparedHistoryChange> stage;
    Check(object->PrepareInsertion(insertion, stage) == MMS_OK && stage->IsCurrent() &&
        scene.app->ModelSlot(4) == raw && object->Present() &&
        SceneObjectIdentity(raw).generation == identity.generation,
        "measured insertion preparation leaves membership and generation unchanged");
    resource_budget::Report resources;
    Check(object->MeasureResources(resources) == MMS_OK && resources.Complete(),
        "measured insertion fixture has complete native resources");
    const auto expectedNative = insertion->RetainedBytes() + resources.totalBytes;
    uint64_t stagingBytes = 0;
    Check(stage->TargetRetainedBytes(stagingBytes) && stagingBytes < expectedNative,
        "staging measures removal while insertion owns an additional record wrapper");
    failAfter = 0; const bool current = stage->IsCurrent(); if (current) stage->Publish(); failAfter = -1;
    Check(current && !scene.app->ModelSlot(4) && !object->Present(),
        "measured insertion parks new resources without allocation");
    stage.reset();
    std::unique_ptr<PreparedHistoryChange> target;
    Check(insertion->Prepare(true, target) == MMS_OK, "prepare measured insertion target");
    uint64_t targetBytes = 0;
    Check(target->TargetRetainedBytes(targetBytes) && targetBytes == expectedNative,
        "insertion dynamic target includes all resources and its complete wrapper");
    target.reset();
    Check(!scene.app->ModelSlot(4) && !object->Present(), "cancelling insertion target leaves new object parked");
    const std::string label = "measured import";
    const auto expectedTotal = expectedNative + sizeof(EditRecord) + label.size();
    StorageStore storage;
    std::unique_ptr<SharedHistory::NativeTransaction> tx;
    {
        SharedHistory tooSmall(storage, {}, 128, expectedTotal - 1);
        Check(tooSmall.PrepareNative(label, insertion, tx) == MMS_E_MEMORY && !tx &&
            !tooSmall.UndoCount() && !tooSmall.RetainedBytes() && !object->Present(),
            "one byte below complete measured insertion charge rejects before publication");
    }
    {
        SharedHistory exact(storage, {}, 128, expectedTotal);
        Check(exact.PrepareNative(label, insertion, tx) == MMS_OK, "exact measured insertion cap admits preparation");
        tx.reset();
        Check(!exact.UndoCount() && !exact.RetainedBytes() && !object->Present(),
            "cancelled measured journal reservation leaves resources parked");
        Check(exact.PrepareNative(label, insertion, tx) == MMS_OK, "prepare exact measured insertion again");
        failAfter = 0; const auto committed = exact.CommitNative(std::move(tx)); failAfter = -1;
        Check(committed == MMS_OK && object->Present() && scene.app->ModelSlot(4) == raw &&
            exact.RetainedBytes() == expectedTotal, "measured insertion publishes without allocation and charges wrapper");
        Check(exact.History(false) == MMS_OK && !object->Present() && exact.RetainedBytes() == expectedTotal,
            "measured insertion undo keeps complete dynamic charge");

        auto large = scene.Model(5, 4096);
        std::shared_ptr<HistoryChange> expensive; std::unique_ptr<PreparedHistoryChange> expensiveStage;
        Check(large->PrepareInsertion(expensive, expensiveStage) == MMS_OK, "prepare a larger measured import");
        expensiveStage->Publish(); expensiveStage.reset();
        const auto redoLabel = *exact.NextLabel(true);
        Check(exact.PrepareNative(label, expensive, tx) == MMS_E_MEMORY && !tx &&
            !large->Present() && !exact.UndoCount() && exact.RedoCount() == 1 &&
            exact.RetainedBytes() == expectedTotal && *exact.NextLabel(true) == redoLabel,
            "over-budget staged import preserves prior redo and its retained resources");
        Check(exact.History(true) == MMS_OK && object->Present() && model.boneKeys[3].position[0] == 7 &&
            exact.RetainedBytes() == expectedTotal,
            "measured redo restores original model data without rereading source");
        Check(mdl::BoneKeyPoolEnsure(model, mdl::BoneKeyCapacity(model) + 1), "grow resource beyond exact insertion budget");
        const auto beforeFailedUndo = SceneObjectIdentity(raw).generation;
        Check(exact.History(false) == MMS_E_MEMORY && object->Present() && scene.app->ModelSlot(4) == raw &&
            exact.UndoCount() == 1 && !exact.RedoCount() && exact.RetainedBytes() == expectedTotal &&
            SceneObjectIdentity(raw).generation == beforeFailedUndo,
            "grown insertion undo rejects insufficient dynamic budget without moving history or membership");
        Check(exact.Reset() == MMS_OK, "reset exact measured insertion journal"); exact.Collect();
    }
    Check(object->PrepareInsertion(insertion, stage) == MMS_OK, "stage measured model for growth checks");
    stage->Publish(); stage.reset();
    SharedHistory roomy(storage);
    Check(roomy.PrepareNative(label, insertion, tx) == MMS_OK && roomy.CommitNative(std::move(tx)) == MMS_OK,
        "record measured import before native pool growth");
    const auto originalCharge = roomy.RetainedBytes();
    Check(mdl::BoneKeyPoolEnsure(model, mdl::BoneKeyCapacity(model) + 1), "grow imported model native pool");
    Check(roomy.History(false) == MMS_OK && !object->Present() && roomy.RetainedBytes() > originalCharge,
        "insertion undo remeasures increased native resource capacity");
    const auto grownCharge = roomy.RetainedBytes();
    Check(roomy.History(true) == MMS_OK && object->Present() && roomy.RetainedBytes() == grownCharge,
        "insertion redo retains grown resource charge and wrapper");
    Check(roomy.History(false) == MMS_OK, "park grown imported model");
    Check(roomy.PrepareNative(label, insertion, tx) == MMS_OK, "prepare measured insertion before another resource grows");
    Check(mdl::BoneKeyPoolEnsure(model, mdl::BoneKeyCapacity(model) + 1), "grow parked imported model after prepare");
    failAfter = 0; const auto stale = roomy.CommitNative(std::move(tx)); failAfter = -1;
    Check(stale == MMS_E_STALE && !object->Present() && !roomy.UndoCount() && roomy.RedoCount() == 1 &&
        roomy.RetainedBytes() == grownCharge, "resource growth invalidates prepared insertion without replacing redo");
    Check(roomy.History(true) == MMS_OK && roomy.RetainedBytes() > grownCharge,
        "fresh measured redo admits new capacity after stale insertion rejection");
}

void MeasuredInsertionFailureChecks() {
    using namespace plugins;
    MeasuredInsertionScene scene, otherScene;
    auto object = scene.Model(4); auto foreign = otherScene.Model(4);
    auto* raw = scene.app->ModelSlot(4); auto& model = *mdl::Mdl(raw);
    const auto identity = SceneObjectIdentity(raw);
    std::shared_ptr<HistoryChange> insertion;
    std::unique_ptr<PreparedHistoryChange> stage;
    Check(object->PrepareInsertion(insertion, stage) == MMS_OK, "prepare output sentinels for insertion failures");
    auto* originalChange = insertion.get(); auto* originalStage = stage.get();
    const auto unchanged = [&] {
        return insertion.get() == originalChange && stage.get() == originalStage &&
            object->Present() && scene.app->ModelSlot(4) == raw &&
            SceneObjectIdentity(raw).generation == identity.generation;
    };
    for (const RetainedSceneObject::Objects& invalid : {
        RetainedSceneObject::Objects{}, RetainedSceneObject::Objects{nullptr},
        RetainedSceneObject::Objects{object, object}, RetainedSceneObject::Objects{object, foreign}}) {
        Check(RetainedSceneObject::PrepareInsertionBatch(invalid, insertion, stage) == MMS_E_ARGUMENT &&
            unchanged() && stage->IsCurrent(), "invalid measured insertion description preserves outputs and scene");
    }
    // Unknown pointers must produce incomplete budgets without being scanned.
    model.pmxTextBuffers[0] = reinterpret_cast<wchar_t*>(std::uintptr_t(1));
    const auto incomplete = object->PrepareInsertion(insertion, stage);
    model.pmxTextBuffers[0] = nullptr;
    Check(incomplete == MMS_E_NOT_READY && unchanged() && stage->IsCurrent(),
        "unknown allocation refuses measured staging without changing existing outputs");
    Check(object->PrepareInsertion(UINT64_MAX, insertion, stage) == MMS_E_MEMORY && unchanged(),
        "legacy explicit insertion overflow still leaves outputs unchanged");
    {
        std::shared_ptr<HistoryChange> explicitBudget;
        std::unique_ptr<PreparedHistoryChange> explicitStage, explicitTarget;
        uint64_t bytes = 0;
        Check(object->PrepareInsertion(1024, explicitBudget, explicitStage) == MMS_OK &&
            explicitBudget->Prepare(false, explicitTarget) == MMS_OK &&
            !explicitTarget->TargetRetainedBytes(bytes),
            "explicit-budget insertion retains its existing fixed-charge test interface");
    }

    std::shared_ptr<HistoryChange> attempt;
    std::unique_ptr<PreparedHistoryChange> candidate;
    const long counter = (std::numeric_limits<long>::max)();
    failAfter = counter;
    const auto initial = object->PrepareInsertion(attempt, candidate);
    const auto allocations = counter - failAfter; failAfter = -1;
    Check(initial == MMS_OK && allocations > 0, "calibrate complete measured insertion preparation allocations");
    attempt.reset(); candidate.reset();
    bool complete = false;
    for (long limit = 0; limit <= allocations && !complete; ++limit) {
        failAfter = limit; const auto result = object->PrepareInsertion(attempt, candidate); failAfter = -1;
        if (result == MMS_OK) complete = true;
        else Check(result == MMS_E_MEMORY && !attempt && !candidate,
            "every measured insertion allocation failure leaves both outputs empty");
        Check(unchanged() && stage->IsCurrent(), "failed or cancelled insertion preparation leaves original candidate valid");
        attempt.reset(); candidate.reset();
    }
    Check(complete, "measured insertion allocation sweep reaches successful cancellable preparation");
    stage->Publish(); stage.reset();
    failAfter = counter;
    const auto targetResult = insertion->Prepare(true, candidate);
    const auto targetAllocations = counter - failAfter; failAfter = -1;
    Check(targetResult == MMS_OK && targetAllocations > 0, "calibrate insertion target including dynamic charge wrapper");
    candidate.reset(); complete = false;
    for (long limit = 0; limit <= targetAllocations && !complete; ++limit) {
        failAfter = limit; const auto result = insertion->Prepare(true, candidate); failAfter = -1;
        if (result == MMS_OK) complete = true;
        else Check(result == MMS_E_MEMORY && !candidate, "insertion target allocation failure remains unpublished");
        Check(!object->Present() && !scene.app->ModelSlot(4), "insertion target preparation never unparks resources");
        candidate.reset();
    }
    Check(complete, "dynamic insertion target allocation sweep reaches cancellation");
    Check(object->PrepareInsertion(attempt, candidate) == MMS_E_ARGUMENT && !attempt && !candidate,
        "already parked object cannot be captured as a new insertion");
}

void MeasuredInsertionOwnershipChecks() {
    using namespace plugins;
    {
        MeasuredInsertionScene scene;
        auto model = scene.Model(4); auto accessory = scene.Accessory(3);
        auto* raw = scene.app->ModelSlot(4); auto* original = scene.app->AccessorySlot(3);
        auto* keys = scene.app->AccessoryKeys(3);
        scene.app->AccessoryTrackCursor(3) = 2; scene.app->AccessoryTrackActive(3) = 1;
        std::shared_ptr<HistoryChange> insertion; std::unique_ptr<PreparedHistoryChange> stage;
        Check(RetainedSceneObject::PrepareInsertionBatch(scene.objects, insertion, stage) == MMS_OK && stage->IsCurrent(),
            "prepare measured mixed model and accessory batch");
        stage.reset(); insertion.reset();
        Check(model->Present() && accessory->Present() && scene.app->AccessoryKeys(3) == keys &&
            scene.app->AccessoryTrackCursor(3) == 2, "cancelled mixed staging leaves live scene and accessory track intact");
        Check(RetainedSceneObject::PrepareInsertionBatch(scene.objects, insertion, stage) == MMS_OK,
            "prepare mixed measured batch again");
        failAfter = 0; stage->Publish(); failAfter = -1; stage.reset();
        Check(!model->Present() && !accessory->Present() && !scene.app->ModelSlot(4) &&
            !scene.app->AccessorySlot(3) && scene.app->AccessoryKeys(3) != keys,
            "mixed measured staging parks both objects and exchanges accessory pool");
        StorageStore storage; SharedHistory journal(storage);
        std::unique_ptr<SharedHistory::NativeTransaction> tx;
        Check(journal.PrepareNative("mixed import", insertion, tx) == MMS_OK, "prepare mixed measured journal insertion");
        failAfter = 0; const auto committed = journal.CommitNative(std::move(tx)); failAfter = -1;
        Check(committed == MMS_OK && scene.app->ModelSlot(4) == raw && scene.app->AccessorySlot(3) == original &&
            scene.app->AccessoryKeys(3) == keys && scene.app->AccessoryTrackCursor(3) == 2 &&
            scene.app->AccessoryTrackActive(3) == 1, "mixed measured publication restores both retained resources without allocation");
        Check(journal.History(false) == MMS_OK && !model->Present() && !accessory->Present(),
            "mixed measured undo parks both resources atomically");
        Check(journal.History(true) == MMS_OK && scene.app->ModelSlot(4) == raw &&
            scene.app->AccessorySlot(3) == original && scene.app->AccessoryKeys(3) == keys,
            "mixed measured redo restores retained identities and pool");
        Check(journal.History(false) == MMS_OK, "park mixed measured resources for owner release");
        std::weak_ptr<RetainedSceneObject> weakModel = model, weakAccessory = accessory;
        scene.objects.clear(); model.reset(); accessory.reset(); insertion.reset();
        Check(!weakModel.expired() && !weakAccessory.expired(), "journal alone owns parked measured import resources");
        Check(journal.Reset() == MMS_OK && !weakModel.expired() && !weakAccessory.expired(),
            "measured import disposal is deferred outside history publication");
        journal.Collect();
        Check(weakModel.expired() && weakAccessory.expired() && !scene.app->ModelSlot(4) && !scene.app->AccessorySlot(3),
            "collection releases all parked mixed import owners");
    }
    {
        MeasuredInsertionScene scene; auto object = scene.Model(4);
        std::weak_ptr<RetainedSceneObject> weak = object;
        std::shared_ptr<HistoryChange> insertion; std::unique_ptr<PreparedHistoryChange> stage;
        Check(object->PrepareInsertion(insertion, stage) == MMS_OK, "stage measured import for reservation cancellation lifetime");
        stage->Publish(); stage.reset();
        StorageStore storage; SharedHistory journal(storage);
        std::unique_ptr<SharedHistory::NativeTransaction> tx;
        Check(journal.PrepareNative("cancel import", insertion, tx) == MMS_OK, "reserve measured import before releasing caller owners");
        scene.objects.clear(); object.reset(); insertion.reset();
        Check(!weak.expired() && !scene.app->ModelSlot(4), "uncommitted reservation keeps parked import alive");
        tx.reset();
        Check(weak.expired() && !scene.app->ModelSlot(4) && !journal.UndoCount() && !journal.RetainedBytes(),
            "cancelled last reservation disposes parked import without recording history");
    }
}

void SceneObjectInsertionChecks() {
    MeasuredInsertionBudgetChecks();
    MeasuredInsertionFailureChecks();
    MeasuredInsertionOwnershipChecks();
}
}
