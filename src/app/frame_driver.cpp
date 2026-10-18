#include "mikudancestudio/perf_timing.hpp"
// ===========================================================================
// VA 0x0046B090 - FrameDriver  (original: sub_46B090, 0xED01 = 60KB)
// ===========================================================================
// The verified frame flow skeleton with the interaction-mode dispatch
// table; the per-mode manipulation bodies (modes 1..0x12) live in
// frame_modes_bone.cpp and are field-exact against the decompilation.
//
// Flow (verified):
//   1. mouse-delta snapshot reset
//   2. interaction mode state machine, this+836:
//        1..3   rotation X/Y/Z (object slot+0x220 / camera; UI edit 481..)
//        4..6   translation X/Z/Y
//        7      scale
//        8..9   bone rotation (matrix path via 0x507AA0/0x507970)
//        10..12 physics body pos/rot X/Y/Z (ctrl 0x2C5+ / 0x2E8+)
//        0xD..F camera adjustment
//        0x10..0x12 angle adjustment
//        0x13..0x16 bone scale uniform/X/Y/Z (HUD scale row)
//   3. mouse position update (this+0xA03E8)
//   4. timeline: 0x9EDD0 -> fcn_460130 + clear 0xA03B7;  0xA03B7 sets 0x9EDD0
//   5. reload path: 0xA04B8 -> clear 0xA0478/0x308/0x30C, fcn_42E640,
//      fcn_41A650, clear 0xA04B8
//   6. D3D cooperative-level / device reset dance (device =
//      *(this+0xA06C4)+120032), reset params from 0xA0D24/0xA0D38/
//      renderW/H; message-aware D3DERR_DEVICELOST wait in both window modes,
//      followed by fcn_440DB0 only when the device needs a reset
//   7. animation frame section (0x479CC6): byte 0x9EDD8 -> copy 0x9EDDC ->
//      this+0x980, 0x9ED90=1, unsigned frame ints 0xA0B00/0xA0B04 (fild
//      +2^32 wrap, const 0x52B9F0) / 30.0 (dbl_52BA68) -> 0x9E654/0x9E658,
//      int copy 0x9E648, cursor 0x9E64C, 0x9EDB6 equality flag, 0x330=1,
//      fcn_433A40, 0x9ED94=0, EnableWindow(GetDlgItem(0xA06B8, 0x198),
//      FALSE), 0x9EDD8=0
//   7b. selection-active local var_14A1 (0x46DCA6..0x46DCCD), threaded
//      into the playback catch-up below
//   8. playback catch-up (0x46EEE0..0x46F575, playback_catchup.cpp) and
//      the physics simulation blocks (0x46F7FF..0x46FEB8, physics_frame.cpp)
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include "device_lost_wait.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/d3dx_dyn.hpp"
#include "mikudancestudio/image_sequence_export.hpp"
#include "mikudancestudio/transparent_capture.hpp"
#include "mikudancestudio/frame_presentation.hpp"
#include "mikudancestudio/frame_rate.hpp"
#include "mikudancestudio/frame_sampling.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/globals.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/native_media.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/runtime_log.hpp"
#include "frame_state_dump.hpp"

// key_ladder.cpp (registered in CMakeLists next to frame_modes_bone.cpp):
// main-pump letter hotkey consumption ladder (x86 0x46FF35..0x4739E2).
// pump_navigation.cpp / pump_edit_keys.cpp: the non-letter pump segments
// that surround the ladder in the original message pump.
namespace mikudancestudio {

void ConsumeLetterHotkeys(MMDApp* app);
void ConsumeRightButtonDrag(MMDApp* app);   // pump_navigation.cpp (G10)
void ConsumeMiddleButtonPan(MMDApp* app);   // pump_navigation.cpp (G11)
void ConsumeEditKeys(MMDApp* app);          // pump_edit_keys.cpp

namespace {

void AdvanceFrameRenderGate(MMDApp* app) {
    // 0x46B0B1..0x46B11D: transient render/present state machine.
    // The three gates independently restart the cycle before its 1->2->3->0
    // transition; the normal main-window path therefore enters rendering at 2.
    auto& state = app->state.messageSeen;
    if (app->PlaybackActive() != 0)
        state = 1;
    if (app->state.frameVolumeControlEnabled == 0)
        state = 1;
    if (app->FrameRangeDialog() != nullptr)
        state = 1;

    if (state == 1)
        state = 2;
    else if (state == 2)
        state = 3;
    else if (state == 3)
        state = 0;
}

void PrepareUninitializedImageModels(MMDApp* app) {
    if (!image_sequence::Active(app)) return;
    bool uninitialized[kModelSlotCount]{};
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        auto* model = app->ModelSlot(slot);
        if (!model || !mdl::Mdl(model)->boneCount || !mdl::Bones(model)) continue;
        // Freshly loaded models can reach frame-zero capture before the first
        // physics/pose pass. An affine bone matrix can never be entirely zero.
        bool zero = true;
        for (float value : mdl::Bones(model)[0].matInit) zero = zero && value == 0;
        uninitialized[slot] = zero;
    }
    // Prepare the authored pose in native dependency order. Do not run the
    // physics world or advance the output clock; the usual frame tail owns it.
    for (int phase : {0, 1})
        for (int order = 0; order < kModelSlotCount; ++order)
            for (int slot = 0; slot < kModelSlotCount; ++slot) {
                auto* model = app->ModelSlot(slot);
                if (uninitialized[slot] && mdl::Mdl(model)->comboSelIndex2 == order) {
                    ModelApplyMorphs(model);
                    SetPhysicsMode(model, phase, app->ModelSlots(), 0);
                }
            }
}

#ifdef MIKUDANCESTUDIO_DIAG
// ---- A/B capture tooling (see frame_state_dump.hpp for the gate) ---------

void TraceOperationInput(MMDApp* app, const char* phase) {
    char directory[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA(
        "MIKUDANCESTUDIO_AB_TRACE_INPUT", directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;
    const int state = app->LeftMouseButtonState();
    const auto mode = app->InteractionDragMode();
    if (state == 0 && mode == ViewportDragMode::None)
        return;
    char path[MAX_PATH]{};
    std::snprintf(path, sizeof(path), "%s\\input_trace.jsonl", directory);
    std::FILE* stream = nullptr;
    if (fopen_s(&stream, path, "ab") != 0 || stream == nullptr)
        return;
    std::uint32_t quaternion[4]{};
    const unsigned slot = app->state.slotIdx;
    auto* model = app->ModelSlot(slot);
    if (model != nullptr) {
        const int selected = mikudancestudio::mdl::Mdl(model)->selectedBone;
        auto* bones = mikudancestudio::mdl::Bones(model);
        const int count = static_cast<int>(mikudancestudio::mdl::Mdl(model)->boneCount);
        if (bones != nullptr && selected >= 0 && selected < count)
            std::memcpy(quaternion, &bones[selected].rotQuat[0],
                        sizeof(quaternion));
    }
    std::fprintf(stream,
        "{\"phase\":\"%s\",\"state\":%d,\"mode\":%u,"
        "\"mouse\":[%d,%d,%d,%d],\"quaternion\":["
        "\"%08X\",\"%08X\",\"%08X\",\"%08X\"]}\n",
        phase, state, mode,
        app->MouseX(), app->MouseY(),
        app->PreviousMouseX(), app->PreviousMouseY(),
        quaternion[0], quaternion[1], quaternion[2], quaternion[3]);
    std::fclose(stream);
}

bool LineCaptureSwitchReady() {
    char stableCapture[2]{};
    if (GetEnvironmentVariableA("MIKUDANCESTUDIO_AB_STABLE_CAPTURE", stableCapture,
                                sizeof(stableCapture)) != 1 ||
        stableCapture[0] != '1') {
        return true;
    }

    char directory[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA(
        "MIKUDANCESTUDIO_VB_DUMP_DIR", directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;

    char readyPath[MAX_PATH]{};
    std::snprintf(readyPath, sizeof(readyPath),
                  "%s\\vb.capture.ready", directory);
    const DWORD attributes = GetFileAttributesA(readyPath);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsRegularFile(const char* path) {
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool WriteCaptureMarker(const char* path, const char* contents) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(std::strlen(contents));
    const bool ok = WriteFile(file, contents, size, &written, nullptr) != FALSE &&
                    written == size && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return ok;
}

void ActivateRefreshedVbCapture() {
    char directory[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA(
        "MIKUDANCESTUDIO_VB_DUMP_DIR", directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;

    char requestPath[MAX_PATH]{};
    char activePath[MAX_PATH]{};
    std::snprintf(requestPath, sizeof(requestPath),
                  "%s\\vb.capture.ready", directory);
    std::snprintf(activePath, sizeof(activePath),
                  "%s\\vb.capture.active", directory);
    if (IsRegularFile(requestPath) && !IsRegularFile(activePath))
        WriteCaptureMarker(activePath, "active after successful Present\n");
}

void HoldPresentCaptureFence() {
    char stableCapture[2]{};
    if (GetEnvironmentVariableA("MIKUDANCESTUDIO_AB_STABLE_CAPTURE", stableCapture,
                                sizeof(stableCapture)) != 1 ||
        stableCapture[0] != '1') {
        return;
    }

    char directory[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA(
        "MIKUDANCESTUDIO_VB_DUMP_DIR", directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;

    char requestPath[MAX_PATH]{};
    char readyPath[MAX_PATH]{};
    char releasePath[MAX_PATH]{};
    std::snprintf(requestPath, sizeof(requestPath),
                  "%s\\screen.present.request", directory);
    if (!IsRegularFile(requestPath))
        return;
    std::snprintf(readyPath, sizeof(readyPath),
                  "%s\\screen.present.ready", directory);
    std::snprintf(releasePath, sizeof(releasePath),
                  "%s\\screen.present.release", directory);
    if (!WriteCaptureMarker(readyPath, "presented\n"))
        return;

    // Test-only handshake: keep the UI thread immediately after a successful
    // Present until the external capturer has copied that exact desktop frame.
    const ULONGLONG deadline = GetTickCount64() + 30000;
    while (!IsRegularFile(releasePath) && GetTickCount64() < deadline)
        Sleep(1);
}

#endif  // MIKUDANCESTUDIO_DIAG

}  // namespace

// ---------------------------------------------------------------------------
// VA 0x00460130 - TimelineAdvance (misleading ledger name: this is the
// frame-capture / screenshot tail invoked from the timeline flag handoff at
// 0x479864 when byte 0x9EDD0 is set).  Render wrapper = app+0xA06C4, device
// at wrapper+0x1D4E0, back buffer at wrapper+0x1D538, back-buffer format at
// wrapper+0x1D540; capture size app+0xA08D4/A08D8; wait window app+0xA0D24;
// save path buffer app+0x9F134 (wchar); window size app+0xA02AC/A02B0.
//
// Flow (vtable slots empirically anchored by the A/B traces: 0x48 =
// GetBackBuffer, 0x70 = CreateRenderTarget, 0x88 = StretchRect, 0x94 =
// SetRenderTarget):
//   1. CreateRenderTarget(w, h, backBufferFormat, 0, 0, 0, &surf, 0);
//      FAILED -> destroy wait window, viewport refresh, return.
//   2. back buffer lazily re-fetched (GetBackBuffer(0, 0, MONO, &bb)).
//   3. StretchRect(bb, rect{0,0,w,h}, surf, same rect, D3DTEXF_LINEAR);
//      if that FAILED, retried with filter NONE.  If both succeeded ->
//      D3DXSaveSurfaceToFileW(path, extFormat(path), surf, 0, &rect) (the
//      extension ladder ".bmp"0/".jpg"1/".png"3/".dds"4/".dib"6/".pfm"8/
//      ".hdr"7; an unrecognized extension passes the surface pointer as the
//      format - a latent original quirk, D3DX rejects it harmlessly).
//   4. save tail: Release(surf); re-fetch back buffer if null;
//      SetRenderTarget(0, bb) when wrapper byte 0x1D4F8 == 0; destroy the
//      wait window; when the window size (0xA02AC/A02B0) exceeds the device
//      size (wrapper 0x1D4E4/0x1D4E8), copy it into 0x1D4E4/0x1D4E8 and
//      0x1D4FC/0x1D500 and run PostDeviceReset (0x440DB0).
//   5. always RefreshMainWindowViewport (viewport refresh) at the end (save path); the
//      blit-failed and create-failed paths only destroy + refresh.
// ---------------------------------------------------------------------------
void TimelineAdvance(MMDApp* app) {
    struct FinishStill {
        MMDApp* app;
        ~FinishStill() { image_sequence::EndStill(app); }
    } finish{app};
    D3DRenderer* r = app->Renderer();
    if (r == nullptr)
        return;  // wrapper absent: nothing the original could act on either
    IDirect3DDevice9* device = r->device;  // +0x1D4E0
    if (device == nullptr)
        return;

    IDirect3DSurface9* surf = nullptr;                            // 0x46016A
    if (FAILED(device->CreateRenderTarget(
            app->RenderWidth(),
            app->RenderHeight(),
            TransparentExportActive(app)?D3DFMT_A8R8G8B8:r->backbufferFormat,
            D3DMULTISAMPLE_NONE, 0, FALSE, &surf, nullptr))) {
        DestroyWindow(app->RecordingWindow());                    // 0x460177
        app->RecordingWindow() = nullptr;
        RefreshMainWindowViewport(app);                                           // 0x460185
        return;
    }

    IDirect3DSurface9** backBuffer = &r->backbufferSurface;  // +0x1D538
    if (*backBuffer == nullptr)                                   // 0x460197
        device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backBuffer);

    RECT rect;                                                    // 0x4601B7
    rect.left = 0;
    rect.top = 0;
    rect.right = app->RenderWidth();
    rect.bottom = app->RenderHeight();

    auto* source=TransparentExportActive(app)?TransparentCaptureSurface(app):*backBuffer;
    HRESULT hr=source?device->StretchRect(source,&rect,surf,&rect,
        TransparentExportActive(app)?D3DTEXF_NONE:D3DTEXF_LINEAR):E_FAIL;
    if (FAILED(hr))
        hr = source?device->StretchRect(source, &rect, surf, &rect,
                                 D3DTEXF_NONE):E_FAIL;             // 0x46022E
    if (FAILED(hr)) {
        // both blits rejected: no file is written, just clean up.
        DestroyWindow(app->RecordingWindow());                    // 0x46023B
        app->RecordingWindow() = nullptr;
        RefreshMainWindowViewport(app);                                           // 0x460249
        if (surf != nullptr)
            surf->Release();                                      // 0x460260
        return;
    }

    // ---- save path (0x460269): extension ladder -------------------------
    const wchar_t* path = app->CaptureSavePath();
    const HRESULT saved=image_sequence::SaveSurface(path,image_sequence::FormatFromPath(path),
        surf,TransparentExportActive(app));
    if(FAILED(saved)) {
        MessageBoxW(app->MainWindow(),image_sequence::Text(app,L"图片导出失败，请检查图片格式、保存位置和剩余空间。",
            L"Image export failed. Check the format, destination and free space.",
            L"画像を出力できません。形式、保存先、空き容量を確認してください。"),L"MMS",MB_OK|MB_ICONERROR);
    }
    if (surf != nullptr) {
        surf->Release();                                          // 0x460340
        surf = nullptr;
    }

    if (*backBuffer == nullptr)                                   // 0x46034C
        device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backBuffer);
    if (r->multisampleAvailable == 0)  // wrapper+0x1D4F8
        device->SetRenderTarget(0, *backBuffer);                  // 0x460391
    DestroyWindow(app->RecordingWindow());                        // 0x46039A
    app->RecordingWindow() = nullptr;

    // window-vs-device size check (0x4603B2..0x460410): when either device
    // dimension is larger than the window's, adopt the window size and reset.
    const std::int32_t winW = app->state.recRTW;
    const std::int32_t winH = app->state.recRTH;
    const bool tooWide = r->screenHeight > winH;   // wrapper+0x1D4E8
    const bool tooHigh = r->screenWidth > winW;    // wrapper+0x1D4E4
    if (tooWide || tooHigh) {
        r->presentParameters.BackBufferWidth = winW;   // 0x1D4FC
        r->presentParameters.BackBufferHeight = winH;  // 0x1D500
        r->screenWidth = winW;   // 0x1D4E4
        r->screenHeight = winH;  // 0x1D4E8
        if (!PostDeviceReset(app)) return;                        // 0x460410
    }
    RefreshMainWindowViewport(app);                                               // 0x460417
}

namespace {

// Field trace for the coordinate selector. It writes one line per change to the
// inputs read by the selector and its consumers, and creates nothing unless
// MIKUDANCESTUDIO_TRACE_FILE names a file (the runtime_log::Trace contract).
// The reported flicker and unresponsive switching outlived three code-level
// explanations, so the next report needs data: this line reveals which of the
// state byte, the panel byte, the state limit, the accessory slot, or the HUD
// scale is the value that keeps changing.
void TraceCoordinateSelector(MMDApp* app) {
    static int lastState = -1;
    static int lastPanel = -1;
    static int lastLimit = -1;
    static int lastSlot = -1;
    static int lastAccessory = -1;
    static int lastScale = -1;

    const int slot = app->SelectedAccessorySlot();
    const int accessory =
        slot < kModelSlotCount && app->AccessorySlot(slot) != nullptr ? 1 : 0;
    const int limit = CoordinateStateLimit(app);
    const D3DRenderer* renderer = app->Renderer();
    const int scale = renderer != nullptr
        ? static_cast<int>(renderer->viewScale * 100.0f) : -1;
    const int state = app->state.coordinateSystem;
    const int panel = app->state.optflag[0];
    if (state == lastState && panel == lastPanel && limit == lastLimit &&
        slot == lastSlot && accessory == lastAccessory && scale == lastScale)
        return;
    lastState = state;
    lastPanel = panel;
    lastLimit = limit;
    lastSlot = slot;
    lastAccessory = accessory;
    lastScale = scale;
    runtime_log::Trace(
        "coord state=%d panel=%d limit=%d slot=%d accessory=%d scale=%d",
        state, panel, limit, slot, accessory, scale);
}

}  // namespace

// Field trace for the viewpoint. Every value that decides the rendered image is
// recorded whenever it changes: the panel byte, the follow switch, the resolved
// pivot and its source, the eye, cameraPos and the camera angles. The panel
// switch, the follow switch, the camera-key evaluation and the stop path all
// rewrite these, so a recorded jump points straight at the writer. Creates
// nothing unless MIKUDANCESTUDIO_TRACE_FILE names a file (runtime_log::Trace).
void TraceFrameViewpoint(MMDApp* app) {
    static int lastPanel = -1;
    static int lastTrack = -1;
    static int lastSource = -1;
    static int lastReference = -1;
    static int lastParent = -2;
    static int lastParentBone = -2;
    static float lastFocus[3] = {-1.0f, -1.0f, -1.0f};
    static float lastEye[3] = {-1.0f, -1.0f, -1.0f};
    static float lastPosition[3] = {-1.0f, -1.0f, -1.0f};
    static float lastAngles[3] = {-1.0f, -1.0f, -1.0f};

    float focus[3] = {0.0f, 0.0f, 0.0f};
    const int source = static_cast<int>(ResolveFrameFocus(
        app, static_cast<int>(app->CameraReferenceMode()), focus));
    const float eye[3] = {app->ViewOffsetX(), app->ViewOffsetY(),
                          app->CameraDistance()};
    const float position[3] = {app->CameraPositionX(), app->CameraPositionY(),
                               app->CameraPositionZ()};
    const float angles[3] = {app->CameraYaw(), app->CameraPitch(),
                             app->CameraRoll()};
    const int panel = app->state.optflag[0];
    const int track = app->state.followCameraEnabled;
    const int reference = static_cast<int>(app->CameraReferenceMode());
    const int parent = app->CameraParentModel();
    const int parentBone = app->CameraParentBone();
    if (panel == lastPanel && track == lastTrack && source == lastSource &&
        reference == lastReference && parent == lastParent &&
        parentBone == lastParentBone &&
        std::memcmp(focus, lastFocus, sizeof(focus)) == 0 &&
        std::memcmp(eye, lastEye, sizeof(eye)) == 0 &&
        std::memcmp(position, lastPosition, sizeof(position)) == 0 &&
        std::memcmp(angles, lastAngles, sizeof(angles)) == 0)
        return;
    lastPanel = panel;
    lastTrack = track;
    lastSource = source;
    lastReference = reference;
    lastParent = parent;
    lastParentBone = parentBone;
    std::memcpy(lastFocus, focus, sizeof(focus));
    std::memcpy(lastEye, eye, sizeof(eye));
    std::memcpy(lastPosition, position, sizeof(position));
    std::memcpy(lastAngles, angles, sizeof(angles));
    runtime_log::Trace(
        "view panel=%d track=%d src=%d ref=%d parent=%d/%d "
        "focus=%.3f,%.3f,%.3f eye=%.3f,%.3f,%.3f cpos=%.3f,%.3f,%.3f "
        "angles=%.3f,%.3f,%.3f",
        panel, track, source, reference, parent, parentBone, focus[0], focus[1],
        focus[2], eye[0], eye[1], eye[2], position[0], position[1],
        position[2], angles[0], angles[1], angles[2]);
}

void FrameDriver(MMDApp* app) {
    if (device_lost_wait::Active() ||
        !device_lost_wait::Resume(app, app->state.frameStepPlayback != 0)) return;
    MMD_PERF_SCOPE(Frame);
    // Opt-in heap tripwire for the crash investigation.  The field crashes are
    // intermittent (0%..50% between batches) and only surface later, wherever a
    // corrupted block is next used, so the crash itself is a poor bisect
    // signal.  With MIKUDANCESTUDIO_HEAP_CHECK set, every frame validates the
    // process heap and reports the FIRST frame on which it is already corrupt -
    // a signal that is independent of whether a crash follows.
    static const bool heapCheck = [] {
        char value[8]{};
        return GetEnvironmentVariableA("MIKUDANCESTUDIO_HEAP_CHECK", value,
                                       sizeof(value)) > 0 &&
               value[0] != '\0' && value[0] != '0';
    }();
    if (heapCheck) {
        static bool reported = false;
        if (!reported && !HeapValidate(GetProcessHeap(), 0, nullptr)) {
            reported = true;
            runtime_log::Write("HEAP invalid at FrameDriver entry frame=%d",
                               static_cast<int>(app->CurrentFrame()));
            runtime_log::Trace("HEAP invalid at FrameDriver entry frame=%d",
                               static_cast<int>(app->CurrentFrame()));
        }
    }
    if (app->SceneMutationInProgress())
        return;
#ifdef MIKUDANCESTUDIO_DIAG
    // DIAGNOSTIC ONLY: append a tick per pass to prove liveness of the
    // frame driver in real-time runs (stderr is unavailable in the GUI
    // subsystem).
    static char tickPath[MAX_PATH]{};
    static bool tickInit = GetEnvironmentVariableA(
        "MIKUDANCESTUDIO_TRACE_TICKS", tickPath, sizeof tickPath) != 0;
    if (tickInit) {
        static LONG tickCount = 0;
        if (InterlockedIncrement(&tickCount) % 60 == 0) {
            if (FILE* f = fopen(tickPath, "a"))
                std::fprintf(f, "tick %ld\n", tickCount), std::fclose(f);
        }
    }
#endif
    auto& s = *app;
    // Initialize recording before the first render/readback, not at the tail.
    // Byte compare at 0x479CB9; 0xA0B00/0xA0B04 are INTEGER frame fields
    // (fild + 2^32 fixup on negative, i.e. unsigned load) divided by the
    // constant 30.0 (dbl_52BA68); EnableWindow pushes 0 (0x479D58).
    if (s.state.recordPlaybackStartPending != 0) {       // 0x9EDD8
        antialias::ResetHistory(app);
        s.CurrentFrame() =
            s.AviRecordStartFrame();
        s.state.frameStepPlayback = 1;          // 0x9ED90
        // (was a raw 650128 literal = 0x9EB90, one of the +/-0x100
        // decimal slips; 0x9ED90 = 650640)

        const std::int32_t fa = s.state.aviRecordStartFrame;
        s.PlaybackStartSeconds() = static_cast<double>(fa) / g_FrameScale;

        const std::int32_t fb = s.state.aviRecordEndFrame;
        s.PlaybackEndSeconds() = static_cast<double>(fb) / g_FrameScale;

        s.state.aviBackgroundSample = s.state.aviRecordStartFrame;
        s.PlaybackCursorSeconds() = s.PlaybackStartSeconds();

        if (s.state.aviRecordStartFrame ==
            s.CurrentFrame())
            s.state.playbackFrameChanged = 1;

        s.state.playbackActive = 1;
        UpdateBoneFrames(app);                                    // 0x433A40
        // Initialize the first output sample even when the editing playhead
        // already equals the export start (including frame zero).
        PlaybackPoseAdvance(app, 0);
        PrepareUninitializedImageModels(app);
        s.state.timelineAdvanceDue = 0;
        s.state.timelineAdvanceRequested = 0;
        s.state.recordedFrameCount = 0;
        EnableWindow(GetDlgItem(static_cast<HWND>(s.Hwnd()), panel::kPlayButton),
                     FALSE);
        s.state.recordPlaybackStartPending = 0;
    }

    // Export time follows the output FPS; disk/encoder latency must not become
    // the elapsed time supplied to effects and physics plugins.
    struct RestoreDelta {
        MMDApp& app;
        float value;
        ~RestoreDelta() { app.DeltaTime() = value; }
    } restoreDelta{s, s.DeltaTime()};
    if (s.FrameStepPlayback() && s.PlaybackActive() && s.AviRecordFps() > 0)
        s.DeltaTime() = 1.0f / s.AviRecordFps();


    AdvanceFrameRenderGate(app);
#ifdef MIKUDANCESTUDIO_DIAG
    char freezePhysics[2]{};
    if (GetEnvironmentVariableA("MIKUDANCESTUDIO_AB_FREEZE_PHYSICS", freezePhysics,
                                sizeof(freezePhysics)) == 1 &&
        freezePhysics[0] == '1') {
        app->state.accessoryEditDialogOpen = 1;
    }
    char requireLine[2]{};
    const bool requireLineEnabled =
        GetEnvironmentVariableA("MIKUDANCESTUDIO_AB_REQUIRE_LINE", requireLine,
                                sizeof(requireLine)) == 1 &&
        requireLine[0] == '1';
    char keepPhysics[2]{};
    const bool keepPhysicsEnabled =
        GetEnvironmentVariableA("MIKUDANCESTUDIO_AB_KEEP_PHYSICS", keepPhysics,
                                sizeof(keepPhysics)) == 1 &&
        keepPhysics[0] == '1';
    static LONG lineModeRequested = 0;
#endif
    DumpFrameEntryState(app);
#ifdef MIKUDANCESTUDIO_DIAG
    if (requireLineEnabled && lineModeRequested != 0)
        DumpFrameEntryState(app, "frame_state_line.json");
#endif
#if MMD_ORIGINAL_PROFILE
    // 0x46B118..0x46B17B: one-second render-frame counter.  The original
    // snaps the two common cap-adjacent values to their nominal rates.
    float& fpsElapsed = s.state.fpsOverlayElapsedSeconds;
    std::uint32_t& fpsFrames = s.state.fpsOverlayFrameCount;
    fpsElapsed += s.DeltaTime();
    // 0x46B132: fld1/fcompp + test $0x05/jp - reset only when elapsed is
    // STRICTLY greater than 1.0 (equal or unordered skips).
    if (fpsElapsed > 1.0f) {
        s.state.framesPerSecond = fpsFrames - 1;
        fpsElapsed = 0.0f;
        fpsFrames = 0;
    }
    ++fpsFrames;
    if (s.state.framesPerSecond == 29)
        s.state.framesPerSecond = 30;
    if (s.state.framesPerSecond == 59)
        s.state.framesPerSecond = 60;
#else
    // Idle/performance-saving pump iterations are not rendered scene frames.
    frame_rate::Tick(app);
#endif

    // ---- 1. mouse-delta snapshot reset ------------------------------------
    // Clamp the coordinate selector's state before anything reads it this frame
    // (the drag handlers, the gizmo and the caption all read the byte), and
    // record the selector's inputs when the field trace is enabled.
    NormalizeCoordinateState(app);
    TraceCoordinateSelector(app);
    TraceFrameViewpoint(app);
    MouseInteractionBegin(app);                                  // Per-frame logic.
    // 0x46FF02 poll done.  The original pump then walks, in this order:
    // the right/middle-button camera drags (x86 0x470BF5..0x47133D), the
    // non-letter edit-key chain whose panel focus sweep opens the ladder
    // region (0x471342..0x473127), and the letter ladder itself
    // (0x46FF35..0x4739E2) - the arrow-key and numpad blocks are
    // interleaved inside the ladder at their binary positions.
    mikudancestudio::ConsumeRightButtonDrag(app);  // pump_navigation.cpp
    mikudancestudio::ConsumeMiddleButtonPan(app);  // pump_navigation.cpp
    mikudancestudio::ConsumeEditKeys(app);         // pump_edit_keys.cpp
    if (device_lost_wait::Pending(app)) return;
    mikudancestudio::ConsumeLetterHotkeys(app);    // key_ladder.cpp
    if (device_lost_wait::Pending(app)) return;
#ifdef MIKUDANCESTUDIO_DIAG
    TraceOperationInput(app, "before");
#endif

    // ---- 2. interaction-mode dispatch --------------------------------------
    const ViewportDragMode mode = s.InteractionDragMode();
    if (mode != ViewportDragMode::None && s.LeftMouseButtonHeld()) {
        switch (mode) {
        case ViewportDragMode::ViewAxisRotateX:
        case ViewportDragMode::ViewAxisRotateY:
        case ViewportDragMode::ViewAxisRotateZ:
            ModeRotate(app, static_cast<int>(mode) - 1);
            break;
        case ViewportDragMode::LocalAxisTranslateX:
        case ViewportDragMode::LocalAxisTranslateY:
        case ViewportDragMode::LocalAxisTranslateZ:
            ModeTranslate(app, static_cast<int>(mode) - 4);
            break;
        case ViewportDragMode::BoneScale:
            ModeScale(app);
            break;
        case ViewportDragMode::BoneMoveVertical:
        case ViewportDragMode::BoneMoveScreenPlane:
            ModeBoneRotate(app, static_cast<int>(mode) - 8);
            break;
        case ViewportDragMode::PhysicsAxisX:
        case ViewportDragMode::PhysicsAxisY:
        case ViewportDragMode::PhysicsAxisZ:
            ModePhysicsBody(app, static_cast<int>(mode) - 10);
            break;
        case ViewportDragMode::CameraAdjustX:
        case ViewportDragMode::CameraAdjustY:
        case ViewportDragMode::CameraAdjustZ:
            ModeCameraAdjust(app, static_cast<int>(mode) - 13);
            break;
        case ViewportDragMode::AngleAdjustX:
        case ViewportDragMode::AngleAdjustY:
        case ViewportDragMode::AngleAdjustZ:
            ModeAngleAdjust(app, static_cast<int>(mode) - 16);
            break;
        case ViewportDragMode::ScaleUniform:
        case ViewportDragMode::ScaleX:
        case ViewportDragMode::ScaleY:
        case ViewportDragMode::ScaleZ:
            // Bone-scale HUD row.  axis -1 is uniform, 0/1/2 are X/Y/Z; the
            // transform, save and undo live in the bone-scale runtime.
            bone_scale::Drag(app,
                mode == ViewportDragMode::ScaleUniform
                    ? -1
                    : static_cast<int>(mode) -
                          static_cast<int>(ViewportDragMode::ScaleX));
            break;
        case ViewportDragMode::ScaleGizmoX:
        case ViewportDragMode::ScaleGizmoY:
        case ViewportDragMode::ScaleGizmoZ:
            // MMS addition: the viewport scale handles.  The amount is this
            // frame's pointer delta measured along the handle's projected screen
            // direction, so a horizontal axis is driven by a horizontal drag;
            // the axis itself is the coordinate selector's, exactly like the HUD
            // row's X/Y/Z cell.
            bone_scale::DragScreen(app,
                static_cast<int>(mode) -
                    static_cast<int>(ViewportDragMode::ScaleGizmoX));
            break;
        case ViewportDragMode::ScaleGizmoPlane:
            // MMS addition: the white square of the viewport gizmo scales both
            // view-plane axes in one increment, driven along its projected
            // diagonal so horizontal and vertical drags both work.
            bone_scale::DragScreenPlane(app);
            break;
        default:
            break;
        }
        // The drag that owns the gesture re-centres the pointer once the
        // movement has been applied, which is what gives it an unlimited
        // range (the original does this at the end of every drag body).
        RecentreCenteredDragCursor(app);
    }
#ifdef MIKUDANCESTUDIO_DIAG
    TraceOperationInput(app, "after");
#endif

    // ---- 3. mouse position update ------------------------------------------
    MouseInteractionEnd(app);
    // MMS: keep the 缩放 X/Y/Z edits in step with scale drags, undo and frame
    // steps without waiting for a view refresh.
    SyncBoneScaleEdits(app);

    // ---- 4. selection callback (0x46DCA4..0x46DCCD) ---------------------
    unsigned char selActive = 0;
    if (s.state.frameStepPlayback == 0 &&                  // 0x9ED90
        s.state.depthDeviceEnabled != 0) {                  // 0xA03B8
        // 0xA03D4 = the OpenNI is-tracking callback slot (literal was a
        // +0x80 decimal slip that landed mid-cameraAttachmentBasis)
        // ?OpenNIIsTracking@@YGXPA_N@Z - void __stdcall OpenNIIsTracking(bool*)
        using OpenNIIsTrackingFn = void(__stdcall*)(unsigned char*);
        auto cb = reinterpret_cast<OpenNIIsTrackingFn>(
            s.OpenniTrackingCallback());                         // 0xA03D4
        if (cb != nullptr)
            cb(&selActive);
        // 0x46DCCF..0x46DD61（x64 0x7FF7CB44A34C..0x7FF7CB44A3EC）：原版
        // 泵在探测之后立即做的深度图请求与菜单 0x124 卫生（oni_skeleton_
        // pump.cpp 含地址锚点）。
        ManageKinectRecordGate(app, selActive);
    }

    // ---- 5. dynamic overlay producer + D3D scene envelope ---------------
    if (device_lost_wait::Pending(app)) return;
    // Poll modern video even while stopped, so the last asynchronous scrub
    // request reaches the screen without another mouse movement.
    if (s.AviBackgroundEnabled() != 0 &&
        (s.PlaybackActive() != 0 || media::HasVideo(app) || s.AviBackgroundTexture() == nullptr ||
         device_lost_wait::BackgroundRefreshPending(app)))
        AviBgOverlayRefresh(app);
    if (s.state.timelineAdvanceRequested != 0) antialias::ResetHistory(app);
    RenderFrameSceneForOutput(app);

    // ida_capture_original_vb.py samples operation state at 0x46FF07: the
    // current overlay has been produced, while 0x4757C3 has not yet reset
    // the transient line counter for the next frame.
    DumpRequestedFrameState(app);

#ifdef MIKUDANCESTUDIO_DIAG
    // Match ida_capture_original_vb.py: its one-shot breakpoint changes
    // these fields at the first camera sprite Unlock, after transforms and
    // sprite production but before the following FrameDriver cycle.
    if (requireLineEnabled && LineCaptureSwitchReady() &&
        InterlockedCompareExchange(&lineModeRequested, 1, 0) == 0) {
        const bool alreadyModelMode =
            app->state.optflag[0] == 0 &&
            app->EditMode() == ViewportEditMode::Bone;
        if (!alreadyModelMode) {
            RebuildModelModePanel(app);
            app->state.optflag[0] = 0;
            app->EditMode() = ViewportEditMode::Bone;
            PostLanguageSweep(app);
            PostModelReload2(app);
            HandleWindowSize(app);
            InvalidateRect(static_cast<HWND>(app->Hwnd()), nullptr, FALSE);
        }
        char stableCapture[2]{};
        if (GetEnvironmentVariableA(
                "MIKUDANCESTUDIO_AB_STABLE_CAPTURE", stableCapture,
                sizeof(stableCapture)) == 1 && stableCapture[0] == '1') {
            app->state.fpsOverlayElapsedSeconds = 0.0f;
            app->state.fpsOverlayFrameCount = 0;
            app->state.framesPerSecond = 0;
        }
        if (!keepPhysicsEnabled)
            app->state.accessoryEditDialogOpen = 1;
    }
#endif

    // ---- 5.5 frame-step recording readback (0x46E787..0x46EFB7) ---------
    // The original splits right after the scene render's EndScene: with
    // the frame-step byte 0x9ED90 set (AVI recording / manual stepping)
    // the readback+push pass runs before the catch-up; a failure inside
    // it runs the recording epilogue and returns from the whole driver.
    if (s.state.frameStepPlayback != 0) {                   // 0x9ED90
        if (!RecordingReadbackPass(app))
            return;
    }

    // ---- 6. playback catch-up + physics (0x46EEE0..0x46FEB8) ------------
    PlaybackCatchup(app, selActive);
    if (device_lost_wait::Pending(app)) return;
    // ---- 6.5 Kinect 骨架驱动泵块（x64 0x7FF7CB44C12E..0x7FF7CB44C656）----
    // 原版位于物理帧内部：settle 上升沿请求（0x7FF7CB44C12E）之后、gate B
    // （0x7FF7CB44C65B）之前。settle 请求已移植在 physics_frame.cpp，该文件
    // 本轮冻结，故此处按同一三重门先行调用（门输入互不影响；块尾与
    // DisableKinect 都会补置 settle，先后次序无观测差异）。
    // oni_skeleton_pump.cpp 含完整地址锚点。
    PumpKinectSkeleton(app, selActive);
    { MMD_PERF_SCOPE(Physics); PhysicsFrame(app, selActive); }
    // x64 0x7FF7CB456F21（泵尾，紧随 previousMouse 回写 0x7FF7CB456F08..F17）:
    // 把本趟 selActive 滞留进 app+0xA137C；下一趟物理帧的 settle 请求
    // （0x7FF7CB44C12E）读它做上升沿判定。物理帧已消费完上一趟的值，
    // 此处回写不影响本趟。
    s.state.selectionActiveLatch = selActive;

    // The original produces text at 0x46FECF and resets/rebuilds the
    // selection line batch at 0x4757C3. Both are consumed on the next frame.
    if (s.RecordingWindow() == nullptr)
        PrepareFrameTextOverlay(app);
    PrepareFrameLineOverlay(app);

    // ---- 7. timeline flag handoff (0x479864..0x479896) -------------------
    if (s.state.timelineAdvanceDue != 0) {
        TimelineAdvance(app);                                     // 0x460130
        s.state.timelineAdvanceDue = 0;
        s.state.timelineAdvanceRequested = 0;
        if (device_lost_wait::Pending(app)) return;
    }
    if (s.state.timelineAdvanceRequested != 0) {
        s.state.timelineAdvanceDue = 1;
        s.state.timelineAdvanceRequested = 0;
    }

    // ---- 8. reload path (0x47989D..0x4798CB) ----------------------------
    if (s.state.modelReloadPending != 0) {
#ifdef MIKUDANCESTUDIO_DIAG
        if (getenv("MIKUDANCESTUDIO_TRACE_REC")) {
            FILE* tf = fopen(getenv("MIKUDANCESTUDIO_TRACE_REC"), "a");
            if (tf) { fputs("frame section8 reload path\n", tf); fclose(tf); }
        }
#endif
        // A deferred camera-follow refresh must not re-anchor the free view
        // if the user has already returned to model editing without follow.
        if (s.CameraMode() != 0 || s.state.followCameraEnabled != 0) {
            s.state.cameraAttachmentTransformSuppressed = 0;
            // Keep the eye: it carries the viewpoint, and the 10 belonging to
            // cameraPosY must not be written here either - that is the same
            // mistake InitDefaults made (commit 116e3aa). PostModelReload below
            // re-anchors the camera's own target.
            ReloadModels(app);                                    // 0x42E640
            PostModelReload(app);                                 // 0x41A650
        }
        s.state.modelReloadPending = 0;
    }

    // ---- 9. Present + device-lost recovery (0x479B23..0x479CB9) ----------
    // NOT a Reset dance (phase-A misread): the vtable call is slot +0x44 =
    // 17 = Present (Reset is slot 16 - adjacent, hence the confusion).
    // Gate: A0D6C != 0 && A0D61 == 0 (the skip path reuses a stale local
    // hr in the original; the port keeps last frame's value).
    //   A0274 != 0                 -> Present(NULL, NULL, NULL, NULL)
    //   else A0D24 != 0            -> Present(&r{0,0,A08D4,A08D8}, NULL,
    //                                   hwnd A0D24, NULL)
    //   else A0D38 != 0            -> Present(&RECT@A0D40, same, A0D38, NULL)
    //   else                       -> Present(&RECT@A0D40, same, A06B8, NULL)
    // Device loss waits with a shared message pump, including windowed mode.
    if (device_lost_wait::Pending(app)) return;
    IDirect3DDevice9* device = nullptr;
    if (s.Renderer() != nullptr)
        device = s.Renderer()->device;                     // +0x1D4E0
    static HRESULT hr = D3D_OK;         // stale-local equivalent
    bool presented = false;
    if (device != nullptr) {
        if (s.state.messageSeen != 0 &&
            s.AviStereoOutput() == 0) {
            MMD_PERF_SCOPE(Present);
            presented = true;
            if (s.FullscreenMode() != 0) {
                hr = device->Present(nullptr, nullptr, nullptr, nullptr);
            } else if (s.RecordingWindow() != nullptr) {          // 0xA0D24
                RECT r;
                r.left = 0;
                r.top = 0;
                // 0xA08D4/0xA08D8 = render size (literals were decimal
                // slips landing inside the dirModel path buffer)
                r.right = static_cast<LONG>(s.RenderWidth());
                r.bottom = static_cast<LONG>(s.RenderHeight());
                // The recording preview may be smaller than the capture.
                // A null destination fits the entire source to its client area.
                hr = device->Present(&r, nullptr,
                                     s.RecordingWindow(), nullptr);
            } else if (s.FloatingWindow() != nullptr) {
                RECT r = FramePresentationRect(app);
                hr = device->Present(&r, &r,
                                     s.state.floatingWindow,
                                     nullptr);
            } else {
                RECT r = FramePresentationRect(app);
                hr = device->Present(&r, &r,
                                     static_cast<HWND>(s.Hwnd()),
                                     nullptr);
            }
            frame_rate::Presented(app, SUCCEEDED(hr));
            if (hr == D3DERR_DEVICELOST) {
                device_lost_wait::Wait(app, device, false);
                // A dispatched message may have destroyed the renderer.
                return;
            }
        }
    }
#ifdef MIKUDANCESTUDIO_DIAG
    if (presented && SUCCEEDED(hr)) {
        if (requireLineEnabled && lineModeRequested != 0)
            ActivateRefreshedVbCapture();
        HoldPresentCaptureFence();
    }
#endif


}

}  // namespace mikudancestudio
