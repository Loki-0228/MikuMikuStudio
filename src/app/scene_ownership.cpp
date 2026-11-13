#include "mikumikustudio/key_pool.hpp"
// ===========================================================================
// MikuMikuStudio - scene-resident allocation teardown
// ===========================================================================
#include <cstdio>
#include <cstdlib>
#include <new>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/recording_state.hpp"

#include "mikumikustudio/effect_api.hpp"

namespace mikumikustudio {

namespace {

// Porting-era trace under MIKUMIKUSTUDIO_PMM_TRACE_DIR (CMake option
// MIKUMIKUSTUDIO_DIAG, default OFF); the OFF stub keeps the call sites
// valid and inlines away to nothing.
#ifdef MIKUMIKUSTUDIO_DIAG
void TraceAccessoryRelease(int slot, const char* phase, const void* track) {
    const char* directory = std::getenv("MIKUMIKUSTUDIO_PMM_TRACE_DIR");
    if (directory == nullptr || directory[0] == '\0')
        return;
    // The directory path comes directly from the environment and has no
    // length limit of its own, so a value exceeding the 245 characters that
    // fit the literal and terminator made the array overload of sprintf_s
    // invoke the CRT invalid-parameter handler, which by default terminates
    // the process.  snprintf instead truncates and always terminates, matching
    // the DIAG dump path in frame_state_dump.hpp:40-41.  DIAG-only helper
    // (MIKUMIKUSTUDIO_DIAG, default OFF).
    // Defect AB-09.
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\pmm_model_load.log", directory);
    FILE* stream = nullptr;
    if (fopen_s(&stream, path, "ab") != 0 || stream == nullptr)
        return;
    fprintf(stream, "stage=accessory-track-%s slot=%d track=%p\r\n", phase,
            slot, track);
    fclose(stream);
}
#else
inline void TraceAccessoryRelease(int, const char*, const void*) {}
#endif

}  // namespace

#if !MMD_ORIGINAL_PROFILE
bool SceneCloseBlocked(MMDApp& app) noexcept {
    return app.SceneMutationInProgress() || app.sceneMutationFinishing || app.sceneCloseHold ||
        app.sceneMainCloseProcessing || app.sceneFloatingCloseProcessing ||
        plugins::CloseBlocked(&app) || SceneSaveBlocksEdits() || bake::UiLocked() ||
        recording_state::Stopping(&app) ||
        recording_state::Guard::Active(&app, recording_state::Operation::Start) ||
        recording_state::Guard::Active(app.Recorder(), recording_state::Operation::GraphTeardown);
}
void RetryDeferredSceneCloses(MMDApp& app) noexcept {
    if (SceneCloseBlocked(app)) return;
    const HWND main = app.deferredSceneMainClose;
    const HWND floating = app.deferredSceneFloatingClose;
    if (floating && (floating != app.FloatingWindow() || !IsWindow(floating))) {
        app.deferredSceneFloatingClose = nullptr; app.sceneFloatingClosePosted = false;
    } else if (floating && !app.sceneFloatingClosePosted) {
        app.sceneFloatingClosePosted = true;
        if (!PostMessageW(floating, kSceneCloseReplayMessage, 0, 0)) app.sceneFloatingClosePosted = false;
    }
    if (main && (main != app.MainWindow() || !IsWindow(main))) {
        app.deferredSceneMainClose = nullptr; app.sceneMainClosePosted = false;
    } else if (main && !app.sceneMainClosePosted) {
        app.sceneMainClosePosted = true;
        if (!PostMessageW(main, kSceneCloseReplayMessage, 0, 0)) app.sceneMainClosePosted = false;
    }
}
ScopedSceneCloseDeferral::ScopedSceneCloseDeferral(MMDApp& app) noexcept : app_(app) { ++app_.sceneCloseHold; }
ScopedSceneCloseDeferral::~ScopedSceneCloseDeferral() {
    --app_.sceneCloseHold;
    RetryDeferredSceneCloses(app_);
}
#endif

ScopedSceneMutation::ScopedSceneMutation(MMDApp& app)
    : app_(app), previousLayoutReady_(app.WindowLayoutReady()) {
#if !MMD_ORIGINAL_PROFILE
    // Establish the gate before releasing device objects, which may dispatch.
    ++app_.m_sceneMutationDepth;
    app_.WindowLayoutReady() = 0;
#endif
    InvalidateEffectCurrentObjectCache();
    ShutdownD3D12Native();
#if MMD_ORIGINAL_PROFILE
    ++app_.m_sceneMutationDepth;
    app_.WindowLayoutReady() = 0;
#endif
}

ScopedSceneMutation::~ScopedSceneMutation() {
    app_.WindowLayoutReady() = previousLayoutReady_;
#if !MMD_ORIGINAL_PROFILE
    if (app_.m_sceneMutationDepth == 1) {
        ++app_.sceneMutationFinishing;
        --app_.m_sceneMutationDepth;
        app_.state.messageSeen = 1;
        if (app_.MainWindow() != nullptr)
            RedrawWindow(app_.MainWindow(), nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        // Redraw may dispatch WM_CLOSE. Keep a close gate while painting but
        // preserve the existing first-paint contract of a cleared depth.
        --app_.sceneMutationFinishing;
        if (app_.sceneMutationFinishing) return;
        RetryDeferredSceneCloses(app_);
    } else {
        --app_.m_sceneMutationDepth;
    }
#else
    if (--app_.m_sceneMutationDepth == 0) {
        app_.state.messageSeen = 1;
        if (app_.MainWindow() != nullptr)
            // Loading may paint the parent panel while child controls are
            // already validated. Repaint both after the outer mutation ends.
            RedrawWindow(app_.MainWindow(), nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
#endif
}

void ReleaseSceneModels(MMDApp& app) {
    // x64 teardown sub_7FF7CB42BD40+0x42C130: 255-count do/while over the
    // model slots (app+0xBE8), each entry through sub_7FF7CB4C8D50
    // (ModelDispose twin) then operator delete - kModelSlotCount wide.
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        unsigned char*& model = app.ModelSlot(slot);
        if (model == nullptr)
            continue;
        ModelDispose(model);
        std::free(model);
        model = nullptr;
    }
}

void ReleaseGlobalTimelineTracks(MMDApp& app) {
    // The four tracks are allocated with ::operator new followed by an immediate
    // memset (ui_init.cpp:60-63 cites the VC9 j_??2@YAPAXI@Z thunk of the
    // original; reset_app_state.cpp:317-336 repeats the same pairing), but
    // this teardown freed them via the CRT free. Calling free() on storage
    // obtained from ::operator new is undefined behaviour, even though the
    // MSVC CRT heap makes it work; the release now matches the allocation.
    // The x64 reference likewise frees these four adjacent slots with a delete
    // form (operator delete[] at 0x14000C03F..0x14000C08A). Each slot is
    // nulled immediately after, so ShutdownCleanup cannot free the same block
    // twice.
    // Defect AB-06.
    if (app.CameraKeys() != nullptr) {
        mdl::KeyPoolDestroy(app.CameraKeys());
        app.CameraKeys() = nullptr;
    }
    if (app.LightKeys() != nullptr) {
        mdl::KeyPoolDestroy(app.LightKeys());
        app.LightKeys() = nullptr;
    }
    if (app.ShadowKeys() != nullptr) {
        mdl::KeyPoolDestroy(app.ShadowKeys());
        app.ShadowKeys() = nullptr;
    }
    if (app.GravityKeys() != nullptr) {
        mdl::KeyPoolDestroy(app.GravityKeys());
        app.GravityKeys() = nullptr;
    }
}

void ReleaseAccessoriesAndTracks(MMDApp& app) {
    for (int slot = 0; slot < 255; ++slot) {
        mdl::AccessoryRecord*& accessory = app.AccessorySlot(slot);
        if (accessory != nullptr) {
            // The record block is allocated with ::operator new(sizeof(
            // mdl::AccessoryRecord), std::nothrow) (accessory.cpp:870, also
            // window_init.cpp:127 and both PMM loaders) and DeleteAccessory
            // releases it with ::operator delete (accessory.cpp:837-842),
            // while this scene teardown used the CRT free.  Calling free() on
            // storage obtained from ::operator new is undefined behaviour,
            // even though the MSVC CRT heap makes it work.  The x64 reference
            // releases the same block with a scalar operator delete after the
            // record's own buffers (0x14000C109).  The slot is nulled right
            // after, so ShutdownCleanup cannot release the same block twice.
            // Defect AB-08.
            DisposeAccessory(accessory);
            ::operator delete(accessory);
            accessory = nullptr;
        }
        // The 255 accessory track arrays are allocated by the same ::operator new
        // wrapper as the four global tracks (ui_init.cpp:60-63 and
        // reset_app_state.cpp:337-343); the x64 reference frees each of
        // them with a delete form (operator delete[] at 0x14000C11E inside
        // the 255-slot loop).
        // Defect AB-06.
        mdl::AccessoryKey*& track = app.AccessoryKeys(slot);
        if (track != nullptr) {
            TraceAccessoryRelease(slot, "before-free", track);
            mdl::KeyPoolDestroy(track);
            TraceAccessoryRelease(slot, "after-free", nullptr);
            track = nullptr;
        }
    }
}

}  // namespace mikumikustudio
