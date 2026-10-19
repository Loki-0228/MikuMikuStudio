// Include after native_numpad_view_checks.inc. Uses the existing real native
// arithmetic, private model identities and independent model/key/ring snapshots.
#include "../src/plugins/native_camera_hud_history.hpp"
#include "../src/plugins/native_object_commands.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/viewport_bounds.hpp"
#include "mikumikustudio/window_dpi_scope.hpp"
namespace {
void HudFixtureGeometry(MMDApp& app) {
    const bool floating=app.FloatingWindow()!=nullptr;
    const HWND owner=floating?app.FloatingWindow():app.MainWindow();
    const WindowDpiScope dpi(owner);RECT client{};FrameViewportGeometry geometry{};
    Check(owner&&GetClientRect(owner,&client)&&CalculateFrameViewport(client,app.SidebarWidth(),
        plugins::ToolbarHeight(&app),floating?FrameViewportMode::Floating:FrameViewportMode::Docked,
        app.RenderWidth(),app.RenderHeight(),geometry),
        "CPU HUD fixture has positive output dimensions and valid geometry in its actual hidden client");
    app.ViewportRect()=geometry.scene;
    const auto overlay=FrameOverlayRect(&app);
    Check(!IsRectEmpty(&overlay)&&FrameRectWithin(overlay,geometry.presentationBounds),
        "CPU HUD fixture meets production viewport admission before hit testing or cursor placement");
}
struct CameraHudFixture:NumpadFixture {
    CameraHudFixture() {
        app->RenderWidth()=1280;app->RenderHeight()=720;
        Check(SetWindowPos(app->MainWindow(),nullptr,0,0,800,600,SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,
            "size the hidden HUD-only fixture without changing the shared numeric or numpad fixtures");
        HudFixtureGeometry(*app);
    }
};
void HudFixtureCursor(MMDApp& app,HWND owner,POINT requested,const char* placement,const char* roundtrip) {
    POINT screen=requested;
    const bool positioned=ClientToScreen(owner,&screen)&&SetCursorPos(screen.x,screen.y);
    POINT actualScreen{},actualClient{};
    const bool sampled=GetCursorPos(&actualScreen)!=FALSE;
    actualClient=actualScreen;const bool converted=sampled&&ScreenToClient(owner,&actualClient);
    const bool exact=positioned&&converted&&actualClient.x==requested.x&&actualClient.y==requested.y;
    if(!exact){RECT bounds{},client{},clip{};GetWindowRect(owner,&bounds);GetClientRect(owner,&client);GetClipCursor(&clip);
        const auto view=app.ViewportRect(),overlay=FrameOverlayRect(&app);
        std::fprintf(stderr,"HUD cursor mismatch: expected_client=(%ld,%ld) expected_screen=(%ld,%ld) actual_screen=(%ld,%ld) actual_client=(%ld,%ld) positioned=%u sampled=%u converted=%u window=(%ld,%ld,%ld,%ld) client=(%ld,%ld,%ld,%ld) clip=(%ld,%ld,%ld,%ld) viewport=(%ld,%ld,%ld,%ld) overlay=(%ld,%ld,%ld,%ld) output=(%d,%d)\n",
            requested.x,requested.y,screen.x,screen.y,actualScreen.x,actualScreen.y,actualClient.x,actualClient.y,
            positioned?1u:0u,sampled?1u:0u,converted?1u:0u,bounds.left,bounds.top,bounds.right,bounds.bottom,
            client.left,client.top,client.right,client.bottom,clip.left,clip.top,clip.right,clip.bottom,
            view.left,view.top,view.right,view.bottom,overlay.left,overlay.top,overlay.right,overlay.bottom,
            app.RenderWidth(),app.RenderHeight());}
    Check(positioned,placement);Check(exact,roundtrip);
}
void CameraHudSample(MMDApp& app, int operation, int dy, unsigned modifier = 0) {
    app.PreviousMouseX() = 100; app.PreviousMouseY() = 100;
    app.MouseX() = 103; app.MouseY() = 100 + dy;
    app.ShiftModifierState() = (modifier & 1) ? 3 : 0;
    app.CtrlModifierState() = (modifier & 2) ? 3 : 0;
    if (operation <= 17) ModeCameraAdjust(&app, operation - 15);
    else ModeAngleAdjust(&app, operation - 18);
}
void CameraHudMathAndHistory() {
    using Mode = plugins::SharedHistory::RecordingMode;
    for (int operation = 15; operation <= 20; ++operation) for (int coordinate : {0, 1})
        for (int parent : {-1, 1}) for (unsigned modifier : {0u, 1u, 2u, 3u})
            for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) {
                CameraHudFixture f, oracle;
                for (auto* app : {f.app.get(), oracle.app.get()}) {
                    app->CameraMode() = 1; app->state.coordinateSystem = coordinate;
                    app->CameraParentModel() = parent; app->CameraParentBone() = 0;
                }
                // Load native math helpers before the allocation-free samples.
                CameraHudSample(*oracle.app, operation, 0, modifier);
                plugins::NativePoseTestBinding binding(*f.app, f.history);
                Check(f.history.SetMode(mode) == MMS_OK, "configure camera HUD history policy");
                const auto before = NumpadRead(*f.app);
                const auto keys = GlobalRegistrationKeys(f.app->CameraKeys());
                const auto first = f.first.Digest(), second = f.second.Digest(), ring = PoseRingDigest(f.first);
                Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, operation) == MMS_OK,
                    "reserve a complete six-axis camera HUD gesture before native authoring");
                Check(f.history.SetMode(mode) == MMS_E_BUSY, "pending camera HUD owns one journal reservation");
                for (int dy : {3, -7, 11, 2}) {
                    CameraHudSample(*oracle.app, operation, dy, modifier);
                    failAfter = 0; CameraHudSample(*f.app, operation, dy, modifier); failAfter = -1;
                    Check(NumpadSame(NumpadRead(*f.app), NumpadRead(*oracle.app)) && !f.history.UndoCount(),
                        "HUD preview preserves native local/global/parented float math and modifier precedence without allocating a node");
                }
                const auto after = NumpadRead(*f.app);
                bool changed = false; failAfter = 0;
                const auto result = plugins::EndCameraHudGesture(*f.app, changed); failAfter = -1;
                Check(result == MMS_OK && changed && f.history.UndoCount() == (mode == Mode::AllOperations ? 1u : 0u),
                    "whole HUD drag commits exactly one adjustment or no retained node in explicit-only mode");
                Check(GlobalRegistrationSame(f.app->CameraKeys(), keys) && f.first.Digest() == first &&
                    f.second.Digest() == second && PoseRingDigest(f.first) == ring,
                    "camera HUD never modifies keys model pose physics or legacy rings");
                if (mode == Mode::AllOperations) {
                    auto expectedBefore = before, expectedAfter = after;
                    f.app->CameraDistance() -= 19; f.app->CameraFov() += 7;
                    f.app->ViewOffsetX() += 3; f.app->CameraAttachmentBasis().m[1][3] += 5;
                    for (auto* expected : {&expectedBefore, &expectedAfter}) {
                        expected->values[6] = f.app->CameraDistance(); expected->values[9] = f.app->CameraFov();
                        expected->values[7] = f.app->ViewOffsetX(); expected->basis = f.app->CameraAttachmentBasis();
                    }
                    f.app->CurrentFrame() = 91; f.app->CameraMode() = 0; f.app->SelectedModelSlot() = 0;
                    Check(plugins::RunBoundObjectHistory(*f.app, f.history, false, true) == MMS_OK &&
                        NumpadSame(NumpadRead(*f.app), expectedBefore) && f.app->CurrentFrame() == 91,
                        "real authorized dispatcher undoes HUD across mode and frame without restoring unrelated view fields");
                    Check(plugins::RunBoundObjectHistory(*f.app, f.history, true, true) == MMS_OK &&
                        NumpadSame(NumpadRead(*f.app), expectedAfter),
                        "real authorized dispatcher redoes frozen HUD pose without seeking or reevaluating");
                }
            }
}
void CameraHudCancelNoopAndBudget() {
    using Mode = plugins::SharedHistory::RecordingMode;
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) {
        CameraHudFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        Check(f.history.SetMode(mode) == MMS_OK, "configure HUD cancellation recording policy");
        const auto before = NumpadRead(*f.app);
        for (UINT message : {UINT(WM_CANCELMODE), UINT(WM_KILLFOCUS), UINT(WM_CAPTURECHANGED),
            UINT(WM_KEYDOWN), UINT(WM_ACTIVATEAPP), UINT(WM_CLOSE), UINT(WM_DESTROY), UINT(WM_NCDESTROY)}) {
            Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 18) == MMS_OK, "begin HUD cancellation sample");
            CameraHudSample(*f.app, 18, 7);
            Check(!NumpadSame(NumpadRead(*f.app), before), "HUD has an effective native preview before cancellation");
            failAfter = 0;
            plugins::HandleBoundViewportMessage(*f.app, nullptr, message, message == WM_KEYDOWN ? VK_ESCAPE : 0,
                message == WM_CAPTURECHANGED ? LPARAM(1) : 0);
            failAfter = -1;
            Check(!plugins::CameraHudGestureActive(*f.app) && NumpadSame(NumpadRead(*f.app), before) && !f.history.UndoCount(),
                "escape focus capture cancellation deactivation close and destruction restore the complete HUD without allocation");
        }
        Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 18) == MMS_OK, "begin effective no-op HUD");
        CameraHudSample(*f.app, 18, 0); bool changed = true;
        Check(plugins::EndCameraHudGesture(*f.app, changed) == MMS_OK && !changed && NumpadSame(NumpadRead(*f.app), before),
            "zero-delta HUD is truly empty in either recording mode");
    }
    CameraHudFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    bool prepared = false; const auto before = NumpadRead(*f.app);
    for (long allowance = 0; allowance < 50; ++allowance) {
        failAfter = allowance; const auto result = plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 15); failAfter = -1;
        Check(NumpadSame(NumpadRead(*f.app), before) && !f.history.UndoCount(), "HUD OOM preflight never authors any partial pose or history");
        if (result == MMS_OK) { prepared = true; plugins::CancelCameraHudGesture(*f.app); break; }
        Check(result == MMS_E_MEMORY, "HUD allocation refusal has an explicit memory result");
    }
    Check(prepared, "HUD OOM sweep eventually reserves a complete transaction");
    f.app->CameraPitch() = std::numeric_limits<float>::infinity();
    Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 18) == MMS_E_ARGUMENT &&
        std::isinf(f.app->CameraPitch()) && !f.history.UndoCount(), "nonfinite initial HUD source is refused without replacing the source");
    NumpadWrite(*f.app, before);
    Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 18) == MMS_OK, "begin HUD before invalid native sample");
    CameraHudSample(*f.app, 18, 3); f.app->CameraPitch() = std::numeric_limits<float>::infinity();
    failAfter = 0; const bool accepted = plugins::AcceptCameraHudPreview(*f.app); failAfter = -1;
    Check(!accepted && !plugins::CameraHudGestureActive(*f.app) && NumpadSame(NumpadRead(*f.app), before),
        "nonfinite HUD sample restores the complete original gesture without allocation");
    plugins::StorageStore tinyStore; plugins::SharedHistory tiny(tinyStore, {}, 8, 1);
    Check(plugins::BeginCameraHudGesture(*f.app, tiny, nullptr, 15) == MMS_E_MEMORY && NumpadSame(NumpadRead(*f.app), before),
        "HUD retained budget rejects before the first native preview");
    Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 18) == MMS_OK, "seed HUD redo preservation");
    CameraHudSample(*f.app, 18, 7); bool changed = false;
    Check(plugins::EndCameraHudGesture(*f.app, changed) == MMS_OK && changed && f.history.History(false) == MMS_OK,
        "effective HUD creates an undoable whole gesture");
    const auto redoBefore = NumpadRead(*f.app);
    Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 18) == MMS_OK &&
        plugins::EndCameraHudGesture(*f.app, changed) == MMS_OK && !changed && f.history.RedoCount() == 1 &&
        NumpadSame(NumpadRead(*f.app), redoBefore), "empty HUD preserves the existing plugin/native redo branch");
}
void CameraHudIdentityAndGates() {
    for (unsigned reason = 0; reason < 9; ++reason) {
        CameraHudFixture f; f.app->CameraParentModel() = 1; f.app->CameraParentBone() = 0;
        plugins::NativePoseTestBinding binding(*f.app, f.history);
        const auto ownedBefore = NumpadRead(*f.app);
        Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 15) == MMS_OK, "start parented HUD before source identity changes");
        CameraHudSample(*f.app, 15, 3);
        const auto pool = f.app->CameraKeys(); mdl::CameraKey* replacement = nullptr;
        const auto parent = f.app->ModelSlot(1); const auto bones = f.second.m->boneTable; const auto count = f.second.m->boneCount;
        switch (reason) {
        case 0: ++f.app->CurrentFrame(); break;
        case 1: f.app->CameraParentBone() = 1; break;
        case 2: f.app->state.coordinateSystem = 0; break;
        case 3: plugins::ForgetSceneObject(parent); plugins::SceneObjectIdentity(parent); break;
        case 4: f.app->ModelSlot(1) = f.first.raw(); break;
        case 5: f.second.m->boneTable = bones + 1; break;
        case 6: --f.second.m->boneCount; break;
        case 7: replacement = mdl::KeyPoolCreate<mdl::CameraKey>(4); f.app->CameraKeys() = replacement; break;
        case 8: f.app->CameraMode() = 0; break;
        }
        const auto foreign = NumpadRead(*f.app);
        const bool uiOnly = reason == 2 || reason == 8;
        const auto uiMode = f.app->CameraMode(); const auto uiCoordinate = f.app->state.coordinateSystem;
        Check(!plugins::AllowCameraHudPreview(*f.app, 15) && !plugins::CameraHudGestureActive(*f.app) &&
            NumpadSame(NumpadRead(*f.app), uiOnly ? ownedBefore : foreign) && !f.history.UndoCount() &&
            f.app->CameraMode() == uiMode && f.app->state.coordinateSystem == uiCoordinate,
            uiOnly ? "coordinate or editor-mode change cancels owned HUD pose while retaining the new UI selection" :
                "stale frame attachment stable-ID model-pointer bone-table capacity and camera-pool sources never overwrite their new evaluation domain");
        f.app->ModelSlot(1) = parent; f.second.m->boneTable = bones; f.second.m->boneCount = count;
        if (replacement) { f.app->CameraKeys() = pool; mdl::KeyPoolDestroy(replacement); }
    }
    CameraHudFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    const auto before = NumpadRead(*f.app);
    for (int operation : {1, 2, 9, 12, 14, 21, 22, 26})
        Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, operation) == MMS_E_NOT_READY &&
            NumpadSame(NumpadRead(*f.app), before), "unmigrated HUD families remain refused");
    f.app->state.coordinateSystem = 2;
    Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 15) == MMS_E_NOT_READY,
        "accessory coordinate target is not admitted as a camera HUD gesture");
    f.app->state.coordinateSystem = 1; f.app->CameraMode() = 0;
    Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 15) == MMS_E_NOT_READY,
        "model pose HUD remains behind its original migration gate");
    f.app->CameraMode() = 1; f.app->PlaybackActive() = 1;
    Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 15) == MMS_E_BUSY,
        "playback cannot begin camera HUD authoring");
    f.app->PlaybackActive() = 0;
    Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 15) == MMS_OK, "begin HUD before external author conflict");
    CameraHudSample(*f.app, 15, 3); f.app->CameraPositionX() += 8; const auto foreign = NumpadRead(*f.app);
    Check(!plugins::AllowCameraHudPreview(*f.app, 15) && NumpadSame(NumpadRead(*f.app), foreign),
        "stale HUD cancellation does not overwrite another author's camera values");
    for(int operation:{15,18}){
        CameraHudFixture independent;plugins::NativePoseTestBinding independentBinding(*independent.app,independent.history);
        const auto before=NumpadRead(*independent.app);
        Check(plugins::BeginCameraHudGesture(*independent.app,independent.history,nullptr,operation)==MMS_OK,
            "begin HUD before a foreign author edits an independent pose field");
        CameraHudSample(*independent.app,operation,3);auto expected=before;
        const unsigned foreignField=operation==15?3:0;
        if(foreignField<3)independent.app->CameraPositionAxis(foreignField)+=8;
        else independent.app->CameraRotation()[foreignField-3]+=8;
        expected.values[foreignField]=NumpadRead(*independent.app).values[foreignField];
        Check(!plugins::AllowCameraHudPreview(*independent.app,operation)&&
            !plugins::CameraHudGestureActive(*independent.app)&&NumpadSame(NumpadRead(*independent.app),expected)&&
            !independent.history.UndoCount(),"conflicting position/rotation author cancels only the still-owned HUD fields without an unrecorded preview residue");
    }
    for (unsigned stale = 0; stale < 2; ++stale) {
        CameraHudFixture replay; replay.app->CameraParentModel() = 1;
        plugins::NativePoseTestBinding replayBinding(*replay.app, replay.history);
        Check(plugins::BeginCameraHudGesture(*replay.app, replay.history, nullptr, 18) == MMS_OK,
            "begin a parented HUD record before replay identity test");
        CameraHudSample(*replay.app, 18, 3); bool changed = false;
        Check(plugins::EndCameraHudGesture(*replay.app, changed) == MMS_OK && changed, "retain a complete parented HUD record");
        if (stale) replay.app->CameraParentBone() = 1;
        else { plugins::ForgetSceneObject(replay.second.raw()); plugins::SceneObjectIdentity(replay.second.raw()); }
        const auto current = NumpadRead(*replay.app);
        Check(replay.history.History(false) == MMS_E_STALE && NumpadSame(NumpadRead(*replay.app), current) && replay.history.UndoCount() == 1,
            "published HUD cannot replay against a replacement parent identity or another attachment basis");
    }
}
void CameraHudReleaseAndBinding() {
    CameraHudFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    struct CursorRestore{POINT saved{};bool valid=GetCursorPos(&saved)!=FALSE;~CursorRestore(){if(valid)SetCursorPos(saved.x,saved.y);}} cursorRestore;
    const auto before = NumpadRead(*f.app); const auto owner = f.app->MainWindow();
    SetCapture(owner); Check(GetCapture() == owner, "HUD mouse-up fixture owns real capture");
    Check(plugins::BeginCameraHudGesture(*f.app, f.history, owner, 18) == MMS_OK, "begin a captured HUD gesture");
    f.app->PreviousMouseX() = f.app->MouseX() = 100; f.app->PreviousMouseY() = f.app->MouseY() = 100;
    HudFixtureCursor(*f.app,owner,{100,107},
        "final HUD sample fixture places the real OS cursor in the owner client's coordinate system",
        "final camera HUD cursor round-trips through the actual screen and client coordinates");
    plugins::HandleBoundViewportMessage(*f.app, owner, WM_LBUTTONUP, 0, MAKELPARAM(100, 107));
    Check(!plugins::CameraHudGestureActive(*f.app) && f.history.UndoCount() == 1 &&
        !NumpadSame(NumpadRead(*f.app), before), "coalesced final mouse-up sample belongs to the same complete HUD transaction");
    ReleaseCapture();
    Check(f.history.History(false) == MMS_OK && NumpadSame(NumpadRead(*f.app), before),
        "one undo removes the final coalesced release sample");
    const auto stealer = CreateWindowExW(0, L"STATIC", L"isolated HUD capture owner", WS_OVERLAPPED,
        0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(stealer != nullptr, "create an isolated real capture replacement");
    SetCapture(owner);
    Check(plugins::BeginCameraHudGesture(*f.app, f.history, owner, 18) == MMS_OK, "begin HUD before actual capture takeover");
    CameraHudSample(*f.app, 18, 7); SetCapture(stealer);
    plugins::HandleBoundViewportMessage(*f.app, owner, WM_CAPTURECHANGED, 0, reinterpret_cast<LPARAM>(stealer));
    Check(!plugins::CameraHudGestureActive(*f.app) && NumpadSame(NumpadRead(*f.app), before) && GetCapture() == stealer,
        "actual capture takeover cancels HUD and leaves the new owner's capture intact");
    ReleaseCapture(); DestroyWindow(stealer);
    Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 18) == MMS_OK, "begin HUD before nested binding");
    CameraHudSample(*f.app, 18, 7);
    { CameraHudFixture other; plugins::NativePoseTestBinding nested(*other.app, other.history);
        Check(!plugins::CameraHudGestureActive(*f.app) && NumpadSame(NumpadRead(*f.app), before),
            "binding replacement cancels HUD while the original app and journal still exist"); }
    Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 18) == MMS_OK, "begin HUD before existing reset cleanup entry");
    CameraHudSample(*f.app, 18, 7);
    Check(plugins::CancelViewportCameraGesture(*f.app) && NumpadSame(NumpadRead(*f.app), before) &&
        !plugins::CameraHudGestureActive(*f.app), "reset and shutdown's existing viewport cancellation also rolls back HUD");
    Check(plugins::BeginCameraHudGesture(*f.app, f.history, nullptr, 18) == MMS_OK, "begin HUD before journal reset invalidates its token");
    CameraHudSample(*f.app, 18, 7); Check(f.history.Reset() == MMS_OK, "reset the actual journal with pending HUD");
    bool changed = true;
    Check(plugins::EndCameraHudGesture(*f.app, changed) == MMS_E_STALE && !changed && NumpadSame(NumpadRead(*f.app), before),
        "stale reset token cancels its still-owned HUD preview and publishes no partial record");
}
struct CameraHudReleaseReentryWindow {
    inline static thread_local CameraHudReleaseReentryWindow* active=nullptr;
    NumpadFixture& fixture;HWND owner,edit,floating;WNDPROC ownerProc,editProc;RECT originalView{};POINT cursor{};bool cursorValid,armed=false,observed=false;
    int replacementOperation;NumpadView original{},native{},replacement{};
    CameraHudReleaseReentryWindow(NumpadFixture& f,int operation):fixture(f),owner(f.app->MainWindow()),
        edit(GetDlgItem(owner,operation==15?panel::kReadoutPosXEdit:panel::kReadoutRotXEdit)),floating(f.app->FloatingWindow()),
        ownerProc(nullptr),editProc(nullptr),originalView(f.app->ViewportRect()),cursorValid(GetCursorPos(&cursor)!=FALSE),replacementOperation(operation==15?18:15){
        Check(!active&&edit,"camera release reentry uses the actual native readout EDIT");active=this;
        ownerProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(owner,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(MicWndProc)));
        editProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(EditProc)));
        Check(ownerProc&&editProc,"camera release fixture installs real MicWndProc and forwards the native EDIT procedure");f.app->FloatingWindow()=owner;HudFixtureGeometry(*f.app);
    }
    ~CameraHudReleaseReentryWindow(){armed=false;plugins::CancelCameraHudGesture(*fixture.app);if(GetCapture()==owner)ReleaseCapture();
        SetWindowLongPtrW(edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(editProc));SetWindowLongPtrW(owner,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ownerProc));
        fixture.app->FloatingWindow()=floating;fixture.app->ViewportRect()=originalView;active=nullptr;if(cursorValid)SetCursorPos(cursor.x,cursor.y);}
    static LRESULT CALLBACK EditProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        auto* self=active;const auto originalProc=self->editProc;
        if(message==WM_SETTEXT&&self->armed){self->armed=false;self->observed=true;
            Check(NumpadSame(NumpadRead(*self->fixture.app),self->native),"real camera release readout callback observes its accepted seven-pixel native sample");
            SendMessageW(self->owner,WM_CANCELMODE,0,0);
            Check(!plugins::CameraHudGestureActive(*self->fixture.app)&&NumpadSame(NumpadRead(*self->fixture.app),self->original),
                "real readout callback cancels the old camera gesture and restores its owned pose before replacement");
            Check(plugins::BeginCameraHudGesture(*self->fixture.app,self->fixture.history,self->owner,self->replacementOperation)==MMS_OK,
                "camera readout callback starts a distinct gesture with its own history reservation in the same owner");SetCapture(self->owner);
            CameraHudSample(*self->fixture.app,self->replacementOperation,3);self->replacement=NumpadRead(*self->fixture.app);
            self->fixture.app->MouseX()=333;self->fixture.app->MouseY()=444;self->fixture.app->PreviousMouseX()=300;self->fixture.app->PreviousMouseY()=400;
            self->fixture.app->ViewToolDragOperation()=static_cast<ViewportToolAction>(self->replacementOperation);
            self->fixture.app->InteractionDragMode()=static_cast<ViewportDragMode>(self->replacementOperation-2);
            self->fixture.app->LeftMouseButtonState()=3;self->fixture.app->state.a06B5=1;
        }
        return CallWindowProcA(originalProc,hwnd,message,wp,lp);
    }
};
void CameraHudReleaseReentrantReplacement(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(int operation:{15,18}){
        CameraHudFixture f;f.app->state.coordinateSystem=1;CameraHudReleaseReentryWindow window(f,operation);bool changed=false;
        Check(plugins::AdjustViewportCameraWheel(*f.app,f.history,120,true,changed)==MMS_OK&&changed&&f.history.History(false)==MMS_OK,
            "seed redo before same-owner camera release replacement");
        Check(f.history.SetMode(mode)==MMS_OK,"configure camera release replacement recording mode");plugins::NativePoseTestBinding binding(*f.app,f.history);
        window.original=NumpadRead(*f.app);const auto first=f.first.Digest(),second=f.second.Digest();const auto keys=GlobalRegistrationKeys(f.app->CameraKeys());
        const auto bytes=f.history.RetainedBytes();std::uint64_t revision=0;f.history.Revision(revision);PostViewRefresh(f.app.get());
        Check(plugins::BeginCameraHudGesture(*f.app,f.history,window.owner,operation)==MMS_OK,"reserve old camera HUD before a real mouse-up readout callback");SetCapture(window.owner);
        f.app->PreviousMouseX()=f.app->MouseX()=100;f.app->PreviousMouseY()=f.app->MouseY()=100;
        HudFixtureCursor(*f.app,window.owner,{100,107},"place actual camera release cursor seven native pixels from its previous sample",
            "camera release replacement cursor round-trips through actual screen and client coordinates");
        window.native=window.original;const auto field=operation==15?0u:3u;const double scale=operation==15?0.05000000074505806:0.019999999552965164;
        window.native.values[field]=float(double(window.native.values[field])+7.0*scale);window.armed=true;
        // Before the source guard, this exact WM_SETTEXT callback replaced the
        // gesture, then the old release copied PreviousMouse and End'ed its new
        // token; MicWndProc also released the new same-owner capture.
        SendMessageW(window.owner,WM_LBUTTONUP,0,MAKELPARAM(1,1));std::uint64_t afterRevision=0;f.history.Revision(afterRevision);
        Check(window.observed&&plugins::CameraHudGestureActive(*f.app)&&GetCapture()==window.owner&&
            f.app->MouseX()==333&&f.app->MouseY()==444&&f.app->PreviousMouseX()==300&&f.app->PreviousMouseY()==400&&
            f.app->ViewToolDragOperation()==static_cast<ViewportToolAction>(window.replacementOperation)&&
            f.app->InteractionDragMode()==static_cast<ViewportDragMode>(window.replacementOperation-2)&&f.app->LeftMouseButtonState()==3&&f.app->state.a06B5==1&&
            NumpadSame(NumpadRead(*f.app),window.replacement)&&!f.history.UndoCount()&&f.history.RedoCount()==1&&f.history.RetainedBytes()==bytes&&
            afterRevision==revision&&f.history.SetMode(mode)==MMS_E_BUSY&&f.first.Digest()==first&&f.second.Digest()==second&&GlobalRegistrationSame(f.app->CameraKeys(),keys),
            "old camera mouse-up is swallowed without ending replacement token, overwriting mouse snapshots, clearing tools, releasing capture or changing redo/history");
        Check(plugins::EndCameraHudGesture(*f.app,changed)==MMS_OK&&changed&&f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u),
            "the preserved replacement camera reservation still commits its own independently sampled pose once");ReleaseCapture();
        if(mode==Mode::AllOperations)Check(f.history.History(false)==MMS_OK&&NumpadSame(NumpadRead(*f.app),window.original),
            "replacement camera undo restores only its own gesture, proving old release did not consume its transaction");
    }
}
}
void NativeCameraHudHistoryChecks() {
    CameraHudMathAndHistory(); CameraHudCancelNoopAndBudget(); CameraHudIdentityAndGates(); CameraHudReleaseAndBinding();CameraHudReleaseReentrantReplacement();
}
