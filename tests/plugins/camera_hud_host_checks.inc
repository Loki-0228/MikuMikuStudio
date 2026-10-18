// Include after model_viewport_host_checks.inc; invoke Run(app, history) while
// the real hidden host and its edit-probe menu are alive. No SendInput is used.
// Actual OS cursor warps are exercised and the original position is restored.
namespace camera_hud_host_checks {
using model_viewport_host_checks::View;
using model_viewport_host_checks::Read;
using model_viewport_host_checks::Write;
using model_viewport_host_checks::Same;
using Mode=plugins::SharedHistory::RecordingMode;
struct InputRestore {
    MMDApp& app;POINT cursor{};bool cursorValid;
    const int coordinate,left,drag,operation,hovered,tool;
    const ViewportEditMode edit;
    const unsigned char jumped,separate,authored;
    std::array<unsigned char,4> rows{};
    std::array<unsigned char,kModelSlotCount> accessoryRows{};
    explicit InputRestore(MMDApp& value):app(value),cursorValid(GetCursorPos(&cursor)!=FALSE),
        coordinate(value.state.coordinateSystem),left(value.LeftMouseButtonState()),
        drag(int(value.InteractionDragMode())),operation(int(value.ViewToolDragOperation())),
        hovered(value.ViewportToolHovered()),tool(int(value.ViewportToolOperation())),edit(value.EditMode()),
        jumped(value.state.mouseJumped),separate(value.state.separateWindowMouseSeen),authored(value.state.a06B5){
        std::memcpy(rows.data(),app.state.globalTrackSelected,rows.size());
        for(unsigned i=0;i<kModelSlotCount;++i)if(auto* object=app.AccessorySlot(i))accessoryRows[i]=object->rowSelected;
        // This is a UI selection. Keep the existing invariant fixture's global
        // row snapshot stable while native HUD press selects the camera row.
        app.SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
    }
    ~InputRestore(){
        plugins::CancelCameraHudGesture(app);if(GetCapture()==app.MainWindow()||GetCapture()==app.FloatingWindow())ReleaseCapture();
        app.state.coordinateSystem=coordinate;app.LeftMouseButtonState()=left;
        app.InteractionDragMode()=static_cast<ViewportDragMode>(drag);app.ViewToolDragOperation()=static_cast<ViewportToolAction>(operation);
        app.ViewportToolHovered()=hovered;app.ViewportToolOperation()=static_cast<ViewportToolAction>(tool);app.EditMode()=edit;
        app.state.mouseJumped=jumped;app.state.separateWindowMouseSeen=separate;app.state.a06B5=authored;
        std::memcpy(app.state.globalTrackSelected,rows.data(),rows.size());
        for(unsigned i=0;i<kModelSlotCount;++i)if(auto* object=app.AccessorySlot(i))object->rowSelected=accessoryRows[i];
        if(cursorValid)SetCursorPos(cursor.x,cursor.y);
    }
};
POINT ClientCursor(HWND owner){POINT point{};Check(GetCursorPos(&point)&&ScreenToClient(owner,&point),
    "real HUD cursor sample converts screen coordinates to the owning main/detached client");return point;}
void MoveCursor(HWND owner,POINT point){const auto expected=point;
    Check(ClientToScreen(owner,&point)&&SetCursorPos(point.x,point.y),"place the real OS cursor for a production centered HUD message");
    const auto actual=ClientCursor(owner);Check(actual.x==expected.x&&actual.y==expected.y,"real OS cursor reaches the requested owner-client point without clipping");}
POINT Hit(MMDApp& app,int operation){
    const auto hud=viewport_tool_layout::Make(FrameOverlayRect(&app),FrameOverlayScale(&app),viewport_corner_settings::Current());
    const int column=(operation-15)%3;const float x[]={110.f,70.f,30.f};
    POINT point{LONG(hud.x-x[column]*hud.scale),LONG(hud.y-(operation<18?65.f:105.f)*hud.scale)};
    app.MouseX()=point.x;app.MouseY()=point.y;
    Check(ViewportToolAtPointForTest(&app)==operation,"actual rendered HUD layout hits the requested camera translation/rotation axis");return point;
}
void Build(MMDApp& app){
    app.CameraMode()=1;app.CameraParentModel()=-1;app.CameraParentBone()=0;
    app.state.coordinateSystem=1;app.EditMode()=ViewportEditMode::Camera;app.ViewportInputActive()=1;
    app.LeftMouseButtonState()=0;app.ViewToolDragOperation()=ViewportToolAction::None;app.InteractionDragMode()=ViewportDragMode::None;
    app.ShiftModifierState()=app.CtrlModifierState()=0;app.state.a06B5=0;app.state.mouseJumped=0;
    app.SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);app.ViewportRect()=FrameOverlayRect(&app);
}
void CheckPressed(MMDApp& app,HWND owner,int operation){
    const auto cursor=ClientCursor(owner);const auto& view=app.ViewportRect();
    Check(plugins::CameraHudGestureActive(app)&&GetCapture()==owner&&
        int(app.ViewToolDragOperation())==operation&&int(app.InteractionDragMode())==operation-2&&
        cursor.x==(view.left+view.right)/2&&cursor.y==(view.top+view.bottom)/2&&
        app.MouseX()==cursor.x&&app.PreviousMouseX()==cursor.x&&app.MouseY()==cursor.y&&app.PreviousMouseY()==cursor.y,
        "production HUD press reserves history, selects native mode, captures the real window and warps both mouse snapshots to the viewport center");
}
void DispatchQueued(HWND owner,UINT expected){MSG message{};
    Check(PeekMessageW(&message,owner,WM_LBUTTONDOWN,WM_LBUTTONUP,PM_REMOVE)&&message.message==expected,
        "retrieve the previously queued real HUD press/release without an intervening frame");
    DispatchMessageW(&message);
}
void NativePreview(MMDApp& app,int operation){
    // The source operation is the unchanged production frame-mode body. The
    // hidden host cannot poll a physically held desktop button without input
    // injection, so supply only the frame's already sampled three-pixel delta.
    app.MouseY()=app.PreviousMouseY()+3;
    if(operation<18)ModeCameraAdjust(&app,operation-15);else ModeAngleAdjust(&app,operation-18);
}
void Run(MMDApp* app,plugins::SharedHistory& history){
    using edit_history_host_checks::Menu;
    Check(!history.UndoCount()&&!history.RedoCount()&&!GetCapture(),"real HUD suite starts with an empty journal and no foreign capture");
    InputRestore input(*app);model_viewport_host_checks::Original original(*app,history);model_viewport_host_checks::Windows windows(*app);
    const auto initial=Read(*app);
    for(bool detached:{false,true})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(int operation=15;operation<=20;++operation){
        const auto owner=windows.Select(detached);Build(*app);Write(*app,initial);
        Check(history.SetMode(mode)==MMS_OK,"configure actual main/detached HUD recording policy");
        const auto point=Hit(*app,operation);MoveCursor(owner,point);const auto before=Read(*app);
        Check(PostMessageW(owner,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y))&&
            PostMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y)),"queue down and up at the same HUD point before production press warps the cursor");
        DispatchQueued(owner,WM_LBUTTONDOWN);CheckPressed(*app,owner,operation);
        DispatchQueued(owner,WM_LBUTTONUP);
        Check(Same(Read(*app),before)&&!history.UndoCount()&&!history.RedoCount()&&!plugins::CameraHudGestureActive(*app)&&
            GetCapture()!=owner&&app->InteractionDragMode()==ViewportDragMode::None&&app->ViewToolDragOperation()==ViewportToolAction::None,
            "queued same-position click ignores pre-warp release coordinates and leaves a truly empty whole gesture");

        Build(*app);const auto next=Hit(*app,operation);MoveCursor(owner,next);
        SendMessageW(owner,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(next.x,next.y));CheckPressed(*app,owner,operation);
        auto final=ClientCursor(owner);final.y+=7;MoveCursor(owner,final);
        // A stale queued move/release must not win over the centered gesture's
        // live OS cursor; the final seven pixels still belong to this record.
        SendMessageW(owner,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(next.x,next.y));
        SendMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(next.x,next.y));
        auto expected=before;const unsigned field=unsigned(operation-15);
        expected.values[field]=float(double(expected.values[field])+7.0*(operation<18?0.05000000074505806:0.019999999552965164));
        const auto after=Read(*app);
        Check(Same(after,expected)&&!plugins::CameraHudGestureActive(*app)&&GetCapture()!=owner&&
            history.UndoCount()==(mode==Mode::AllOperations?1u:0u),
            "production main/detached release uses its live client cursor and commits the exact final native delta once");
        if(mode==Mode::AllOperations){Menu(app,L"Edit probe undo");Check(Same(Read(*app),before),"actual menu undo removes the entire HUD gesture including its final sample");
            Menu(app,L"Edit probe redo");Check(Same(Read(*app),after),"actual menu redo restores the exact completed HUD pose");}
        Check(history.Reset()==MMS_OK,"retire the real HUD message record between independent axes");history.Collect();original.Invariants();
        if(operation==15||operation==18){
            for(unsigned context=0;context<2;++context){
                Build(*app);Write(*app,initial);const auto press=Hit(*app,operation);MoveCursor(owner,press);
                SendMessageW(owner,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(press.x,press.y));NativePreview(*app,operation);
                Check(!Same(Read(*app),initial),"real HUD press owns an effective native preview before changing a UI context");
                if(context)app->CameraMode()=0;else app->state.coordinateSystem=0;
                SendMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(press.x,press.y));
                Check(Same(Read(*app),initial)&&!history.UndoCount()&&!plugins::CameraHudGestureActive(*app)&&GetCapture()!=owner&&
                    (context?app->CameraMode()==0:app->state.coordinateSystem==0),
                    "production release cancels HUD pose after coordinate/editor-mode change while preserving the new UI choice");
            }
            Build(*app);Write(*app,initial);const auto press=Hit(*app,operation);MoveCursor(owner,press);
            SendMessageW(owner,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(press.x,press.y));NativePreview(*app,operation);
            auto foreign=Read(*app);const unsigned field=operation==15?3u:0u;foreign.values[field]+=8;Write(*app,foreign);
            auto restored=initial;restored.values[field]=foreign.values[field];
            SendMessageW(owner,WM_CANCELMODE,0,0);
            Check(Same(Read(*app),restored)&&!history.UndoCount()&&!plugins::CameraHudGestureActive(*app)&&GetCapture()!=owner,
                "production cancellation rolls back its own translation/rotation fields and preserves the independent foreign author's field");

            Build(*app);Write(*app,initial);const auto stolen=Hit(*app,operation);MoveCursor(owner,stolen);
            SendMessageW(owner,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(stolen.x,stolen.y));NativePreview(*app,operation);
            Check(PostMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(stolen.x,stolen.y)),
                "queue a real old-owner mouse-up before another window takes HUD capture");
            const auto other=detached?app->MainWindow():windows.detached;SetCapture(other);
            Check(Same(Read(*app),initial)&&!plugins::CameraHudGestureActive(*app)&&GetCapture()==other,
                "actual OS capture takeover reaches production WM_CAPTURECHANGED and cancels HUD without releasing the new owner");
            DispatchQueued(owner,WM_LBUTTONUP);
            Check(Same(Read(*app),initial)&&!history.UndoCount()&&!plugins::CameraHudGestureActive(*app)&&GetCapture()==other,
                "dispatching the old main/detached queued mouse-up after cancellation preserves the new capture owner and cancelled pose");
            ReleaseCapture();original.Invariants();
        }
        std::printf("TRACE camera HUD production owner=%s hwnd=%p operation=%d historyMode=%u press=(%ld,%ld) final=(%ld,%ld) delta=7 queued-click=empty\n",
            detached?"MicWndProc":"MainWndProc",static_cast<void*>(owner),operation,unsigned(mode),point.x,point.y,final.x,final.y);
    }
}
}
