namespace mikumikustudio {
INT_PTR __stdcall GroundShadowColorDlgProc(HWND,UINT,WPARAM,LPARAM);
}
namespace {
using TestAmbientValues=std::array<float,4>;
TestAmbientValues ReadAmbient(MMDApp& app){return {app.ProjectedShadowAmbientIntensity(),app.state.projectedShadowAmbientG,app.state.projectedShadowAmbientB,app.state.projectedShadowAmbientA};}
void WriteAmbient(MMDApp& app,TestAmbientValues values){app.ProjectedShadowAmbientIntensity()=values[0];app.state.projectedShadowAmbientG=values[1];app.state.projectedShadowAmbientB=values[2];app.state.projectedShadowAmbientA=values[3];}
struct ShadowAmbientFixture : GridDisplayFixture {
    HWND dialog=nullptr,edit=nullptr,slider=nullptr;
    ShadowAmbientFixture(){app->SetProjectedShadowAmbient(.7312345f);dialog=CreateWindowExA(0,"STATIC","shadow ambient",WS_OVERLAPPED,0,0,240,100,app->MainWindow(),nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(dialog!=nullptr,"create hidden ambient dialog");edit=CreateWindowExA(0,"EDIT","",WS_CHILD,0,0,80,20,dialog,reinterpret_cast<HMENU>(INT_PTR(panel::kGroundShadowEdit)),GetModuleHandleW(nullptr),nullptr);
        slider=CreateWindowExA(0,TRACKBAR_CLASSA,"",WS_CHILD,0,20,80,20,dialog,reinterpret_cast<HMENU>(INT_PTR(panel::kGroundShadowSlider)),GetModuleHandleW(nullptr),nullptr);
        Check(edit&&slider,"create native ambient edit and slider");app->GroundShadowColorDialog()=dialog;GroundShadowColorDlgProc(dialog,WM_INITDIALOG,0,0);}
    ~ShadowAmbientFixture(){if(GetCapture())ReleaseCapture();DestroyWindow(dialog);app->GroundShadowColorDialog()=nullptr;}
    void Text(const char* input){g_Block=app.get();SetWindowTextA(edit,input);SendMessageW(edit,EM_SETMODIFY,TRUE,0);SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);}
    void Scroll(int position){g_Block=app.get();SendMessageW(slider,TBM_SETPOS,TRUE,position);GroundShadowColorDlgProc(dialog,WM_HSCROLL,TB_THUMBTRACK,reinterpret_cast<LPARAM>(slider));}
    int Tick(){return int(SendMessageW(slider,TBM_GETPOS,0,0));}
    std::string Text(){char text[64]{};GetWindowTextA(edit,text,sizeof text);return text;}
};
void ShadowAmbientParityChecks(){
    for(const char* input:{"0.53","1.234567","-1.2","2.9","nan","inf","xyz","0.625tail","9999999999999999999999999999999999999999999999999999999999999999999999"}){
        ShadowAmbientFixture controlled,oracle;g_Block=oracle.app.get();oracle.Text(input);g_Block=controlled.app.get();plugins::NativePoseTestBinding binding(*controlled.app,controlled.history);controlled.Text(input);
        Check(ReadAmbient(*controlled.app)==ReadAmbient(*oracle.app)&&controlled.Text()==oracle.Text()&&controlled.Tick()==oracle.Tick(),"ambient text parse clamp float slider conversion and canonical text match actual unbound subclass");
    }
    for(int tick:{0,1,53,100,123,199,200}){ShadowAmbientFixture controlled,oracle;oracle.Scroll(tick);g_Block=controlled.app.get();plugins::NativePoseTestBinding binding(*controlled.app,controlled.history);controlled.Scroll(tick);
        Check(ReadAmbient(*controlled.app)==ReadAmbient(*oracle.app)&&controlled.Text()==oracle.Text()&&controlled.Tick()==oracle.Tick(),"ambient live slider matches legacy double division and text formatting");
        plugins::RefreshBoundGlobalReadout(*controlled.app);Check(controlled.Tick()==tick,"safe ambient refresh retains an exact live thumb tick representing the same author value");}
}
void ShadowAmbientHistoryChecks(){
    ShadowAmbientFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const TestAmbientValues before{.7312345f,.42f,.83f,1.31f};WriteAmbient(*f.app,before);
    const auto pose=NumericCameraPose(*f.app);const auto keys=GlobalRegistrationKeys(f.app->CameraKeys());const auto ring=PoseRingDigest(f.first);const auto flags=ShadowDisplayValues(*f.app);
    f.Text("0.53");const TestAmbientValues after{.53f,.53f,.53f,.53f};
    Check(ReadAmbient(*f.app)==after&&f.history.UndoCount()==1&&f.Tick()==int(after[0]*100.f)&&f.app->SceneModified(),"ambient text commits RGBA mirrors in one step using legacy float tick calculation");
    SendMessageW(f.edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(f.dialog),0);SendMessageW(f.edit,WM_KEYDOWN,VK_RETURN,0);
    Check(f.history.UndoCount()==1&&ReadAmbient(*f.app)==after&&!SendMessageW(f.edit,EM_GETMODIFY,0,0),"Enter then focus loss does not double-submit rounded ambient text");
    f.Scroll(123);Check(f.history.UndoCount()==2&&ReadAmbient(*f.app)==TestAmbientValues{1.23f,1.23f,1.23f,1.23f},"live ambient slider adds its own common step");
    Check(f.history.History(false)==MMS_OK&&ReadAmbient(*f.app)==after,"undo slider restores preceding precise text value");
    f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;f.Text(".53");
    Check(f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),"same ambient input preserves redo and dirty state");
    f.Text("invalid");Check(f.history.RedoCount()==1&&ReadAmbient(*f.app)==after&&!f.app->SceneModified(),"invalid ambient input restores canonical text without discarding redo");
    Check(f.history.History(false)==MMS_OK&&ReadAmbient(*f.app)==before,"ambient undo restores all original RGBA bytes including unequal mirrors");
    f.app->CameraMode()=1;f.app->SelectedModelSlot()=1;f.app->CurrentFrame()=51;
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&f.app->CurrentFrame()==51,"ambient common redo is independent of model mode and playhead");
    Check(NumericCameraPose(*f.app)==pose&&GlobalRegistrationSame(f.app->CameraKeys(),keys)&&PoseRingDigest(f.first)==ring&&f.SelectionUnchanged()&&ShadowDisplayValues(*f.app)==flags,
        "ambient history preserves poses keys selections old ring and project display flags");
    SetWindowTextA(f.edit,"pending input");SendMessageW(f.edit,EM_SETMODIFY,TRUE,0);plugins::RefreshBoundGlobalReadout(*f.app);
    Check(f.Text()=="pending input"&&SendMessageW(f.edit,EM_GETMODIFY,0,0)&&f.Tick()==int(1.23f*100.f),"safe history refresh preserves pending ambient text and updates slider");
}
void ShadowAmbientFailureChecks(){
    for(bool slider:{false,true}){ShadowAmbientFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.Scroll(100);Check(f.history.History(false)==MMS_OK,"seed ambient failure redo");
        const auto before=ReadAmbient(*f.app);const auto pose=NumericCameraPose(*f.app);const auto ring=PoseRingDigest(f.first);f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;unsigned failures=0;bool success=false;PoseAxisMessageProbe messages(f);
        for(long limit=0;limit<128;++limit){if(!slider){SetWindowTextA(f.edit,"1.25");SendMessageW(f.edit,EM_SETMODIFY,TRUE,0);}else SendMessageW(f.slider,TBM_SETPOS,TRUE,125);
            failAfter=limit;if(slider)GroundShadowColorDlgProc(f.dialog,WM_HSCROLL,TB_THUMBTRACK,reinterpret_cast<LPARAM>(f.slider));else SendMessageW(f.edit,WM_KEYDOWN,VK_RETURN,0);failAfter=-1;
            if(f.history.UndoCount()){success=true;break;}++failures;
            Check(ReadAmbient(*f.app)==before&&f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&NumericCameraPose(*f.app)==pose&&PoseRingDigest(f.first)==ring&&f.SelectionUnchanged(),"ambient allocation failure preserves exact RGBA redo dirty poses and selection");
            Check(f.Text()=="0.73"&&!SendMessageW(f.edit,EM_GETMODIFY,0,0),"ambient failure replaces rejected text with canonical current value");
            Check(f.history.Enter()==MMS_OK,"ambient failure releases history gate");f.history.Leave();}
        Check(success&&failures>=5&&!messages.observedBusy,"ambient allocation sweep reaches commit and keeps UI outside publication");
        Check(f.history.History(false)==MMS_OK&&ReadAmbient(*f.app)==before,"ambient remains undoable after allocation failures");
    }
}
void ShadowAmbientGuardChecks(){
    ShadowAmbientFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=ReadAmbient(*f.app);bool changed=true;
    Check(plugins::ApplyBoundProjectedShadowAmbient(*f.app,.5f,changed)==MMS_E_NOT_READY&&!changed,"direct ambient caller cannot bypass owning modeless dialog gate");
    for(int gate=0;gate<7;++gate){if(gate==0)f.app->PlaybackActive()=1;if(gate==1)f.app->FrameStepPlayback()=1;if(gate==2)f.app->state.recordPlaybackStartPending=true;
        if(gate==3)f.app->FrameRangeDialog()=reinterpret_cast<HWND>(INT_PTR(1));if(gate==4)f.app->state.accessoryEditDialogOpen=true;
        if(gate==5)Check(f.history.Enter()==MMS_OK,"hold ambient history gate");if(gate==6){SetCapture(f.edit);Check(GetCapture()==f.edit,"capture unrelated ambient edit control");}
        f.Scroll(123);Check(ReadAmbient(*f.app)==before&&!f.history.UndoCount()&&f.Text()=="0.73","ambient playback modal busy and unrelated capture gates reject without fallback");
        f.app->PlaybackActive()=f.app->FrameStepPlayback()=0;f.app->state.recordPlaybackStartPending=false;f.app->FrameRangeDialog()=nullptr;f.app->state.accessoryEditDialogOpen=false;if(gate==5)f.history.Leave();if(gate==6)ReleaseCapture();}
    SetCapture(f.slider);Check(GetCapture()==f.slider,"capture ambient thumb control");f.Scroll(123);ReleaseCapture();Check(f.history.UndoCount()==1&&f.Tick()==123,"own slider capture permits immediate controlled preview");
    for(float invalid:{-1.f,3.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
        Check(plugins::ApplyBoundProjectedShadowAmbient(*f.app,invalid,changed,f.dialog)==MMS_E_ARGUMENT&&!changed&&f.history.UndoCount()==1,"invalid direct ambient value rejects before history allocation");}
    auto* grown=mdl::KeyPoolCreate<mdl::CameraKey>(12);std::memcpy(grown,f.app->CameraKeys(),mdl::KeyPoolCapacity(f.app->CameraKeys())*sizeof(mdl::CameraKey));mdl::KeyPoolCommitGrowth(f.app->CameraKeys(),grown);
    Check(f.history.History(false)==MMS_OK&&ReadAmbient(*f.app)==before&&f.history.History(true)==MMS_OK,"ambient records survive ordinary camera root growth");const auto after=ReadAmbient(*f.app);
    mdl::KeyPoolDestroy(f.app->CameraKeys());f.app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(12);
    Check(f.history.History(false)==MMS_E_STALE&&ReadAmbient(*f.app)==after&&f.history.UndoCount()==1,"replaced project root rejects ambient history without moving cursor");
}
}
void NativeShadowAmbientChecks(){ShadowAmbientParityChecks();ShadowAmbientHistoryChecks();ShadowAmbientFailureChecks();ShadowAmbientGuardChecks();}
