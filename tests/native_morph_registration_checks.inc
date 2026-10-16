// Include after native_morph_key_checks.inc; call NativeMorphRegistrationChecks().
// Compare private plans with the actual current-value registrar, not AtIndex.
namespace {
void MorphRegistrationMarks(Fixture& f) {
    f.m->boneKeys[0].allocated=2;f.m->boneKeys[31].allocated=3;
    f.m->morphKeys[1].allocated=4;f.m->morphKeys[9].allocated=5;
    f.m->displayKeys[0].allocated=6;f.m->displayKeys[3].allocated=7;
}
void MorphRegistrationClearMarks(Fixture& f) {
    for(std::size_t i=0;i<mdl::BoneKeyCapacity(f.m->boneKeys);++i)f.m->boneKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::MorphKeyCapacity(f.m->morphKeys);++i)f.m->morphKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::KeyPoolCapacity(f.m->displayKeys);++i)f.m->displayKeys[i].allocated=0;
}
std::shared_ptr<plugins::HistoryChange> MorphRegistrationPlan(MorphKeyFixture& f,int morph,int frame,unsigned slot=0) {
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanMorphRegistration(*f.app,slot,morph,frame,change)==MMS_OK&&change,
        "morph registration produces an owned key record");return change;
}
void MorphRegistrationCommit(MorphKeyFixture& f,const std::shared_ptr<plugins::HistoryChange>& change) {
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("morph registration",change,transaction)==MMS_OK,"morph registration journal prepares");
    Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,
        "morph registration publication and growth acceptance do not allocate");
}
void MorphRegistrationParity() {
    for(int morph:{0,1})for(int frame:{0,12,20,120}) {
        MorphKeyFixture f;MorphKeySeed(f.first);MorphKeySeed(f.second);
        for(auto* model:{&f.first,&f.second}) {model->m->physicsMode=2;MorphRegistrationMarks(*model);}
        f.app->CurrentFrame()=7;f.app->state.lastRegisteredFrame=13;
        const auto before=MorphKeysCopy(f.first);const auto ring=PoseRingDigest(f.first);
        const auto boneBefore=RegistrationKeys(f.first);const auto weights=f.firstMorphs;const auto bones=f.first.bones;
        const auto digest=f.first.Digest();auto change=MorphRegistrationPlan(f,morph,frame);
        Check(f.first.Digest()==digest&&change->RetainedBytes()>sizeof(mdl::MorphKey),
            "morph plan leaves keys marks current values metadata and ring untouched");
        MorphRegistrationCommit(f,change);
        MorphRegistrationClearMarks(f.second);RegisterMorphKeyCurrent(f.second.raw(),morph,frame);
        Check(!std::memcmp(f.first.m->morphKeys,f.second.m->morphKeys,16*sizeof(mdl::MorphKey))&&
            SameRegistrationKeys(f.first,RegistrationKeys(f.second)),
            "morph insert overwrite frame-zero and append match native keys and all-family mark clearing");
        Check(f.first.m->maxFrame==f.second.m->maxFrame&&f.first.m->searchCursor==5&&
            f.app->state.lastRegisteredFrame==(std::max)(13u,f.second.m->maxFrame)&&
            !std::memcmp(weights.data(),f.firstMorphs.data(),sizeof(weights))&&
            !std::memcmp(bones.data(),f.first.bones.data(),sizeof(bones))&&PoseRingDigest(f.first)==ring,
            "initial registration preserves all current poses and native ring while recording max and global last frame");
        for(unsigned i=0;i<4;++i)Check(f.first.m->displayKeys[i].allocated==0,"registration clears display marks");
        const auto after=MorphKeysCopy(f.first);
        f.app->CurrentFrame()=21;f.app->SelectedModelSlot()=1;
        std::memcpy(f.second.m->morphKeys,before.data(),before.size()*sizeof(mdl::MorphKey));SeekModelFrame(f.second.raw(),21,0);
        const auto other=f.secondMorphs;
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&SameRegistrationKeys(f.first,boneBefore)&&
            f.first.m->displayKeys[3].allocated==7&&f.first.m->maxFrame==99&&f.app->state.lastRegisteredFrame==13,
            "undo restores native keys three-family marks and scalar metadata across selected models");
        Check(MorphSameFloat(f.firstMorphs[morph].value,f.secondMorphs[morph].value)&&
            !std::memcmp(other.data(),f.secondMorphs.data(),sizeof(other)),"undo samples only the registered model at the new playhead");
        for(int i=0;i<3;++i)if(i!=morph)Check(MorphSameFloat(f.firstMorphs[i].value,weights[i].value),
            "undo does not overwrite another morph's unregistered current weight");
        f.app->CurrentFrame()=5;std::memcpy(f.second.m->morphKeys,after.data(),after.size()*sizeof(mdl::MorphKey));SeekModelFrame(f.second.raw(),5,0);
        Check(f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,after)&&
            MorphSameFloat(f.firstMorphs[morph].value,f.secondMorphs[morph].value),"redo samples target registration at the new playhead");
    }
    MorphKeyFixture f;MorphKeySeed(f.first);f.first.m->maxFrame=3;
    MorphRegistrationCommit(f,MorphRegistrationPlan(f,1,12));
    Check(f.first.m->maxFrame==3,"overwriting an existing key retains the native registrar's unchanged maximum quirk");
}
void MorphRegistrationCurvesAndNoOp() {
    MorphKeyFixture f;MorphKeySeed(f.first);MorphKeySeed(f.second);std::string error;
    const auto track=MorphAuthoredTrack(f.firstMorphs[1],1);
    Check(morph_curve::Write(f.first.raw(),{track},error)&&morph_curve::Write(f.second.raw(),{track},error),
        "morph registration seeds pristine custom tangents in independent oracle");
    f.firstMorphs[1].value=f.secondMorphs[1].value=.42f;f.app->CurrentFrame()=14;
    const auto before=MorphKeysCopy(f.first);morph_curve::AuthoredLayers layers;
    Check(morph_curve::CaptureLayers(f.first.raw(),layers,error),"capture pristine registration layers");
    auto change=MorphRegistrationPlan(f,1,18);MorphRegistrationCommit(f,change);
    Check(morph_curve::LayersUnchanged(f.first.raw(),layers)&&f.firstMorphs[1].value==.42f,
        "native morph registration preserves pristine effective mirror and observed layers until normal synchronization");
    MorphRegistrationClearMarks(f.second);RegisterMorphKeyCurrent(f.second.raw(),1,18);SeekModelFrame(f.second.raw(),28,0);
    animation_curve::Track derived;Check(morph_curve::Read(f.first.raw(),1,derived,error),"normal reader derives registered authored track");
    for(unsigned i=0;i<100;++i)PushBoneEditUndo(f.app.get());const auto ring=PoseRingDigest(f.first);
    f.app->CurrentFrame()=6;Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&
        MorphSameFloat(f.firstMorphs[1].value,float(animation_curve::Evaluate(track.track,6).value))&&PoseRingDigest(f.first)==ring,
        "undo after one hundred ring writes revives pristine curve at the current playhead");
    animation_curve::Track restored;Check(morph_curve::Read(f.first.raw(),1,restored,error)&&
        restored.keys[1].inWeight==.2&&restored.keys[0].outSlope==-.02,"undo preserves authored handles and tangents");
    const auto redo=f.history.RedoCount();f.firstMorphs[1].value=f.first.m->morphKeys[1].value;MorphRegistrationMarks(f.first);
    const auto noOp=f.first.Digest();std::shared_ptr<plugins::HistoryChange> empty;
    Check(plugins::PlanMorphRegistration(*f.app,0,1,0,empty)==MMS_OK&&!empty&&f.first.Digest()==noOp&&f.history.RedoCount()==redo,
        "same authored value ignores selection-only changes and preserves redo marks and current weight");
    f.app->CurrentFrame()=28;Check(f.history.History(true)==MMS_OK&&MorphSameFloat(f.firstMorphs[1].value,f.secondMorphs[1].value),
        "redo derives native edits from pristine morph authoring without rereading source files");
}
void MorphGrowthTokenChecks() {
    Fixture f;const auto* source=f.m->morphKeys;const auto identity=mdl::MorphKeyPoolIdentity(source);
    const auto before=MorphKeysCopy(f);std::unique_ptr<mdl::MorphKeyPoolGrowth> growth;
    Check(mdl::MorphKeyPoolGrowth::Prepare(*f.m,17,growth)&&growth->Grows()&&growth->IsCurrent(),"private morph growth prepares");
    const auto* candidate=growth->CandidatePool();const auto capacity=growth->CandidateCapacity();
    Check(candidate!=source&&growth->CandidateIdentity()==identity&&mdl::MorphKeyCapacity(candidate)==0&&
        mdl::MorphKeyPoolIdentity(candidate)==0&&f.m->morphKeys==source&&MorphKeysMatch(f,before),
        "private morph candidate remains invisible while the live owned allocation is unchanged");
    failAfter=0;growth.reset();failAfter=-1;
    Check(f.m->morphKeys==source&&mdl::MorphKeyCapacity(source)==16&&mdl::MorphKeyPoolIdentity(source)==identity,
        "cancelling a private morph allocation keeps source capacity and stable identity");
    Check(mdl::MorphKeyPoolGrowth::Prepare(*f.m,17,growth),"prepare morph publish rollback");
    failAfter=0;const bool published=growth->Publish();const bool current=growth->IsCurrent();const bool rolledBack=growth->Rollback();growth.reset();failAfter=-1;
    Check(published&&current&&rolledBack&&f.m->morphKeys==source&&MorphKeysMatch(f,before),
        "morph publication and rollback restore the old allocation without allocation");
    Check(mdl::MorphKeyPoolGrowth::Prepare(*f.m,17,growth),"prepare morph publish acceptance");
    failAfter=0;const bool acceptedPublish=growth->Publish();const bool accepted=growth->Accept();growth.reset();failAfter=-1;
    Check(acceptedPublish&&accepted&&f.m->morphKeys!=source&&mdl::MorphKeyCapacity(f.m->morphKeys)==capacity&&
        mdl::MorphKeyPoolIdentity(f.m->morphKeys)==identity&&MorphKeysMatch(f,before),"accepted morph growth retains stable identity and source bytes");
    Check(mdl::MorphKeyPoolGrowth::Prepare(*f.m,capacity+1,growth),"prepare morph stale replacement token");
    auto* old=f.m->morphKeys;auto* replacement=mdl::MorphKeyPoolCreate(16);f.m->morphKeys=replacement;
    Check(!growth->IsCurrent(),"replacement pool invalidates prepared morph growth");growth.reset();
    Check(f.m->morphKeys==replacement&&mdl::MorphKeyPoolIdentity(old)==identity,
        "stale private cancellation does not overwrite or destroy the replacement or untouched source");mdl::MorphKeyPoolDestroy(old);
}
void MorphRegistrationFullPool(Fixture& f) {
    std::memset(f.m->morphKeys,0,16*sizeof(mdl::MorphKey));unsigned previous=1;
    for(unsigned slot=3;slot<16;++slot) {
        auto& key=f.m->morphKeys[slot];key.frame=slot*10;key.value=float(slot)/20;
        key.previous=previous;key.reserved[1]=static_cast<unsigned char>(slot);f.m->morphKeys[previous].next=slot;previous=slot;
    }
    f.m->morphs[1].value=.91f;
}
void MorphRegistrationGrowthAndFailures() {
    MorphKeyFixture f;MorphRegistrationFullPool(f.first);f.app->CurrentFrame()=17;
    const auto before=MorphKeysCopy(f.first);const auto digest=f.first.Digest();const auto* pool=f.first.m->morphKeys;
    const auto identity=mdl::MorphKeyPoolIdentity(pool);bool planned=false;unsigned failures=0;
    std::shared_ptr<plugins::HistoryChange> change;
    for(long limit=0;limit<256;++limit) {
        failAfter=limit;const auto status=plugins::PlanMorphRegistration(*f.app,0,1,180,change);failAfter=-1;
        Check(f.first.Digest()==digest&&mdl::MorphKeyPoolIdentity(pool)==identity,"planning allocation failures preserve live model and pool registry");
        if(status==MMS_OK){planned=true;break;}Check(status==MMS_E_MEMORY&&!change,"morph plan allocation failure is explicit");++failures;
    }
    Check(planned&&change&&failures>5,"morph plan failure sweep reaches sparse record creation");
    bool prepared=false;failures=0;
    for(long limit=0;limit<256;++limit) {
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        failAfter=limit;const auto status=change->Prepare(true,candidate);failAfter=-1;
        Check(f.first.Digest()==digest&&f.first.m->morphKeys==pool&&mdl::MorphKeyCapacity(pool)==16,
            "private candidate allocation failures preserve pointer capacity keys and metadata");
        if(status==MMS_OK){prepared=true;candidate.reset();break;}
        Check(status==MMS_E_MEMORY&&!candidate,"morph candidate allocation failure reports memory");++failures;
    }
    Check(prepared&&failures>5,"candidate failure sweep reaches growth registry and layer capture");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> cancelled;
    Check(f.history.PrepareNative("cancel morph growth",change,cancelled)==MMS_OK,"prepare cancellable morph growth in journal");cancelled.reset();
    Check(f.first.Digest()==digest&&f.history.UndoCount()==0,"journal cancellation destroys private growth without publishing");
    MorphRegistrationCommit(f,change);const auto capacity=mdl::MorphKeyCapacity(f.first.m->morphKeys);
    Check(capacity>16&&f.first.m->morphKeys!=pool&&mdl::MorphKeyPoolIdentity(f.first.m->morphKeys)==identity&&
        f.first.m->morphKeys[16].frame==180&&f.first.m->morphKeys[16].value==.91f,
        "full owned morph pool grows privately and registers first added slot");
    const auto after=MorphKeysCopy(f.first);
    Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&mdl::MorphKeyCapacity(f.first.m->morphKeys)==capacity&&
        f.first.m->morphKeys[16].frame==0,"undo restores authored keys without shrinking accepted morph allocation");
    Check(f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,after),"redo uses retained keys without another growth");
    Check(f.history.History(false)==MMS_OK,"prepare redo preservation fixture");const auto redo=f.history.RedoCount();
    f.firstMorphs[1].value=.23f;auto next=MorphRegistrationPlan(f,1,200);
    std::string label="morph journal allocation failure";std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    failAfter=0;const auto status=f.history.PrepareNative(std::move(label),next,transaction);failAfter=-1;
    Check(status==MMS_E_MEMORY&&!transaction&&f.history.RedoCount()==redo&&MorphKeysMatch(f.first,before),
        "journal reservation failure retains redo and all current keys");
    plugins::SharedHistory tiny(f.storage,{},128,1);
    Check(tiny.PrepareNative("morph budget",next,transaction)==MMS_E_MEMORY&&!transaction&&f.history.RedoCount()==redo,
        "insufficient actual record budget refuses registration without consuming another journal's redo");
}
void MorphRegistrationStaleChecks() {
    for(unsigned kind=0;kind<7;++kind) {
        MorphKeyFixture f;MorphKeySeed(f.first);f.app->CurrentFrame()=8;
        auto change=MorphRegistrationPlan(f,1,20);std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_OK,"prepare morph registration freshness test");
        switch(kind) {
        case 0:f.firstMorphs[1].value+=1;break;
        case 1:f.first.m->boneKeys[30].allocated=1;break;
        case 2:f.first.m->morphKeys[15].allocated=1;break;
        case 3:f.first.m->displayKeys[3].allocated=1;break;
        case 4:++f.app->CurrentFrame();break;
        case 5:++f.first.m->searchCursor;break;
        default:++f.app->state.lastRegisteredFrame;break;
        }
        const auto edited=f.first.Digest();Check(!candidate->IsCurrent(),"prepared morph registration detects changed weight marks playhead and metadata");candidate.reset();
        Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate&&f.first.Digest()==edited,
            "first registration preparation rejects changed planning inputs without rollback of external changes");
    }
    MorphKeyFixture f;MorphKeySeed(f.first);std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanMorphRegistration(*f.app,0,0,10,change)==MMS_E_NOT_READY&&!change,
        "PMD base morph remains excluded from current-value registration");
    for(int morph:{-1,3})Check(plugins::PlanMorphRegistration(*f.app,0,morph,10,change)==MMS_E_ARGUMENT&&!change,
        "invalid morph index is rejected before native access");
    Check(plugins::PlanMorphRegistration(*f.app,0,1,-1,change)==MMS_E_ARGUMENT&&!change,"negative morph key frame rejected");
    f.app->PlaybackActive()=1;
    Check(plugins::PlanMorphRegistration(*f.app,0,1,10,change)==MMS_E_NOT_READY&&!change,"playback refuses private morph registration");
    f.app->PlaybackActive()=0;f.app->state.recordPlaybackStartPending=true;
    Check(plugins::PlanMorphRegistration(*f.app,0,1,10,change)==MMS_E_NOT_READY&&!change,"pending recording refuses private morph registration");
    f.app->state.recordPlaybackStartPending=false;
    f.first.m->morphKeys[6].next=6;const auto invalid=f.first.Digest();
    Check(plugins::PlanMorphRegistration(*f.app,0,1,20,change)==MMS_E_ARGUMENT&&!change&&f.first.Digest()==invalid,
        "malformed morph chain rejects before preparing native registration");
    MorphKeySeed(f.first);f.first.m->morphKeys[5].next=5;const auto badHole=f.first.Digest();
    Check(plugins::PlanMorphRegistration(*f.app,0,1,90,change)==MMS_E_ARGUMENT&&!change&&f.first.Digest()==badHole,
        "native append with a corrupt recycled slot is safely rejected before publication");
}
void MorphRegistrationCrossModel() {
    MorphKeyFixture f;MorphKeySeed(f.first);MorphKeySeed(f.second);
    const auto first=MorphKeysCopy(f.first),second=MorphKeysCopy(f.second);
    MorphRegistrationCommit(f,MorphRegistrationPlan(f,1,100));const auto firstAfter=MorphKeysCopy(f.first);
    f.app->SelectedModelSlot()=1;MorphRegistrationCommit(f,MorphRegistrationPlan(f,2,200,1));
    const auto secondAfter=MorphKeysCopy(f.second);f.app->CurrentFrame()=77;
    Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.second,second)&&MorphKeysMatch(f.first,firstAfter),
        "shared journal first reverses the later registered model");
    Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,first)&&MorphKeysMatch(f.second,second),
        "shared journal then reverses the earlier model regardless of current selection");
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&
        MorphKeysMatch(f.first,firstAfter)&&MorphKeysMatch(f.second,secondAfter),"cross-model redo preserves registration order and global metadata");
}
void MorphRegistrationSelectionReplay() {
    for(bool initialMarks:{false,true}) {
        MorphKeyFixture f;MorphKeySeed(f.first);
        if(initialMarks)MorphRegistrationMarks(f.first);
        const auto before=MorphKeysCopy(f.first);const auto bones=RegistrationKeys(f.first);
        std::array<unsigned char,4> displayBefore{};
        for(unsigned i=0;i<4;++i)displayBefore[i]=f.first.m->displayKeys[i].allocated;
        MorphRegistrationCommit(f,MorphRegistrationPlan(f,1,20));const auto after=MorphKeysCopy(f.first);
        Check(f.history.History(false)==MMS_OK,"selection replay fixture undoes registration");
        f.first.m->boneKeys[2].allocated=9;f.first.m->morphKeys[2].allocated=8;f.first.m->displayKeys[2].allocated=7;
        Check(f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,after)&&
            f.first.m->boneKeys[2].allocated==0&&f.first.m->displayKeys[2].allocated==0,
            "redo clears newly selected zero-baseline slots across all three captured key pools");
        f.first.m->boneKeys[3].allocated=6;f.first.m->morphKeys[15].allocated=5;f.first.m->displayKeys[1].allocated=4;
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&SameRegistrationKeys(f.first,bones),
            "undo restores the complete captured selection after further user selection changes");
        for(unsigned i=0;i<4;++i)Check(f.first.m->displayKeys[i].allocated==displayBefore[i],
            "undo restores original display marks including originally unselected slots");
        Check(mdl::MorphKeyPoolEnsure(*f.first.m,17),"selection replay accepts later independent pool growth");
        f.first.m->morphKeys[16].allocated=11;
        Check(f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,after)&&f.first.m->morphKeys[16].allocated==11,
            "complete captured selection replay preserves unrelated later growth tail marks");
    }
}
}
void NativeMorphRegistrationChecks() {
    std::puts("  morph registration: native parity and cross-frame undo");MorphRegistrationParity();
    std::puts("  morph registration: pristine curves no-op and overwritten ring");MorphRegistrationCurvesAndNoOp();
    std::puts("  morph registration: cancellable pool growth");MorphGrowthTokenChecks();
    std::puts("  morph registration: growth allocation failures and budget");MorphRegistrationGrowthAndFailures();
    std::puts("  morph registration: stale and malformed input");MorphRegistrationStaleChecks();
    std::puts("  morph registration: shared cross-model ordering");MorphRegistrationCrossModel();
    std::puts("  morph registration: complete selection replay");MorphRegistrationSelectionReplay();
}
