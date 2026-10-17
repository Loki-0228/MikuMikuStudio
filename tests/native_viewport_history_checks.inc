#include "../src/plugins/native_viewport_history.hpp"
#include "mikudancestudio/plugin_storage.hpp"
namespace mikudancestudio {void ConsumeRightButtonDrag(MMDApp*);void ConsumeMiddleButtonPan(MMDApp*);}
namespace {
using ViewportValues=std::array<float,7>;
ViewportValues ViewportRead(MMDApp& a){return {a.CameraPositionX(),a.CameraPositionY(),a.CameraPositionZ(),a.CameraPitch(),a.CameraYaw(),a.CameraRoll(),a.CameraDistance()};}
bool ViewportEqual(const ViewportValues& a,const ViewportValues& b){return !std::memcmp(a.data(),b.data(),sizeof(a));}
void ViewportRoundTrip(){
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        std::printf("  viewport roundtrip mode=%u fixture\n",unsigned(mode));
        GlobalRegistrationFixture f;Check(f.history.SetMode(mode)==MMS_OK,"viewport mode configured");
        const auto before=ViewportRead(*f.app);const auto keys=GlobalRegistrationKeys(f.app->CameraKeys());
        std::puts("  viewport roundtrip begin");
        Check(plugins::BeginViewportCameraGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"viewport reserves before authoring");
        Check(f.history.SetMode(mode)==MMS_E_BUSY,"pending gesture prevents journal mode changes");
        for(unsigned i=0;i<500;++i){
            failAfter=0;
            const bool allowed=plugins::AllowViewportCameraPreview(*f.app,WM_RBUTTONDOWN);
            f.app->CameraYaw()+=.001f;f.app->CameraPitch()-=.0003f;
            const bool accepted=plugins::AcceptViewportCameraPreview(*f.app);
            failAfter=-1;Check(allowed&&accepted,"many live previews allocate no history resources");
            Check(f.history.UndoCount()==0,"preview never publishes partial history nodes");
        }
        std::puts("  viewport roundtrip end");const auto after=ViewportRead(*f.app);bool changed=false;failAfter=0;
        const auto result=plugins::EndViewportCameraGesture(*f.app,changed);failAfter=-1;
        Check(result==MMS_OK&&changed&&GlobalRegistrationSame(f.app->CameraKeys(),keys),"gesture end publishes without allocation or modifying camera keys");
        Check(f.history.UndoCount()==(mode==plugins::SharedHistory::RecordingMode::AllOperations?1:0),"one adjustment gesture obeys recording mode");
        if(mode==plugins::SharedHistory::RecordingMode::AllOperations){
            f.app->CameraMode()=0;f.app->CurrentFrame()=98;
            Check(f.history.History(false)==MMS_OK&&ViewportEqual(ViewportRead(*f.app),before),"gesture undo independent of current model mode and playhead");
            Check(f.history.History(true)==MMS_OK&&ViewportEqual(ViewportRead(*f.app),after)&&f.app->CurrentFrame()==98,"gesture redo owns final camera values without seeking");
            f.app->CameraMode()=1;
            Check(f.history.History(false)==MMS_OK,"gesture creates redo branch");
            Check(plugins::BeginViewportCameraGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN)==MMS_OK,"no-op gesture reserves");
            Check(plugins::EndViewportCameraGesture(*f.app,changed)==MMS_OK&&!changed&&f.history.RedoCount()==1,"no-op gesture preserves redo");
        }
    }
}
void ViewportCancellation(){
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        GlobalRegistrationFixture f;f.history.SetMode(mode);const auto before=ViewportRead(*f.app);
        Check(plugins::BeginViewportCameraGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN)==MMS_OK,"cancel fixture begins gesture");
        f.app->CameraPositionX()+=10;Check(plugins::AcceptViewportCameraPreview(*f.app),"cancel fixture accepts owned preview");
        failAfter=0;const auto restored=plugins::CancelViewportCameraGesture(*f.app);failAfter=-1;
        Check(restored&&ViewportEqual(ViewportRead(*f.app),before)&&!f.history.UndoCount(),"cancel restores entire gesture without allocations or partial nodes");
        for(UINT message:{UINT(WM_CANCELMODE),UINT(WM_KILLFOCUS),UINT(WM_CAPTURECHANGED),UINT(WM_KEYDOWN),UINT(WM_ACTIVATEAPP)}){
            plugins::NativePoseTestBinding binding(*f.app,f.history);
            Check(plugins::BeginViewportCameraGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"message cancellation begins");
            f.app->CameraYaw()+=1;plugins::AcceptViewportCameraPreview(*f.app);
            plugins::HandleBoundViewportMessage(*f.app,nullptr,message,message==WM_KEYDOWN?VK_ESCAPE:0,message==WM_CAPTURECHANGED?LPARAM(1):0);
            Check(ViewportEqual(ViewportRead(*f.app),before)&&!f.history.UndoCount(),"cancel focus capture escape and deactivate roll back previews");
        }
        Check(plugins::BeginViewportCameraGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"nonfinite fixture begins");
        f.app->CameraYaw()=std::numeric_limits<float>::infinity();
        Check(!plugins::AcceptViewportCameraPreview(*f.app)&&ViewportEqual(ViewportRead(*f.app),before),"nonfinite preview is rolled back and cancels gesture");
        Check(plugins::BeginViewportCameraGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"external author fixture begins");
        f.app->CameraYaw()+=1;plugins::AcceptViewportCameraPreview(*f.app);
        f.app->CameraYaw()+=2;const auto external=ViewportRead(*f.app);
        Check(!plugins::AllowViewportCameraPreview(*f.app,WM_RBUTTONDOWN)&&ViewportEqual(ViewportRead(*f.app),external),"stale gesture cannot overwrite another author's pose");
    }
}
void ViewportPreflight(){
    GlobalRegistrationFixture f;const auto before=ViewportRead(*f.app);
    bool prepared=false;
    for(long allowance=0;allowance<30;++allowance){
        failAfter=allowance;const auto r=plugins::BeginViewportCameraGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN);failAfter=-1;
        Check(ViewportEqual(ViewportRead(*f.app),before)&&!f.history.UndoCount(),"OOM preflight never authors pose or history");
        if(!r){prepared=true;plugins::CancelViewportCameraGesture(*f.app);break;}Check(r==MMS_E_MEMORY,"preflight OOM has explicit failure");
    }
    Check(prepared,"OOM sweep reaches a complete viewport preparation");
    plugins::StorageStore storage;plugins::SharedHistory budgetHistory(storage,{},8,1);
    Check(plugins::BeginViewportCameraGesture(*f.app,budgetHistory,nullptr,WM_RBUTTONDOWN)==MMS_E_MEMORY&&ViewportEqual(ViewportRead(*f.app),before),"budget rejected before first preview");
    f.app->PlaybackActive()=1;Check(plugins::BeginViewportCameraGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_E_BUSY,"playback rejects gesture");f.app->PlaybackActive()=0;
    f.app->CameraMode()=0;Check(plugins::BeginViewportCameraGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_E_BUSY,"unmigrated model-follow camera gesture fails closed");f.app->CameraMode()=1;
    Check(plugins::BeginViewportCameraGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"stale revision fixture starts");
    f.app->CameraYaw()+=1;plugins::AcceptViewportCameraPreview(*f.app);
    Check(f.history.Reset()==MMS_OK,"scene reset invalidates pending camera transaction");bool changed=true;
    Check(plugins::EndViewportCameraGesture(*f.app,changed)==MMS_E_STALE&&!changed&&ViewportEqual(ViewportRead(*f.app),before),"stale journal cancels owned preview and records nothing");
}
void ViewportLegacyMath(){
    const auto savedBlock=g_Block;
    for(UINT button:{UINT(WM_RBUTTONDOWN),UINT(WM_MBUTTONDOWN)})for(int parent:{-1,0})for(unsigned modifier:{0u,1u,2u}){
        GlobalRegistrationFixture f,oracle;
        // The unbound legacy orbit activates the camera timeline when no
        // track is selected, which requires a real renderer/UI. This fixture
        // already tests camera math, so begin with that track selected.
        oracle.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
        f.app->CameraParentModel()=oracle.app->CameraParentModel()=parent;
        f.app->ShiftModifierState()=oracle.app->ShiftModifierState()=modifier==1?3:0;
        f.app->CtrlModifierState()=oracle.app->CtrlModifierState()=modifier==2?3:0;
        f.app->ViewportInputActive()=oracle.app->ViewportInputActive()=1;
        plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(plugins::BeginViewportCameraGesture(*f.app,f.history,nullptr,button)==MMS_OK,"math oracle begins camera gesture");
        for(unsigned sample=0;sample<3;++sample){
            for(auto* a:{f.app.get(),oracle.app.get()}){
                a->PreviousMouseX()=100;a->PreviousMouseY()=100;a->MouseX()=107+int(sample);a->MouseY()=96-int(sample);
                g_Block=a;
                if(button==WM_RBUTTONDOWN){a->state.rightMouseButtonState=3;ConsumeRightButtonDrag(a);}
                else {a->state.middleMouseButtonState=3;ConsumeMiddleButtonPan(a);}
            }
            Check(ViewportEqual(ViewportRead(*f.app),ViewportRead(*oracle.app)),"native camera orbit zoom and free/parented pan preserve exact legacy float math");
        }
        bool changed=false;Check(plugins::EndViewportCameraGesture(*f.app,changed)==MMS_OK&&changed&&f.history.UndoCount()==1,"legacy-equivalent samples form one journal node");
        g_Block=savedBlock;
    }
    g_Block=savedBlock;
}
void ViewportPendingLifecycle(){
    GlobalRegistrationFixture first,second;const auto before=ViewportRead(*first.app),otherBefore=ViewportRead(*second.app);
    const auto owner=CreateWindowExW(0,L"STATIC",L"viewport lifecycle fixture",WS_OVERLAPPED,0,0,300,200,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(owner!=nullptr,"pending lifecycle has an owned hidden capture window");
    const auto begin=[&](GlobalRegistrationFixture& f){
        Check(plugins::BeginViewportCameraGesture(*f.app,f.history,owner,WM_RBUTTONDOWN)==MMS_OK,"lifecycle begins a genuinely pending gesture");
        SetCapture(owner);Check(GetCapture()==owner,"pending lifecycle acquires capture");
        f.app->CameraYaw()+=.75f;Check(plugins::AcceptViewportCameraPreview(*f.app),"pending lifecycle publishes a live preview");
        Check(!f.history.UndoCount(),"unfinished lifecycle gesture has no history node");
    };
    {
        plugins::NativePoseTestBinding binding(*first.app,first.history);begin(first);
    }
    Check(ViewportEqual(ViewportRead(*first.app),before)&&GetCapture()!=owner&&!first.history.UndoCount(),"binding destruction cancels preview and capture before journal ownership ends");
    {
        plugins::NativePoseTestBinding outer(*first.app,first.history);begin(first);
        {
            plugins::NativePoseTestBinding nested(*second.app,second.history);
            Check(ViewportEqual(ViewportRead(*first.app),before)&&GetCapture()!=owner,"switching bindings cancels the previous application's gesture");
            begin(second);
        }
        Check(ViewportEqual(ViewportRead(*second.app),otherBefore)&&GetCapture()!=owner,"nested unbind cancels its own preview before returning to the previous app");
        begin(first);Check(plugins::ResetPluginProject(first.app.get()),"real plugin reset boundary succeeds with a pending viewport preview");
        Check(ViewportEqual(ViewportRead(*first.app),before)&&GetCapture()!=owner&&!first.history.UndoCount(),"reset boundary cancels gesture before native scene resources can be destroyed");
        begin(first);plugins::CancelViewportCameraGesture(*first.app);
    }
    DestroyWindow(owner);
}
void NativeViewportHistoryChecks(){std::puts("  viewport: roundtrip");ViewportRoundTrip();std::puts("  viewport: cancellation");ViewportCancellation();std::puts("  viewport: preflight");ViewportPreflight();std::puts("  viewport: legacy math");ViewportLegacyMath();std::puts("  viewport: pending lifecycle");ViewportPendingLifecycle();}
}
