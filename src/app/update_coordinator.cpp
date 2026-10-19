#include "mikumikustudio/update_coordinator.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "device_lost_wait.hpp"
#include <native_ui/update.hpp>
#include <algorithm>
#include <cmath>

namespace mikumikustudio::updates {
namespace {
constexpr UINT kWake = WM_APP + 0x73;
struct State {
    MMDApp* app = nullptr;
    HWND wake = nullptr;
    native_ui::UpdateCoordinator queue;
    uint64_t generation = 0, geometry = 1, nextTick = 0;
    uint64_t previousTick = 0;
    unsigned messageDepth = 0;
    bool draining = false, frame = false, posted = false, sizeMove = false, repaintOnly = false;
} state;

uint64_t Now() {
    // GetTickCount64 can advance in roughly 16 ms steps even when the timer
    // period is 1 ms. Using it for a 16 ms deadline can halve preview cadence.
    // QPC is monotonic on supported Windows versions and also avoids the
    // 32-bit multimedia clock wrap in playback anchors.
    struct Clock {
        LARGE_INTEGER frequency{}, origin{};
        uint64_t epoch = GetTickCount64();
        uint64_t last = epoch;
        bool precise = QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0 && QueryPerformanceCounter(&origin);
    };
    static Clock clock;
    LARGE_INTEGER counter{};
    if (!clock.precise || !QueryPerformanceCounter(&counter)) return clock.last = std::max<uint64_t>(clock.last, GetTickCount64());
    const auto elapsed = uint64_t(std::max<LONGLONG>(0, counter.QuadPart - clock.origin.QuadPart));
    const auto frequency = uint64_t(clock.frequency.QuadPart);
    return clock.last = std::max(clock.last, clock.epoch + (elapsed / frequency) * 1000 + (elapsed % frequency) * 1000 / frequency);
}

bool Exporting(MMDApp* app) {
    return app->RecordingWindow() && (app->FrameStepPlayback() || app->state.recordPlaybackStartPending);
}
bool Displayable(MMDApp* app) {
    HWND window = app->FloatingWindow() ? app->FloatingWindow() : app->MainWindow();
    return window && IsWindow(window) && IsWindowVisible(window) && !IsIconic(window);
}
bool Blocked(MMDApp* app) {
    return !app || state.app != app || Running() ||
        (state.messageDepth && !(state.sizeMove && state.messageDepth == 1)) ||
        app->SceneMutationInProgress() || !app->WindowLayoutReady() ||
        bake::UiLocked() || plugins::InCallback() || device_lost_wait::Active();
}
DWORD Period(MMDApp* app) {
    if (Exporting(app)) return 1;
    float fps = app->FpsLimit();
    if (!std::isfinite(fps) || fps < 1) fps = 60;
    return std::clamp<DWORD>(DWORD(1000.0f / fps), 1, 1000);
}
void Wake() {
    if (!state.posted && state.wake) state.posted = PostMessageW(state.wake, kWake, 0, 0) != FALSE;
}
LRESULT CALLBACK WakeProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == kWake || message == WM_TIMER) {
        if (message == kWake) state.posted = false;
        // Timer dispatch also occurs inside move/size and modal loops. Only
        // the OS move/size loop is allowed to bypass the outer dispatch scope.
        SafePoint(state.app);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
}

bool Initialize(MMDApp* app) {
    if (build_profile::original) return true;
    if (!app || state.app) return state.app == app;
    WNDCLASSW wc{}; wc.lpfnWndProc = WakeProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MMS.UpdateCoordinator.V1";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    state.wake = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!state.wake) return false;
    if (!SetTimer(state.wake, 1, 16, nullptr)) { DestroyWindow(state.wake); state.wake = nullptr; return false; }
    state.app = app; ++state.generation; state.geometry = 1;
    state.previousTick = state.nextTick = Now();
    app->TimeNowLow() = DWORD(state.previousTick); app->TimeNowHigh() = DWORD(state.previousTick >> 32);
    state.queue.Request({1, state.generation, state.geometry, state.nextTick, native_ui::EvaluateDirty | native_ui::RenderDirty});
    Wake(); return true;
}
void Shutdown(MMDApp* app) {
    if (state.app != app || !app) return;
    state.app = nullptr; state.queue.Forget(1);
    HWND window = state.wake; state.wake = nullptr;
    state.posted = state.sizeMove = false;
    if (window) { KillTimer(window, 1); DestroyWindow(window); }
}
bool Running() { return state.frame || state.draining; }
bool RepaintOnly() { return state.repaintOnly; }
MessageScope::MessageScope() { ++state.messageDepth; }
MessageScope::~MessageScope() { --state.messageDepth; }
FrameScope::FrameScope() : entered_(!state.frame) { if (entered_) state.frame = true; }
FrameScope::~FrameScope() { if (entered_) state.frame = false; }

void RequestViewport(MMDApp* app, bool geometryChanged) {
    if (build_profile::original || !app || state.app != app) return;
    if (geometryChanged) ++state.geometry;
    state.queue.Request({1, state.generation, state.geometry, state.nextTick, native_ui::RenderDirty | native_ui::PresentDirty});
    Wake();
}
void Exposed(MMDApp* app, HWND window, const RECT& damaged) {
    if (!app) return;
    const HWND target = app->FloatingWindow() ? app->FloatingWindow() : app->MainWindow();
    if (window != target) return;
    RECT intersection{}, viewport = FramePresentationRect(app);
    if (IntersectRect(&intersection, &viewport, &damaged)) RequestViewport(app);
}
void ObserveMessage(MMDApp* app, UINT message, WPARAM wp, LPARAM lp) {
    if (build_profile::original || !app) return;
    if (message == WM_ENTERSIZEMOVE) state.sizeMove = true;
    if (message == WM_EXITSIZEMOVE) { state.sizeMove = false; RequestViewport(app, true); }
    if (message == WM_SIZE || message == WM_DISPLAYCHANGE || message == WM_SHOWWINDOW || message == WM_ENABLE) RequestViewport(app, message == WM_SIZE);
    // Passive paint, colour, hit-test and timer queries are not scene edits.
    // Legacy business commands still use their existing two-pass pose gate.
    bool changed = (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
        (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        message == WM_HSCROLL || message == WM_VSCROLL || message == WM_DROPFILES ||
        (message == WM_TIMER && (wp == 100 || wp == 101));
    if (message == WM_COMMAND) {
        const auto id = LOWORD(wp), notification = HIWORD(wp);
        changed = !(id >= 0xa000 && id <= 0xbfff) &&
            (!lp || (notification != EN_SETFOCUS && notification != EN_KILLFOCUS));
    }
    // MainWndProc's HandleNotify only paints native control labels
    // (NM_CUSTOMDRAW). Trackbar edits arrive as WM_HSCROLL above.
    if (changed) app->MessageSeen() = 1;
}
void SafePoint(MMDApp* app) {
    if (!app || state.app != app) return;
    // WM_TIMER is the modal-loop fallback. Re-arm according to the same
    // deadline as the outer pump, including blocked/hidden backoff.
    struct TimerGuard { MMDApp* app; ~TimerGuard() {
        if (state.app == app && state.wake) SetTimer(state.wake, 1, std::max<DWORD>(10, WaitMilliseconds(app)), nullptr);
    } } timer{app};
    if (Blocked(app)) return;
    if (state.messageDepth && !(state.sizeMove && state.messageDepth == 1)) return;
    HWND main = app->MainWindow();
    if (!main || !IsWindow(main)) return;
    const bool available = (IsWindowEnabled(main) || Exporting(app)) && (Displayable(app) || Exporting(app));
    state.queue.Available(1, available);
    const auto now = Now();
    if (!available) { state.previousTick = now; return; }
    if (now >= state.nextTick)
        state.queue.Request({1, state.generation, state.geometry, now, native_ui::EvaluateDirty});
    state.draining = true;
    struct Guard { ~Guard() { state.draining = false; } } guard;
    state.queue.Drain(now, 1, [app, now](const native_ui::UpdateRequest& request) {
        // A scene rendered after Present cannot be re-presented with DISCARD.
        // Exposure requests one replacement frame, without restarting the
        // legacy two-pass edit gate when the scene is otherwise stationary.
        state.repaintOnly = (request.flags & native_ui::RenderDirty) && app->MessageSeen() == 0 &&
            app->FrameVolumeControlEnabled() && !app->PlaybackActive() && !app->RecordingWindow() &&
            !app->FrameRangeDialog() && !app->state.depthDeviceEnabled &&
            !app->state.timelineAdvanceRequested && !app->state.timelineAdvanceDue;
        struct RepaintGuard { ~RepaintGuard() { state.repaintOnly = false; } } repaintGuard;
        if ((request.flags & native_ui::RenderDirty) && !state.repaintOnly && app->MessageSeen() == 0) app->MessageSeen() = 1;
        const auto tick = Now();
        const auto presentedBefore = app->frameRate.presentedFrames;
        app->DeltaTime() = float(tick >= state.previousTick ? tick - state.previousTick : 0) * 0.001f;
        app->TimeNowLow() = DWORD(tick); app->TimeNowHigh() = DWORD(tick >> 32);
        state.previousTick = tick;
        runtime_log::SetPhase("FrameDriver");
        FrameDriver(app);
        runtime_log::SetPhase("message loop");
        if (state.app != app) return native_ui::UpdateOutcome{native_ui::UpdateResult::Done, request.flags};
        runtime_log::Heartbeat(app->CurrentFrame());
        state.nextTick = now + Period(app);
        if (device_lost_wait::Pending(app) || (state.repaintOnly && app->frameRate.presentedFrames == presentedBefore))
            return native_ui::UpdateOutcome{native_ui::UpdateResult::Retry, 0, state.nextTick};
        return native_ui::UpdateOutcome{native_ui::UpdateResult::Done, request.flags};
    });
    if (state.app == app) autosave::Tick(app, GetTickCount64());
}
DWORD WaitMilliseconds(MMDApp* app) {
    if (Blocked(app)) return 100;
    if (!IsWindowEnabled(app->MainWindow()) && !Exporting(app)) return 100;
    if (!Displayable(app) && !Exporting(app)) return 100;
    auto deadline = state.queue.Deadline();
    uint64_t next = deadline ? std::min(state.nextTick, *deadline) : state.nextTick;
    uint64_t now = Now();
    return DWORD(std::min<uint64_t>(100, next > now ? next - now : 0));
}
}
