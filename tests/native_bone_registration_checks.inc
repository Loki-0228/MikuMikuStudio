// Include after native_pose_scope_checks.inc; call NativeBoneRegistrationChecks().
// Uses the actual legacy registrar as an independent interpolation oracle.
#include "../src/plugins/native_bone_registration.hpp"
namespace mikumikustudio {
void CmdControl500(MMDApp*,HWND,std::uint16_t,std::uint16_t);
void RegisterSelectedBoneKeys(unsigned char*,int,int);
}
namespace {
void RegistrationProgress(const char* stage) {
    std::printf("  registration: %s\n",stage);std::fflush(stdout);
}
struct BoneRegistrationFixture : PoseNumericFixture {
    BoneRegistrationFixture() {
        for(int id:{panel::kPhysicsFrameCheckbox,panel::kPhysicsCheckbox}) {
            const auto control=CreateWindowExA(0,"BUTTON","registration",WS_CHILD|BS_CHECKBOX,
                0,0,80,20,app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
            Check(control!=nullptr,"hidden registration checkbox created");
        }
        for(auto* f:{&first,&second})*reinterpret_cast<HWND*>(f->raw())=app->MainWindow();
    }
    void Command(){CmdControl500(app.get(),app->MainWindow(),500,0);}
    void Automatic(bool enabled){CheckDlgButton(app->MainWindow(),panel::kPhysicsFrameCheckbox,enabled?BST_CHECKED:BST_UNCHECKED);}
};
std::vector<mdl::BoneKey> RegistrationKeys(const Fixture& f) {
    return {f.m->boneKeys,f.m->boneKeys+mdl::BoneKeyCapacity(f.m->boneKeys)};
}
bool SameRegistrationKeys(const Fixture& f,const std::vector<mdl::BoneKey>& keys) {
    return mdl::BoneKeyCapacity(f.m->boneKeys)>=keys.size()&&
        !std::memcmp(f.m->boneKeys,keys.data(),keys.size()*sizeof(mdl::BoneKey));
}
MmsResult CommitRegistrationNoAllocation(plugins::SharedHistory& history,
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction) {
    failAfter=0;const auto result=history.CommitNative(std::move(transaction));failAfter=-1;return result;
}
std::shared_ptr<plugins::HistoryChange> RegistrationPlan(BoneRegistrationFixture& f,unsigned slot,int frame,bool automatic=false) {
    plugins::BoneRegistrationOptions options;options.frame=frame;options.automaticInterpolation=automatic;
    std::shared_ptr<plugins::HistoryChange> result;
    Check(plugins::PlanBoneRegistration(*f.app,slot,options,result)==MMS_OK,"bone registration plan succeeds");return result;
}
void CommitRegistration(BoneRegistrationFixture& f,unsigned slot,int frame,bool automatic=false) {
    auto change=RegistrationPlan(f,slot,frame,automatic);Check(bool(change),"bone registration plan owns an authored change");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("bone registration",change,transaction)==MMS_OK,"journal prepares complete bone registration");
    Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,"bone registration commits without allocation");
}
void SeedRegistrationChain(Fixture& f) {
    f.m->boneKeys[0].next=2;
    for(unsigned slot:{2u,3u}) {
        auto& key=f.m->boneKeys[slot];key.frame=slot==2?10:30;key.previous=slot==2?0:2;key.next=slot==2?3:0;
        key.rotation[3]=1;key.position[0]=slot==2?10.f:30.f;key.position[1]=slot==2?-5.f:10.f;
        for(int lane=0;lane<4;++lane){key.interpolation[lane]=20;key.interpolation[lane+4]=20;
            key.interpolation[lane+8]=107;key.interpolation[lane+12]=107;}
    }
    f.bones[0].trans[0]=18;f.bones[0].trans[1]=7;f.bones[0].trans[2]=-2;
    f.dirty[0]=1;f.m->boneKeys[1].allocated=1;f.m->morphKeys[0].allocated=1;f.m->displayKeys[0].allocated=1;
    bone_scale::SetPose(f.raw(),0,Scale(2));bone_scale::SetKey(f.raw(),2,Scale(3));
}
void RegistrationLegacyParityChecks() {
    for(bool multiple:{false,true})for(bool automatic:{false,true})for(int frame:{0,10,20,40}) {
        BoneRegistrationFixture f;SeedRegistrationChain(f.first);SeedRegistrationChain(f.second);f.Automatic(automatic);
        if(multiple)for(auto* model:{&f.first,&f.second}) {
            model->selected[1]=1;model->bones[1].trans[2]=4;model->dirty[1]=1;
            bone_scale::SetPose(model->raw(),1,Scale(3));
        }
        const auto before=f.first.Digest(),ring=PoseRingDigest(f.first);const auto original=RegistrationKeys(f.first);
        const auto scaleBefore=bone_scale::AuthoringState::Capture(f.first.raw());
        auto change=RegistrationPlan(f,0,frame,automatic);
        Check(change&&f.first.Digest()==before&&bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scaleBefore),
            "planning insert overwrite and interpolation keeps live native and scale authoring untouched");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("registration parity",change,transaction)==MMS_OK&&f.first.Digest()==before,
            "journal preparation does not modify native keys pose flags or ring");
        Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,"prepared parity registration publishes without allocation");
        RegisterSelectedBoneKeys(f.second.raw(),frame,2);
        Check(!std::memcmp(f.first.m->boneKeys,f.second.m->boneKeys,32*sizeof(mdl::BoneKey)),
            "owned registrar matches real legacy key contents and automatic interpolation neighbours");
        Check(!std::memcmp(f.first.bones.data(),f.second.bones.data(),sizeof(f.first.bones))&&f.first.dirty==f.second.dirty&&
            f.first.m->searchCursor==f.second.m->searchCursor&&f.first.m->maxFrame==f.second.m->maxFrame&&
            f.first.m->morphKeys[0].allocated==0&&f.first.m->displayKeys[0].allocated==0,
            "owned registrar matches legacy pose dirty cursor maximum frame and auxiliary selection clearing");
        for(unsigned slot=0;slot<32;++slot)
            Check(bone_scale::Key(f.first.raw(),int(slot))==bone_scale::Key(f.second.raw(),int(slot))&&
                bone_scale::KeyCurve(f.first.raw(),int(slot))==bone_scale::KeyCurve(f.second.raw(),int(slot)),
                "owned registrar matches real legacy scale key and scale interpolation");
        Check(PoseRingDigest(f.first)==ring&&f.history.UndoCount()==1,"owned registration does not write the thirty-slot ring");
        const auto registered=RegistrationKeys(f.first);
        Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,original)&&
            bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scaleBefore)&&f.first.dirty[0]==1,
            "registration undo restores keys scale and dirty source");
        Check(f.history.History(true)==MMS_OK&&SameRegistrationKeys(f.first,registered),"registration redo restores independent owned keys");
    }
}
bone_curve::Tracks RegistrationFreeTracks() {
    bone_curve::Tracks tracks;
    for(int channel=0;channel<9;++channel){animation_curve::Key first,last;last.frame=20;
        first.tangent=last.tangent=animation_curve::Tangent::Free;first.value=last.value=channel>=6?1:0;
        if(channel==0){last.value=12;first.outSlope=.2;last.inSlope=.1;first.outWeight=.2;last.inWeight=.4;}
        tracks[channel].keys={first,last};}
    return tracks;
}
void RegistrationCurveAndRingChecks() {
    BoneRegistrationFixture f;std::string error;
    Check(bone_curve::Write(f.first.raw(),0,RegistrationFreeTracks(),error),"real free curve seeded before native registration");
    const auto author=bone_curve::AuthoringState::Capture(f.first.raw());const auto original=RegistrationKeys(f.first);
    const auto scaleBefore=bone_scale::AuthoringState::Capture(f.first.raw());
    f.first.bones[0].trans[0]=9;f.first.dirty[0]=1;bone_scale::SetPose(f.first.raw(),0,Scale(2));
    CommitRegistration(f,0,10);
    const unsigned inserted=f.first.m->boneKeys[0].next;
    bone_curve::Tracks registeredTracks;Check(bone_curve::Read(f.first.raw(),0,registeredTracks),"registered native pose retains its free authoring");
    const auto added=std::find_if(registeredTracks[0].keys.begin(),registeredTracks[0].keys.end(),[](const auto& key){return key.frame==10;});
    Check(f.first.m->boneKeys[inserted].frame==10&&bone_scale::Key(f.first.raw(),int(inserted))==Scale(2)&&
        added!=registeredTracks[0].keys.end()&&added->value==9&&registeredTracks[0].keys.back().frame==20&&registeredTracks[0].keys.back().value==12,
        "registering native pose adds its explicit free-curve key and scale while preserving the other authored endpoint");
    const auto registeredAuthoring=bone_curve::AuthoringState::Capture(f.first.raw());
    const auto registered=RegistrationKeys(f.first);
    // Exercise real ring capture, including scale and curve rings, over three
    // complete wraps. It must not supply any payload to this journal node.
    for(unsigned n=0;n<100;++n)PushBoneEditUndo(f.app.get());
    const auto overwrittenRing=PoseRingDigest(f.first);
    Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,original)&&
        bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scaleBefore)&&
        bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*author),
        "registration restores native keys scale and authored free curve after one hundred legacy ring captures");
    bone_curve::Tracks read;Check(bone_curve::Read(f.first.raw(),0,read)&&read[0].keys.back().value==12&&
        read[0].keys.front().outWeight==.2,"restored free curve hash validates against restored native and scale keys");
    Check(PoseRingDigest(f.first)==overwrittenRing&&f.history.History(true)==MMS_OK&&
        SameRegistrationKeys(f.first,registered)&&bone_curve::Has(f.first.raw(),0)&&bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*registeredAuthoring),
        "redo restores the complete reconciled free authoring without replacing the newer legacy ring");
}
void RegistrationCommandAndCrossModelChecks() {
    {
        BoneRegistrationFixture f;f.first.bones[0].trans[0]=6;f.app->CurrentFrame()=15;
        const auto cursor=f.first.m->undoState[0];
        Check(!plugins::TryRegisterBoundBones(f.app.get()),"unbound registrar leaves production routing with the legacy owner");
        // Call the native writer directly: legacy CmdControl500 also invokes
        // PanelPaint, which requires the renderer absent from this CPU fixture.
        RegisterSelectedBoneKeys(f.first.raw(),15,2);
        Check(f.first.m->undoState[0]==(cursor+1)%30&&f.history.UndoCount()==0&&
            f.first.m->boneKeys[f.first.m->boneKeys[0].next].position[0]==6,
            "unbound native registration keeps its original legacy ring path");
    }
    BoneRegistrationFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
    const auto firstRing=PoseRingDigest(f.first),secondRing=PoseRingDigest(f.second);
    f.Commit(panel::kReadoutPosXEdit,"5");f.app->CurrentFrame()=10;f.Command();
    const auto firstKeys=RegistrationKeys(f.first);
    f.app->SelectedModelSlot()=1;f.Commit(panel::kReadoutPosYEdit,"8");f.app->CurrentFrame()=20;f.Command();
    const auto secondKeys=RegistrationKeys(f.second);
    Check(f.history.UndoCount()==4&&f.first.dirty[0]==0&&f.second.dirty[0]==0&&
        f.SelectionUnchanged()&&PoseRingDigest(f.first)==firstRing&&PoseRingDigest(f.second)==secondRing,
        "actual register button interleaves two model registrations with numeric pose edits without legacy rings or selection changes");
    f.app->SelectedModelSlot()=0;
    Check(f.history.History(false)==MMS_OK&&f.second.m->boneKeys[0].next==0&&f.second.dirty[0]==1,
        "first common undo removes the most recent other-model registration");
    Check(f.history.History(false)==MMS_OK&&f.second.bones[0].trans[1]==0,"second common undo restores other-model pose edit");
    Check(f.history.History(false)==MMS_OK&&f.first.m->boneKeys[0].next==0&&f.first.dirty[0]==1,
        "third common undo removes earlier-model registration");
    Check(f.history.History(false)==MMS_OK&&f.first.bones[0].trans[0]==0,"fourth common undo restores earlier-model pose");
    for(int n=0;n<4;++n)Check(f.history.History(true)==MMS_OK,"cross-model mixed pose and key redo succeeds");
    Check(SameRegistrationKeys(f.first,firstKeys)&&SameRegistrationKeys(f.second,secondKeys)&&
        f.app->CurrentFrame()==20&&f.app->SelectedModelSlot()==0&&f.SelectionUnchanged(),
        "mixed redo restores both model key tracks without seeking or selecting a model");
    const auto before=f.first.Digest();const auto count=f.history.UndoCount();
    f.app->CameraMode()=1;f.Command();f.app->CameraMode()=0;
    Check(f.first.Digest()==before&&f.history.UndoCount()==count,"bound register in camera mode never falls through to legacy ring");
    f.app->SelectedModelSlot()=255;f.Command();
    Check(f.first.Digest()==before&&f.history.UndoCount()==count,"bound register rejects invalid model slot safely");
}
void RegistrationNoOpAndRejectionChecks() {
    BoneRegistrationFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
    f.first.bones[0].trans[0]=5;f.app->CurrentFrame()=10;f.Command();
    f.Commit(panel::kReadoutPosYEdit,"3");Check(f.history.History(false)==MMS_OK,"prepare redo branch before registration no-op");
    const auto before=f.first.Digest();const auto undo=f.history.UndoCount(),redo=f.history.RedoCount();
    f.app->SceneModified()=0;f.app->RecordedFrameCountByte1()=0;f.Command();
    Check(f.first.Digest()==before&&f.history.UndoCount()==undo&&f.history.RedoCount()==redo&&f.app->SceneModified()==0,
        "unchanged registration preserves redo and scene dirty flags");
    f.first.selected[0]=0;f.Command();f.first.selected[0]=1;
    Check(f.first.Digest()==before&&f.history.RedoCount()==redo,"empty bone selection preserves redo and native state");
    for(int gate=0;gate<3;++gate){
        if(gate==0)f.app->PlaybackActive()=1;if(gate==1)f.app->FrameStepPlayback()=1;
        if(gate==2)f.app->state.recordPlaybackStartPending=1;
        f.Command();f.app->PlaybackActive()=f.app->FrameStepPlayback()=0;f.app->state.recordPlaybackStartPending=0;
        Check(f.first.Digest()==before&&f.history.RedoCount()==redo,"registration rejects playback or pending recording without legacy fallback");
    }
    f.first.bones[0].hasRigidBody=1;f.first.bones[0].physicsDisabled=0;
    const auto physicsBefore=f.first.Digest();f.Command();
    Check(f.first.Digest()==physicsBefore&&f.history.RedoCount()==redo&&f.app->PhysicsResetPending()==0,
        "unsupported solver switch rejects registration before changing pose physics or redo");
    f.first.bones[0].hasRigidBody=0;
    const auto oldNext=f.first.m->boneKeys[0].next;f.first.m->boneKeys[0].next=999;
    const auto invalid=f.first.Digest();std::shared_ptr<plugins::HistoryChange> change;
    plugins::BoneRegistrationOptions options;options.frame=15;
    Check(plugins::PlanBoneRegistration(*f.app,0,options,change)==MMS_E_ARGUMENT&&!change&&f.first.Digest()==invalid,
        "invalid bone key chain is rejected before planning mutates native state");
    f.first.m->boneKeys[0].next=oldNext;
    Check(f.history.Enter()==MMS_OK,"lock journal for registration busy guard");f.Command();f.history.Leave();
    Check(f.history.RedoCount()==redo&&f.app->SceneModified()==0,"busy bound registration keeps redo and scene dirty flags");
}
void RegistrationStaleChecks() {
    BoneRegistrationFixture f;f.first.bones[0].trans[0]=5;f.first.dirty[0]=1;
    bone_scale::SetPose(f.first.raw(),0,Scale(2));
    for(int kind=0;kind<6;++kind) {
        auto change=RegistrationPlan(f,0,10);std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("stale registration",change,transaction)==MMS_OK,"registration stale fixture prepares");
        const auto key=f.first.m->boneKeys[0];const auto bone=f.first.bones[0];const auto last=f.app->state.lastRegisteredFrame;
        if(kind==0)f.first.m->boneKeys[0].position[0]=99;
        if(kind==1)f.first.dirty[0]=0;
        if(kind==2)f.app->state.lastRegisteredFrame=91;
        if(kind==3)bone_scale::SetKey(f.first.raw(),0,Scale(7));
        if(kind==4)f.first.bones[0].trans[0]=19;
        if(kind==5)bone_scale::SetPose(f.first.raw(),0,Scale(9));
        const auto stale=f.first.Digest();
        Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_E_STALE&&
            f.first.Digest()==stale&&f.history.UndoCount()==0,"stale registration candidate rejects before any participant publishes");
        f.first.m->boneKeys[0]=key;f.first.bones[0]=bone;f.first.dirty[0]=1;f.app->state.lastRegisteredFrame=last;
        if(kind==3)bone_scale::SetKey(f.first.raw(),0,bone_scale::Matrix{});
        if(kind==5)bone_scale::SetPose(f.first.raw(),0,Scale(2));
    }
}
void RegistrationGrowthAndFailureChecks() {
    BoneRegistrationFixture f;
    for(unsigned slot=2;slot<32;++slot){f.first.m->boneKeys[slot].frame=slot;f.first.m->boneKeys[slot].rotation[3]=1;}
    f.first.bones[0].trans[0]=9;f.first.dirty[0]=1;bone_scale::SetPose(f.first.raw(),0,Scale(2));
    const auto original=RegistrationKeys(f.first);const auto digest=f.first.Digest();auto* const pool=f.first.m->boneKeys;
    auto change=RegistrationPlan(f,0,40);Check(change&&f.first.Digest()==digest,"growth plan keeps original pool pointer and native bytes");
    unsigned failures=0;bool committed=false;
    for(long limit=0;limit<512;++limit) {
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        // Argument construction belongs to this test, outside the API's
        // allocation boundary. Inject failures only after the label exists.
        std::string label="growth registration";
        failAfter=limit;const auto result=f.history.PrepareNative(std::move(label),change,transaction);failAfter=-1;
        Check(f.first.Digest()==digest&&f.first.m->boneKeys==pool&&mdl::BoneKeyCapacity(pool)==32,
            "growth preparation and allocation failures preserve live pool address capacity and all native bytes");
        if(result==MMS_OK){Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,
            "prepared growing registration publishes with allocation disabled");committed=true;break;}
        ++failures;Check(result==MMS_E_MEMORY&&!transaction&&f.history.UndoCount()==0,
            "registration preparation allocation failure leaves no journal node");
    }
    Check(committed&&failures>10&&mdl::BoneKeyCapacity(f.first.m->boneKeys)>32&&f.first.m->boneKeys!=pool,
        "allocation sweep reaches scale and pool growth before one successful commit");
    auto* const grown=f.first.m->boneKeys;const auto capacity=mdl::BoneKeyCapacity(grown);
    Check(f.first.m->boneKeys[0].next==32&&f.first.m->boneKeys[32].frame==40&&bone_scale::Key(f.first.raw(),32)==Scale(2),
        "growth publishes expected first new slot and keyed scale");
    Check(f.history.History(false)==MMS_OK&&f.first.m->boneKeys==grown&&mdl::BoneKeyCapacity(grown)==capacity&&
        SameRegistrationKeys(f.first,original)&&f.first.m->boneKeys[32].frame==0&&bone_scale::Identity(bone_scale::Key(f.first.raw(),32)),
        "undo restores authored keys and scales without shrinking successful pool growth");
    Check(f.history.History(true)==MMS_OK&&f.first.m->boneKeys[32].frame==40,"grown registration remains redoable");
}
void RegistrationPlanFailureChecks() {
    BoneRegistrationFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
    f.Commit(panel::kReadoutPosXEdit,"5");Check(f.history.History(false)==MMS_OK,"seed redo before failed registration planning");
    f.app->CurrentFrame()=10;f.app->SceneModified()=0;f.app->RecordedFrameCountByte1()=0;
    const auto before=f.first.Digest();unsigned failures=0;bool succeeded=false;
    for(long limit=0;limit<512;++limit) {
        failAfter=limit;f.Command();failAfter=-1;
        if(f.history.UndoCount()){succeeded=true;break;}
        ++failures;Check(f.first.Digest()==before&&f.history.RedoCount()==1&&f.app->SceneModified()==0&&
            f.app->RecordedFrameCountByte1()==0&&f.SelectionUnchanged(),
            "real register allocation failure preserves authoring legacy ring redo and UI metadata");
    }
    Check(succeeded&&failures>10&&f.history.UndoCount()==1&&f.history.RedoCount()==0,
        "real registration allocation sweep reaches owned capture and journal preparation before branching redo once");
}
}
void NativeBoneRegistrationChecks() {
    RegistrationProgress("legacy parity");RegistrationLegacyParityChecks();
    RegistrationProgress("curve and ring");RegistrationCurveAndRingChecks();
    RegistrationProgress("command and cross-model");RegistrationCommandAndCrossModelChecks();
    RegistrationProgress("no-op and rejection");RegistrationNoOpAndRejectionChecks();
    RegistrationProgress("stale candidates");RegistrationStaleChecks();
    RegistrationProgress("growth and failure");RegistrationGrowthAndFailureChecks();
    RegistrationProgress("plan failure");RegistrationPlanFailureChecks();
}
