#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/shadow_filter.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/image_sequence_export.hpp"
#include "mikumikustudio/default_effect.hpp"
#include "mikumikustudio/mmv4mms.hpp"
#include "mikumikustudio/plugin_menu.hpp"
#include "mikumikustudio/transparent_capture.hpp"
#include "mikumikustudio/wic_mov_recorder.hpp"
#include "mikumikustudio/native_media.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "device_lost_wait.hpp"
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/transparent_peeling.hpp"
#endif
// ===========================================================================
// VA 0x00462C40 - ShutdownCleanup  (original: sub_462C40, 0x6A8 bytes)
// ===========================================================================
// The full pre-free teardown chain WinMain (0x4C4460) drives after WM_QUIT,
// before operator delete(Block).  Original call order, all on this = app:
//
//   1. flag @+656312 -> indirect call through fn-ptr @+656324, clear flag
//   2. FreeLibrary(hmodule @+656316)
//   3. recording active (@+658724) -> TeardownDShowGraph(*(+657088))
//      (run-and-drain of the DirectShow graph; 0x409320 below)
//   4. separate window (@+658744) -> SaveFlagSubsystem (mic_window.cpp)
//   5. AVI background playback: AVIStreamGetFrameClose(+648196),
//      AVIStreamRelease(+648192), AVIFileRelease(+648188), AVIFileExit,
//      DrawDibClose(+648172) (the last one without a null guard)
//   6. free +652084; Release() on +655624/+655628/+651560; 11 toon-texture
//      slots at +650720 (kPtrToontex); +651568
//   7. free +650708 and +658300 (accessory records base)
//   8. Release() pairs +772/+768; render-side slots +650776/+650764/
//      +650784/+650772; +650120/+650124/+650112; AVI config +648180/
//      +648184/+648240/+648236/+648176
//   9. free the four buffers +884/+888/+892/+896
//  10. 255 accessory slots at +646512: DisposeAccessory (0x4C4700, real
//      body in src/render/accessory.cpp) then free; the twin track array
//      at +900 (kBufAcctrk) entries are just freed
//  11. 100 model slots at +1920: ModelDispose (0x48F830,
//      src/model/model_dispose.cpp) then free
//  12. accessory-record pool (+860, count +645680, stride 24): free
//      record +12 and +20, then the pool and the singles +868/+864/+856/
//      +872/+876/+880/+852/+848
//  13. free the seven config pointers +656400/+656408/+656368/+656376/
//      +656424/+656384/+656416
//  14. DeleteDC on +736/+724/+744 (no null guards)
//  15. Sub048 physics wrapper (+650672): DisposePhysicsWorld (0x4030F0)
//      then free; AxisMeshObject (+650656): DisposeAccessory then free;
//      Sub06C recorder (+657088): TeardownDShowGraphCoUninit (0x4096C0)
//      then free; Sub025C audio ctx (+204): DisposeAudioContext
//      (0x4C2C40) then free; Sub1D574 render wrapper (+657092):
//      DisposeRenderSubsystem (0x406BE0) then free
//  16. tail: nullsub_1(this + 652088) - empty function at 0x4D5AE0, no-op
//
// Bodies ported here (originals were __thiscall, this = sub-object):
//   0x00409320 TeardownDShowGraph - drain the running capture graph
//              (IMediaEvent::GetEvent loop + message pump) then Release()
//              every COM interface of the 0x6C recorder object
//   0x004096C0 TeardownDShowGraphCoUninit - 0x409320 + CoUninitialize
//   0x004030F0 DisposePhysicsWorld - physics world teardown through
//              public Bullet APIs (constraints, collision objects and owned
//              world components), matching src/model/model_dispose.cpp
//   0x004C2C40 DisposeAudioContext - CloseDataFile (0x4C2680) + Release()
//              of the two COM members + free of the two path buffers
//   0x00406BE0 DisposeRenderSubsystem - Release() run over the render
//              wrapper's interface slots, the 10000-entry locale table
//              (12-byte entries: free +4, Release +8) and, when stereo
//              was activated (+120166), 0x4CB4F0 on the NVAPI stereo
//              handle at +120020
//   0x004CB4F0 NvapiStereoDestroyHandle - NvAPI_Stereo_DestroyHandle
//              (QueryInterface id 974467380 = 0x3A153134)
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objbase.h>  // CoUninitialize
#include <dshow.h>    // IMediaControl/IMediaEvent complete types
#include <vfw.h>      // AVIFile*/AVIStream*/DrawDib*

#include <cstdint>
#include <cstdlib>
#include <new>

#include "btBulletDynamicsCommon.h"

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/dshow_recorder.hpp"
#include "mikumikustudio/recording_state.hpp"
#include <wrl/client.h>

#include "mikumikustudio/texture_lookup.hpp"

namespace mikumikustudio {

// 0x4C4700 real body lives in src/render/accessory.cpp (not yet declared
// in ported_funcs.hpp; the no-op stub for it in stubs.cpp is superseded).
void DisposeAccessory(void* accessory);

namespace {

// ---- helpers (same conventions as src/model/model_dispose.cpp) ------------
inline void** FieldPtr(unsigned char* base, std::size_t off) {
    return reinterpret_cast<void**>(base + off);
}

// IUnknown::Release is vtable slot 2 (byte +8); the call compiles to the
// original's `push obj; call [vtable+8]`.
inline void ReleaseField(unsigned char* base, std::size_t off) {
    void** p = FieldPtr(base, off);
    if (*p != nullptr) {
        reinterpret_cast<IUnknown*>(*p)->Release();      // vtable+8
        *p = nullptr;
    }
}

inline void FreeField(unsigned char* base, std::size_t off) {
    void** p = FieldPtr(base, off);
    if (*p != nullptr) {
        std::free(*p);
        *p = nullptr;
    }
}

// The timeline-selection record arrays are allocated with ::operator new in
// ui_editor_click.cpp (FillSelectionRecords and the accessory, ModelIk,
// ModelMorph, and ModelBone fill blocks), but this teardown freed them with
// the CRT free - the same idiom and the same deviation documented by the
// HandleLButtonUp release in ui_ctlcolor.cpp. ::operator delete matches the
// allocation; free() on storage from ::operator new is undefined behaviour
// even though the MSVC CRT heap happens to make it work. The slot is nulled
// immediately after, so this path and the drag-release path cannot free the
// same block twice.
// Defect B1-03.
inline void FreeTimelineSelectionRecords(MMDApp& app,
                                         TimelineSelectionBand band) {
    TimelineSelectionRecord*& records = app.TimelineSelectionRecords(band);
    if (records != nullptr) {
        ::operator delete(records);
        records = nullptr;
    }
}

// Same release for a typed wrapper (D3DRenderer) slot.  reinterpret_cast to
// IUnknown* keeps the forward-declared ID3DXEffect* member usable without
// including d3dx9.h - Release stays vtable slot 2 (byte +8), exactly like
// the raw-pointer ReleaseField above.
template <typename ComSlot>
inline void ReleaseSlot(ComSlot& slot) {
    if (slot != nullptr) {
        reinterpret_cast<IUnknown*>(slot)->Release();  // vtable+8
        slot = nullptr;
    }
}

template <typename ComSlot>
void ReleaseRecorderCom(ComSlot& slot) {
    if (slot != nullptr) {
        reinterpret_cast<IUnknown*>(slot)->Release();
        slot = nullptr;
    }
}

// ===========================================================================
// VA 0x00409320 - TeardownDShowGraph and 0x004096C0 -
// TeardownDShowGraphCoUninit now live at mikumikustudio:: scope below (the graph
// builder in src/app/dshow_record_graph.cpp calls the teardown too).
// =========================================================================//

// ===========================================================================
// VA 0x004030F0 - DisposePhysicsWorld  (original: sub_4030F0, __thiscall)
// ===========================================================================
// this = the 0x48-byte physics-scene wrapper at app+0x9EDB0 (kPtrSub048),
// restored as PhysicsScene (physics_scene.hpp); this->world (slot 64) is
// the btDynamicsWorld (same layout the model disposers walk - see
// model_dispose.cpp).  Slot map of the binary Bullet build: world vtable
// +80 = constraint count, +88 = constraint(i), +40 = removeConstraint,
// +20 = removeCollisionObject; rigid bodies are tagged +244==2 with motion
// state at +516 and collision shape at +204 in the x86 reference only.
// The implementation below uses the linked Bullet ABI on both architectures.
// =========================================================================//
void DisposePhysicsWorld(PhysicsScene* scene) {
    // Bullet private offsets and deleting-destructor vtable slots from the
    // x86 binary are not valid in the x64 build. Match ModelDispose's typed
    // ownership rules, retaining the original constraints -> bodies -> world
    // -> solver/broadphase/dispatcher/configuration destruction order.
    auto* world = scene->world;
    if (world != nullptr) {
        for (int i = world->getNumConstraints() - 1; i >= 0; --i) {
            auto* constraint = world->getConstraint(i);
            world->removeConstraint(constraint);
            delete constraint;
        }
        for (int i = world->getNumCollisionObjects() - 1; i >= 0; --i) {
            auto* object = world->getCollisionObjectArray()[i];
            if (auto* body = btRigidBody::upcast(object)) {
                delete body->getMotionState();
                delete body->getCollisionShape();
            }
            world->removeCollisionObject(object);
            delete object;
        }
    }
    scene->groundBody = nullptr;
    delete scene->world;
    scene->world = nullptr;
    delete scene->solver;
    scene->solver = nullptr;
    delete scene->broadphase;
    scene->broadphase = nullptr;
    delete scene->dispatcher;
    scene->dispatcher = nullptr;
    delete scene->collisionConfig;
    scene->collisionConfig = nullptr;
    // kReleaseOffsets {36, 4, 12, 20, 28, 8, 16, 24, 32, 40} - the ten
    // gizmo buffers, Released and nulled in the original's order.
    ReleaseSlot(scene->gizmoBoxSelVB);    // 36
    ReleaseSlot(scene->gizmoSphereVB);    // 4
    ReleaseSlot(scene->gizmoCubeVB);      // 12
    ReleaseSlot(scene->gizmoSphere33VB);  // 20
    ReleaseSlot(scene->gizmoArrowVB);     // 28
    ReleaseSlot(scene->gizmoSphereIB);    // 8
    ReleaseSlot(scene->gizmoCubeIB);      // 16
    ReleaseSlot(scene->gizmoSphere33IB);  // 24
    ReleaseSlot(scene->gizmoIdentityIB);  // 32
    ReleaseSlot(scene->gizmoBoxSelIB);    // 40
}

// ===========================================================================
// VA 0x004C2C40 - DisposeAudioContext  (original: sub_4C2C40, __thiscall)
// ===========================================================================
// this = the 0x25C audio/data context at app+0xCC (kPtrSub025c).  Waits out
// any in-flight async read (inside CloseDataFile), closes the stream and
// thread handle, releases the two COM members at +20/+16 (no nulling), then
// frees the two heap buffers at +0/+4 (no nulling) - exactly like the
// original, which leaves the object fields dangling for the caller's free().
// =========================================================================//
void DisposeAudioContext(WaveAudioContext* audio) {
    CloseDataFile(audio);                                       // 0x4C2C43
    media::ReleaseAudio(audio);
    if (audio->streamingBuffer != nullptr)
        audio->streamingBuffer->Release();                      // 0x4C2C57
    if (audio->directSound != nullptr)
        audio->directSound->Release();                          // 0x4C2C68
    std::free(audio->waveformMax);                              // 0x4C2C71
    std::free(audio->waveformMin);                              // 0x4C2C81
}

// ===========================================================================
// VA 0x004CB4F0 - NvapiStereoDestroyHandle  (original: sub_4CB4F0, __cdecl)
// ===========================================================================
// NvAPI_Stereo_DestroyHandle, resolved through nvapi_QueryInterface with id
// 974467380 (0x3A153134).  The original caches the resolved pointer in the
// .data globals @0x5425A8/@0x5425AC and consults the init-time pointer
// @0x545944 (set by the 0x4C6940 nvapi init chain); the port resolves
// lazily through the same LoadLibrary("nvapi.dll") path used by
// src/render/stereo_nvapi.cpp.  The two conditional trace hooks
// (dword_545948/dword_54594C) are always-null instrumentation in the
// original and are omitted.
// =========================================================================//

// NVAPI interface id resolved through nvapi_QueryInterface (opaque
// selector, kept in hex).
constexpr std::uint32_t kNvapiStereoDestroyHandleId = 0x3A153134u;

int NvapiStereoDestroyHandle(void* stereoHandle) {
    using QueryInterface = void*(__cdecl*)(std::uint32_t);
    using DestroyHandle = int(__cdecl*)(void*);
    static DestroyHandle destroy = nullptr;   // mirrors 0x5425A8 cache
    static bool resolved = false;             // mirrors 0x5425AC flag
    if (!resolved) {
        resolved = true;
        // arch-split library name, as in stereo_nvapi.cpp (x64 original:
        // sub_7FF7CB4FEA80 loads "nvapi64.dll")
        HMODULE module = LoadLibraryA(sizeof(void*) == 8 ? "nvapi64.dll"
                                                         : "nvapi.dll");
        if (module != nullptr) {
            auto query = reinterpret_cast<QueryInterface>(
                GetProcAddress(module, "nvapi_QueryInterface"));
            if (query != nullptr)
                destroy = reinterpret_cast<DestroyHandle>(
                    query(kNvapiStereoDestroyHandleId));
        }
    }
    if (destroy == nullptr)
        return -3;                                              // 0x4CB536
    return destroy(stereoHandle);                               // 0x4CB579
}

// ===========================================================================
// VA 0x00406BE0 - DisposeRenderSubsystem  (original: sub_406BE0, __thiscall)
// ===========================================================================
// this = the 0x1D574 render/locale wrapper at app+0xA06C4 (kPtrSub1d574),
// restored as D3DRenderer (d3d_wrapper.hpp).  Member names below carry the
// original x86 offsets:
//   * Release() run over the interface slots effect(+120160), shadowSurface
//     (+120144), hdrTexture(+120136), spriteTexture(+120140),
//     shadowDepthSurface(+120148), depthStencilSurface(+120124),
//     backbufferSurface(+120120), captureSurface(+120116), lineVertexBuffer(+120052),
//     device(+120032), d3d9(+120028) - the same slots Sub1D574Init seeds;
//   * the 10000-entry resource pool at +4 (12-byte entries): free the
//     heapBuffer (+4) member, Release() the comObject (+8) member;
//   * if stereo was activated (byte +120166, probed by 0x406E18..0x406E42),
//     destroy the NVAPI stereo handle stored at +120020 (0x4CB4F0).
// =========================================================================//
void DisposeRenderSubsystem(D3DRenderer* sub) {
    shadow_filter::ReleaseRenderer(sub, true);
    CancelDefaultEffectCompilation(sub);
    InvalidateTextureLookup();
#if !MMD_ORIGINAL_PROFILE
    ClearTransparentPeelingCache();
#endif
    ReleaseSlot(sub->effect);             // +120160  0x406BF8
    ReleaseSlot(sub->shadowSurface);      // +120144
    ReleaseSlot(sub->hdrTexture);         // +120136
    ReleaseSlot(sub->spriteTexture);      // +120140
    ReleaseSlot(sub->shadowDepthSurface); // +120148
    ReleaseSlot(sub->depthStencilSurface);// +120124
    ReleaseSlot(sub->backbufferSurface);  // +120120
    ReleaseSlot(sub->captureSurface);     // +120116
    ReleaseSlot(sub->lineVertexBuffer);            // +120052
    ReleaseSlot(sub->device);             // +120032
    ReleaseSlot(sub->d3d9);               // +120028  0x406CE8

    for (int k = 0; k < 10000; ++k) {    // 0x406CF3
        ResourcePoolEntry& entry = sub->resourcePool[k];
        if (entry.heapBuffer != nullptr) {   // +4        0x406D00
            std::free(entry.heapBuffer);
            entry.heapBuffer = nullptr;
        }
        if (entry.comObject != nullptr) {    // +8        0x406D17
            entry.comObject->Release();      // vtable+8
            entry.comObject = nullptr;
        }
    }

    if (sub->stereoEnabled != 0)         // +120166  0x406D23
        NvapiStereoDestroyHandle(sub->stereoHandle);  // +120020  0x406D32
}

}  // namespace

// ===========================================================================
// VA 0x00409320 - TeardownDShowGraph  (original: sub_409320, __thiscall)
// ===========================================================================
// this = the 0x6C recorder object allocated at app+0xA06C0 (see
// src/app/dshow_record_graph.cpp for the full member map; dword indices
// this[n] = byte offset 4*n).  Runs only when a graph was built.
//   * this[26] (MMDxShow frame-push interface): vtable+24 call
//   * if this[23] (IMediaEvent) exists: GetEvent/FreeEventParams spin
//     (vtable+32 / +48) until an event code in 1..3 (EC_COMPLETE ..
//     EC_ERRORABORT) arrives, pumping PeekMessage/Translate/Dispatch
//     meanwhile; then this[17] (IMediaControl) vtable+36 (Stop)
//   * Release() (vtable+8) and null, in the original's exact order, over
//     this[23],[17],[20],[21],[22],[19],[18],[17] (again),[16],[15],[14],
//     [13],[12],[11],[10],[9],[8],[7],[6],[5],[26],[4],[3],[1],[0];
//     free(this[2]) between [3] and [1].
// At mikumikustudio:: scope (not the anonymous namespace above) because the graph
// builder in src/app/dshow_record_graph.cpp tears down on every failure
// path exactly like the original.
// =========================================================================//
bool TeardownDShowGraph(DShowRecorder* rec, bool* completed) {
    if (completed) *completed = false;
    recording_state::Guard stopping(rec, recording_state::Operation::GraphTeardown);
    if (!stopping) return false;
    AbortPngMovRecording(rec);
    bool didComplete = false;
    // 0x409327: MMDxShow frame-push interface, vtable slot +24
    bool awaitEnd = false;
    if (IPushSource* push = static_cast<IPushSource*>(rec->framePush))
        awaitEnd = SUCCEEDED(push->BeginStreaming());

    Microsoft::WRL::ComPtr<IMediaEvent> mediaEvent = rec->mediaEvent;
    Microsoft::WRL::ComPtr<IMediaControl> mediaControl = rec->mediaControl;
    if (mediaEvent && awaitEnd) {
        // 0x40936C: GetEvent(&code,&p1,&p2,0) / 0x409386: FreeEventParams
        long code = 0;
        LONG_PTR param1 = 0;
        LONG_PTR param2 = 0;
        bool done = false;
        const auto began = GetTickCount64();
        do {
            if (SUCCEEDED(mediaEvent->GetEvent(&code, &param1, &param2, 0))) {
                mediaEvent->FreeEventParams(code, param1, param2);
                if (code > 0 && code <= 3) {
                    done = true;
                    didComplete = code == EC_COMPLETE;
                }
            }
            MSG msg;                                           // 0x4093A4
            for (unsigned count = 0; count < 64 && PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE); ++count) {
                if (msg.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(msg.wParam)); done = true; break;
                }
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (!done && GetTickCount64() - began >= 120000) done = true;
            if (!done) MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } while (!done);
    }
    if (mediaControl) mediaControl->Stop();

    // 0x4093E9..0x4095BC: release run (Release() = vtable+8, then null).
    // this[17] appears twice like the original (the second pass sees the
    // null written by the first and is a no-op).  free(this[2]) sits
    // between the Release() of this[3] and this[1] - kept in place.
    ReleaseRecorderCom(rec->mediaEvent);
    ReleaseRecorderCom(rec->mediaControl);
    ReleaseRecorderCom(rec->audioGrabber);
    ReleaseRecorderCom(rec->audioGrabberInput);
    ReleaseRecorderCom(rec->audioGrabberOutput);
    ReleaseRecorderCom(rec->waveOutput);
    ReleaseRecorderCom(rec->waveSource);
    ReleaseRecorderCom(rec->mediaControl);  // original releases this slot twice
    ReleaseRecorderCom(rec->muxAudioInput);
    ReleaseRecorderCom(rec->reservedInput1);
    ReleaseRecorderCom(rec->reservedInput0);
    ReleaseRecorderCom(rec->muxVideoInput);
    ReleaseRecorderCom(rec->compressorOutput);
    ReleaseRecorderCom(rec->compressorInput);
    ReleaseRecorderCom(rec->grabberOutput);
    ReleaseRecorderCom(rec->grabberInput);
    ReleaseRecorderCom(rec->sourceOutput);
    ReleaseRecorderCom(rec->fileWriter);
    ReleaseRecorderCom(rec->aviMux);
    ReleaseRecorderCom(rec->videoGrabber);
    ReleaseRecorderCom(rec->framePush);
    ReleaseRecorderCom(rec->videoSource);
    ReleaseRecorderCom(rec->graph);
    std::free(rec->compressorState);        // 0x40958F: free(this[2])
    rec->compressorState = nullptr;
    ReleaseRecorderCom(rec->compressorDialogs);
    ReleaseRecorderCom(rec->compressor);
    if (completed != nullptr)
        *completed = didComplete;
    return didComplete;
}

// ===========================================================================
// VA 0x004096C0 - TeardownDShowGraphCoUninit  (original: sub_4096C0)
// =========================================================================//
void TeardownDShowGraphCoUninit(DShowRecorder* rec) {
    TeardownDShowGraph(rec);                                    // 0x4096C0
    CoUninitialize();                                           // 0x4096C5
}

// ===========================================================================
// VA 0x00462C40 - ShutdownCleanup  (original: sub_462C40, __thiscall, app)
// =========================================================================//
void ShutdownCleanup(MMDApp* app) {
    viewport_render_scale::Release(app);
    antialias::Release(app);
    autosave::Shutdown();
    device_lost_wait::Forget(app);
    AbortPngMovRecording(app->Recorder());
    image_sequence::Abort(app);
    ReleaseTransparentCapture(app);
    mmv4mms::Shutdown();
    plugin_menu::Shutdown(app);
    native_mesh::Shutdown();
    ShutdownD3D12Native();
    skinning::Shutdown();
    auto& s = *app;

    // ---- 1/2: flag-gated callback + module unload ------------------------
    if (s.state.depthDeviceEnabled != 0) {        // 0x462C6F
        reinterpret_cast<void(*)()>(
            s.state.oniExportSlot1)();                                   // 0x462C81
        s.state.depthDeviceEnabled = 0;
    }
    if (HMODULE mod = s.state.oniModule) {                          // 0x462C89
        FreeLibrary(mod);                                       // 0x462C94
        s.state.oniModule = nullptr;
    }

    // ---- 3/4: capture graph + separate ("Mic") window --------------------
    if (s.RecordingWindow() != nullptr)                         // 0x462CA0
        TeardownDShowGraph(s.Recorder());                        // 0x462CAE
    if (s.FloatingWindow() != nullptr)                           // 0x462CB3
        SaveFlagSubsystem(app);                                 // 0x462CBD

    // ---- 5: AVI background playback handles ------------------------------
    media::ReleaseVideo(app);
    if (s.AviFrameReader() != nullptr)                           // 0x462CC2
        AVIStreamGetFrameClose(                                 // 0x462CCD
            static_cast<PGETFRAME>(s.AviFrameReader()));
    if (s.AviStream() != nullptr)                                // 0x462CD2
        AVIStreamRelease(                                       // 0x462CDD
            static_cast<PAVISTREAM>(s.AviStream()));
    if (s.AviFile() != nullptr)                                  // 0x462CE2
        AVIFileRelease(                                         // 0x462CED
            static_cast<PAVIFILE>(s.AviFile()));
    AVIFileExit();                                              // 0x462CF2
    DrawDibClose(static_cast<HDRAWDIB>(s.AviDrawDib()));        // 0x462CFE

    // ---- 6: toon texture slots and the first Release() run ---------------
    ::operator delete(s.CaptureReadbackPixels());               // 0x462D0E
    s.CaptureReadbackPixels() = nullptr;
    ReleaseSlot(s.LeftViewportVertices());                     // 0x462D2C
    ReleaseSlot(s.RightViewportVertices());                    // 0x462D44
    ReleaseSlot(s.GroundPlaneVertices());                       // 0x462D5C
    for (int i = 0; i < 11; ++i)                                // 0x462D64
        ReleaseSlot(s.ToonTexture(i));                          // 0x462D7C
    ReleaseSlot(s.ProjectedShadowRestoreTexture());             // 0x462D98

    // ---- 7: accessory-record base + misc frees ----------------------------
    delete s.RecordingCompletionFlag();                         // 0x462DAB
    s.RecordingCompletionFlag() = nullptr;
    if (s.state.rigidScratchArray != nullptr) {                   // 0x462DC4
        std::free(s.state.rigidScratchArray);
        s.state.rigidScratchArray = nullptr;
    }

    // ---- 8: render-side and AVI-config Release() run ----------------------
    ReleaseSlot(s.GroundGridIndices());                         // 0x462DE2
    ReleaseSlot(s.GroundGridVertices());                        // 0x462DFA
    ReleaseSlot(s.OverlayVertices());                           // 0x462E12
    ReleaseSlot(s.SpriteOverlayVertices());                     // 0x462E2A
    ReleaseSlot(s.SceneFontTexture());                          // 0x462E42
    ReleaseSlot(s.OverlayTexture());                            // 0x462E5A
    ReleaseSlot(s.CaptureRenderTarget());                       // 0x462E72
    ReleaseSlot(s.CaptureSystemSurface());                      // 0x462E8A
    ReleaseSlot(s.CaptureTexture());                            // 0x462EA2
    ReleaseSlot(s.AviBackgroundSurface());                      // 0x462EBA
    ReleaseSlot(s.AviOverlayVertices());                        // 0x462ED2
    ReleaseSlot(s.PictureOverlayVertices());                    // 0x462EEA
    ReleaseSlot(s.PictureBackgroundTexture());                  // 0x462F02
    if (s.AviBackgroundTexture() != nullptr) {                   // 0x462F1A
        s.AviBackgroundTexture()->Release();
        s.AviBackgroundTexture() = nullptr;
    }

    // ---- 9: four global keyframe tracks -----------------------------------
    // The four tracks are allocated with ::operator new followed immediately
    // by a memset (ui_init.cpp:60-63 identifies the VC9 j_??2@YAPAXI@Z thunk
    // of the original; reset_app_state.cpp:317-336 uses the same pairing), but
    // the teardown released them through the CRT free.  Calling free() on
    // storage obtained from ::operator new is undefined behaviour, even though
    // the MSVC CRT heap happens to make it work; the release now matches the
    // allocation.  The x64 reference likewise releases these four adjacent
    // slots with a delete form (operator delete[] at
    // 0x14000C03F..0x14000C08A).  Each slot is set to null right after, so the
    // scene-teardown twin in scene_ownership.cpp cannot release the same block
    // twice.
    // Defect AB-06.
    mdl::KeyPoolDestroy(s.CameraKeys());                          // 0x462F2D
    s.CameraKeys() = nullptr;
    mdl::KeyPoolDestroy(s.LightKeys());                           // 0x462F46
    s.LightKeys() = nullptr;
    mdl::KeyPoolDestroy(s.ShadowKeys());                          // 0x462F5F
    s.ShadowKeys() = nullptr;
    mdl::KeyPoolDestroy(s.GravityKeys());                         // 0x462F78
    s.GravityKeys() = nullptr;

    // ---- 10: 255 accessory slots + twin track array ------------------------
    for (int i = 0; i < 255; ++i) {                             // 0x462F8C
        mdl::AccessoryRecord*& accessory = s.AccessorySlot(i);  // 0x462F94
        if (accessory != nullptr) {
            // The record block is allocated with ::operator new(sizeof(
            // mdl::AccessoryRecord), std::nothrow) (accessory.cpp:870, also in
            // window_init.cpp and both PMM loaders), and DeleteAccessory
            // releases it with ::operator delete (accessory.cpp:837-842). The
            // CRT free used here was therefore undefined behaviour on that
            // storage, even though the MSVC CRT heap happens to make it work.
            // The x64 reference releases the same block with a scalar
            // operator delete after the record's own buffers (0x14000C109).
            // Defect AB-08.
            DisposeAccessory(accessory);                        // 0x462F9C
            ::operator delete(accessory);                       // 0x462FA2
            accessory = nullptr;
        }
        // The 255 accessory track arrays come from the same ::operator new
        // wrapper as the four global tracks (ui_init.cpp:60-63 and
        // reset_app_state.cpp:337-343); the x64 reference releases each of
        // them with a delete form (operator delete[] at 0x14000C11E inside
        // the 255-slot loop).
        // Defect AB-06.
        mdl::AccessoryKey*& track = s.AccessoryKeys(i);
        if (track != nullptr) {
            mdl::KeyPoolDestroy(track);                           // 0x462FB7
            track = nullptr;
        }
    }

    // ---- 11: model slots (x64 walks 255; x86 twin 0x462FD5 walks 100) ----
    for (int i = 0; i < kModelSlotCount; ++i) {                // 0x462FD5
        unsigned char*& model = s.ModelSlot(i);
        if (model != nullptr) {                                 // 0x462FE0
            ModelDispose(model);                                // 0x462FE9
            std::free(model);                                   // 0x462FEF
            model = nullptr;
        }
    }

    // ---- 12: clipboard arrays and singles ----------------------------------
    mdl::DisplayClipboardRecord*& displayRecords = s.DisplayClipboard();
    if (displayRecords != nullptr) {
        std::int32_t nRec =
            static_cast<std::int32_t>(s.ClipboardCounts().displays);
        if (nRec > 0) {                                         // 0x46300E
            for (std::int32_t k = 0; k < nRec; ++k) {           // 0x46306E
                std::free(displayRecords[k].ikStates);          // 0x46302F
                displayRecords[k].ikStates = nullptr;
                std::free(displayRecords[k].selectorStates);    // 0x463050
                displayRecords[k].selectorStates = nullptr;
            }
        }
    }
    std::free(displayRecords);                                  // 0x46307B
    displayRecords = nullptr;
    std::free(s.LightClipboard());                              // 0x463094
    s.LightClipboard() = nullptr;
    std::free(s.CameraClipboard());                             // 0x4630AD
    s.CameraClipboard() = nullptr;
    std::free(s.MorphClipboard());                              // 0x4630C6
    s.MorphClipboard() = nullptr;
    std::free(s.ShadowClipboard());                             // 0x4630DF
    s.ShadowClipboard() = nullptr;
    std::free(s.GravityClipboard());                            // 0x4630F8
    s.GravityClipboard() = nullptr;
    std::free(s.AccessoryClipboard());                          // 0x463111
    s.AccessoryClipboard() = nullptr;
    std::free(s.BoneClipboard());                               // 0x46312A
    s.BoneClipboard() = nullptr;
    std::free(s.BoneCopyRecords());                               // 0x463143
    s.BoneCopyRecords() = nullptr;

    // ---- 13: selection-record buffers -------------------------------------
    // The original releases seven of the eight timeline-selection buffer slots
    // here (0x46315C..0x4631F2); those seven addresses are exactly the set that the
    // HandleLButtonUp drag-release block uses (0x44A9BF..0x44AA68, see
    // ui_ctlcolor.cpp).  The Gravity band's slot appears in neither list, even
    // though the selection pass fills it whenever the global-track option is
    // enabled (ui_editor_click.cpp, 0x448EEB..0x44A437), so every box-select
    // leaked that buffer (selected keys x 16 bytes).  Deliberate deviation from
    // the original, which has the same seven-address omission: the port
    // releases the missing eighth slot.  The helper nulls each slot, so this
    // path and the drag-release path cannot release the same block twice.
    // Defect B1-02.
    FreeTimelineSelectionRecords(s, TimelineSelectionBand::Accessory);   // 0x46315C
    FreeTimelineSelectionRecords(s, TimelineSelectionBand::ModelIk);     // 0x463175
    FreeTimelineSelectionRecords(s, TimelineSelectionBand::Camera);      // 0x46318E
    FreeTimelineSelectionRecords(s, TimelineSelectionBand::Light);       // 0x4631A7
    FreeTimelineSelectionRecords(s, TimelineSelectionBand::ModelBone);   // 0x4631C0
    FreeTimelineSelectionRecords(s, TimelineSelectionBand::SelfShadow);  // 0x4631D9
    FreeTimelineSelectionRecords(s, TimelineSelectionBand::ModelMorph);  // 0x4631F2
    FreeTimelineSelectionRecords(s, TimelineSelectionBand::Gravity);

    // ---- 14: GDI DCs (no null guards in the original) -----------------------
    DeleteDC(s.TimelineDC());                                   // 0x46320D
    DeleteDC(s.PanelDC());                                      // 0x463216
    DeleteDC(s.CurveDC());                                      // 0x46321F

    // ---- 15: subsystem objects ----------------------------------------------
    if (PhysicsScene* phys = s.Physics()) {                    // 0x463221
        DisposePhysicsWorld(phys);                              // 0x46322D
        std::free(phys);                                        // 0x463233
        s.Physics() = nullptr;
    }
    if (void* acc = s.AxisMeshObject()) {                              // 0x463241
        DisposeAccessory(acc);                                  // 0x46324D
        std::free(acc);                                         // 0x463253
        s.AxisMeshObject() = nullptr;
    }
    if (DShowRecorder* rec = s.Recorder()) {                    // 0x463261
        TeardownDShowGraphCoUninit(rec);                         // 0x46326D
        std::free(rec);                                         // 0x463273
        s.Recorder() = nullptr;
    }
    if (WaveAudioContext* audio = s.Audio()) {                  // 0x463281
        DisposeAudioContext(audio);                             // 0x46328D
        std::free(audio);                                       // 0x463293
        s.Audio() = nullptr;
    }
    if (D3DRenderer* render = s.Renderer()) {                   // 0x4632A1
        DisposeRenderSubsystem(render);                         // 0x4632AD
        std::free(render);                                      // 0x4632B3
        s.Renderer() = nullptr;
    }

    // 0x4632CF: nullsub_1(this + 652088) - empty function at 0x4D5AE0,
    // (the inline font sub-object at kBufFontsub); intentionally nothing.
}

}  // namespace mikumikustudio
