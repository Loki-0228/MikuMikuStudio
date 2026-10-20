#include "../src/plugins/native_model_outline_ui.hpp"
namespace model_outline_ui_checks {
using namespace plugins;
LRESULT CALLBACK Hide(HWND window,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR){
    if(msg==WM_WINDOWPOSCHANGING){auto* value=reinterpret_cast<WINDOWPOS*>(lp);value->flags&=~SWP_SHOWWINDOW;value->flags|=SWP_NOACTIVATE;}
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(window,Hide,id);return DefSubclassProc(window,msg,wp,lp);
}
LRESULT CALLBACK HideCreate(int code,WPARAM wp,LPARAM lp){
    if(code==HCBT_CREATEWND){auto* value=reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if(!(value->style&WS_CHILD)){value->style&=~WS_VISIBLE;SetWindowSubclass(reinterpret_cast<HWND>(wp),Hide,1,0);}}
    if(code==HCBT_ACTIVATE)return 1;return CallNextHookEx(nullptr,code,wp,lp);
}
struct Fixture:ObjectCommandFixture {
    HWND dialog=nullptr;
    LPARAM origin=0,lastPoint=0;
    Fixture(){app->HInstance()=GetModuleHandleW(nullptr);mdl::Mdl(app->ModelSlot(0))->edgeScale=.625f;mdl::Mdl(app->ModelSlot(1))->edgeScale=.875f;
        INITCOMMONCONTROLSEX controls{sizeof controls,ICC_BAR_CLASSES};InitCommonControlsEx(&controls);}
    ~Fixture(){Close();}
    void Open(){const auto hook=SetWindowsHookExW(WH_CBT,HideCreate,nullptr,GetCurrentThreadId());Check(hook!=nullptr,"hide actual model outline dialog");
        CmdViewMenu(app.get(),app->MainWindow(),253,0);UnhookWindowsHookEx(hook);dialog=app->state.edgeThicknessDialog;
        Check(dialog&&IsWindow(dialog)&&!IsWindowVisible(dialog),"actual253 dialog opens without showing or activating a user window");}
    void Close(){if(dialog&&IsWindow(dialog))SendMessageW(dialog,WM_COMMAND,IDCANCEL,0);dialog=nullptr;}
    HWND Edit(){return GetDlgItem(dialog,panel::kEdgeThicknessEdit);}
    HWND Slider(){return GetDlgItem(dialog,panel::kEdgeThicknessSlider);}
    void Text(const char* text){SetWindowTextA(Edit(),text);SendMessageA(Edit(),WM_KEYDOWN,VK_RETURN,0);}
    float Value(unsigned slot=0){return mdl::Mdl(app->ModelSlot(slot))->edgeScale;}
    LPARAM Point(int tick){RECT channel{},thumb{};SendMessageW(Slider(),TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&channel));
        SendMessageW(Slider(),TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
        const int width=thumb.right-thumb.left,start=channel.left+width/2,end=channel.right-(width+1)/2;
        return MAKELPARAM(start+MulDiv(end-start,tick,200),(thumb.top+thumb.bottom)/2);}
    void Down(){RECT thumb{};SendMessageW(Slider(),TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
        origin=MAKELPARAM((thumb.left+thumb.right)/2,(thumb.top+thumb.bottom)/2);
        lastPoint=origin;
        SendMessageW(Slider(),WM_LBUTTONDOWN,MK_LBUTTON,origin);
        Check(GetCapture()==Slider(),"actual native trackbar owns mouse capture");}
    void Move(int tick){lastPoint=Point(tick);SendMessageW(Slider(),WM_MOUSEMOVE,MK_LBUTTON,lastPoint);}
    void ReturnToOrigin(){lastPoint=origin;SendMessageW(Slider(),WM_MOUSEMOVE,MK_LBUTTON,lastPoint);}
    void Up(){SendMessageW(Slider(),WM_LBUTTONUP,0,lastPoint);}
    void ClickTrack(int tick){lastPoint=Point(tick);SendMessageW(Slider(),WM_LBUTTONDOWN,MK_LBUTTON,lastPoint);Up();}
    bool Readout(float value){char text[96]{},expected[96]{};GetWindowTextA(Edit(),text,sizeof text);sprintf_s(expected,"%3.2f",double(value));
        return !std::strcmp(text,expected)&&SendMessageW(Slider(),TBM_GETPOS,0,0)==int((std::clamp)(double(value)*100.,0.,200.));}
};
void TextAndIdleHistory(){
    for(unsigned language:{0u,1u,2u})for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}){
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);f.app->EnglishUI()=std::uint8_t(language);f.history.SetMode(mode);f.Open();
        Check(ControlledModelOutlineDialog(f.dialog)&&!NativeModelOutlineDialogBlocks(*f.app),"registered idle outline window admits common history without closing");
        f.Text("1.25");Check(model_outline_checks::Same(f.Value(),1.25f)&&f.app->SceneModified()&&!f.app->PhysicsResetPending()&&
            f.history.UndoCount()==(mode==SharedHistory::RecordingMode::AllOperations?1:0),"actual646 Enter obeys recording mode and changes only model outline");
        SendMessageA(f.Edit(),WM_KILLFOCUS,reinterpret_cast<WPARAM>(f.app->MainWindow()),0);
        Check(f.history.UndoCount()==(mode==SharedHistory::RecordingMode::AllOperations?1:0),"later native EDIT focus-loss message does not duplicate an entered outline value");
        const auto entered=f.history.UndoCount();for(const char* equal:{"+1.25"," +1.25","1.25 "})f.Text(equal);
        Check(model_outline_checks::Same(f.Value(),1.25f)&&f.history.UndoCount()==entered,"valid legacy plus and whitespace input keeps the same numeric value without extra history");
        f.Text("-1.25");Check(model_outline_checks::Same(f.Value(),-1.25f),"actual outline text preserves finite values outside slider bounds");
        for(const char* invalid:{"nan","inf","1e99999","junk","+-1","++1"}){const auto count=f.history.UndoCount();f.Text(invalid);
            Check(model_outline_checks::Same(f.Value(),-1.25f)&&f.history.UndoCount()==count,"invalid actual outline text does not write or add history");SendMessageW(f.Edit(),WM_KEYDOWN,VK_ESCAPE,0);}
        SetWindowTextA(f.Edit(),"1.75");RefreshBoundModelOutline(*f.app);char pending[32]{};GetWindowTextA(f.Edit(),pending,sizeof pending);
        Check(!std::strcmp(pending,"1.75")&&model_outline_checks::Same(f.Value(),-1.25f),"readout refresh preserves a pending outline text draft");
        SendMessageW(f.Edit(),WM_KEYDOWN,VK_ESCAPE,0);
        if(mode==SharedHistory::RecordingMode::AllOperations){
            const auto window=f.dialog;Check(RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK&&model_outline_checks::Same(f.Value(),1.25f)&&IsWindow(window)&&f.Readout(1.25f),
                "common undo works while the controlled outline window remains open");
            Check(RunBoundObjectHistory(*f.app,f.history,true)==MMS_OK&&model_outline_checks::Same(f.Value(),-1.25f)&&f.Readout(-1.25f),"common redo refreshes the still-open property window");}
        const auto fake=CreateWindowExW(0,L"STATIC",L"foreign",0,0,0,10,10,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        f.app->state.edgeThicknessDialog=fake;Check(NativeModelOutlineDialogBlocks(*f.app),"foreign window cannot inherit the controlled outline gate");
        f.app->state.edgeThicknessDialog=f.dialog;DestroyWindow(fake);Check(!NativeModelOutlineDialogBlocks(*f.app),"restoring exact owned window restores idle admission");
        SetWindowTextA(f.Edit(),"1.75");f.app->SelectedModelSlot()=1;RefreshBoundModelOutline(*f.app);SendMessageA(f.Edit(),WM_KEYDOWN,VK_RETURN,0);
        Check(model_outline_checks::Same(f.Value(),-1.25f)&&model_outline_checks::Same(f.Value(1),.875f),"switching targets never applies the previous model's unsubmitted text to another model");
        f.Close();Check(!f.app->state.edgeThicknessDialog,"destroying253 clears the exact app window reference");
    }
}
void RealGestures(){
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}){
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);f.history.SetMode(mode);f.Open();
        f.Down();f.Up();Check(model_outline_checks::Same(f.Value(),.625f)&&!f.history.UndoCount()&&!f.app->SceneModified(),
            "a press and release without moving preserves the unrounded model outline and redo");
        f.Down();f.Move(90);Check(f.Value()>.625f&&!f.history.UndoCount()&&!f.app->SceneModified()&&NativeModelOutlineDialogBlocks(*f.app),"native mouse move previews before any journal or dirty publication");
        SendMessageW(f.Slider(),WM_KEYDOWN,VK_SHIFT,0);SendMessageW(f.Slider(),WM_KEYUP,VK_SHIFT,0);
        SendMessageW(f.Slider(),WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);
        Check(!f.history.UndoCount()&&!f.app->SceneModified(),"modifier release and wheel inside a mouse drag cannot split its history");
        f.Move(140);f.Up();const auto final=f.Value();
        Check(final>.625f&&f.app->SceneModified()&&f.history.UndoCount()==(mode==SharedHistory::RecordingMode::AllOperations?1:0)&&
            !NativeModelOutlineDialogBlocks(*f.app),"one actual DOWN MOVE UP gesture publishes exactly one adjustment");
        if(mode==SharedHistory::RecordingMode::AllOperations){Check(RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK&&model_outline_checks::Same(f.Value(),.625f)&&
            RunBoundObjectHistory(*f.app,f.history,true)==MMS_OK&&model_outline_checks::Same(f.Value(),final),"actual drag replays its last native trackbar sample");}
        f.ClickTrack(0);Check(f.Value()<final&&f.app->SceneModified()&&f.history.UndoCount()==(mode==SharedHistory::RecordingMode::AllOperations?2:0),
            "clicking the track outside the thumb performs its native page adjustment rather than being mistaken for a zero-displacement drag");
        Check(f.history.Reset()==MMS_OK,"reset actual outline gesture history");f.history.Collect();f.app->SceneModified()=0;
        const auto original=f.Value();f.Down();f.Move(75);SetCapture(f.app->MainWindow());
        Check(model_outline_checks::Same(f.Value(),original)&&!f.history.UndoCount()&&!f.app->SceneModified(),"foreign capture cancels and rolls back the owned model outline");ReleaseCapture();
        f.Down();f.Move(100);SendMessageW(f.Slider(),WM_KEYDOWN,VK_ESCAPE,0);
        Check(model_outline_checks::Same(f.Value(),original)&&!f.history.UndoCount()&&!GetCapture(),"Escape cancels preview and releases only its own native capture");
        SendMessageW(f.Slider(),WM_KEYDOWN,VK_RIGHT,1);SendMessageW(f.Slider(),WM_KEYDOWN,VK_SHIFT,1);SendMessageW(f.Slider(),WM_KEYUP,VK_SHIFT,LPARAM(0xc0000001));
        Check(!f.history.UndoCount()&&!f.app->SceneModified(),"unrelated key release cannot end an active outline keyboard gesture");
        SendMessageW(f.Slider(),WM_KEYUP,VK_RIGHT,LPARAM(0xc0000001));
        Check(f.history.UndoCount()==(mode==SharedHistory::RecordingMode::AllOperations?1:0)&&f.app->SceneModified(),"matching navigation key release ends its adjustment exactly once");
        f.app->SceneModified()=0;const auto committed=f.Value();f.Down();f.Move(50);f.Close();
        Check(model_outline_checks::Same(f.Value(),committed)&&!GetCapture()&&!f.app->SceneModified(),"closing the real property window cancels an uncommitted mouse preview");
    }
}
void ExactNoopGestures(){
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly})
    for(float original:{.625f,-1.25f,3.5f,(std::numeric_limits<float>::max)()}){
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);mdl::Mdl(f.app->ModelSlot(0))->edgeScale=original;
        model_outline_checks::Apply(f,0,.75f);Check(f.history.History(false)==MMS_OK,"seed exact outline redo before a noop gesture");
        f.history.SetMode(mode);const auto redo=f.history.RedoCount();std::uint64_t revision=0;f.history.Revision(revision);f.Open();
        f.Down();f.Up();f.Down();f.ReturnToOrigin();f.Up();
        Check(model_outline_checks::Same(f.Value(),original)&&!f.history.UndoCount()&&f.history.RedoCount()==redo&&!f.app->SceneModified(),
            "native click and zero-displacement MOVE retain exact unclamped outline bits and existing redo");
        const auto tick=SendMessageW(f.Slider(),TBM_GETPOS,0,0);f.Down();f.Move(tick<100?160:40);f.ReturnToOrigin();f.Up();
        std::uint64_t after=0;f.history.Revision(after);
        Check(model_outline_checks::Same(f.Value(),original)&&!f.history.UndoCount()&&f.history.RedoCount()==redo&&!f.app->SceneModified()&&after==revision&&f.Readout(original),
            "returning a real drag to the exact original pointer coordinate restores precise source without dirtying or truncating redo");
    }
}
void AllNativeTicks(){
    Fixture f;NativePoseTestBinding binding(*f.app,f.history);mdl::Mdl(f.app->ModelSlot(0))->edgeScale=0;f.Open();
    for(int tick=1;tick<=200;++tick){
        SendMessageW(f.Slider(),WM_KEYDOWN,VK_RIGHT,1);SendMessageW(f.Slider(),WM_KEYUP,VK_RIGHT,LPARAM(0xc0000001));
        Check(model_outline_checks::Same(f.Value(),float(double(tick)/100.0))&&SendMessageW(f.Slider(),TBM_GETPOS,0,0)==tick,
            "every native percentage tick advances and echoes without rounding back to the previous tick");
    }
    const auto nodes=f.history.UndoCount();f.app->SceneModified()=0;
    SendMessageW(f.Slider(),WM_KEYDOWN,VK_RIGHT,1);SendMessageW(f.Slider(),WM_KEYUP,VK_RIGHT,LPARAM(0xc0000001));
    Check(f.Value()==2.f&&f.history.UndoCount()==nodes&&!f.app->SceneModified(),"native keyboard navigation at the boundary is a noop");
    for(int tick=199;tick>=0;--tick){
        SendMessageW(f.Slider(),WM_KEYDOWN,VK_LEFT,1);SendMessageW(f.Slider(),WM_KEYUP,VK_LEFT,LPARAM(0xc0000001));
        Check(model_outline_checks::Same(f.Value(),float(double(tick)/100.0))&&SendMessageW(f.Slider(),TBM_GETPOS,0,0)==tick,
            "all percentage ticks also descend without display feedback changing authored values");
    }
}
void SourceAndPreparationFailures(){
    Fixture f;NativePoseTestBinding binding(*f.app,f.history);f.Open();f.Down();
    Check(NativeModelOutlineDialogBlocks(*f.app),"a pressed but not yet moved native slider excludes other window editors");
    model_outline_checks::Apply(f,0,.875f);f.Move(140);f.Up();
    Check(model_outline_checks::Same(f.Value(),.875f)&&f.history.UndoCount()==1&&!f.app->SceneModified(),
        "a foreign same-field writer before the first sample invalidates the old mouse origin without overwriting it");
    Check(f.history.Reset()==MMS_OK,"reset history after foreign author test");f.history.Collect();
    SetWindowTextA(f.Edit(),"1.25000");failAfter=0;SendMessageA(f.Edit(),WM_KEYDOWN,VK_RETURN,1);failAfter=-1;
    char draft[32]{};GetWindowTextA(f.Edit(),draft,sizeof draft);
    Check(model_outline_checks::Same(f.Value(),.875f)&&!f.history.UndoCount()&&!f.app->SceneModified()&&!std::strcmp(draft,"1.25000"),
        "actual text preparation allocation failure preserves live field, draft, history and dirty state");
    SendMessageA(f.Edit(),WM_KEYDOWN,VK_RETURN,1);
    Check(model_outline_checks::Same(f.Value(),1.25f)&&f.history.UndoCount()==1,"the same failed draft can be explicitly submitted after memory failure clears");
    Check(f.history.Reset()==MMS_OK,"reset before target switch during native movement");f.history.Collect();f.app->SceneModified()=0;
    f.Down();f.app->SetSelectedModelSlot(1);f.Move(140);f.Up();
    Check(model_outline_checks::Same(f.Value(),1.25f)&&model_outline_checks::Same(f.Value(1),.875f)&&!f.history.UndoCount()&&!f.app->SceneModified(),
        "a mouse notification from the old selected model cannot round or edit the replacement model during rebind");
}
void PluginCompositionAndDeletion(){
    Fixture f;NativePoseTestBinding binding(*f.app,f.history);f.Open();
    auto change=model_outline_checks::Plan(f,0,1.25f);camera_multiply_checks::PluginState plugin;
    StorageClient storage;storage.id="test.model-outline";f.storage.Api(storage);auto serializer=plugin.Serializer();MmsHandle provider=0;
    Check(f.storage.Register(storage,Text(storage.id),&serializer,sizeof serializer,&provider)==MMS_OK,"register real outline/plugin history serializer");
    EditStore edits(f.history,f.storage);EditClient client;client.id=storage.id;auto api=edits.Api(client);MmsHandle transaction=0;std::uint64_t revision=0;
    Check(f.history.Revision(revision)==MMS_OK&&api.begin(api.context,Text("outline and plugin"),revision,&transaction)==MMS_OK,"begin mixed outline/plugin transaction with real253 open");
    NativeModelOutlinePlan busy;Check(busy.Begin(*f.app,0)==MMS_E_BUSY,"pending plugin edit excludes competing native outline planning");auto participant=plugin.Participant(Text(client.id));
    Check(api.join(api.context,transaction,&participant)==MMS_OK&&edits.JoinNative(client,transaction,change)==MMS_OK&&api.prepare(api.context,transaction)==MMS_OK&&
        api.commit(api.context,transaction)==MMS_OK&&plugin.value==42&&f.history.UndoCount()==1,"native outline and plugin state commit as one real ABI transaction");
    RefreshBoundModelOutline(*f.app);Check(f.Readout(1.25f),"mixed publication readout reflects outline field");
    Check(RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK&&plugin.value==0&&model_outline_checks::Same(f.Value(),.625f)&&f.Readout(.625f)&&IsWindow(f.dialog)&&
        RunBoundObjectHistory(*f.app,f.history,true)==MMS_OK&&plugin.value==42&&model_outline_checks::Same(f.Value(),1.25f)&&f.Readout(1.25f),
        "host undo redo restore mixed plugin state and exact native outline while the property window remains registered");
    Check(f.history.Reset()==MMS_OK,"retire mixed outline records before unregistering the serializer");f.history.Collect();
    Check(f.storage.Unregister(storage,provider)==MMS_OK,"unregister outline test serializer after retirement");
    f.Text("1.75");auto* target=f.app->ModelSlot(0);const auto identity=SceneObjectIdentity(target);
    {ObjectModalProbe probe(f,true);f.Command(true);Check(probe.seen==1&&!f.app->ModelSlot(0),"idle real253 permits actual437 deletion and its confirmation");}
    Check(RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK&&f.app->ModelSlot(0)==target,"host undo restores deleted outline target while safely refreshing native windows");
    const auto restored=SceneObjectIdentity(target);Check(restored.id.hi==identity.id.hi&&restored.id.lo==identity.id.lo&&restored.generation!=identity.generation,"restored outline model retains stable ID and advances generation");
    f.app->SetSelectedModelSlot(0);f.app->CameraMode()=0;if(!IsWindow(f.dialog))f.Open();else RefreshBoundModelOutline(*f.app);
    Check(RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK&&model_outline_checks::Same(f.Value(),1.25f)&&f.Readout(1.25f)&&
        RunBoundObjectHistory(*f.app,f.history,true)==MMS_OK&&model_outline_checks::Same(f.Value(),1.75f)&&f.Readout(1.75f),"reopened actual property window replays outline across controlled delete restore");
    Check(RunBoundObjectHistory(*f.app,f.history,true)==MMS_OK&&!f.app->ModelSlot(0)&&RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK,
        "redo deletion and undo restoration remain safe with real outline controller lifecycle");
}
struct CloseDuringEcho {
    HWND target=nullptr;bool fired=false,deferred=false;
    static LRESULT CALLBACK Observe(HWND window,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data){
        auto& s=*reinterpret_cast<CloseDuringEcho*>(data);
        if(msg==WM_SETTEXT&&!s.fired){s.fired=true;SendMessageW(s.target,WM_CLOSE,0,0);s.deferred=IsWindow(s.target)!=FALSE;}
        return DefSubclassProc(window,msg,wp,lp);
    }
};
void ReentrantClose(){
    for(bool main:{false,true}){
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);f.Open();SetWindowTextA(f.Edit(),"1.25000");
        CloseDuringEcho probe;probe.target=main?f.app->MainWindow():f.dialog;
        Check(SetWindowSubclass(f.Edit(),CloseDuringEcho::Observe,7,reinterpret_cast<DWORD_PTR>(&probe))!=FALSE,"install synchronous close during native readout");
        SendMessageA(f.Edit(),WM_KEYDOWN,VK_RETURN,1);
        Check(probe.fired&&probe.deferred&&model_outline_checks::Same(f.Value(),1.25f)&&f.history.UndoCount()==1&&f.app->SceneModified(),
            "synchronous close during text echo is deferred until after the input operation publishes its field and history");
        MSG message{};Check(PeekMessageW(&message,probe.target,WM_CLOSE,WM_CLOSE,PM_REMOVE)!=FALSE,"outer busy scope posts requested close at a safe message boundary");
        DispatchMessageW(&message);Check(!IsWindow(probe.target)&&!f.app->state.edgeThicknessDialog,"deferred close retires exact controller and property HWND");
    }
}
void BindingAndInitializationFailure(){
    Fixture f;
    {NativePoseTestBinding binding(*f.app,f.history);f.Open();f.Text("1.25");f.Down();f.Move(160);}
    Check(model_outline_checks::Same(f.Value(),1.25f)&&!GetCapture(),"history unbinding cancels a live native mouse preview before releasing its journal");
    Check(RefreshBoundModelOutline(*f.app)&&!IsWindowEnabled(f.Edit()),"unbound controlled window remains disabled rather than reverting to old writes");
    SetWindowTextA(f.Edit(),"1.75");SendMessageA(f.Edit(),WM_KEYDOWN,VK_RETURN,0);SendMessageW(f.Slider(),TBM_SETPOS,TRUE,50);
    SendMessageW(f.dialog,WM_HSCROLL,TB_THUMBPOSITION,reinterpret_cast<LPARAM>(f.Slider()));
    Check(model_outline_checks::Same(f.Value(),1.25f),"program messages cannot bypass the permanent controlled-window identity after unbinding");
    {NativePoseTestBinding binding(*f.app,f.history);Check(RefreshBoundModelOutline(*f.app)&&IsWindowEnabled(f.Edit()),"new binding safely reconnects the same native property window");f.Text("1.75");}
    f.Close();
    f.Open(); // Ordinary window, so initialization OOM can be injected exactly.
    {NativePoseTestBinding binding(*f.app,f.history);failAfter=0;const bool consumed=InitializeBoundModelOutline(*f.app,f.dialog);failAfter=-1;
        Check(consumed&&ControlledModelOutlineDialog(f.dialog)&&!IsWindowEnabled(f.Edit())&&NativeModelOutlineDialogBlocks(*f.app),"controller OOM retains a permanently controlled disabled window and restrictive gate");}
    SetWindowTextA(f.Edit(),"0.25");SendMessageA(f.Edit(),WM_KEYDOWN,VK_RETURN,0);
    Check(model_outline_checks::Same(f.Value(),1.75f),"failed initialization cannot become an unjournaled edit after unbinding");f.Close();
}
}
void NativeModelOutlineUiChecks(){
    std::puts("outline UI: text and idle history");model_outline_ui_checks::TextAndIdleHistory();
    std::puts("outline UI: real gestures");model_outline_ui_checks::RealGestures();
    std::puts("outline UI: every native tick");model_outline_ui_checks::AllNativeTicks();
    std::puts("outline UI: source and preparation failure");model_outline_ui_checks::SourceAndPreparationFailures();
    std::puts("outline UI: plugin composition and deletion");model_outline_ui_checks::PluginCompositionAndDeletion();
    std::puts("outline UI: synchronous close");model_outline_ui_checks::ReentrantClose();
    std::puts("outline UI: binding and initialization failure");model_outline_ui_checks::BindingAndInitializationFailure();
    std::puts("outline UI: exact noop gestures");model_outline_ui_checks::ExactNoopGestures();
}
