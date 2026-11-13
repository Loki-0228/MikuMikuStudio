#pragma once
#include <array>
#include <cmath>
#include <limits>
#if !MMD_ORIGINAL_PROFILE
#include "../src/plugins/background_identity.hpp"
#endif

// Exercise native window procedures without moving the user's pointer or focus.
namespace cursor_checks {
inline HWND foreground = nullptr, capture = nullptr;
inline bool held = false, control = false;
inline bool warpSucceeds = true, positionSucceeds = true;
inline int warps = 0, visibility = 0;
inline HCURSOR cursor = nullptr;
inline HWND WINAPI Foreground() { return foreground; }
inline HWND WINAPI Capture() { return capture; }
inline SHORT WINAPI Key(int key) {
    return (held && key == VK_LBUTTON) || (control && key == VK_CONTROL) ? SHORT(0x8000) : 0;
}
inline POINT pointer{};
inline BOOL WINAPI Position(LPPOINT p) { if (!positionSucceeds) return FALSE; *p=pointer; return TRUE; }
inline BOOL WINAPI Warp(int x, int y) { ++warps; if (!warpSucceeds) return FALSE; pointer={x,y}; return TRUE; }
inline BOOL WINAPI Release() { capture = nullptr; return TRUE; }
inline HCURSOR WINAPI Cursor(HCURSOR next) { auto previous = cursor; cursor = next; return previous; }
inline int WINAPI Visibility(BOOL show) { return visibility += show ? 1 : -1; }
// Reusable OS-input boundary for other real-host tests. Production frame math,
// input dispatch, background resources and journal code remain unmodified.
class ScopedInputHooks final {
public:
    ScopedInputHooks() {
        foreground=capture=nullptr;held=control=false;warps=visibility=0;
        warpSucceeds=positionSucceeds=true;pointer={};cursor=nullptr;
        const auto install=[](auto api,auto callback) {
            Check(MH_CreateHook(reinterpret_cast<void*>(api),reinterpret_cast<void*>(callback),nullptr)==MH_OK &&
                  MH_EnableHook(reinterpret_cast<void*>(api))==MH_OK,"install isolated actual-input boundary");
        };
        install(&GetForegroundWindow,&Foreground);install(&GetCapture,&Capture);install(&ReleaseCapture,&Release);
        install(&GetKeyState,&Key);install(&GetAsyncKeyState,&Key);install(&GetCursorPos,&Position);
        install(&SetCursorPos,&Warp);install(&SetCursor,&Cursor);install(&ShowCursor,&Visibility);
    }
    ~ScopedInputHooks() {
        for(void* api:{reinterpret_cast<void*>(&GetForegroundWindow),reinterpret_cast<void*>(&GetCapture),
            reinterpret_cast<void*>(&ReleaseCapture),reinterpret_cast<void*>(&GetKeyState),reinterpret_cast<void*>(&GetAsyncKeyState),
            reinterpret_cast<void*>(&GetCursorPos),reinterpret_cast<void*>(&SetCursorPos),reinterpret_cast<void*>(&SetCursor),
            reinterpret_cast<void*>(&ShowCursor)}) {MH_DisableHook(api);MH_RemoveHook(api);}
        foreground=capture=nullptr;held=control=false;warpSucceeds=positionSucceeds=true;
    }
    ScopedInputHooks(const ScopedInputHooks&)=delete;
    ScopedInputHooks& operator=(const ScopedInputHooks&)=delete;
};
inline void Run(MMDApp* app) {
    const auto saved = std::make_unique<MMDAppState>(app->state);
    const auto savedGesture = app->backgroundGesture;
    const auto install = [](auto api, auto callback) {
        Check(MH_CreateHook(reinterpret_cast<void*>(api), reinterpret_cast<void*>(callback), nullptr) == MH_OK,
              "install isolated cursor observation");
        Check(MH_EnableHook(reinterpret_cast<void*>(api)) == MH_OK, "enable cursor observation");
    };
    install(&GetForegroundWindow, &Foreground); install(&GetCapture, &Capture);
    install(&GetKeyState, &Key); install(&GetAsyncKeyState, &Key);
    install(&GetCursorPos, &Position); install(&SetCursorPos, &Warp); install(&SetCursor, &Cursor); install(&ShowCursor, &Visibility);
    const auto main = app->MainWindow();
    foreground = main;
    app->state.optflag[0] = 1;
    app->PlaybackActive() = 0;
    const RECT view = app->ViewportRect();
    const RECT frame = FrameOverlayRect(app);
    const float scale = FrameOverlayScale(app);
    app->MouseX() = app->PreviousMouseX() = frame.right - int(70 * scale);
    app->MouseY() = app->PreviousMouseY() = frame.bottom - int(65 * scale);
    app->LeftMouseButtonState() = 0;
    held = true;
    MouseInteractionBegin(app);
    Check(warps == 0 && app->ViewToolDragOperation() == ViewportToolAction::None,
          "non-client press cannot start a drag using stale viewport coordinates");
    const auto mode = app->EditMode();
    MicWndProc(main, WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(main), MAKELPARAM(HTRIGHT, WM_LBUTTONDOWN));
    Check(warps == 0 && app->EditMode() == mode, "border activation leaves the cursor and editing mode alone");
    cursor = LoadCursorA(nullptr, IDC_SIZEWE);
    app->ViewportToolHovered() = 0;
    Check(MicWndProc(main, WM_SETCURSOR, reinterpret_cast<WPARAM>(main), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)) == TRUE &&
          cursor == LoadCursorA(nullptr, IDC_ARROW), "detached viewport clears the resize cursor on client entry");
    capture = main;
    app->LeftMouseButtonState() = 0;
    Check(HandleViewportToolPress(app), "a real captured viewport press still starts the tool");
    Check(warps == 1, "captured tool starts one centred gesture");
    capture = nullptr;
    app->state.mouseJumped = 0;
    app->MouseY() = view.bottom + 40;
    RecentreCenteredDragCursor(app);
    Check(warps == 1, "capture loss prevents recentering over another window");
    HandleViewportToolRelease(app);
    Check(warps == 1 && visibility == 0, "capture cancellation restores visibility without warping");
    if (!build_profile::original && scale > 1) {
        capture = main;
        app->LeftMouseButtonState() = 0;
        app->MouseX() = frame.right - int(70 * scale);
        app->MouseY() = frame.bottom - int(65 * scale);
        const auto position = [&] {
            return std::array<float, 3>{app->CameraPositionX(), app->CameraPositionY(), app->CameraPositionZ()};
        };
        const auto before = position();
        Check(HandleViewportToolPress(app) && static_cast<int>(app->ViewToolDragOperation()) == 16,
              "pressing the enlarged camera Y handle starts its actual native gesture");
        pointer.y += 12;
        MouseInteractionBegin(app);
        ModeCameraAdjust(app, 1);
        MouseInteractionEnd(app);
        held = false; HandleViewportToolRelease(app);
        Check(position() != before && app->ViewToolDragOperation() == ViewportToolAction::None && visibility == 0,
              "the enlarged handle moves the camera and completes on release");
    }
    RECT client{}; GetClientRect(main,&client);
    const auto headerPoint = [&](int operation) {
        auto panels = FramePanelLayout(app);
        if (MMD_ORIGINAL_PROFILE) panels.overlay.right = client.right;
        app->MouseX() = panels.TopX(operation == 1 ? 48 : 18);
        app->MouseY() = panels.TopY(-12);
    };
    for (int operation : {1,2}) {
        capture=foreground=main;held=true;app->LeftMouseButtonState()=0;
        app->CameraMode()=1;app->CameraParentModel()=-1;
        headerPoint(operation);
        const int before=warps;
        Check(HandleViewportToolPress(app),"header zoom/pan starts a captured drag");
        Check(OwnsCenteredViewportDrag(app) && warps==before+1,"header buttons enter unlimited centered dragging");
        float expected=app->CameraDistance();
        const float panX=app->CameraPositionX(),panY=app->CameraPositionY(),panZ=app->CameraPositionZ();
        for(int repeat=0;repeat<12;++repeat) {
            const int x=operation==1?(view.left+view.right)/2:view.right+20;
            const int y=view.top-20;
            const int dy=y-app->PreviousMouseY();
            if(operation==1)expected-=dy*.1f;
            pointer={x,y};ClientToScreen(main,&pointer);
            MouseInteractionBegin(app);
            Check(app->MouseY()==(view.top+view.bottom)/2 && app->PreviousMouseY()==app->MouseY(),
                  "crossing top edge recenters without introducing a jump");
            if(operation==1)Check(std::abs(app->CameraDistance()-expected)<.01f,"zoom continues accumulating after each boundary crossing");
            MouseInteractionEnd(app);
        }
        if(operation==2)Check(app->CameraPositionX()!=panX || app->CameraPositionY()!=panY || app->CameraPositionZ()!=panZ,
                              "pan continues changing the camera beyond window edges");
        for(int edge=0;edge<4;++edge) {
            const int x=edge==0?view.left:edge==1?view.right-1:(view.left+view.right)/2;
            const int y=edge==2?view.top:edge==3?view.bottom-1:(view.top+view.bottom)/2;
            pointer={x,y};ClientToScreen(main,&pointer);
            MouseInteractionBegin(app);MouseInteractionEnd(app);
            Check(app->MouseX()==(view.left+view.right)/2 && app->MouseY()==(view.top+view.bottom)/2,
                  "touching each viewport edge is sufficient to continue dragging");
        }
        const float distance=app->CameraDistance();
        MouseInteractionBegin(app);MouseInteractionEnd(app);
        Check(app->CameraDistance()==distance,"recentered stationary cursor adds no zoom");
        held=false;HandleViewportToolRelease(app);
        Check(!OwnsCenteredViewportDrag(app) && visibility==0,"header release restores cursor visibility");
        app->LeftMouseButtonState()=0;held=true;
        headerPoint(operation);
        Check(HandleViewportToolPress(app),"header tool can start again");
        const int cancelWarps=warps;capture=nullptr;
        HandleViewportToolRelease(app);
        Check(warps==cancelWarps && visibility==0,"header capture loss restores visibility without moving pointer");
    }
    for (int operation : {1,2}) {
        capture=foreground=main;held=true;control=false;app->CtrlModifierState()=0;app->LeftMouseButtonState()=0;
        headerPoint(operation);
        const POINT clicked{app->MouseX(),app->MouseY()};
        pointer=clicked;ClientToScreen(main,&pointer);const auto stationary=pointer;
        warpSucceeds=false;
        Check(HandleViewportToolPress(app) && app->MouseX()==clicked.x && app->MouseY()==clicked.y &&
              app->PreviousMouseX()==clicked.x && app->PreviousMouseY()==clicked.y &&
              pointer.x==stationary.x && pointer.y==stationary.y,
              "failed initial centering retains the actual pointer sample and matching previous coordinates");
        const auto cameraValues=[&]{return std::array<float,4>{app->CameraDistance(),app->CameraPositionX(),app->CameraPositionY(),app->CameraPositionZ()};};
        const auto before=cameraValues();
        MouseInteractionBegin(app);MouseInteractionEnd(app);
        Check(cameraValues()==before && app->MouseX()==clicked.x && app->MouseY()==clicked.y,
              "stationary pointer after denied initial warp cannot manufacture camera movement");
        held=false;HandleViewportToolRelease(app);warpSucceeds=true;
        app->LeftMouseButtonState()=0;held=true;headerPoint(operation);
        Check(HandleViewportToolPress(app),"start a fresh successful centered drag before later warp failure");
        pointer={view.left-3,view.top-5};ClientToScreen(main,&pointer);
        warpSucceeds=false;
        MouseInteractionBegin(app);MouseInteractionEnd(app);
        Check(app->MouseX()==view.left-3 && app->MouseY()==view.top-5 &&
              app->PreviousMouseX()==view.left-3 && app->PreviousMouseY()==view.top-5,
              "failed boundary centering preserves the real out-of-bounds current and previous samples");
        const auto after=cameraValues();
        MouseInteractionBegin(app);MouseInteractionEnd(app);
        Check(cameraValues()==after,"stationary pointer after a failed boundary warp cannot repeat a phantom delta");
        warpSucceeds=true;RecentreCenteredDragCursor(app);
        Check(app->MouseX()==(view.left+view.right)/2 && app->MouseY()==(view.top+view.bottom)/2 &&
              app->PreviousMouseX()==app->MouseX() && app->PreviousMouseY()==app->MouseY(),
              "a later successful boundary warp restores the established centered sampling behavior");
        held=false;HandleViewportToolRelease(app);
    }
    const auto camera = [&] {
        return std::array<float, 6>{app->CameraDistance(), app->CameraPositionX(), app->CameraPositionY(),
            app->CameraPositionZ(), app->ViewOffsetX(), app->ViewOffsetY()};
    };
    const auto cameraBefore = camera();
    const float savedScale = app->Renderer()->viewScale;
    const auto move = [&](int dx, int dy) {
        pointer = {app->PreviousMouseX() + dx, app->PreviousMouseY() + dy};
        ClientToScreen(main, &pointer);
        MouseInteractionBegin(app); MouseInteractionEnd(app);
    };
    const auto press = [&](int operation) {
        capture = foreground = main; held = control = true;
        app->LeftMouseButtonState() = 0; app->CtrlModifierState() = 1;
        headerPoint(operation);
        Check(HandleViewportToolPress(app), "Ctrl header drag starts at the displayed icon");
    };
    app->PictureWidth() = app->AviFrameWidth() = 160;
    app->PictureHeight() = app->AviFrameHeight() = 96;
    // Video takes priority over a visible picture; hiding it exposes the picture.
    for (bool video : {false, true}) {
        app->PictureBackgroundEnabled() = 1; app->AviBackgroundEnabled() = video;
        auto& x = video ? app->AviOffsetX() : app->PictureOffsetX();
        auto& y = video ? app->AviOffsetY() : app->PictureOffsetY();
        auto& mediaScale = video ? app->AviScale() : app->PictureScale();
        for (float viewportScale : {.6f, 1.2f, 2.4f}) {
            app->Renderer()->viewScale = viewportScale;
            x = 20; y = 15; mediaScale = 1;
            press(2);
            // Releasing Ctrl during a gesture must never switch to the camera.
            control = false;
            for (int i = 0; i < 10; ++i) move(1, -1);
            const double factor = 1.2000000476837158 / viewportScale;
            Check(x == std::llround(20 + 10 * factor) && y == std::llround(15 - 10 * factor),
                  "background follows small pointer movements at every viewport scale");
            Check(camera() == cameraBefore, "background pan leaves all camera values unchanged");
            held = false; HandleViewportToolRelease(app);
            const double centreX = x + 80 * mediaScale, centreY = y + 48 * mediaScale;
            press(1); move(0, -20);
            Check(mediaScale > 1, "Ctrl zoom enlarges background when dragging upward");
            Check(std::abs(x + 80 * mediaScale - centreX) <= .51 &&
                  std::abs(y + 48 * mediaScale - centreY) <= .51, "zoom preserves background centre");
            move(0, 20);
            Check(std::abs(mediaScale - 1) < .00001, "reverse zoom restores background size");
            Check(camera() == cameraBefore, "background zoom leaves all camera values unchanged");
            float* vertices = nullptr;
            auto* buffer = video ? app->AviOverlayVertices() : app->PictureOverlayVertices();
            Check(buffer && SUCCEEDED(buffer->Lock(0, 0, reinterpret_cast<void**>(&vertices), D3DLOCK_READONLY)),
                  "read background geometry after drag");
            Check(std::abs(vertices[14] - (view.left + x / factor - 1)) < .01 &&
                  std::abs(vertices[1] - (view.top + y / factor - 1)) < .01,
                  "drag immediately updates the actual rendered background vertices");
            buffer->Unlock();
            capture = nullptr; held = false; HandleViewportToolRelease(app);
            Check(visibility == 0 && app->backgroundGesture.target == MMDApp::BackgroundGesture::Target::Camera,
                  "cancelling background gesture restores the pointer and clears its target");
        }
        if (video) {
            const int pictureX = app->PictureOffsetX(), pictureY = app->PictureOffsetY();
            press(2); move(11, 7); held = false; HandleViewportToolRelease(app);
            Check(app->PictureOffsetX() == pictureX && app->PictureOffsetY() == pictureY,
                  "topmost video gesture does not move the picture underneath");
        }
    }
#if !MMD_ORIGINAL_PROFILE
    // Drive the actual header press/frame samples while changing the production
    // identity registry exactly as loaders do. These fixtures model identity
    // publication; they do not claim to exercise another codec or file decoder.
    namespace identity = plugins::background_identity;
    using Target = MMDApp::BackgroundGesture::Target;
    const auto release = [&] { held = false; HandleViewportToolRelease(app); };
    for (bool video : {false, true}) {
        app->PictureBackgroundEnabled() = 1; app->AviBackgroundEnabled() = video;
        auto& x = video ? app->AviOffsetX() : app->PictureOffsetX();
        auto& y = video ? app->AviOffsetY() : app->PictureOffsetY();
        auto& mediaScale = video ? app->AviScale() : app->PictureScale();
        const auto kind = video ? identity::Kind::Video : identity::Kind::Picture;
        auto* path = video ? app->AviBackgroundPath() : app->PictureBackgroundPath();
        wcscpy_s(path, 256, video ? L"same-background.avi" : L"same-background.bmp");
        x = 20; y = 15; mediaScale = 1;
        press(2); move(3, 4);
        const auto oldTicket = app->backgroundGesture.media;
        {
            identity::Mutation replacement(app, !video, video);
            x = 71; y = 82; mediaScale = .75f;
        }
        Check(identity::Acquire(app, kind) != oldTicket, "same-path replacement receives a different media identity");
        move(11, 7);
        Check(x == 71 && y == 82 && mediaScale == .75f && app->backgroundGesture.target == Target::Empty,
              "old header drag cannot overwrite a same-path replacement");
        move(5, 5);
        Check(x == 71 && y == 82 && mediaScale == .75f && camera() == cameraBefore,
              "stale media drag stays inert until release and never falls through to camera");
        release();

        // A new object at an old address has a different application domain,
        // even when media kind, path and placement are unchanged.
        press(1);
        const auto oldInstance = app->backgroundGesture.instance;
        { identity::Mutation closing(app, true, true, true); }
        Check(identity::Register(app) && identity::Instance(app) != oldInstance,
              "closed application address registers a fresh identity");
        move(0, -10);
        Check(x == 71 && y == 82 && mediaScale == .75f && app->backgroundGesture.target == Target::Empty,
              "stale drag cannot write into a new application identity at the same address");
        release();

        {
            identity::Mutation replacing(app, !video, video, false, !video);
            press(2); move(8, 9);
            Check(app->backgroundGesture.target == Target::Empty && x == 71 && y == 82 && mediaScale == .75f,
                  "cannot start placement while the selected media is being mutated");
        }
        move(8, 9);
        Check(x == 71 && y == 82 && mediaScale == .75f && camera() == cameraBefore,
              "a rejected press remains rejected when media mutation ends");
        release();

        press(2);
        {
            identity::Mutation replacing(app, !video, video, false, !video);
            move(8, 9);
            Check(x == 71 && y == 82 && mediaScale == .75f && app->backgroundGesture.target == Target::Empty,
                  "active media mutation rejects the whole drag sample without a partial write");
        }
        move(8, 9);
        Check(x == 71 && y == 82 && mediaScale == .75f,
              "a rejected sample does not resume after a failed replacement");
        release();

        if (!video) {
            press(2);
            const auto ticket = app->backgroundGesture.media;
            { identity::Mutation failedCandidate(app, true, false, false, true); }
            Check(identity::IsCurrent(app, kind, ticket), "unpublished failed picture candidate preserves old media identity");
            move(8, 9);
            Check(app->backgroundGesture.target == Target::Picture && (x != 71 || y != 82),
                  "existing drag continues after a failed candidate when no sample crossed the mutation");
            release();
        }
        x = 20; y = 15; mediaScale = 1;
        press(2); mediaScale = .625f;
        move(8, 9);
        Check(x == 20 && y == 15 && mediaScale == .625f && app->backgroundGesture.target == Target::Empty,
              "pan rejects an external scale edit without writing either offset");
        release();

        // Hiding the selected media ends this preview; showing it again must
        // not resume the old pointer deltas or select the picture underneath.
        x = 20; y = 15; mediaScale = 1;
        press(2);
        if (video) app->AviBackgroundEnabled() = 0;
        else app->PictureBackgroundEnabled() = 0;
        move(7, 8);
        Check(app->backgroundGesture.target == Target::Empty && x == 20 && y == 15,
              "hiding selected media rejects the pending placement sample");
        if (video) app->AviBackgroundEnabled() = 1;
        else app->PictureBackgroundEnabled() = 1;
        move(7, 8);
        Check(x == 20 && y == 15 && mediaScale == 1 && camera() == cameraBefore,
              "showing media again does not resume a cancelled preview");
        release();

        for (float invalidScale : {std::numeric_limits<float>::quiet_NaN(),
                                   std::numeric_limits<float>::infinity(), -1.0f, 0.0f}) {
            x = 20; y = 15; mediaScale = invalidScale;
            press(1); move(0, -20);
            Check(std::isfinite(mediaScale) && mediaScale > 1 &&
                  app->backgroundGesture.target == (video ? Target::Video : Target::Picture),
                  "zoom normalizes invalid starting scale without invalid integer conversion");
            release();
        }
        x = 20; y = 15; mediaScale = 1;
        press(1);
        // The subtraction is representable: -1 - INT_MAX == INT_MIN.
        app->PreviousMouseY() = (std::numeric_limits<int>::max)();
        move(0, (std::numeric_limits<int>::min)());
        Check(mediaScale == 100 && camera() == cameraBefore,
              "minimum signed mouse delta enlarges media to the upper zoom limit");
        release();

        // Any external field change terminates ownership of this preview.
        // Zoom must not write its scale first and discover the conflict later.
        for (unsigned field = 0; field != 3; ++field) {
            x = 20; y = 15; mediaScale = 1;
            press(1);
            if (field == 0) x = 123; else if (field == 1) y = 234; else mediaScale = .5f;
            const int foreignX = x, foreignY = y; const float foreignScale = mediaScale;
            move(0, -20);
            Check(x == foreignX && y == foreignY && mediaScale == foreignScale &&
                  app->backgroundGesture.target == Target::Empty && camera() == cameraBefore,
                  "external placement edit cancels all preview writes, including zoom scale");
            release();
            Check(x == foreignX && y == foreignY && mediaScale == foreignScale,
                  "ending a conflicted drag does not roll back external placement");
        }
    }
#endif
    app->PictureBackgroundEnabled() = app->AviBackgroundEnabled() = 0;
    press(2); move(20, 20); held = false; HandleViewportToolRelease(app);
    Check(camera() == cameraBefore, "Ctrl drag without visible media does not move camera");
    app->Renderer()->viewScale = savedScale;
    control = false;
    for (void* api : {reinterpret_cast<void*>(&GetForegroundWindow), reinterpret_cast<void*>(&GetCapture),
                     reinterpret_cast<void*>(&GetKeyState), reinterpret_cast<void*>(&GetAsyncKeyState),
                     reinterpret_cast<void*>(&GetCursorPos), reinterpret_cast<void*>(&SetCursorPos), reinterpret_cast<void*>(&SetCursor),
                     reinterpret_cast<void*>(&ShowCursor)}) {
        MH_DisableHook(api); MH_RemoveHook(api);
    }
    app->state = *saved;
    app->backgroundGesture = savedGesture;
    std::puts("PASS camera and Ctrl background gestures, scaled header hit testing, cursor cancellation and geometry");
}
}
