// Include after the existing FOV/Light/Morph slider fixtures. Uses their real
// native controls and production entries; no renderer or fake history callback.
#include "slider_readout_spy.hpp"
namespace {
using ReadoutSpy=slider_readout_probe::Spy;
bool ReadoutHasText(HWND control,const char* expected){char actual[64]{};
    return GetWindowTextA(control,actual,sizeof(actual))==int(std::strlen(expected))&&!std::strcmp(actual,expected);}
struct ReadoutLightSpies {
    std::array<ReadoutSpy,6> sliders,edits;
    explicit ReadoutLightSpies(MMDApp& app){for(unsigned i=0;i<6;++i){
        Check(sliders[i].Watch(GetDlgItem(app.MainWindow(),455+int(i))),"observe actual light trackbar messages");
        Check(edits[i].Watch(GetDlgItem(app.MainWindow(),461+int(i))),"observe actual light readout messages");}}
    void Clear(){for(unsigned i=0;i<6;++i){sliders[i].Clear();edits[i].Clear();}}
    bool NoEcho()const{for(unsigned i=0;i<6;++i)if(!sliders[i].counts.NoEcho()||!edits[i].counts.NoEcho()){
        const auto& s=sliders[i].counts;const auto& e=edits[i].counts;
        std::fprintf(stderr,"DIAG light readout lane=%u TBM_SETPOS=%u redraw=%u textset=%u replace=%u select=%u paint=%u erase=%u\n",
            i,s.positions,s.redrawPositions,e.textSets,e.textReplaces,e.selections,s.paints,s.erases);return false;}return true;}
};
void ReadoutOrdinaryLightChecks(){
    LightSliderFixture f;
    auto& root=f.app->LightKeys()[0];root={};root.color[0]=1.f;root.color[1]=.5f;root.color[2]=.25f;
    root.direction[0]=.531234f;root.direction[1]=-.53f;root.direction[2]=.12f;f.app->CurrentFrame()=0;
    RefreshLightPanel(f.app.get());
    const auto keys=GlobalRegistrationKeys(f.app->LightKeys());const auto pose=LightTextPose(*f.app);const auto ring=PoseRingDigest(f.first);
    ReadoutLightSpies spies(*f.app);
    for(unsigned i=0;i<10;++i)RefreshLightPanel(f.app.get());
    Check(spies.NoEcho()&&f.app->LightColor()[0]==1.f&&SendMessageW(f.Slider(0),TBM_GETPOS,0,0)==255&&
        ReadoutHasText(f.Field(0),"256")&&
        LightTextPose(*f.app)==pose&&GlobalRegistrationSame(f.app->LightKeys(),keys)&&PoseRingDigest(f.first)==ring&&!f.history.UndoCount(),
        "ordinary RefreshLightPanel keeps authored 1.0 and fractional directions without same slider or text messages");
    root.color[1]=.75f;spies.Clear();RefreshLightPanel(f.app.get());
    Check(f.app->LightColor()[1]==.75f&&SendMessageW(f.Slider(1),TBM_GETPOS,0,0)==192&&spies.sliders[1].counts.positions==1&&
        spies.sliders[1].counts.redrawPositions==1&&spies.edits[1].counts.textReplaces==1&&ReadoutHasText(f.Field(1),"192")&&
        f.app->LightColor()[0]==1.f&&SendMessageW(f.Slider(0),TBM_GETPOS,0,0)==255&&ReadoutHasText(f.Field(0),"256"),
        "ordinary light sampling still immediately updates a changed native thumb and readout");
    for(unsigned i=0;i<6;++i)if(i!=1)Check(spies.sliders[i].counts.NoEcho()&&spies.edits[i].counts.NoEcho(),
        "ordinary changed light lane does not echo unchanged siblings");
}
void ReadoutOrdinaryHScrollChecks(){
    LightSliderFixture f;
    Check(!plugins::BoundNativePoseHistory(f.app.get()),"ordinary HScroll fixture has no common-history binding");
    // Keep the existing light row selected so the ordinary legacy command
    // does not need a renderer-dependent track-selection panel rebuild.
    Check(f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)!=0,"ordinary HScroll uses the already selected light row");
    const auto keys=GlobalRegistrationKeys(f.app->LightKeys());const auto ring=PoseRingDigest(f.first);const auto frame=f.app->CurrentFrame();
    auto initial=LightTextPose(*f.app);initial[0]=float(double(137)*.00390625);f.Scroll(0,137);
    Check(LightTextPose(*f.app)==initial&&SendMessageW(f.Slider(0),TBM_GETPOS,0,0)==137&&ReadoutHasText(f.Field(0),"137"),
        "unbound production HScroll authors the legacy RGB value and actual readout before repeat checks");
    ReadoutLightSpies spies(*f.app);g_Block=f.app.get();
    for(unsigned repeat=0;repeat<10;++repeat)for(unsigned code:{unsigned(TB_THUMBTRACK),unsigned(TB_THUMBPOSITION),unsigned(TB_ENDTRACK)})
        HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(0)),MAKEWPARAM(code,0));
    Check(spies.NoEcho()&&LightTextPose(*f.app)==initial&&ReadoutHasText(f.Field(0),"137")&&
        GlobalRegistrationSame(f.app->LightKeys(),keys)&&PoseRingDigest(f.first)==ring&&f.app->CurrentFrame()==frame&&
        !f.history.UndoCount()&&!f.history.RedoCount(),
        "unbound same-tick HScroll and ENDTRACK do not repeat text or selection messages or alter old history and keys");
    // Exclude the simulated user's native thumb movement; observe only the
    // ordinary host's reaction through its actual HandleHScroll entry.
    SendMessageW(f.Slider(0),TBM_SETPOS,TRUE,139);spies.Clear();
    HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(0)),TB_THUMBTRACK);
    auto changed=initial;changed[0]=float(double(139)*.00390625);
    Check(LightTextPose(*f.app)==changed&&SendMessageW(f.Slider(0),TBM_GETPOS,0,0)==139&&ReadoutHasText(f.Field(0),"139")&&
        !spies.sliders[0].counts.positions&&spies.edits[0].counts.textReplaces==1&&spies.edits[0].counts.TextWrites()==1&&
        GlobalRegistrationSame(f.app->LightKeys(),keys)&&PoseRingDigest(f.first)==ring&&f.app->CurrentFrame()==frame&&
        !f.history.UndoCount()&&!f.history.RedoCount(),
        "unbound changed RGB tick keeps legacy author conversion and writes correct text exactly once without a common step");
    for(unsigned i=1;i<6;++i)Check(spies.sliders[i].counts.NoEcho()&&spies.edits[i].counts.NoEcho(),
        "unbound changed RGB tick does not echo unchanged siblings");
    spies.Clear();HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(0)),TB_ENDTRACK);
    Check(spies.NoEcho()&&LightTextPose(*f.app)==changed&&ReadoutHasText(f.Field(0),"139")&&
        GlobalRegistrationSame(f.app->LightKeys(),keys)&&PoseRingDigest(f.first)==ring&&!f.history.UndoCount(),
        "unbound ENDTRACK after a real change remains a readout no-op with legacy author and ring intact");
}
// Add only the actual RGB/direction peers absent from the existing FOV fixture.
struct ReadoutLightPeers {
    MMDApp& app;mdl::LightKey* keys=nullptr;
    explicit ReadoutLightPeers(MMDApp& value):app(value){
        Check(!app.LightKeys(),"existing FOV fixture has no light pool to replace");keys=mdl::KeyPoolCreate<mdl::LightKey>(4);app.LightKeys()=keys;
        for(unsigned i=0;i<6;++i){
            Check(CreateWindowExA(0,"EDIT","0",WS_CHILD,0,0,80,20,app.MainWindow(),reinterpret_cast<HMENU>(INT_PTR(461+i)),GetModuleHandleW(nullptr),nullptr)!=nullptr,
                "add real native light readout peer to FOV fixture");
            const auto slider=CreateWindowExA(0,TRACKBAR_CLASSA,"",WS_CHILD,0,0,80,20,app.MainWindow(),reinterpret_cast<HMENU>(INT_PTR(455+i)),GetModuleHandleW(nullptr),nullptr);
            Check(slider!=nullptr,"add real native light slider peer to FOV fixture");SendMessageW(slider,TBM_SETRANGE,FALSE,MAKELPARAM(i<3?0:-100,i<3?255:100));
        }
        const std::array<float,6> values{1.f,.5f,.25f,.531234f,-.53f,.12f};
        for(unsigned i=0;i<3;++i){app.LightColor()[i]=values[i];app.LightDirection()[i]=values[i+3];}app.ApplyTimelineLightState();
        plugins::RefreshBoundGlobalReadout(app);
    }
    ~ReadoutLightPeers(){app.LightKeys()=nullptr;mdl::KeyPoolDestroy(keys);}
};
void ReadoutFovBoundChecks(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        FovSliderFixture f;ReadoutLightPeers peers(*f.app);plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(mode)==MMS_OK,"configure readout FOV history mode");
        ReadoutLightSpies spies(*f.app);ReadoutSpy slider,edit;Check(slider.Watch(f.Slider())&&edit.Watch(f.Field()),"observe bound FOV source controls");
        const auto light=LightTextPose(*f.app);const auto keys=GlobalRegistrationKeys(f.app->CameraKeys());const auto ring=PoseRingDigest(f.first);const auto frame=f.app->CurrentFrame();
        for(unsigned i=0;i<10;++i)plugins::RefreshBoundGlobalReadout(*f.app);
        Check(spies.NoEcho()&&slider.counts.NoEcho()&&edit.counts.NoEcho()&&f.app->CameraFov()==45.125f&&LightTextPose(*f.app)==light,
            "bound global readout does not rewrite stable RGB, fractional direction, FOV or text peers");
        // The displayed tick was already 45. This is still a real authored edit.
        SendMessageW(f.Slider(),TBM_SETPOS,TRUE,45);slider.Clear();edit.Clear();spies.Clear();g_Block=f.app.get();
        HandleHScroll(reinterpret_cast<LPARAM>(f.Slider()),TB_THUMBTRACK);
        Check(f.app->CameraFov()==45&&f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u)&&f.app->SceneModified()&&
            slider.counts.NoEcho()&&edit.counts.TextWrites()==1&&spies.NoEcho(),
            "45.125 to displayed tick45 remains an effective FOV edit in both modes while stable RGB peers receive no echo");
        slider.Clear();edit.Clear();spies.Clear();f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
        HandleHScroll(reinterpret_cast<LPARAM>(f.Slider()),TB_ENDTRACK);
        Check(slider.counts.NoEcho()&&edit.counts.NoEcho()&&spies.NoEcho()&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&
            f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u),"duplicate FOV ENDTRACK neither repaints siblings nor adds history");
        Check(f.history.Enter()==MMS_OK,"hold common journal before moved FOV tick rejection");
        SendMessageW(f.Slider(),TBM_SETPOS,TRUE,67);slider.Clear();edit.Clear();spies.Clear();
        HandleHScroll(reinterpret_cast<LPARAM>(f.Slider()),TB_THUMBTRACK);f.history.Leave();
        Check(f.app->CameraFov()==45&&slider.counts.positions==1&&slider.counts.redrawPositions==1&&slider.counts.lastPosition==45&&
            SendMessageW(f.Slider(),TBM_GETPOS,0,0)==45&&edit.counts.NoEcho()&&spies.NoEcho(),
            "rejected moved FOV thumb is restored once without echoing unchanged readouts or RGB peers");
        SendMessageW(f.Slider(),TBM_SETPOS,TRUE,67);slider.Clear();edit.Clear();spies.Clear();
        HandleHScroll(reinterpret_cast<LPARAM>(f.Slider()),TB_THUMBTRACK);
        Check(f.app->CameraFov()==67&&slider.counts.NoEcho()&&edit.counts.TextWrites()==1&&spies.NoEcho()&&
            f.history.UndoCount()==(mode==Mode::AllOperations?2u:0u)&&LightTextPose(*f.app)==light&&f.app->CurrentFrame()==frame&&
            GlobalRegistrationSame(f.app->CameraKeys(),keys)&&PoseRingDigest(f.first)==ring,
            "new FOV tick still publishes immediately without echoing its native thumb or repainting RGB siblings");
    }
}
void ReadoutDirectionBoundChecks(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        LightSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"configure direction readout history mode");
        f.app->LightDirection()[0]=.531234f;f.app->ApplyTimelineLightState();plugins::RefreshBoundGlobalReadout(*f.app);
        ReadoutLightSpies spies(*f.app);const auto frame=f.app->CurrentFrame();const auto keys=GlobalRegistrationKeys(f.app->LightKeys());
        Check(SendMessageW(f.Slider(3),TBM_GETPOS,0,0)==53,"fractional direction starts at the same displayed hundredth tick");
        g_Block=f.app.get();HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(3)),TB_THUMBTRACK);
        Check(f.app->LightDirection()[0]==.53f&&f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u)&&spies.NoEcho()&&LightTextMirror(*f.app),
            "fractional direction to identical displayed tick is still authored while unchanged one-decimal text is not rewritten");
        spies.Clear();f.app->SceneModified()=0;HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(3)),TB_ENDTRACK);
        Check(spies.NoEcho()&&!f.app->SceneModified(),"equal direction ENDTRACK does not echo stable native controls");
        Check(f.history.Enter()==MMS_OK,"hold direction journal before negative tick rejection");SendMessageW(f.Slider(3),TBM_SETPOS,TRUE,-53);spies.Clear();
        HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(3)),TB_THUMBTRACK);f.history.Leave();
        Check(f.app->LightDirection()[0]==.53f&&SendMessageW(f.Slider(3),TBM_GETPOS,0,0)==53&&spies.sliders[3].counts.positions==1,
            "rejected negative direction thumb returns once to the authored positive tick");
        SendMessageW(f.Slider(3),TBM_SETPOS,TRUE,-53);spies.Clear();HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(3)),TB_THUMBTRACK);
        Check(f.app->LightDirection()[0]==-.53f&&!spies.sliders[3].counts.positions&&spies.edits[3].counts.TextWrites()==1&&
            f.history.UndoCount()==(mode==Mode::AllOperations?2u:0u)&&f.app->CurrentFrame()==frame&&GlobalRegistrationSame(f.app->LightKeys(),keys),
            "negative direction still publishes immediately and preserves native keys and playhead in either history mode");
    }
}
void ReadoutMorphBoundChecks(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        MorphSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"configure morph readout history mode");
        f.firstMorphs[1].value=.12345678f;plugins::RefreshBoundMorphReadout(*f.app);ReadoutSpy slider,edit;
        Check(slider.Watch(f.Slider(1))&&edit.Watch(f.Field(1)),"observe actual morph source control messages");
        Check(f.history.Enter()==MMS_OK,"hold morph history for moved native thumb rejection");SendMessageW(f.Slider(1),TBM_SETPOS,TRUE,53);slider.Clear();edit.Clear();g_Block=f.app.get();
        HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(1)),TB_THUMBTRACK);f.history.Leave();
        Check(f.firstMorphs[1].value==.12345678f&&SendMessageW(f.Slider(1),TBM_GETPOS,0,0)==12&&slider.counts.positions==1&&slider.counts.redrawPositions==1&&edit.counts.NoEcho(),
            "rejected morph tick restores native thumb once without rewriting identical fractional readout");
        SendMessageW(f.Slider(1),TBM_SETPOS,TRUE,53);slider.Clear();edit.Clear();HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(1)),TB_THUMBTRACK);
        Check(f.firstMorphs[1].value==.53f&&SendMessageW(f.Slider(1),TBM_GETPOS,0,0)==53&&slider.counts.NoEcho()&&edit.counts.TextWrites()==1&&
            f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u),"morph .53f retains tick53 and writes only changed text once in both history modes");
        slider.Clear();edit.Clear();f.app->SceneModified()=0;
        for(unsigned i=0;i<10;++i){plugins::RefreshBoundMorphReadout(*f.app);HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(1)),TB_ENDTRACK);}
        Check(slider.counts.NoEcho()&&edit.counts.NoEcho()&&f.firstMorphs[1].value==.53f&&SendMessageW(f.Slider(1),TBM_GETPOS,0,0)==53&&
            !f.app->SceneModified()&&f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u),"repeated safe morph refresh and ENDTRACK cause no echo, drift or duplicate history");
    }
}
}
void NativeSliderReadoutChecks(){ReadoutOrdinaryLightChecks();ReadoutOrdinaryHScrollChecks();ReadoutFovBoundChecks();ReadoutDirectionBoundChecks();ReadoutMorphBoundChecks();}
