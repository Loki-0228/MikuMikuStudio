// Included after the real 420 batch fixtures. Invokes the actual 421 route.
namespace {
namespace signed_paste_command_checks {
namespace batch = frame_copy_batch_checks;
namespace nf = negative_frames;
using Mode = batch::Mode;
using HistoryMode = batch::HistoryMode;

void PeerClipboard(HistoryMode mode) {
    batch::BatchFixture f(mode);
    const auto global=batch::Copy(f,Mode::Global,false,true);
    const auto model=batch::Copy(f,Mode::Model,false,true);
    Check(!global->hasNative && !model->hasNative && plugins::FrameCopyBatchCurrent(*f.app,*global) &&
        plugins::FrameCopyBatchCurrent(*f.app,*model),"both real negative-only copy receipts remain current");
    const auto camera=batch::source::Row(*global->negative,2,-40).values[0];
    const auto light=batch::source::Row(*global->negative,3,-70).values[0];
    f.SelectMode(Mode::Global);f.app->CurrentFrame()=-200;
    // Edit a copied source value after the other mode took over the side
    // clipboard. Paste must read the earlier camera and light receipt.
    f.app->CameraPositionX()=float(camera+20);
    Check(nf::RegisterGlobal(f.app.get(),nf::Global::Camera,-40),"edit the source after taking the copy receipt");
    const auto originalCount=nf::Count(f.app.get());
    const auto undo=f.history.UndoCount();
    const GlobalPasteKeys native(*f.app);
    const auto keys=batch::copy::NegativeRecord(*f.app,nf::kPmmKey);
    {
        const batch::result::ModelLive before(f);
        failAfter=0;CmdControl400(f.app.get(),nullptr,421,0);failAfter=-1;
        before.Expect(f);
    }
    CmdControl400(f.app.get(),nullptr,421,0);
    const auto selected=batch::source::Capture(*f.app);
    Check(nf::Count(f.app.get())==originalCount+2 && f.history.UndoCount()==undo+1 && native.Same(*f.app) &&
        batch::source::Row(*selected,2,-170).values[0]==camera && batch::source::Row(*selected,3,-200).values[0]==light,
        "421 uses its active-mode frozen values and shared timing and does not write native pools");
    const auto pasted=batch::copy::NegativeRecord(*f.app,nf::kPmmKey);
    CmdControl400(f.app.get(),nullptr,400,0);
    Check(f.history.RedoCount()==1 && native.Same(*f.app) && f.app->CurrentFrame()==-200 &&
        batch::copy::NegativeRecord(*f.app,nf::kPmmKey)==keys,"one undo removes both pasted tracks without moving the playhead");
    CmdControl400(f.app.get(),nullptr,401,0);
    Check(!f.history.RedoCount() && native.Same(*f.app) && batch::copy::NegativeRecord(*f.app,nf::kPmmKey)==pasted,
        "one redo restores both exact pasted rows");

    // A second destination exactly at frame zero must not publish the first
    // negative row, even though it would be individually admissible.
    f.app->CurrentFrame()=-30;
    {const batch::result::ModelLive before(f);CmdControl400(f.app.get(),nullptr,421,0);before.Expect(f);}
    Check(nf::Count(f.app.get())==originalCount+2,"cross-zero paste never partly writes the negative side");
    f.app->CurrentFrame()=-250;
    Check(f.history.Configure(mode,30,1)==MMS_OK,"shrink history below one paste record");
    {const batch::result::ModelLive before(f);CmdControl400(f.app.get(),nullptr,421,0);before.Expect(f);}
    Check(nf::Count(f.app.get())==originalCount+2,"insufficient shared history budget publishes no paste");
}

void StaleAndEmpty() {
    batch::BatchFixture f(HistoryMode::AllOperations);
    const auto receipt=batch::Copy(f,Mode::Model,false,true);
    const auto* original=f.model.raw();
    plugins::ForgetSceneObject(original);plugins::SceneObjectIdentity(original);
    Check(!plugins::FrameCopyBatchCurrent(*f.app,*receipt),"same-address replacement has a new object identity");
    f.app->CurrentFrame()=-200;
    {const batch::result::ModelLive before(f);CmdControl400(f.app.get(),nullptr,421,0);before.Expect(f);}
    Check(batch::Read(*f.app,Mode::Model,batch::Status::Captured)==receipt,"rejecting a stale paste does not rewrite the receipt");

    batch::Copy(f,Mode::Model,false,false);
    batch::Copy(f,Mode::Global,false,true);
    f.SelectMode(Mode::Model);f.app->CurrentFrame()=-300;
    {const batch::result::ModelLive before(f);CmdControl400(f.app.get(),nullptr,421,0);before.Expect(f);}
    Check(!batch::Read(*f.app,Mode::Model,batch::Status::Empty),"empty active mode cannot consume the other mode's clipboard");
    // A source captured at a shared origin must not be confused with a
    // different mode's side clipboard even when its native portion is empty.
    const auto replacement=batch::Copy(f,Mode::Model,false,true);
    batch::Copy(f,Mode::Global,false,false);
    Check(batch::Read(*f.app,Mode::Model,batch::Status::Captured)==replacement,
        "clearing the global clipboard leaves the model receipt available");
}

void MixedDestinationRefused(HistoryMode historyMode,Mode timeline) {
    batch::BatchFixture f(historyMode);
    const batch::Held peer(batch::Copy(f,batch::Other(timeline),false,true));
    f.SelectMode(timeline);f.SelectSources(false,true);
    const bool global=timeline==Mode::Global;
    if(global) {
        auto* keys=f.app->CameraKeys();
        keys[1].selected=1;
        Check(keys[0].frame==0 && keys[0].eye[0]!=keys[1].eye[0],
            "the native camera source differs from its frame-zero anchor");
    } else {
        f.model.m->boneKeys[0].frame=f.model.m->displayKeys[0].frame=0;
        auto* keys=f.model.m->morphKeys;
        keys[0].frame=0;keys[0].next=1;keys[0].value=.125f;
        keys[1].frame=17;keys[1].previous=0;keys[1].next=0;keys[1].allocated=1;keys[1].value=.625f;
    }
    const auto originalCount=nf::Count(f.app.get());
    const batch::result::ModelLive beforeCopy(f);
    CmdControl400(f.app.get(),nullptr,420,0);beforeCopy.Expect(f);
    const batch::Held active(batch::Read(*f.app,timeline,batch::Status::Captured));
    const auto& receipt=*active.batch;
    Check(receipt.hasNative && receipt.negative && receipt.first==-70 &&
        receipt.nativeFirst==(global?21u:17u) && f.history.RedoCount()==1 &&
        plugins::FrameCopyBatchCurrent(*f.app,receipt) && plugins::FrameCopyBatchCurrent(*f.app,*peer.batch),
        "real 420 produces a current mixed receipt with a separate native origin and pending redo");
    if(global) {
        Check(receipt.global && receipt.global->payload.cameras.size()==1 &&
            receipt.global->payload.cameras[0].frame==0 &&
            receipt.global->payload.cameras[0].eye[0]!=f.app->CameraKeys()[0].eye[0],
            "global receipt would overwrite the camera anchor at zero");
    } else {
        Check(receipt.model && receipt.model->payload.morphs.size()==1 &&
            receipt.model->payload.morphs[0].key.frame==0 &&
            receipt.model->payload.morphs[0].key.value!=f.model.m->morphKeys[0].value,
            "model receipt would overwrite the morph anchor at zero");
    }
    const auto& negativeSource=global ? batch::source::Row(*receipt.negative,2,-40) :
        batch::source::Row(*receipt.negative,0,-55,true);
    const batch::source::RawClip raw(*f.app);
    for(const int nativeDestination:{0,1}) {
        const auto nativeOffset=std::int64_t(receipt.nativeFirst)-receipt.first;
        f.app->CurrentFrame()=int(nativeDestination-nativeOffset);
        std::int32_t negativeFrame=0,nativeFrame=-1;
        Check(plugins::ResolveSignedPasteFrame(f.app->CurrentFrame(),negativeSource.frame-receipt.first,negativeFrame) &&
            negativeFrame<0 && plugins::ResolveSignedPasteFrame(f.app->CurrentFrame(),nativeOffset,nativeFrame) &&
            nativeFrame==nativeDestination,"a single copied track would span a negative row and native zero or one");
        const batch::result::ModelLive before(f);
        const auto light=batch::copy::Bytes(&f.app->SceneLight(),sizeof(D3DLIGHT9));
        std::shared_ptr<plugins::HistoryChange> nativePlan;
        // Probe the existing detached paste planner, not a legacy registrar or
        // a temporary playhead seek. Even with rebased frames it refuses this
        // negative playhead. Remove this diagnostic when a signed planner exists.
        if(global) {
            auto source=receipt.global->payload;source.frameOffset=unsigned(nativeDestination);
            Check(plugins::PlanGlobalKeyPaste(*f.app,source,nativePlan)==MMS_E_ARGUMENT && !nativePlan,
                "global native paste planner does not prepare at a negative real playhead");
        } else {
            auto source=receipt.model->payload;source.frameOffset=unsigned(nativeDestination);
            Check(plugins::PlanModelKeyPaste(*f.app,0,source,nativePlan)==MMS_E_ARGUMENT && !nativePlan,
                "model native paste planner does not prepare at a negative real playhead");
        }
        before.Expect(f);
        CmdControl400(f.app.get(),nullptr,421,0);before.Expect(f);raw.Expect(*f.app);
        Check(light==batch::copy::Bytes(&f.app->SceneLight(),sizeof(D3DLIGHT9)) &&
            f.history.RedoCount()==1 && nf::Count(f.app.get())==originalCount &&
            batch::Read(*f.app,timeline,batch::Status::Captured)==active.batch &&
            batch::Read(*f.app,batch::Other(timeline),batch::Status::Captured)==peer.batch &&
            plugins::FrameCopyBatchCurrent(*f.app,receipt) && plugins::FrameCopyBatchCurrent(*f.app,*peer.batch),
            "mixed refusal keeps the playhead keys anchors history and both current copy receipts");
        active.Expect();peer.Expect();
    }
}

void FrozenTrackNames() {
    batch::BatchFixture f(HistoryMode::AllOperations);
    batch::source::Parent target;
    f.app->ModelSlot(1)=target.model.raw();
    f.SelectMode(Mode::Model);f.SelectSources(false,false);
    // Copy the second occurrence of a duplicate, then rename that source.
    std::strcpy(f.model.bones[0].name,"same");std::strcpy(f.model.bones[1].name,"same");
    std::strcpy(target.model.bones[0].name,"same");std::strcpy(target.model.bones[1].name,"same");
    f.model.bones[1].trans[0]=13.5f;
    Check(nf::RegisterBone(f.model.raw(),1,-70),"seed second occurrence with a distinct value");
    nf::SelectRange(f.app.get(),-70,-70);
    CmdControl400(f.app.get(),nullptr,420,0);
    const auto receipt=batch::Read(*f.app,Mode::Model,batch::Status::Captured);
    const auto& source=batch::source::Row(*receipt->negative,1,-70);
    Check(source.occurrence==1 && source.values[0]==13.5,"420 freezes the copied occurrence and payload");
    std::strcpy(f.model.bones[1].name,"changed");
    f.app->SelectedModelSlot()=1;f.app->CurrentFrame()=-90;
    const auto undo=f.history.UndoCount();
    CmdControl400(f.app.get(),nullptr,421,0);
    const auto pasted=batch::source::Capture(*f.app);
    Check(f.history.UndoCount()==undo+1 && pasted->keys.size()==1 &&
        batch::source::Row(*pasted,1,-90).values[0]==13.5 && target.model.bones[0].trans[0]==0,
        "cross-model 421 resolves the copied duplicate by frozen name and occurrence");
    f.history.Reset();f.history.Collect();nf::Reset(f.app.get());f.app->ModelSlot(1)=nullptr;
}
}

void NativeSignedPasteCommandChecks() {
    using namespace signed_paste_command_checks;
    for(auto mode:{HistoryMode::AllOperations,HistoryMode::ExplicitOnly}) {
        PeerClipboard(mode);
        MixedDestinationRefused(mode,Mode::Global);
        MixedDestinationRefused(mode,Mode::Model);
    }
    StaleAndEmpty();
    FrozenTrackNames();
}
}
