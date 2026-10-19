// Include after Fixture/Check and scene_resource_budget declarations.
#include "mikumikustudio/undo_storage.hpp"
namespace {
void UndoStorageChecks() {
    namespace us = undo_storage;
    namespace rb = plugins::resource_budget;
    const auto baseline = us::Count();
    {
        bool success = false;
        for (long limit = 0; limit < 8 && !success; ++limit) {
            void* pointer = nullptr; bool memory = false;
            failAfter = limit;
            try { pointer = us::Allocate(137); } catch (const std::bad_alloc&) { memory = true; }
            failAfter = -1;
            if (memory) Check(us::Count() == baseline, "undo allocation/registration failure leaves no entry");
            else {
                us::Allocation allocation;
                Check(us::Find(pointer, allocation) && allocation.bytes == 137,
                    "undo new allocation records exact requested bytes");
                failAfter = 0; us::Free(pointer); failAfter = -1;
                success = true;
            }
        }
        Check(success && us::Count() == baseline, "undo new allocation and no-allocation release complete");
        failAfter = 0; void* failedMalloc = us::Malloc(89); void* failedCalloc = us::Calloc(3, 31); failAfter = -1;
        Check(!failedMalloc && !failedCalloc && us::Count() == baseline,
            "undo malloc/calloc registration failure releases temporary payload");
        failAfter = 0; void* overflow = us::Calloc((std::numeric_limits<std::size_t>::max)(), 2); failAfter = -1;
        Check(!overflow && us::Count() == baseline, "undo calloc rejects product overflow without allocation");
        auto* zeroed = static_cast<unsigned char*>(us::Calloc(3, 31));
        us::Allocation allocation;
        Check(zeroed && us::Find(zeroed, allocation) && allocation.bytes == 93,
            "undo calloc records complete product capacity");
        bool zero = true; for (unsigned i = 0; i < 93; ++i) zero = zero && zeroed[i] == 0;
        Check(zero, "undo calloc preserves zero initialization");
        failAfter = 0; us::Delete(zeroed); failAfter = -1;
        void* empty = us::Allocate(0);
        Check(us::Find(empty, allocation) && allocation.bytes == 0, "zero-byte undo allocation remains tracked");
        us::Delete(empty);
        Check(us::Count() == baseline, "undo allocator families unregister after mixed release entrypoints");
    }
    {
        auto model = std::make_unique<mdl::ModelRecord>();
        auto& first = model->undoRings[0].slots[1];
        first.operation = 7; first.dirty = 999999; first.reserved0 = 999999;
        first.bonePose = reinterpret_cast<mdl::BonePoseSnapshot*>(std::uintptr_t(1));
        first.auxiliaryPose = reinterpret_cast<void*>(std::uintptr_t(3));
        rb::Collector unknown; nh::MeasureResources(*model, unknown);
        Check(unknown.Result().cpuBytes == 0 && (unknown.Result().missing & rb::UndoCapacity),
            "unknown undo buffers report missing capacity without reading supposed headers");
        first.bonePose = nullptr;
        first.auxiliaryPose = us::Allocate(13);
        auto& alias = model->undoRings[1].slots[5]; alias.auxiliaryPose = first.auxiliaryPose;
        us::Allocation allocation; Check(us::Find(first.auxiliaryPose, allocation), "find odd-size undo test allocation");
        rb::Collector known; nh::MeasureResources(*model, known);
        Check(known.Result().Complete() && known.Result().cpuBytes == 13 + allocation.registryBytes,
            "undo measurement counts actual capacity once despite invalid metadata and cross-ring aliases");
        us::Free(first.auxiliaryPose); first.auxiliaryPose = nullptr; alias.auxiliaryPose = nullptr;
    }
    {
        Fixture f;
        f.selected[0] = 1; f.selected[1] = 0;
        auto app = std::make_unique<MMDApp>(); ::new (static_cast<void*>(&app->state)) MMDAppState{};
        app->ModelSlot(0) = f.raw(); app->state.slotIdx = 0;
        for (unsigned edit = 0; edit < 100; ++edit) {
            f.bones[0].trans[0] = static_cast<float>(edit);
            PushBoneEditUndo(app.get());
            const auto& entry = f.m->undoRings[0].slots[f.m->undoState[0]];
            us::Allocation allocation;
            Check(us::Find(entry.bonePose, allocation) && allocation.bytes == sizeof(mdl::BonePoseSnapshot),
                "real pose writer retains selected allocation capacity through ring wrap");
        }
        Check(us::Count() == baseline + 30, "one hundred pose edits retain only thirty registered buffers");
        rb::Collector measured; nh::MeasureResources(*f.m, measured);
        Check(measured.Result().Complete(), "real wrapped undo ring has complete raw-buffer budget");
        int frame = 0;
        nh::ModelEdit undo;
        const auto beforePrepare = us::Count();
        Check(undo.Prepare(f.raw(), frame, false) == nh::Result::Ready && us::Count() == beforePrepare + 1,
            "native undo prepares registered inverse pose outside live ring");
        Check(PublishNoAllocation(undo, frame), "native undo publishes registered inverse without allocation");
        const auto& redo = f.m->undoRings[1].slots[f.m->undoState[1]];
        us::Allocation inverse;
        Check(us::Find(redo.bonePose, inverse) && inverse.bytes == sizeof(mdl::BonePoseSnapshot),
            "native redo ring owns the prepared inverse allocation");
        rb::Collector after; nh::MeasureResources(*f.m, after);
        Check(after.Result().Complete() && after.Result().cpuBytes > measured.Result().cpuBytes,
            "native undo inverse contributes its exact retained allocation");
        app->ModelSlot(0) = nullptr;
    }
    Check(us::Count() == baseline, "wrapped pose fixture releases all registered undo buffers");
    {
        Fixture f;
        f.selected = {1, 0};
        RegisterSelectedBoneKeys(f.raw(), 0, 2);
        const auto& entry = f.m->undoRings[0].slots[f.m->undoState[0]];
        us::Allocation pose, keys;
        Check(us::Find(entry.bonePose, pose) && pose.bytes == 2 * sizeof(mdl::BonePoseSnapshot) &&
            us::Find(entry.auxiliaryPose, keys) && keys.bytes == 3 * sizeof(mdl::BoneKeySnapshot),
            "real key registrar records full reserved auxiliary capacity beyond dirty count");
        Check(entry.dirty < 3, "registration fixture exercises unused allocated auxiliary slots");
        rb::Collector report; nh::MeasureResources(*f.m, report);
        Check(report.Result().Complete() && report.Result().cpuBytes == pose.bytes + keys.bytes +
            pose.registryBytes + keys.registryBytes, "key undo budget includes both payloads and registry nodes");
        const auto count = us::Count();
        nh::ModelEdit candidate; int frame = 0;
        // MSVC allocates a sentinel even for an empty map. The existing scale
        // and curve history candidates contain complete 30-slot side states;
        // measure this fixture's allocation count instead of guessing a limit.
        const long counterStart = (std::numeric_limits<long>::max)();
        failAfter = counterStart;
        const auto initialResult = candidate.Prepare(f.raw(), frame, false);
        const long prepareAllocations = counterStart - failAfter;
        failAfter = -1;
        Check(initialResult == nh::Result::Ready && prepareAllocations > 0,
            "prepare registered key undo candidate");
        Check(us::Count() > count, "prepared inverse key buffers are registered");
        candidate = nh::ModelEdit{};
        Check(us::Count() == count, "cancelled candidate releases its private undo registrations");
        const auto digest = f.Digest(); bool prepared = false;
        for (long limit = 0; limit <= prepareAllocations && !prepared; ++limit) {
            nh::ModelEdit attempt;
            failAfter = limit; const auto result = attempt.Prepare(f.raw(), frame, false); failAfter = -1;
            if (result == nh::Result::Ready) prepared = true;
            else Check(result == nh::Result::Memory && us::Count() == count,
                "native undo allocation failure releases every staged registration");
            Check(f.Digest() == digest, "staged registration never changes live undo or authored data");
        }
        Check(prepared && us::Count() == count, "native undo allocation sweep reaches successful cancellable prepare");
        const auto ring = f.m->undoState[0]; const auto* oldAux = entry.auxiliaryPose;
        auto app = std::make_unique<MMDApp>(); ::new (static_cast<void*>(&app->state)) MMDAppState{};
        app->ModelSlot(0) = f.raw(); app->state.slotIdx = 0;
        f.m->undoState[0] = (ring + 29) % 30; PushBoneEditUndo(app.get());
        const auto& replaced = f.m->undoRings[0].slots[ring];
        Check(replaced.operation == 1 && replaced.auxiliaryPose == oldAux && us::Find(oldAux, keys),
            "pose-only overwrite preserves registration of retained older auxiliary storage");
        rb::Collector retained; nh::MeasureResources(*f.m, retained);
        Check(retained.Result().Complete() && retained.Result().cpuBytes >= keys.bytes + keys.registryBytes,
            "retained auxiliary is charged independently of current pose-only operation metadata");
        app->ModelSlot(0) = nullptr;
    }
    Check(us::Count() == baseline, "key writer fixture releases all registered undo buffers");
    {
        auto model = std::make_unique<mdl::ModelRecord>();
        for (auto& ring : model->undoRings) for (auto& entry : ring.slots) {
            entry.bonePose = static_cast<mdl::BonePoseSnapshot*>(us::Allocate(sizeof(mdl::BonePoseSnapshot)));
            entry.auxiliaryPose = us::Malloc(11);
            Check(entry.auxiliaryPose != nullptr, "allocate disposable undo auxiliary");
        }
        Check(us::Count() == baseline + 120, "both full rings own all registered allocations");
        ModelDispose(reinterpret_cast<unsigned char*>(model.get()));
        Check(us::Count() == baseline, "model disposal unregisters all sixty undo and redo slots");
    }
}
}
