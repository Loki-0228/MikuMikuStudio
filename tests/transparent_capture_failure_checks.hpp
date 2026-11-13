// Real menu/window/device paths with failures limited to the capture operation.
enum class StillFailure { None, CreateSurface, CopySurface, TransparentTarget, CreateWindow, BeginScene };
StillFailure stillFailure = StillFailure::None;
bool observeStillExit = false;
unsigned stillFailureCalls = 0, stillStopPlaybackCalls = 0;
using StillCreateTarget = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT,
    D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
using StillCopySurface = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
    IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
StillCreateTarget originalStillTarget = nullptr;
StillCopySurface originalStillCopy = nullptr;
HRESULT (WINAPI* originalStillBeginScene)(IDirect3DDevice9*) = nullptr;
decltype(&CreateWindowExW) originalStillWindow = nullptr;
decltype(&StopPlayback) originalStillStop = nullptr;
HRESULT WINAPI FailStillTarget(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format,
    D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL lockable, IDirect3DSurface9** surface, HANDLE* shared) {
    if ((stillFailure == StillFailure::CreateSurface || stillFailure == StillFailure::TransparentTarget) &&
        format == D3DFMT_A8R8G8B8 && seq::StillActive(g_Block)) {
        ++stillFailureCalls;
        if (surface) *surface = nullptr;
        return E_OUTOFMEMORY;
    }
    return originalStillTarget(device, width, height, format, samples, quality, lockable, surface, shared);
}
HRESULT WINAPI FailStillCopy(IDirect3DDevice9* device, IDirect3DSurface9* source, const RECT* sourceRect,
    IDirect3DSurface9* target, const RECT* targetRect, D3DTEXTUREFILTERTYPE filter) {
    if (stillFailure == StillFailure::CopySurface && seq::StillActive(g_Block)) {
        ++stillFailureCalls;
        return D3DERR_INVALIDCALL;
    }
    return originalStillCopy(device, source, sourceRect, target, targetRect, filter);
}
HWND WINAPI FailStillWindow(DWORD extended, LPCWSTR name, LPCWSTR title, DWORD style, int x, int y,
    int width, int height, HWND owner, HMENU menu, HINSTANCE instance, LPVOID parameter) {
    if (stillFailure == StillFailure::CreateWindow && reinterpret_cast<ULONG_PTR>(name) > 0xffff &&
        std::wcscmp(name, L"RecWindow") == 0 && seq::StillActive(g_Block)) {
        ++stillFailureCalls;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return originalStillWindow(extended, name, title, style, x, y, width, height, owner, menu, instance, parameter);
}
void ObserveStillStop(MMDApp* app) {
    if (observeStillExit) ++stillStopPlaybackCalls;
    originalStillStop(app);
}
HRESULT WINAPI FailStillBeginScene(IDirect3DDevice9* device) {
    if (stillFailure == StillFailure::BeginScene && seq::StillActive(g_Block) &&
        g_Block->state.timelineAdvanceDue) {
        ++stillFailureCalls;
        return D3DERR_INVALIDCALL;
    }
    return originalStillBeginScene(device);
}

void TransparentStillFailures(MMDApp* app) {
    StillSaveDialogProbe pickerProbe;
    auto** table = *reinterpret_cast<void***>(app->Renderer()->device);
    void* hooks[] = {table[28], table[34], reinterpret_cast<void*>(&CreateWindowExW),
        reinterpret_cast<void*>(&StopPlayback), table[41]};
    Check(MH_CreateHook(hooks[0], &FailStillTarget, reinterpret_cast<void**>(&originalStillTarget)) == MH_OK &&
        MH_CreateHook(hooks[1], &FailStillCopy, reinterpret_cast<void**>(&originalStillCopy)) == MH_OK &&
        MH_CreateHook(hooks[2], &FailStillWindow, reinterpret_cast<void**>(&originalStillWindow)) == MH_OK &&
        MH_CreateHook(hooks[3], &ObserveStillStop, reinterpret_cast<void**>(&originalStillStop)) == MH_OK &&
        MH_CreateHook(hooks[4], &FailStillBeginScene, reinterpret_cast<void**>(&originalStillBeginScene)) == MH_OK,
        "install bounded still-export failure observation");
    for (auto* hook : hooks) Check(MH_EnableHook(hook) == MH_OK, "enable still-export failure observation");
#if !MMD_ORIGINAL_PROFILE
    plugins::StorageStore storage;
    plugins::SharedHistory history(storage);
    plugins::NativePoseTestBinding binding(*app, history);
#endif
    const int requestedWidth = app->RenderWidth(), requestedHeight = app->RenderHeight();
#if !MMD_ORIGINAL_PROFILE
    const auto previousSettings = app->antialiasing;
    std::wstring error;
    Check(antialias::Select(app, antialias::Algorithm::MmdOriginal, error), "use native AA for deterministic still failures");
#endif
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        auto* renderer = app->Renderer();
        const int screenWidth = renderer->screenWidth, screenHeight = renderer->screenHeight;
        Check(screenWidth > 0 && screenHeight > 0 && screenWidth < 16382 && screenHeight < 16382,
            "still failure fixture can grow the backbuffer");
        app->RenderWidth() = screenWidth + 2; app->RenderHeight() = screenHeight + 2;
        // Deliberately differ from registered frame-zero values. A video
        // finalizer would resample these and destroy the unregistered edit.
        app->CurrentFrame() = 19; app->PlaybackCursorSeconds() = .625;
        app->CameraPositionX() = 12.345f; app->CameraPositionY() = 6.789f;
        app->CameraDistance() = -32.125f;
        app->LightDirection()[0] = .25f; app->LightDirection()[1] = -.75f; app->LightDirection()[2] = .375f;
        app->LightColor()[0] = .17f; app->LightColor()[1] = .37f; app->LightColor()[2] = .57f;
        const auto dirty = app->SceneModified();
        const auto playback = app->PlaybackActive(), frameStep = app->FrameStepPlayback();
        const auto pending = app->state.recordPlaybackStartPending;
#if !MMD_ORIGINAL_PROFILE
        std::uint64_t revision = 0;
        Check(history.Revision(revision) == MMS_OK, "read history before failed still output");
#endif
        stillPath = output / (L"failed-still-" + std::to_wstring(scenario) + L".png");
        stillPickers = 0; stillFailureCalls = 0; stillStopPlaybackCalls = 0;
        const auto oldWarnings = warnings;
        observeStillExit = true; expectWarning = true;
        stillFailure = scenario == 3 ? StillFailure::CreateWindow : StillFailure::None;
        SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
        Check(stillPickers == 1, "failed still output uses the real save dialog path");
        if (scenario != 3) {
            Check(app->RecordingWindow() && seq::StillActive(app) &&
                renderer->screenWidth == screenWidth + 2 && renderer->screenHeight == screenHeight + 2,
                "still failure begins with a real window and enlarged backbuffer");
            if (scenario == 0) {
                app->state.messageSeen = 1; RenderFrameScene(app);
                stillFailure = StillFailure::CreateSurface;
                TimelineAdvance(app);
            } else if (scenario == 1) {
                // Build the alpha source first; only the final copy is failed.
                app->state.messageSeen = 1; RenderFrameScene(app);
                Check(TransparentCaptureSurface(app) != nullptr, "copy failure has a real alpha source surface");
                stillFailure = StillFailure::CopySurface;
                TimelineAdvance(app);
            } else if (scenario == 2) {
                stillFailure = StillFailure::TransparentTarget;
                app->state.messageSeen = 1; RenderFrameScene(app);
            } else if (scenario == 4) {
                Check(seq::CancelStillCapture(app), "explicit still cancel owns its capture");
            } else if (scenario == 5) {
                // The RecWindow Esc/close path still calls this legacy entry.
                FinishAviRecord(app);
            } else {
                stillFailure = StillFailure::BeginScene;
                for (unsigned pass = 0; app->RecordingWindow() && pass < 12; ++pass) {
                    app->state.messageSeen = 1;
                    coordinator_fixture::Drive(app);
                }
                Check(!fs::exists(stillPath), "a failed due frame cannot publish the previous render target");
            }
        }
        stillFailure = StillFailure::None; observeStillExit = false; expectWarning = false;
        const bool failed = scenario < 4 || scenario == 6;
        Check(stillFailureCalls == (scenario == 1 ? 2u : failed ? 1u : 0u),
            "failure injection reaches only the requested capture boundary");
        Check(warnings == oldWarnings + (failed ? 1u : 0u), "failed output reports once and cancel reports no error");
        Check(!app->RecordingWindow() && !seq::StillActive(app) && !seq::Transparent(app) &&
            !TransparentCaptureSurface(app) && !app->state.timelineAdvanceRequested && !app->state.timelineAdvanceDue,
            "every still exit releases the window, alpha target, session and capture requests");
        Check(renderer->screenWidth == screenWidth && renderer->screenHeight == screenHeight &&
            renderer->presentParameters.BackBufferWidth == unsigned(screenWidth) &&
            renderer->presentParameters.BackBufferHeight == unsigned(screenHeight),
            "failed still output restores the original device dimensions");
        Check(!seq::CancelStillCapture(app) && !fs::exists(stillPath), "still cleanup is idempotent and writes no failed image");
        Check(stillStopPlaybackCalls == 0 && app->PlaybackActive() == playback && app->FrameStepPlayback() == frameStep &&
            app->state.recordPlaybackStartPending == pending, "still failure and cancellation never run the video stop path");
        Check(app->CurrentFrame() == 19 && app->PlaybackCursorSeconds() == .625 &&
            app->CameraPositionX() == 12.345f && app->CameraPositionY() == 6.789f && app->CameraDistance() == -32.125f &&
            app->LightDirection()[0] == .25f && app->LightDirection()[1] == -.75f && app->LightDirection()[2] == .375f &&
            app->LightColor()[0] == .17f && app->LightColor()[1] == .37f && app->LightColor()[2] == .57f &&
            app->SceneModified() == dirty, "still failures preserve the playhead and unregistered camera and light edits");
#if !MMD_ORIGINAL_PROFILE
        std::uint64_t after = 0;
        Check(history.Revision(after) == MMS_OK && after == revision && !history.UndoCount() && !history.RedoCount(),
            "still failures leave the common history revision and records unchanged");
#endif
        std::printf("PASS still-failure scenario=%u errors=%u injection_calls=%u\n", scenario,
            warnings - oldWarnings, stillFailureCalls);
    }
    app->RenderWidth() = requestedWidth; app->RenderHeight() = requestedHeight;
    RunStillRecovery(app, app->EnglishUI(), L"begin-scene");
#if !MMD_ORIGINAL_PROFILE
    Check(antialias::Select(app, previousSettings.algorithm, error), "restore AA after still failure checks");
    if (app->antialiasing.enabled != previousSettings.enabled)
        Check(antialias::Toggle(app, error), "restore AA enable state after still failure checks");
#endif
    for (auto* hook : hooks) { MH_DisableHook(hook); MH_RemoveHook(hook); }
}
