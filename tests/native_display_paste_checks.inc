// Include after native_display_registration_checks.inc.
namespace mikudancestudio {
bool RegisterDisplayKeyFromRecord(unsigned char*,int,unsigned char,int,const unsigned char*,int,const unsigned char*,int);
void ResetDisplayKeyCursor(unsigned char*);
}
namespace {
struct DisplayPasteFixture : DisplayFixture {
    std::array<mdl::DisplayClipboardRecord,3> clipboard{};
    std::array<std::array<mdl::IkClipboardState,3>,3> ik{};
    std::array<std::array<mdl::SelectorClipboardState,3>,3> parents{};
    DisplayPasteFixture() {
        DisplayEvaluationSeed(*this);
        for(auto* fixture:{&first,&second}) {
            std::memcpy(fixture->bones[0].name,"IK A",5);std::memcpy(fixture->bones[1].name,"IK B",5);
            fixture->m->ikChains[0].boneIndex=0;fixture->m->ikChains[1].boneIndex=1;
            fixture->m->boneKeys[0].allocated=3;fixture->m->morphKeys[0].allocated=4;
        }
        for(unsigned i=0;i<clipboard.size();++i) {
            auto& source=clipboard[i];source.frame=i*10;source.visible=static_cast<unsigned char>(5+i);
            source.ikCount=3;source.ikStates=ik[i].data();source.selectorCount=3;source.selectorStates=parents[i].data();
            std::memcpy(ik[i][0].boneName,"missing",8);ik[i][0].enabled=99;
            std::memcpy(ik[i][1].boneName,"IK B",5);ik[i][1].enabled=static_cast<unsigned char>(7+i);
            std::memcpy(ik[i][2].boneName,"IK A",5);ik[i][2].enabled=static_cast<unsigned char>(9+i);
            std::memcpy(parents[i][0].boneName,"model root",11);parents[i][0].modelIndex=1;parents[i][0].boneIndex=1;
            std::memcpy(parents[i][1].boneName,"IK B",5);parents[i][1].modelIndex=-1;parents[i][1].boneIndex=0;
            std::memcpy(parents[i][2].boneName,"IK A",5);parents[i][2].modelIndex=1;parents[i][2].boneIndex=0;
        }
        app->DisplayClipboard()=clipboard.data();app->ClipboardCounts()={};app->ClipboardCounts().displays=3;
        app->CurrentFrame()=5;app->state.lastRegisteredFrame=74;
    }
    ~DisplayPasteFixture() {
        app->DisplayClipboard()=nullptr;app->ClipboardCounts()={};
        history.Reset();history.Collect();DisplayTestFreeGrowth(first);DisplayTestFreeGrowth(second);
    }
    std::shared_ptr<plugins::HistoryChange> Plan() {
        std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanDisplayKeyPaste(*app,0,change)==MMS_OK&&change,"display-only paste builds private deep record");return change;
    }
};
void DisplayPasteOracle(DisplayPasteFixture& f) {
    auto& model=*f.second.m;
    for(std::size_t i=0;i<mdl::BoneKeyCapacity(model.boneKeys);++i)model.boneKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::MorphKeyCapacity(model.morphKeys);++i)model.morphKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::KeyPoolCapacity(model.displayKeys);++i)model.displayKeys[i].allocated=0;
    ResetDisplayKeyCursor(f.second.raw());
    for(unsigned i=0;i<f.clipboard.size();++i) {
        const auto& source=f.clipboard[i];
        Check(RegisterDisplayKeyFromRecord(f.second.raw(),int(source.frame),source.visible,source.ikCount,
            reinterpret_cast<const unsigned char*>(source.ikStates),source.selectorCount,
            reinterpret_cast<const unsigned char*>(source.selectorStates),f.app->CurrentFrame()),"legacy display paste oracle writes parsed deep record");
    }
    DisplayLegacyFrame(f.second,unsigned(f.app->CurrentFrame()));
}
void NativeDisplayPasteChecks() {
    using Mode=plugins::SharedHistory::RecordingMode;
    std::puts("  display paste: legacy deep-payload parity");
    for(int frame:{0,5,10,20,40,150})for(bool partial:{false,true}) {
        DisplayPasteFixture f;f.app->CurrentFrame()=frame;
        if(partial)for(auto& source:f.clipboard){source.ikCount=1;source.selectorCount=1;}
        const DisplayTestState before(f.first);const auto digest=f.first.Digest(),ring=PoseRingDigest(f.first);
        const auto pose=f.first.bones[1];const auto dirty=f.first.dirty[1];const auto scale=bone_scale::Pose(f.first.raw(),1);
        const auto identity=mdl::KeyPoolIdentity(f.first.m->displayKeys);auto* oldPool=f.first.m->displayKeys;
        std::array<unsigned char*,4> pointers{};for(unsigned i=0;i<4;++i)pointers[i]=oldPool[i].ikStates;
        auto change=f.Plan();std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("display paste parity",change,tx)==MMS_OK&&f.first.Digest()==digest&&before.Matches(f.first)&&
            f.first.m->displayKeys==oldPool,"display paste prepares deep payloads and growth without live writes");
        Check(CommitRegistrationNoAllocation(f.history,std::move(tx))==MMS_OK,"display paste publication performs no allocations");
        DisplayPasteOracle(f);
        Check(DisplayTestState(f.second).Matches(f.first)&&DisplayFrameEqual(f.first,f.second)&&
            f.first.m->searchCursor==f.second.m->searchCursor&&f.first.m->maxFrame==f.second.m->maxFrame&&
            f.app->state.lastRegisteredFrame==74,"display paste matches native names partial entries frame-zero insertion cursor max and selector evaluation");
        Check(PoseRingDigest(f.first)==ring&&mdl::KeyPoolIdentity(f.first.m->displayKeys)==identity&&
            !std::memcmp(&f.first.bones[1],&pose,sizeof(pose))&&f.first.dirty[1]==dirty&&bone_scale::Pose(f.first.raw(),1)==scale,
            "display paste preserves legacy history ownership and dirty pose and scale");
        for(unsigned i=0;i<4;++i)Check(f.first.m->displayKeys[i].ikStates==pointers[i],"display growth preserves original nested allocations");
        const DisplayTestState after(f.first);
        for(auto& source:f.ik)for(auto& entry:source)entry.enabled=100;
        for(auto& source:f.parents)for(auto& entry:source){entry.modelIndex=-1;entry.boneIndex=0;}
        f.clipboard={};f.app->ClipboardCounts()={};
        Check(f.history.History(false)==MMS_OK&&before.Matches(f.first)&&f.first.m->searchCursor==5&&
            f.first.m->boneKeys[0].allocated==3&&f.first.m->morphKeys[0].allocated==4,"display paste undo restores keys nested payloads all marks and cursor");
        f.app->CurrentFrame()=83;f.app->SelectedModelSlot()=1;f.app->CameraMode()=1;
        Check(f.history.History(true)==MMS_OK&&after.Matches(f.first)&&f.app->CurrentFrame()==83,
            "display paste redo owns nested clipboard bytes and original model across selection changes");
    }
    std::puts("  display paste: modes no-op and references");
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}) {
        DisplayPasteFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.app->CurrentFrame()=10;
        Check(f.history.SetMode(mode)==MMS_OK,"configure display paste recording mode");
        CmdControl400(f.app.get(),f.app->MainWindow(),421,0);
        Check(f.history.UndoCount()==1,"display-only paste is one explicit transaction in both modes");
        auto second=DisplayRegistrationPlan(f,200,1);DisplayRegistrationCommit(f,second);
        Check(f.history.History(false)==MMS_OK&&f.history.RedoCount()==1,"display paste no-op test starts with existing redo");
        const auto current=f.first.Digest();std::shared_ptr<plugins::HistoryChange> noop;
        Check(plugins::PlanDisplayKeyPaste(*f.app,0,noop)==MMS_OK&&!noop&&f.first.Digest()==current&&f.history.RedoCount()==1,
            "identical display paste does not clear marks cursor or redo");
        f.clipboard[2].selectorCount=3;f.parents[2][0].modelIndex=254;f.parents[2][0].boneIndex=222;
        auto change=f.Plan();DisplayTestCommit(f,change);
        Check(f.parents[2][0].modelIndex==254&&f.parents[2][0].boneIndex==222,
            "invalid external-parent repair never edits the live clipboard");
        const auto& key=f.first.m->displayKeys[2];
        Check(key.frame==30&&key.selectorStates[0].modelIndex==-1&&key.selectorStates[0].boneIndex==0,
            "missing external-parent model is privately detached");
    }
    std::puts("  display paste: cancellation stale and fault injection");
    {
        DisplayPasteFixture f;
        for(auto* model:{&f.first,&f.second})
            std::memcpy(model->bones[1].name,model->bones[0].name,sizeof(model->bones[0].name));
        const DisplayTestState before(f.first);const auto digest=f.first.Digest();
        auto change=f.Plan();Check(f.first.Digest()==digest&&before.Matches(f.first),
            "display-only paste with duplicate IK names plans without changing native data");
        DisplayTestCommit(f,change);DisplayPasteOracle(f);
        Check(DisplayTestState(f.second).Matches(f.first)&&DisplayFrameEqual(f.first,f.second),
            "display paste broadcasts matching flags to duplicate IK and selector entries like native registrar");
        const DisplayTestState after(f.first);f.clipboard={};f.app->ClipboardCounts()={};
        Check(f.history.UndoCount()==1&&f.history.History(false)==MMS_OK&&before.Matches(f.first)&&
            f.history.History(true)==MMS_OK&&after.Matches(f.first),
            "display-only duplicate-name payload remains one reversible owned transaction");
    }
    {
        DisplayPasteFixture f;const auto before=f.first.Digest();auto change=f.Plan();
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("cancel display paste",change,tx)==MMS_OK,"display paste cancellation prepares");tx.reset();
        Check(f.first.Digest()==before&&!f.history.UndoCount(),"cancel display paste leaves no nested growth marks or history");
        Check(f.history.PrepareNative("stale display paste",change,tx)==MMS_OK,"display paste stale candidate prepares");
        f.first.bones[0].name[0]='x';
        Check(f.history.CommitNative(std::move(tx))==MMS_E_STALE&&!f.history.UndoCount(),"display paste rechecks destination mapping names");
        f.first.bones[0].name[0]='I';
        auto* target=f.app->ModelSlot(1);f.app->ModelSlot(1)=nullptr;
        std::unique_ptr<plugins::PreparedHistoryChange> stale;
        Check(change->Prepare(true,stale)==MMS_E_STALE&&!stale,"display paste refuses a missing retained external target before publication");
        f.app->ModelSlot(1)=target;
        f.app->ModelSlot(1)=f.first.raw();
        Check(change->Prepare(true,stale)==MMS_E_STALE&&!stale,"display paste rejects a different external model reusing the same slot");
        f.app->ModelSlot(1)=target;
    }
    {
        DisplayPasteFixture f;const auto before=f.first.Digest();std::shared_ptr<plugins::HistoryChange> change;bool planned=false;
        for(long allowance=0;allowance<1024&&!planned;++allowance) {
            failAfter=allowance;const auto result=plugins::PlanDisplayKeyPaste(*f.app,0,change);failAfter=-1;
            Check(f.first.Digest()==before&&!f.history.UndoCount(),"display paste planning faults preserve nested source bytes");
            if(result==MMS_OK){Check(bool(change),"display paste fault sweep ends with complete plan");planned=true;}
            else Check(result==MMS_E_MEMORY&&!change,"display paste failed plan publishes no output");
        }
        Check(planned,"display paste fault sweep reaches all planner allocations");bool prepared=false;
        for(long allowance=0;allowance<2048&&!prepared;++allowance) {
            std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;std::string label="deep display paste";
            failAfter=allowance;const auto result=f.history.PrepareNative(std::move(label),change,tx);failAfter=-1;
            Check(f.first.Digest()==before&&!f.history.UndoCount(),"display paste candidate faults preserve source pointers and payloads");
            if(result==MMS_OK){prepared=true;tx.reset();}
            else Check(result==MMS_E_MEMORY&&!tx,"display paste candidate OOM publishes no journal record");
        }
        Check(prepared,"display paste fault sweep reaches private growth evaluation and journal allocations");
        Check(f.history.Configure(Mode::AllOperations,32,1)==MMS_OK,"configure display paste budget rejection");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("budget display paste",change,tx)==MMS_E_MEMORY&&f.first.Digest()==before,
            "display paste budget is checked before live writes");
    }
}
}
