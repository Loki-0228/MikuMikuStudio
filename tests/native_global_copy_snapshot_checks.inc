// Included after the real global-copy fixture. Tests the new production 420
// receipt independently of the pending model-payload/keyboard suites.
#include "../src/plugins/native_global_copy.hpp"
#include "mikumikustudio/plugin_scene.hpp"

namespace {
namespace global_copy_snapshot_checks {
namespace copy = global_copy_failure_checks;
using Status = frame_clipboard::GlobalCopyStatus;
using Receipt = std::shared_ptr<const plugins::GlobalCopySnapshot>;
struct OwnedFixture : copy::CopyFixture {
    ~OwnedFixture() {
        for(auto& object:accessory)plugins::ForgetSceneObject(&object);
    }
};

Receipt Read(copy::CopyFixture& f, Status expected) {
    Receipt out = f.app->GlobalClipboardCopy().snapshot;
    Check(plugins::ReadGlobalCopySnapshot(*f.app,out)==expected,
          "receipt reports the exact current copy state");
    Check(bool(out)==(expected==Status::Captured),"only a complete copy exposes an immutable payload");
    return out;
}
template<class T> bool Equal(const std::vector<T>& a,const std::vector<T>& b) {
    return a.size()==b.size() && (a.empty() || !std::memcmp(a.data(),b.data(),a.size()*sizeof(T)));
}
bool Equal(const plugins::GlobalPasteSource& a,const plugins::GlobalPasteSource& b) {
    return a.frameOffset==b.frameOffset && Equal(a.cameras,b.cameras) && Equal(a.lights,b.lights) &&
        Equal(a.shadows,b.shadows) && Equal(a.gravity,b.gravity) && Equal(a.accessories,b.accessories);
}
void MatchesRaw(copy::CopyFixture& f,const plugins::GlobalCopySnapshot& receipt) {
    plugins::GlobalPasteSource source;
    Check(plugins::CaptureGlobalPastePayload(*f.app,source)==MMS_OK && Equal(source,receipt.payload),
          "receipt owns every raw global clipboard field and relative frame");
    Check(!receipt.payload.frameOffset && receipt.nativeFirst==9 &&
          receipt.attempt==f.app->GlobalClipboardCopy().attempt && receipt.sceneGeneration==f.app->SceneGeneration(),
          "receipt records the copy-time origin and batch separately from future destination offsets");
}
void DetachParents(copy::CopyFixture& f) {
    for(std::size_t i=0;i<mdl::KeyPoolCapacity(f.app->CameraKeys());++i)f.app->CameraKeys()[i].parentModel=-1;
    for(unsigned slot:{7u,254u})for(std::size_t i=0;i<mdl::KeyPoolCapacity(f.app->AccessoryKeys(slot));++i)
        f.app->AccessoryKeys(slot)[i].parentModel=-1;
    plugins::SceneObjectIdentity(f.app->AccessorySlot(7));
    plugins::SceneObjectIdentity(f.app->AccessorySlot(254));
}

void LifetimeAndOrigin() {
    OwnedFixture f;DetachParents(f);Read(f,Status::Unavailable);
    const copy::Snapshot before(f);copy::Copy(f);before.Expect(f);
    auto old=Read(f,Status::Captured);MatchesRaw(f,*old);
    Check(plugins::GlobalCopySnapshotCurrent(*f.app,*old),"complete receipt binds live accessory identities");
    const auto saved=old->payload;const auto first=old->nativeFirst;const auto batch=old->attempt;
    f.app->CurrentFrame()=-123;f.Select(false);
    f.app->CameraKeys()[1].frame=777;f.app->CameraKeys()[1].eye[0]=91;
    Check(Read(f,Status::Captured)==old && old->nativeFirst==first && Equal(old->payload,saved) &&
          plugins::GlobalCopySnapshotCurrent(*f.app,*old),
          "playhead selection and later source edits cannot rewrite the saved copy origin or payload");
    f.SeedOld();
    Check(Equal(old->payload,saved),"freeing and replacing all native clipboard buffers preserves the retained receipt bytes");

    // An actual empty model-mode copy replaces its own buffers only.
    Fixture model;ClearRealCopyMarks(model);
    f.app->ModelSlot(0)=model.raw();f.app->SelectedModelSlot()=0;f.app->CameraMode()=0;
    CmdControl400(f.app.get(),nullptr,420,0);
    Check(Read(f,Status::Captured)==old && old->attempt==batch && Equal(old->payload,saved),
          "actual model-mode copy does not erase or replace the global-mode receipt");
    f.app->CameraMode()=1;f.app->ModelSlot(0)=nullptr;
    // The model copy freed its buffers; its fixture owns no such allocations.
    f.Select(true);f.app->CameraKeys()[1].frame=21;
    CmdControl400(f.app.get(),nullptr,420,0);
    auto next=Read(f,Status::Captured);
    Check(next!=old && next->attempt==batch+1 && !plugins::GlobalCopySnapshotCurrent(*f.app,*old) &&
          plugins::GlobalCopySnapshotCurrent(*f.app,*next) && Equal(old->payload,saved),
          "a subsequent real global copy publishes a new immutable batch while older bytes remain readable");
    f.Select(false);CmdControl400(f.app.get(),nullptr,420,0);Read(f,Status::Empty);
    Check(!plugins::GlobalCopySnapshotCurrent(*f.app,*next) && Equal(old->payload,saved),
          "an empty copy is distinct from missing or failed and supersedes the previous batch");
    f.Select(true);CmdControl400(f.app.get(),nullptr,420,0);auto reset=Read(f,Status::Captured);
    const auto resetBytes=reset->payload;f.app->AdvanceSceneGeneration();Read(f,Status::Unavailable);
    Check(!plugins::GlobalCopySnapshotCurrent(*f.app,*reset) && Equal(reset->payload,resetBytes),
          "the production scene-generation retirement clears current receipt ownership without damaging retained bytes");
}

void RawFieldsAndMissingIdentity() {
    OwnedFixture f;
    for(auto& accessory:f.accessory)plugins::ForgetSceneObject(&accessory);
    f.app->CameraKeys()[1].eye[0]=std::numeric_limits<float>::quiet_NaN();
    f.app->CameraKeys()[1].frame=UINT32_MAX;
    copy::Copy(f);auto receipt=Read(f,Status::Captured);MatchesRaw(f,*receipt);
    Check(!plugins::GlobalCopySnapshotCurrent(*f.app,*receipt),
          "raw capture records missing references instead of declaring them valid");
    Check(std::isnan(receipt->payload.cameras[0].eye[0]) && receipt->payload.cameras[0].frame==UINT32_MAX-9,
          "raw capture preserves invalid numeric and frame data for later whole-source semantic admission");
    Fixture parent;f.app->ModelSlot(17)=parent.raw();f.app->ModelSlot(19)=parent.raw();
    plugins::SceneObjectIdentity(parent.raw());
    for(auto& accessory:f.accessory)plugins::SceneObjectIdentity(&accessory);
    Check(!plugins::GlobalCopySnapshotCurrent(*f.app,*receipt),
          "later identities or objects cannot retroactively repair a missing copy-time reference");
    f.app->ModelSlot(17)=f.app->ModelSlot(19)=nullptr;plugins::ForgetSceneObject(parent.raw());
}

void IdentitiesAndStructure() {
    OwnedFixture f;auto parent=std::make_unique<Fixture>();
    f.app->ModelSlot(17)=parent->raw();f.app->ModelSlot(19)=parent->raw();
    plugins::SceneObjectIdentity(parent->raw());
    for(auto& accessory:f.accessory)plugins::SceneObjectIdentity(&accessory);
    copy::Copy(f);auto receipt=Read(f,Status::Captured);
    Check(plugins::GlobalCopySnapshotCurrent(*f.app,*receipt),"receipt accepts existing parent and accessory identities");
    mdl::AccessoryRecord replacement{};plugins::SceneObjectIdentity(&replacement);
    f.app->AccessorySlot(7)=&replacement;
    Check(!plugins::GlobalCopySnapshotCurrent(*f.app,*receipt),"a different accessory in the same slot is not the copied source");
    f.app->AccessorySlot(7)=&f.accessory[0];
    Check(plugins::GlobalCopySnapshotCurrent(*f.app,*receipt),"restoring the original still-live object restores its reference guard");
    std::array<mdl::BoneRecord,2> newBones{};const auto originalBones=parent->m->boneTable;
    parent->m->boneTable=newBones.data();
    Check(!plugins::GlobalCopySnapshotCurrent(*f.app,*receipt),"same model identity with a replacement bone table is stale");
    parent->m->boneTable=originalBones;--parent->m->boneCount;
    Check(!plugins::GlobalCopySnapshotCurrent(*f.app,*receipt),"same model pointer with a different bone count is stale");
    ++parent->m->boneCount;
    plugins::ForgetSceneObject(parent->raw());plugins::SceneObjectIdentity(parent->raw());
    Check(!plugins::GlobalCopySnapshotCurrent(*f.app,*receipt),"reused model pointer with a fresh identity cannot stand in for the original");
    copy::Copy(f);receipt=Read(f,Status::Captured);
    Check(plugins::GlobalCopySnapshotCurrent(*f.app,*receipt),"a new explicit copy can capture the replacement identity");
    const auto bytes=receipt->payload;const auto pointer=parent->raw();
    f.app->ModelSlot(17)=f.app->ModelSlot(19)=nullptr;plugins::ForgetSceneObject(pointer);parent.reset();
    Check(!plugins::GlobalCopySnapshotCurrent(*f.app,*receipt) && Equal(bytes,receipt->payload),
          "checking a removed and destroyed parent uses slot identity before any cached pointer dereference");
    plugins::ForgetSceneObject(&replacement);
}

bool CopyAtAllowance(copy::CopyFixture& f,long allowance) {
    const auto old=f.app->MainWindow();f.app->MainWindow()=f.controls.host;
    bool threw=false;failAfter=allowance;
    try{CmdControl400(f.app.get(),f.controls.host,420,0);}catch(...){threw=true;}
    failAfter=-1;f.app->MainWindow()=old;return !threw;
}
void AllocationFailures() {
    OwnedFixture f;DetachParents(f);f.SeedHistory();
    Check(CopyAtAllowance(f,0),"first receipt allocation failure stays inside the real copy command");
    Read(f,Status::Failed);copy::Empty(f);f.controls.Expect(false,false);
    Check(f.app->GlobalClipboardCopy().error==MMS_E_MEMORY && f.app->GlobalClipboardCopy().attempt==1,
          "first-ever capture OOM has a persistent failed state and error without needing a side-map allocation");
    unsigned failed=0;bool completed=false;
    for(long allowance=0;allowance<96;++allowance) {
        f.Select(false);negative_frames::SelectRange(f.app.get(),-70,-70);
        copy::Copy(f);Read(f,Status::Empty); // Valid old negative clipboard.
        negative_frames::SelectRange(f.app.get(),1,2);f.Select(true);f.SeedOld();
        const copy::Snapshot before(f);const auto batch=f.app->GlobalClipboardCopy().attempt;
        Check(CopyAtAllowance(f,allowance),"receipt allocation failures never escape production 420");
        before.Expect(f);
        if(f.app->GlobalClipboardCopy().status==Status::Captured) {
            auto receipt=Read(f,Status::Captured);MatchesRaw(f,*receipt);f.controls.Expect(true,true);
            Check(receipt->attempt==batch+1 && plugins::GlobalCopySnapshotCurrent(*f.app,*receipt),
                  "successful receipt preparation publishes exactly one complete new source batch");
            completed=true;break;
        }
        ++failed;Read(f,Status::Failed);copy::Empty(f);f.controls.Expect(false,false);
        Check(f.app->GlobalClipboardCopy().attempt==batch+1 && f.app->GlobalClipboardCopy().error==MMS_E_MEMORY,
              "each receipt preparation OOM leaves a failed batch and no partial source");
        f.app->CurrentFrame()=-500;const copy::Snapshot noPaste(f);
        CmdControl400(f.app.get(),nullptr,421,0);noPaste.Expect(f);
        Check(negative_frames::Count(f.app.get())==2,"metadata capture OOM also invalidates the old negative clipboard");
    }
    Check(completed && failed>=6,"allocation sweep covers snapshot ownership and all five payload vectors before success");
    std::printf("  global copy receipt allocation sweep: %u failed stages\n",failed);
    f.app->GlobalClipboardCopy().attempt=UINT64_MAX;
    Check(CopyAtAllowance(f,0),"exhausted copy batch counter rejects without allocation or throwing");
    Read(f,Status::Failed);copy::Empty(f);
    Check(f.app->GlobalClipboardCopy().attempt==UINT64_MAX,"copy batch identities never wrap to a previously valid value");
}

void OwnerDestruction() {
    std::weak_ptr<const plugins::GlobalCopySnapshot> weak;
    {
        OwnedFixture f;copy::Copy(f);auto receipt=Read(f,Status::Captured);weak=receipt;
        receipt.reset();Check(!weak.expired(),"the application owns its receipt without a process-global map");
    }
    Check(weak.expired(),"destroying the application releases its last receipt ownership");
}
void ReceiptProtocol() {
    OwnedFixture f;DetachParents(f);
    plugins::CompleteEmptyGlobalCopy(*f.app);Read(f,Status::Failed);
    Check(plugins::CaptureGlobalCopySnapshot(*f.app,9)==MMS_E_NOT_READY,
          "completion or capture cannot fabricate a successful begin");
    copy::Copy(f);auto receipt=Read(f,Status::Captured);Receipt read;
    bool threw=false,current=false;Status status=Status::Unavailable;
    failAfter=0;
    try{status=plugins::ReadGlobalCopySnapshot(*f.app,read);current=plugins::GlobalCopySnapshotCurrent(*f.app,*receipt);}
    catch(...){threw=true;}
    failAfter=-1;
    Check(!threw && current && status==Status::Captured && read==receipt &&
          receipt->sourceMode==plugins::GlobalCopySourceMode::Global,
          "receipt reads and identity checks allocate nothing and retain the original global source mode");
    ++f.app->GlobalClipboardCopy().attempt;Read(f,Status::Failed);
    Check(!plugins::GlobalCopySnapshotCurrent(*f.app,*receipt),"a mismatched batch stamp cannot expose a captured envelope");
    f.app->GlobalClipboardCopy().attempt=UINT64_MAX;
    Check(!plugins::BeginGlobalCopy(*f.app) && !f.app->GlobalClipboardCopy().preparing,
          "counter exhaustion does not leave a preparation permit");
    Check(plugins::CaptureGlobalCopySnapshot(*f.app,9)==MMS_E_NOT_READY,
          "capture cannot reuse the failed exhausted begin");
    plugins::CompleteEmptyGlobalCopy(*f.app);Read(f,Status::Failed);
}
}
void NativeGlobalCopySnapshotChecks() {
    using namespace global_copy_snapshot_checks;
    LifetimeAndOrigin();RawFieldsAndMissingIdentity();IdentitiesAndStructure();AllocationFailures();OwnerDestruction();ReceiptProtocol();
}
}
