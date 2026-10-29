#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/key_pool.hpp"
// ===========================================================================
// VA 0x00446A70 - HandleLButtonDown  (original: sub_446A70)
// ===========================================================================
// WM_LBUTTONDOWN handler for the model-editor panel (sole caller: WndProc
// 0x4C3A10 @ 0x4C412D).  The largest UI function (~0x3F27 bytes, 3591
// instructions).  this+4 / this+8 hold the click X / Y coordinates.
//
// Click regions (gates computed once up front):
//   var_190 = Y > 160 ; var_1A0 = Y < clientBottom - 0xF8 (248)
//   A. Margin column  X in (4, 20):    row-type display-flag toggle
//      (model[0x26D0] table, 0x65-stride, flag at +0x64) + PostLanguageSweep
//   B. Name column    X in (20, 95]:  bone/morph row selection + keyframe
//      insert (0x4A1510) - display-mode sub-branch when byte 0x2F8 set
//   C. Main area      X in (95, sidebar-18) && Y gates: selection bitmap
//      clear across all 100 model slots, scrollbar 0x1A1 sync, per-slot
//      frame apply (0x4B4260 / 0x4A02C0), mode branch on byte 0x2F8,
//      scroll clamp, PanelPaint (0x414610), timeline strip redraw
//      (0x4C2A00 + InvalidateRect), physics sync (0x4C2B80 / 0x4C3530)
//   D. else: bone/morph name-row rect hit test (rects derived from bytes
//      0x9DA05/0x9DA06 and 0x9DA07/0x9DA08 + client bottom) -> keyframe
//      insert + selection-row marker 0x9DA09 = 1 (bone) / 2 (morph)
//
// Every path funnels into the shared tail (dirty flag 0xA0189, click anchor
// 0xA018C/0xA0190) and the selection-count re-evaluation (0xA03EC region,
// 0x40 dwords, per-model flag counting + qsort + 0x49D410 feed).
//
// Layout: HandleLButtonDown keeps the S1 gates and dispatches to one
// file-local sub-function per click region; every original `goto L_tail`
// exit became a plain return, and the L_rowtype forward jump inside the
// name column is kept verbatim:
//   HandleLButtonDown_MarginColumnToggle   S2    0x446B01-0x446B9C
//   HandleLButtonDown_NameColumnHit        S3+S4 0x446BA1-0x44738F
//   HandleLButtonDown_TimelineAreaHit      S5-S6 0x447390-0x448EEB
//   HandleLButtonDown_FrameStripHit        S7    0x44A468-0x44A850
//   HandleLButtonDown_NameRowRectHit       S7b   0x44A855-0x44A97D
//
// Reference: ../translated/MikuMikuDance/fcn_00446a70.cpp
//   NOTE: the translated file is a register-tracking re-render that deviates
//   in places (coordinate mapping, message ids, helper arg lists); this port
//   follows the IDA decompilation of MikuMikuDance.exe 0x446A70.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/bone_scale.hpp"
#include <Windows.h>
#include <commctrl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"

namespace mikumikustudio {

// Forward declarations for functions ported in other translation units
// (declared here with their original VAs; bodies in the files noted).
void RefreshAfterFrameApply(MMDApp* app);
void PanelPaint(MMDApp* app);                                   // VA 0x00414610 (ui_panel_paint.cpp)
void PostLanguageSweep(MMDApp* app);                            // VA 0x0042F1E0 (ui_panel_sweep.cpp)
void TimelineDrawTicks(int frameOffset, int width);             // VA 0x004C2A00 (ui_timeline_gfx.cpp)
void SetFrameNormalized(int frame);                             // VA 0x004C2B80 (ui_timeline_gfx.cpp)

// Editor-panel refresh helpers, ported in other translation units.
void RefreshLightPanel(MMDApp* app);                                    // VA 0x00411070 (ui_frame_refresh.cpp)
void RefreshSelfShadowPanel(MMDApp* app);                                    // VA 0x00411B90 (ui_frame_refresh.cpp)
void ApplyGravityTrack(MMDApp* app);                                    // VA 0x00412330 (track_apply.cpp)
void SyncAccessoryEditPanel(MMDApp* app);                                    // VA 0x004134E0 (ui_frame_refresh.cpp)
void ApplyAccessoryTrack(MMDApp* app, int idx);                           // VA 0x00413120 (accessory_paste.cpp)
void SnapshotPoseBeforeFrameChange(unsigned char* model, int frame);  // VA 0x004A0080, was Sub4A0080 (model_keyframe_edit.cpp)
void SyncModelEditControls(unsigned char* model);                     // VA 0x004A02C0, was Sub4A02C0 (model_keyframe_edit.cpp)
void SnapshotSelectedKeysForUndo(unsigned char* model, int frame);    // VA 0x004A1510, was Sub4A1510 (model_keyframe_edit.cpp)
int SeekModelFrame(unsigned char* model, int frame, int physicsMode);  // VA 0x004B4260
void WaveRestartAt(void* sub, double v);                       // VA 0x004C3530, was Sub4C3530
void AviBgOverlayRefresh(MMDApp* app);                                    // VA 0x004168D0 (bg_overlay.cpp)
void AppendBoneKeyToUndo(unsigned char* model, int index);           // VA 0x0049D410, was Sub49D410 (key_registrars.cpp)
// Same, with the visit map the caller resolved once for a per-record sweep.
void AppendBoneKeyToUndo(unsigned char* model, int index,
                         unsigned char* visited);
void PostLanguageSweep2(MMDApp* app);  // VA 0x0040D070 (ui_view_refresh.cpp);
                                       // was declared here as Sub40D070 - same
                                       // function, the local twin was removed
void SelectionReeval(MMDApp* app);                              // VA 0x00430510 (ui_selection_reeval.cpp)

// qsort comparator of the selection-stat records - original VA 0x0040EC70:
//   v2 = a1[3]; v3 = a2[3]; return (v2 >= v3) ? (v2 > v3) : -1;
// (compares the 4th dword of each 16-byte record; -1 when a1 < a2,
//  (v2>v3) when a1 >= a2 - so equal records return 0)
int CompareFunction(const void* a, const void* b) {
    const std::int32_t keyA = static_cast<const std::int32_t*>(a)[3];
    const std::int32_t keyB = static_cast<const std::int32_t*>(b)[3];
    if (keyA >= keyB)
        return keyA > keyB;
    return -1;
}

// ---- file-local helpers (extracted from sub_446A70, original VAs in the
//      comments) -----------------------------------------------------------

// Signed division by 13 (original magic 0x4EC4EC4F, sar 2) - used for the
// (coord - 100) / 13 row indexing.  (sub_446A70 @ 0x4473F8 etc.)
static int Div13(int v) {
    return v / 13;
}

// Signed division by 14 (original magic 0x92492493, sar 3) - used for the
// (coord - 160) / 14 and (coord - 216) / 14 row indexing.  (sub_446A70 @
// 0x446B5C, 0x446CA2, 0x4472E7)
static int Div14(int v) {
    return v / 14;
}

// Bone-list row -> bone index.  The label column keeps one record per drawn
// line (model+0x3224, 200 entries): the 20-byte display records of a bone row
// carry the bone index, morph rows carry -1-morphIndex and rows that were
// never painted keep the -999 sentinel.  The click handler must therefore
// treat "record in [0, boneCount)" as a bone row and everything else as "no
// bone on this row" - reading past boneCount indexes boneSelection /
// selectedBone outside their allocation (the original has no such guard
// because only painted rows are hit; the port is defensive and the guard is
// what makes the mapping testable).
//   Original: 0x446C8A / x64 0x7FF7CB459F00 (`movsxd rdx,[r8+rax*4+0x3224]`
//   then `test edx,edx / js` to the -999 morph path).
int BoneListRowBoneIndex(const mikumikustudio::mdl::ModelRecord* record,
                         std::int32_t row) {
    if (record == nullptr)
        return -1;
    if (row < 0 || row >= 200)  // the row table is 200 entries wide
        return -1;
    const std::int32_t idx = record->boneListRowRecord[row];
    if (idx < 0)  // -999 sentinel or a -1-morph row
        return -1;
    if (static_cast<std::uint32_t>(idx) >= record->boneCount)
        return -1;  // stale record (e.g. left over from a previous model)
    return idx;
}

// Same row, but only the -1-morph records are of interest: returns the morph
// index encoded in the record (-1 - record) or -1 when the row is not a morph
// row.  A -999 record encodes want == 998, which never matches a real morph,
// exactly like the original's `or r11,-1 / sub r11d,edx` fall-through.
std::int32_t BoneListRowMorphIndex(const mikumikustudio::mdl::ModelRecord* record,
                                   std::int32_t row) {
    if (record == nullptr)
        return -1;
    if (row < 0 || row >= 200)
        return -1;
    const std::int32_t idx = record->boneListRowRecord[row];
    if (idx >= 0 || idx == -999)
        return -1;
    return -1 - idx;
}

// Active model pointer = slot[byte this+0x910] of the 100-slot array at
// this+0x780.  (sub_446A70 pattern @ 0x446B48, re-read every use like the
// original reloads it in every loop iteration)
static unsigned char* ActiveModel(MMDApp* app) {
    return app->SelectedModel();
}

static unsigned char* ModelAtSlot(MMDApp* app, std::size_t slot) {
    return app->ModelSlot(static_cast<int>(slot));
}

// Byte-wise string equality used for the combo-list matching (original
// inline 2-byte-step loop @ 0x446F01 / 0x446F50).
static bool MatchText(const char* a, const char* b) {
    while (*a != '\0') {
        if (*a != *b)
            return false;
        ++a;
        ++b;
    }
    return *b == '\0';
}

// Clear the four selection-bitmap arrays pointed to by this+0x374 (rigid,
// 0x54-stride, flag +0x48), this+0x378 (joint, 0x28-stride, flag +0x24),
// this+0x37C (IK, 0x18-stride, flag +0x14), this+0x380 (morph, 0x24-stride,
// flag +0x21) - 10000 records each - plus the 255-slot record table at
// this+0x384 (0x3C-stride, flag +0x18, 10000 records per slot).
// (sub_446A70 @ 0x447460 / 0x447580 / 0x4476A0 / 0x4477C0 / 0x447900 /
//  0x4479E0 / 0x447A30)
static void ClearSelectionBitmaps(MMDApp* app) {
    negative_frames::SelectCell(app,0,-999,false);
    for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(app->CameraKeys()); i < capacity; ++i)
        app->CameraKeys()[i].selected = 0;
    for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(app->LightKeys()); i < capacity; ++i)
        app->LightKeys()[i].selected = 0;
    for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(app->ShadowKeys()); i < capacity; ++i)
        app->ShadowKeys()[i].selected = 0;
    for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(app->GravityKeys()); i < capacity; ++i)
        app->GravityKeys()[i].selected = 0;
    for (int s = 0; s < 0xFF; ++s) {
        auto* rec = reinterpret_cast<unsigned char*>(app->AccessoryKeys(s));
        if (rec == nullptr)
            continue;
        // Original termination is byte offset 0x927C0 (0x44781E,
        // 0x44795E, 0x447A3E).  Dividing by the 0x3C record stride gives
        // exactly 0x2710 records.  The old 0x3D80 loop wrote 344,640 bytes
        // beyond every 600,000-byte accessory table and made a timeline
        // key click corrupt the neighbouring heap allocations.
        for (int i = 0, poolCapacity = static_cast<int>(mdl::KeyPoolCapacity(app->AccessoryKeys(s))); i < poolCapacity; ++i)
            rec[i * 0x3C + 0x18] = 0;
    }
}

// Clear the active model's bone and morph key selections, plus the remaining
// display-key selections.  (sub_446A70 @ 0x447AF0 / 0x447BA0 / 0x447C30 etc.)
static void ClearActiveModelFlags(MMDApp* app) {
    negative_frames::SelectCell(app,0,-999,false);
    // The original reaches this helper only while the model-edit context is
    // active.  During a partially completed mode switch MikuMikuStudio can briefly
    // expose the same row with no active slot, so preserve that entrance
    // invariant here instead of dereferencing slot 0/null.
    if (ActiveModel(app) == nullptr)
        return;
    unsigned char* model = ActiveModel(app);
    mikumikustudio::mdl::BoneKey* boneKeys = mikumikustudio::mdl::BoneKeys(model);
    const std::size_t boneKeyCapacity =
        mikumikustudio::mdl::BoneKeyCapacity(*mikumikustudio::mdl::Mdl(model));
    for (std::size_t i = 0; i < boneKeyCapacity; ++i)
        boneKeys[i].allocated = 0;

    mikumikustudio::mdl::MorphKey* morphKeys = mikumikustudio::mdl::MorphKeys(model);
    const auto morphCapacity1 = mikumikustudio::mdl::MorphKeyCapacity(morphKeys);
    for (std::size_t i = 0; i < morphCapacity1; ++i)
        morphKeys[i].allocated = 0;
    mikumikustudio::mdl::DisplayKey* displayKeys = mikumikustudio::mdl::DisplayKeys(model);
    for (std::size_t i = 0, poolCapacity = static_cast<std::size_t>(mdl::KeyPoolCapacity(displayKeys)); i < poolCapacity; ++i)
        displayKeys[i].allocated = 0;
}

void ClearTimelineKeySelection(MMDApp* app) {
    negative_frames::SelectCell(app,0,-999,false);
    if (app->CameraMode() != 0) ClearGlobalFrameSelection(app);
    else ClearActiveModelFlags(app);
}

static void SetTimelineSelectionRange(MMDApp* app, std::int32_t firstOffset,
                                      std::int32_t firstBase,
                                      std::int32_t lastOffset,
                                      std::int32_t lastBase) {
    app->TimelineRangeFirstOffset() = firstOffset;
    app->TimelineRangeFirstBase() = firstBase;
    app->TimelineRangeLastOffset() = lastOffset;
    app->TimelineRangeLastBase() = lastBase;
}

// Capture global-track selections through the key type. This keeps the scan
// bound, selection byte and frame value tied to the same record definition.
template <typename Key>
static void FillSelectionRecords(MMDApp* app, const Key* keys,
                                 TimelineSelectionBand band) {
    auto& count = app->TimelineSelectionCount(band);
    count = 0;
    if (keys == nullptr) return;
    for (std::size_t i = 0, poolCapacity = static_cast<std::size_t>(mdl::KeyPoolCapacity(keys)); i < poolCapacity; ++i)
        if (keys[i].selected) ++count;
    if (count == 0) return;
    auto* records = static_cast<TimelineSelectionRecord*>(
        ::operator new(sizeof(TimelineSelectionRecord) * count));
    app->TimelineSelectionRecords(band) = records;
    int out = 0;
    for (std::size_t i = 0, poolCapacity = static_cast<std::size_t>(mdl::KeyPoolCapacity(keys)); i < poolCapacity; ++i) {
        if (keys[i].selected)
            records[out++] = {{static_cast<std::int32_t>(i), 0, 0,
                              static_cast<std::int32_t>(keys[i].frame)}};
    }
    qsort(records, count, sizeof(TimelineSelectionRecord), CompareFunction);
}

// Shared selection-change point (loc_448EEB-0x44A437): dirty flag + click
// anchor + selection-count re-evaluation (0xA03EC region, per-array flag
// counting + record fills + qsort), panel repaint (0x414610) and the
// selection-range reset (0xA05C0-0xA05CC / 0xA05D0).
// (sub_446A70 @ 0x448EEB-0x44A437)
static void SelectionStats(MMDApp* app, int x, int y, HWND hwnd) {
    app->SelectionBoxDragging() = 1;
    app->SelectionBoxAnchorX() = x - 6;
    app->SelectionBoxAnchorY() = y - 0x91;
    if (app->TimelineSelectionChanged() != 0) {
        memset(app->TimelineSelectionRegion(), 0, 0x40);        // 656364 (0xA03EC)
        if (app->state.optflag[0] != 0) {   // 760 (0x2F8)
            FillSelectionRecords(app, app->CameraKeys(), TimelineSelectionBand::Camera);
            FillSelectionRecords(app, app->LightKeys(), TimelineSelectionBand::Light);
            FillSelectionRecords(app, app->ShadowKeys(), TimelineSelectionBand::SelfShadow);
            FillSelectionRecords(app, app->GravityKeys(), TimelineSelectionBand::Gravity);
            for (int slot = 0; slot < 255; ++slot) {
                const auto* keys = app->AccessoryKeys(slot);
                if (!keys) continue;
                for (std::size_t i = 0, poolCapacity = static_cast<std::size_t>(mdl::KeyPoolCapacity(keys)); i < poolCapacity; ++i)
                    if (keys[i].selected)
                        ++app->TimelineSelectionCount(TimelineSelectionBand::Accessory);
            }
            if (app->TimelineSelectionCount(TimelineSelectionBand::Accessory) > 0) {
                std::int32_t* out = static_cast<std::int32_t*>(::operator new(
                    static_cast<std::size_t>(app->TimelineSelectionCount(
                        TimelineSelectionBand::Accessory)) * 0x10));
                app->TimelineSelectionRecords(TimelineSelectionBand::Accessory) =
                    reinterpret_cast<TimelineSelectionRecord*>(out);
                std::size_t maximumCapacity = 0;
                for (int i = 0; i < 255; ++i) maximumCapacity = (std::max)(maximumCapacity, mdl::KeyPoolCapacity(app->AccessoryKeys(i)));
                for (std::size_t off = 0; off < maximumCapacity; ++off) {
                    for (int g = 0; g < 0x33; ++g) {
                        for (int j = 0; j < 5; ++j) {
                            auto* slot = app->AccessoryKeys(g * 5 + j);
                            if (slot && off < mdl::KeyPoolCapacity(slot) && slot[off].selected != 0) {
                                out[1] = off;
                                out[2] = g * 5 + j;
                                out[3] = static_cast<std::int32_t>(slot[off].frame);
                                out += 4;
                            }
                        }
                    }
                }
                qsort(app->TimelineSelectionRecords(TimelineSelectionBand::Accessory),
                      static_cast<std::size_t>(app->TimelineSelectionCount(
                          TimelineSelectionBand::Accessory)),
                      0x10, CompareFunction);
            }
            // 0x4495BD jle
        } else {
            // ---- edit-mode stats (loc_4497A1-0x44A426) ------------------------
            // count the 6 bone display flags (model+0x26E0, 0x168-stride,
            // flags at +0x38+i*0x3C) into 0xA0414
            std::int32_t& boneCnt =
                app->TimelineSelectionCount(TimelineSelectionBand::ModelIk);
            boneCnt = 0;
            unsigned char* m = ActiveModel(app);
            if (m != nullptr) {
                mikumikustudio::mdl::BoneKey* bones = mikumikustudio::mdl::BoneKeys(m);
                const std::size_t boneKeyCapacity =
                    mikumikustudio::mdl::BoneKeyCapacity(*mikumikustudio::mdl::Mdl(m));
                for (std::size_t i = 0; i < boneKeyCapacity; ++i)
                    if (bones[i].allocated != 0) ++boneCnt;
                if (boneCnt > 0) {
                    EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), TRUE);      // 400
                    EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE);     // 401
                    // keyframe-cluster append on the active model (0x449886-0x449A5A)
                    auto* record = mikumikustudio::mdl::Mdl(m);
                    record->undoDirty = 1;
                    record->redoDirty = 0;
                    std::uint32_t& kf = record->undoState[0];
                    kf = kf + 1;
                    if (kf >= 0x1E)
                        kf = 0;
                    record->undoState[1] = kf;
                    bone_scale::CaptureUndo(m);
                    auto& undo = record->undoRings[0].slots[kf];
                    undo.operation = 2;
                    undo.frame = app->state.currentFrame;
                    auto*& bufSlot = undo.bonePose;
                    if (bufSlot != nullptr) {
                        free(bufSlot);
                        bufSlot = nullptr;
                    }
                    const std::int32_t boneN = mikumikustudio::mdl::Mdl(m)->boneCount;
                    bufSlot = static_cast<mikumikustudio::mdl::BonePoseSnapshot*>(
                        ::operator new(static_cast<std::size_t>(boneN) *
                                       sizeof(mikumikustudio::mdl::BonePoseSnapshot)));
                    memset(bufSlot, 0, static_cast<std::size_t>(boneN) *
                                           sizeof(mikumikustudio::mdl::BonePoseSnapshot));
                    mikumikustudio::mdl::BoneRecord* srcBase = mikumikustudio::mdl::Bones(m);
                    unsigned char* flagMap = mikumikustudio::mdl::Mdl(m)->bonePhysicsState;
                    for (int i = 0; i < boneN; ++i) {
                        auto& out = bufSlot[i];
                        out.boneIndex = 0;
                        memcpy(out.position, srcBase[i].trans, sizeof out.position);
                        memcpy(out.rotation, srcBase[i].rotQuat, sizeof out.rotation);
                        out.physicsDisabled = flagMap[i];
                    }
                    // second keyframe cluster + data buffer (0x449BB5-0x449CC2)
                    undo.dirty = 0;
                    void*& dataSlot = undo.auxiliaryPose;
                    if (dataSlot != nullptr) {
                        free(dataSlot);
                        dataSlot = nullptr;
                    }
                    dataSlot = ::operator new(static_cast<std::size_t>(boneCnt) * sizeof(mdl::BoneKeySnapshot));
                    memset(dataSlot, 0, static_cast<std::size_t>(boneCnt) * sizeof(mdl::BoneKeySnapshot));
                    mikumikustudio::mdl::BoneKeyVisitClear(*mikumikustudio::mdl::Mdl(m));
                    // ... and the visit map the registrar dedups through, for
                    // the same capacity, resolved once for the sweep below.
                    unsigned char* const visited =
                        mikumikustudio::mdl::BoneKeyVisitMapOf(
                            *mikumikustudio::mdl::Mdl(m), boneKeyCapacity).bytes;
                    // feed every set bone flag to 0x49D410 (0x449CF0-0x449D1C)
                    for (std::size_t i = 0; i < boneKeyCapacity; ++i)
                        if (bones[i].allocated != 0)
                            AppendBoneKeyToUndo(ActiveModel(app), static_cast<int>(i),
                                                visited);
                    // bone-temp records (0x449D1E-0x449F53): 6 flags per 0x168-stride
                    // record -> 0xA0414/0xA0418, qsorted
                    std::int32_t* bout = static_cast<std::int32_t*>(
                        ::operator new(static_cast<std::size_t>(boneCnt) * 0x10));
                    app->TimelineSelectionRecords(TimelineSelectionBand::ModelIk) =
                        reinterpret_cast<TimelineSelectionRecord*>(bout);
                    // The lane value `idx - 2 + k` was always the slot index (idx
                    // starts at 2 and advances by six per group), so this flat
                    // form is identical for every full group and also covers the
                    // tail: a runtime capacity is not necessarily a multiple of
                    // six, and skipping the tail would leave part of the buffer
                    // the qsort below sorts uninitialised, since boneCnt counts
                    // every allocated slot.
                    for (std::size_t slot = 0; slot < boneKeyCapacity; ++slot) {
                        mikumikustudio::mdl::BoneKey& key = bones[slot];
                        if (key.allocated != 0) {
                            bout[0] = static_cast<std::int32_t>(slot);
                            bout[3] = static_cast<std::int32_t>(key.frame);
                            bout += 4;
                        }
                    }
                    qsort(app->TimelineSelectionRecords(TimelineSelectionBand::ModelIk),
                          static_cast<std::size_t>(boneCnt),
                          0x10, CompareFunction);
                }
                // morph-temp records (0x449F56-0x44A1C2): 5 flags per 0x64-stride
                // record -> 0xA041C/0xA0420, qsorted
                std::int32_t& morphCnt =
                    app->TimelineSelectionCount(TimelineSelectionBand::ModelMorph);
                morphCnt = 0;
                {
                    unsigned char* mm = ActiveModel(app);
                    mikumikustudio::mdl::MorphKey* morphs = mikumikustudio::mdl::MorphKeys(mm);
                    const auto morphCapacity2 = mikumikustudio::mdl::MorphKeyCapacity(morphs);
                    for (std::size_t i = 0; i < morphCapacity2; ++i)
                        if (morphs[i].allocated != 0) ++morphCnt;
                    if (morphCnt > 0) {
                        std::int32_t* mout = static_cast<std::int32_t*>(
                            ::operator new(static_cast<std::size_t>(morphCnt) * 0x10));
                        app->TimelineSelectionRecords(TimelineSelectionBand::ModelMorph) =
                            reinterpret_cast<TimelineSelectionRecord*>(mout);
                        int idx = 2;
                        for (int i = 0; i < 0xFA0; ++i) {
                            for (int k = 0; k < 5; ++k) {
                                mikumikustudio::mdl::MorphKey& key = morphs[i * 5 + k];
                                if (key.allocated != 0) {
                                    mout[0] = idx - 2 + k;
                                    mout[3] = static_cast<std::int32_t>(key.frame);
                                    mout += 4;
                                }
                            }
                            idx += 5;
                        }
                        qsort(app->TimelineSelectionRecords(TimelineSelectionBand::ModelMorph),
                              static_cast<std::size_t>(morphCnt),
                              0x10, CompareFunction);
                    }
                }
                // IK-temp records (0x44A1C2-0x44A426): 5 flags per 0x8C-stride
                // record -> 0xA0424/0xA0428, qsorted at loc_44A426
                std::int32_t& ikCnt =
                    app->TimelineSelectionCount(TimelineSelectionBand::ModelBone);
                ikCnt = 0;
                {
                    unsigned char* mm = ActiveModel(app);
                    mikumikustudio::mdl::DisplayKey* displayKeys = mikumikustudio::mdl::DisplayKeys(mm);
                    for (std::size_t i = 0, poolCapacity = static_cast<std::size_t>(mdl::KeyPoolCapacity(displayKeys)); i < poolCapacity; ++i)
                        if (displayKeys[i].allocated != 0) ++ikCnt;
                    if (ikCnt > 0) {
                        std::int32_t* iout = static_cast<std::int32_t*>(
                            ::operator new(static_cast<std::size_t>(ikCnt) * 0x10));
                        app->TimelineSelectionRecords(TimelineSelectionBand::ModelBone) =
                            reinterpret_cast<TimelineSelectionRecord*>(iout);
                        int idx = 2;
                        for (int i = 0; i < 0xC8; ++i) {       // 0x6D60/0x8C
                            for (int k = 0; k < 5; ++k) {
                                mikumikustudio::mdl::DisplayKey& key = displayKeys[i * 5 + k];
                                if (key.allocated != 0) {
                                    iout[0] = idx - 2 + k;
                                    iout[3] = static_cast<std::int32_t>(key.frame);
                                    iout += 4;
                                }
                            }
                            idx += 5;
                        }
                    }
                }
                // fall into the shared qsort site at loc_44A426 with the IK pair.
                qsort(app->TimelineSelectionRecords(TimelineSelectionBand::ModelBone),
                      static_cast<std::size_t>(
                          app->TimelineSelectionCount(TimelineSelectionBand::ModelBone)),
                      0x10, CompareFunction);
            }
        }
    }
    if(app->TimelineSelectionChanged())negative_frames::BeginDrag(app);
    // loc_44A437
    PanelPaint(app);                               // 0x414610
    SelectionReeval(app);                          // 0x430510
}

// x64-safe home for the eight timeline-selection record pointers - see the
// g_timelineSelectionRecords comment in mmd_app.hpp.
TimelineSelectionRecord* g_timelineSelectionRecords[8] = {};

// ---- HandleLButtonDown region helpers ------------------------------------

// One S5a display-mode cell toggle - the 15-line branch body that the four
// per-band blocks (0x4473DA-0x4479BB: rigid/joint/IK/morph) repeat
// verbatim.  selected != 0: shift clears it, a plain click only marks the
// stats dirty; selected == 0: a plain click first clears all selection
// bitmaps, a shift-click sets it without the clear.
static void ToggleTimelineCell(MMDApp* app, bool shiftActive,
                               std::uint8_t& selected) {
    if (selected != 0) {
        if (shiftActive)
            selected = 0;
        else
            app->TimelineSelectionChanged() = 1;
    } else if (!shiftActive) {
        ClearSelectionBitmaps(app);
        selected = 1;
        app->TimelineSelectionChanged() = 1;
    } else {
        selected = 1;
    }
}

// S5a band table: the four structurally identical band toggles differ only
// in {y-range, row-hit table, track array}, so they collapse into the
// bands[] loop in HandleLButtonDown_TimelineAreaHit.  The y-ranges are
// disjoint and contiguous ([0xA0, 0xD8) in 14px steps), so at most one
// entry matches - the same one-block-executes invariant as the original
// if-chain.  The fifth band (accessory 2D grid, y >= 0xD8) has its own
// index math and stays separate.
struct TimelineBandHit {
    int yMin;
    int yMax;
    const std::int32_t* rowHits;              // state.rowHitBand0..3
    std::uint8_t& (*keySelected)(MMDApp*, int);
};

static std::uint8_t& CameraKeySelected(MMDApp* app, int i) {
    return app->CameraKeys()[i].selected;
}
static std::uint8_t& LightKeySelected(MMDApp* app, int i) {
    return app->LightKeys()[i].selected;
}
static std::uint8_t& ShadowKeySelected(MMDApp* app, int i) {
    return app->ShadowKeys()[i].selected;
}
static std::uint8_t& GravityKeySelected(MMDApp* app, int i) {
    return app->GravityKeys()[i].selected;
}

// S6 stage-toggle helpers.  The three edit-mode stages (display/morph/bone
// keys, loc_447A4D-0x448EEB) repeat two branch shapes over three key
// families:
//   type > 0 / type == -10 : the allocated-toggle below (6 copies in the
//                            original, differing only in key family/index)
//   type == -1 / type == -2: the timeline-range rows below (4 copies,
//                            morph/bone stages only)
// Negative entries aggregate the keys of a collapsed group. The painter
// resolves that group with a half-open frame/track rectangle, never with
// pointers into a key pool. -1 denotes selected keys; -2 unselected keys.
template <typename KeyT>
static void ToggleModelKeyAllocated(MMDApp* app, KeyT& key,
                                    bool shiftActive) {
    if (key.allocated != 0) {
        if (shiftActive)
            key.allocated = 0;
        else
            app->TimelineSelectionChanged() = 1;
    } else if (shiftActive) {
        key.allocated = 1;
    } else {
        ClearActiveModelFlags(app);
        key.allocated = 1;
        app->TimelineSelectionChanged() = 1;
    }
}

static void StageRangeRow(MMDApp* app, std::int32_t type,
                          bool shiftActive, int frameColumn, int trackRow) {
    if (type == -1) {                          // loc_4483B8 / loc_448AB8
        if (shiftActive) {
            SetTimelineSelectionRange(app, frameColumn, trackRow,
                                      frameColumn + 1, trackRow + 1);
            app->TimelineRangeApplyEnabled() = 1;
        } else {
            app->TimelineSelectionChanged() = 1;
        }
    } else {                                   // type == -2, loc_4483F1/0x448AF1
        SetTimelineSelectionRange(app, frameColumn, trackRow,
                                  frameColumn + 1, trackRow + 1);
        if (!shiftActive)
            ClearActiveModelFlags(app);
        if (!shiftActive) {
            PanelPaint(app); // Resolve the collapsed row into actual selected keys.
            app->ClearTimelineRange();
            app->TimelineSelectionChanged() = 1;
        }
    }
}

// ---- S2 (VA 0x446B01-0x446B9C): margin column (X in (4,20)) ---------------
// Row-type display toggle, edit mode only; entered with the row gates
// already checked.  Row index = (y-0xA0)/14 (magic 0x92492493, sar 3); row
// type byte at model+0x2DC0 indexes a 0x65-stride table (model+0x26D0)
// whose flag at +0x64 is toggled.
static void HandleLButtonDown_MarginColumnToggle(MMDApp* app,
                                                 std::int32_t y) {
    if (app->state.optflag[0] == 0) {  // 760 (0x2F8)
        unsigned char* model = app->SelectedModel();
        if (model == nullptr)
            return;
        const std::int32_t type =
            mdl::DisplayRowType(*mdl::Mdl(model), Div14(y - 0xA0));
        if (type > 0) {
            mdl::DisplayGroup& frame = mdl::DisplayGroups(model)[type];
            frame.flags = (frame.flags == 0) ? 1 : 0;   // cmp/setz toggle
            PostLanguageSweep(app);             // 0x42F1E0
        }
    }
    return;
}

// ---- S3+S4 (VA 0x446BA1-0x44738F): name column (X in (20,95]) --------------
// Bone/morph row selection; entered with the row gates already checked.
// Edit mode selects the clicked row (row table model+0x2E88, special morph
// rows encoded as -1-idx, -999 = "bottom" morph row) and dispatches the
// row-type handlers (model+0x2DC0 byte); display mode goes to the S4
// sub-branch below.
static void HandleLButtonDown_NameColumnHit(MMDApp* app, HWND hwnd,
                                            std::int32_t y) {
    if (app->state.optflag[0] != 0) {  // 760 (0x2F8)
        // ---- S4: display-mode name column sub-branch (loc_44719F) ---
        if (!app->ShiftModifierActive()) {
            // clear the accessory-row selection flag of all 255 accessory
            // slots (the original walks it unrolled, five 4-byte pointers
            // per 0x14 step; on x64 the pointer array is indexed directly
            // through ObjectSlot)
            for (int i = 0; i < 255; ++i) {
                mdl::AccessoryRecord* accessory =
                    static_cast<mdl::AccessoryRecord*>(app->ObjectSlot(i));
                if (accessory != nullptr)
                    accessory->rowSelected = 0;
            }
            app->ClearGlobalTimelineTrackSelection();  // 0xA03E4..0xA03E7
        }
        // loc_447210: column-band toggles (band selected by this+8) - four
        // isomorphic {y-range, global-track} blocks merged into the table
        // below; the y-ranges are disjoint/contiguous ([0xA0, 0xD8) in 14px
        // steps), so at most one entry matches, exactly like the original
        // if-chain.
        static const struct {
            int yMin, yMax;
            GlobalTimelineTrack track;
        } kColumnBands[4] = {
            {0xA0, 0xAE, GlobalTimelineTrack::Camera},      // bone band
            {0xAE, 0xBC, GlobalTimelineTrack::Light},       // morph band
            {0xBC, 0xCA, GlobalTimelineTrack::SelfShadow},  // IK band
            {0xCA, 0xD8, GlobalTimelineTrack::Gravity},     // rigid band
        };
        for (const auto& band : kColumnBands) {
            if (y >= band.yMin && y < band.yMax) {
                app->GlobalTrackSelected(band.track) =
                    app->GlobalTrackSelected(band.track) == 0 ? 1 : 0;
                PostLanguageSweep(app);
                return;
            }
        }
        {                                                        // accessory rows (y >= 0xD8)
            const std::int32_t jrow = Div14(y - 0xD8);
            const std::int32_t jidx = app->state.jointLineMap[jrow];
            if (jidx >= 0) {
                mdl::AccessoryRecord* accessory = static_cast<
                    mdl::AccessoryRecord*>(app->ObjectSlot(jidx));
                if (accessory->rowSelected != 0) {
                    accessory->rowSelected = 0;
                    PostLanguageSweep(app);
                    PostLanguageSweep2(app);
                } else {
                    accessory->rowSelected = 1;
                    app->state.selectedObjectSlot =
                        static_cast<std::uint8_t>(jidx);
                    PostLanguageSweep(app);
                    SendMessageA(GetDlgItem(hwnd, panel::kAccessoryCombo), CB_SETCURSEL,
                                 accessory->order, 0);
                    SyncAccessoryEditPanel(app);
                    PostLanguageSweep2(app);
                }
            } else {
                PostLanguageSweep2(app);
            }
            return;
        }
    }

    // ---- edit-mode name column: clear stale selection state ---------
    // (0x446BC7-0x446C8A)
    if (ActiveModel(app) == nullptr)
        return;
    if (!app->ShiftModifierActive()) {
        unsigned char* m0 = ActiveModel(app);
        const std::int32_t boneCount =
            mikumikustudio::mdl::Mdl(m0)->boneCount;
        for (int i = 0; i < boneCount; ++i) {
            unsigned char* mm = ActiveModel(app);
            mikumikustudio::mdl::Mdl(mm)->boneSelection[i] = 0;
        }
        for (int i = 0;
             i < static_cast<int>(mikumikustudio::mdl::FacialFrameCount(*mikumikustudio::mdl::Mdl(m0))); ++i) {
            unsigned char* mm = ActiveModel(app);
            mdl::DisplayFrames(mm)[i].selected = 0;
        }
        mikumikustudio::mdl::Mdl(ActiveModel(app))->displayKeyframesPresent = 0;
    }

    // loc_446C8A: row lookup + selection toggle
    {
        unsigned char* m = ActiveModel(app);
        mdl::ModelRecord* const record = mikumikustudio::mdl::Mdl(m);
        const std::int32_t row = Div14(y - 0xA0);
        // The row table is 200 entries and only rows that were painted carry a
        // live record; guard the derived index instead of reading past it.
        if (row < 0 || row >= 200) {
            PostLanguageSweep2(app);
            return;
        }
        // Bone rows: the record is the bone index.  The mapping also rejects
        // records that point outside boneCount (a row painted for another
        // model / a stale table), so the selection below can never write
        // outside boneSelection.
        const std::int32_t idx = BoneListRowBoneIndex(record, row);
        if (idx >= 0) {
            unsigned char* p = mikumikustudio::mdl::Mdl(m)->boneSelection + idx;
            if (*p != 0) {
                *p = 0;
            } else {
                *p = 1;
                mikumikustudio::mdl::Mdl(m)->selectedBone = idx;
            }
            // Repaint the label column here: the visible bone-row highlight
            // (themeColors[34] rows in PanelPaint) is produced by
            // PostLanguageSweep, so without it a click on a bone name changed
            // the state but showed nothing until some later repaint.
            PostLanguageSweep(app);
            // via 0x44702D
        } else if (BoneListRowMorphIndex(record, row) >= 0) {
            // Any negative record other than the -999 sentinel is a morph row
            // and encodes its morph index as -1-idx; -999 (a row without a
            // record) skips the morph search and falls through to the row-type
            // dispatch below, exactly like the original's
            // `test edx,edx / js` + `cmp edx,0xFFFFFC19 / je` pair.
            const std::int32_t want = BoneListRowMorphIndex(record, row);
            const std::int32_t morphCount =
                static_cast<std::int32_t>(mikumikustudio::mdl::FacialFrameCount(*mikumikustudio::mdl::Mdl(m)));
            int i = 0;
            for (; i < morphCount; ++i) {
                if (mdl::FrameTargetIndex(mdl::DisplayFrames(m)[i]) == want)
                    break;
            }
            if (i < morphCount) {
                // morph row found: toggle its flag, then sync the bone
                // name combo (controls 0x1F8..0x209 by bone row type)
                mdl::FrameGroup& faceRec = mdl::DisplayFrames(m)[i];
                if (faceRec.selected != 0) {
                    faceRec.selected = 0;
                } else {
                    faceRec.selected = 1;
                    const std::int32_t morphIndex = mikumikustudio::mdl::FrameTargetIndex(faceRec);
                    auto& morph = mikumikustudio::mdl::Morphs(m)[morphIndex];
                    const auto panel = morph.panel;
                    HWND hCombo = nullptr, hSpin = nullptr, hText = nullptr;
                    switch (panel) {                         // 0x446D99-0x446E77
                        case mikumikustudio::mdl::MorphPanel::eyebrow:
                            hCombo = GetDlgItem(hwnd, panel::kMorphCombo0);
                            hSpin = GetDlgItem(hwnd, panel::kMorphSlider0);
                            hText = GetDlgItem(hwnd, panel::kMorphEdit0);
                            break;
                        case mikumikustudio::mdl::MorphPanel::eye:
                            hCombo = GetDlgItem(hwnd, panel::kMorphCombo1);
                            hSpin = GetDlgItem(hwnd, panel::kMorphSlider1);
                            hText = GetDlgItem(hwnd, panel::kMorphEdit1);
                            break;
                        case mikumikustudio::mdl::MorphPanel::mouth:
                            hCombo = GetDlgItem(hwnd, panel::kMorphCombo2);
                            hSpin = GetDlgItem(hwnd, panel::kMorphSlider2);
                            hText = GetDlgItem(hwnd, panel::kMorphEdit2);
                            break;
                        case mikumikustudio::mdl::MorphPanel::other:
                            hCombo = GetDlgItem(hwnd, panel::kMorphCombo3);
                            hSpin = GetDlgItem(hwnd, panel::kMorphSlider3);
                            hText = GetDlgItem(hwnd, panel::kMorphEdit3);
                            break;
                        // default: original falls through with stale stack
                        // handles; nullptr here is the safe equivalent.
                    }
                    // Preserve the active morph separately for each panel.
                    const auto panelIndex = static_cast<unsigned>(panel) - 1;
                    if (panelIndex >= 4) {
                        PostLanguageSweep(app);
                        PostLanguageSweep2(app);
                        return;
                    }
                    mikumikustudio::mdl::Mdl(m)->selectedMorphs[panelIndex] = morphIndex;
                    // sync the combo cursor with the morph name
                    // (CB_GETCOUNT 0x146 / CB_GETLBTEXT 0x148 / CB_SETCURSEL
                    // 0x14E, 0x446EC0-0x446FAE)
                    const LRESULT count = SendMessageA(hCombo, CB_GETCOUNT, 0, 0);
                    for (LONG i2 = 0; i2 < count; ++i2) {
                        if (SendMessageW(hCombo, CB_GETITEMDATA, i2, 0) == morphIndex) {
                            SendMessageA(hCombo, CB_SETCURSEL, i2, 0);
                        }
                    }
                    // Morph value spin/text sync (0x446FB4-0x447027):
                    // spin gets (int)(value*100.0), text gets "%5.4f"
                    const float v = mikumikustudio::mdl::Morphs(m)[
                        mikumikustudio::mdl::Mdl(m)->selectedMorphs[panelIndex]].value;
                    SendMessageA(hSpin, TBM_SETPOS, 1,
                                 static_cast<LPARAM>(static_cast<std::int32_t>(v * 100.0)));
                    char valBuf[0x64];
                    sprintf_s(valBuf, 0x64u, "%5.4f", static_cast<double>(v));
                    SetWindowTextA(hText, valBuf);
                }
                PostLanguageSweep(app);
            }
        }
    }

    // loc_447034: row-type dispatch after selection (entry point for
    // the selection paths above)
    {
        unsigned char* m = ActiveModel(app);
        const std::int32_t type =
            mdl::DisplayRowType(*mdl::Mdl(m), Div14(y - 0xA0));
        if (type == 1) {
            mdl::Mdl(m)->displayKeyframesPresent =
                (mdl::Mdl(m)->displayKeyframesPresent == 0) ? 1 : 0;  // setz
            PostLanguageSweep(app);
            PostLanguageSweep2(app);
            return;
        }
        if (type == 2) {
            for (int i = 0;
                 i < static_cast<int>(mikumikustudio::mdl::FacialFrameCount(*mikumikustudio::mdl::Mdl(m))); ++i) {
                mdl::FrameGroup& face = mdl::DisplayFrames(m)[i];
                face.selected = (face.selected == 0) ? 1 : 0;
            }
            PostLanguageSweep(app);
            PostLanguageSweep2(app);
            return;
        }
        if (type <= 0) {                                 // jle loc_447384
            PostLanguageSweep2(app);
            return;
        }
        // Rigid-group rows (x86 0x4470FA..0x44718A / x64 0x7FF7CB45A379..):
        // walk the 46-byte (0x2E) FrameGroup records at model+0x26D8, and
        // for each record whose type byte (record+0x28, read signed like
        // the original movsx) matches the clicked row type, toggle the
        // selection flag of the bone named by the uint16 at record+0x2A.
        for (int i = 0;
             i < static_cast<int>(mdl::Mdl(m)->rigidBodyCount); ++i) {
            mdl::FrameGroup& rigid = mdl::RigidGroups(m)[i];
            if (mikumikustudio::mdl::FrameGroupIndex(rigid) == type) {
                const std::int32_t boneIdx = mikumikustudio::mdl::FrameTargetIndex(rigid);
                unsigned char* p =
                    mikumikustudio::mdl::Mdl(m)->boneSelection + boneIdx;
                if (*p != 0)
                    *p = 0;
                else
                    *p = 1;
            }
        }
        PostLanguageSweep(app);
        PostLanguageSweep2(app);
        return;
    }
}

// ---- S5-S6 (VA 0x447390-0x448EEB): main area (X in (95, sidebar-18)) --------
// Timeline cell hits, entered with the row gates already checked.  Display
// mode (S5a) toggles keys in the five timeline bands; edit mode (S6) runs
// the three-stage row-type chain.  Both converge on SelectionStats
// (loc_448EEB).
static void HandleLButtonDown_TimelineAreaHit(MMDApp* app, HWND hwnd,
                                              std::int32_t x,
                                              std::int32_t y) {
    if (app->PlaybackActive() != 0)
        return;
    const auto negativeCell=std::int64_t(app->state.timelineStartFrame)+Div13(x-100);
    if(negativeCell<0 && negativeCell>=frame_limits::minimum) {
        app->TimelineSelectionChanged()=0;
        app->ClearTimelineRange();
        const bool shift=app->ShiftModifierActive();
        const bool selected=negative_frames::CellSelected(app,int(negativeCell),Div14(y-160));
        if(!shift && !selected)ClearTimelineKeySelection(app);
        const bool hit=selected && !shift ? true : negative_frames::SelectCell(app,int(negativeCell),Div14(y-160),shift);
        app->TimelineSelectionChanged()=hit && !shift;
        SelectionStats(app,x,y,hwnd);
        SelectionReeval(app);
        return;
    }
    app->TimelineSelectionChanged() = 0;
    app->ClearTimelineRange();
    if (app->state.optflag[0] != 0) {  // 760 (0x2F8)
        // ---- S5a: display-mode selection columns --------------------
        // (0x4473DA-0x4479BB) band selected by this+8, row by
        // (x-100)/13 (magic 0x4EC4EC4F, sar 2).  Selection indices are
        // latched into locals (rigid/joint/ik/morph); when none of the
        // five bands hit and mode != 3 the four selection bitmaps are
        // cleared wholesale (0x447990-0x447A48).
        const std::int32_t rowD = Div13(x - 0x64);
        std::int32_t bandIdx = -1;  // var_194 / hWnd var / var_198 / wParam var
        std::int32_t joint2dIdx = -1;                        // var_18C
        const bool shiftActive = app->ShiftModifierActive();
        // four isomorphic band toggles (0x4473DA-0x4479BB), table-driven:
        // rigid/joint/IK/morph differ only in {y-range, row-hit table,
        // track array}.  At most one band matches; the shared bandIdx latch
        // therefore equals the original four latches all staying -1.
        const TimelineBandHit bands[4] = {
            {0xA0, 0xAE, app->state.rowHitBand0, CameraKeySelected},  // rigid
            {0xAE, 0xBC, app->state.rowHitBand1, LightKeySelected},   // joint
            {0xBC, 0xCA, app->state.rowHitBand2, ShadowKeySelected},  // IK
            {0xCA, 0xD8, app->state.rowHitBand3, GravityKeySelected}, // morph
        };
        for (const TimelineBandHit& band : bands) {
            if (y < band.yMin || y >= band.yMax)
                continue;
            bandIdx = band.rowHits[rowD];
            if (bandIdx >= 0)
                ToggleTimelineCell(app, shiftActive,
                                   band.keySelected(app, bandIdx));
        }
        if (y >= 0xD8) {                                     // joint 2D band
            const std::int32_t jrow = Div14(y - 0xD8);
            const std::int32_t slotIdx =
                app->state.jointLineMap[jrow];
            const std::int32_t col = Div13(x - 0x64);
            joint2dIdx =
                app->state.rowHitAcc[col * 0xC8 + jrow];
            if (joint2dIdx >= 0) {
                unsigned char* slot = reinterpret_cast<unsigned char*>(
                    app->AccessoryKeys(slotIdx));
                unsigned char* rec = slot + joint2dIdx * 0x3C + 0x18;
                if (*rec != 0) {
                    if (shiftActive)
                        *rec = 0;
                    else
                        app->TimelineSelectionChanged() = 1;
                } else if (!shiftActive) {
                    ClearSelectionBitmaps(app);
                    *rec = 1;
                    app->TimelineSelectionChanged() = 1;
                } else {
                    *rec = 1;
                }
            }
        }
        // loc_447990: no band hit at all -> wholesale bitmap clear
        if (bandIdx == -1 && joint2dIdx == -1 && !shiftActive) {
            ClearSelectionBitmaps(app);
        }
        SelectionStats(app, x, y, hwnd);       // loc_448EEB
        return;
    }
    // ---- S6: edit-mode generic selection chain (loc_447A4D-0x448EEB)
    // Three 2D-table row-type stages (app+0x4EB84 -> IK-type flags,
    // app+0x27A84 -> morph-type flags, app+0x984 -> bone-type flags),
    // each toggling one flag of the active model; the shared
    // selection-change point (loc_448EEB) runs the selection stats.
    else {
        if (ActiveModel(app) == nullptr)
            return;
        const bool shiftActive = app->ShiftModifierActive();
        const std::int32_t row2d = Div13(x - 0x64) * 0xC8 + Div14(y - 0xA0);
        // stage 1 (loc_447A4D): IK-type flags, model+0x26E8 0x1C-stride
        // records with flag at +0x14
        const std::int32_t ikType = app->state.rowHitIk[row2d];
        if (ikType > 0) {
            ToggleModelKeyAllocated(app,
                mikumikustudio::mdl::DisplayKeys(ActiveModel(app))[ikType],
                shiftActive);
        } else if (ikType == -10) {                 // loc_447CD9 (row 0)
            ToggleModelKeyAllocated(app,
                mikumikustudio::mdl::DisplayKeys(ActiveModel(app))[0],
                shiftActive);
        }
        // stage 2 (loc_447F03): morph-type flags, model+0x26E4
        // 0x14-stride records with flag at +0x10
        const std::int32_t morphType = app->state.rowHitMorph[row2d];
        if (morphType > 0) {
            ToggleModelKeyAllocated(app,
                mikumikustudio::mdl::MorphKeys(ActiveModel(app))[morphType],
                shiftActive);
        } else if (morphType == -10) {              // loc_448189 (row 0)
            ToggleModelKeyAllocated(app,
                mikumikustudio::mdl::MorphKeys(ActiveModel(app))[0],
                shiftActive);
        } else if (morphType == -1) {               // loc_4483B8
            StageRangeRow(app, morphType, shiftActive,
                          Div13(x - 0x64), Div14(y - 0xA0));
        } else if (morphType == -2) {               // loc_4483F1
            StageRangeRow(app, morphType, shiftActive,
                          Div13(x - 0x64), Div14(y - 0xA0));
        }
        // stage 3 (loc_4485FB): bone-type flags, model+0x26E0 0x3C flag
        // stride with flag at +0x38
        const std::int32_t boneType = app->state.rowHitBone[row2d];
        if (boneType > 0) {
            ToggleModelKeyAllocated(app,
                mikumikustudio::mdl::BoneKeys(ActiveModel(app))[boneType],
                shiftActive);
        } else if (boneType == -10) {               // loc_448889 (row 0)
            ToggleModelKeyAllocated(app,
                mikumikustudio::mdl::BoneKeys(ActiveModel(app))[0],
                shiftActive);
        } else if (boneType == -1) {                // loc_448AB8
            StageRangeRow(app, boneType, shiftActive,
                          Div13(x - 0x64), Div14(y - 0xA0));
        } else if (boneType == -2) {                // loc_448AF1
            StageRangeRow(app, boneType, shiftActive,
                          Div13(x - 0x64), Div14(y - 0xA0));
        }
        // loc_448CFB: none of the three stages hit -> wholesale
        // active-model flag clear (unless mode == 3)
        if (ikType == 0 && morphType == 0 && boneType == 0 && !shiftActive)
            ClearActiveModelFlags(app);
        SelectionStats(app, x, y, hwnd);           // loc_448EEB
        return;
    }
}

// ---- S7 (VA 0x44A468-0x44A850): frame header strip (y in (144,160)) ---------
// Reached when the main-area gate fails; the strip gate in the original is
// (x < sidebar-0x12) && y in (0x90, 0xA0) && x > 0x64.  Snapshots edited
// bones, seeks every model to the clicked frame, then repaints and resyncs
// physics/audio.
static void HandleLButtonDown_FrameStripHit(MMDApp* app, HWND hwnd,
                                            std::int32_t x, std::int32_t y,
                                            std::int32_t sidebar) {
    if (app->PlaybackActive() != 0)
        return;
    app->CameraAttachmentTransformSuppressed() = 0;
    // Snapshot every model with edited bones, then clear its edit flags.
    for (int i = 0; i < kModelSlotCount; ++i) {
        unsigned char* model = ModelAtSlot(app, i);
        if (model == nullptr)
            continue;
        mdl::ModelRecord* modelRecord = mdl::Mdl(model);
        const std::int32_t n =
            static_cast<std::int32_t>(modelRecord->boneCount);
        if (n <= 0)
            continue;
        unsigned char* flags = modelRecord->bonePhysicsState;
        int j = 0;
        for (; j < n; ++j)
            if (flags[j] != 0)
                break;
        if (j < n) {
            // original: sub_4A0080(ecx = model, frame = app+0x980)
            SnapshotPoseBeforeFrameChange(model, app->state.currentFrame);
            modelRecord = mdl::Mdl(model);
            flags = modelRecord->bonePhysicsState;
            for (j = 0; j < n; ++j)
                flags[j] = 0;
        }
    }
    // scrollbar 0x1A1 sync (0x44A520-0x44A5BA): frame =
    // (x-100)/13 + scroll offset; EM_SETSEL (0xB1) to the end of the old
    // text, then WM_SETTEXT (0xC2) with the new frame
    const auto clickedFrame=std::int64_t(Div13(x-0x64))+app->state.timelineStartFrame;
    if(clickedFrame<frame_limits::minimum||clickedFrame>frame_limits::maximum)return;
    app->state.currentFrame=static_cast<std::int32_t>(clickedFrame);
    const LRESULT textLen = GetWindowTextLengthA(GetDlgItem(hwnd, panel::kCurrentFrameEdit));
    SendMessageA(GetDlgItem(hwnd, panel::kCurrentFrameEdit), EM_SETSEL, 0, textLen);
    char frameText[0x100];
    sprintf_s(frameText, 0x100u, "%d",
              app->state.currentFrame);
    SendMessageA(GetDlgItem(hwnd, panel::kCurrentFrameEdit), EM_REPLACESEL, 0,
                 reinterpret_cast<LPARAM>(frameText));
    // per-slot frame apply + active-slot panel sync (0x44A5C8-0x44A606)
    for (int i = 0; i < kModelSlotCount; ++i) {
        unsigned char* model = ModelAtSlot(app, i);
        if (model == nullptr)
            continue;
        // original: sub_4B4260(ecx = model, frame = app+0x980,
        //            app+0xA0CC4)
        SeekModelFrame(model,
                  app->state.currentFrame,
                  app->PlaybackPhysicsMode());
        if (i == static_cast<int>(app->SelectedModelSlot())) {
            SyncModelEditControls(model);
        }
    }
    // mode branch (0x44A608-0x44A72B)
    if (app->state.optflag[0] != 0) {  // 760 (0x2F8)
        ReloadModels(app);                            // 0x42E640
        RefreshLightPanel(app);                               // 0x411070
        RefreshSelfShadowPanel(app);                               // 0x411B90
        ApplyGravityTrack(app);                               // 0x412330
        for (int i = 0; i < 0xFF; ++i) {
            if (app->ObjectSlot(i) != nullptr)
                ApplyAccessoryTrack(app, i);                    // 0x413120
        }
        SyncAccessoryEditPanel(app);                               // 0x4134E0
    } else if (app->state.followCameraEnabled != 0) {  // 650648
        // Keep the eye: it owns the viewpoint; clearing its offsets here
        // shifted the view on every edit click. See the eye-ownership note in
        // src/app/timeline_advance.cpp.
        ReloadModels(app);
        RefreshLightPanel(app);
        RefreshSelfShadowPanel(app);
        ApplyGravityTrack(app);
        for (int i = 0; i < 0xFF; ++i) {
            if (app->ObjectSlot(i) != nullptr)
                ApplyAccessoryTrack(app, i);
        }
        SyncAccessoryEditPanel(app);
        const std::int32_t sel =
            app->CameraParentModel();
        if (sel >= 0) {
            unsigned char* selModel = ModelAtSlot(app, sel);
            // original: sub_4970B0(ecx = model at slot sel)
            ModelApplyMorphs(selModel);                        // 0x4970B0
            SetPhysicsMode(selModel, 0, app->ModelSlots(),
                           app->NativePhysicsMode());                    // 0x4A9220
        }
        // Frame selection is not a camera-reference transition. Keep the
        // current eye and let SetupFrameWorldTransform resolve the new tracked
        // model/bone pivot; re-anchoring here applies the camera/reference
        // offset a second time and drifts the model-mode view after scrubbing.
    } else {
        RefreshLightPanel(app);
        RefreshSelfShadowPanel(app);
        EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), TRUE);  // 400
        EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE); // 401
    }
    // scroll clamp + panel repaint (0x44A72D-0x44A766):
    // 0x97C = max(0, 0x980 - (sidebar-0x54)/26) with an unsigned compare
    // Clicking an already visible frame must not recenter the timeline.
    PanelPaint(app);                                  // 0x414610
    // timeline strip redraw + physics sync (0x44A768-0x44A850)
    if (app->state.waveEnabled != 0) {  // 657100 (0xA06CC)
        TimelineDrawTicks(app->state.timelineStartFrame,
                          app->SidebarWidth());
        RECT rc2;
        rc2.left = 6;
        rc2.top = 95;
        rc2.right = sidebar - 3;
        rc2.bottom = 146;
        InvalidateRect(hwnd, &rc2, 0);
        if (app->state.wavPlaysOnFrameMove != 0) {  // 655766 (0xA0196)
            // gate flag read BEFORE the 0xA02B6 store (asm zf capture)
            const bool gate =
                app->state.automaticFrameAdvanceEnabled == 0;  // 656361 (0xA03E9)
            app->AudioSeekReady() = 1;  // 656054 (0xA02B6)
            if (gate) {
                SetFrameNormalized(app->FrameNormalization()); // 0x4C2B80
                const std::int32_t frameMinus1 =
                    app->state.currentFrame - 1;
                // fild + (negative ? fadd 2^32f) + fdiv 30.0 ==
                // (double)(unsigned)frameMinus1 / 30.0
                double t = static_cast<double>(static_cast<std::uint32_t>(frameMinus1)) / 30.0;
                if (t < 0.0)                          // fldz/fcom clamp
                    t = 0.0;
                // original: sub_4C3530(ecx = app+0xCC subsystem, t)
                WaveRestartAt(app->Audio(), t);
            }
        }
    }
    if (app->state.aviBackgroundEnabled == 1)  // 2332 (0x91C)
        AviBgOverlayRefresh(app);                               // 0x4168D0
    app->PhysicsResetPending() = 1;
    return;
}

// ---- S7b (VA 0x44A855-0x44A97D): bone/morph name-row rects ------------------
// Bone rect: X in (9DA05+3, 9DA05+13), Y in (9DA06+bottom-0x8C,
// 9DA06+bottom-0x82), gated on byte 9DA04; keyframe insert (0x4A1510)
// + row marker 0x9DA09 = 1.  Morph rect: same with 9DA07/9DA08, marker 2.
// A miss in both rects falls through to the caller's tail (loc_44A97E).
static void HandleLButtonDown_NameRowRectHit(MMDApp* app, std::int32_t x,
                                             std::int32_t y,
                                             std::int32_t bottom) {
    const std::int32_t boneOff =
        static_cast<std::int8_t>(app->state.interpCurveControlCache[1]) + bottom;
    const std::int32_t boneTop =
        static_cast<std::int8_t>(app->state.interpCurveControlCache[0]);
    if (x < boneTop + 0xD && x > boneTop + 3 &&
        y > boneOff - 0x8C && y < boneOff - 0x82 &&
        app->state.interpCurveUniformFound != 0) {
        if (app->PlaybackActive() != 0)
            return;
        unsigned char* m = ActiveModel(app);
        if (m != nullptr) {
            // original: sub_4A1510(ecx = model, frame = app+0x980)
            SnapshotSelectedKeysForUndo(m, app->state.currentFrame);
        }
        app->PendingTimelineSelectionRow() = TimelineSelectionRow::Bone;
        return;
    }
    const std::int32_t morphOff =
        static_cast<std::int8_t>(app->state.interpCurveControlCache[3]) + bottom;
    const std::int32_t morphTop =
        static_cast<std::int8_t>(app->state.interpCurveControlCache[2]);
    if (x < morphTop + 0xD && x > morphTop + 3 &&
        y > morphOff - 0x8C && y < morphOff - 0x82 &&
        app->state.interpCurveUniformFound != 0) {
        if (app->PlaybackActive() != 0)
            return;
        unsigned char* m = ActiveModel(app);
        if (m != nullptr) {
            SnapshotSelectedKeysForUndo(m, app->state.currentFrame);
        }
        app->PendingTimelineSelectionRow() = TimelineSelectionRow::Morph;
        return;
    }
}

void HandleLButtonDown(MMDApp* app) {
    // ---- S1: guard 0xA0274 + client rect + early-out gate ------------
    // (0x446A70-0x446B00) region dispatch follows; each branch returns
    // straight to the shared tail (loc_44A980).
    if (app->FullscreenMode() != 0)
        return;                                              // loc_44A980

    const HWND hwnd = static_cast<HWND>(app->state.hwnd);  // 657080
    RECT rc;
    GetClientRect(hwnd, &rc);
    const std::int32_t y = app->MouseY();
    const std::int32_t x = app->MouseX();
    const std::int32_t sidebar = app->SidebarWidth();

    // Start a deferred sidebar resize. The current layout remains valid
    // until release, so normal paints need not be suspended. Gates as in the
    // original): y <= bottom-0x9E(158) && x <= sidebar+6 &&
    // [0xA0D38]==0 && sidebar <= x.
    if (y <= rc.bottom - 0x9E && x <= sidebar + 6 &&
        app->FloatingWindow() == nullptr && sidebar <= x) {
        app->SidebarResizeDragging() = 1;
        return;
    }
    // row gates used by every region below (0x446B01-0x446B3A):
    //   var_1A0 = y < bottom - 0xF8 (248) ; var_190 = y > 0xA0 (160)
    const bool rowGateY = y > 0xA0 && y < rc.bottom - 0xF8;

    // ---- S2: margin column (X in (4,20)): row-type display toggle
    if (rowGateY && x > 4 && x < 0x14) {
        HandleLButtonDown_MarginColumnToggle(app, y);
        return;
    }

    // ---- S3: name column (X in (20,95]): bone/morph row selection
    if (rowGateY && x > 0x14 && x <= 0x5F) {
        HandleLButtonDown_NameColumnHit(app, hwnd, y);
        return;
    }

    // ---- S5: main area (X in (95, sidebar-18) && row gates) --------------
    // (0x447390-0x447A4C) gate fail falls into S7 (name-row rects).
    if (rowGateY && x > 0x5F && x < sidebar - 0x12) {
        HandleLButtonDown_TimelineAreaHit(app, hwnd, x, y);
        return;
    }

    // ---- S7: header strip (y in (144,160))
    if (x < sidebar - 0x12 && y > 0x90 && y < 0xA0 && x > 0x64) {
        HandleLButtonDown_FrameStripHit(app, hwnd, x, y, sidebar);
        return;
    }

    // ---- S7b: bone/morph name-row rects
    HandleLButtonDown_NameRowRectHit(app, x, y, rc.bottom);
}

}  // namespace mikumikustudio
