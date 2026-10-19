#pragma once
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"

namespace mikumikustudio::device_lost_wait {

enum class Result { Ready, Reset, Interrupted, Failed };

// Recovery belongs to the window thread. Do not keep a device reference across
// frames: shutdown must be free to release it. These pointers are identities,
// checked against the live app before any device call.
struct State {
    MMDApp* app = nullptr;
    D3DRenderer* renderer = nullptr;
    IDirect3DDevice9* device = nullptr;
    MMDApp* backgroundRefreshApp = nullptr;
    bool resetRequired = false;
    bool active = false;
};
inline thread_local State state;

inline bool Active() noexcept { return state.active; }

inline void ClearPending() noexcept {
    state.app = nullptr; state.renderer = nullptr; state.device = nullptr;
    state.resetRequired = false;
}

// Device recovery can complete without Reset. Keep deferred content changes
// independently, so a still-allocated texture does not hide a pending seek.
inline void DeferBackgroundRefresh(MMDApp* app) noexcept { state.backgroundRefreshApp = app; }
inline bool BackgroundRefreshPending(MMDApp* app) noexcept { return state.backgroundRefreshApp == app; }
inline void BackgroundRefreshed(MMDApp* app) noexcept {
    if (BackgroundRefreshPending(app)) state.backgroundRefreshApp = nullptr;
}
inline void Forget(MMDApp* app) noexcept {
    if (state.app == app) ClearPending();
    BackgroundRefreshed(app);
    // An enclosing wait/reset scope still owns the reentry guard.
}

inline bool SameDevice(MMDApp* app, D3DRenderer* renderer, IDirect3DDevice9* device) {
    return app->Renderer() == renderer && renderer && renderer->device == device;
}

// Phase boundaries use this without attempting recovery in the middle of a
// frame. Resume() owns recovery before the next frame starts using resources.
inline bool Pending(MMDApp* app) noexcept {
    if (Active()) return true;
    if (state.app != app) return false;
    if (!SameDevice(app, state.renderer, state.device)) {
        ClearPending();
        return false;
    }
    return true;
}

// PostDeviceReset also runs from window messages (resize, recording cancel,
// fullscreen changes). Register its resource loss before releasing anything,
// and keep the pending identity on every failure/early return. Nested scopes
// preserve a Wait's active guard, including synchronous window messages from
// the driver. Successful() belongs at the very end of PostDeviceReset.
class ResetScope {
    MMDApp* app_;
    D3DRenderer* renderer_;
    IDirect3DDevice9* device_;
    bool wasActive_;
    bool successful_ = false;

    void Pending() noexcept {
        state.app = app_; state.renderer = renderer_; state.device = device_;
        state.resetRequired = true;
    }
    bool MatchesPending() const noexcept {
        return state.app == app_ && state.renderer == renderer_ && state.device == device_;
    }
public:
    explicit ResetScope(MMDApp* app) noexcept
        : app_(app), renderer_(app ? app->Renderer() : nullptr),
          device_(renderer_ ? renderer_->device : nullptr), wasActive_(Active()) {
        state.active = true;
        Pending();
    }
    ResetScope(const ResetScope&) = delete;
    ResetScope& operator=(const ResetScope&) = delete;
    void Successful() noexcept {
        successful_ = true;
        if (MatchesPending()) ClearPending();
    }
    ~ResetScope() {
        // A nested successful Reset cannot conceal a later outer failure.
        if (!successful_ && app_ && SameDevice(app_, renderer_, device_)) Pending();
        else if (MatchesPending()) ClearPending();
        state.active = wasActive_;
    }
};

inline Result Wait(MMDApp* app, IDirect3DDevice9* device, bool recording) {
    if (Active() || !app || !device) return Result::Interrupted;
    auto* renderer = app->Renderer();
    if (!SameDevice(app, renderer, device)) return Result::Interrupted;
    if (state.app != app || state.renderer != renderer || state.device != device)
        ClearPending();
    state.app = app; state.renderer = renderer; state.device = device;
    struct Scope {
        IDirect3DDevice9* device;
        explicit Scope(IDirect3DDevice9* value) : device(value) { state.active = true; device->AddRef(); }
        ~Scope() { device->Release(); state.active = false; }
    } scope(device);

    const HWND window = static_cast<HWND>(app->Hwnd());
    const HWND recordWindow = app->RecordingWindow();
    const auto* recorder = app->Recorder();
    const auto frameStep = app->state.frameStepPlayback;
    const auto* target = app->CaptureRenderTarget();
    const auto* system = app->CaptureSystemSurface();
    const auto* back = renderer->backbufferSurface;
    const auto* pixels = app->CaptureReadbackPixels();
    const auto controlsCurrent = [&] {
        if (!SameDevice(app, renderer, device)) {
            ClearPending();
            return false;
        }
        if (app->SceneMutationInProgress() || static_cast<HWND>(app->Hwnd()) != window ||
            (window && !IsWindow(window))) return false;
        return !recording ||
            (app->state.frameStepPlayback == frameStep && app->RecordingWindow() == recordWindow &&
             app->Recorder() == recorder);
    };
    const auto current = [&] {
        // A dispatched cancel/reset/resize can release every capture resource.
        // Compare identities only; never dereference a surface after dispatch.
        return controlsCurrent() && (!recording ||
            (app->CaptureRenderTarget() == target && app->CaptureSystemSurface() == system &&
             renderer->backbufferSurface == back && app->CaptureReadbackPixels() == pixels));
    };
    for (;;) {
        if (!current()) return Result::Interrupted;
        // Bound one batch so a producer cannot starve the cooperative-level
        // check. Both windowed and fullscreen paths must dispatch messages.
        for (unsigned count = 0; count != 64; ++count) {
            MSG message{};
            if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) break;
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                return Result::Interrupted;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (!current()) return Result::Interrupted;
        }
        const HRESULT status = device->TestCooperativeLevel();
        if (!current()) return Result::Interrupted;
        if (status == D3D_OK && !state.resetRequired) {
            ClearPending();
            return Result::Ready;
        }
        if (status == D3DERR_DEVICENOTRESET || (status == D3D_OK && state.resetRequired)) {
            // PostDeviceReset releases resources and calls OnLostDevice before
            // Reset. Even S_OK cannot restore those resources after a failure.
            state.resetRequired = true;
            const bool reset = PostDeviceReset(app);
            if (!SameDevice(app, renderer, device)) {
                ClearPending();
                return Result::Interrupted;
            }
            if (!reset) return Result::Failed;
            ClearPending();
            return controlsCurrent() ? Result::Reset : Result::Interrupted;
        }
        // Cancellation and other failures retain the same-device pending
        // identity. A later frame must recover before it starts rendering.
        if (status != D3DERR_DEVICELOST) return Result::Failed;
        // Sleep only while lost; incoming input wakes the wait immediately.
        MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

// A failed Reset must not fall through to rendering on the following frame.
// On the normal path this is just a thread-local pointer check.
inline bool Resume(MMDApp* app, bool recording) {
    if (Active()) return false;
    if (state.app != app) return true;
    if (!SameDevice(app, state.renderer, state.device)) {
        ClearPending();
        return true;
    }
    const auto result = Wait(app, state.device, recording);
    return result == Result::Ready || result == Result::Reset;
}

} // namespace mikumikustudio::device_lost_wait
