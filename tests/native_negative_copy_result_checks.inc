// Include after native_global_copy_snapshot_checks.inc. The parent TU owns
// NativeNegativeCopyResultChecks() routing (--negative-copy-result-only).
// Uses synthetic fixtures and invisible controls only. No cross-zero paste,
// lossless NativeClipboard, or common positive/negative batch identity claim.
#include <type_traits>

namespace {
namespace negative_copy_result_checks {
namespace copy = global_copy_failure_checks;
namespace receipt = global_copy_snapshot_checks;
using Result = negative_frames::CopyResult;
using Status = frame_clipboard::GlobalCopyStatus;
using Mode = plugins::SharedHistory::RecordingMode;
constexpr long measureAllowance = (std::numeric_limits<long>::max)();
static_assert(std::is_same<decltype(negative_frames::CopySelected(nullptr)),Result>::value,
    "negative copy exposes its typed outcome");
static_assert(noexcept(negative_frames::CopySelected(nullptr)),
    "negative copy contains every exception");
static_assert(std::is_same<decltype(negative_frames::Command(nullptr,420)),bool>::value,
    "the legacy command remains a handled-bool adapter");

// Observe real frees, without allocating or substituting a production result.
// A new global receipt (or model target mapping) must already be published when
// its native buffers are retired. An early native/receipt OOM cannot satisfy
// this witness. All five pointers are saved before the first one is freed.
struct Retirement {
    MMDApp* app=nullptr;
    unsigned char* model=nullptr;
    DWORD thread=GetCurrentThreadId();
    std::uint64_t attempt=0;
    std::array<void*,5> pointers{};
    std::array<unsigned,5> freed{};
    bool published=false;
    void Observe(void* pointer) noexcept {
        if(!pointer)return;
        if(!published) {
            std::array<void*,5> candidates{};
            if(!model) {
                const auto& state=app->GlobalClipboardCopy();
                if(state.attempt!=attempt || state.status!=Status::Captured || state.preparing ||
                    !state.snapshot || state.snapshot->attempt!=attempt || state.error!=MMS_OK ||
                    copy::Counts(*app)!=copy::counts)return;
                const auto& payload=state.snapshot->payload;
                if(payload.cameras.size()!=copy::counts[0] || payload.lights.size()!=copy::counts[1] ||
                    payload.shadows.size()!=copy::counts[2] || payload.gravity.size()!=copy::counts[3] ||
                    payload.accessories.size()!=copy::counts[4])return;
                candidates=copy::Buffers(*app);
            } else {
                const auto& n=app->ClipboardCounts();
                if(n.bones!=1 || n.morphs!=1 || n.displays!=1 || !app->DisplayClipboard() ||
                    frame_clipboard::BoneTarget(app,model,0)!=0 ||
                    frame_clipboard::MorphTarget(app,model,0)!=0)return;
                const auto& display=*app->DisplayClipboard();
                if(display.ikCount!=1 || display.selectorCount!=1)return;
                candidates={{app->BoneClipboard(),app->MorphClipboard(),app->DisplayClipboard(),
                    display.ikStates,display.selectorStates}};
            }
            bool matches=false;
            for(auto p:candidates){if(!p)return;if(p==pointer)matches=true;}
            if(!matches)return;
            pointers=candidates;published=true;
        }
        for(unsigned i=0;i<pointers.size();++i)if(pointer==pointers[i])++freed[i];
    }
    void Expect() const {
        Check(published,"negative OOM retires an already completed native copy, not an early receipt failure");
        for(auto count:freed)Check(count==1,"each completed native allocation, including display children, is freed exactly once");
    }
};
thread_local Retirement* retirement=nullptr;
thread_local unsigned freeDepth=0;
using FreeFn=void (__cdecl*)(void*);
FreeFn originalFree=nullptr;
void __cdecl ObserveFree(void* pointer) {
    if(retirement && !freeDepth && retirement->thread==GetCurrentThreadId())retirement->Observe(pointer);
    ++freeDepth;originalFree(pointer);--freeDepth;
}
struct FreeHook {
    void* target=reinterpret_cast<void*>(&std::free);
    bool owns=false,created=false,enabled=false;
    bool Install() noexcept {
        auto status=MH_Initialize();owns=status==MH_OK;
        if(status!=MH_OK && status!=MH_ERROR_ALREADY_INITIALIZED)return false;
        if(MH_CreateHook(target,reinterpret_cast<void*>(&ObserveFree),
            reinterpret_cast<void**>(&originalFree))!=MH_OK)return false;
        created=true;
        if(MH_EnableHook(target)!=MH_OK)return false;
        enabled=true;return true;
    }
    bool Remove() noexcept {
        retirement=nullptr;bool clean=true;
        if(enabled) {
            const auto status=MH_DisableHook(target);
            if(status==MH_OK || status==MH_ERROR_DISABLED)enabled=false;else clean=false;
        }
        if(created){if(MH_RemoveHook(target)==MH_OK)created=enabled=false;else clean=false;}
        if(owns){if(MH_Uninitialize()==MH_OK)owns=false;else clean=false;}
        return clean;
    }
    ~FreeHook(){Remove();}
};
struct Attempt {
    long spent=0;
    bool exhausted=false,threw=false,overflow=false;
    std::size_t outstanding=0;
};
Attempt Command(copy::CopyFixture& f,long allowance=measureAllowance,Retirement* witness=nullptr) {
    Check(failAfter==-1 && !allocationAudit && !retirement && !copy::activeTrace,
        "negative copy injection starts outside every other allocation hook");
    FreeHook hook;const bool installed=!witness || hook.Install();
    AllocationAudit audit;Attempt out;const auto window=f.app->MainWindow();
    if(installed) {
        f.app->MainWindow()=f.controls.host;
        retirement=witness;allocationAudit=&audit;failAfter=allowance;
        try{CmdControl400(f.app.get(),f.controls.host,420,0);}catch(...){out.threw=true;}
        out.spent=allowance-failAfter;out.exhausted=failAfter==0;
        failAfter=-1;allocationAudit=nullptr;retirement=nullptr;f.app->MainWindow()=window;
    }
    out.outstanding=audit.outstanding;out.overflow=audit.overflow;
    // Check terminates rather than unwinding: restore all TLS/window/hook state
    // before asserting, including a partially failed hook installation.
    const bool removed=hook.Remove();
    Check(installed && removed && !freeDepth,"negative-copy free observer installs and removes only its own hook");
    Check(!out.threw && !out.overflow,"real 420 contains failures within the fixed allocation audit capacity");
    return out;
}
Result Typed(MMDApp* app,long allowance,Attempt& out) {
    Check(failAfter==-1 && !allocationAudit,"typed negative copy starts outside failure injection");
    AllocationAudit audit;allocationAudit=&audit;failAfter=allowance;
    const auto result=negative_frames::CopySelected(app);
    out.spent=allowance-failAfter;out.exhausted=failAfter==0;
    failAfter=-1;allocationAudit=nullptr;
    out.outstanding=audit.outstanding;out.overflow=audit.overflow;
    Check(!out.overflow,"typed copy fits the fixed allocation audit");return result;
}
void NoNegativePaste(copy::CopyFixture& f) {
    const int cursor=f.app->CurrentFrame();f.app->CurrentFrame()=-500;
    const copy::Snapshot before(f);const auto count=negative_frames::Count(f.app.get());
    CmdControl400(f.app.get(),nullptr,421,0);before.Expect(f);
    Check(negative_frames::Count(f.app.get())==count,
        "failed or empty copy leaves no old negative batch usable by actual negative-to-negative 421");
    const auto window=f.app->MainWindow();f.app->MainWindow()=f.controls.host;
    negative_frames::RefreshControls(f.app.get());f.app->MainWindow()=window;
    f.controls.Expect(false,false);f.app->CurrentFrame()=cursor;
}
void FailedReceipt(copy::CopyFixture& f,std::uint64_t attempt) {
    receipt::Read(f,Status::Failed);copy::Empty(f);f.controls.Expect(false,false);
    const auto& state=f.app->GlobalClipboardCopy();
    Check(state.error==MMS_E_MEMORY && state.attempt==attempt && !state.snapshot && !state.preparing &&
        state.sceneGeneration==f.app->SceneGeneration(),
        "negative OOM invalidates the receipt with MEMORY and an empty snapshot without incrementing attempt twice");
}
void SelectOld(copy::CopyFixture& f) {
    negative_frames::SelectRange(f.app.get(),-70,-70);
    Check(negative_frames::CopySelected(f.app.get())==Result::Copied,"seed a distinct old negative batch");
    negative_frames::SelectRange(f.app.get(),-40,-40);
}

void TypedResults(Mode mode) {
    receipt::OwnedFixture f;receipt::DetachParents(f);
    Check(f.history.SetMode(mode)==MMS_OK,"configure typed-copy history mode");
    Attempt attempt;
    Check(Typed(nullptr,0,attempt)==Result::Failed && !attempt.spent && !attempt.outstanding,
        "null application returns Failed without allocation");
    {const copy::Snapshot before(f);
        Check(Typed(f.app.get(),0,attempt)==Result::Empty && !attempt.outstanding,
            "an application without negative state returns Empty without creating state");before.Expect(f);}
    f.Select(false);Command(f);f.SeedHistory();
    SelectOld(f);negative_frames::SelectRange(f.app.get(),-70,-40);
    {const copy::Snapshot before(f);
        Check(Typed(f.app.get(),measureAllowance,attempt)==Result::Copied && attempt.spent>0,
            "valid selected negative keys report Copied");before.Expect(f);}
    const auto cost=attempt.spent;
    for(long allowance=0;allowance<cost;++allowance) {
        SelectOld(f);negative_frames::SelectRange(f.app.get(),-70,-40);const copy::Snapshot before(f);
        Check(Typed(f.app.get(),allowance,attempt)==Result::Memory && attempt.exhausted && !attempt.outstanding,
            "every measured direct-copy allocation failure reports Memory and releases partial storage");
        before.Expect(f);f.controls.Expect(false,false);NoNegativePaste(f);
    }
    SelectOld(f);negative_frames::SelectRange(f.app.get(),1,2);
    {const copy::Snapshot before(f);
        Check(Typed(f.app.get(),0,attempt)==Result::Empty && !attempt.outstanding,
            "no current selected negative key returns Empty and discards the old batch without allocating");before.Expect(f);}
    NoNegativePaste(f);

    SelectOld(f);f.app->CameraMode()=0;
    {const copy::Snapshot before(f);
        Check(Typed(f.app.get(),0,attempt)==Result::Empty,
            "selection in the other timeline is Empty and clears the old negative clipboard");before.Expect(f);}
    f.app->CameraMode()=1;NoNegativePaste(f);

    // A selected key whose accessory disappeared is not a current source.
    f.accessory[0].parentModel=-1;f.accessory[0].scale=f.accessory[0].opacity=1;f.accessory[0].visible=1;
    Check(negative_frames::RegisterAccessory(f.app.get(),7,-90),"seed a removable synthetic negative accessory");
    negative_frames::SelectRange(f.app.get(),-90,-90);
    Check(negative_frames::CopySelected(f.app.get())==Result::Copied,"copy the live accessory before detaching it");
    f.app->AccessorySlot(7)=nullptr;
    {const copy::Snapshot before(f);
        Check(Typed(f.app.get(),0,attempt)==Result::Empty,"selected stale negative identity is Empty, not Copied");
        before.Expect(f);}
    // Restoring the same identity makes a leaked old batch pasteable again.
    f.app->AccessorySlot(7)=&f.accessory[0];NoNegativePaste(f);
    SelectOld(f);const copy::Snapshot beforeLegacy(f);
    failAfter=0;const bool handled=negative_frames::Command(f.app.get(),420);failAfter=-1;
    Check(handled,"legacy 420 reports handled even when its typed copy outcome is Memory");
    beforeLegacy.Expect(f);NoNegativePaste(f);
    Check(negative_frames::CopySelected(f.app.get())==Result::Copied,"direct copy recovers after injection ends");
}

void GlobalFailures(Mode mode,bool positive) {
    receipt::OwnedFixture f;receipt::DetachParents(f);
    Check(f.history.SetMode(mode)==MMS_OK,"configure real 420 negative-failure history mode");f.SeedHistory();
    Command(f);auto held=receipt::Read(f,Status::Captured);
    const auto saved=held->payload;const auto heldAttempt=held->attempt;
    f.Select(positive);negative_frames::SelectRange(f.app.get(),1,2);
    const auto native=Command(f);
    Check(!native.exhausted,"measure native-only copy allocation cost without a fixed failure index");
    receipt::Read(f,positive?Status::Captured:Status::Empty);
    if(!positive)Check(native.spent==0,"empty native selection publishes Empty without a receipt allocation");
    negative_frames::SelectRange(f.app.get(),-40,-40);
    const auto complete=Command(f);
    Check(!complete.exhausted && complete.spent>native.spent,
        "selected negative copy adds allocation stages after the measured native prefix");
    unsigned failures=0;
    for(long allowance=native.spent;allowance<complete.spent;++allowance) {
        SelectOld(f);f.controls.EnableOld();const copy::Snapshot before(f);
        const auto next=f.app->GlobalClipboardCopy().attempt+1;
        Retirement witness;witness.app=f.app.get();witness.attempt=next;
        const auto failed=Command(f,allowance,positive?&witness:nullptr);
        if(!failed.exhausted || failed.outstanding)std::printf(
            "  copy failure diagnostic: positive=%u allowance=%ld prefix=%ld total=%ld spent=%ld outstanding=%zu exhausted=%u\n",
            unsigned(positive),allowance,native.spent,complete.spent,failed.spent,failed.outstanding,unsigned(failed.exhausted));
        Check(failed.exhausted && !failed.outstanding,"negative-side OOM releases all new C++ allocations");
        FailedReceipt(f,next);before.Expect(f);
        if(positive)witness.Expect();
        Check(held->attempt==heldAttempt && receipt::Equal(held->payload,saved) &&
            !plugins::GlobalCopySnapshotCurrent(*f.app,*held),
            "failed new copy preserves caller-held old receipt bytes while retiring its eligibility");
        NoNegativePaste(f);++failures;
    }
    Check(failures>0,"at least one actual negative allocation fails after the native phase");
    // Seed a usable side batch first: the following empty copy must clear it.
    SelectOld(f);f.Select(false);negative_frames::SelectRange(f.app.get(),1,2);
    const auto emptyAttempt=f.app->GlobalClipboardCopy().attempt+1;
    {const copy::Snapshot before(f);Command(f,0);before.Expect(f);}
    receipt::Read(f,Status::Empty);copy::Empty(f);f.controls.Expect(false,false);
    Check(f.app->GlobalClipboardCopy().attempt==emptyAttempt && f.app->GlobalClipboardCopy().error==MMS_OK,
        "empty copy supersedes failure with one successful Empty attempt");NoNegativePaste(f);
    f.Select(positive);negative_frames::SelectRange(f.app.get(),-40,-40);
    const auto recoveredAttempt=f.app->GlobalClipboardCopy().attempt+1;
    {const copy::Snapshot before(f);Command(f);before.Expect(f);}
    receipt::Read(f,positive?Status::Captured:Status::Empty);f.controls.Expect(true,positive);
    Check(f.app->GlobalClipboardCopy().attempt==recoveredAttempt && receipt::Equal(held->payload,saved),
        "copy recovers with one new attempt and leaves retained receipt bytes unchanged");
    if(positive){copy::FullPayload(f);receipt::MatchesRaw(f,*f.app->GlobalClipboardCopy().snapshot);}
    else {
        const auto count=negative_frames::Count(f.app.get()),undo=f.history.UndoCount();
        f.app->CurrentFrame()=-500;CmdControl400(f.app.get(),nullptr,421,0);
        Check(negative_frames::Count(f.app.get())==count+1 && f.history.UndoCount()==undo+1 &&
            !copy::NegativeRow(f,2,-500).empty(),
            "recovered negative-only copy really pastes its new camera and records one history step");
    }
    std::printf("  negative copy allocation sweep: positive=%u native=%ld negative failures=%u\n",
        unsigned(positive),native.spent,failures);
}

struct ModelFixture : receipt::OwnedFixture {
    Fixture model;
    std::array<mdl::IkChain,1> chains{};
    explicit ModelFixture(Mode mode) {
        Check(history.SetMode(mode)==MMS_OK,"configure model negative-copy history mode");
        receipt::DetachParents(*this);SeedHistory();Command(*this);
        app->ModelSlot(0)=model.raw();app->SelectedModelSlot()=0;app->CameraMode()=0;
        std::strcpy(model.bones[0].name,"root");std::strcpy(model.bones[1].name,"child");
        chains[0].boneIndex=0;chains[0].enabled=1;model.m->ikChains=chains.data();
        ClearRealCopyMarks(model);
        model.m->boneKeys[0].allocated=1;model.m->boneKeys[0].frame=0;
        model.m->morphKeys[0].allocated=1;model.m->morphKeys[0].frame=0;
        model.m->displayKeys[0].allocated=1;model.m->displayKeys[0].frame=0;
        model.m->displayKeys[0].visible=1;
        Check(negative_frames::RegisterBone(model.raw(),0,-70),"seed old negative model bone");
        Check(negative_frames::RegisterBone(model.raw(),1,-40),"seed selected negative model bone");
        Check(negative_frames::RegisterBone(model.raw(),0,-20),"seed disposable negative model history");
        Check(history.History(false)==MMS_OK && history.UndoCount()>0 && history.RedoCount()==1,
            "model failure fixture retains meaningful undo and redo branches");
        negative_frames::SelectRange(app.get(),1,2);
    }
    ~ModelFixture() {
        history.Reset();history.Collect();negative_frames::Reset(app.get());frame_clipboard::Reset(app.get());
        plugins::ForgetSceneObject(model.raw());app->ModelSlot(0)=nullptr;model.m->ikChains=nullptr;
    }
};
std::vector<unsigned char> ModelState(MMDApp& app) {
    auto bytes=copy::StateBytes(app);
    const auto mask=[&](const void* pointer,std::size_t size) {
        const auto begin=reinterpret_cast<std::uintptr_t>(&app.state),at=reinterpret_cast<std::uintptr_t>(pointer);
        if(at>=begin && at-begin<=bytes.size() && size<=bytes.size()-(at-begin))
            std::memset(bytes.data()+(at-begin),0,size);
    };
    mask(&app.BoneClipboard(),sizeof(void*));mask(&app.MorphClipboard(),sizeof(void*));
    mask(&app.DisplayClipboard(),sizeof(void*));auto& n=app.ClipboardCounts();
    mask(&n.bones,sizeof(n.bones));mask(&n.morphs,sizeof(n.morphs));mask(&n.displays,sizeof(n.displays));
    return bytes;
}
struct ModelLive {
    GlobalPasteKeys keys;
    std::vector<unsigned char> state,negative,selection,morphs;
    std::array<unsigned char,2> selected;
    std::uint64_t model,revision=0,epoch,sceneRevision,sceneGeneration;
    std::size_t undo,redo,retained;
    explicit ModelLive(ModelFixture& f):keys(*f.app),state(ModelState(*f.app)),
        negative(copy::NegativeRecord(*f.app,negative_frames::kPmmKey)),
        selection(copy::NegativeRecord(*f.app,negative_frames::kPmmSelectionKey)),
        morphs(copy::Bytes(f.model.morphs.data(),sizeof(f.model.morphs))),selected(f.model.selected),
        model(f.model.Digest()),epoch(f.history.NativeEpoch()),sceneRevision(f.app->SceneRevision()),
        sceneGeneration(f.app->SceneGeneration()),undo(f.history.UndoCount()),redo(f.history.RedoCount()),
        retained(f.history.RetainedBytes()) {
        Check(f.history.Revision(revision)==MMS_OK,"snapshot model copy history revision");
    }
    void Expect(ModelFixture& f) const {
        std::uint64_t now=0;
        Check(keys.Same(*f.app) && state==ModelState(*f.app) && model==f.model.Digest() && selected==f.model.selected &&
            morphs==copy::Bytes(f.model.morphs.data(),sizeof(f.model.morphs)) &&
            negative==copy::NegativeRecord(*f.app,negative_frames::kPmmKey) &&
            selection==copy::NegativeRecord(*f.app,negative_frames::kPmmSelectionKey),
            "model copy failure preserves native and negative key payloads, selections, poses and app state");
        Check(f.history.Revision(now)==MMS_OK && now==revision && epoch==f.history.NativeEpoch() &&
            undo==f.history.UndoCount() && redo==f.history.RedoCount() && retained==f.history.RetainedBytes() &&
            sceneRevision==f.app->SceneRevision() && sceneGeneration==f.app->SceneGeneration(),
            "model copy failure preserves history and scene revisions");
    }
};
struct GlobalPeer {
    std::array<void*,5> pointers;
    std::array<unsigned,5> counts;
    std::array<std::vector<unsigned char>,5> bytes;
    frame_clipboard::GlobalCopyState state;
    plugins::GlobalPasteSource payload;
    explicit GlobalPeer(ModelFixture& f):pointers(copy::Buffers(*f.app)),counts(copy::Counts(*f.app)),
        state(f.app->GlobalClipboardCopy()),payload(receipt::Read(f,Status::Captured)->payload) {
        Check(counts==copy::counts,"model test starts with all five complete peer global buffers");
        for(unsigned i=0;i<bytes.size();++i)bytes[i]=copy::Bytes(pointers[i],copy::sizes[i]);
    }
    void Expect(ModelFixture& f) const {
        const auto& now=f.app->GlobalClipboardCopy();
        Check(pointers==copy::Buffers(*f.app) && counts==copy::Counts(*f.app),
            "model negative OOM keeps all five peer global buffer pointers and counts");
        for(unsigned i=0;i<bytes.size();++i)Check(copy::Bytes(pointers[i],bytes[i].size())==bytes[i],
            "model negative OOM leaves every byte of each global buffer unchanged");
        Check(receipt::Read(f,Status::Captured)==state.snapshot && now.attempt==state.attempt &&
            now.sceneGeneration==state.sceneGeneration && now.error==state.error && now.preparing==state.preparing &&
            receipt::Equal(state.snapshot->payload,payload) && plugins::GlobalCopySnapshotCurrent(*f.app,*state.snapshot),
            "model negative OOM preserves the complete current global receipt and its owned bytes");
    }
};
void ModelFailure(Mode mode) {
    ModelFixture f(mode);
    // Warm reusable scale/map storage, then measure the actual native prefix.
    Command(f);const auto native=Command(f);const GlobalPeer peer(f);
    Check(!native.exhausted && frame_clipboard::BoneTarget(f.app.get(),f.model.raw(),0)==0 &&
        frame_clipboard::MorphTarget(f.app.get(),f.model.raw(),0)==0,
        "real model copy publishes usable bone and morph mappings before negative injection");
    negative_frames::SelectRange(f.app.get(),-40,-40);
    const auto complete=Command(f);
    Check(!complete.exhausted && complete.spent>native.spent,"model negative copy has measured allocation work after native publication");
    // One failure at the measured boundary is enough to exercise nested cleanup;
    // the global/direct sweeps cover the remainder of the side-vector growth.
    SelectOld(f);f.controls.EnableOld();const ModelLive before(f);
    Retirement witness;witness.app=f.app.get();witness.model=f.model.raw();
    const auto failed=Command(f,native.spent,&witness);
    Check(failed.exhausted && !failed.outstanding,"model negative OOM releases its temporary C++ storage and target map");
    witness.Expect();before.Expect(f);peer.Expect(f);f.controls.Expect(false,false);
    const auto& n=f.app->ClipboardCounts();
    Check(!n.bones && !n.morphs && !n.displays && !f.app->BoneClipboard() && !f.app->MorphClipboard() &&
        !f.app->DisplayClipboard() && frame_clipboard::BoneTarget(f.app.get(),f.model.raw(),0)==-1 &&
        frame_clipboard::MorphTarget(f.app.get(),f.model.raw(),0)==-1,
        "model failure clears every native family and makes its copied track mappings unavailable");
    const int cursor=f.app->CurrentFrame();f.app->CurrentFrame()=-500;
    {const ModelLive noPaste(f);CmdControl400(f.app.get(),nullptr,421,0);noPaste.Expect(f);}
    peer.Expect(f);f.app->CurrentFrame()=cursor;
    const auto window=f.app->MainWindow();f.app->MainWindow()=f.controls.host;
    negative_frames::RefreshControls(f.app.get());f.app->MainWindow()=window;f.controls.Expect(false,false);
    {const ModelLive recover(f);Command(f);recover.Expect(f);peer.Expect(f);}
    Check(f.app->ClipboardCounts().bones==1 && f.app->ClipboardCounts().morphs==1 &&
        f.app->ClipboardCounts().displays==1 && frame_clipboard::BoneTarget(f.app.get(),f.model.raw(),0)==0 &&
        frame_clipboard::MorphTarget(f.app.get(),f.model.raw(),0)==0,
        "model copy recovers its payload and mappings after the injected negative failure");
}
} // namespace negative_copy_result_checks

void NativeNegativeCopyResultChecks() {
    using namespace negative_copy_result_checks;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}) {
        TypedResults(mode);GlobalFailures(mode,true);GlobalFailures(mode,false);ModelFailure(mode);
    }
}
} // namespace
