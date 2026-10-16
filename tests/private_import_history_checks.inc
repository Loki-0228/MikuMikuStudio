#include "mikudancestudio/model_import.hpp"
#include "../src/plugins/scene_identity.hpp"
#include <io.h>
namespace {
void PrivateAccessoryJournalChecks() {
    using namespace plugins;
    AccessoryImportFixture f; f.Vac();
    StorageStore storage; SharedHistory history(storage);
    import_texture_cache::Preparation textures(*f.renderer, AccessoryImportMissingTexture);
    ai::Prepared prepared;
    Check(ai::Prepare(*f.app, f.vac.c_str(), textures, prepared) == ai::Result::Ready, "journal private VAC parse");
    // CPU journal tests use a geometry-free object; real mesh accounting and
    // loader-to-journal integration are exercised by the isolated host tests.
    static_cast<IUnknown*>(prepared.Object()->mesh)->Release(); prepared.Object()->mesh = nullptr;
    auto* pointer = prepared.Object(); auto* keys = prepared.Keys();
    std::shared_ptr<import_texture_cache::Retained> retained;
    std::unique_ptr<import_texture_cache::Publication> publication;
    Check(textures.Finish(retained, publication) == MMS_OK, "freeze private import texture publication");
    auto change = RetainedSceneObject::ImportAccessory(*f.app, 7, prepared, retained, std::move(publication));
    Check(!prepared.Object() && !f.app->AccessorySlot(7), "adopting parser output does not publish object");
    f.Unchanged();
    unsigned failures = 0;
    for (long limit = 0; limit < 1024; ++limit) {
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        failAfter = limit; const auto result = history.PrepareNative("import", change, transaction); failAfter = -1;
        Check(!f.app->AccessorySlot(7) && !history.UndoCount() && !history.RedoCount(), "failed or cancelled import reservation preserves scene and journal");
        if (result == MMS_OK) { transaction.reset(); Check(failures > 5, "import reservation failure sweep covers several allocations"); break; }
        Check(result == MMS_E_MEMORY, "import candidate allocation failure reported"); ++failures;
        Check(limit < 1023, "import reservation failure sweep terminates");
    }
    {
        SharedHistory tiny(storage, {}, 128, 1);
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        Check(tiny.PrepareNative("import", change, transaction) != MMS_OK && !transaction && !f.app->AccessorySlot(7),
              "resource budget rejection preserves unpublished import");
    }
    std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    Check(history.PrepareNative("import", change, transaction) == MMS_OK, "reserve wholly private accessory import");
    failAfter = 0; const auto committed = history.CommitNative(std::move(transaction)); failAfter = -1;
    Check(committed == MMS_OK && f.app->AccessorySlot(7) == pointer && f.app->AccessoryKeys(7) == keys,
          "first private import publication allocates nothing and exchanges slot and keys together");
    const auto identity = SceneObjectIdentity(pointer);
    Check(keys[0].scale == 2.5f && keys[0].position[1] == -2.5f && pointer->order == 0, "private VAC pose remains in committed root key");
    DeleteFileW(f.vac.c_str()); DeleteFileW(f.mesh.c_str()); const auto loads = f.context.calls;
    Check(history.History(false) == MMS_OK && !f.app->AccessorySlot(7) && !SceneObjectPresent(pointer, identity.id), "undo private import removes identity and object together");
    Check(history.History(true) == MMS_OK && f.app->AccessorySlot(7) == pointer && f.app->AccessoryKeys(7) == keys &&
          SceneObjectPresent(pointer, identity.id) && SceneObjectIdentity(pointer).generation != identity.generation && f.context.calls == loads,
          "redo private import restores same resources and stable ID after source deletion");
    Check(history.History(false) == MMS_OK, "park private import for history eviction");
    change.reset(); Check(history.Reset() == MMS_OK, "reset private import journal"); history.Collect();
    mdl::KeyPoolDestroy(f.app->AccessoryKeys(7)); f.app->AccessoryKeys(7) = nullptr;
    curve_host::Reset(f.app.get());
    f.Empty(); f.Unchanged();
}
void PrivateModelFailureChecks() {
    AccessoryImportFixture f; PhysicsFixture physics;
    PhysicsScene live{}; live.world = &physics.world; live.dispatcher = &physics.dispatcher;
    live.broadphase = &physics.broadphase; live.constraintId = 100;
    f.app->Physics() = &live;
    std::memcpy(f.stateBefore.data(), &f.app->state, f.stateBefore.size());
    const auto path = f.directory + L"invalid.pmd", closed = path + L".closed";
    AccessoryImportFixture::Write(path, "invalid model header");
    const auto beforeCount = physics.world.getNumCollisionObjects();
    for (long limit : {-1L, 0L, 1L}) {
        history_physics::ImportScene scene(live);
        import_texture_cache::Preparation textures(*f.renderer, AccessoryImportMissingTexture);
        model_import::Prepared prepared;
        failAfter = limit;
        const auto result = model_import::Prepare(*f.app, path.c_str(), true, scene.Scene(), textures, prepared);
        failAfter = -1;
        Check(result == model_import::Result::InvalidModel || result == model_import::Result::Memory,
              "private malformed model returns error without showing parser dialogs");
        Check(!prepared.Object() && physics.world.getNumCollisionObjects() == beforeCount && scene.Scene().world->getNumCollisionObjects() == 0,
              "private model failure preserves old world and output ownership");
        Check(MoveFileW(path.c_str(), closed.c_str()) && MoveFileW(closed.c_str(), path.c_str()),
              "parser closes source file even when deferred error-message allocation fails");
        f.Unchanged();
    }
    auto* partial = static_cast<mdl::ModelRecord*>(std::calloc(1, sizeof(mdl::ModelRecord)));
    partial->scenePtr = &live; partial->boneCount = partial->rigidCount = partial->jointCount = 5;
    model_import::DisposePrivate(reinterpret_cast<unsigned char*>(partial)); std::free(partial);
    Check(physics.world.getNumCollisionObjects() == beforeCount, "partial model count before table allocation is safely disposable");
    DeleteFileW(path.c_str()); f.app->Physics() = nullptr;
}
void PrivateImportIdentityChecks() {
    plugins::SceneIdentity ids; int cancelled = 0, committed = 0;
    const auto cancelledId = ids.CreateAbsent(&cancelled).object.id;
    Check(ids.Known(cancelledId) && !ids.Retained(&cancelled)->present, "prepared object starts with an absent identity");
    ids.Forget(&cancelled);
    Check(!ids.Known(cancelledId) && !ids.Retained(&cancelled), "cancelled unpublished identity leaves no tombstone allocation");
    const auto committedId = ids.CreateAbsent(&committed).object.id;
    auto publish = ids.PreparePresence(&committed, true);
    failAfter = 0; const bool ok = publish.Publish(); failAfter = -1;
    Check(ok && ids.Get(&committed).published, "private identity publishes without allocation");
    auto remove = ids.PreparePresence(&committed, false); Check(remove.Publish(), "published identity can be retained absent");
    ids.Forget(&committed);
    Check(ids.Known(committedId), "formerly published identity remains known after disposal for stale ID detection");
}
void PrivateImportHistoryChecks() { PrivateImportIdentityChecks(); PrivateAccessoryJournalChecks(); PrivateModelFailureChecks(); }
}
