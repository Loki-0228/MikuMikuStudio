namespace shadow_ambient_host_checks {
using Values=std::array<float,4>;
Values Read(MMDApp* app){return {app->ProjectedShadowAmbientIntensity(),app->state.projectedShadowAmbientG,app->state.projectedShadowAmbientB,app->state.projectedShadowAmbientA};}
void Write(MMDApp* app,const Values& values){app->ProjectedShadowAmbientIntensity()=values[0];app->state.projectedShadowAmbientG=values[1];app->state.projectedShadowAmbientB=values[2];app->state.projectedShadowAmbientA=values[3];}
HWND Open(MMDApp* app){SendMessageW(app->MainWindow(),WM_COMMAND,248,0);const auto dialog=app->GroundShadowColorDialog();Check(dialog&&IsWindow(dialog),"actual menu 248 opens ambient editor");return dialog;}
void Close(MMDApp* app){const auto dialog=app->GroundShadowColorDialog();SendMessageW(dialog,WM_COMMAND,IDCANCEL,0);Check(!IsWindow(dialog)&&!app->GroundShadowColorDialog(),"ambient close destroys modeless editor without cancelling live edits");}
void Text(HWND dialog,const char* text){const auto edit=GetDlgItem(dialog,panel::kGroundShadowEdit);SetWindowTextA(edit,text);SendMessageW(edit,EM_SETMODIFY,TRUE,0);SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);}
void Slider(HWND dialog,int tick){const auto slider=GetDlgItem(dialog,panel::kGroundShadowSlider);SendMessageW(slider,TBM_SETPOS,TRUE,tick);SendMessageW(dialog,WM_HSCROLL,TB_THUMBTRACK,reinterpret_cast<LPARAM>(slider));}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    const auto original=Read(app);const Values before{.7312345f,.42f,.83f,1.31f},after{.53f,.53f,.53f,.53f},slid{1.23f,1.23f,1.23f,1.23f};Write(app,before);
    auto& model=*mdl::Mdl(app->ModelSlot(0));const auto ring=model.undoState[0];const auto bone=model.boneTable[0];const auto mode=app->CameraMode();const auto frame=app->CurrentFrame();
    Menu(app,L"Edit probe prepared");auto dialog=Open(app);app->SceneModified()=app->RecordedFrameCountByte1()=0;Text(dialog,"1.2");Slider(dialog,125);
    Check(Read(app)==before&&!history.UndoCount()&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),"pending real plugin transaction blocks ambient text and slider without old fallback");Close(app);Menu(app,L"Edit probe cancel");
    Menu(app,L"Edit probe once");dialog=Open(app);Text(dialog,"0.53");
    Check(Read(app)==after&&value()==1&&history.UndoCount()==2&&SendDlgItemMessageW(dialog,panel::kGroundShadowSlider,TBM_GETPOS,0,0)==int(after[0]*100.f),"actual ambient edit creates one plugin-interleaved RGBA step with old text tick precision");
    const auto edit=GetDlgItem(dialog,panel::kGroundShadowEdit);SendMessageW(edit,WM_KILLFOCUS,reinterpret_cast<WPARAM>(dialog),0);
    Check(history.UndoCount()==2&&Read(app)==after,"actual ambient Enter and focus loss do not double-commit rounded text");
    Text(dialog,"pending");Check(Read(app)==after&&history.UndoCount()==2,"invalid actual ambient text makes no author edit");
    Slider(dialog,123);Check(Read(app)==slid&&history.UndoCount()==3&&SendDlgItemMessageW(dialog,panel::kGroundShadowSlider,TBM_GETPOS,0,0)==123,"actual ambient slider keeps live thumb tick while adding common step");
    app->SceneModified()=app->RecordedFrameCountByte1()=0;Slider(dialog,123);Check(history.UndoCount()==3&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),"duplicate actual ambient scroll notification is a no-op");
    Close(app);Check(Read(app)==slid,"closing real ambient editor retains live value");Menu(app,L"Edit probe once");
    Check(SaveSceneCopy(app,std::filesystem::absolute(L"shadow-ambient.pmm").c_str()),"PMM saves live ambient author value with committed plugin state");
    Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Check(Read(app)==after&&value()==1&&history.RedoCount()==2,"plugin undo restores preceding ambient text state");
    dialog=Open(app);app->SceneModified()=app->RecordedFrameCountByte1()=0;Text(dialog,".53");Text(dialog,"nan");
    Check(history.RedoCount()==2&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),"same or invalid real ambient input retains plugin redo");Close(app);
    Menu(app,L"Edit probe undo");Check(Read(app)==before,"actual ambient undo restores exact unequal source RGBA");Menu(app,L"Edit probe undo");
    app->CameraMode()=!mode;app->CurrentFrame()=frame+61;Menu(app,L"Edit probe redo");SendMessageW(app->MainWindow(),WM_COMMAND,401,0);Menu(app,L"Edit probe redo");Menu(app,L"Edit probe redo");
    Check(Read(app)==slid&&value()==2&&app->CurrentFrame()==frame+61&&model.undoState[0]==ring&&!std::memcmp(model.boneTable[0].trans,bone.trans,12),"real ambient cross-mode/frame redo preserves pose old ring and chronological plugin order");
    dialog=Open(app);char text[64]{};GetDlgItemTextA(dialog,panel::kGroundShadowEdit,text,sizeof text);Check(!std::strcmp(text,"1.23"),"reopened ambient dialog shows current shared-history result");Close(app);
    for(int i=0;i<4;++i)Menu(app,L"Edit probe undo");Check(Read(app)==before&&value()==0&&history.Reset()==MMS_OK,"restore ambient host case and clear isolated history");
    Write(app,original);app->CameraMode()=mode;app->CurrentFrame()=frame;std::puts("PASS actual ambient modeless text/slider, no-op close, plugin history, RGBA mirrors and PMM save");
}
}
