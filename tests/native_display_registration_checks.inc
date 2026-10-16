// Include after native_display_key_checks.inc. The independent oracle is the
// real RegisterDisplayKeyCurrent plus command 438's selection/scalar writes.
namespace {
void DisplayRegistrationInputs(DisplayFixture& f) {
    for(auto* target:{&f.first,&f.second}) {
        DisplayTestSeed(*target);auto& m=*target->m;m.loadComplete=7;
        m.ikChains[0].enabled=2;m.ikChains[1].enabled=9;
        auto* order=static_cast<mdl::BoneOrderEntry*>(m.boneOrderTable);
        for(unsigned i=0;i<3;++i) {
            order[i].linkedModel=i==1?-1:1;order[i].linkedBone=int(i%2);
            order[i].windowStart=81+i;order[i].windowEnd=101+i;
        }
        m.boneKeys[0].allocated=3;m.boneKeys[9].allocated=5;
        m.morphKeys[0].allocated=6;m.displayKeys[1].allocated=7;
    }
}
void DisplayRegistrationOracle(Fixture& f,int frame) {
    for(std::size_t i=0;i<mdl::BoneKeyCapacity(f.m->boneKeys);++i)f.m->boneKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::MorphKeyCapacity(f.m->morphKeys);++i)f.m->morphKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::KeyPoolCapacity(f.m->displayKeys);++i)f.m->displayKeys[i].allocated=0;
    RegisterDisplayKeyCurrent(f.raw(),frame);
}
std::shared_ptr<plugins::HistoryChange> DisplayRegistrationPlan(DisplayFixture& f,int frame,unsigned slot=0) {
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanDisplayRegistration(*f.app,slot,frame,change)==MMS_OK&&change,
        "display registration privately plans one authoring change");return change;
}
void DisplayRegistrationCommit(DisplayFixture& f,const std::shared_ptr<plugins::HistoryChange>& change) {
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
    Check(f.history.PrepareNative("register display",change,tx)==MMS_OK,"display registration journal prepares");
    Check(CommitRegistrationNoAllocation(f.history,std::move(tx))==MMS_OK,"display registration publishes without allocation");
}
void DisplayRegistrationParity() {
    for(int frame:{0,10,15,40,150})for(bool duplicate:{false,true}) {
        DisplayFixture f;DisplayRegistrationInputs(f);f.app->CurrentFrame()=frame;f.app->state.lastRegisteredFrame=60;
        for(auto* fixture:{&f.first,&f.second}) {
            std::memset(fixture->bones[0].name,0,sizeof(fixture->bones[0].name));
            std::memset(fixture->bones[1].name,0,sizeof(fixture->bones[1].name));
            if(duplicate){std::memcpy(fixture->bones[0].name,"same",5);std::memcpy(fixture->bones[1].name,"same",5);}
        }
        const DisplayTestState before(f.first);const auto ring=PoseRingDigest(f.first);
        const auto bones=f.first.bones;const auto dirty=f.first.dirty;
        const auto selectors=f.firstOrder;const auto ik=f.firstIk;const auto identity=mdl::KeyPoolIdentity(f.first.m->displayKeys);
        auto* oldPool=f.first.m->displayKeys;std::array<unsigned char*,4> nested{};
        for(unsigned i=0;i<4;++i)nested[i]=oldPool[i].ikStates;
        auto change=DisplayRegistrationPlan(f,frame);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("display register parity",change,tx)==MMS_OK&&before.Matches(f.first)&&
            f.first.m->displayKeys==oldPool&&mdl::KeyPoolCapacity(oldPool)==4&&f.app->state.lastRegisteredFrame==60,
            "registration and possible growth remain private before publication");
        Check(CommitRegistrationNoAllocation(f.history,std::move(tx))==MMS_OK,"display insert overwrite and growth commit without allocation");
        DisplayRegistrationOracle(f.second,frame);
        Check(DisplayTestState(f.second).Matches(f.first)&&f.first.m->maxFrame==f.second.m->maxFrame&&
            f.first.m->searchCursor==5&&f.app->state.lastRegisteredFrame==(std::max)(60u,f.second.m->maxFrame),
            "root overwrite middle insertion and append match the real registrar and command metadata");
        Check(!std::memcmp(bones.data(),f.first.bones.data(),sizeof(bones))&&dirty==f.first.dirty&&
            !std::memcmp(selectors.data(),f.firstOrder.data(),sizeof(selectors))&&
            !std::memcmp(ik.data(),f.firstIk.data(),sizeof(ik))&&f.first.m->loadComplete==7&&PoseRingDigest(f.first)==ring,
            "first registration preserves live IK selectors visibility bones dirty flags and old ring");
        Check(mdl::KeyPoolIdentity(f.first.m->displayKeys)==identity,"display growth preserves registered pool identity");
        for(unsigned i=0;i<4;++i)Check(f.first.m->displayKeys[i].ikStates==nested[i],
            "display growth transfers each existing nested allocation without replacing it");
        for(std::size_t i=0;i<mdl::BoneKeyCapacity(f.first.m->boneKeys);++i)
            Check(f.first.m->boneKeys[i].allocated==0,"display registration clears all bone marks");
        for(std::size_t i=0;i<mdl::MorphKeyCapacity(f.first.m->morphKeys);++i)
            Check(f.first.m->morphKeys[i].allocated==0,"display registration clears all morph marks");
        const DisplayTestState after(f.first);
        Check(f.history.History(false)==MMS_OK&&before.Matches(f.first)&&f.first.m->boneKeys[9].allocated==5&&
            f.first.m->morphKeys[0].allocated==6&&f.first.m->maxFrame==99&&f.app->state.lastRegisteredFrame==60,
            "registration undo restores keys deep payloads all family marks and exact scalar metadata");
        Check(f.history.History(true)==MMS_OK&&after.Matches(f.first),"registration redo restores complete independent key payloads");
        DisplayTestFreeGrowth(f.first);DisplayTestFreeGrowth(f.second);
    }
}
void DisplayRegistrationNoopAndLifetime() {
    DisplayFixture f;DisplayRegistrationInputs(f);f.app->CurrentFrame()=10;
    auto first=DisplayRegistrationPlan(f,10);DisplayRegistrationCommit(f,first);
    auto second=DisplayRegistrationPlan(f,20,1);DisplayRegistrationCommit(f,second);
    Check(f.history.History(false)==MMS_OK&&f.history.RedoCount()==1,"second model registration occupies the common history order");
    f.first.m->boneKeys[0].allocated=5;f.first.m->morphKeys[0].allocated=6;f.first.m->displayKeys[0].allocated=7;
    const auto marks=f.first.Digest();std::shared_ptr<plugins::HistoryChange> noop=first;
    Check(plugins::PlanDisplayRegistration(*f.app,0,10,noop)==MMS_OK&&!noop&&f.first.Digest()==marks&&
        f.history.UndoCount()==1&&f.history.RedoCount()==1,"identical display data preserves all marks and the redo branch");
    Check(f.history.History(true)==MMS_OK,"cross-model redo survives repeated display registration");
    for(unsigned i=0;i<100;++i)PushBoneEditUndo(f.app.get());
    Check(f.history.History(false)==MMS_OK&&f.history.History(false)==MMS_OK,
        "two models' display registrations survive overwriting the thirty-slot legacy ring");
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK,
        "cross-model display registration retains deep before and after ownership");
}
void DisplayRegistrationEvaluation() {
    for(int playhead:{0,10,15,25,40}) {
        DisplayFixture f;DisplayEvaluationSeed(f);f.app->CurrentFrame()=15;
        f.first.m->loadComplete=8;f.firstIk[0].enabled=9;
        f.firstOrder[0].linkedModel=1;f.firstOrder[0].linkedBone=1;
        const auto initial=f.first.bones;const auto scales=bone_scale::PoseValues(f.first.raw());
        const DisplayTestState before(f.first);
        auto change=DisplayRegistrationPlan(f,15);DisplayRegistrationCommit(f,change);
        Check(!std::memcmp(initial.data(),f.first.bones.data(),sizeof(initial))&&bone_scale::PoseValuesEqual(f.first.raw(),scales),
            "first display registration does not evaluate even clean associated bone poses");
        const DisplayTestState after(f.first);
        f.app->CurrentFrame()=playhead;
        Check(f.history.History(false)==MMS_OK&&before.Matches(f.first),"display registration undo works at another playhead");
        DisplayLegacyFrame(f.second,unsigned(playhead));
        Check(DisplayFrameEqual(f.first,f.second)&&f.first.bones[1].trans[0]==456&&f.first.dirty[1]==1&&
            bone_scale::Pose(f.first.raw(),1)==Scale(9),"undo uses native display and selector windows while preserving dirty pose and scale");
        Check(f.history.History(true)==MMS_OK&&after.Matches(f.first),"display registration redo samples its target at the new playhead");
        // Copy only authored key data to the oracle, then use the legacy frame
        // helpers. Both pools have the same growth policy and slot identities.
        Check(mdl::DisplayKeyPoolEnsure(*f.second.m,mdl::KeyPoolCapacity(f.first.m->displayKeys)),"oracle pool reserves matching growth");
        after.Restore(f.second);DisplayLegacyFrame(f.second,unsigned(playhead));
        Check(DisplayFrameEqual(f.first,f.second)&&f.first.bones[1].trans[0]==456&&f.first.dirty[1]==1,
            "redo reconstructs exact between-key and tail selector windows without replacing an unregistered pose");
        DisplayTestFreeGrowth(f.first);DisplayTestFreeGrowth(f.second);
    }
}
void DisplayRegistrationFailures() {
    DisplayFixture f;DisplayRegistrationInputs(f);f.app->CurrentFrame()=15;
    const DisplayTestState before(f.first);const auto digest=f.first.Digest();auto* pool=f.first.m->displayKeys;
    const auto identity=mdl::KeyPoolIdentity(pool);std::shared_ptr<plugins::HistoryChange> change;
    unsigned failures=0;bool success=false;
    for(long allowance=0;allowance<512&&!success;++allowance) {
        failAfter=allowance;const auto result=plugins::PlanDisplayRegistration(*f.app,0,15,change);failAfter=-1;
        Check(before.Matches(f.first)&&f.first.Digest()==digest&&f.first.m->displayKeys==pool,
            "every registration planning allocation failure preserves live authoring and pool");
        if(result==MMS_OK){success=true;Check(bool(change),"registration fault sweep returns one complete plan");}
        else{++failures;Check(result==MMS_E_MEMORY&&!change,"registration planning OOM leaves no partial record");}
    }
    Check(success&&failures>10,"registration planning failure sweep covers deep payload ownership");
    success=false;failures=0;
    for(long allowance=0;allowance<512&&!success;++allowance) {
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;std::string label="display growth faults";
        failAfter=allowance;const auto result=f.history.PrepareNative(std::move(label),change,tx);failAfter=-1;
        Check(before.Matches(f.first)&&f.first.Digest()==digest&&f.first.m->displayKeys==pool&&
            mdl::KeyPoolIdentity(pool)==identity&&mdl::KeyPoolCapacity(pool)==4,
            "candidate OOM and cancellation preserve pointer capacity identity and every nested byte");
        if(result==MMS_OK){success=true;failAfter=0;tx.reset();failAfter=-1;}
        else{++failures;Check(result==MMS_E_MEMORY&&!tx,"registration prepare OOM leaves no reserved transaction");}
    }
    Check(success&&failures>10&&f.history.UndoCount()==0,"cancelled growth does not switch registration to replay semantics");
    for(int kind=0;kind<8;++kind) {
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("stale display registration",change,tx)==MMS_OK,"registration stale candidate prepares");
        if(kind==0)++f.first.m->loadComplete;if(kind==1)++f.firstIk[0].enabled;
        if(kind==2)++f.firstOrder[0].linkedBone;if(kind==3)++f.first.m->boneKeys[9].allocated;
        if(kind==4)++f.first.m->morphKeys[0].allocated;if(kind==5)++f.first.m->maxFrame;
        if(kind==6)++f.app->state.lastRegisteredFrame;if(kind==7)++f.app->CurrentFrame();
        Check(CommitRegistrationNoAllocation(f.history,std::move(tx))==MMS_E_STALE&&f.history.UndoCount()==0&&f.first.m->displayKeys==pool,
            "changed live display IK parents marks metadata and playhead reject without publishing growth");
        if(kind==0)--f.first.m->loadComplete;if(kind==1)--f.firstIk[0].enabled;
        if(kind==2)--f.firstOrder[0].linkedBone;if(kind==3)--f.first.m->boneKeys[9].allocated;
        if(kind==4)--f.first.m->morphKeys[0].allocated;if(kind==5)--f.first.m->maxFrame;
        if(kind==6)--f.app->state.lastRegisteredFrame;if(kind==7)--f.app->CurrentFrame();
    }
    ++f.firstIk[0].enabled;std::unique_ptr<plugins::PreparedHistoryChange> stale;
    Check(change->Prepare(true,stale)==MMS_E_STALE&&!stale,"first prepare rejects source authoring changed after planning");
    --f.firstIk[0].enabled;DisplayRegistrationCommit(f,change);const DisplayTestState after(f.first);
    failAfter=0;const auto undo=f.history.History(false);failAfter=-1;
    Check(undo==MMS_E_MEMORY&&after.Matches(f.first)&&f.history.UndoCount()==1,"undo allocation failure preserves registered data and cursor");
    Check(f.history.History(false)==MMS_OK&&before.Matches(f.first),"registration undo survives injected failure");
    failAfter=0;const auto redo=f.history.History(true);failAfter=-1;
    Check(redo==MMS_E_MEMORY&&before.Matches(f.first)&&f.history.RedoCount()==1,"redo allocation failure preserves redo and original marks");
    Check(f.history.History(true)==MMS_OK&&after.Matches(f.first),"registration redo survives injected failure");
    DisplayTestFreeGrowth(f.first);
}
void DisplayRegistrationGuards() {
    DisplayFixture f;DisplayRegistrationInputs(f);std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanDisplayRegistration(*f.app,0,-1,change)==MMS_E_ARGUMENT&&!change,"negative display registration frame rejects");
    auto* pool=f.first.m->displayKeys;std::array<mdl::DisplayKey,4> borrowed{};f.first.m->displayKeys=borrowed.data();
    Check(plugins::PlanDisplayRegistration(*f.app,0,15,change)==MMS_E_NOT_READY&&!change,"borrowed display pools are not inferred owned");
    f.first.m->displayKeys=pool;const auto next=pool[2].next;pool[2].next=3;
    Check(plugins::PlanDisplayRegistration(*f.app,0,15,change)==MMS_E_ARGUMENT&&!change,"cyclic display registration chain rejects");pool[2].next=next;
    const auto parent=f.firstOrder[0].linkedModel;f.firstOrder[0].linkedModel=254;
    Check(plugins::PlanDisplayRegistration(*f.app,0,15,change)==MMS_E_NOT_READY&&!change,"missing external parent rejects before registration");
    f.firstOrder[0].linkedModel=parent;
    f.app->PlaybackActive()=1;Check(plugins::PlanDisplayRegistration(*f.app,0,15,change)==MMS_E_NOT_READY&&!change,
        "display registration refuses playback");f.app->PlaybackActive()=0;
}
void NativeDisplayRegistrationChecks() {
    DisplayRegistrationParity();DisplayRegistrationNoopAndLifetime();DisplayRegistrationEvaluation();
    DisplayRegistrationFailures();DisplayRegistrationGuards();
}
}
