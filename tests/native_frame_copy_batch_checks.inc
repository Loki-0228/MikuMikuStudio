// Include after native_negative_copy_source_checks.inc. The parent TU owns
// the CLI route to NativeFrameCopyBatchChecks(). No model-copy test dependency.
// All copies below use production 420 except explicitly labelled protocol tests.
#include "../src/plugins/frame_copy_batch.hpp"
#include "../src/plugins/native_model_copy.hpp"

namespace {
namespace frame_copy_batch_checks {
namespace copy = global_copy_failure_checks;
namespace receipt = global_copy_snapshot_checks;
namespace result = negative_copy_result_checks;
namespace source = negative_copy_source_checks;
namespace nf = negative_frames;
using Mode = frame_clipboard::CopyMode;
using Status = frame_clipboard::CopyBatchStatus;
using HistoryMode = plugins::SharedHistory::RecordingMode;
using Batch = std::shared_ptr<const plugins::FrameCopyBatch>;
constexpr long allowanceMax = (std::numeric_limits<long>::max)();
Mode Other(Mode mode) { return mode==Mode::Global?Mode::Model:Mode::Global; }

struct BatchFixture : result::ModelFixture {
    std::array<mdl::BoneOrderEntry,1> selectors{};
    explicit BatchFixture(HistoryMode mode):result::ModelFixture(mode) {
        plugins::SceneObjectIdentity(model.raw());
        selectors[0].boneIndex=-1;selectors[0].linkedModel=selectors[0].linkedBone=-1;
        model.m->boneOrderTable=selectors.data();
        auto& display=model.m->displayKeys[0];
        display.selectorStates[0].modelIndex=display.selectorStates[0].boneIndex=-1;
        // Three different positive frames: the last family supplies the minimum.
        model.m->boneKeys[0].frame=29;model.m->morphKeys[0].frame=17;display.frame=11;
        model.m->boneKeys[0].position[0]=12.5f;model.m->morphKeys[0].value=.625f;
        model.m->loadComplete=1;
        Check(nf::RegisterMorph(model.raw(),0,-55,.375f) && nf::RegisterDisplay(model.raw(),-30),
            "batch fixture includes negative bone morph display IK and whole-model selector rows");
        Check(nf::RegisterBone(model.raw(),0,-10) && history.History(false)==MMS_OK && history.RedoCount()==1,
            "batch fixture has nonempty undo and redo branches");
        SelectMode(Mode::Model);SelectSources(true,true);
    }
    ~BatchFixture(){model.m->boneOrderTable=nullptr;}
    void SelectMode(Mode mode){app->CameraMode()=mode==Mode::Global?1:0;app->SelectedModelSlot()=0;}
    Mode Active() const {return app->CameraMode()?Mode::Global:Mode::Model;}
    void SelectSources(bool native,bool negative) {
        if(Active()==Mode::Global)Select(native);
        else {
            ClearRealCopyMarks(model);
            model.m->boneKeys[0].allocated=model.m->morphKeys[0].allocated=
                model.m->displayKeys[0].allocated=native?1:0;
        }
        nf::SelectRange(app.get(),negative?-70:1,negative?-30:2);
    }
};

Batch Read(MMDApp& app,Mode mode,Status expected) {
    Batch out=app.FrameClipboardCopy(mode).snapshot;
    Check(plugins::ReadFrameCopyBatch(app,mode,out)==expected && bool(out)==(expected==Status::Captured),
        "batch reader reports exact state and clears every non-Captured output");
    return out;
}
Batch Copy(BatchFixture& f,Mode mode,bool native=true,bool negative=true) {
    f.SelectMode(mode);f.SelectSources(native,negative);
    const result::ModelLive before(f);const auto attempt=f.app->FrameClipboardCopy(mode).attempt;
    const auto command=result::Command(f);before.Expect(f);
    Check(!command.exhausted && f.app->FrameClipboardCopy(mode).attempt==attempt+1,
        "real 420 completes one new attempt without changing live keys or history");
    return Read(*f.app,mode,native||negative?Status::Captured:Status::Empty);
}
bool SameModelPayload(const plugins::ModelPasteSource& a,const plugins::ModelPasteSource& b) {
    if(a.frameOffset!=b.frameOffset || a.advanceLastRegisteredFrame!=b.advanceLastRegisteredFrame ||
        a.resetDisplayCursor!=b.resetDisplayCursor || a.boneCurveBlock!=b.boneCurveBlock ||
        a.morphCurveBlock!=b.morphCurveBlock || a.curveTargetsResolved!=b.curveTargetsResolved ||
        a.boneCurveTargets!=b.boneCurveTargets || a.morphCurveTargets!=b.morphCurveTargets ||
        a.bones.size()!=b.bones.size() || a.morphs.size()!=b.morphs.size() || a.displays.size()!=b.displays.size())return false;
    for(std::size_t i=0;i<a.bones.size();++i) {
        const auto& x=a.bones[i];const auto& y=b.bones[i];
        if(std::memcmp(&x.key,&y.key,sizeof(x.key)) || x.scale!=y.scale || x.scaleCurve!=y.scaleCurve ||
            x.targetResolved!=y.targetResolved || x.targetIndex!=y.targetIndex)return false;
    }
    for(std::size_t i=0;i<a.morphs.size();++i) {
        const auto& x=a.morphs[i];const auto& y=b.morphs[i];
        if(std::memcmp(&x.key,&y.key,sizeof(x.key)) || x.targetResolved!=y.targetResolved || x.targetIndex!=y.targetIndex)return false;
    }
    for(std::size_t i=0;i<a.displays.size();++i) {
        const auto& x=a.displays[i];const auto& y=b.displays[i];
        if(x.frame!=y.frame || x.visible!=y.visible || x.ikTargetsResolved!=y.ikTargetsResolved ||
            x.ikTargets!=y.ikTargets || !receipt::Equal(x.ik,y.ik) || !receipt::Equal(x.selectors,y.selectors))return false;
    }
    return true;
}
struct Held {
    Batch batch;
    plugins::FrameCopyTicket ticket;
    bool hasNative;
    std::uint32_t nativeFirst;
    std::int64_t first;
    std::unique_ptr<plugins::GlobalCopySnapshot> global;
    std::unique_ptr<plugins::ModelCopySnapshot> model;
    std::unique_ptr<nf::CopySource> negative;
    explicit Held(Batch b):batch(std::move(b)),ticket(batch->ticket),hasNative(batch->hasNative),
        nativeFirst(batch->nativeFirst),first(batch->first),
        global(batch->global?std::make_unique<plugins::GlobalCopySnapshot>(*batch->global):nullptr),
        model(batch->model?std::make_unique<plugins::ModelCopySnapshot>(*batch->model):nullptr),
        negative(batch->negative?std::make_unique<nf::CopySource>(*batch->negative):nullptr) {}
    void Expect() const {
        Check(batch->ticket.owner==ticket.owner && batch->ticket.mode==ticket.mode &&
            batch->ticket.attempt==ticket.attempt && batch->ticket.sceneGeneration==ticket.sceneGeneration &&
            batch->ticket.previousGlobalAttempt==ticket.previousGlobalAttempt &&
            batch->hasNative==hasNative && batch->nativeFirst==nativeFirst && batch->first==first,
            "held batch retains its owner mode attempt generation and signed origins");
        Check(bool(batch->global)==bool(global) && bool(batch->model)==bool(model) && bool(batch->negative)==bool(negative),
            "held batch retains both owned source envelopes");
        if(global)Check(receipt::Equal(global->payload,batch->global->payload) && global->nativeFirst==batch->global->nativeFirst,
            "held global records remain byte-exact after clipboard replacement or retirement");
        if(model)Check(SameModelPayload(model->payload,batch->model->payload) && model->nativeFirst==batch->model->nativeFirst,
            "held model records including nested IK selector and scale values remain owned");
        if(negative)Check(source::SameSource(*negative,*batch->negative),"held negative rows and reference stamps remain unchanged");
    }
};

// A narrow CRT observer also proves Current does not hide allocations behind
// a caught bad_alloc. For model OOM it arms at the old bone buffer's free;
// the next five allocations are the contiguous native buffer allocation phase.
namespace heap {
struct Trace {
    DWORD thread=GetCurrentThreadId();
    void* oldBone=nullptr;
    int failStage=-1;
    unsigned stage=0,failures=0,calls=0;
    bool armed=false,observeOnly=false;
    std::array<void*,5> pointers{};
    std::array<bool,5> live{};
    const std::array<std::size_t,5> sizes{{sizeof(mdl::BoneClipboardRecord),sizeof(mdl::MorphClipboardRecord),
        sizeof(mdl::DisplayClipboardRecord),sizeof(mdl::IkClipboardState),sizeof(mdl::SelectorClipboardState)}};
    bool Reject(std::size_t size) noexcept {
        ++calls;
        if(observeOnly || !armed || stage>=sizes.size() || size!=sizes[stage])return false;
        if(int(stage)==failStage){++failures;armed=false;return true;}return false;
    }
    void Acquire(void* p,std::size_t size) noexcept {
        if(!observeOnly && armed && p && stage<sizes.size() && size==sizes[stage]) {
            pointers[stage]=p;live[stage]=true;++stage;
        }
    }
    void Release(void* p) noexcept {
        if(!p)return;
        if(p==oldBone){oldBone=nullptr;armed=true;}
        for(unsigned i=0;i<stage;++i)if(live[i] && pointers[i]==p)live[i]=false;
    }
};
thread_local Trace* active=nullptr;
thread_local unsigned depth=0;
copy::MallocFn mallocOriginal=nullptr;
copy::CallocFn callocOriginal=nullptr;
copy::FreeFn freeOriginal=nullptr;
Trace* Active() noexcept {return active && !depth && active->thread==GetCurrentThreadId()?active:nullptr;}
void* __cdecl Malloc(std::size_t size) {
    auto* t=Active();if(t && t->Reject(size))return nullptr;
    ++depth;auto* p=mallocOriginal(size);--depth;if(t)t->Acquire(p,size);return p;
}
void* __cdecl Calloc(std::size_t n,std::size_t size) {
    auto* t=Active();const bool valid=!size || n<=(std::numeric_limits<std::size_t>::max)()/size;
    if(t && valid && t->Reject(n*size))return nullptr;
    ++depth;auto* p=callocOriginal(n,size);--depth;if(t && valid)t->Acquire(p,n*size);return p;
}
void __cdecl Free(void* p) {
    if(auto* t=Active())t->Release(p);++depth;freeOriginal(p);--depth;
}
struct Hooks {
    std::array<void*,3> targets{{reinterpret_cast<void*>(&std::malloc),reinterpret_cast<void*>(&std::calloc),reinterpret_cast<void*>(&std::free)}};
    std::array<bool,3> created{},enabled{};
    bool owns=false;
    bool Install() noexcept {
        const auto init=MH_Initialize();owns=init==MH_OK;
        if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
        void* detours[]{reinterpret_cast<void*>(&Malloc),reinterpret_cast<void*>(&Calloc),reinterpret_cast<void*>(&Free)};
        void** originals[]{reinterpret_cast<void**>(&mallocOriginal),reinterpret_cast<void**>(&callocOriginal),reinterpret_cast<void**>(&freeOriginal)};
        for(unsigned i=0;i<3;++i){if(MH_CreateHook(targets[i],detours[i],originals[i])!=MH_OK)return false;created[i]=true;}
        for(unsigned i=0;i<3;++i){if(MH_EnableHook(targets[i])!=MH_OK)return false;enabled[i]=true;}return true;
    }
    bool Remove() noexcept {
        active=nullptr;bool ok=true;
        for(unsigned i=0;i<3;++i)if(enabled[i]){const auto s=MH_DisableHook(targets[i]);
            if(s==MH_OK || s==MH_ERROR_DISABLED)enabled[i]=false;else ok=false;}
        for(unsigned i=0;i<3;++i)if(created[i]){if(MH_RemoveHook(targets[i])==MH_OK)created[i]=false;else ok=false;}
        if(owns){if(MH_Uninitialize()==MH_OK)owns=false;else ok=false;}return ok;
    }
    ~Hooks(){Remove();}
};
}
void Current(MMDApp& app,const Batch& batch,bool expected) {
    Check(batch && failAfter==-1 && !allocationAudit && !heap::active && !copy::activeTrace,
        "batch Current audit starts with no other injector active");
    heap::Hooks hooks;const bool installed=hooks.Install();heap::Trace trace;trace.observeOnly=true;
    bool current=false;long remaining=0;AllocationAudit audit;
    if(installed){heap::active=&trace;allocationAudit=&audit;failAfter=allowanceMax;
        current=plugins::FrameCopyBatchCurrent(app,*batch);remaining=failAfter;
        failAfter=-1;allocationAudit=nullptr;heap::active=nullptr;}
    const bool removed=hooks.Remove();
    Check(installed && removed && !heap::depth,"Current CRT observers are fully restored before assertions");
    Check(current==expected && remaining==allowanceMax && !trace.calls && !audit.outstanding && !audit.overflow,
        "batch Current returns the expected result with zero C++ malloc or calloc allocations");
}
struct Peer {
    Mode mode;
    frame_clipboard::CopyBatchState state;
    source::RawClip raw;
    Held held;
    Peer(BatchFixture& f,Mode active):mode(Other(active)),state(f.app->FrameClipboardCopy(mode)),raw(*f.app),
        held(Read(*f.app,mode,Status::Captured)) {}
    void Expect(BatchFixture& f) const {
        const auto& now=f.app->FrameClipboardCopy(mode);
        Check(Read(*f.app,mode,Status::Captured)==state.snapshot && now.attempt==state.attempt &&
            now.sceneGeneration==state.sceneGeneration && now.error==state.error && now.preparing==state.preparing,
            "opposite mode retains the exact published batch and state");
        const source::RawClip after(*f.app);
        const unsigned first=mode==Mode::Global?0:5,last=mode==Mode::Global?5:10;
        for(unsigned i=first;i<last;++i)Check(raw.pointers[i]==after.pointers[i] && raw.bytes[i]==after.bytes[i],
            "opposite native buffer pointers and nested bytes are preserved");
        for(unsigned i=first;i<(std::min)(last,8u);++i)Check(raw.counts[i]==after.counts[i],"peer native counts are preserved");
        if(mode==Mode::Global)Check(after.receipt.snapshot==raw.receipt.snapshot && after.receipt.attempt==raw.receipt.attempt &&
            after.receipt.status==raw.receipt.status && after.receipt.error==raw.receipt.error &&
            after.receipt.preparing==raw.receipt.preparing && after.receipt.sceneGeneration==raw.receipt.sceneGeneration,
            "model copy preserves the legacy global receipt as well as its owned batch");
        held.Expect();Current(*f.app,held.batch,true);
    }
};
void EmptyNative(BatchFixture& f,Mode mode) {
    if(mode==Mode::Global)copy::Empty(f);
    else {const auto& n=f.app->ClipboardCounts();
        Check(!n.bones && !n.morphs && !n.displays && !f.app->BoneClipboard() && !f.app->MorphClipboard() && !f.app->DisplayClipboard() &&
            frame_clipboard::BoneTarget(f.app.get(),f.model.raw(),0)==-1 && frame_clipboard::MorphTarget(f.app.get(),f.model.raw(),0)==-1,
            "active model failure clears all native buffers including nested display ownership and track maps");}
}
void NoPaste(BatchFixture& f) {
    f.controls.Expect(false,false);
    const auto window=f.app->MainWindow();f.app->MainWindow()=f.controls.host;
    nf::RefreshControls(f.app.get());f.app->MainWindow()=window;
    f.controls.Expect(false,false); // A leaked legacy negative clipboard re-enables paste.
    const auto cursor=f.app->CurrentFrame();f.app->CurrentFrame()=-500;
    const result::ModelLive before(f);const auto count=nf::Count(f.app.get());
    CmdControl400(f.app.get(),nullptr,421,0);before.Expect(f);
    Check(nf::Count(f.app.get())==count,"real negative 421 cannot paste a stale legacy clipboard after empty/failure");
    f.app->CurrentFrame()=cursor;
}
void Failed(BatchFixture& f,Mode mode,std::uint64_t attempt,MmsResult error) {
    Read(*f.app,mode,Status::Failed);const auto& state=f.app->FrameClipboardCopy(mode);
    Check(state.attempt==attempt && state.error==error && !state.preparing && !state.snapshot &&
        state.sceneGeneration==f.app->SceneGeneration(),"failed batch has exactly one attempt the exact error and no partial ownership");
    EmptyNative(f,mode);
    if(mode==Mode::Global) {
        receipt::Read(f,frame_clipboard::GlobalCopyStatus::Failed);
        Check(f.app->GlobalClipboardCopy().error==error && !f.app->GlobalClipboardCopy().preparing,
            "batch failure synchronizes the legacy global receipt error even after native publication");
    }
    NoPaste(f);
}
void Origins(BatchFixture& f,const Batch& batch,Mode mode,bool native,bool negative) {
    Check(batch && batch->ticket.owner==f.app.get() && batch->ticket.mode==mode &&
        batch->ticket.sceneGeneration==f.app->SceneGeneration() && batch->hasNative==native && batch->negative &&
        batch->negative->owner==f.app.get() && batch->negative->sceneGeneration==batch->ticket.sceneGeneration &&
        (batch->negative->mode==nf::CopySourceMode::Global)==(mode==Mode::Global),
        "both native and negative sources are owned by the same production 420 batch");
    const auto selected=source::Capture(*f.app);
    Check(source::SameSource(*batch->negative,*selected) && batch->negative->keys.empty()==!negative,
        "batch owns the complete raw selected negative source from this mode");
    const auto first=mode==Mode::Global?9u:11u;
    Check(batch->nativeFirst==(native?first:0u) && batch->first==(negative?-70:std::int64_t(first)),
        "nativeFirst is the true positive minimum; combined first uses negative minimum without a sentinel origin");
    if(native && negative)Check(batch->first==(std::min)(batch->negative->first,std::int64_t(batch->nativeFirst)),
        "signed batch first is exactly min(negative.first, nativeFirst)");
    if(native && mode==Mode::Global) {
        Check(batch->global && !batch->model && batch->global==f.app->GlobalClipboardCopy().snapshot,
            "global batch reuses the exact successful native receipt");receipt::MatchesRaw(f,*batch->global);copy::FullPayload(f);
    } else if(native) {
        Check(batch->model && !batch->global && batch->model->nativeFirst==first && batch->model->modelSlot==0,
            "model batch owns its native source and copy-time model slot");
        const auto& p=batch->model->payload;
        Check(p.bones.size()==1 && p.morphs.size()==1 && p.displays.size()==1 && !p.frameOffset &&
            p.bones[0].key.frame==18 && p.morphs[0].key.frame==6 && p.displays[0].frame==0 &&
            p.displays[0].ik.size()==1 && p.displays[0].selectors.size()==1,
            "model native records retain one shared relative origin and complete display children");
    } else Check(!batch->global && !batch->model,"negative-only batch has no fabricated native snapshot");
    Current(*f.app,batch,true);
}

void LifetimeAndModes(HistoryMode historyMode) {
    BatchFixture f(historyMode);
    Copy(f,Mode::Global);Copy(f,Mode::Model);
    for(auto mode:{Mode::Global,Mode::Model}) {
        const Peer peer(f,mode);auto original=Copy(f,mode);Origins(f,original,mode,true,true);const Held held(original);
        f.app->CurrentFrame()=-1234;f.SelectSources(false,false);f.app->SelectedModelSlot()=17;
        held.Expect();Current(*f.app,original,true);peer.Expect(f);
        f.app->SelectedModelSlot()=0;
        auto next=Copy(f,mode);Check(next!=original && next->ticket.attempt==original->ticket.attempt+1,
            "same-mode recopy replaces only its own batch");Current(*f.app,original,false);held.Expect();peer.Expect(f);
        auto positive=Copy(f,mode,true,false);Origins(f,positive,mode,true,false);peer.Expect(f);
        auto negative=Copy(f,mode,false,true);Origins(f,negative,mode,false,true);EmptyNative(f,mode);peer.Expect(f);
        const Held negativeHeld(negative);Copy(f,mode,false,false);EmptyNative(f,mode);NoPaste(f);
        Check(f.app->FrameClipboardCopy(mode).error==MMS_OK && !f.app->FrameClipboardCopy(mode).preparing,
            "actual empty 420 explicitly supersedes the active batch with successful Empty");
        Current(*f.app,negative,false);negativeHeld.Expect();held.Expect();peer.Expect(f);
        Copy(f,mode);
    }
    const Held global(Read(*f.app,Mode::Global,Status::Captured)),model(Read(*f.app,Mode::Model,Status::Captured));
    f.app->AdvanceSceneGeneration();
    Read(*f.app,Mode::Global,Status::Unavailable);Read(*f.app,Mode::Model,Status::Unavailable);
    Current(*f.app,global.batch,false);Current(*f.app,model.batch,false);global.Expect();model.Expect();
}

void AllocationFailures(HistoryMode historyMode,Mode mode,bool native,bool negative) {
    BatchFixture f(historyMode);Copy(f,Other(mode));Copy(f,mode);
    f.SelectSources(native,negative);
    const Peer peer(f,mode);
    // Warm reusable native scale storage. Each trial restores the exact same
    // successful selection/clipboard setup before taking its live-state oracle.
    result::Command(f);const auto measured=result::Command(f);
    Check(!measured.exhausted && (measured.spent>0 || (!native && !negative)),
        "measure the complete real 420 allocation path; truly empty copy needs no descriptor");
    if(!native && !negative) {
        const result::ModelLive before(f);const auto empty=result::Command(f,0);
        Check(!empty.spent && !empty.outstanding,"empty production copy succeeds without allocation");
        Read(*f.app,mode,Status::Empty);EmptyNative(f,mode);NoPaste(f);before.Expect(f);peer.Expect(f);return;
    }
    for(long allowance=0;allowance<measured.spent;++allowance) {
        // Always seed an old usable two-sided clipboard, even when the new
        // selection is native-only or empty. Failure must retire that old clip.
        const Held held(Copy(f,mode));f.SelectSources(native,negative);f.controls.EnableOld();
        const result::ModelLive before(f);const auto attempt=f.app->FrameClipboardCopy(mode).attempt+1;
        const auto failed=result::Command(f,allowance);
        Check(failed.exhausted && !failed.outstanding,"every measured 420 OOM releases new C++ allocations");
        before.Expect(f);Failed(f,mode,attempt,MMS_E_MEMORY);peer.Expect(f);held.Expect();Current(*f.app,held.batch,false);
    }
    auto recovered=Copy(f,mode,native,negative);
    if(recovered)Origins(f,recovered,mode,native,negative);
    else {EmptyNative(f,mode);NoPaste(f);}
    peer.Expect(f);
    std::printf("  frame batch OOM: mode=%u native=%u negative=%u stages=%ld\n",
        unsigned(mode),unsigned(native),unsigned(negative),measured.spent);
}
void RawAllocationFailures(HistoryMode historyMode) {
    BatchFixture f(historyMode);Copy(f,Mode::Model);Copy(f,Mode::Global);
    {
        const Peer peer(f,Mode::Global);
        for(unsigned family=0;family<5;++family) {
            const Held held(Copy(f,Mode::Global));const result::ModelLive before(f);
            const auto attempt=f.app->FrameClipboardCopy(Mode::Global).attempt+1;
            const auto trace=copy::Copy(f,int(family));copy::FailureTrace(trace,family);
            before.Expect(f);Failed(f,Mode::Global,attempt,MMS_E_MEMORY);held.Expect();Current(*f.app,held.batch,false);peer.Expect(f);
        }
    }
    Copy(f,Mode::Global);Copy(f,Mode::Model);const Peer peer(f,Mode::Model);
    for(unsigned family=0;family<5;++family) {
        const Held held(Copy(f,Mode::Model));const result::ModelLive before(f);
        const auto attempt=f.app->FrameClipboardCopy(Mode::Model).attempt+1;
        heap::Trace trace;trace.oldBone=f.app->BoneClipboard();trace.failStage=int(family);
        heap::Hooks hooks;const bool installed=hooks.Install();bool threw=false;
        const auto window=f.app->MainWindow();
        if(installed){f.app->MainWindow()=f.controls.host;heap::active=&trace;
            try{CmdControl400(f.app.get(),f.controls.host,420,0);}catch(...){threw=true;}
            heap::active=nullptr;f.app->MainWindow()=window;}
        const bool removed=hooks.Remove();
        Check(installed && removed && !threw && !heap::depth && trace.failures==1 && trace.stage==family,
            "model native OOM hits each consecutive bone morph display IK selector allocation exactly once");
        for(bool live:trace.live)Check(!live,"model native OOM releases every partially allocated raw buffer");
        before.Expect(f);Failed(f,Mode::Model,attempt,MMS_E_MEMORY);peer.Expect(f);held.Expect();Current(*f.app,held.batch,false);
    }
}

void ReferencesAndReset(HistoryMode historyMode) {
    BatchFixture f(historyMode);auto global=Copy(f,Mode::Global);auto model=Copy(f,Mode::Model);
    const Held globalHeld(global),modelHeld(model);
    mdl::AccessoryRecord replacement{};plugins::SceneObjectIdentity(&replacement);
    f.app->AccessorySlot(7)=&replacement;Current(*f.app,global,false);Current(*f.app,model,true);
    f.app->AccessorySlot(7)=&f.accessory[0];Current(*f.app,global,true);plugins::ForgetSceneObject(&replacement);
    const auto pointer=f.app->ModelSlot(0);f.app->ModelSlot(0)=nullptr;
    Current(*f.app,model,false);f.app->ModelSlot(0)=pointer;Current(*f.app,model,true);
    source::Parent replacementModel;f.app->ModelSlot(0)=replacementModel.model.raw();
    Current(*f.app,model,false);f.app->ModelSlot(0)=pointer;Current(*f.app,model,true);
    std::array<mdl::BoneRecord,2> bones=f.model.bones;f.model.m->boneTable=bones.data();
    Current(*f.app,model,false);f.model.m->boneTable=f.model.bones.data();Current(*f.app,model,true);
    plugins::ForgetSceneObject(pointer);plugins::SceneObjectIdentity(pointer);Current(*f.app,model,false);
    model=Copy(f,Mode::Model);Current(*f.app,model,true);

    plugins::ForgetSceneObject(pointer);model=Copy(f,Mode::Model);MmsObjectRef absent{};
    Check(model->model && !model->model->referencesAvailable && !plugins::FindSceneObjectIdentity(pointer,absent),
        "real model 420 captures raw data without creating a missing source identity");
    Current(*f.app,model,false);plugins::SceneObjectIdentity(pointer);Current(*f.app,model,false);
    model=Copy(f,Mode::Model);Current(*f.app,model,true);

    // Raw capture never repairs an absent identity or missing parent reference.
    plugins::ForgetSceneObject(&f.accessory[0]);global=Copy(f,Mode::Global);
    Check(global->global && global->negative,"missing identity still publishes the complete raw batch");Current(*f.app,global,false);
    plugins::SceneObjectIdentity(&f.accessory[0]);Current(*f.app,global,false);
    global=Copy(f,Mode::Global);Current(*f.app,global,true);
    f.model.m->displayKeys[0].selectorStates[0].modelIndex=19;
    model=Copy(f,Mode::Model);Check(model->model && !model->model->referencesAvailable,"missing native selector parent retains raw Captured source");
    Current(*f.app,model,false);f.model.m->displayKeys[0].selectorStates[0].modelIndex=-1;
    source::Parent parent;f.app->ModelSlot(19)=parent.model.raw();Current(*f.app,model,false);f.app->ModelSlot(19)=nullptr;
    // Only the negative source names this absent parent; native sources are valid.
    f.app->CameraParentModel()=19;f.app->CameraParentBone()=1;
    f.SelectMode(Mode::Global);Check(nf::RegisterGlobal(f.app.get(),nf::Global::Camera,-40),"seed missing negative-only parent");
    global=Copy(f,Mode::Global);Check(global->global && !global->negative->referencesAvailable,
        "raw negative missing reference is retained inside Captured");Current(*f.app,global,false);
    f.app->ModelSlot(19)=parent.model.raw();Current(*f.app,global,false);f.app->ModelSlot(19)=nullptr;
    f.app->CameraParentModel()=-1;Check(nf::RegisterGlobal(f.app.get(),nf::Global::Camera,-40),"restore negative parent for reset checks");
    global=Copy(f,Mode::Global);model=Copy(f,Mode::Model);
    const Held beforeResetGlobal(global),beforeResetModel(model);
    const auto globalAttempt=global->ticket.attempt,modelAttempt=model->ticket.attempt;
    nf::Reset(f.app.get());
    for(auto mode:{Mode::Global,Mode::Model}) {
        Batch out=mode==Mode::Global?global:model;
        Check(plugins::ReadFrameCopyBatch(*f.app,mode,out)!=Status::Captured && !out,
            "negative Reset explicitly invalidates both published mode batches");
    }
    Check(f.app->FrameClipboardCopy(Mode::Global).attempt==globalAttempt+1 && f.app->FrameClipboardCopy(Mode::Model).attempt==modelAttempt+1,
        "negative Reset advances both counters so old tickets and UI continuations cannot be reused");
    Current(*f.app,global,false);Current(*f.app,model,false);
    beforeResetGlobal.Expect();beforeResetModel.Expect();globalHeld.Expect();modelHeld.Expect();
    auto next=Copy(f,Mode::Model,true,false);Check(next->ticket.attempt==modelAttempt+2,"post-Reset copy advances the already retired counter");
}

void ExpectReset(BatchFixture& f,const Held& global,const Held& model) {
    for(const auto* held:{&global,&model}) {
        const auto mode=held->ticket.mode;const auto& state=f.app->FrameClipboardCopy(mode);
        Read(*f.app,mode,Status::Unavailable);
        Check(state.attempt==held->ticket.attempt+1 && !state.preparing && state.error==MMS_OK,
            "same-scene retirement advances each mode once and publishes Unavailable");
        Current(*f.app,held->batch,false);held->Expect();
    }
}
void ResetWithoutStateAndLoad(HistoryMode historyMode) {
    BatchFixture f(historyMode);nf::Reset(f.app.get());
    const Held global(Copy(f,Mode::Global,true,false)),model(Copy(f,Mode::Model,true,false));
    Check(!global.batch->negative->hadState && !model.batch->negative->hadState &&
        global.batch->negative->keys.empty() && model.batch->negative->keys.empty(),
        "both actual native-only copies were captured without any negative State");
    const result::ModelLive before(f);const source::RawClip raw(*f.app);
    nf::Reset(f.app.get());before.Expect(f);raw.Expect(*f.app);ExpectReset(f,global,model);
    Check(!source::Capture(*f.app)->hadState,"Reset and native-only capture do not materialize negative State");
    // Without explicit batch retirement these two independent sources would
    // still look current: their capture never observed a State lifetime token.
    Check(nf::CopySourceCurrent(*f.app,*global.batch->negative) && nf::CopySourceCurrent(*f.app,*model.batch->negative),
        "absent-State sources isolate the explicit batch retirement regression");
    plugins::FrameCopyTicket pendingGlobal,pendingModel;
    Check(plugins::BeginFrameCopy(*f.app,Mode::Global,pendingGlobal) &&
        plugins::BeginFrameCopy(*f.app,Mode::Model,pendingModel),"prepare both tickets before an absent-State Reset");
    nf::Reset(f.app.get());failAfter=0;
    const auto g=plugins::CaptureFrameCopyBatch(*f.app,pendingGlobal,9);
    const auto m=plugins::CaptureFrameCopyBatch(*f.app,pendingModel,11);const auto remaining=failAfter;failAfter=-1;
    Check(g==MMS_E_STALE && m==MMS_E_STALE && remaining==0 &&
        f.app->FrameClipboardCopy(Mode::Global).attempt==pendingGlobal.attempt+1 &&
        f.app->FrameClipboardCopy(Mode::Model).attempt==pendingModel.attempt+1,
        "Reset invalidates both in-flight tickets even with no negative State");
    for(auto mode:{Mode::Global,Mode::Model})f.app->FrameClipboardCopy(mode).attempt=UINT64_MAX;
    nf::Reset(f.app.get());
    for(auto mode:{Mode::Global,Mode::Model}) {
        Read(*f.app,mode,Status::Unavailable);
        Check(f.app->FrameClipboardCopy(mode).attempt==UINT64_MAX,"Reset saturates exhausted counters instead of wrapping");
    }

    BatchFixture loaded(historyMode);Copy(loaded,Mode::Global);Copy(loaded,Mode::Model);
    const Held oldGlobal(Read(*loaded.app,Mode::Global,Status::Captured)),oldModel(Read(*loaded.app,Mode::Model,Status::Captured));
    pmm_extension::Document document;nf::Capture(*loaded.app,document);
    Check(pmm_extension::Find(document,nf::kPmmKey)!=nullptr,"load control contains real serialized negative rows");
    const result::ModelLive live(loaded);const source::RawClip clips(*loaded.app);
    Check(nf::Load(*loaded.app,document),"successful real negative Load accepts its captured payload");
    live.Expect(loaded);clips.Expect(*loaded.app);ExpectReset(loaded,oldGlobal,oldModel);
    auto recovered=Copy(loaded,Mode::Model);Current(*loaded.app,recovered,true);
}

void ForgetSourceAndUnrelated(HistoryMode historyMode,bool native) {
    BatchFixture f(historyMode);Copy(f,Mode::Global);Copy(f,Mode::Model,native,true);
    source::Parent unrelated;f.app->ModelSlot(19)=unrelated.model.raw();
    const Peer globalPeer(f,Mode::Model);const Held model(Read(*f.app,Mode::Model,Status::Captured));
    const result::ModelLive before(f);const source::RawClip raw(*f.app);
    frame_clipboard::ForgetModel(unrelated.model.raw());before.Expect(f);raw.Expect(*f.app);
    Check(!model.batch->sourceRetired && !globalPeer.held.batch->sourceRetired &&
        Read(*f.app,Mode::Model,Status::Captured)==model.batch,"forgetting an unreferenced model keeps both owned batches available");
    Current(*f.app,model.batch,true);globalPeer.Expect(f);
    frame_clipboard::ForgetModel(f.model.raw());before.Expect(f);raw.Expect(*f.app);
    Check(Read(*f.app,Mode::Model,Status::Captured)==model.batch && model.batch->sourceRetired,
        "ForgetModel source retires eligibility while preserving readable Captured ownership");
    Current(*f.app,model.batch,false);model.Expect();globalPeer.Expect(f);
    Check((!model.batch->model || plugins::ModelCopySnapshotCurrent(*f.app,*model.batch->model)) &&
        nf::CopySourceCurrent(*f.app,*model.batch->negative),
        "source Forget is observed even while the exact source slot identity and tables still exist");
    auto recovered=Copy(f,Mode::Model,native,true);Check(!recovered->sourceRetired,"explicit recopy creates a new non-retired source batch");
    Current(*f.app,recovered,true);Current(*f.app,model.batch,false);globalPeer.Expect(f);f.app->ModelSlot(19)=nullptr;
}
void ForgetParent(HistoryMode historyMode,Mode mode,bool nativeReference) {
    BatchFixture f(historyMode);source::Parent parent;f.app->ModelSlot(19)=parent.model.raw();
    f.SelectMode(mode);
    if(mode==Mode::Global) {
        if(nativeReference){f.app->CameraKeys()[1].parentModel=19;f.app->CameraKeys()[1].parentBone=1;}
        else {f.app->CameraParentModel()=19;f.app->CameraParentBone()=1;
            Check(nf::RegisterGlobal(f.app.get(),nf::Global::Camera,-40),"seed parent only in negative camera source");}
    } else {
        if(nativeReference){f.model.m->displayKeys[0].selectorStates[0].modelIndex=19;f.model.m->displayKeys[0].selectorStates[0].boneIndex=1;}
        else {f.selectors[0].linkedModel=19;f.selectors[0].linkedBone=1;
            Check(nf::RegisterDisplay(f.model.raw(),-30),"seed parent only in negative selector source");}
    }
    Copy(f,Other(mode));const Held held(Copy(f,mode));const Peer peer(f,mode);Current(*f.app,held.batch,true);
    const bool nativeMentions=mode==Mode::Global?held.batch->global->modelReferences[19].referenced:held.batch->model->models[19].referenced;
    Check(nativeMentions==nativeReference && held.batch->negative->models[19].referenced==!nativeReference,
        "Forget parent witness isolates the native or negative half of the batch");
    const result::ModelLive before(f);const source::RawClip raw(*f.app);const auto parentDigest=parent.model.Digest();
    frame_clipboard::ForgetModel(parent.model.raw());before.Expect(f);raw.Expect(*f.app);
    Check(parent.model.Digest()==parentDigest && Read(*f.app,mode,Status::Captured)==held.batch && held.batch->sourceRetired,
        "forgetting a referenced parent retires only batch eligibility without deleting source bytes or live objects");
    Current(*f.app,held.batch,false);held.Expect();peer.Expect(f);
    Check(nf::CopySourceCurrent(*f.app,*held.batch->negative) &&
        (mode==Mode::Global?plugins::GlobalCopySnapshotCurrent(*f.app,*held.batch->global):
            plugins::ModelCopySnapshotCurrent(*f.app,*held.batch->model)),
        "parent retirement is explicit even before its live slot or identity disappears");
    auto fresh=Copy(f,mode);Check(!fresh->sourceRetired,"new explicit copy can use the still-live parent");
    Current(*f.app,fresh,true);Current(*f.app,held.batch,false);peer.Expect(f);f.app->ModelSlot(19)=nullptr;
}

void Protocol(HistoryMode historyMode,Mode mode) {
    BatchFixture f(historyMode);Copy(f,Other(mode));auto original=Copy(f,mode);const Peer peer(f,mode);const Held held(original);
    plugins::FrameCopyTicket old,next;
    Check(plugins::BeginFrameCopy(*f.app,mode,old) && plugins::FrameCopyPreparing(*f.app,old),"begin issues one preparation ticket");
    Check(plugins::BeginFrameCopy(*f.app,mode,next) && next.attempt==old.attempt+1,"a newer begin supersedes a pending ticket");
    failAfter=0;
    const auto stale=plugins::CaptureFrameCopyBatch(*f.app,old,mode==Mode::Global?9:11);
    const bool failed=plugins::FailFrameCopy(*f.app,old,MMS_E_MEMORY);const long remaining=failAfter;failAfter=-1;
    Check(stale==MMS_E_STALE && !failed && remaining==0 && plugins::FrameCopyPreparing(*f.app,next),
        "stale capture and failure cannot consume or retire the newer preparation and allocate nothing");
    auto wrongOwner=next;wrongOwner.owner=nullptr;auto wrongMode=next;wrongMode.mode=Other(mode);
    failAfter=0;
    const auto ownerCode=plugins::CaptureFrameCopyBatch(*f.app,wrongOwner,9);
    const auto modeCode=plugins::CaptureFrameCopyBatch(*f.app,wrongMode,9);const auto left=failAfter;failAfter=-1;
    Check(ownerCode==MMS_E_STALE && modeCode==MMS_E_STALE && left==0 && plugins::FrameCopyPreparing(*f.app,next),
        "wrong-owner and wrong-mode tickets cannot steal the active preparation permit");
    if(mode==Mode::Global) {
        Check(next.previousGlobalAttempt==f.app->GlobalClipboardCopy().attempt &&
            plugins::CaptureFrameCopyBatch(*f.app,next,9)==MMS_E_STALE,
            "a fresh frame ticket cannot reuse the previous successful native global receipt");
        Check(plugins::BeginFrameCopy(*f.app,mode,next) && plugins::BeginGlobalCopy(*f.app) &&
            plugins::CaptureGlobalCopySnapshot(*f.app,9)==MMS_OK,
            "direct protocol control prepares exactly one fresh native receipt after its frame ticket");
    }
    Check(plugins::CaptureFrameCopyBatch(*f.app,next,mode==Mode::Global?9:11)==MMS_OK,
        "current ticket freezes its prepared native source exactly once");
    auto published=Read(*f.app,mode,Status::Captured);Current(*f.app,published,true);
    Check(plugins::CaptureFrameCopyBatch(*f.app,next,mode==Mode::Global?9:11)==MMS_E_STALE &&
        !plugins::FailFrameCopy(*f.app,next,MMS_E_FAILED),"a completed ticket cannot capture twice or fail an already published batch");
    Current(*f.app,original,false);held.Expect();peer.Expect(f);
    // Publication-envelope corruption, without mutating any const held payload.
    auto& state=f.app->FrameClipboardCopy(mode);const auto saved=state;
    for(unsigned kind=0;kind<7;++kind) {
        state=saved;
        switch(kind) {
        case 0:++state.attempt;break;
        case 1:++state.sceneGeneration;break;
        case 2:state.error=MMS_E_MEMORY;break;
        case 3:state.preparing=true;break;
        case 4:state.snapshot.reset();break;
        case 5:state.snapshot=f.app->FrameClipboardCopy(Other(mode)).snapshot;break;
        case 6:state.status=Status::Empty;break;
        }
        Batch out=published;
        Check(plugins::ReadFrameCopyBatch(*f.app,mode,out)==(kind==6?Status::Empty:Status::Failed) && !out,
            "incoherent envelope rejects and clears a prepopulated caller output");Current(*f.app,published,false);
    }
    state=saved;Current(*f.app,published,true);
    auto impostor=std::make_shared<plugins::FrameCopyBatch>(*published);Current(*f.app,impostor,false);
    auto foreign=std::make_unique<MMDApp>();Current(*foreign,published,false);
    state.attempt=UINT64_MAX;const result::ModelLive before(f);
    const auto exhausted=result::Command(f,0);before.Expect(f);
    Check(!exhausted.spent && !exhausted.outstanding,"exhausted production batch counter rejects before allocation");
    Failed(f,mode,UINT64_MAX,MMS_E_NOT_READY);peer.Expect(f);held.Expect();
    plugins::FrameCopyTicket denied=next;
    Check(!plugins::BeginFrameCopy(*f.app,mode,denied) && !denied.owner && !denied.attempt &&
        !plugins::FrameCopyPreparing(*f.app,next) && !plugins::FailFrameCopy(*f.app,next,MMS_E_MEMORY),
        "counter exhaustion clears output ticket and never wraps or revives a preparation permit");
}

void InvalidPreparation(HistoryMode historyMode) {
    BatchFixture f(historyMode);Copy(f,Mode::Model);auto held=Copy(f,Mode::Global);const Peer peer(f,Mode::Global);
    plugins::FrameCopyTicket ticket;
    Check(plugins::BeginFrameCopy(*f.app,Mode::Global,ticket),"begin global preparation for mode coherence checks");
    f.SelectMode(Mode::Model);failAfter=0;
    const auto wrongMode=plugins::CaptureFrameCopyBatch(*f.app,ticket,9);const auto remaining=failAfter;failAfter=-1;
    Check(wrongMode==MMS_E_STALE && remaining==0 && !plugins::FrameCopyPreparing(*f.app,ticket),
        "a mode switch before capture rejects and consumes the old preparation without allocating");
    f.SelectMode(Mode::Global);
    Check(plugins::BeginFrameCopy(*f.app,Mode::Global,ticket) && plugins::BeginGlobalCopy(*f.app) &&
        plugins::CaptureGlobalCopySnapshot(*f.app,9)==MMS_OK &&
        plugins::CaptureFrameCopyBatch(*f.app,ticket,10)==MMS_E_STALE,
        "native origin inconsistent with the successful global receipt cannot be published");
    Read(*f.app,Mode::Global,Status::Failed);peer.Expect(f);
    held=Copy(f,Mode::Global);const Held saved(held);
    f.app->GlobalClipboardCopy().attempt=UINT64_MAX;
    const auto attempt=f.app->FrameClipboardCopy(Mode::Global).attempt+1;
    const result::ModelLive before(f);const auto failed=result::Command(f,0);before.Expect(f);
    Check(!failed.spent && !failed.outstanding && f.app->GlobalClipboardCopy().attempt==UINT64_MAX,
        "exhausted legacy receipt counter rejects inside real 420 without wrapping or allocating");
    Failed(f,Mode::Global,attempt,MMS_E_NOT_READY);saved.Expect();Current(*f.app,held,false);peer.Expect(f);
}

enum class ReentryAction { SwitchMode, CopyAgain, Reset };
void MissingModelAdmission(HistoryMode historyMode) {
    for(bool invalidSlot:{false,true}) {
        BatchFixture f(historyMode);Copy(f,Mode::Global);const Peer peer(f,Mode::Model);
        const auto held=Copy(f,Mode::Model);const auto attempt=held->ticket.attempt+1;
        const auto undo=f.history.UndoCount(),redo=f.history.RedoCount(),count=nf::Count(f.app.get());
        if(invalidSlot)f.app->SelectedModelSlot()=255;else f.app->ModelSlot(0)=nullptr;
        const auto failed=result::Command(f,0);
        Check(!failed.spent && !failed.outstanding,"missing or out-of-range production model rejects before allocation");
        f.app->SelectedModelSlot()=0;f.app->ModelSlot(0)=f.model.raw();
        Failed(f,Mode::Model,attempt,MMS_E_NOT_READY);Current(*f.app,held,false);peer.Expect(f);
        Check(f.history.UndoCount()==undo && f.history.RedoCount()==redo && nf::Count(f.app.get())==count,
            "invalid model copy leaves all authored rows and history intact");
    }
}
struct Reentry {
    BatchFixture* fixture=nullptr;
    WNDPROC original=nullptr;
    ReentryAction action=ReentryAction::SwitchMode;
    Mode mode=Mode::Global;
    Batch atCallback,nested;
    std::unique_ptr<Held> saved;
    bool armed=false,fired=false,complete=false,threw=false;
};
thread_local Reentry* reentry=nullptr;
LRESULT CALLBACK PasteProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* r=reentry;
    if(r && message==WM_ENABLE && r->armed) {
        r->armed=false;r->fired=true;
        try {
            auto& f=*r->fixture;
            const auto status=plugins::ReadFrameCopyBatch(*f.app,r->mode,r->atCallback);
            const auto& b=r->atCallback;
            r->complete=status==Status::Captured && b && b->negative && !b->negative->keys.empty() && b->hasNative &&
                b->first==-70 && b->nativeFirst==(r->mode==Mode::Global?9u:11u) &&
                (r->mode==Mode::Global?bool(b->global):bool(b->model)) && plugins::FrameCopyBatchCurrent(*f.app,*b);
            if(b)r->saved=std::make_unique<Held>(b);
            if(r->action==ReentryAction::Reset)nf::Reset(f.app.get());
            else if(r->action==ReentryAction::SwitchMode)f.SelectMode(Other(r->mode));
            else {
                CmdControl400(f.app.get(),f.controls.host,420,0);
                plugins::ReadFrameCopyBatch(*f.app,r->mode,r->nested);
            }
        } catch(...) {r->threw=true;}
    }
    return CallWindowProcW(r->original,window,message,wparam,lparam);
}
void UiReentry(HistoryMode historyMode,Mode mode,ReentryAction action,bool lastAttempt=false) {
    BatchFixture f(historyMode);Copy(f,Other(mode));Copy(f,mode);
    const Held peer(Read(*f.app,Other(mode),Status::Captured));
    if(lastAttempt)f.app->FrameClipboardCopy(mode).attempt=UINT64_MAX-1;
    const auto attempt=f.app->FrameClipboardCopy(mode).attempt+1;
    EnableWindow(f.controls.paste,FALSE);
    EnableWindow(f.controls.reverse,mode==Mode::Global?TRUE:FALSE);
    EnableMenuItem(f.controls.menu,0xFA,MF_BYCOMMAND|MF_GRAYED);
    Reentry probe;probe.fixture=&f;probe.mode=mode;probe.action=action;
    probe.original=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(f.controls.paste,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&PasteProc)));
    Check(probe.original!=nullptr,"subclass only the invisible fixture paste button for synchronous WM_ENABLE reentry");
    reentry=&probe;probe.armed=true;const auto window=f.app->MainWindow();f.app->MainWindow()=f.controls.host;
    bool threw=false;try{CmdControl400(f.app.get(),f.controls.host,420,0);}catch(...){threw=true;}
    f.app->MainWindow()=window;probe.armed=false;
    const auto restored=SetWindowLongPtrW(f.controls.paste,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(probe.original));reentry=nullptr;
    Check(restored && !threw && !probe.threw && probe.fired && probe.complete,
        "first WM_ENABLE callback sees a fully frozen and published two-sided batch");
    Check(probe.atCallback->ticket.attempt==attempt,"UI callback belongs to the just-published outer attempt");
    if(action!=ReentryAction::CopyAgain) {
        const auto menu=GetMenuState(f.controls.menu,0xFA,MF_BYCOMMAND);
        Check(bool(IsWindowEnabled(f.controls.reverse))==(mode==Mode::Global) && menu!=UINT(-1) &&
            (menu&(MF_DISABLED|MF_GRAYED)),
            "mode change or Reset inside WM_ENABLE prevents all later outer reverse-button and menu updates");
    }
    if(action==ReentryAction::Reset) {
        for(auto selected:{mode,Other(mode)}) {
            Batch out=probe.atCallback;
            Check(plugins::ReadFrameCopyBatch(*f.app,selected,out)!=Status::Captured && !out,
                "outer 420 cannot resurrect either batch invalidated inside WM_ENABLE");
        }
        Check(f.app->FrameClipboardCopy(mode).attempt==(lastAttempt?UINT64_MAX:attempt+1) && f.app->FrameClipboardCopy(Other(mode)).attempt==peer.ticket.attempt+1,
            "reentrant Reset advances both counters without outer publication afterward");
        Current(*f.app,probe.atCallback,false);Current(*f.app,peer.batch,false);
    } else {
        Check(Read(*f.app,Other(mode),Status::Captured)==peer.batch,"UI reentry preserves the opposite mode's owned batch");
        if(action==ReentryAction::CopyAgain) {
            Check(probe.nested && probe.nested!=probe.atCallback && probe.nested->ticket.attempt==attempt+1 &&
                Read(*f.app,mode,Status::Captured)==probe.nested,"nested real 420 remains published after outer UI callbacks return");
            Current(*f.app,probe.atCallback,false);Current(*f.app,probe.nested,true);
        } else {
            Check(f.Active()==Other(mode) && Read(*f.app,mode,Status::Captured)==probe.atCallback,
                "UI mode switch cannot mix a later timeline into the frozen outer copy");Current(*f.app,probe.atCallback,true);
        }
        Current(*f.app,peer.batch,true);
    }
    Check(bool(probe.saved),"UI callback retained the complete source before performing its reentrant action");
    probe.saved->Expect();peer.Expect();
}
} // namespace frame_copy_batch_checks

void NativeFrameCopyBatchChecks() {
    using namespace frame_copy_batch_checks;
    for(auto historyMode:{HistoryMode::AllOperations,HistoryMode::ExplicitOnly}) {
        LifetimeAndModes(historyMode);ReferencesAndReset(historyMode);RawAllocationFailures(historyMode);InvalidPreparation(historyMode);
        ResetWithoutStateAndLoad(historyMode);
        MissingModelAdmission(historyMode);
        for(bool native:{true,false})ForgetSourceAndUnrelated(historyMode,native);
        for(auto mode:{Mode::Global,Mode::Model}) {
            for(bool nativeReference:{true,false})ForgetParent(historyMode,mode,nativeReference);
            AllocationFailures(historyMode,mode,true,true);
            AllocationFailures(historyMode,mode,true,false);
            AllocationFailures(historyMode,mode,false,true);
            AllocationFailures(historyMode,mode,false,false);
            Protocol(historyMode,mode);
            for(auto action:{ReentryAction::SwitchMode,ReentryAction::CopyAgain,ReentryAction::Reset})UiReentry(historyMode,mode,action);
            UiReentry(historyMode,mode,ReentryAction::Reset,true);
        }
    }
}
} // namespace
