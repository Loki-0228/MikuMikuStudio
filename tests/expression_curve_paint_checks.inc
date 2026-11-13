#include "expression_curve_first_paint.inc"

namespace expression_paint {
bool observingMove;
int layoutRequests;
LRESULT CALLBACK ObserveLayout(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR) {
    if (observingMove && msg==WM_WINDOWPOSCHANGING) {
        const UINT flags=reinterpret_cast<const WINDOWPOS*>(lp)->flags;
        if ((flags&(SWP_NOMOVE|SWP_NOSIZE))!=(SWP_NOMOVE|SWP_NOSIZE)) ++layoutRequests;
    }
    if (msg==WM_NCDESTROY) RemoveWindowSubclass(w,ObserveLayout,id);
    return DefSubclassProc(w,msg,wp,lp);
}

void Run() {
    auto app = std::make_unique<MMDApp>(); ResetApp(*app);
    for (int i=0; i<35; ++i) app->ThemeColorAt(i)=RGB(200,200,200);
    auto model = MakeModel({{"expression-smile",1}}, "expression-paint");
    NativeTrack(model,0,{{0,.1f},{20,.9f}});
    // A real host write calls SeekModelFrame, whose mandatory display-track
    // sentinel is needed even by a model with no bones or IK chains.
    auto displayKeys=std::make_unique<mdl::DisplayKey[]>(mdl::kDisplayKeyCapacity);
    displayKeys[0].visible=1;
    model.record()->displayKeys=displayKeys.get();
    owner = CreateWindowW(L"STATIC",L"expression paint owner",WS_OVERLAPPED,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Require(owner != nullptr, "create expression host owner");
    app->Hwnd()=owner; app->SetSelectedModelSlot(0); app->ModelSlot(0)=model.raw();
    app->CurrentFrame()=3; model.record()->selectedMorphs[0]=0;
    ac::Track authored; std::string error;
    Require(mc::Read(model.raw(),0,authored,error) && authored.keys.size()==2 &&
            ac::SetMmdControls(authored,1,{20,20,107,107},error) &&
            mc::Write(model.raw(),{{mc::StableId(0,model.morphs[0].name),authored}},error),
            "seed the real expression host with two keys and a linear control interval");
    const auto open = [&] {
        canvas={}; mmd={};
        const auto& first=model.keys[0];
        const auto next=first.next;
        Require(next>0 && next<model.keys.size(),"expression fixture retains its native key chain");
        const auto& last=model.keys[next];
        std::printf("EXPRESSION_NATIVE_SELECTION frame0=%u selected0=%u frame1=%u selected1=%u playhead=%d\n",
                    unsigned(first.frame),unsigned(first.allocated),unsigned(last.frame),unsigned(last.allocated),int(app->CurrentFrame()));
        std::fflush(stdout);
        Require(first.frame==0 && last.frame==20 && first.allocated && last.allocated && !last.next,
                "both native expression endpoints are selected before OpenMorph without editor input");
        ch::OpenMorph(app.get(),0,false);
        HWND editor=FindWindowW(L"MikuMikuStudioCurveEditorWindow",nullptr);
        Require(editor && GetWindow(editor,GW_OWNER)==owner, "opened this fixture's real expression editor");
        ShowWindow(editor,SW_SHOWNOACTIVATE);
        RedrawWindow(editor,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);
        Require(!canvas.pixels.empty() && !mmd.pixels.empty(), "capture both first visible WM_PAINT presentations before any input");
        for (HWND w : {editor, GetDlgItem(editor,1020), GetDlgItem(editor,1031)})
            Require(SetWindowSubclass(w,ObserveLayout,1,0)!=FALSE,"observe editor and both expression surface layouts");
        return editor;
    };
    HWND editor=open(); const unsigned ink=Geometry(editor,authored,false,0);
    std::puts("EXPRESSION_STAGE baseline first-paint geometry complete; beginning MMD drag");
    HWND panel=GetDlgItem(editor,1031), plot=GetDlgItem(editor,1020);
    const RECT r=Square(panel); const auto from=OnSquare(r,20./127,20./127), to=OnSquare(r,22./127,115./127);
    SendMessageW(panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(from.x,from.y));
    Require(GetCapture()==panel,"real expression MMD handle owns capture");
    RedrawWindow(editor,nullptr,nullptr,RDW_UPDATENOW|RDW_ALLCHILDREN);
    RECT before{},after{}; GetWindowRect(plot,&before);
    const int canvasBefore=canvas.paints, mmdBefore=mmd.paints, shellBefore=shellPaints;
    observingMove=true;
    SendMessageW(panel,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(to.x,to.y));
    UpdateWindow(panel); GetWindowRect(plot,&after);
    observingMove=false;
    Require(!layoutRequests && mmd.paints>mmdBefore && canvas.paints==canvasBefore && shellPaints==shellBefore && !GetUpdateRect(plot,nullptr,FALSE) &&
            !GetUpdateRect(editor,nullptr,FALSE) && EqualRect(&before,&after),
            "MMD drag preview repaints its own surface without moving or damaging the expression canvas");
    Require(model.record()->displayKeys==displayKeys.get(),"real expression commit retains its mandatory display key pool");
    std::puts("EXPRESSION_STAGE MMD release entering real host write and SeekModelFrame");
    SendMessageW(panel,WM_LBUTTONUP,0,MAKELPARAM(to.x,to.y));
    std::puts("EXPRESSION_STAGE MMD release returned");
    Require(mc::Read(model.raw(),0,authored,error) && authored.keys.size()==2 &&
            ac::Evaluate(authored,10).value>.60, "released handle persists a measurable non-linear expression");
    ce::CloseOwned(owner);
    editor=open(); Geometry(editor,authored,true,ink);
    std::puts("EXPRESSION_STAGE reopened nonlinear first-paint geometry complete; beginning canvas selection");
    const auto key=SendMessageW(editor,WM_APP+300,0,0);
    plot=GetDlgItem(editor,1020);
    CurveEditorAxisState axis{};
    Require(SendMessageW(editor,WM_APP+394,0,reinterpret_cast<LPARAM>(&axis))!=0,"read plot for selection");
    const LPARAM blank=MAKELPARAM((axis.plot.left+axis.plot.right)/2,axis.plot.bottom-5);
    SendMessageW(plot,WM_LBUTTONDOWN,MK_LBUTTON,blank); SendMessageW(plot,WM_LBUTTONUP,0,blank);
    Require(!model.keys[0].allocated && !model.keys[model.keys[0].next].allocated,
            "blank canvas click clears both native expression selections");
    SendMessageW(plot,WM_LBUTTONDOWN,MK_LBUTTON,key); SendMessageW(plot,WM_LBUTTONUP,0,key);
    wchar_t selectedFrame[32]{}; GetDlgItemTextW(editor,1041,selectedFrame,32);
    std::printf("EXPRESSION_CANVAS_SELECTION key=%d,%d frame_field=%ls native0=%u native20=%u playhead=%d modifiers=%04x,%04x\n",
                int(short(LOWORD(key))),int(short(HIWORD(key))),selectedFrame,unsigned(model.keys[0].allocated),
                unsigned(model.keys[model.keys[0].next].allocated),int(app->CurrentFrame()),
                unsigned(static_cast<unsigned short>(GetKeyState(VK_SPACE))),unsigned(static_cast<unsigned short>(GetKeyState(VK_MENU))));
    Require(model.keys[0].allocated && !model.keys[model.keys[0].next].allocated &&
            std::wstring(selectedFrame)==L"0" && app->CurrentFrame()==0,
            "visible expression key click keeps its new selection through seek and publishes only frame zero");
    panel=GetDlgItem(editor,1031);
    Require(SendMessageW(editor,WM_APP+394,0,reinterpret_cast<LPARAM>(&axis))!=0,"read selected expression handle geometry");
    const auto head=authored.keys[0];
    const double dt=(authored.keys[1].frame-head.frame)*head.outWeight;
    const int hx=int(std::lround(axis.plot.left+(head.frame+dt-axis.timeStart)/(axis.timeEnd-axis.timeStart)*(axis.plot.right-axis.plot.left)));
    const int hy=int(std::lround(axis.plot.bottom-(head.value+dt*head.outSlope-axis.viewLow)/(axis.viewHigh-axis.viewLow)*(axis.plot.bottom-axis.plot.top)));
    std::printf("EXPRESSION_CANVAS_HANDLE point=%d,%d target=%d,%d plot=%ld,%ld,%ld,%ld slope=%.17g weight=%.17g\n",
                hx,hy,hx+4,hy+8,axis.plot.left,axis.plot.top,axis.plot.right,axis.plot.bottom,head.outSlope,head.outWeight);
    const POINT handle{hx,hy};
    Require(PtInRect(&axis.plot,handle) && authored.keys[1].interpolation==ac::Interpolation::Bezier &&
            !ac::SideIsStraight(authored,0,true),"selected outgoing expression handle lies inside the actual canvas plot");
    ac::Track expected=authored;
    const double targetFrame=axis.timeStart+(hx+4-axis.plot.left)*(axis.timeEnd-axis.timeStart)/(axis.plot.right-axis.plot.left);
    const double targetValue=axis.viewHigh-(hy+8-axis.plot.top)*(axis.viewHigh-axis.viewLow)/(axis.plot.bottom-axis.plot.top);
    Require(ac::MoveHandle(expected,0,true,targetFrame,targetValue),"canvas target defines a real handle edit");
    SendMessageW(plot,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(hx,hy));
    Require(GetCapture()==plot,"outgoing expression canvas handle owns capture");
    RedrawWindow(editor,nullptr,nullptr,RDW_UPDATENOW|RDW_ALLCHILDREN);
    GetWindowRect(panel,&before);
    const auto priorCanvas=canvas.paints,priorMmd=mmd.paints,priorShell=shellPaints;
    observingMove=true;
    SendMessageW(plot,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(hx+4,hy+8)); UpdateWindow(plot);
    GetWindowRect(panel,&after);
    observingMove=false;
    Require(!layoutRequests && canvas.paints>priorCanvas && mmd.paints==priorMmd && shellPaints==priorShell && !GetUpdateRect(panel,nullptr,FALSE) &&
            !GetUpdateRect(editor,nullptr,FALSE) && EqualRect(&before,&after),
            "canvas drag preview repaints its own surface without moving or damaging the expression MMD panel");
    SendMessageW(plot,WM_LBUTTONUP,0,MAKELPARAM(hx+4,hy+8));
    ac::Track changed;
    Require(mc::Read(model.raw(),0,changed,error) && changed.keys.size()==2,"read the real canvas gesture result");
    std::printf("EXPRESSION_CANVAS_RESULT frame=%d value=%.17g slope=%.17g weight=%.17g expected_slope=%.17g expected_weight=%.17g\n",
                int(changed.keys[0].frame),changed.keys[0].value,changed.keys[0].outSlope,changed.keys[0].outWeight,
                expected.keys[0].outSlope,expected.keys[0].outWeight);
    Require(
            changed.keys[0].frame==head.frame && changed.keys[0].value==head.value &&
            changed.keys[1].frame==authored.keys[1].frame && changed.keys[1].value==authored.keys[1].value &&
            changed.keys[0].outSlope!=head.outSlope &&
            std::abs(changed.keys[0].outSlope-expected.keys[0].outSlope)<1e-10 &&
            std::abs(changed.keys[0].outWeight-expected.keys[0].outWeight)<1e-10,
            "canvas gesture changed the expression handle rather than moving a key");
    SendMessageW(plot,WM_LBUTTONDOWN,MK_LBUTTON,blank); SendMessageW(plot,WM_LBUTTONUP,0,blank);
    SendMessageW(plot,WM_LBUTTONDOWN,MK_LBUTTON,key); SendMessageW(plot,WM_LBUTTONUP,0,key);
    double view[4]{};
    for (int button : {1011,1010}) {
        SendMessageW(GetDlgItem(editor,button),BM_CLICK,0,0);
        SendMessageW(editor,WM_APP+304,0,reinterpret_cast<LPARAM>(view));
        Require(view[0]==0 && view[1]==2 &&
                (button!=1011 || (view[2]==0 && view[3]==1)), "fit follows the selected expression key; time fit preserves weight range");
    }
    ce::CloseOwned(owner); DestroyWindow(owner); owner=nullptr;
    app->ModelSlot(0)=nullptr; app->Hwnd()=nullptr; mc::Forget(model.raw());
}
}

int ExpressionFirstPaint() {
    using namespace expression_paint;
    // Redirected stdout must survive a native crash: an absent line should
    // identify the stage, not merely bytes still in the CRT output buffer.
    std::setvbuf(stdout,nullptr,_IONBF,0);
    // This opt-in executable exits immediately after the fixture. Its desktop
    // remains attached until process exit, including any USER/IME helpers.
    // Never switch the input desktop or fall back to the interactive desktop.
    wchar_t name[100]{};
    swprintf_s(name,L"MMS-expression-%lu-%llu",GetCurrentProcessId(),GetTickCount64());
    HDESK desktop=CreateDesktopW(name,nullptr,nullptr,0,GENERIC_ALL,nullptr);
    Require(desktop && SetThreadDesktop(desktop),"attach test thread to its private desktop before creating windows");
    Require(MH_Initialize()==MH_OK,"initialize GDI observation");
    Hook(&BeginPaint,&Begin,begin); Hook(&EndPaint,&End,end); Hook(&BitBlt,&Blit,blit);
    Run();
    Require(MH_Uninitialize()==MH_OK,"remove first-frame GDI observers");
    std::printf("expression first WM_PAINT: %d checks, %d failures\n",g_checks,g_failures);
    return g_failures?1:0;
}
