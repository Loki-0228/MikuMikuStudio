// Include after native_model_viewport_history_checks.inc. These checks use
// its real pump samples and NumpadFixture, including a separate unbound oracle.
namespace {
void SameModelFollowSetup(NumpadFixture& f){
    f.Branch(2,CameraAttachmentReference::SelectedBone);
    f.app->state.followCameraEnabled=1;f.app->CameraParentModel()=0;f.app->CameraParentBone()=0;
    f.app->state.viewDirty=0;f.app->state.b6568482=0;f.app->WindowLayoutReady()=1;
    // Deliberately unregistered poses/physics flags must survive navigation.
    f.first.bones[0].trans[0]=3.125f;f.first.bones[0].rotQuat[1]=.6f;f.first.bones[0].rotQuat[3]=.8f;
    f.first.bones[1].trans[2]=-9.75f;f.first.bones[1].physicsDisabled=1;f.first.dirty={1,1};
}
void SameModelFollowReleaseOracle(NumpadFixture& f,UINT button){
    if(button==WM_RBUTTONDOWN){f.app->state.rightMouseButtonState=2;ConsumeRightButtonDrag(f.app.get());}
    else{f.app->state.middleMouseButtonState=2;ConsumeMiddleButtonPan(f.app.get());}
    f.app->state.rightMouseButtonState=f.app->state.middleMouseButtonState=0;
}
void SameModelFollowNativeMath(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(UINT button:{UINT(WM_RBUTTONDOWN),UINT(WM_MBUTTONDOWN)})for(unsigned modifiers=0;modifiers<4;++modifiers)
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        NumpadFixture f,oracle;SameModelFollowSetup(f);SameModelFollowSetup(oracle);
        const auto before=NumpadRead(*f.app);const auto first=f.first.Digest(),second=f.second.Digest();
        const auto keys=GlobalRegistrationKeys(f.app->CameraKeys());const auto ring=PoseRingDigest(f.first);
        // Keep the legacy oracle unbound; the other app's binding is explicit.
        plugins::NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"configure matched-follow recording policy");
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,button)==MMS_OK,"matched-follow gesture reserves before any original hint or numeric write");
        for(unsigned sample=0;sample<4;++sample){
            const int dx=sample&1?-7:13,dy=sample&1?11:-5;
            ModelViewportSample(oracle,button,modifiers,dx,dy);
            ModelViewportSample(f,button,modifiers,dx,dy);
            Check(plugins::AcceptViewportModelPreview(*f.app)&&NumpadSame(NumpadRead(*f.app),NumpadRead(*oracle.app))&&
                f.app->state.viewDirty==oracle.app->state.viewDirty&&f.app->WindowLayoutReady()==oracle.app->WindowLayoutReady(),
                "matched-follow held samples reproduce the actual unbound RMB hint/MMB float math with Shift before Ctrl");
            Check(!f.history.UndoCount()&&f.first.Digest()==first&&f.second.Digest()==second,
                "intermediate matched-follow input changes neither authored bone pose nor physics flags or partial history");
        }
        SameModelFollowReleaseOracle(oracle,button);bool changed=true;failAfter=0;
        const auto result=plugins::EndViewportModelGesture(*f.app,changed);failAfter=-1;
        const bool pan=button==WM_MBUTTONDOWN;
        Check(result==MMS_OK&&changed==pan&&NumpadSame(NumpadRead(*f.app),NumpadRead(*oracle.app))&&
            !f.app->state.viewDirty&&!f.app->state.b6568482&&f.app->WindowLayoutReady()==1,
            "release runs the actual original hint cleanup and publishes the complete pan without allocation");
        Check(f.history.UndoCount()==(pan&&mode==Mode::AllOperations?1:0)&&f.first.Digest()==first&&f.second.Digest()==second&&
            GlobalRegistrationSame(f.app->CameraKeys(),keys)&&PoseRingDigest(f.first)==ring,
            "only a real numeric pan records one adjustment while unregistered bones, keys, physics and legacy rings remain exact");
        if(pan&&mode==Mode::AllOperations){
            const auto after=NumpadRead(*f.app);auto undo=before,redo=after;
            f.app->CameraFov()+=.125f;f.app->CameraPositionX()+=11;
            undo.values[9]=redo.values[9]=f.app->CameraFov();undo.values[0]=redo.values[0]=f.app->CameraPositionX();
            f.app->SelectedModelSlot()=1;f.app->CurrentFrame()=59;f.app->CameraMode()=1;f.app->state.followCameraEnabled=0;
            f.second.bones[0].trans[1]=17.25f;const auto liveFirst=f.first.Digest(),liveSecond=f.second.Digest();f.app->PhysicsResetPending()=0;
            Check(plugins::RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK&&NumpadSame(NumpadRead(*f.app),undo)&&
                f.app->CurrentFrame()==59&&f.first.Digest()==liveFirst&&f.second.Digest()==liveSecond&&!f.app->PhysicsResetPending(),
                "real guarded undo crosses selected model and frame without discarding unregistered pose or settling physics");
            Check(plugins::RunBoundObjectHistory(*f.app,f.history,true)==MMS_OK&&NumpadSame(NumpadRead(*f.app),redo)&&
                f.first.Digest()==liveFirst&&f.second.Digest()==liveSecond&&!f.app->PhysicsResetPending(),
                "real guarded redo restores only captured pan values without evaluating any bone or current-frame source");
        }
    }
}
void SameModelFollowHintsAndRedo(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        NumpadFixture f;SameModelFollowSetup(f);bool changed=false;
        Check(plugins::AdjustViewportModelWheel(*f.app,f.history,120,true,changed)==MMS_OK&&changed&&f.history.History(false)==MMS_OK,
            "seed matched-follow detached-wheel redo before hint-only input");
        Check(f.history.SetMode(mode)==MMS_OK,"configure hint-only adjustment recording mode");
        plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=NumpadRead(*f.app);
        const auto first=f.first.Digest(),second=f.second.Digest();const auto bytes=f.history.RetainedBytes();
        std::uint64_t revision=0;f.history.Revision(revision);f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
        for(int delta:{-32768,-120,1,120,32767}){
            changed=true;failAfter=0;const auto result=plugins::AdjustViewportModelWheel(*f.app,f.history,std::int16_t(delta),false,changed);failAfter=-1;
            std::uint64_t afterRevision=0;f.history.Revision(afterRevision);
            Check(result==MMS_OK&&!changed&&NumpadSame(NumpadRead(*f.app),before)&&f.app->state.viewDirty==1&&
                f.app->state.b6568482==1&&f.app->WindowLayoutReady()==1&&afterRevision==revision&&f.history.RetainedBytes()==bytes&&
                !f.history.UndoCount()&&f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&
                f.first.Digest()==first&&f.second.Digest()==second,
                "native main-window matched-follow wheel changes only its two hints and preserves redo revision dirty and author data");
        }
        f.app->state.viewDirty=0;f.app->state.b6568482=0;
        changed=true;failAfter=0;const auto zero=plugins::AdjustViewportModelWheel(*f.app,f.history,0,false,changed);failAfter=-1;
        Check(zero==MMS_OK&&!changed&&!f.app->state.viewDirty&&!f.app->state.b6568482&&f.history.RedoCount()==1,
            "zero matched-follow wheel is an allocation-free no-op including hint state");
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"begin hint-only right drag with an existing redo branch");
        ModelViewportSample(f,WM_RBUTTONDOWN,3,31,-11);
        Check(f.app->state.viewDirty==1,"right drag still displays the original trace-release hint");
        changed=true;Check(plugins::EndViewportModelGesture(*f.app,changed)==MMS_OK&&!changed&&!f.app->state.viewDirty&&
            NumpadSame(NumpadRead(*f.app),before)&&f.history.RedoCount()==1&&!f.history.UndoCount(),
            "hint-only RMB release clears its hint without manufacturing an empty journal entry or trimming redo");
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN)==MMS_OK,"begin matched-follow zero-displacement middle gesture");
        ModelViewportSample(f,WM_MBUTTONDOWN,0,0,0);changed=true;
        Check(plugins::EndViewportModelGesture(*f.app,changed)==MMS_OK&&!changed&&!f.app->state.viewDirty&&f.history.RedoCount()==1,
            "zero-displacement matched-follow pan cancels its reserved node and runs native release cleanup");
    }
}
void SameModelFollowCancelAndForeign(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(UINT button:{UINT(WM_RBUTTONDOWN),UINT(WM_MBUTTONDOWN)})
    for(UINT message:{UINT(WM_CANCELMODE),UINT(WM_KILLFOCUS),UINT(WM_CAPTURECHANGED),UINT(WM_KEYDOWN),UINT(WM_ACTIVATEAPP),UINT(WM_NCDESTROY)}){
        NumpadFixture f;SameModelFollowSetup(f);f.history.SetMode(mode);plugins::NativePoseTestBinding binding(*f.app,f.history);
        const auto before=NumpadRead(*f.app);const auto first=f.first.Digest(),second=f.second.Digest();
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,button)==MMS_OK,"begin matched-follow lifecycle cancellation");
        ModelViewportSample(f,button,1,13,7);failAfter=0;
        plugins::HandleBoundViewportMessage(*f.app,nullptr,message,message==WM_KEYDOWN?VK_ESCAPE:0,message==WM_CAPTURECHANGED?LPARAM(1):0);failAfter=-1;
        Check(NumpadSame(NumpadRead(*f.app),before)&&!f.app->state.viewDirty&&!f.app->state.b6568482&&f.app->WindowLayoutReady()==1&&
            !f.history.UndoCount()&&!f.history.RetainedBytes()&&f.first.Digest()==first&&f.second.Digest()==second,
            "matched-follow cancellation restores owned pan and transient hints on focus capture Escape deactivate and destruction without allocation");
    }
    for(unsigned context=0;context<6;++context){
        NumpadFixture f;SameModelFollowSetup(f);plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=NumpadRead(*f.app);
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN)==MMS_OK,"begin followed pan before independent UI or foreign numeric edit");
        ModelViewportSample(f,WM_MBUTTONDOWN,0,13,7);auto expected=before;
        if(context==0)f.app->CameraMode()=1;
        if(context==1)f.app->CameraReferenceMode()=CameraAttachmentReference::None;
        if(context==2)f.app->SelectedModelSlot()=1;
        if(context==3){f.app->CameraFov()+=3;expected.values[9]=f.app->CameraFov();}
        if(context==4){f.app->ViewOffsetX()+=9;expected.values[7]=f.app->ViewOffsetX();}
        if(context==5){f.app->state.followCameraEnabled=0;f.app->state.viewDirty=2;}
        Check(!plugins::AllowViewportModelPreview(*f.app,WM_MBUTTONDOWN)&&NumpadSame(NumpadRead(*f.app),expected)&&
            f.app->state.viewDirty==(context==5?2:0)&&!f.history.UndoCount(),
            "UI context cancels its still-owned pan fields while foreign camera fields and a foreign hint value remain intact");
    }
    for(unsigned invalid=0;invalid<5;++invalid){
        NumpadFixture f;SameModelFollowSetup(f);plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN)==MMS_OK,"begin followed pan before evaluation-domain replacement");
        ModelViewportSample(f,WM_MBUTTONDOWN,0,13,7);
        const auto keys=f.app->CameraKeys();auto* replacement=mdl::KeyPoolCreate<mdl::CameraKey>(4);const auto bones=f.first.m->boneTable;
        if(invalid==0)++f.app->CurrentFrame();
        if(invalid==1)f.app->CameraKeys()=replacement;
        if(invalid==2)f.app->CameraParentBone()=1;
        if(invalid==3)f.first.m->boneTable=bones+1;
        if(invalid==4){plugins::ForgetSceneObject(f.first.raw());plugins::SceneObjectIdentity(f.first.raw());}
        const auto foreign=NumpadRead(*f.app);
        Check(!plugins::AllowViewportModelPreview(*f.app,WM_MBUTTONDOWN)&&NumpadSame(NumpadRead(*f.app),foreign)&&
            !f.app->state.viewDirty&&!f.history.UndoCount(),
            "new frame pool attachment bone table or object identity owns a different evaluation domain and is never overwritten by cancellation");
        f.app->CameraKeys()=keys;mdl::KeyPoolDestroy(replacement);f.first.m->boneTable=bones;
    }
}
void SameModelFollowMessageAndBinding(){
    for(bool detached:{false,true})for(UINT button:{UINT(WM_RBUTTONDOWN),UINT(WM_MBUTTONDOWN)}){
        NumpadFixture f;SameModelFollowSetup(f);plugins::NativePoseTestBinding binding(*f.app,f.history);
        const auto owner=f.app->MainWindow();if(detached)f.app->state.floatingWindow=owner;
        f.app->ViewportRect()={10,10,700,500};const auto before=NumpadRead(*f.app);
        Check(plugins::HandleBoundViewportMessage(*f.app,owner,button,0,MAKELPARAM(100,100))&&GetCapture()==owner,
            "actual bound main/detached message dispatcher admits a followed gesture and captures its owner");
        plugins::HandleBoundViewportMessage(*f.app,owner,WM_MOUSEMOVE,0,MAKELPARAM(113,107));
        plugins::HandleBoundViewportMessage(*f.app,owner,button==WM_RBUTTONDOWN?WM_RBUTTONUP:WM_MBUTTONUP,0,MAKELPARAM(119,111));
        auto expected=before;if(button==WM_MBUTTONDOWN){expected=ModelViewportOracle(expected,button,0,13,7);expected=ModelViewportOracle(expected,button,0,6,4);}
        Check(NumpadSame(NumpadRead(*f.app),expected)&&GetCapture()!=owner&&!f.app->state.viewDirty&&
            f.history.UndoCount()==(button==WM_MBUTTONDOWN?1:0),
            "actual message release includes its final coalesced pan sample once and clears the native follow hint and capture");
        f.app->state.floatingWindow=nullptr;
    }
    NumpadFixture f;SameModelFollowSetup(f);plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=NumpadRead(*f.app);
    Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN)==MMS_OK,"begin followed pan before nested journal binding");
    ModelViewportSample(f,WM_MBUTTONDOWN,0,13,7);
    {plugins::NativePoseTestBinding nested(*f.app,f.history);Check(NumpadSame(NumpadRead(*f.app),before)&&!f.app->state.viewDirty&&!f.history.UndoCount(),
        "rebinding cancels followed preview while its original object and journal are still alive");}
    Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN)==MMS_OK,"begin followed pan before journal reset");
    ModelViewportSample(f,WM_MBUTTONDOWN,0,13,7);Check(f.history.Reset()==MMS_OK,"invalidate followed pan's prepared publication with journal reset");bool changed=true;
    Check(plugins::EndViewportModelGesture(*f.app,changed)==MMS_E_STALE&&!changed&&NumpadSame(NumpadRead(*f.app),before)&&
        !f.app->state.viewDirty&&!f.history.UndoCount(),"stale followed reservation rolls back all still-owned numeric preview and transient hints");
}
void SameModelFollowFailureAndIdentity(){
    NumpadFixture f;SameModelFollowSetup(f);plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=NumpadRead(*f.app);
    const auto first=f.first.Digest(),second=f.second.Digest();bool complete=false;unsigned refused=0;
    for(long allowance=0;allowance<512;++allowance){
        failAfter=allowance;const auto result=plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN);failAfter=-1;
        Check(NumpadSame(NumpadRead(*f.app),before)&&!f.app->state.viewDirty&&!f.history.UndoCount()&&
            f.first.Digest()==first&&f.second.Digest()==second,"followed gesture allocation refusal never changes native pose view hint or journal");
        if(result==MMS_OK){complete=true;plugins::CancelViewportModelGesture(*f.app);break;}
        Check(result==MMS_E_MEMORY,"followed preparation reports actual allocation failure");++refused;
    }
    Check(complete&&refused>5,"followed OOM sweep reaches model ownership resource-ledger and journal preparation");
    plugins::StorageStore storage;plugins::SharedHistory budget(storage,{},8,1);
    Check(plugins::BeginViewportModelGesture(*f.app,budget,nullptr,WM_MBUTTONDOWN)==MMS_E_MEMORY&&
        NumpadSame(NumpadRead(*f.app),before)&&!f.app->state.viewDirty,"followed gesture budget refuses before its first native preview");
    {
        NumpadFixture conflict;SameModelFollowSetup(conflict);conflict.app->state.viewDirty=1;
        unsigned calls=0;plugins::StorageStore conflictStorage;
        plugins::SharedHistory conflictHistory(conflictStorage,[&]{if(++calls==3)conflict.app->CameraFov()+=3;return MMS_OK;});
        plugins::NativePoseTestBinding conflictBinding(*conflict.app,conflictHistory);auto expected=NumpadRead(*conflict.app);
        Check(plugins::BeginViewportModelGesture(*conflict.app,conflictHistory,nullptr,WM_MBUTTONDOWN)==MMS_OK,
            "reserve followed pan before a final publication callback changes an independent field");
        ModelViewportSample(conflict,WM_MBUTTONDOWN,0,13,7);bool changed=true;
        const auto result=plugins::EndViewportModelGesture(*conflict.app,changed);expected.values[9]+=3;
        Check(result==MMS_E_STALE&&!changed&&NumpadSame(NumpadRead(*conflict.app),expected)&&conflict.app->state.viewDirty==1&&
            !conflictHistory.UndoCount()&&!conflictHistory.RetainedBytes(),
            "late publication conflict restores still-owned pan and preexisting hint even after native release while preserving foreign FOV");
    }
    for(unsigned stale=0;stale<3;++stale){
        NumpadFixture replay;SameModelFollowSetup(replay);plugins::NativePoseTestBinding replayBinding(*replay.app,replay.history);
        Check(plugins::BeginViewportModelGesture(*replay.app,replay.history,nullptr,WM_MBUTTONDOWN)==MMS_OK,"begin followed replay identity fixture");
        ModelViewportSample(replay,WM_MBUTTONDOWN,0,13,7);bool changed=false;
        Check(plugins::EndViewportModelGesture(*replay.app,changed)==MMS_OK&&changed,"publish followed pan with its stable parent retained");
        const auto bones=replay.first.m->boneTable;
        if(stale==0)replay.app->CameraParentBone()=1;
        if(stale==1)replay.first.m->boneTable=bones+1;
        if(stale==2){plugins::ForgetSceneObject(replay.first.raw());plugins::SceneObjectIdentity(replay.first.raw());}
        const auto foreign=NumpadRead(*replay.app);
        Check(replay.history.History(false)==MMS_E_STALE&&NumpadSame(NumpadRead(*replay.app),foreign)&&replay.history.UndoCount()==1,
            "followed undo refuses replacement attachment table or parent identity and retains its original record");
        replay.first.m->boneTable=bones;
    }
}
struct SameModelFollowReentrantWindow{
    inline static thread_local SameModelFollowReentrantWindow* active=nullptr;
    NumpadFixture& fixture;HWND owner,button,other,floating;WNDPROC ownerProc,buttonProc;
    bool armed=false,observed=false,restart=false;unsigned foreign=0;NumpadView native{},foreignView{};
    std::shared_ptr<const plugins::HistoryChange> restarted;
    explicit SameModelFollowReentrantWindow(NumpadFixture& f):fixture(f),owner(f.app->MainWindow()),button(nullptr),other(nullptr),
        floating(f.app->FloatingWindow()),ownerProc(nullptr),buttonProc(nullptr){
        Check(!active,"matched-follow reentrant control fixture has one owner");active=this;
        ownerProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(owner,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(MicWndProc)));
        Check(ownerProc!=nullptr,"matched-follow reentrant fixture installs the actual detached viewport procedure");f.app->FloatingWindow()=owner;
        button=CreateWindowExW(0,L"BUTTON",L"force paste",WS_CHILD,0,0,80,20,owner,
            reinterpret_cast<HMENU>(INT_PTR(panel::kForcePasteButton)),GetModuleHandleW(nullptr),nullptr);
        other=CreateWindowExW(0,L"STATIC",L"matched follow capture replacement",WS_OVERLAPPED,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(button&&other,"create actual force-paste button and foreign capture owner");
        buttonProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(button,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ButtonProc)));
        Check(buttonProc!=nullptr,"subclass actual WM_ENABLE notification while forwarding native button behavior");
        f.app->ClipboardCounts().bones=f.app->ClipboardCounts().morphs=0;f.app->ViewportRect()={10,10,700,500};
    }
    ~SameModelFollowReentrantWindow(){
        armed=false;plugins::CancelViewportModelGesture(*fixture.app);
        if(GetCapture()==owner||GetCapture()==other)ReleaseCapture();
        SetWindowLongPtrW(button,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(buttonProc));DestroyWindow(button);DestroyWindow(other);
        SetWindowLongPtrW(owner,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ownerProc));fixture.app->FloatingWindow()=floating;active=nullptr;
    }
    static LRESULT CALLBACK ButtonProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        auto* self=active;const auto original=self->buttonProc;
        if(message==WM_ENABLE&&self->armed){
            self->armed=false;self->observed=true;
            Check(!wp&&NumpadSame(NumpadRead(*self->fixture.app),self->native),
                "real WM_ENABLE runs after the complete native pan write and before synchronous capture cancellation");
            if(self->foreign==1)self->fixture.app->ViewOffsetX()+=9.f;
            if(self->foreign==2)self->fixture.app->ViewOffsetY()+=9.f;
            if(self->foreign==3)self->fixture.app->CameraFov()+=3.f;
            self->foreignView=NumpadRead(*self->fixture.app);
            SetCapture(self->other);
            if(self->restart){
                ReleaseCapture();
                Check(plugins::HandleBoundViewportMessage(*self->fixture.app,self->owner,WM_RBUTTONDOWN,0,MAKELPARAM(200,120))&&GetCapture()==self->owner,
                    "native refresh callback can cancel its old middle gesture and start a distinct captured right gesture");
                self->fixture.app->state.rightMouseButtonState=3;
                self->restarted=plugins::ViewportModelFollowRefreshSource(*self->fixture.app);
                Check(bool(self->restarted),"callback retains the new gesture's exact source identity");
            }
        }
        return CallWindowProcW(original,hwnd,message,wp,lp);
    }
    void Arm(const NumpadView& expected,unsigned independent){EnableWindow(button,TRUE);observed=false;native=expected;foreign=independent;armed=true;}
};
void SameModelFollowReentrantCancellation(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(unsigned layout:{0u,1u})
    for(unsigned foreign=0;foreign<4;++foreign)for(unsigned phase=0;phase<3;++phase){
        NumpadFixture f;SameModelFollowSetup(f);SameModelFollowReentrantWindow window(f);bool changed=false;
        Check(plugins::AdjustViewportModelWheel(*f.app,f.history,120,true,changed)==MMS_OK&&changed&&f.history.History(false)==MMS_OK,
            "seed a real redo branch before a sample-internal follow cancellation");
        Check(f.history.SetMode(mode)==MMS_OK,"configure sample-internal cancellation recording mode");
        plugins::NativePoseTestBinding binding(*f.app,f.history);f.app->WindowLayoutReady()=std::uint8_t(layout);f.app->state.viewDirty=phase==2?1:0;
        const auto before=NumpadRead(*f.app);const auto first=f.first.Digest(),second=f.second.Digest();const auto keys=GlobalRegistrationKeys(f.app->CameraKeys());
        const auto bytes=f.history.RetainedBytes();std::uint64_t revision=0;f.history.Revision(revision);
        Check(plugins::HandleBoundViewportMessage(*f.app,window.owner,WM_MBUTTONDOWN,0,MAKELPARAM(100,100))&&GetCapture()==window.owner,
            "begin actual captured middle gesture before synchronous native control cancellation");
        auto native=before;
        if(phase){plugins::HandleBoundViewportMessage(*f.app,window.owner,WM_MOUSEMOVE,0,MAKELPARAM(113,107));native=ModelViewportOracle(native,WM_MBUTTONDOWN,0,13,7);}
        if(phase<2)native=ModelViewportOracle(native,WM_MBUTTONDOWN,0,phase?-7:13,phase?11:7);
        window.Arm(native,foreign);changed=true;failAfter=0;
        if(phase<2)plugins::HandleBoundViewportMessage(*f.app,window.owner,WM_MOUSEMOVE,0,phase?MAKELPARAM(106,118):MAKELPARAM(113,107));
        else Check(plugins::EndViewportModelGesture(*f.app,changed)==MMS_E_STALE&&!changed,
            "real native release reports synchronous capture cancellation instead of publishing a partial gesture");
        failAfter=-1;
        auto expected=before;if(foreign==1)expected.values[7]=window.foreignView.values[7];
        if(foreign==2)expected.values[8]=window.foreignView.values[8];if(foreign==3)expected.values[9]=window.foreignView.values[9];
        std::uint64_t afterRevision=0;f.history.Revision(afterRevision);
        Check(window.observed&&GetCapture()==window.other&&NumpadSame(NumpadRead(*f.app),expected)&&
            f.app->state.viewDirty==unsigned(phase==2)&&f.app->WindowLayoutReady()==layout&&
            !plugins::AcceptViewportModelPreview(*f.app)&&!f.history.UndoCount()&&f.history.RedoCount()==1&&
            f.history.RetainedBytes()==bytes&&afterRevision==revision&&f.first.Digest()==first&&f.second.Digest()==second&&
            GlobalRegistrationSame(f.app->CameraKeys(),keys),
            "sample-internal WM_ENABLE capture transfer restores every actually-owned pan/hint, protects foreign axes/FOV and keeps capture, redo, budget, models and keys");
        ReleaseCapture();
    }
    // Allow is permission to run the native code, not proof that its stores
    // happened. An independent field written before the pump runs stays foreign.
    NumpadFixture f;SameModelFollowSetup(f);plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=NumpadRead(*f.app);
    Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN)==MMS_OK&&
        plugins::AllowViewportModelPreview(*f.app,WM_MBUTTONDOWN),"allow a follow sample without executing any native math");
    f.app->ViewOffsetX()+=9.f;const auto foreign=NumpadRead(*f.app);plugins::CancelViewportModelGesture(*f.app);
    Check(NumpadSame(NumpadRead(*f.app),foreign)&&f.app->ViewOffsetY()==before.values[8]&&!f.history.UndoCount(),
        "cancellation never assumes an allowed but unexecuted sample already owns an independent author's offset");
    {
        NumpadFixture replacement;SameModelFollowSetup(replacement);SameModelFollowReentrantWindow window(replacement);
        plugins::NativePoseTestBinding replacementBinding(*replacement.app,replacement.history);const auto original=NumpadRead(*replacement.app);
        Check(plugins::HandleBoundViewportMessage(*replacement.app,window.owner,WM_MBUTTONDOWN,0,MAKELPARAM(100,100)),
            "begin the old middle gesture before a release callback replaces it");
        plugins::HandleBoundViewportMessage(*replacement.app,window.owner,WM_MOUSEMOVE,0,MAKELPARAM(113,107));
        const auto old=plugins::ViewportModelFollowRefreshSource(*replacement.app);window.restart=true;window.Arm(NumpadRead(*replacement.app),0);bool changed=true;
        Check(plugins::EndViewportModelGesture(*replacement.app,changed)==MMS_E_STALE&&!changed&&window.observed&&window.restarted!=old&&
            plugins::ViewportModelFollowRefreshSource(*replacement.app)==window.restarted&&GetCapture()==window.owner&&
            replacement.app->state.rightMouseButtonState==3&&!replacement.app->state.middleMouseButtonState&&
            replacement.app->MouseX()==200&&replacement.app->MouseY()==120&&NumpadSame(NumpadRead(*replacement.app),original)&&
            !replacement.history.UndoCount()&&replacement.history.SetMode(Mode::ExplicitOnly)==MMS_E_BUSY,
            "old release cannot move or commit a callback's replacement gesture, clear its button state, release its capture or reuse its identity");
        plugins::CancelViewportModelGesture(*replacement.app);
    }
}
void NativeSameModelFollowChecks(){
    SameModelFollowNativeMath();SameModelFollowHintsAndRedo();SameModelFollowCancelAndForeign();
    SameModelFollowMessageAndBinding();SameModelFollowFailureAndIdentity();SameModelFollowReentrantCancellation();
}
}
