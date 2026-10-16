// Include after Hook in the hidden production host. The translation unit needs
// d3d_wrapper.hpp and path_workspace.hpp at global scope. Existing MessageBox
// guards turn any unexpected native dialog into a test failure.
namespace import_buffer_failure_checks {
using LockFn = HRESULT (WINAPI*)(void*, UINT, UINT, void**, DWORD);
using UnlockFn = HRESULT (WINAPI*)(void*);
enum class Operation { None, Lock, Unlock };
struct Fault {
    Operation operation = Operation::None;
    D3DRESOURCETYPE type = D3DRTYPE_VERTEXBUFFER;
    unsigned ordinal = 0, seen = 0, injected = 0;
} fault;
LockFn originalLock[3]{};
UnlockFn originalUnlock[3]{};
void* lockTargets[3]{};
void* unlockTargets[3]{};
unsigned lockCount = 0, unlockCount = 0;
bool Matches(void* self, Operation operation) {
    return model_import::Preparing() && fault.operation == operation &&
        static_cast<IDirect3DResource9*>(self)->GetType() == fault.type &&
        ++fault.seen == fault.ordinal;
}
template<unsigned Index>
HRESULT WINAPI Lock(void* self, UINT offset, UINT size, void** data, DWORD flags) {
    if (Matches(self, Operation::Lock)) {
        ++fault.injected;
        if (data) *data = nullptr;
        return D3DERR_INVALIDCALL;
    }
    return originalLock[Index](self, offset, size, data, flags);
}
template<unsigned Index>
HRESULT WINAPI Unlock(void* self) {
    // The injected failure must not leave a real D3D lock outstanding. Exercise
    // the production HRESULT branch after the driver has released its lock.
    const HRESULT result = originalUnlock[Index](self);
    if (Matches(self, Operation::Unlock)) {
        Check(SUCCEEDED(result), "real imported buffer unlock succeeds before injected failure");
        ++fault.injected;
        return D3DERR_INVALIDCALL;
    }
    return result;
}
struct BufferHooks {
    BufferHooks(void* mainBuffer, void* edgeBuffer, void* indexBuffer) {
        const LockFn locks[]{&Lock<0>, &Lock<1>, &Lock<2>};
        const UnlockFn unlocks[]{&Unlock<0>, &Unlock<1>, &Unlock<2>};
        lockCount = unlockCount = 0;
        for (void* buffer : {mainBuffer, edgeBuffer, indexBuffer}) {
            Check(buffer != nullptr, "existing model supplies real D3D buffers for failure hooks");
            auto** table = *static_cast<void***>(buffer);
            bool knownLock = false, knownUnlock = false;
            for (unsigned i = 0; i < lockCount; ++i) knownLock |= lockTargets[i] == table[11];
            for (unsigned i = 0; i < unlockCount; ++i) knownUnlock |= unlockTargets[i] == table[12];
            // VB/IB implementations can share entry points. Install one hook
            // per address and let GetType plus ordinal select the fault.
            if (!knownLock) {
                lockTargets[lockCount] = table[11];
                Hook(reinterpret_cast<LockFn>(table[11]), locks[lockCount], &originalLock[lockCount]);
                ++lockCount;
            }
            if (!knownUnlock) {
                unlockTargets[unlockCount] = table[12];
                Hook(reinterpret_cast<UnlockFn>(table[12]), unlocks[unlockCount], &originalUnlock[unlockCount]);
                ++unlockCount;
            }
        }
    }
    ~BufferHooks() {
        fault.operation = Operation::None;
        for (unsigned i = 0; i < lockCount; ++i) {
            MH_DisableHook(lockTargets[i]); MH_RemoveHook(lockTargets[i]);
        }
        for (unsigned i = 0; i < unlockCount; ++i) {
            MH_DisableHook(unlockTargets[i]); MH_RemoveHook(unlockTargets[i]);
        }
    }
};
void Run(MMDApp* app, const wchar_t* modelPath) {
    Check(app && app->Renderer() && app->Physics() && app->ModelSlot(0),
          "buffer failure probe has an existing real scene");
    const auto& oldModel = *mdl::Mdl(app->ModelSlot(0));
    BufferHooks hooks(oldModel.vertexBuffer, oldModel.vertexBuffer2, oldModel.indexBuffer);
    auto* world = app->Physics()->world;
    const int constraintId = app->Physics()->constraintId;
    std::vector<std::pair<btCollisionObject*, btBroadphaseProxy*>> bodies;
    std::vector<btTypedConstraint*> joints;
    for (int i = 0; i < world->getNumCollisionObjects(); ++i) {
        auto* body = world->getCollisionObjectArray()[i];
        bodies.emplace_back(body, body->getBroadphaseHandle());
    }
    for (int i = 0; i < world->getNumConstraints(); ++i) joints.push_back(world->getConstraint(i));
    std::vector<unsigned char*> models;
    std::vector<mdl::AccessoryRecord*> accessories;
    for (unsigned i = 0; i < 255; ++i) {
        models.push_back(app->ModelSlot(i)); accessories.push_back(app->AccessorySlot(i));
    }
    const auto& pool = app->Renderer()->resourcePool;
    std::vector<unsigned char> cache(sizeof pool);
    std::memcpy(cache.data(), &pool, sizeof pool);
    const auto cwd = std::filesystem::current_path();
    const auto workspace = app->PathWorkspace();
    const auto dirty = app->SceneModified(), recorded = app->RecordedFrameCountByte1();
    const auto selectedModel = app->SelectedModelSlot(), selectedAccessory = app->SelectedAccessorySlot();
    const auto camera = app->CameraMode();
    auto unchanged = [&] {
        Check(app->Physics()->world == world && app->Physics()->constraintId == constraintId &&
              world->getNumCollisionObjects() == int(bodies.size()) && world->getNumConstraints() == int(joints.size()),
              "failed private buffer preparation preserves live physics counts and ID allocator");
        for (size_t i = 0; i < bodies.size(); ++i)
            Check(world->getCollisionObjectArray()[int(i)] == bodies[i].first &&
                  bodies[i].first->getBroadphaseHandle() == bodies[i].second,
                  "failed private buffer preparation preserves live body order and proxies");
        for (size_t i = 0; i < joints.size(); ++i)
            Check(world->getConstraint(int(i)) == joints[i], "failed private buffer preparation preserves live joints");
        bool sameSlots = true;
        for (unsigned i = 0; i < 255; ++i)
            sameSlots &= app->ModelSlot(i) == models[i] && app->AccessorySlot(i) == accessories[i];
        Check(sameSlots, "failed private buffer preparation preserves every scene slot");
        Check(std::memcmp(cache.data(), &pool, sizeof pool) == 0,
              "failed private buffer preparation leaves shared texture cache byte-identical");
        Check(std::filesystem::current_path() == cwd &&
              std::memcmp(&workspace, &app->PathWorkspace(), sizeof workspace) == 0 &&
              app->SceneModified() == dirty && app->RecordedFrameCountByte1() == recorded &&
              app->SelectedModelSlot() == selectedModel && app->SelectedAccessorySlot() == selectedAccessory &&
              app->CameraMode() == camera,
              "failed private buffer preparation preserves paths selection and dirty state");
    };
    // A successful preparation proves these failures are caused by the hook,
    // not by a malformed/missing fixture or an unrelated importer gate.
    {
        history_physics::ImportScene scene(*app->Physics());
        import_texture_cache::Preparation textures(*app->Renderer());
        model_import::Prepared prepared;
        Check(model_import::Prepare(*app, modelPath, false, scene.Scene(), textures, prepared) ==
                  model_import::Result::Ready && prepared.Object(),
              "same geometry fixture prepares successfully without injected buffer failure");
        const auto& model = *mdl::Mdl(prepared.Object());
        Check(model.vertexCount && model.indexCount && model.vertexBuffer && model.vertexBuffer2 && model.indexBuffer,
              "buffer failure fixture exercises main outline and index resources");
    }
    unchanged();
    for (auto operation : {Operation::Lock, Operation::Unlock}) {
        for (unsigned stage = 0; stage < 3; ++stage) {
            fault = {operation, stage == 2 ? D3DRTYPE_INDEXBUFFER : D3DRTYPE_VERTEXBUFFER,
                     stage == 1 ? 2u : 1u, 0, 0};
            {
                history_physics::ImportScene scene(*app->Physics());
                import_texture_cache::Preparation textures(*app->Renderer());
                model_import::Prepared prepared;
                const auto result = model_import::Prepare(*app, modelPath, false, scene.Scene(), textures, prepared);
                Check(fault.injected == 1, "selected imported buffer operation reaches its single fault");
                Check(result == model_import::Result::InvalidModel && !prepared.Object() && prepared.Messages().empty(),
                      "required buffer failure rejects private import without publishing output or dialogs");
                Check(scene.Scene().world->getNumCollisionObjects() == 0 && scene.Scene().world->getNumConstraints() == 0,
                      "required buffer failure cleans the private physics scene");
            }
            fault.operation = Operation::None;
            unchanged();
        }
    }
    std::puts("PASS private import main-VB outline-VB and IB Lock/Unlock failures preserve the existing scene");
}
}
