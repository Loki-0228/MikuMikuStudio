// ===========================================================================
// MikuMikuStudio - cross-module declarations for ported original functions
// ===========================================================================
// Every declaration records its original virtual address. Functions whose
// bodies are not yet ported live in src/unported/stubs.cpp (or
// src/window/wndproc_stubs.cpp); a "(stub)" marker on a declaration indicates
// The body remains in one of the two files. Defect HB-11.
// ===========================================================================
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>

namespace mikumikustudio {

class MMDApp;
struct PathResolutionWorkspace;
struct DShowRecorder;
class D3DRenderer;  // d3d_wrapper.hpp (the "0x1D574" render/locale object)
class PhysicsScene;  // physics_scene.hpp (the "0x48" physics scene wrapper)

// Shared camera/light/shadow/gravity/accessory selection reset. Missing
// tracks are allowed during scene construction and teardown.
void ClearGlobalFrameSelection(MMDApp* app);
void ClearTimelineKeySelection(MMDApp* app);

// ---- startup chain --------------------------------------------------------
// VA 0x0047A5B0 - subsystem alloc + config load + window classes + window.
bool InitMainWindowAndD3D(MMDApp* app, void* hInstance, int nShowCmd);

// VA 0x0046B090 - per-frame driver (60 KB in the original; ported in phases).
void FrameDriver(MMDApp* app);
void PrepareFrameSpriteOverlay(MMDApp* app);  // VA 0x0046BD79 HUD sprites
void PrepareFrameTextOverlay(MMDApp* app);    // VA 0x00423420
void PrepareFrameLineOverlay(MMDApp* app);    // VA 0x004757C3

// VA 0x00462C40 - pre-free cleanup (DirectX release, font/GDI teardown).
void ShutdownCleanup(MMDApp* app);

// VA 0x00407A70 - robust ANSI -> wide conversion (locale fallback chain).
void ConvertAnsiToWide(void* localeTableBase, const char* multiByteStr,
                       wchar_t* destination, int destinationWords);

// ---- math -----------------------------------------------------------------
// VA 0x00401000 - quaternion (x,y,z,w) -> row-major 3x4 rotation matrix.
void QuaternionToMatrix3x4(float* outMatrix, const float* quaternion);

// VA 0x0040AD00 - COLORREF lerp: a - (a - b) * t per channel, truncated.
std::uint32_t ColorLerp(std::uint32_t a, std::uint32_t b, float t);

// VA 0x00408960 - removes the file name from a full path and returns the filled
// directory buffer. The inner copy is limited to the destination capacity that the
// caller provides, since the original fixed 1000-wchar limit exceeds
// the buffers most callers supply: those are PathResolutionWorkspace
// fields, each kPathFieldCapacity (256 wchars) wide. Sources are 255
// characters or fewer today, so the overflow was latent.
// Defect HB-02.
wchar_t* ExtractDirFromPath(wchar_t* destination, const wchar_t* fullPath,
                            std::size_t destinationCapacity);

// ---- subsystem init targets (bodies pending; offsets in offsets.hpp) -----
void RendererInit(D3DRenderer* renderer);  // VA 0x00406D40 (locale/font)
void Sub025CInit(void* sub);               // VA 0x004C2450
void Sub06CInit(DShowRecorder* recorder);  // VA 0x00408EA0
void Sub04B0Init(void* sub);               // VA 0x004C4760
bool InitAxisMesh(MMDApp* app);             // VA 0x004C5150
bool MakeLineGeometry(MMDApp* app);          // VA 0x0040AF40 (was InitGridGeometry;
                                             // full body incl. ground-quad VB + JP msg)
void PhysicsSceneInit(PhysicsScene* scene); // VA 0x00401360
void FontSubInit(MMDApp* app, const wchar_t* exeDir);  // VA 0x00408E70
void LocalizeUI(MMDApp* app);             // VA 0x00441AD0 (UI language pass)
// MMS addition (no original VA): writes the interpolation-channel selector
// (control 433) for the panel that is active - the four bone channels plus the
// bone scale channel, or the six camera channels - and repairs the list when it
// no longer matches.  With `force` the list is always rewritten (language
// change).  Defined in src/window/localize_ui.cpp.
void EnsureInterpChannelCombo(MMDApp* app, bool force = false);
// MMS addition (no original VA): caption and check states of the model panel's
// bone-mode buttons, derived from MMDApp::EditMode().  It is the single writer
// of the merged select / box-select button 490, whose caption follows the
// active mode.  Defined in src/window/localize_ui.cpp.
void RefreshBoneModePanel(MMDApp* app);
void RefreshMenuLanguage(MMDApp* app);   // VA 0x0040B5A0
bool CreateUIControls(MMDApp* app, HWND hwnd);  // VA 0x00466D20 phase A
void InitFlagSubsystem(MMDApp* app);       // VA 0x00461E00

// ---- file loaders reached from startup dispatch ---------------------------
void LoadSceneFile();                      // VA 0x00458F80 (.pmm)
void LoadSceneFile(const wchar_t* candidate);
void LoadModelFile(MMDApp* app, const wchar_t* path,
                   bool showModelInfo = true);  // VA 0x00460430 (.pmd/.pmx)
void LoadAccessoryFile(const wchar_t* path);  // VA 0x00460B30 (.x)

// ---- model object chain (0x460430 -> 0x4A8DC0 -> 0x4BF3E0) ---------------
void ModelInitDefaults(unsigned char* model);         // VA 0x004A8DC0
void ModelInitMorphSlots(unsigned char* model);       // VA 0x004A89B0
bool ModelLoadPMD(unsigned char* model, HWND hwnd, const wchar_t* path,
                  D3DRenderer* sub, int a5, std::uint8_t a6,
                  std::uint8_t a7, PhysicsScene* a8,
                  PathResolutionWorkspace& paths);    // VA 0x004BF3E0
// a6 = message-box gate (constant 1 at 0x460430), a7 = EnglishUI flag
// (stored to model+12740), a8 = Sub048 pointer *(app+0x9EDB0), typed as
// PhysicsScene* (stored to model+60 - the physics scene wrapper).  Full
// arg map in pmd_load.cpp header.
bool LoadPMX(unsigned char* model, D3DRenderer* sub,
             std::uint8_t showInfo, std::uint8_t englishUI,
             PathResolutionWorkspace& paths, int fh); // VA 0x004B77E0
void ModelDispose(unsigned char* model);              // VA 0x0048F830
void DisposeAccessory(void* accessory);               // VA 0x004C4700
void InitBoneSortOrder(unsigned char* model);         // VA 0x00490070（src/model/bone_sort.cpp）。Defect HB-14。
void PostLoadInit(unsigned char* model);              // VA 0x0049C850（src/model/post_load_init.cpp）。Defect HB-14。
bool SceneConstruct(PhysicsScene* scene, D3DRenderer* d3dSub);  // 0x4032B0
void ModelBonePhysicsModeApply(unsigned char* model, int mode); // VA 0x004B2210
void ModelKinematicSync(unsigned char* model);        // VA 0x004B22F0
void ModelDynamicReseat(unsigned char* model);        // VA 0x004B3460
void ModelPhysicsReadback(unsigned char* model);      // VA 0x004B25D0
void PhysicsFrame(MMDApp* app, unsigned char selActive);  // FrameDriver physics section 0x46F0D0..
void PlaybackCatchup(MMDApp* app, unsigned char selActive);
                                // FrameDriver playback section 0x46EEE0..
void PlaybackPoseAdvance(MMDApp* app, int advance);  // VA 0x004175A0
void InitModelTrackCursors(unsigned char* model, double cursor,
                           int physicsMode);  // was Sub4A2CD0, VA 0x004A2CD0
void AdvanceModelKeyframes(unsigned char* model, double cursor,
                           int physicsMode);  // was Sub4A31D0, VA 0x004A31D0
float BoneEase(unsigned char* model, int channel, int keyIdx,
               float t);  // was Sub4A05A0, VA 0x004A05A0 (VMD easing)
void NotifyBonePhysicsMode(unsigned char* model, int boneIdx,
                           unsigned char mode);  // was Sub499B50, VA 0x00499B50
int SeekModelFrame(unsigned char* model, int frame,
                  int physicsMode);  // was Sub4B4260, VA 0x004B4260 (frame seek)
void FinishAviRecord(MMDApp* app, bool reachedRequestedEnd = false);  // was Sub464A00, VA 0x00464A00
                                  // (recording-end epilogue; full port)
bool TeardownDShowGraph(DShowRecorder* rec, bool* completed = nullptr);
void StopPlayback(MMDApp* app);  // was Sub4341E0, VA 0x004341E0
                                // (playback-end UI restore; full port)
void CloseDataFile(void* file);                       // VA 0x004C2680 (ui_refresh.cpp)
// WAV open/play chain of the 0x25C audio context (src/media/wave_audio.cpp).
// WaveStartPlayback supersedes the old void Sub4C2760(void*) stub decl
// (that stub body has since been deleted from stubs.cpp).
void WaveCtxReset(void* obj);                         // VA 0x004C2660
void ClearWaveAudio(MMDApp* app); // New project: release audio without reopening a device.
void WaveFindDataChunk(void* obj, FILE* stream);      // VA 0x004C26F0
bool WaveStartPlayback(void* obj);                      // VA 0x004C2760
bool WaveStreamRead(void* obj, void* buf, int bytes); // VA 0x004C2960
bool WaveStreamFeed(void* obj, void* buf, int bytes, void* buf2,
                    int bytes2);                      // VA 0x004C2C90
bool WaveLoadFile(void* obj, const wchar_t* path,
                  PathResolutionWorkspace& paths);    // VA 0x004C2F70
void SetFrameNormalized(int v);                       // VA 0x004C2B80 (ui_timeline_gfx.cpp)
void TimelineDrawTicks(int frameOffset, int width);  // VA 0x004C2A00 (ui_timeline_gfx.cpp)
void WaveSeekAndFeed(void* obj, double seconds);  // was Sub4C34A0, VA 0x004C34A0

// ---- edit-commit chain (ui_edit_commit.cpp) -------------------------------
void CommitEditControl(MMDApp* app, HWND edit);      // was Sub463640, VA 0x00463640
void CommitEditControlTail(MMDApp* app, HWND edit);  // was Sub44BEF0, VA 0x0044BEF0
void InstallControlSubclasses(MMDApp* app, HWND hwnd);  // 0x466D20 chains
void InstallControlSubclass(MMDApp* app, HWND control, int id);

// ---- dialog procs (dialog_procs.cpp) --------------------------------------
INT_PTR CALLBACK FrameRangeDlgProc(HWND, UINT, WPARAM, LPARAM);  // was Sub44C5D0, VA 0x0044C5D0
INT_PTR CALLBACK SelectNavDlgProc(HWND, UINT, WPARAM, LPARAM);  // was Sub47A3F0, VA 0x0047A3F0
void SetPhysicsMode(unsigned char* model, int a2,
                    unsigned char* const* modelSlots,
                    int a4);                          // VA 0x004A9220
void ModelApplyMorphs(unsigned char* model);          // VA 0x004970B0
void InitializeBoneMorphOffsets(unsigned char* model);
void UpdateModelVertexBuffers(MMDApp* app, unsigned char* model,
                              const float frameWorld[16]); // VA 0x004B0C50
void CreateRigidBody(PhysicsScene* scene, void** out, int shape, float sx, float sy,
                     float sz, const float* matrix, int mode, float mass,
                     float dampLin, float dampRot, float restitution,
                     float friction, char group,
                     std::uint16_t mask);             // VA 0x004064F0
int CreatePhysJoint(PhysicsScene* scene, void* bodyA, void* bodyB,
                    float ax, float ay, float az, float aq0, float aq1,
                    float aq2, float aq3, float bx, float by, float bz,
                    float bq0, float bq1, float bq2, float bq3,
                    float linUpper0, float linUpper1, float linUpper2,
                    float linLower0, float linLower1, float linLower2,
                    float angUpper0, float angUpper1, float angUpper2,
                    float angLower0, float angLower1, float angLower2,
                    float spring0, float spring1, float spring2,
                    float spring3, float spring4,
                    float spring5);                  // VA 0x00406010
void TransformJointPointOriginal(float out[3], const float point[3],
                                 const float matrix[16]);
// Joint limit-sphere radius bound shared by the PMD/PMX loaders (returns
// distA + distB + the limit norm; see physics_create.cpp for the x64 anchors).
float JointRadiusBound(const float limits[6], float distA, float distB);
void BoneFrameTransform(unsigned char* model, unsigned char a2, int a3,
                        unsigned char* const* modelSlots,
                        int a5);  // VA 0x00493A60 (per-frame bone
                                          // transform updater; a2 channel,
                                          // a3 frame/layer, a4 slot array,
                                          // a5 physics mode)
void ArmBoneTransformIkProbe();

// ---- model-load path helpers --------------------------------------------
const wchar_t* ResolveUserFilePath(PathResolutionWorkspace& paths,
                                   const wchar_t* path);      // VA 0x4089F0
bool ResolveAnsiUserFile(unsigned char* sub, const char* mbName,
                         wchar_t* wideOut, rsize_t sizeWords,
                         PathResolutionWorkspace& paths);     // VA 0x407BA0
errno_t ConvertMaterialName(unsigned char* sub, const char* mbName,
                            wchar_t* wideOut, rsize_t sizeWords,
                            const wchar_t* dirW);             // VA 0x407DA0
int LoadTextureShared(unsigned char* sub, wchar_t* path);     // VA 0x407490
// The four ints are the rectangle edges of the filled panel (left, top,
// right, bottom), matching the parameter names used in
// src/render/fill_panel.cpp. The declaration previously spelled them
// x/y/w/h, which suggests an origin and size rather than edges.
// Defect HB-03.
void FillPanelBottom(HDC, int left, int top, int right, int bottom,
                     std::uint32_t color, std::uint32_t edge,
                     int flag);           // See VA 0x40DF10 (src/render/fill_panel.cpp). Defect HB-14.
void PostModelReload2(MMDApp*);                       // VA 0x0040D940 (ported)
void ApplyCurrentCameraView(MMDApp*);                  // use the current camera key as the viewport
void RebuildModelModePanel(MMDApp*);     // was Sub44D610, VA 0x0044D610
void RebuildCameraModePanel(MMDApp*);    // was Sub44D780, VA 0x0044D780
void ApplyModelComboSelection(MMDApp*);  // was Sub44D940, VA 0x0044D940
int ResolveModelComboSelection(MMDApp*);

// ---- window procedures ----------------------------------------------------
LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);  // VA 0x004C3A10 (ported)
LRESULT CALLBACK RecWndProc(HWND, UINT, WPARAM, LPARAM);   // VA 0x00479DA0
LRESULT CALLBACK MicWndProc(HWND, UINT, WPARAM, LPARAM);   // VA 0x00466A10

// ---- message targets referenced by MainWndProc (bodies pending) ----------
void CommandDispatch(HWND ctrl, WPARAM wParam);              // VA 0x0047E8A0 (68KB)
void SaveFlagSubsystem(MMDApp*);                            // VA 0x00461FA0
void HandleWindowSize(MMDApp*);                             // VA 0x00443300
void RefreshMainWindowViewport(MMDApp*);      // was Sub42C810, VA 0x0042C810
void RefreshSeparateWindowViewport(MMDApp*);  // was Sub4290F0, VA 0x004290F0
void HandleWindowPaint(MMDApp*);                            // VA 0x0047C0A0
LRESULT HandleNotify(HWND, UINT, WPARAM, LPARAM);           // VA 0x004398B0
void InstallPanelCheckboxPaint(HWND control);               // MMS themed-panel labels
void HandlePaletteChanged(HDC);                             // VA 0x0042CEB0
void HandlePaletteChanged2(HDC);                            // VA 0x0042C140
void HandleDropFiles(HDROP);                                // VA 0x00461300

// ---- render-output chain (0xD4 / 0x114 / 0xDF; bodies in
//      src/render/bg_overlay.cpp, src/render/device_reset.cpp,
//      src/app/avi_record_start.cpp, src/app/dshow_record_graph.cpp) ------
void AviBgOverlayRefresh(MMDApp*);            // VA 0x004168D0 (was Sub4168D0)
void AviBgPlacementRefresh(MMDApp*);          // Refresh geometry without decoding a frame.
void PicBgOverlayRefresh(MMDApp*);            // VA 0x00417130 (was Sub417130)
void StartAviRecordWindow(MMDApp*);           // VA 0x0045E820 (stub twin Sub45E820)
void StartAviRecordFullscreen(MMDApp*);       // VA 0x00464760 (stub twin Sub464760)
void ApplyFullscreenWindowState(MMDApp*, bool exclusive = false);  // was Sub4629D0, VA 0x004629D0
                                          // (fullscreen enter/restore window mgr)
void KickRecordPhysics(MMDApp*);  // was Sub401BD0, VA 0x00401BD0 physics kick
void HandleTimer100(MMDApp*);                               // VA 0x00429770
void HandleHScroll(LPARAM lParam, WPARAM wParam);   // VA 0x0044AEE0
void HandleVScroll(LPARAM lParam, WPARAM wParam);   // VA 0x0044BB30
LRESULT HandleCtlColor(HWND control, HDC dc);  // VA 0x0040E0E0
void HandleMouseMove(std::uint32_t lParam, int mouseY);     // VA 0x00444CC0
void HandleLButtonDown(MMDApp*);                            // VA 0x00446A70
void HandleLButtonUp(MMDApp*);                              // VA 0x0044A9A0
void HandleLButtonDblClk(MMDApp*);                          // VA 0x0044AAA0
void HandleMouseActivate(MMDApp*);                          // VA 0x004632F0
void HandleMouseWheel(int wheelDelta);                      // VA 0x0044BD70
void PostLanguageSweep(MMDApp*);                            // VA 0x0042F1E0
void PostLanguageSweep2(MMDApp*);                           // VA 0x0040D070

// ---- model queries used by the effect-API exports -------------------------
int GetPmdNum(MMDApp* app);                                   // VA 0x0042A110

// ---- font / texture subsystem (phase 4) ----------------------------------
void CreateUiFont(MMDApp* app, HWND hwnd);        // from 0x00466D20 phases 21-23
void ApplyUiFontToControl(MMDApp* app, HWND control, int id);
int  DrawGlyph(MMDApp*, const char*, HDC, int size, int x, int y,
               unsigned char r, unsigned char g, unsigned char b,
               int bold);                         // VA 0x0040E290
bool InitToonTextures(MMDApp* app);               // VA 0x00424DC0 (reclassified: toon, not font)
bool InitSceneFontTexture(MMDApp* app);           // VA 0x0042AE80

// One text run inside the 512x512 HUD font atlas, addressed with normalized
// texture coordinates.  The atlas rows are 32 pixels tall and the strings are
// drawn with a proportional font (Arial / MS PGothic, or its Chinese
// translation), so a label must be sampled as ONE cell: per-character cells
// only line up for the fixed-pitch digit rows.
struct SceneFontCell {
    float u0;
    float v0;
    float u1;
    float v1;
    // Atlas rows the cell spans.  The coordinate captions also exist as a
    // 64-row, 60-pixel copy, so the caller scales its u extent by
    // kSceneFontRowHeight / band to keep the painted width with the painted
    // height.
    float band;
};
constexpr float kSceneFontAtlasSize = 512.0f;
constexpr float kSceneFontRowHeight = 32.0f;
// Looks up a string drawn into the atlas and reports its measured extent, from
// whichever of the two caption tiers better matches the HUD scale the caller is
// painting at.  Returns false when the atlas does not contain that exact text
// (the atlas is built with the localized strings, so callers pass the English
// key).
bool SceneFontLabelCell(const char* text, float scale, SceneFontCell* out);

// World-space direction that appears along one screen axis (0 = right,
// 1 = up, 2 = towards the viewer) for the current camera rotation.  Shared by
// the view-space bone drag and the operation-axis gizmo, which must agree.
void ViewWorldAxis(MMDApp* app, int axis, float out[3]);

// How many coordinate-system states the selector may reach right now.  The
// third state belongs to the active panel: "View" (screen axes) on the
// model/bone panel, "Accessory" (edit the selected accessory) on the
// camera/light/accessory panel.  The accessory state only exists while that
// panel really has an accessory to edit, so with an empty accessory list the
// selector stays on Local/Global instead of offering a state that edits
// nothing.
int CoordinateStateLimit(MMDApp* app);
// The stored coordinate state clamped to [0, CoordinateStateLimit).
// Every reader of this state - the accessory drag handlers, the bone-handle
// axes, the operation-axis gizmo, the mouse-wheel refresh gate, and the caption -
// must use this rather than the raw byte; NormalizeCoordinateState writes the
// clamped value back once per frame. Otherwise a stale 2 (the accessory state
// after the selected slot lost its accessory) would keep steering those paths.
int EffectiveCoordinateState(MMDApp* app);
void NormalizeCoordinateState(MMDApp* app);

// ---- frame driver sub-phases (0x0046B090; bodies pending) -----------------
void MouseInteractionBegin(MMDApp*);              // Snapshot of mouse deltas
// Warp the pointer of a running centred drag back into the viewport when it
// leaves it, so the drag has no vertical limit (original: once per frame in
// each drag body, x64 0x323DA / 0x326B4 / 0x32761 / 0x36BB6).
// Centred tools sample the live cursor once per frame. Queued WM_MOUSEMOVE
// coordinates may precede a cursor warp and must not overwrite their origin.
bool OwnsCenteredViewportDrag(MMDApp* app);
void ApplyCenteredDragCursorSample(MMDApp* app, int x, int y);
void RecentreCenteredDragCursor(MMDApp*);
bool HandleViewportToolPress(MMDApp*);
// The same press handling, restricted to the coordinate strip.  The strip is
// a toggle, and the window class sets CS_DBLCLKS, so the second click of a double
// click arrives as WM_LBUTTONDBLCLK and must also toggle the strip -
// otherwise two quick clicks would move the selector only once.  Returns false
// and leaves the message for the other handlers when the cursor is not on the strip.
bool HandleCoordinateStripPress(MMDApp*);
void HandleViewportToolRelease(MMDApp*);
// Ends the current gesture the way a mouse-up does, but keeps the tracked
// left-button state "held" while `physicallyHeld` says the button is really
// still down.  WM_CAPTURECHANGED and WM_CANCELMODE both come through here with
// the button held; reporting them as a release let the frame driver's input poll
// re-read the same press as a new one, so the coordinate selector advanced twice
// per click.  See frame_modes.cpp.
void ReleaseLeftButtonState(MMDApp* app, bool physicallyHeld);
void HandleViewportRightDoubleClick(MMDApp*);
void HandleViewportBonePress(MMDApp*, bool additive);
void MouseInteractionEnd(MMDApp*);                // mouse position update
void ModeRotate(MMDApp*, int axis);               // modes 1..3 view-axis bone rotation
void ModeTranslate(MMDApp*, int axis);            // modes 4..6 local-axis bone rotation
void ModeScale(MMDApp*);                          // mode 7 horizontal bone move
void ModeBoneRotate(MMDApp*, int part);           // modes 8..9 vertical/combined move
void ModePhysicsBody(MMDApp*, int axis);          // modes 10..12 local-axis/record move
void ModeCameraAdjust(MMDApp*, int part);         // modes 13..15
void ModeAngleAdjust(MMDApp*, int part);          // modes 16..18
void BoneLocalAxes(MMDApp*, float out[16]);       // VA 0x0040E670
void BoneEditModes(MMDApp*);                      // VA 0x475A6E..0x4786FA
void PushBoneEditUndo(MMDApp*);                    // VA 0x0042D6E0, was Sub42D6E0
void RefreshRequest(int area);                    // VA 0x00440AC0
void TimelineAdvance(MMDApp*);                    // VA 0x00460130
void ReloadModels(MMDApp*);                       // VA 0x0042E640
void PostModelReload(MMDApp*);                    // VA 0x0041A650 (Sub41A650 was an
                                                  //  alias of this - decl removed)
void SelectionReeval(MMDApp*);                    // VA 0x00430510
bool PostDeviceReset(MMDApp*);                    // VA 0x00440DB0; false on reset failure
void UpdateBoneFrames(MMDApp*);                   // VA 0x00433A40
void RenderFrameScene(MMDApp*);                   // VA 0x0046DC00..0x0046E787
bool RecordingReadbackPass(MMDApp*);              // VA 0x0046E787..0x0046EFB7
ULONG DownsampleCaptureSurface(MMDApp*);          // VA 0x00425970
// Recreates the lockable recording render target released by PostDeviceReset
// and points render target 0 back at it
// false means callers must use the back-buffer path instead.
// Defect RB-04.
bool EnsureCaptureRenderTarget(MMDApp* app);
void DrawAccessoryDebug(MMDApp*);                 // VA 0x00420F30
void DrawPhysicsCollisionDebug(PhysicsScene*);    // VA 0x00406950
void DrawBoneOperationAxis(MMDApp*, const float[16]); // VA 0x0042DB10
void DrawFrameOverlayBatches(MMDApp* app);
void SetupFrameWorldTransform(MMDApp* app);       // VA 0x0046B185..0x0046BC20
// Camera attachment/model tracking reference: 0 = default target, 1 = model root,
// 2 = selected bone. Untracked model free view always uses the world origin.
void ModelViewReferencePoint(MMDApp* app, int mode, float point[3]);

// Records which source produced the frame pivot.
enum class FrameFocusSource { FreeView, ModelReference, CameraPosition, AttachmentBone };

// Resolve the point around which the frame transform rotates the world, reading
// `referenceMode` instead of the live reference-mode byte. Model root/selected-bone
// references apply to tracked model mode; untracked model mode stays a free view.
// Both the rendered frame (src/render/debug_geometry.cpp) and eye re-anchoring
// obtain the pivot through this single function.
FrameFocusSource ResolveFrameFocus(MMDApp* app, int referenceMode, float focus[3]);

// Moves the eye by R(focus_old - focus_new), so changing the frame's orbit pivot
// keeps the rendered image in place: the same correction the reference-mode
// re-anchor performs, and which the camera/model panel switch also uses.
void CarryEyeForFocusChange(MMDApp* app, const float focusOld[3],
                            const float focusNew[3]);
bool UseEffectModelRenderer(MMDApp*);            // gate at VA 0x0046DD9A
void RenderShadowMap(MMDApp*, const float[16]);  // VA 0x00426CD0
// Same native pass, with failure reporting for a borrowed MMV shadow snapshot.
bool RenderShadowMapChecked(MMDApp*, const float[16]);
bool RenderAccessoriesShadowChecked(MMDApp*);
void RenderModelsFixed(MMDApp*);                 // VA 0x00425D20
void RenderModelsEffect(MMDApp*, const float[16]); // VA 0x004277E0
void RenderAccessoriesFixed(MMDApp*);             // VA 0x004C4A10
void RenderAccessoriesFixedRange(MMDApp*, int, int);
void RenderAccessoriesProjectedGroundShadow(MMDApp*);
void RenderAccessoriesProjectedGroundShadowGeometry(MMDApp*);
void RenderAccessoriesShadow(MMDApp*);            // VA 0x004C52D0
void RenderAccessoriesEffect(MMDApp*);            // VA 0x004C55C0
void RenderAccessoriesEffectRange(MMDApp*, int, int);

// ---- command targets (0x0047E8A0 phase A; bodies pending) ----------------
void CmdLoadPose(MMDApp*);                        // 0xCA
void CmdSavePose(MMDApp*);                        // 0xCB
void CmdResetState(MMDApp*);                      // 0xCC -> 0x0044E540
void CmdOpenScene(MMDApp*);                       // 0xCD -> 0x00458F80
void CmdOpenWave(MMDApp*);                        // 0xCE -> 0x00418500
void CmdSaveScene(MMDApp*);                       // 0xD0 -> 0x0041B080
bool SaveSceneAs(MMDApp*, HWND owner);            // Accepted background Save As; explicit picker owner
void CmdLoadMotion(MMDApp*);                      // 0xD1 -> 0x00434B60
void CmdSaveMotion(MMDApp*);                      // 0xD2 -> 0x00419370
void CmdLoadAvi(MMDApp*);                         // 0xD5 -> 0x00433250

// ---- D3D subsystem (phase 5) -----------------------------------------------
bool InitD3D(MMDApp* app, HWND hwnd, bool english, HMODULE hModule); // 0x00408020
void InitRenderStates(MMDApp*);                   // VA 0x00406E90
bool ProbeStereo3D(void* device, void* arg20);    // VA 0x004CB430
void UpdateFrameStereo(MMDApp* app);              // VA 0x0046DC75
int NvapiInitChain();                             // VA 0x004C6940 (0/-1/-2)
int NvapiStereoCaps(int arg);                     // VA 0x004CAF10 (0/status/-3)
int NvapiStereoSupportGate();                     // VA 0x004CB210 (0/status/-3)
void PostViewRefresh(MMDApp*);                    // VA 0x0040D130

// ---- file loaders reached from the command dialogs ------------------------
void LoadVpdFile(const wchar_t* path);            // VA 0x00418A10 (src/io/vpd_file.cpp)
void SaveVpdFile(const wchar_t* path);            // VA 0x00418750 (src/io/vpd_file.cpp)
void ResetAppState(MMDApp*);                      // VA 0x0044E540
// Real bodies (src/media/): __thiscall(app) like the original - the path is
// read from app storage (+0xD0 WAV, +0x9E1EC AVI, +0x9E448 picture).
void LoadWaveFile(MMDApp* app);                   // VA 0x00418500 (real)
bool SaveSceneFile(MMDApp* app);                  // VA 0x0041B080 (src/io/pmm_save.cpp)
void PathToProjectDir(wchar_t* destination, const wchar_t* source);  // VA 0x00408870
void LoadSceneV2(MMDApp* app, int fd);             // VA 0x00450000 (v2 loader body,
                                                  //  phase 1), was Sub450000
void LoadSceneV1(MMDApp* app, int fd);            // VA 0x0045916D..0x45E7F7 (v1
                                                  //  loader body inside sub_458F80)
void LoadVmdFile(const wchar_t* path);            // wide-path wrapper of
                                                  //  LoadVmdMotion (VA 0x00434B60)
int LoadVmdMotion(MMDApp* app, const char* fileName);  // VA 0x00434B60, was Sub434B60
void PanelPaint(MMDApp* app);                     // VA 0x00414610 (ui_panel_paint.cpp)
int ResetMorphKeyCursor(unsigned char* model);    // VA 0x004A49A0, was Sub4A49A0
bool RegisterBoneKey(unsigned char* model, unsigned char* rec,
                     int frameOffset, unsigned char useSelected);  // 0x0049D880,
                                                  // was Sub49D880 (VMD/paste buffer)
bool RegisterBoneKeyAtIndex(unsigned char* model, unsigned char* rec,
                            int frameOffset, int boneIdx);
bool RegisterMirroredBoneKey(unsigned char* model, unsigned char* rec,
                             int frameOffset);    // 0x0049E310, was Sub49E310
void ResetBoneKeyCursor(unsigned char* model);    // VA 0x004A4940, was Sub4A4940
void AppendBoneKeyToUndo(unsigned char* model, int index);  // VA 0x0049D410, was
                                                  // Sub49D410
// Same, with the visit map the caller already resolved from
// mdl::BoneKeyCapacity.  Per-record loops use this form so the capacity lookup
// (mutex + map) does not run once per iteration; `visited` must come from
// mdl::BoneKeyVisitMap for the pool's current capacity.
void AppendBoneKeyToUndo(unsigned char* model, int index,
                         unsigned char* visited);
// VA 0x0049D4D0 - auto-smoothed interpolation rebuild (model_keyframe_edit).
void RebuildBoneKeyInterpolation(unsigned char* model, int index, int lane);
                                                  // was Sub49D4D0
int RegisterBonePoseAtFrame(unsigned char* model, int boneIdx,
                            std::uint32_t frame, int mode);  // VA 0x004B38A0,
                                                  // was Sub4B38A0
void RegisterSelectedBoneKeys(unsigned char* model, int frame, int mode);
                                                  // VA 0x004C2080, was Sub4C2080
void ResetDisplayKeyCursor(unsigned char* model); // VA 0x004A4A00, was Sub4A4A00
void DeleteMarkedModelKeys(unsigned char* model, int frame);
                                                  // VA 0x004A09E0, was Sub4A09E0
void BeginModelFramePasteUndo(unsigned char* model, int frame,
                             std::uint32_t bonePasteCount, bool morph, bool display);
void UndoModelEdit(unsigned char* model, std::int32_t* frame);
                                                  // VA 0x004A1870, was Sub4A1870
void RedoModelEdit(unsigned char* model, std::int32_t* frame);
                                                  // VA 0x004A2490, was Sub4A2490
// UI callers pass the global mode so undo/redo respects physics off/always-on.
void UndoModelEdit(unsigned char* model, std::int32_t* frame, int physicsMode);
void RedoModelEdit(unsigned char* model, std::int32_t* frame, int physicsMode);
// Exact 0x43F15E..0x43F60D inline block used by the frame-range scaler:
// convert the deletion snapshot to type 4, then open the paired type-2
// reinsertion snapshot with room for three 64-byte bone-key records per key.
void BeginRangeScaleBoneUndo(unsigned char* model, int frame,
                             int transformedKeyCount);
// Global-track key registrars (0x410AA0 camera / 0x411900 light /
// 0x4120B0 self-shadow / 0x412DF0 gravity): real bodies in
// src/window/command_control_400.cpp (sorted 10000-record list insert;
// the original passes the key record by value on the stack - the port
// takes (app, rec) with the same record layouts, a stub-era deviation
// absorbed by those ports).
// overflowAdvertised: 满表报错框里印的 point 数。粘贴路径（本函数 x64 原型
// Sub410AA0）打 10000；相机实况注册器（Sub410560/x64 0x7FF7CB47B18D）打
// 600000 —— 原版两处不一致，故做成参数。
int RegisterCameraKey(MMDApp* app, const void* rec,
                      int overflowAdvertised = 10000);    // VA 0x00410AA0, was Sub410AA0
int RegisterLightKey(MMDApp* app, const void* rec);       // VA 0x00411900, was Sub411900
int RegisterSelfShadowKey(MMDApp* app, const void* rec);  // VA 0x004120B0, was Sub4120B0
int RegisterGravityKey(MMDApp* app, const void* rec);     // VA 0x00412DF0, was Sub412DF0
// Track appliers (src/model/track_apply.cpp)
void ApplyGravityTrack(MMDApp* app);                // VA 0x00412330, was Sub412330
void ApplyAccessoryTrack(MMDApp* app, int slot);    // VA 0x00413120, was Sub413120

// ---- v2 loader dependencies (0x00450000 phase 2; bodies in stubs.cpp) -----
void ClearTimelineAndCurveDCs(MMDApp* app);   // was Sub40AE00, VA 0x0040AE00
void RefillBoneRegisterCombo(MMDApp* app, int slot);  // was Sub410040, VA 0x00410040
// (was Sub4C4700, VA 0x004C4700 - accessory-track dtor; the real port is
//  DisposeAccessory in src/render/accessory.cpp, the decl had zero callers
//  and was removed.)

// ---- v2 loader misc dependencies (decls; bodies elsewhere) ----------------
void IdentityCtor(void* obj);                    // was Sub4C46F0, VA 0x004C46F0
                                                // (original: identity ctor
                                                //  `return this;`; callers
                                                //  ignore the result, so the
                                                //  empty body is faithful)
bool LoadAccessoryObject(MMDApp* app, void* accessory,
                         const wchar_t* path);               // VA 0x004C5F40
void SyncAccessoryEditPanel(MMDApp* app);        // was Sub4134E0, VA 0x004134E0
// (was Sub4168D0/Sub417130 - the 0x4168D0/0x417130 bg-overlay refresh twins
//  unified into AviBgOverlayRefresh/PicBgOverlayRefresh declared above.)
// (was Sub4337A0, VA 0x004337A0 - superseded by LoadBackgroundPicture below;
//  the stub decl had zero callers and was removed.)
// Synchronize controls from live values, without evaluating or changing tracks.
void SyncCameraFovControls(MMDApp* app);
void SyncSelfShadowControls(MMDApp* app);
void RefreshSelfShadowPanel(MMDApp* app);        // was Sub411B90, VA 0x00411B90
void RefreshLightPanel(MMDApp* app);             // was Sub411070, VA 0x00411070
void TraceSceneLightState(MMDApp* app, const char* stage);
void RelayoutSidebarControls(MMDApp* app);       // was Sub442EB0, VA 0x00442EB0
void CommitSidebarResize(MMDApp* app);          // apply the staged pointer width on mouse-up
void SetModelColor(MMDApp* model, int r, int g, int b);  // was Sub4A4850, VA 0x004A4850
void SeekSelectedModelToCurrentFrame(MMDApp* app);  // was Sub4220C0,
                                                   // VA 0x004220C0 (model_frame_seek.cpp)
void SaveEnhancedModel(MMDApp* app, const wchar_t* path);  // was Sub41EC10,
void SetModelExtensionMenuEnabled(MMDApp* app, bool enabled);
bool SetSelectedBoneScaleAxis(MMDApp* app, int axis, float value);
// MMS: per-frame sync of the 缩放 X/Y/Z edits, and the X/Y/Z reset buttons.
void SyncBoneScaleEdits(MMDApp* app);
void ResetSelectedBoneScaleAxis(MMDApp* app, int axis);
bool SelectedBoneScaleReadout(MMDApp* app, float out[3]);
                                                   // VA 0x0041EC10 (enhance_model_io.cpp)
void ReloadTextureCache(void* renderer);        // was Sub4076E0, VA 0x004076E0
                                                // (enhance_model_io.cpp)
bool RegisterMorphKeyFromRecord(unsigned char* model, const unsigned char rec[40],
                                int frameOffset);  // was Sub49F190, VA 0x0049F190
void RegisterMorphKeyCurrent(unsigned char* model, int morph, int frame);  // was Sub49EEE0, 0x0049EEE0
bool RegisterMorphKeyAtIndex(unsigned char* model, int morph, std::uint32_t frame, float value);
bool RegisterDisplayKeyFromRecord(unsigned char* model, int frame,
                                  unsigned char view, int ikCount,
                                  const unsigned char* ikEntries, int selCount,
                                  const unsigned char* selEntries,
                                  int frameOffset);  // was Sub49F8C0, 0x0049F8C0
void RegisterDisplayKeyCurrent(unsigned char* model, int frame);  // was Sub49F480, 0x0049F480
void RegisterAccessoryKey(MMDApp* app, int frame, int slot);  // was Sub413CB0, 0x00413CB0
void DeleteMarkedKeyframes(MMDApp* app);         // was Sub4316B0, VA 0x004316B0
void SaveVmdFile(const wchar_t* path);            // VA 0x00419370 (src/io/vmd_save.cpp)
void LoadAviFile(MMDApp* app);                    // VA 0x00433250 (real, src/media/media_load.cpp)
void LoadBackgroundPicture(MMDApp* app);          // VA 0x004337A0 (real; the
                                                  //  was-Sub4337A0 stub decl
                                                  //  above was superseded and
                                                  //  removed)
void CopyPathW(wchar_t* dest, const wchar_t* src);  // VA 0x0042AE40 (real; the
                                                    //  Sub42AE40 stub decl in
                                                    //  stubs.cpp is unreferenced)
void CopyDirPathW(wchar_t* dest, const wchar_t* src);  // was Sub42AE20, VA 0x0042AE20
                                                       // (real; wcscpy_s with
                                                       //  the 1000-wchar
                                                       //  directory buffers)
void SelectFrameGroup(MMDApp*, int group);        // 0xD9/0xDA/0xDC target
// ---- frame-line edit commands (src/window/frame_line_edit.cpp) -----------
int InsertBoneCameraFrameLine(MMDApp* app);   // was Sub439E40, VA 0x00439E40
int DeleteBoneCameraFrameLine(MMDApp* app);   // was Sub43A650, VA 0x0043A650
// Bone-mode table surgery of the two commands above, WITHOUT the UI tail
// (seek/panel/timeline refresh): same walks, same undo bookkeeping, so the
// frame-line contract is assertable without a device or a window
// (tests/panel_key_tests.cpp).  Both return the number of key records changed
// and treat "0" as "nothing to do" (the undo ring stays untouched).
int ShiftBoneKeysForFrameLineInsert(MMDApp* app, std::uint32_t frame);
int ShiftBoneKeysForFrameLineDelete(MMDApp* app, std::uint32_t frame,
                                    bool autoInterp);
int InsertFacialLightFrameLine(MMDApp* app); // was Sub43B720, VA 0x0043B720
                                             //  insert frame line (facial/light)
int DeleteFacialLightFrameLine(MMDApp* app);  // was Sub43BB30, VA 0x0043BB30
                                              //  delete frame line (facial/light)
// The two bone/camera frame-line commands return the number of key records
// they moved (insert) / removed (delete); 0 means "nothing to do", so the
// I/K consumers can tell the two cases apart without inspecting the tables
// (defect 7: I/K on a frame that already carries a keyframe).

// ---- camera projection funnel (src/window/ui_viewport_refresh.cpp) -------
// Which projection the frame installs; the selector byte is
// cameraPerspective (x64 app+0x354, x86 app+0x31C).
enum class CameraFrameProjection { None, Perspective, Orthographic };
CameraFrameProjection CameraFrameProjectionFor(const MMDApp* app);
// Rebuilds the projection of the LIVE camera state on the device and returns
// which kind it installed.  Called every frame from SetupFrameWorldTransform
// so the visible field of view always equals state.cameraFov (defect 5) while
// the orthographic override (x64 0x140027E96) stays the only alternative.
CameraFrameProjection ApplyFrameCameraProjection(MMDApp* app);
// Perspective matrix of the live camera state (row-major float[16] in
// D3DXMatrixPerspectiveFovLH layout).  Pure - needs no device.
bool BuildCameraPerspectiveProjection(const MMDApp* app, float aspect,
                                      float out[16]);
// The 447 WM_HSCROLL body: state write + projection install.  Returns true
// when the value actually changed.
bool ApplyCameraFovSlider(MMDApp* app, int position);

// ---- Delete shortcut gate + selected-bone key removal -------------------
// (src/app/pump_edit_keys.cpp G8 / src/model/model_keyframe_edit.cpp)
// Pure precondition of the Delete shortcut, split from the window plumbing so
// the rule is testable without a window (defect 6).  `focusAcceptsKeys` is the
// focus half (main/floating window or a cleared focus, never a text entry -
// the same rule the letter ladder uses: FocusAllowsLetterHotkeys);
// `focusInFrameEdit` is the FRESH GetFocus probe against the five frame edits.
bool DeleteShortcutAllowed(const MMDApp* app, bool focusAcceptsKeys,
                           bool focusInFrameEdit) noexcept;
// Marks every bone-key record that sits exactly on `frame` and belongs to a
// bone selected in the model's boneSelection array (0x2D94 x86 / 0x3120 x64)
// so DeleteMarkedKeyframes removes it.  Returns the number of records marked;
// 0 means "nothing to delete" and leaves the tables untouched.
int MarkSelectedBoneKeysAtFrame(unsigned char* model, int frame);

// ---- DxOpenNI / Kinect loader (src/app/oni_kinect.cpp) ------------------
void OpenNiInit(MMDApp* app, const char* sjisPath);  // VA 0x00429CB0
void DisableKinect(MMDApp* app);                     // VA 0x0042A020
// ---- Kinect 骨架驱动泵块（src/app/oni_skeleton_pump.cpp）----------------
// x64 泵 0x7FF7CB44C12E..0x7FF7CB44C656（settle 门内、gate B 前），
// 由 frame_driver.cpp 在 PhysicsFrame 之前调用（见该处注释）。
void PumpKinectSkeleton(MMDApp* app, unsigned char selActive);
// 泵序言的 Kinect 探测后处理（x86 0x46DCCF..0x46DD61 / x64 0x7FF7CB44A34C..
// 0x7FF7CB44A3EC）：深度图绘制请求 + 菜单 0x124 使能/灰化维护，由
// frame_driver.cpp 的 selActive 探测块尾部调用。
void ManageKinectRecordGate(MMDApp* app, unsigned char selActive);
bool RegisterKinectPoseCapture(unsigned char* model, unsigned frame);  // sub_7FF7CB4F2EB0
bool RegisterTraceBoneKey(unsigned char* model, unsigned boneIdx,      // sub_7FF7CB4F23E0
                          int slot, int mode, unsigned frame);
// ---- VSQ loader / auto lipsync (src/io/vsq_load.cpp) --------------------
void LoadVsqFile(MMDApp* app, const wchar_t* path);  // was Sub435FE0, VA 0x00435FE0
// big-endian readers used by the SMF walkers (src/model/model_query_gaps.cpp)
int ReadBeWord(int fh, int* outVal, int nbytes);      // was Sub41A1A0, VA 0x0041A1A0
int ReadFixedString(int fh, char* out, int len);      // was Sub41A1F0, VA 0x0041A1F0
// (supersedes the LoadOniPlugin stand-in name used by earlier call sites)
void ShowWin32ErrorMessage(MMDApp* app, const char* context,
                           DWORD messageId);          // VA 0x0040E440

}  // namespace mikumikustudio
