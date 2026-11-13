// Include after native_signed_delete_model_command_checks.inc.
namespace signed_delete_selector_checks {
namespace command=native_signed_delete_model_command_checks;
namespace n=negative_model_delete_target_checks;
namespace m=model_delete_target_checks;
using Mode=plugins::SharedHistory::RecordingMode;
struct Fixture:command::Fixture {
    std::array<mdl::BoneOrderEntry,2> selectors{};
    void Install() {
        selectors[0]=firstOrder[0];selectors[0].boneIndex=-1;
        selectors[1]={1,0,0,1,0};
        first.m->boneOrderTable=selectors.data();first.m->boneOrderCount=2;
        first.bones[1].type=mdl::BoneType::Move;first.bones[1].slotIndex=1;
        for(std::size_t slot=0;slot<mdl::KeyPoolCapacity(first.m->displayKeys);++slot){
            auto& key=first.m->displayKeys[slot];
            auto next=std::make_unique<mdl::BoneReference[]>(2);
            next[0]=key.selectorStates[0];next[1]={slot==1?-1:1,0};
            delete[] key.selectorStates;key.selectorStates=next.release();
        }
        auto display=DisplayMotionSnapshot::Capture(first.raw());
        std::int32_t mapped=-2;
        Check(display->SelectorCount()==2 && display->SelectorBone(0,mapped) && mapped==-1 &&
            display->SelectorBone(1,mapped) && mapped==1,
            "valid selector fixture has whole-model root zero and a mapped per-bone slot one");
    }
    ~Fixture(){
        history.Reset();history.Collect();
        first.m->boneOrderTable=firstOrder.data();
        // Nested key buffers are still two-element owned arrays; base teardown
        // uses delete[] and does not rely on the model's selector count.
        first.m->boneOrderCount=1;
    }
};
void Seed(Fixture& f,bool display,command::Selection selection,int frame){
    command::Seed(f,display,false,selection);f.Install();
    n::Owner owner(f.app.get());
    if(display){
        f.app->CurrentFrame()=-10;f.selectors[1].linkedModel=-1;
        Check(negative_frames::RegisterDisplay(f.first.raw(),-10) &&
            negative_frames::SelectCell(f.app.get(),-10,0,false),
            "negative display source contains both valid selectors");
        pmm_extension::Document document;negative_frames::Capture(*f.app,document);
        Check(negative_frames::Load(*f.app,document),"clear only selector fixture private setup history");
    }
    command::coordinator::Frame(f,frame);
    f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
}
void Match(Fixture& f,Fixture& expected){
    command::MatchesOracle(f,expected);
    Check(!std::memcmp(f.selectors.data(),expected.selectors.data(),sizeof(f.selectors)),
        "both whole-model and per-bone parent identities and window endpoints match the typed oracle");
    // Reevaluate the oracle through the ordinary production seek as a separate
    // check on the prepared pose, not a copy of the interpolation algorithm.
    const auto before=expected.first.bones[1];
    const auto scale=bone_scale::Pose(expected.first.raw(),1);
    const auto parents=expected.selectors;
    command::coordinator::Frame(expected,expected.app->CurrentFrame());
    for(unsigned lane=0;lane<3;++lane)Check(global_delete_target_checks::Close(before.trans[lane],expected.first.bones[1].trans[lane]),
        "prepared selector bone translation agrees with ordinary signed seek");
    for(unsigned lane=0;lane<4;++lane)Check(global_delete_target_checks::Close(before.rotQuat[lane],expected.first.bones[1].rotQuat[lane]),
        "prepared selector bone rotation agrees with ordinary signed seek");
    Check(scale==bone_scale::Pose(expected.first.raw(),1) &&
        !std::memcmp(parents.data(),expected.selectors.data(),sizeof(parents)),
        "prepared scale and both selector windows agree with ordinary signed seek");
}
void Directions(){
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(bool display:{false,true})
    for(auto selection:{command::Selection::Mixed,command::Selection::NegativeOnly})for(int frame:{-5,0,17}){
        std::printf("  signed selector mode=%d display=%d selection=%d frame=%d\n",int(mode),display,int(selection),frame);
        Fixture oracle,f;Seed(oracle,display,selection,frame);Seed(f,display,selection,frame);
        Check(f.history.SetMode(mode)==MMS_OK,"valid selector command uses the real configured history mode");
        command::Oracle expected(oracle,false,false,selection==command::Selection::Mixed,true);
        expected.Publish(true,true);
        n::Owner owner(f.app.get());plugins::NativePoseTestBinding binding(*f.app,f.history);
        const ModelKeyState original(f.first);const auto negative=NegativeTestRecord(*f.app,negative_frames::kPmmKey);
        command::Delete(f,command::Route::Menu423);command::HistoryPosition(f,true);Match(f,oracle);
        for(unsigned step=0;step<4;++step){
            const bool redo=step%2!=0;const int at=step<2?(frame<0?17+int(step):-7+int(step)):frame;
            command::coordinator::Frame(f,at);command::coordinator::Frame(oracle,at);expected.Publish(redo,false);
            CmdControl400(f.app.get(),f.app->MainWindow(),redo?401:400,0);
            command::HistoryPosition(f,redo);Match(f,oracle);
            if(!redo)Check(original.Matches(f.first) && negative==NegativeTestRecord(*f.app,negative_frames::kPmmKey),
                "one undo restores full native nested payloads and all negative selector records");
        }
        command::NoPrivateHistory(f);
    }
}
void Stale(){
    for(unsigned mutation=0;mutation<3;++mutation){
        Fixture f;Seed(f,true,command::Selection::Mixed,-5);
        n::Owner owner(f.app.get());plugins::NativePoseTestBinding binding(*f.app,f.history);
        auto change=command::coordinator::Plan(f);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("selector source guard",change,token)==MMS_OK && token,
            "prepare before an external selector mapping or payload edit");
        if(mutation==0)f.selectors[1].boneIndex=0;
        if(mutation==1)f.first.m->displayKeys[2].selectorStates[1].modelIndex=-1;
        if(mutation==2)f.first.bones[1].slotIndex=-1;
        const n::Snapshot external(f);const auto selectors=f.selectors;
        Check(f.history.CommitNative(std::move(token))==MMS_E_STALE && external.Matches(f) &&
            !std::memcmp(selectors.data(),f.selectors.data(),sizeof(selectors)),
            "changed per-bone mapping nested parent or slot rejects both halves without overwriting the external edit");
    }
}
}
void NativeSignedDeleteSelectorChecks(){signed_delete_selector_checks::Directions();signed_delete_selector_checks::Stale();}
