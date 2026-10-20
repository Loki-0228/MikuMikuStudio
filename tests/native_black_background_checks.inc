// Include after native_grid_display_checks.inc (and its camera/plugin fixtures).
// Uses hidden HWNDs and explicit letter snapshots, never global keyboard input.
#include "../src/plugins/native_global_commands.hpp"
#include "../src/plugins/native_history_commands.hpp"
#include "mikumikustudio/plugin_input_focus.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include <MinHook.h>
namespace mikumikustudio { void DispatchLetterHotkeys(MMDApp*,HWND); }
namespace black_background_checks {
using namespace plugins;
using Mode=SharedHistory::RecordingMode;
struct Fixture : GridDisplayFixture {
    Fixture() {
        Check(AppendMenuW(menu,MF_STRING,282,L"black background"),"create the real black-background menu id");
        app->CurrentFrame()=37;app->state.timeNowHigh=0x34567890;
        first.bones[0].trans[0]=7.1234567f;second.bones[1].trans[2]=-9.25f;
        app->CameraPositionX()=12.3125f;app->CameraDistance()=-81.5f;
        app->PhysicsResetPending()=app->SceneModified()=app->RecordedFrameCountByte1()=0;
    }
    bool Echo() const {
        const auto flags=GetMenuState(menu,282,MF_BYCOMMAND);
        return flags!=UINT(-1)&&bool(flags&MF_CHECKED)==(app->state.blackBackgroundEnabled!=0);
    }
    void PressB(HWND focus) {
        unsigned char keys[256]{};app->LetterHotkeys().Update(keys,true);
        keys['B']=0x80;app->LetterHotkeys().Update(keys,true);DispatchLetterHotkeys(app.get(),focus);
    }
    void Toggle(unsigned route) {
        if(route==0)CommandDispatch(nullptr,MAKEWPARAM(282,0));
        else PressB(GetDlgItem(app->MainWindow(),panel::kUndoButton));
    }
    void Readout() { RefreshBoundGlobalReadout(*app); }
};
struct Source {
    Fixture& f;
    const std::uint64_t first,second,firstRing,secondRing;
    const std::array<float,7> camera;
    const mdl::AccessoryRecord accessory;
    const std::vector<mdl::CameraKey> cameraKeys;
    const std::vector<mdl::LightKey> lightKeys;
    const std::vector<mdl::SelfShadowKey> shadowKeys;
    const std::vector<mdl::GravityKey> gravityKeys;
    const std::vector<unsigned char> marks;
    const std::array<unsigned char,3> display;
    const int frame;
    const unsigned char reset,enhanced;
    const std::uint32_t clock;
    explicit Source(Fixture& value) : f(value),first(f.first.Digest()),second(f.second.Digest()),
        firstRing(PoseRingDigest(f.first)),secondRing(PoseRingDigest(f.second)),camera(NumericCameraPose(*f.app)),
        accessory(f.accessory),cameraKeys(GlobalRegistrationKeys(f.app->CameraKeys())),
        lightKeys(GlobalRegistrationKeys(f.app->LightKeys())),shadowKeys(GlobalRegistrationKeys(f.app->ShadowKeys())),
        gravityKeys(GlobalRegistrationKeys(f.app->GravityKeys())),marks(AccessoryRegistrationMarks(*f.app)),
        display{f.app->GroundGridEnabled(),f.app->GroundShadowEnabled(),f.app->ProjectedShadowBlendEnabled()},
        frame(f.app->CurrentFrame()),reset(f.app->PhysicsResetPending()),enhanced(f.app->EnhancedModelDirty()),clock(f.app->state.timeNowHigh) {}
    void Unchanged(int expectedFrame=-1) const {
        Check(f.first.Digest()==first&&f.second.Digest()==second&&PoseRingDigest(f.first)==firstRing&&
            PoseRingDigest(f.second)==secondRing&&NumericCameraPose(*f.app)==camera&&
            !std::memcmp(&f.accessory,&accessory,sizeof accessory),"background edits preserve both unregistered model poses, camera, accessory and legacy rings");
        Check(GlobalRegistrationSame(f.app->CameraKeys(),cameraKeys)&&GlobalRegistrationSame(f.app->LightKeys(),lightKeys)&&
            GlobalRegistrationSame(f.app->ShadowKeys(),shadowKeys)&&GlobalRegistrationSame(f.app->GravityKeys(),gravityKeys)&&
            AccessoryRegistrationMarks(*f.app)==marks&&f.SelectionUnchanged(),"background edits never alter keys or native selection marks");
        Check(f.app->CurrentFrame()==(expectedFrame<0?frame:expectedFrame)&&f.app->PhysicsResetPending()==reset&&
            f.app->EnhancedModelDirty()==enhanced&&f.app->state.timeNowHigh==clock&&!f.app->cameraProjectionRefreshPending&&
            display==std::array<unsigned char,3>{f.app->GroundGridEnabled(),f.app->GroundShadowEnabled(),f.app->ProjectedShadowBlendEnabled()},
            "background edits do not seek, reset physics, change asset-save reminders, projection or other display fields");
    }
};
void RoutingAndRawBytes() {
    for(unsigned route=0;route<2;++route)for(unsigned char initial:{0,1,7,255}) {
        Fixture f;NativePoseTestBinding bound(*f.app,f.history);f.app->state.blackBackgroundEnabled=initial;
        bool unchanged=true;
        Check(ApplyBoundBlackBackground(*f.app,initial!=0,unchanged)==MMS_OK&&!unchanged&&
            f.app->state.blackBackgroundEnabled==initial&&!f.history.UndoCount()&&!f.app->SceneModified(),
            "same visual background preserves even a noncanonical raw enabled byte");
        const auto target=initial?0:1;Source source(f);f.Toggle(route);
        Check(f.app->state.blackBackgroundEnabled==target&&f.history.UndoCount()==1&&f.Echo()&&f.app->SceneModified()&&
            !f.history.LastPublicationRequiresPhysicsReset(),"real menu and B-key publish one drawing-only background adjustment");source.Unchanged();
        f.app->SelectedModelSlot()=1;f.app->CameraMode()=1;f.app->CurrentFrame()=49;
        Check(f.history.History(false)==MMS_OK&&f.app->state.blackBackgroundEnabled==initial,
            "background undo restores the exact original byte, including noncanonical enabled values");
        f.Readout();Check(f.Echo(),"common history readout restores the real background menu check");source.Unchanged(49);
        Check(f.history.History(true)==MMS_OK&&f.app->state.blackBackgroundEnabled==target,"background redo is independent of model, mode and playhead");
        f.Readout();Check(f.Echo()&&!f.history.LastPublicationRequiresPhysicsReset(),"background replay never requests a physics reset");source.Unchanged(49);
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;bool changed=true;
        Check(ApplyBoundBlackBackground(*f.app,target!=0,changed)==MMS_OK&&!changed&&f.history.UndoCount()==1&&
            !f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),"same background value has no dirty or history side effect");
        f.Toggle(1-route);Check(f.history.UndoCount()==2&&f.history.History(false)==MMS_OK,"menu and B-key share one chronological background history");
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
        Check(ApplyBoundBlackBackground(*f.app,target!=0,changed)==MMS_OK&&!changed&&f.history.RedoCount()==1&&
            !f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),"background no-op preserves an existing redo branch");
    }
}
void RecordingModes() {
    Fixture f;NativePoseTestBinding bound(*f.app,f.history);Source source(f);
    f.Toggle(0);Check(f.history.History(false)==MMS_OK&&f.history.RedoCount()==1,"seed a background redo before changing recording mode");
    std::uint64_t revision=0,next=0;Check(f.history.Revision(revision)==MMS_OK&&f.history.SetMode(Mode::ExplicitOnly)==MMS_OK,"select explicit-only background adjustments");
    f.Toggle(1);
    Check(f.app->state.blackBackgroundEnabled==1&&!f.history.UndoCount()&&f.history.RedoCount()==1&&
        f.history.Revision(next)==MMS_OK&&next>revision&&f.Echo(),"explicit-only B-key adjustment changes data and revision while retaining redo without a node");
    Check(f.history.History(true)==MMS_OK&&f.app->state.blackBackgroundEnabled==1,"existing redo remains usable after an unrecorded background adjustment");
    Check(f.history.Configure(Mode::ExplicitOnly,128,1)==MMS_OK,"configure a background history budget too small for retained nodes");
    f.Toggle(0);Check(f.app->state.blackBackgroundEnabled==0&&!f.history.UndoCount()&&!f.history.RetainedBytes(),"unrecorded menu adjustment needs no retained history budget");
    Check(f.history.Configure(Mode::AllOperations,128,1024*1024)==MMS_OK,"restore all-operations background mode");f.Toggle(1);
    Check(f.history.UndoCount()==1&&f.history.History(false)==MMS_OK&&f.app->state.blackBackgroundEnabled==0,
        "subsequent all-operations B-key adjustment records the preceding unrecorded value");source.Unchanged();
}
void HistoryUiRoutesPreserveLaterDrafts() {
    // The common UI route intentionally performs its existing read-only
    // PostViewRefresh tail. The no-PostViewRefresh assertion below concerns
    // initial background commit only, not this shared undo/redo notification.
    for(unsigned route=0;route<2;++route)for(unsigned mode=0;mode<2;++mode)for(unsigned char reset:{0,7}) {
        Fixture f;NativePoseTestBinding bound(*f.app,f.history);f.app->state.blackBackgroundEnabled=7;f.Toggle(0);
        Check(!f.app->state.blackBackgroundEnabled&&f.history.UndoCount()==1,"seed a real background node before later unregistered edits");
        f.first.bones[0].trans[1]=13.7182817f;f.first.bones[0].rotQuat[1]=.6f;f.first.bones[0].rotQuat[3]=.8f;
        f.first.dirty[0]=1;f.second.bones[1].trans[0]=-17.125f;f.second.bones[0].rotQuat[2]=.6f;f.second.bones[0].rotQuat[3]=.8f;
        f.second.dirty[1]=1;f.app->CameraPositionY()=-12.8125f;f.app->CameraRotation()[2]=.31415927f;
        f.app->CameraMode()=static_cast<unsigned char>(mode);f.app->SelectedModelSlot()=1;f.app->CurrentFrame()=83;
        f.app->PhysicsResetPending()=reset;f.app->GroundGridEnabled()=7;f.app->GroundShadowEnabled()=3;f.app->ProjectedShadowBlendEnabled()=5;
        Source later(f);
        CheckMenuItem(f.menu,282,MF_UNCHECKED);
        if(route==0)Check(TryBoundHistoryCommand(f.app.get(),false),"bound history UI accepts background undo");
        else CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
        Check(f.app->state.blackBackgroundEnabled==7&&f.history.UndoCount()==0&&f.history.RedoCount()==1&&f.Echo()&&
            !f.history.LastPublicationRequiresPhysicsReset(),"real common undo restores raw background and menu without requesting physics reset");
        Check(f.app->CameraMode()==mode&&f.app->SelectedModelSlot()==1&&
            !IsWindowEnabled(f.Edit(panel::kUndoButton))&&IsWindowEnabled(f.Edit(panel::kRedoButton)),
            "background undo updates real history controls while preserving the active model or camera mode");
        later.Unchanged();
        CheckMenuItem(f.menu,282,MF_CHECKED);
        if(route==0)Check(TryBoundHistoryCommand(f.app.get(),true),"bound history UI accepts background redo");
        else CmdControl400(f.app.get(),f.app->MainWindow(),401,0);
        Check(!f.app->state.blackBackgroundEnabled&&f.history.UndoCount()==1&&!f.history.RedoCount()&&f.Echo()&&
            !f.history.LastPublicationRequiresPhysicsReset(),"real common redo restores background and repairs menu echo without a physics reset");
        Check(f.app->CameraMode()==mode&&f.app->SelectedModelSlot()==1&&
            IsWindowEnabled(f.Edit(panel::kUndoButton))&&!IsWindowEnabled(f.Edit(panel::kRedoButton)),
            "background redo updates real history controls while retaining model selection and camera mode");
        later.Unchanged();
    }
}
struct TinyChange final : HistoryChange {
    int& live;
    explicit TinyChange(int& value) : live(value) {}
    struct Candidate final : PreparedHistoryChange {
        int& live;const int before,after;
        Candidate(int& value,bool forward):live(value),before(forward?0:1),after(forward?1:0){}
        bool IsCurrent() const noexcept override {return live==before;}
        void Publish() noexcept override {live=after;}
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out) override {
        out=std::make_unique<Candidate>(live,forward);return out->IsCurrent()?MMS_OK:MMS_E_STALE;
    }
    std::uint64_t RetainedBytes() const noexcept override {return sizeof(*this);}
    bool RequiresPhysicsReset() const noexcept override {return false;}
};
void BudgetAndAllocationFailures() {
    for(unsigned route=0;route<2;++route) {
        Fixture f;NativePoseTestBinding bound(*f.app,f.history);Source source(f);int value=0;
        std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("seed",std::make_shared<TinyChange>(value),token)==MMS_OK&&
            f.history.CommitNative(std::move(token))==MMS_OK&&f.history.History(false)==MMS_OK,"seed a smaller retained redo before background budget rejection");
        const auto budget=f.history.RetainedBytes();Check(budget&&f.history.Configure(Mode::AllOperations,128,budget)==MMS_OK,"retain the smaller redo while limiting a new background node");
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;CheckMenuItem(f.menu,282,MF_CHECKED);f.Toggle(route);
        Check(!f.app->state.blackBackgroundEnabled&&!f.history.UndoCount()&&f.history.RedoCount()==1&&f.history.RetainedBytes()==budget&&
            !f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&f.Echo()&&value==0,
            "real menu and B-key budget rejection preserve exact data, smaller redo, dirty state and menu echo");source.Unchanged();
        Check(f.history.History(true)==MMS_OK&&value==1,"the retained redo remains usable after background budget rejection");
    }
    for(unsigned route=0;route<2;++route) {
        Fixture f;NativePoseTestBinding bound(*f.app,f.history);f.app->state.blackBackgroundEnabled=7;
        f.Toggle(0);Check(f.history.History(false)==MMS_OK,"seed raw-byte background redo before allocation sweep");Source source(f);
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;const auto bytes=f.history.RetainedBytes();unsigned failures=0;bool success=false;
        for(long limit=0;limit<128;++limit) {
            CheckMenuItem(f.menu,282,MF_UNCHECKED);failAfter=limit;f.Toggle(route);failAfter=-1;
            if(f.history.UndoCount()) {success=true;break;}++failures;
            Check(f.app->state.blackBackgroundEnabled==7&&f.history.RedoCount()==1&&f.history.RetainedBytes()==bytes&&
                !f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&f.Echo(),"C++ allocation refusal preserves raw background byte, redo, menu and dirty flags");source.Unchanged();
            Check(f.history.Enter()==MMS_OK,"background failure releases its history gate");f.history.Leave();
        }
        Check(success&&failures>=5&&f.app->state.blackBackgroundEnabled==0&&!f.history.RedoCount(),"background failure sweep reaches one complete publication from both actual routes");
        Check(f.history.History(false)==MMS_OK&&f.app->state.blackBackgroundEnabled==7,"background remains exactly undoable after allocation failures");source.Unchanged();
    }
}
void GuardsAndRootIdentity() {
    for(unsigned route=0;route<2;++route)for(unsigned gate=0;gate<7;++gate) {
        Fixture f;NativePoseTestBinding bound(*f.app,f.history);Source source(f);f.app->state.blackBackgroundEnabled=7;
        if(gate==0)f.app->PlaybackActive()=1;
        if(gate==1)f.app->FrameStepPlayback()=1;
        if(gate==2)f.app->state.recordPlaybackStartPending=true;
        if(gate==3)f.app->FrameRangeDialog()=reinterpret_cast<HWND>(INT_PTR(1));
        if(gate==4)Check(f.history.Enter()==MMS_OK,"hold background journal to test a busy real command");
        if(gate==5)f.app->state.accessoryEditDialogOpen=true;
        if(gate==6)f.app->GravitySettingDialog()=reinterpret_cast<HWND>(INT_PTR(1));
        f.Toggle(route);
        Check(f.app->state.blackBackgroundEnabled==7&&!f.history.UndoCount()&&!f.app->SceneModified()&&f.Echo(),"bound playback, modal and busy rejections consume menu and B-key without legacy fallback");
        f.app->PlaybackActive()=f.app->FrameStepPlayback()=0;f.app->state.recordPlaybackStartPending=false;f.app->FrameRangeDialog()=nullptr;
        f.app->state.accessoryEditDialogOpen=false;f.app->GravitySettingDialog()=nullptr;if(gate==4)f.history.Leave();source.Unchanged();
    }
    Fixture f;NativePoseTestBinding bound(*f.app,f.history);f.Toggle(0);const auto before=GlobalRegistrationKeys(f.app->CameraKeys());
    auto* grown=mdl::KeyPoolCreate<mdl::CameraKey>(16);std::memcpy(grown,f.app->CameraKeys(),before.size()*sizeof(mdl::CameraKey));mdl::KeyPoolCommitGrowth(f.app->CameraKeys(),grown);
    Check(f.history.History(false)==MMS_OK&&f.app->state.blackBackgroundEnabled==0&&f.history.History(true)==MMS_OK,
        "background records survive registered project-root pool growth");
    mdl::KeyPoolDestroy(f.app->CameraKeys());f.app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(16);
    Check(f.history.History(false)==MMS_E_STALE&&f.app->state.blackBackgroundEnabled==1&&f.history.UndoCount()==1,
        "a replacement project root refuses stale background replay without moving history");
}
void FocusAndOrdinaryRoutes() {
    Fixture f;bool changed=true;
    Check(!TryToggleBoundBlackBackground(f.app.get())&&ApplyBoundBlackBackground(*f.app,true,changed)==MMS_E_NOT_READY&&!changed,
        "unbound background helpers leave ordinary menu and letter routes available");
    f.app->state.blackBackgroundEnabled=7;f.Toggle(0);f.PressB(nullptr);
    Check(f.app->state.blackBackgroundEnabled==1&&f.Echo()&&!f.history.UndoCount(),"ordinary unbound menu and cleared-focus B-key preserve the existing non-journal behavior");
    f.app->state.blackBackgroundEnabled=0;NativePoseTestBinding bound(*f.app,f.history);
    f.PressB(f.Edit(panel::kReadoutPosXEdit));Check(!f.app->state.blackBackgroundEnabled&&!f.history.UndoCount(),"ordinary EDIT focus keeps B-key text input out of background history");
    const auto wrapper=CreateWindowExW(WS_EX_CONTROLPARENT,L"STATIC",L"INPUT",WS_CHILD,0,0,120,40,f.app->MainWindow(),nullptr,GetModuleHandleW(nullptr),nullptr);
    const auto nested=CreateWindowExW(0,L"STATIC",L"",WS_CHILD,0,0,110,30,wrapper,nullptr,GetModuleHandleW(nullptr),nullptr);
    const auto button=CreateWindowExW(0,L"BUTTON",L"input child",WS_CHILD,0,0,100,20,nested,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(wrapper&&nested&&button&&SetPropW(wrapper,kPropertyInputWindowMarker,reinterpret_cast<HANDLE>(1)),"create a real marked INPUT subtree for background shortcut exclusion");
    for(HWND focus:{wrapper,nested,button})f.PressB(focus);
    Check(!f.app->state.blackBackgroundEnabled&&!f.history.UndoCount(),"every INPUT descendant excludes the background letter shortcut");
    RemovePropW(wrapper,kPropertyInputWindowMarker);f.PressB(button);
    Check(f.app->state.blackBackgroundEnabled==1&&f.history.UndoCount()==1,"unmarking INPUT restores the existing eligible-button B-key route");
    DestroyWindow(wrapper);Check(f.history.History(false)==MMS_OK,"undo the restored eligible background shortcut");
    f.PressB(nullptr);Check(f.app->state.blackBackgroundEnabled==1&&f.history.UndoCount()==1,"cleared focus remains eligible for a bound B-key adjustment");
}
void CrossModelAndPluginOrder() {
    Fixture f;NativePoseTestBinding bound(*f.app,f.history);
    // The inherited numeric EDITs belong to the main window, whereas the
    // display fixture also owns a floating viewport. Match the numeric owner
    // only for that real commit, then restore the floating shortcut context.
    const auto commitMainPanel=[&](int id,const char* value) {
        struct RestoreFloating { MMDApp& app;HWND window;~RestoreFloating(){app.FloatingWindow()=window;} } restore{*f.app,f.app->FloatingWindow()};
        f.app->FloatingWindow()=nullptr;f.Commit(id,value);
    };
    commitMainPanel(panel::kReadoutPosYEdit,"3.25");
    Check(f.first.bones[0].trans[1]==3.25f&&f.history.UndoCount()==1,"seed an actual model A edit through its numeric EDIT owner");
    f.Toggle(0);
    Check(f.app->state.blackBackgroundEnabled==1&&f.history.UndoCount()==2,"seed a background node after the real model A edit");
    camera_multiply_checks::PluginState plugin;StorageClient storage;storage.id="test.black-background";f.storage.Api(storage);
    auto serializer=plugin.Serializer();MmsHandle provider=0;
    Check(f.storage.Register(storage,Text(storage.id),&serializer,sizeof serializer,&provider)==MMS_OK,"register a real plugin serializer between native background adjustments");
    EditStore edits(f.history,f.storage);EditClient client;client.id=storage.id;const auto api=edits.Api(client);MmsHandle tx=0;std::uint64_t revision=0;
    Check(f.history.Revision(revision)==MMS_OK&&api.begin(api.context,Text("background plugin"),revision,&tx)==MMS_OK,"begin the real interleaved plugin transaction");
    Check(tx&&edits.HasPending()&&f.history.CheckNativeEdit()==MMS_E_BUSY,"the real pending plugin transaction excludes native edit admission");
    const auto pendingBytes=f.history.RetainedBytes();std::uint64_t afterBlocked=0;
    f.Toggle(1);Check(f.app->state.blackBackgroundEnabled==1&&f.history.UndoCount()==2&&!f.history.RedoCount()&&
        f.history.RetainedBytes()==pendingBytes&&edits.HasPending()&&f.history.Revision(afterBlocked)==MMS_OK&&afterBlocked==revision&&f.Echo(),
        "a pending plugin transaction excludes B-key without legacy fallback");
    const auto participant=plugin.Participant(Text(client.id));
    Check(api.join(api.context,tx,&participant)==MMS_OK&&api.prepare(api.context,tx)==MMS_OK&&api.commit(api.context,tx)==MMS_OK&&plugin.value==42,"publish actual plugin-private state into the same native journal");
    f.app->SelectedModelSlot()=1;commitMainPanel(panel::kReadoutPosZEdit,"8.5");
    Check(f.second.bones[0].trans[2]==8.5f&&f.history.UndoCount()==4,"seed an actual model B edit after the plugin publication");
    f.Toggle(1);
    const auto first=f.first.bones[0],second=f.second.bones[0];
    Check(f.history.UndoCount()==5&&f.app->state.blackBackgroundEnabled==0,"model A, menu background, plugin, model B and B-key share chronological history");
    Check(f.history.History(false)==MMS_OK&&f.app->state.blackBackgroundEnabled==1&&
        !std::memcmp(&f.first.bones[0],&first,sizeof first)&&!std::memcmp(&f.second.bones[0],&second,sizeof second)&&
        !f.history.LastPublicationRequiresPhysicsReset(),"undoing the last background edit preserves both newer unregistered poses and has no physics reset");
    Check(f.history.History(false)==MMS_OK&&f.second.bones[0].trans[2]==0&&plugin.value==42&&
        f.history.History(false)==MMS_OK&&plugin.value==0&&f.history.History(false)==MMS_OK&&f.app->state.blackBackgroundEnabled==0&&
        f.first.bones[0].trans[1]==3.25f&&f.history.History(false)==MMS_OK&&f.first.bones[0].trans[1]==0,
        "mixed history undoes model B, plugin payload, background and model A in their actual order");
    for(unsigned i=0;i<5;++i)Check(f.history.History(true)==MMS_OK,"redo one real mixed plugin/model/background node");
    Check(!std::memcmp(&f.first.bones[0],&first,sizeof first)&&!std::memcmp(&f.second.bones[0],&second,sizeof second)&&
        plugin.value==42&&f.app->state.blackBackgroundEnabled==0,"mixed redo restores exact model poses, plugin state and final background");
    Check(f.history.Reset()==MMS_OK,"release background/plugin records before serializer cleanup");f.history.Collect();
    Check(f.storage.Unregister(storage,provider)==MMS_OK,"unregister the interleaved background serializer");
}
struct RequestProbe {
    using Request=void (*)(MMDApp*,bool);
    using Readout=void (*)(MMDApp*);
    static thread_local RequestProbe* current;
    Fixture& f;Request originalRequest=nullptr;Readout originalReadout=nullptr;
    unsigned requests=0,poseReadouts=0;bool geometry=false,throwAfterForward=false,initialized=false;
    static void ForwardRequest(MMDApp* app,bool changed) {
        auto& self=*current;if(app==self.f.app.get()){++self.requests;self.geometry|=changed;}
        self.originalRequest(app,changed);if(self.throwAfterForward)throw std::bad_alloc();
    }
    static void ForwardReadout(MMDApp* app) {
        auto& self=*current;if(app==self.f.app.get())++self.poseReadouts;self.originalReadout(app);
    }
    explicit RequestProbe(Fixture& fixture):f(fixture) {
        Check(!current,"one background notification observer");current=this;const auto init=MH_Initialize();initialized=init==MH_OK;
        Check(initialized||init==MH_ERROR_ALREADY_INITIALIZED,"initialize real background notification hooks");
        Check(MH_CreateHook(reinterpret_cast<void*>(&updates::RequestViewport),reinterpret_cast<void*>(&ForwardRequest),reinterpret_cast<void**>(&originalRequest))==MH_OK&&
            MH_EnableHook(reinterpret_cast<void*>(&updates::RequestViewport))==MH_OK&&
            MH_CreateHook(reinterpret_cast<void*>(&PostViewRefresh),reinterpret_cast<void*>(&ForwardReadout),reinterpret_cast<void**>(&originalReadout))==MH_OK&&
            MH_EnableHook(reinterpret_cast<void*>(&PostViewRefresh))==MH_OK,"observe production viewport requests and pose readout by forwarding to their real implementations");
    }
    ~RequestProbe() {
        for(void* target:{reinterpret_cast<void*>(&updates::RequestViewport),reinterpret_cast<void*>(&PostViewRefresh)}) {MH_DisableHook(target);MH_RemoveHook(target);}
        if(initialized)MH_Uninitialize();current=nullptr;
    }
};
thread_local RequestProbe* RequestProbe::current=nullptr;
void DrawingOnlyAndNotificationFailure() {
    Fixture f;NativePoseTestBinding bound(*f.app,f.history);Source source(f);RequestProbe probe(f);bool changed=false;
    Check(ApplyBoundBlackBackground(*f.app,true,changed)==MMS_OK&&changed&&probe.requests==1&&!probe.geometry&&!probe.poseReadouts&&f.Echo(),
        "background commit requests drawing only and never enters PostViewRefresh");source.Unchanged();
    probe.throwAfterForward=true;changed=false;
    Check(ApplyBoundBlackBackground(*f.app,false,changed)==MMS_OK&&changed&&probe.requests==2&&!probe.poseReadouts&&
        f.app->state.blackBackgroundEnabled==0&&f.history.UndoCount()==2&&f.Echo(),"a display notification exception after real forwarding cannot report failure or roll back an already committed background");
    source.Unchanged();probe.throwAfterForward=false;f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
    Check(ApplyBoundBlackBackground(*f.app,false,changed)==MMS_OK&&!changed&&probe.requests==2&&!probe.poseReadouts&&
        !f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),"background no-op schedules no drawing or pose work");
    Check(f.history.History(false)==MMS_OK&&f.app->state.blackBackgroundEnabled==1&&f.history.History(false)==MMS_OK&&
        f.app->state.blackBackgroundEnabled==0,"notification failure preserves both valid background undo nodes");
}
}
void NativeBlackBackgroundChecks() {
    black_background_checks::RoutingAndRawBytes();black_background_checks::RecordingModes();
    black_background_checks::HistoryUiRoutesPreserveLaterDrafts();
    black_background_checks::BudgetAndAllocationFailures();black_background_checks::GuardsAndRootIdentity();
    black_background_checks::FocusAndOrdinaryRoutes();black_background_checks::CrossModelAndPluginOrder();
    black_background_checks::DrawingOnlyAndNotificationFailure();
}
