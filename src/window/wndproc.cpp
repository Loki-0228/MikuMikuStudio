#include "mikumikustudio/background_save.hpp"
#include "mikumikustudio/legacy_mmd_plugin.hpp"
#include "window_settings.hpp"
#include "mikumikustudio/recording_state.hpp"
#include "mikumikustudio/autosave.hpp"
#include "cache_manager_dialog.hpp"
#include "mikumikustudio/plugin_menu.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "mikumikustudio/nvidia_antialias_menu.hpp"
#include "mikumikustudio/system_settings_ui.hpp"
#include "mikumikustudio/audio_menu.hpp"
#include "mikumikustudio/picture_menu.hpp"
#include "../plugins/native_picture_history.hpp"
#include "mikumikustudio/menu_initial_paint.hpp"
// ===========================================================================
// VA 0x004C3A10 - MainWndProc  (original: sub_4C3A10, 0x8A6 bytes)
// ===========================================================================
// Full port of the main window message procedure.  Every entry first sets
// the message-seen flag (this+658796 = 1), then dispatches:
//   WM_CREATE      -> CreateUIControls (0x466D20); failure exits process
//   WM_DESTROY     -> save Data\mmconfig.ini (mirror of the 0x47A5B0 load)
//                     then PostQuitMessage
//   WM_SIZE        -> flag 672812 toggles around HandleWindowSize (0x443300,
//                     ui_windowsize.cpp)
//   WM_PAINT       -> HandleWindowPaint (0x47C0A0, wm_paint.cpp)
//   WM_CLOSE       -> dirty-flag confirm dialogs (EN + JP texts; JP bytes
//                     backfilled from x64 0x7FF7CB552700/0x7FF7CB552798)
//   WM_ERASEBKGND  -> suppress (D3D owns the client area)
//   WM_NOTIFY      -> HandleNotify (0x4398B0, ui_notify.cpp)
//   WM_COMMAND     -> CommandDispatch (0x47E8A0, command_dispatch.cpp, 68 KB)
//   WM_DROPFILES   -> HandleDropFiles (0x461300, ui_dropfiles.cpp)
//   0x318 (792)    -> palette: HandlePaletteChanged (0x42CEB0) +
//                     HandlePaletteChanged2 (0x42C140) (ui_palette.cpp)
//   WM_TIMER       -> timer 100: sub_429770; timer 101: the Kinect
//                     auto-frame-record stage byte (app+658792 = 0xA0D68,
//                     state.autoRepeat / x64 0xA1E14), walked 1->2->3->4
//                     at 1500 ms intervals
//   WM_H/VSCROLL   -> HandleHScroll (0x44AEE0, ui_hscroll.cpp) /
//                     HandleVScroll (0x44BB30, ui_scroll_mouse.cpp)
//   WM_CTLCOLORSTATIC / WM_CTLCOLORBTN -> HandleCtlColor for themed labels
//                     (0x40E0E0, ui_ctlcolor.cpp)
//   WM_MOUSE*      -> capture handling + sub_446A70/44A9A0/44AAA0/4632F0
//   WM_MOUSEWHEEL  -> HandleMouseWheel (0x44BD70, ui_scroll_mouse.cpp)
//
// While a replaced PMM scene waits for its recovery reset
// (MMDApp::sceneRecoveryPending) the edit input is dropped before the viewport
// hooks can latch a gesture.  Only File > Exit/New/Open, the recent-project
// list and the window-lifetime/layout/paint paths above stay live until a
// reset recovers the scene.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/frame_rate_correction.hpp"
#include "playback_timing_ui.hpp"
#include <stdio.h>
#include <cstdlib>

#include <cstdint>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/recent_projects.hpp"
#include "mikumikustudio/bottom_panel_resize.hpp"
#include "mikumikustudio/ui_message.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/skinning_menu.hpp"
#include "mikumikustudio/physics_bake.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "viewport_corner_menu.hpp"
#include "../plugins/native_viewport_history.hpp"

namespace mikumikustudio {

namespace {

// Store the mouse position a press message carries, mirroring the store in
// HandleMouseMove (0x444D5B, including the 0xEA60 wrap).
//
// The original writes the stored position only from its mouse-move handler
// (x64 0x381A4..0x3821D): WM_LBUTTONDOWN there just takes capture (0xA4B38)
// and WM_LBUTTONDBLCLK only calls its double-click handler (0xA4A86), so the
// stored position always describes the pointer.  The same store moves the
// previous position with it - and sets mouseJumped - when it follows a cursor
// warp (0x381CF..0x3821D), which is what keeps a warp out of the drag deltas.
//
// This port additionally latches the message's own coordinates, because a
// press can arrive before its move was stored (see the WM_LBUTTONDOWN
// comment).  That value is stale while a centred drag already owns the press:
// the six operation buttons (operations 9..20) warp the pointer to the viewport
// centre and store that centre as both the current and the previous position,
// so latching the button's coordinates would leave previousMouse at the centre
// and the next frame would apply the whole centre-to-button distance as one
// delta - the reported "the view jumps after a fast double click on the
// operation buttons".  The drag owns the press from its own press handling
// until the release, which is exactly when the message's coordinates describe
// the button rather than the pointer.
void LatchPressPosition(MMDApp& s, LPARAM lParam) {
    const int operation = static_cast<int>(s.ViewToolDragOperation());
    const bool centredDragOwnsPress =
        operation >= static_cast<int>(ViewportToolAction::TransformAxisX) &&
        operation <= static_cast<int>(ViewportToolAction::ScaleZ) &&
        operation != static_cast<int>(ViewportToolAction::CycleCoordinateSystem) &&
        s.LeftMouseButtonState() != 0;
    if (centredDragOwnsPress)
        return;
    const int clickX = static_cast<int>(lParam & 0xFFFFu);
    const int clickY = static_cast<int>((lParam >> 16) & 0xFFFFu);
    if (s.MouseX() != clickX || s.MouseY() != clickY) {
        s.MouseX() = clickX > 0xEA60 ? clickX - 0x10000 : clickX;
        s.MouseY() = clickY > 0xEA60 ? clickY - 0x10000 : clickY;
    }
}

// A PMM load that already replaced the native scene can fail its recovery
// reset; the half-loaded scene then stays quarantined (see
// MMDApp::sceneRecoveryPending) until a reset completes.  It must take no
// edits meanwhile.  Drop the input that would edit it or start a gesture.
// Close requests, the system menu (Alt+Space/Alt+F4), layout, paint and the
// recovery reset's own callbacks keep their normal path.
bool SceneRecoveryBlocksInput(const MMDApp& app, UINT msg, WPARAM wParam) noexcept {
    if (!app.sceneRecoveryPending) return false;
    // A dropped file loads a project/model/picture, control notifications and
    // value scrolls dispatch panel edits, and mouse/key messages start
    // viewport/timeline gestures.  This sits ahead of
    // HandleBoundViewportMessage and of the mouse-modifier stores, so neither
    // can write gesture or modifier state for the scene; the caller consumes
    // the WM_DROPFILES handle with DragFinish.
    if (msg == WM_DROPFILES || msg == WM_NOTIFY ||
        msg == WM_HSCROLL || msg == WM_VSCROLL) return true;
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return true;
    if (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) {
        // Alt+Space / Alt+F4 are system-window keys: DefWindowProc turns them
        // into the system menu and SC_CLOSE.
        if (msg >= WM_SYSKEYDOWN && msg <= WM_SYSDEADCHAR) return false;
        // Escape only cancels an in-flight gesture or leaves fullscreen.
        return !(msg == WM_KEYDOWN && wParam == VK_ESCAPE);
    }
    return false;
}

// Only a real menu (notification 0) or accelerator (notification 1) command
// carries lParam 0; a child control always supplies its own window, so a
// control whose numeric id collides with a menu command cannot pass as that
// command.
bool SceneRecoveryIsMenuCommand(WPARAM wParam, LPARAM lParam) noexcept {
    return lParam == 0 && HIWORD(wParam) <= 1;
}

// Of the real menu/shortcut commands only File > Exit/New/Open (200/204/205)
// may run on the quarantined scene: they close the window or replace/reset the
// scene through the existing recovery paths.
bool SceneRecoveryAllowsCommand(WPARAM wParam, LPARAM lParam) noexcept {
    if (!SceneRecoveryIsMenuCommand(wParam, lParam)) return false;
    const unsigned id = LOWORD(wParam);
    return id == 200 || id == 204 || id == 205;
}

}  // namespace

// WM_CLOSE 确认框的 JP 文案，SJIS 字节自 x64 实读回填（MainWndProc
// 0x7FF7CB4FBBF0 引用；x86 侧地址 0x531810/0x531794/0x531864 已弃用）：
static const char kMsgQuitEnhancedJp[] =                 // x64 0x7FF7CB552700
    "\x27\x8A\x67\x92\xA3\x83\x82\x83\x66\x83\x8B\x95\xDB\x91\xB6\x27"
    "\x82\xB5\x82\xC4\x82\xA2\x82\xC8\x82\xA2\x8A\x67\x92\xA3\x95\xD2\x8F\x57"
    "\x82\xB5\x82\xBD\x83\x82\x83\x66\x83\x8B\x82\xAA\x82\xA0\x82\xE8\x82\xDC"
    "\x82\xB7\n\n"
    "\x8F\x49\x97\xB9\x82\xB5\x82\xC4\x82\xE6\x82\xEB\x82\xB5\x82\xA2\x82\xC5"
    "\x82\xB7\x82\xA9\x81\x48";
// "'拡張モデル保存'していない拡張編集したモデルがあります\n\n終了してよろしいですか！"
static const char kMsgQuitModifiedJp[] =                 // x64 0x7FF7CB552798
    "\x95\xDB\x91\xB6\x82\xB5\x82\xC4\x82\xA2\x82\xC8\x82\xA2\x95\xCF\x8D\x58"
    "\x93\x5F\x82\xAA\x82\xA0\x82\xE8\x82\xDC\x82\xB7\n\n"
    "\x8F\x49\x97\xB9\x82\xB5\x82\xC4\x82\xE6\x82\xEB\x82\xB5\x82\xA2\x82\xC5"
    "\x82\xB7\x82\xA9\x81\x48";
// "保存していない変更点があります\n\n終了してよろしいですか！"
static const char kCaptionQuitJp[] = "\x8F\x49\x97\xB9";  // x64 0x7FF7CB5526F8 "終了"

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
#if !MMD_ORIGINAL_PROFILE
    // Subsequent menu messages use one subclass route, including cancellation
    // inside the native handler. Do not also dispatch them from this procedure.
    if (msg == WM_CREATE) menu_initial_paint::Attach(hwnd);
#endif
    updates::MessageScope dispatchScope;
    MMDApp* app = g_Block;
    auto& s = *app;
#if !MMD_ORIGINAL_PROFILE
    const bool replayClose = msg == kSceneCloseReplayMessage;
    const bool closeRequest = replayClose || msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (wParam & 0xfff0) == SC_CLOSE);
    if (msg == WM_NCDESTROY) {
        s.deferredSceneMainClose = nullptr;
        s.sceneMainClosePosted = false;
    }
    if (replayClose) {
        if (!s.sceneMainClosePosted || s.deferredSceneMainClose != hwnd) return 0;
        s.sceneMainClosePosted = false;
    }
    if (closeRequest) {
        if (s.sceneMainCloseProcessing) return 0;
        if (SceneCloseBlocked(s)) { s.deferredSceneMainClose = hwnd; return 0; }
        if (!replayClose && (s.sceneMainClosePosted || s.deferredSceneMainClose)) return 0;
        s.deferredSceneMainClose = nullptr;
        s.sceneMainCloseProcessing = true;
        // Dispatch directly: a second WM_CLOSE could be swallowed by an outer
        // subclass, and SC_CLOSE would otherwise generate a reentrant close.
        msg = WM_CLOSE; wParam = 0; lParam = 0;
    }
    struct CloseGuard {
        MMDApp& app; bool active;
        ~CloseGuard() { if (active) { app.sceneMainCloseProcessing = false; RetryDeferredSceneCloses(app); } }
    } closeGuard{s, closeRequest};
#endif
    const bool finishingRecording = recording_state::Stopping(app);
    const bool startingRecording = recording_state::Guard::Active(app, recording_state::Operation::Start);
    const bool drainingRecording = recording_state::Guard::Active(s.Recorder(), recording_state::Operation::GraphTeardown);
    // A quarantined scene must not take an edit or start a gesture.  This runs
    // before HandleBoundViewportMessage can latch a viewport/HUD gesture and
    // write the mouse modifiers it carries, and before the mouse-modifier
    // stores in the dispatch below.  WM_CLOSE/SC_CLOSE, the system menu, the
    // remaining window-lifetime/layout/paint messages and the recovery reset's
    // own callbacks keep their normal path.
    if (SceneRecoveryBlocksInput(s, msg, wParam)) {
        if (msg == WM_DROPFILES) DragFinish(reinterpret_cast<HDROP>(wParam));
        return 0;
    }
#if !MMD_ORIGINAL_PROFILE
    if (!finishingRecording && !drainingRecording && !startingRecording &&
        plugins::HandleBoundViewportMessage(s, hwnd, msg, wParam, lParam)) return 0;
#endif
    // EnableWindow prevents new user input; queued commands and accelerators
    // still need to be rejected while a bake's message loop is pumping.
    if ((finishingRecording || drainingRecording || startingRecording) &&
        (msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (wParam & 0xfff0) == SC_CLOSE))) {
        recording_state::Guard::DeferClose((finishingRecording || startingRecording) ? static_cast<const void*>(app) : s.Recorder(),
            finishingRecording ? recording_state::Operation::Finish : startingRecording ? recording_state::Operation::Start : recording_state::Operation::GraphTeardown,
            reinterpret_cast<std::uintptr_t>(hwnd));
        return 0;
    }
    if (SceneSaveBlocksEdits() &&
        (msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (wParam & 0xfff0) == SC_CLOSE))) {
        background_save::DeferClose(app);
        return 0;
    }
    if (bake::UiLocked() || SceneSaveBlocksEdits() || finishingRecording || drainingRecording || startingRecording) {
        if (msg == WM_DROPFILES) { DragFinish(reinterpret_cast<HDROP>(wParam)); return 0; }
        if (msg == WM_COMMAND || msg == WM_NOTIFY || msg == WM_CLOSE ||
            msg == WM_HSCROLL || msg == WM_VSCROLL ||
            (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
            (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
            (msg == WM_SYSCOMMAND && (wParam & 0xfff0) == SC_CLOSE)) return 0;
    }
    if (background_save::HandleMessage(app, msg, wParam)) return 0;
    // recent_projects dispatches its WM_COMMAND rows before the gate below and
    // does not inspect lParam itself.  While the scene is quarantined a child
    // control could reuse one of its ids, so only a real menu/accelerator
    // command may enter that command path; control commands still reach the
    // ordinary dispatch (and the recovery reset's own control callbacks)
    // through the gate below.
    const bool isolatedRecentCommand = s.sceneRecoveryPending && msg == WM_COMMAND &&
        !SceneRecoveryIsMenuCommand(wParam, lParam);
    if (!isolatedRecentCommand &&
        recent_projects::HandleMessage(app, msg, wParam, lParam)) return 0;
    // File > Open's recent-project list above stays live: it calls
    // LoadSceneFile, whose pending-recovery gate runs the reset that can clear
    // the quarantine.  Every other menu, accelerator or child-control command
    // would edit the half-loaded scene, so only a real File > Exit/New/Open
    // command passes (see SceneRecoveryAllowsCommand).  The recovery reset
    // drives its own child controls synchronously (BM_CLICK and edit
    // notifications), so those control commands keep the ordinary dispatch
    // while the reset's scene mutation is in progress, exactly like a normal
    // reset.
    if (msg == WM_COMMAND && s.sceneRecoveryPending &&
        !(s.SceneMutationInProgress() && lParam != 0) &&
        !SceneRecoveryAllowsCommand(wParam, lParam)) return 0;
    if (antialias::HandleMessage(app, msg, wParam, lParam)) return 0;
    if (nvidia_antialias_menu::HandleMessage(app, msg, wParam, lParam)) return 0;
    if (HandleBottomPanelResize(app, msg, wParam, lParam)) return msg == WM_SETCURSOR ? TRUE : 0;
    if (msg == WM_INITMENU) { plugin_menu::Refresh(app); plugins::Refresh(app); }
    if (msg == WM_INITMENUPOPUP) {
        // Menu state can survive a programmatic model/panel change. Resolve
        // availability from the current selection whenever a popup opens.
        SetModelExtensionMenuEnabled(app,
            app->CameraMode() == 0 && app->SelectedModel() != nullptr);
        // Accessory ordering is independent of transparent material sorting.
        // Recompute after loads, deletion and menu recreation/language changes.
        bool hasAccessory = false;
        for (int slot = 0; slot < 255; ++slot)
            if (app->AccessorySlot(slot) != nullptr) { hasAccessory = true; break; }
        EnableMenuItem(GetMenu(hwnd), 249, MF_BYCOMMAND |
            (hasAccessory ? MF_ENABLED : MF_GRAYED));
        RefreshSkinningMenu(app); bake::RefreshMenu(app);
        frame_rate_correction::RefreshMenu(app);
        playback_timing_ui::RefreshMenu(app);
        // Keep the Help menu's current dock-corner label and UI language fresh.
        viewport_corner_menu::EnsureItem(app);
        system_settings_ui::EnsureItem(app);
#if !MMD_ORIGINAL_PROFILE
        audio_menu::EnsureItem(app);
        picture_menu::EnsureItem(app);
        // An unbound native picture removal has no history refresh queue.
        // Retry its pending menu state at the next safe menu opening too.
        plugins::RefreshBoundPictureUi(*app);
        cache_manager_ui::RefreshMenu(app);
#endif
    }
    if (msg == WM_INITMENUPOPUP) autosave::RefreshMenu(app);
    if (msg == WM_NCPAINT || msg == WM_NCACTIVATE || msg == WM_SETTEXT || msg == WM_EXITMENULOOP) {
        const auto result = DefWindowProcW(hwnd, msg, wParam, lParam);
        autosave::PaintMenuWarning(app);
        return result;
    }
    if (msg == WM_PRINT && (lParam & PRF_NONCLIENT)) {
        const auto result = DefWindowProcW(hwnd, msg, wParam, lParam);
        autosave::PaintMenuWarning(app, reinterpret_cast<HDC>(wParam));
        return result;
    }
    if (build_profile::original) s.MessageSeen() = 1;
    else updates::ObserveMessage(app, msg, wParam, lParam);
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
        msg == WM_LBUTTONDBLCLK || msg == WM_MOUSEMOVE) {
        // Mouse messages carry the modifiers for this gesture; frame-loop
        // polling can still contain the previous click's modifier state.
        s.ShiftModifierState() = (wParam & MK_SHIFT) != 0 ? 3 : 0;
        s.CtrlModifierState() = (wParam & MK_CONTROL) != 0 ? 3 : 0;
    }

    if (msg <= WM_COMMAND /*0x111*/) {
        if (msg == WM_COMMAND) {
            CommandDispatch(reinterpret_cast<HWND>(lParam),
                            wParam);  // 0x47E8A0
            return 0;
        }
        switch (msg) {
        case WM_SYSKEYDOWN:
            if (wParam == VK_RETURN && (lParam & (LPARAM(1) << 29)) != 0) {
                if ((lParam & (LPARAM(1) << 30)) == 0 && s.FrameStepPlayback() == 0) {
                    s.state.enterKeyState = 3;
                    s.FullscreenMode() = s.FullscreenMode() == 0 ? 1 : 0;
                    ApplyFullscreenWindowState(app);
                    if (!PostDeviceReset(app) && s.FullscreenMode() != 0) {
                        s.FullscreenMode() = 0;
                        ApplyFullscreenWindowState(app);
                        PostDeviceReset(app);
                    }
                }
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE && (s.SidebarResizeDragging()
#if !MMD_ORIGINAL_PROFILE
                || s.sidebarSizing.preparing
#endif
                )) {
#if !MMD_ORIGINAL_PROFILE
                const auto serial = s.sidebarSizingSerial;
                const auto bottomSerial = s.bottomPanelSizingSerial;
#endif
                HandleLButtonUp(app);
#if !MMD_ORIGINAL_PROFILE
                if ((s.sidebarSizingSerial != serial || s.bottomPanelSizingSerial != bottomSerial) &&
                    (s.sidebarSizing.active || s.sidebarSizing.preparing || s.bottomPanelSizing.dragging >= 0 || s.bottomPanelSizing.preparing)) return 0;
#endif
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            if (wParam == VK_ESCAPE && s.FullscreenMode() != 0 && s.FrameStepPlayback() == 0) {
                s.state.escKeyState = 3;
                s.FullscreenMode() = 0;
                ApplyFullscreenWindowState(app);
                PostDeviceReset(app);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_CANCELMODE: {
#if !MMD_ORIGINAL_PROFILE
            const auto serial = s.sidebarSizingSerial;
            const auto bottomSerial = s.bottomPanelSizingSerial;
#endif
            HandleLButtonUp(app);
#if !MMD_ORIGINAL_PROFILE
            if ((s.sidebarSizingSerial != serial || s.bottomPanelSizingSerial != bottomSerial) &&
                (s.sidebarSizing.active || s.sidebarSizing.preparing || s.bottomPanelSizing.dragging >= 0 || s.bottomPanelSizing.preparing)) return 0;
#endif
            if (GetCapture() == hwnd)
                ReleaseCapture();
            return 0;
        }
        case WM_CREATE:
            if (!CreateUIControls(app, hwnd))                     // 0x466D20
                exit(1);
            return 0;

        case WM_DESTROY: {
            runtime_log::Trace("WM_DESTROY begin");
            legacy_mmd_plugin::Stop();
            updates::Shutdown(app);
            plugins::Shutdown();
            autosave::Shutdown();
            runtime_log::Write("WM_DESTROY begin (window is closing)");
            WINDOWPLACEMENT wndpl;
            wndpl.length = sizeof(wndpl);
            GetWindowPlacement(hwnd, &wndpl);
            HMENU configMenu = GetMenu(hwnd);
            if (s.FullscreenMode() != 0) {
                // Fullscreen's placement/menu describe the monitor-sized
                // popup, not the user's ordinary window configuration.
                const WINDOWPLACEMENT& saved = s.SavedPlacement();
                if (saved.length == sizeof(WINDOWPLACEMENT))
                    wndpl = saved;
                configMenu = s.state.savedMenu;
            }
            SetCurrentDirectoryW(app->ExeDir());                  // +657102
            if (s.FloatingWindow() != nullptr) SaveFlagSubsystem(app);
            const HWND dlg530 = GetDlgItem(hwnd, panel::kPhysicsFrameCheckbox);
            system_settings::WriteSection("window", window_settings::Snapshot(s,
                wndpl.rcNormalPosition.left, wndpl.rcNormalPosition.top,
                wndpl.rcNormalPosition.right-wndpl.rcNormalPosition.left,
                wndpl.rcNormalPosition.bottom-wndpl.rcNormalPosition.top,
                wndpl.showCmd==SW_SHOWMAXIMIZED, s.FloatingWindow()!=nullptr,
                SendMessageA(dlg530,BM_GETCHECK,0,0)==BST_CHECKED,
                (GetMenuState(configMenu,0x119,0)&MF_CHECKED)!=0,
                (GetMenuState(configMenu,0x12D,0)&MF_CHECKED)!=0));
            PostQuitMessage(0);
            return 0;
        }

        case WM_SIZE: {
            const auto previousReady = s.WindowLayoutReady();
            s.WindowLayoutReady() = 0;                            // 672812
#if !MMD_ORIGINAL_PROFILE
            if (!ui::LayoutBatch::IsCollecting()) {
                const WindowDpiScope dpi(hwnd);
                ui::LayoutBatch batch;
                HandleWindowSize(app);
                batch.Commit();
                s.WindowLayoutReady() = 1;
                RECT damage{}; GetClientRect(hwnd, &damage);
                ui::RepaintLayout(hwnd, batch.Changes(), damage);
                return 0;
            }
#endif
            HandleWindowSize(app);                                // 0x443300
            s.WindowLayoutReady() = MMD_ORIGINAL_PROFILE ? 1 : previousReady;
            return 0;
        }

        case WM_PAINT:
            HandleWindowPaint(app);                               // 0x47C0A0
            return 0;

        case WM_CLOSE: {
            if (background_save::DeferClose(app)) return 0;
            runtime_log::Write("WM_CLOSE received enhancedDirty=%d sceneModified=%d",
                               s.EnhancedModelDirty(), static_cast<int>(s.SceneModified()));
            // Each branch below ends at DefWindowProcW once the user accepted
            // the close (or when there is nothing unsaved).  From that point
            // the program is exiting: teardown faults must not raise the modal
            // report any more, so the flag is set before the window is
            // destroyed and the configuration is written.
            if (s.EnhancedModelDirty() != 0) {                    // 658276
                // enhanced-model dirty flag
                const int r = ShowUiMessage(app, hwnd, UiMessage::QuitEnhanced,
                                            MB_OKCANCEL | MB_TOPMOST);
                if (r == IDOK) {                                  // original: == 1
                    runtime_log::BeginShutdown();
                    return DefWindowProcW(hwnd, msg, wParam, lParam);
                }
            } else {
                if (s.SceneModified() == 0) {                     // 658189
                    runtime_log::BeginShutdown();
                    return DefWindowProcW(hwnd, msg, wParam, lParam);
                }
                const int r = ShowUiMessage(app, hwnd, UiMessage::QuitUnsaved,
                                            MB_OKCANCEL | MB_TOPMOST);
                if (r == IDOK) {
                    runtime_log::BeginShutdown();
                    return DefWindowProcW(hwnd, msg, wParam, lParam);
                }
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 0;

        case WM_NOTIFY:
            return HandleNotify(hwnd, msg, wParam, lParam);       // 0x4398B0

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    if (msg > 0x233) {
        if (msg != 792 /*0x318*/)
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        HandlePaletteChanged(reinterpret_cast<HDC>(wParam));      // 0x42CEB0
        HandlePaletteChanged2(reinterpret_cast<HDC>(wParam));     // 0x42C140
        return 0;
    }

    if (msg == WM_DROPFILES /*0x233=563*/) {
        HandleDropFiles(reinterpret_cast<HDROP>(wParam));         // 0x461300
        return 0;
    }

    switch (msg) {
    case WM_TIMER:
        if (wParam == 100) {
            KillTimer(hwnd, 100);
            HandleTimer100(app);                                  // 0x429770
            return 0;
        }
        if (wParam != 101)
            return 0;
        KillTimer(hwnd, 101);
        ++s.AutoRepeatCount();                                    // 658792
        if (s.AutoRepeatCount() >= 4 || s.AutoRepeatCount() <= 0)
            return 0;
        SetTimer(hwnd, 101, 0x5DC, nullptr);                      // 1500 ms
        return 0;

    case WM_HSCROLL:
        HandleHScroll(lParam, wParam);                            // 0x44AEE0
        return 0;
    case WM_VSCROLL:
        HandleVScroll(lParam, wParam);                            // 0x44BB30
        return 0;
    case 0x138:  // WM_CTLCOLORSTATIC
        return HandleCtlColor(reinterpret_cast<HWND>(lParam),
                              reinterpret_cast<HDC>(wParam));  // 0x40E0E0
    case WM_CTLCOLORBTN: {
        const HWND control = reinterpret_cast<HWND>(lParam);
        const int controlId = GetDlgCtrlID(control);
        if (controlId == panel::kCameraRefModelCheckbox ||
            controlId == panel::kCameraRefBoneCheckbox ||
            controlId == panel::kAdaptiveShadowCheckbox || controlId == panel::kShadowFilterCheckbox)
            return HandleCtlColor(control, reinterpret_cast<HDC>(wParam));
        if (PaintBottomControlBackground(control, reinterpret_cast<HDC>(wParam)))
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_MOUSEMOVE:
        HandleMouseMove(static_cast<std::uint32_t>(lParam),
                        HIWORD(lParam));                          // 0x444CC0
        return 0;
    case WM_KEYDOWN:
        // Arrow keys are global MMD frame navigation. Prevent focused combo
        // boxes from changing the camera/model selection first.
        if (wParam == VK_LEFT || wParam == VK_RIGHT ||
            wParam == VK_UP || wParam == VK_DOWN) {
            HWND focus = GetFocus();
            wchar_t cls[32]{};
            if (focus != nullptr) GetClassNameW(focus, cls, 32);
            if (focus == hwnd || _wcsicmp(cls, L"EDIT") != 0)
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_LBUTTONDOWN: {
#if !MMD_ORIGINAL_PROFILE
        if (s.sidebarSizing.active || s.sidebarSizing.preparing || s.bottomPanelSizing.dragging >= 0 || s.bottomPanelSizing.preparing) return 0;
        const auto serial = s.sidebarSizingSerial;
        const auto bottomSerial = s.bottomPanelSizingSerial;
#endif
        // HandleLButtonDown (0x446A70) judges the click on the *stored* mouse
        // position (state.mouseX/mouseY), which HandleMouseMove only writes
        // after its three auto-hide gates (0x444CC6) have let the move
        // through.  A press that arrives without a preceding stored move -
        // the first click after the window is re-activated, and every click
        // while a hide-rect gate is armed - was therefore routed with a stale
        // position: the derived row hit a row that was never painted (record
        // -999, type 0), so the bone list cleared its selection and selected
        // nothing ("clicking a bone does not select it").  The press carries
        // its own client coordinates in lParam; latch them first (see
        // LatchPressPosition for why a pending cursor warp keeps the pointer's
        // position instead).
        LatchPressPosition(s, lParam);
        SetCapture(static_cast<HWND>(s.Hwnd()));
#if !MMD_ORIGINAL_PROFILE
        if (s.sidebarSizingSerial != serial || s.bottomPanelSizingSerial != bottomSerial ||
            s.sidebarSizing.active || s.sidebarSizing.preparing || s.bottomPanelSizing.dragging >= 0 || s.bottomPanelSizing.preparing) return 0;
#endif
        // Commit the previously focused value before changing the selected morph.
        SetFocus(hwnd);
#if !MMD_ORIGINAL_PROFILE
        if (s.sidebarSizingSerial != serial || s.bottomPanelSizingSerial != bottomSerial ||
            s.sidebarSizing.active || s.sidebarSizing.preparing || s.bottomPanelSizing.dragging >= 0 || s.bottomPanelSizing.preparing) return 0;
#endif
        if (s.FloatingWindow() == nullptr && HandleViewportToolPress(app)) return 0;
        HandleLButtonDown(app);                                   // 0x446A70
#if !MMD_ORIGINAL_PROFILE
        // A divider press owns the click, including failed/cancelled takeover.
        if (s.sidebarSizingSerial != serial || s.bottomPanelSizingSerial != bottomSerial ||
            s.sidebarSizing.active || s.sidebarSizing.preparing || s.bottomPanelSizing.dragging >= 0 || s.bottomPanelSizing.preparing) return 0;
#endif
        if (s.FloatingWindow() == nullptr)
            HandleViewportBonePress(app, (wParam & MK_SHIFT) != 0);
        return 0;
    }
    case WM_LBUTTONUP: {
#if !MMD_ORIGINAL_PROFILE
        const auto serial = s.sidebarSizingSerial;
        const auto bottomSerial = s.bottomPanelSizingSerial;
        const auto superseded = [&] {
            if (s.sidebarSizingSerial == serial && s.bottomPanelSizingSerial == bottomSerial) return false;
            if (!s.sidebarSizing.active && !s.sidebarSizing.preparing &&
                s.bottomPanelSizing.dragging < 0 && !s.bottomPanelSizing.preparing && GetCapture() == hwnd)
                ReleaseCapture();
            return true;
        };
#endif
        if (s.BoneBoxSelectionActive() != 0)
            LatchPressPosition(s, lParam);
        // A release can carry a newer position than the last coalesced move.
        // Finish the gesture before clearing its range or releasing capture.
        if (s.SelectionBoxDragging() != 0 || s.SidebarResizeDragging() != 0)
            HandleMouseMove(static_cast<std::uint32_t>(lParam), HIWORD(lParam));
#if !MMD_ORIGINAL_PROFILE
        if (superseded()) return 0;
#endif
        CommitSidebarResize(app);
#if !MMD_ORIGINAL_PROFILE
        if (superseded()) return 0;
#endif
        HandleLButtonUp(app);                                     // 0x44A9A0
#if !MMD_ORIGINAL_PROFILE
        if ((s.sidebarSizingSerial != serial || s.bottomPanelSizingSerial != bottomSerial) &&
            (s.sidebarSizing.active || s.sidebarSizing.preparing || s.bottomPanelSizing.dragging >= 0 || s.bottomPanelSizing.preparing)) return 0;
#endif
        if (GetCapture() == hwnd)
            ReleaseCapture();
        return 0;
    }
    case WM_CAPTURECHANGED:
#if !MMD_ORIGINAL_PROFILE
        // Reacquisition can occur during a nested dispatch of the old loss.
        if (GetCapture() == hwnd) return 0;
#endif
        // A child control, dialog or another window can take capture without
        // sending mouse-up here. Do not leave a timeline drag intercepting
        // subsequent moves or clearing the highlight of a registered key.
        HandleLButtonUp(app);
        return 0;
    case WM_LBUTTONDBLCLK:
        LatchPressPosition(s, lParam);
        // The window class sets CS_DBLCLKS, so a second click within the double-click
        // time is delivered as this message, never as WM_LBUTTONDOWN.
        // The coordinate strip is a toggle and must handle both clicks;
        // every other tool keeps the handling below.
        if (s.FloatingWindow() == nullptr && HandleCoordinateStripPress(app))
            return 0;
        if (s.FloatingWindow() == nullptr)
            HandleViewportBonePress(app, (wParam & MK_SHIFT) != 0);
        HandleLButtonDblClk(app);                                 // 0x44AAA0
        return 0;
    case WM_RBUTTONDBLCLK:
        s.MouseX() = static_cast<std::int16_t>(LOWORD(lParam));
        s.MouseY() = static_cast<std::int16_t>(HIWORD(lParam));
        if (s.FloatingWindow() == nullptr) HandleViewportRightDoubleClick(app);
        return 0;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        SetCapture(static_cast<HWND>(s.Hwnd()));
        return 0;
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        ReleaseCapture();
        return 0;
    case WM_MOUSEACTIVATE:
        if (LOWORD(lParam) != HTCLIENT)
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        HandleMouseActivate(app);                                 // 0x4632F0
        return MA_ACTIVATE;
    case WM_MOUSEWHEEL:
        HandleMouseWheel(HIWORD(wParam), WheelClientX(hwnd, lParam, s.MouseX()));
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace mikumikustudio
