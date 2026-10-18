#include "../src/plugins/native_model_paste.hpp"
namespace {
struct ModelPasteFixture : DisplayPasteFixture {
    std::vector<mdl::BoneClipboardRecord> boneClipboard;
    std::vector<mdl::MorphClipboardRecord> morphClipboard;
    explicit ModelPasteFixture(unsigned records=3) {
        for(auto* f:{&first,&second})for(auto& bone:f->bones)bone.type=mdl::BoneType::RotateMove;
        for(unsigned i=0;i<records;++i) {
            mdl::BoneClipboardRecord bone{};std::memcpy(bone.name,first.bones[i%2].name,20);
            bone.frame=i==records-1?0:i*7;bone.position[0]=3+float(i);bone.position[1]=-.5f*float(i);bone.rotation[3]=1;
            for(unsigned lane=0;lane<4;++lane){bone.interpolation[lane]=bone.interpolation[lane+4]=20;bone.interpolation[lane+8]=bone.interpolation[lane+12]=107;}
            boneClipboard.push_back(bone);
            bone_scale::CopyClipboard(app.get(),int(i),bone_scale::Matrix{},false,bone_scale::LinearCurve());
            mdl::MorphClipboardRecord morph{};std::memcpy(morph.name,first.morphs[0].name,20);morph.frame=bone.frame;morph.value=.1f+float(i%8)*.1f;morphClipboard.push_back(morph);
        }
        clipboard[2].frame=0;
        auto& entry=second.m->undoRings[0].slots[second.m->undoState[0]];
        entry.auxiliaryPose=undo_storage::MallocOrThrow(512*sizeof(mdl::BoneKeySnapshot));entry.dirty=0;mdl::BoneKeyVisitClear(*second.m);
        Bind(true);
    }
    ~ModelPasteFixture(){app->BoneClipboard()=nullptr;app->MorphClipboard()=nullptr;app->DisplayClipboard()=nullptr;app->ClipboardCounts()={};}
    void Bind(bool display) {
        app->BoneClipboard()=boneClipboard.data();app->MorphClipboard()=morphClipboard.data();app->DisplayClipboard()=display?clipboard.data():nullptr;
        app->ClipboardCounts()={};auto& n=app->ClipboardCounts();n.bones=unsigned(boneClipboard.size());n.morphs=unsigned(morphClipboard.size());if(display)n.displays=unsigned(clipboard.size());
    }
    void BindMask(unsigned mask) {
        Bind((mask&4u)!=0);
        if(!(mask&1u)){app->BoneClipboard()=nullptr;app->ClipboardCounts().bones=0;}
        if(!(mask&2u)){app->MorphClipboard()=nullptr;app->ClipboardCounts().morphs=0;}
    }
    void ClearSource(){app->BoneClipboard()=nullptr;app->MorphClipboard()=nullptr;app->DisplayClipboard()=nullptr;app->ClipboardCounts()={};boneClipboard.clear();morphClipboard.clear();clipboard={};ik={};parents={};}
    std::shared_ptr<plugins::HistoryChange> Plan(){std::shared_ptr<plugins::HistoryChange> out;Check(plugins::PlanModelKeyPaste(*app,0,out)==MMS_OK,"model clipboard aggregate plans privately");return out;}
    void Oracle() {
        for(std::size_t i=0;i<mdl::BoneKeyCapacity(second.m->boneKeys);++i)second.m->boneKeys[i].allocated=0;
        for(std::size_t i=0;i<mdl::MorphKeyCapacity(second.m->morphKeys);++i)second.m->morphKeys[i].allocated=0;
        for(std::size_t i=0;i<mdl::KeyPoolCapacity(second.m->displayKeys);++i)second.m->displayKeys[i].allocated=0;
        ResetBoneKeyCursor(second.raw());
        if(app->ClipboardCounts().bones)for(auto source:boneClipboard){bone_scale::Registration scale(bone_scale::Matrix{},bone_scale::LinearCurve());
            Check(RegisterBoneKey(second.raw(),reinterpret_cast<unsigned char*>(&source),app->CurrentFrame(),0),"legacy mixed bone source registers");}
        ResetMorphKeyCursor(second.raw());if(app->ClipboardCounts().morphs)for(const auto& source:morphClipboard)
            Check(RegisterMorphKeyFromRecord(second.raw(),reinterpret_cast<const unsigned char*>(&source),app->CurrentFrame()),"legacy mixed morph source registers");
        ResetDisplayKeyCursor(second.raw());
        if(app->ClipboardCounts().displays)for(const auto& source:clipboard)
            Check(RegisterDisplayKeyFromRecord(second.raw(),int(source.frame),source.visible,source.ikCount,reinterpret_cast<const unsigned char*>(source.ikStates),
                source.selectorCount,reinterpret_cast<const unsigned char*>(source.selectorStates),app->CurrentFrame()),"legacy mixed deep display source registers");
        DisplayLegacyFrame(second,unsigned(app->CurrentFrame()));
    }
};
void ModelPasteCommit(ModelPasteFixture& f,const std::shared_ptr<plugins::HistoryChange>& change) {
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
    Check(change&&f.history.PrepareNative("model mixed paste",change,token,plugins::SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,"all model paste candidates prepare together");
    Check(CommitRegistrationNoAllocation(f.history,std::move(token))==MMS_OK,"mixed model publication allocates nothing");
}
void ModelPasteParityChecks() {
    using Mode=plugins::SharedHistory::RecordingMode;
    for(unsigned mask:{3u,5u,6u,7u})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(unsigned physics:{0u,2u}) {
        std::printf("  model mixed parity families=%u mode=%u physics=%u\n",mask,unsigned(mode),physics);std::fflush(stdout);
        const bool display=(mask&4u)!=0;
        ModelPasteFixture f;f.BindMask(mask);f.first.m->physicsMode=f.second.m->physicsMode=static_cast<unsigned char>(physics);
        Check(f.history.SetMode(mode)==MMS_OK,"configure mixed model recording mode");
        const ModelKeyState before(f.first);const auto digest=f.first.Digest(),ring=PoseRingDigest(f.first);const auto last=f.app->LastRegisteredFrame();
        const auto dirty=f.first.bones[1];const auto dirtyScale=bone_scale::Pose(f.first.raw(),1);
        auto change=f.Plan();Check(change&&f.first.Digest()==digest&&before.Matches(f.first),"mixed model planning preserves all native storage");
        ModelPasteCommit(f,change);f.Oracle();
        Check(ModelKeyState(f.second).Matches(f.first)&&f.first.m->maxFrame==f.second.m->maxFrame&&f.first.m->searchCursor==f.second.m->searchCursor,
            "mixed model final keys all marks maximum and final display cursor match sequential legacy registrars");
        if(display)Check(DisplayFrameEqual(f.first,f.second),"mixed model deep IK and external-parent state matches target display frame");
        Check(PoseRingDigest(f.first)==ring&&f.app->LastRegisteredFrame()==last&&f.history.UndoCount()==1,
            "mixed model paste records one explicit step without changing old rings or registration marker");
        Check(!std::memcmp(&f.first.bones[1],&dirty,sizeof(dirty))&&bone_scale::Pose(f.first.raw(),1)==dirtyScale&&f.first.dirty[1],"mixed display and bone paste preserve dirty author pose and scale");
        if(display)Check(f.firstOrder[0].linkedModel==1&&f.firstOrder[0].linkedBone==1&&f.first.bones[0].trans[0]!=123,
            "clean bone and model-wide external parent are evaluated from the same final mixed target");
        const ModelKeyState after(f.first);const auto maxAfter=f.first.m->maxFrame;const auto cursorAfter=f.first.m->searchCursor;f.ClearSource();
        f.app->CurrentFrame()=83;f.app->SelectedModelSlot()=1;f.app->CameraMode()=1;
        Check(f.history.History(false)==MMS_OK&&before.Matches(f.first)&&f.first.m->searchCursor==5,"mixed model undo restores deep payloads marks and metadata after source destruction");
        Check(f.history.History(true)==MMS_OK&&after.Matches(f.first)&&f.first.m->maxFrame==maxAfter&&f.first.m->searchCursor==cursorAfter&&f.app->CurrentFrame()==83,
            "mixed model redo owns every source byte across model and playhead changes");
    }
}
void ModelPasteGrowthAndNoopChecks() {
    std::puts("  model mixed growth and no-op");std::fflush(stdout);
    ModelPasteFixture f(42);const ModelKeyState before(f.first);
    const auto boneId=mdl::BoneKeyPoolIdentity(f.first.m->boneKeys),morphId=mdl::MorphKeyPoolIdentity(f.first.m->morphKeys),displayId=mdl::KeyPoolIdentity(f.first.m->displayKeys);
    ModelPasteCommit(f,f.Plan());
    Check(mdl::BoneKeyCapacity(f.first.m->boneKeys)>32&&mdl::MorphKeyCapacity(f.first.m->morphKeys)>16&&mdl::KeyPoolCapacity(f.first.m->displayKeys)>4,
        "mixed paste privately grows all three full pools");
    Check(mdl::BoneKeyPoolIdentity(f.first.m->boneKeys)==boneId&&mdl::MorphKeyPoolIdentity(f.first.m->morphKeys)==morphId&&mdl::KeyPoolIdentity(f.first.m->displayKeys)==displayId,"three-family growth preserves all pool identities");
    const auto unchanged=f.first.Digest();auto same=f.Plan();Check(!same&&f.first.Digest()==unchanged&&f.history.UndoCount()==1,"identical mixed paste creates no mark-only history");
    Check(f.history.History(false)==MMS_OK&&before.Matches(f.first),"mixed grown pools undo original prefix and selection");
    f.Bind(false);for(auto& source:f.boneClipboard){std::memset(source.name,0,sizeof(source.name));std::memcpy(source.name,"unmatched",10);}
    for(auto& source:f.morphClipboard){std::memset(source.name,0,sizeof(source.name));std::memcpy(source.name,"unmatched",10);}
    const auto noMatch=f.first.Digest();same=f.Plan();Check(!same&&f.history.RedoCount()==1&&f.first.Digest()==noMatch,"all-unmatched two-family paste preserves redo cursor and selection");
}
void ModelPasteFailureChecks() {
    std::puts("  model mixed cancellation failures alias and staleness");std::fflush(stdout);
    {
        ModelPasteFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        ModelPasteCommit(f,f.Plan());const ModelKeyState after(f.first);
        Check(f.history.History(false)==MMS_OK,"seed mixed-family redo before destination name ambiguity");
        auto* oldMorphs=f.first.m->morphs;const auto oldCount=f.first.m->morphCount;
        std::array<mdl::MorphRecord,3> morphs{};morphs[0]=oldMorphs[0];morphs[1]=morphs[0];
        std::strcpy(morphs[2].name,"helper");
        f.first.m->morphs=morphs.data();f.first.m->morphCount=unsigned(morphs.size());
        const auto digest=f.first.Digest();const ModelKeyState before(f.first);
        CmdControl400(f.app.get(),f.app->MainWindow(),421,0);
        Check(f.first.Digest()==digest&&before.Matches(f.first)&&!f.history.UndoCount()&&f.history.RedoCount()==1,
            "late ambiguous morph target rejects the entire real mixed paste before bone or display publication");
        f.first.m->morphs=oldMorphs;f.first.m->morphCount=oldCount;
        Check(f.history.History(true)==MMS_OK&&after.Matches(f.first),
            "failed mixed destination validation preserves existing complete redo payload");
    }
    ModelPasteFixture f;const auto before=f.first.Digest();const ModelKeyState keys(f.first);std::shared_ptr<plugins::HistoryChange> change;
    f.clipboard[2].ikCount=1;f.clipboard[2].ikStates=nullptr;
    Check(plugins::PlanModelKeyPaste(*f.app,0,change)!=MMS_OK&&!change&&f.first.Digest()==before&&keys.Matches(f.first),"malformed late deep participant cancels bone and morph plans");
    f.clipboard[2].ikCount=3;f.clipboard[2].ikStates=f.ik[2].data();
    auto* original=f.first.m->displayKeys[2].ikStates;f.first.m->displayKeys[2].ikStates=reinterpret_cast<unsigned char*>(f.first.m->morphKeys);
    Check(plugins::PlanModelKeyPaste(*f.app,0,change)!=MMS_OK&&!change,"cross-family nested payload alias rejects before any write");f.first.m->displayKeys[2].ikStates=original;
    bool success=false;unsigned failed=0;
    for(long allowance=0;allowance<4096;++allowance){failAfter=allowance;const auto result=plugins::PlanModelKeyPaste(*f.app,0,change);failAfter=-1;
        if(result==MMS_OK){success=bool(change);break;}++failed;Check(!change&&f.first.Digest()==before&&keys.Matches(f.first),"mixed plan allocation failure leaves every family intact");}
    Check(success&&failed>10,"mixed plan failure sweep reaches a complete aggregate");
    plugins::SharedHistory limited(f.storage,{},32,1);std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
    Check(limited.PrepareNative("budget",change,token)==MMS_E_MEMORY&&!token&&f.first.Digest()==before,"mixed deep payload budget refuses before publication");
    success=false;failed=0;
    for(long allowance=0;allowance<4096;++allowance){std::string label="mixed candidate";failAfter=allowance;
        const auto result=f.history.PrepareNative(std::move(label),change,token);failAfter=-1;
        if(result==MMS_OK){token.reset();success=true;break;}++failed;Check(!token&&f.first.Digest()==before&&keys.Matches(f.first),"late mixed candidate failure publishes no earlier family");}
    Check(success&&failed>10,"mixed candidate sweep reaches all participants and deep growth");
    ModelPasteCommit(f,change);std::unique_ptr<plugins::PreparedHistoryChange> old;
    Check(change->Prepare(false,old)==MMS_OK&&old&&old->IsCurrent(),"prepare mixed candidate before ABA");
    Check(f.history.History(false)==MMS_OK&&f.history.History(true)==MMS_OK&&!old->IsCurrent(),"mixed candidate rejects undo redo ABA");
    old.reset();Check(f.history.History(false)==MMS_OK,"seed mixed redo after failure checks");
    const auto stable=f.first.Digest();failAfter=0;const auto result=f.history.History(true);failAfter=-1;
    Check(result==MMS_E_MEMORY&&f.history.RedoCount()==1&&f.first.Digest()==stable,"mixed redo allocation failure preserves all data and journal cursor");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> stale;
    Check(f.history.PrepareNative("identity",change,stale)==MMS_OK,"prepare mixed aggregate before replacing a participant pool");
    auto* oldPool=f.first.m->morphKeys;auto* replacement=mdl::MorphKeyPoolCreate(mdl::MorphKeyCapacity(oldPool));
    std::memcpy(replacement,oldPool,mdl::MorphKeyCapacity(oldPool)*sizeof(*oldPool));f.first.m->morphKeys=replacement;
    const auto changedIdentity=f.first.Digest();
    Check(f.history.CommitNative(std::move(stale))==MMS_E_STALE&&f.first.Digest()==changedIdentity&&f.history.RedoCount()==1,
        "replaced morph pool identity cancels whole mixed publication without consuming redo");
    f.first.m->morphKeys=oldPool;mdl::MorphKeyPoolDestroy(replacement);
}
void ModelPasteZeroBoneChecks() {
    std::puts("  model mixed zero-bone destination");std::fflush(stdout);
    for(unsigned mask:{3u,5u,7u}) {
        ModelPasteFixture f;f.BindMask(mask);
        auto& model=*f.first.m;
        // Retain the registered empty bone allocation so Fixture::Digest can
        // inspect its known 32-slot storage. Remove all bone/IK/parent tables
        // from the model view, then restore them only after the journal dies.
        const auto count=model.boneCount,ikCount=model.ikChainCount,parentCount=model.boneOrderCount;
        auto* bones=model.boneTable;auto* ik=model.ikChains;auto* order=model.boneOrderTable;
        const auto boneKeys=RegistrationKeys(f.first);
        model.boneCount=0;model.boneTable=nullptr;model.ikChainCount=0;model.ikChains=nullptr;
        model.boneOrderCount=0;model.boneOrderTable=nullptr;
        std::memset(model.boneKeys,0,boneKeys.size()*sizeof(mdl::BoneKey));
        for(auto& source:f.clipboard){source.ikCount=source.selectorCount=0;source.ikStates=nullptr;source.selectorStates=nullptr;}
        for(auto& source:f.boneClipboard){std::memset(source.name,0,sizeof(source.name));std::memcpy(source.name,"unmatched",10);}
        const ModelKeyState before(f.first);const auto digest=f.first.Digest();
        auto change=f.Plan();Check(change&&f.first.Digest()==digest,"zero-bone mixed destination privately accepts unmatched bone source and remaining editable families");
        ModelPasteCommit(f,change);const ModelKeyState after(f.first);
        Check(f.history.UndoCount()==1&&f.history.History(false)==MMS_OK&&before.Matches(f.first),"zero-bone mixed paste commits and undoes without a bone participant");
        Check(f.history.History(true)==MMS_OK&&after.Matches(f.first),"zero-bone mixed paste redoes morph and display payloads");
        Check(f.history.History(false)==MMS_OK,"seed zero-bone mixed redo before invalid source");
        f.boneClipboard.back().frame=std::uint32_t(frame_limits::maximum);
        const auto stable=f.first.Digest();std::shared_ptr<plugins::HistoryChange> invalid;
        Check(plugins::PlanModelKeyPaste(*f.app,0,invalid)==MMS_E_ARGUMENT&&!invalid&&f.first.Digest()==stable&&before.Matches(f.first)&&f.history.RedoCount()==1,
            "unmatched zero-bone source still validates every relative frame before publishing other families");
        f.boneClipboard.back().frame=0;f.app->BoneClipboard()=nullptr;
        Check(plugins::PlanModelKeyPaste(*f.app,0,invalid)==MMS_E_ARGUMENT&&!invalid&&f.first.Digest()==stable&&before.Matches(f.first)&&f.history.RedoCount()==1,
            "zero-bone destination rejects a missing counted bone source without modifying model pools");
        f.app->BoneClipboard()=f.boneClipboard.data();change.reset();
        Check(f.history.Reset()==MMS_OK,"release zero-bone history before restoring fixture tables");f.history.Collect();
        model.boneCount=count;model.boneTable=bones;model.ikChainCount=ikCount;model.ikChains=ik;
        model.boneOrderCount=parentCount;model.boneOrderTable=order;
        std::memcpy(model.boneKeys,boneKeys.data(),boneKeys.size()*sizeof(mdl::BoneKey));
    }
}
void ModelPasteLaterGrowthChecks() {
    std::puts("  model mixed later growth selection ownership");std::fflush(stdout);
    ModelPasteFixture f(42);ModelPasteCommit(f,f.Plan());
    const auto boneSize=mdl::BoneKeyCapacity(f.first.m->boneKeys),morphSize=mdl::MorphKeyCapacity(f.first.m->morphKeys),displaySize=mdl::KeyPoolCapacity(f.first.m->displayKeys);
    const auto boneTail=boneSize+3,morphTail=morphSize+3,displayTail=displaySize+3;
    Check(mdl::BoneKeyPoolEnsure(*f.first.m,boneTail+1)&&mdl::MorphKeyPoolEnsure(*f.first.m,morphTail+1)&&mdl::DisplayKeyPoolEnsure(*f.first.m,displayTail+1),
        "later same-identity growth creates unrelated selection tails in every model pool");
    f.first.m->boneKeys[boneTail].allocated=21;f.first.m->morphKeys[morphTail].allocated=22;f.first.m->displayKeys[displayTail].allocated=23;
    for(bool forward:{false,true}) {
        Check(f.history.History(forward)==MMS_OK,"older mixed paste survives later same-identity growth");
        Check(f.first.m->boneKeys[boneTail].allocated==21&&f.first.m->morphKeys[morphTail].allocated==22&&f.first.m->displayKeys[displayTail].allocated==23,
            "older mixed paste leaves marks outside captured and pasted extents unchanged");
    }
}
void NativeModelPasteChecks(){ModelPasteParityChecks();ModelPasteGrowthAndNoopChecks();ModelPasteFailureChecks();ModelPasteZeroBoneChecks();ModelPasteLaterGrowthChecks();}
}
