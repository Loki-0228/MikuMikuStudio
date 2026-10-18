#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/animation_curve_editor.hpp"
// ===========================================================================
// Edit-menu frame-line commands + select-all frame groups.
//   VA 0x00439E40  insert frame line (bone / camera)    menu 0xFF,  key I
//   VA 0x0043A650  delete frame line (bone / camera)    menu 0x100, key K
//   VA 0x0043B720  insert frame line (facial / light)   menu 0x101, key U
//   VA 0x0043BB30  delete frame line (facial / light)   menu 0x102, key J
//   SelectFrameGroup factors the inline dispatcher blocks at
//   0x4831EA (0xD9 bone) / 0x4832C8 (0xDA disp-IK-OP) / 0x483258 (0xDC facial)
// ===========================================================================
// All four frame-line commands branch on the edit-state byte app+0x2F8
// (kByteOptflag0): clear  -> the active model's frame tables, set -> the
// global camera (0x374, 84B records) or light (0x378, 40B records) table.
// "Insert frame line" shifts every key with frame >= current frame
// (app+0x980) up by one; "delete frame line" unlinks keys exactly at the
// current frame and shifts everything above down by one, merging a key
// that lands on frame 0 into its predecessor record slot.
//
// The bone variants additionally take an undo snapshot (model+0x26EC ring,
// 30 slots x 28B: type=2 / count / frame / pose ptr / keys ptr); the
// facial/light variants and the camera variants have NO undo in the
// original.  Only records at index >= bone/morph count participate - the
// per-bone/per-morph head records (index == track id) are never moved,
// matching the original's guard `(index < count) ^ 1`.
//
// Ghidra quirks verified against the already-ported undo machinery in
// src/model/model_keyframe_edit.cpp (0x49D410 family):
//   * SBORROW4(cursor,0x1E)==(cursor-0x1E<0)  ==  signed cursor >= 0x1E
//     -> cursor wraps to 0 at 30.
//   * the EnableWindow argument triples are (hwnd, item, bEnable): both
//     commands enable the undo button 0x190 and disable the redo button
//     0x191 - the same pair as the 0xFA paste - corroborated by the
//     cleared redo flag (model+0x31BD = 0) both commands write.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikudancestudio/bone_scale.hpp"
#include <Windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/global_key_layout.hpp"
#include "mikudancestudio/panel_controls.hpp"

namespace mikudancestudio {
namespace {

// App-field offsets (file-local).

template<class K> bool CanInsertFrame(const K* keys,std::size_t capacity,std::size_t heads,std::uint32_t frame) {
    for(std::size_t i=heads;keys&&i<capacity;++i)
        if(keys[i].frame&&keys[i].frame>=frame&&keys[i].frame>=std::uint32_t(frame_limits::maximum))return false;
    return true;
}
unsigned char* ActiveModel(MMDApp* app) {
    return app->SelectedModel();
}

mikudancestudio::mdl::UndoRecord& CurrentUndo(unsigned char* model) {
    return mikudancestudio::mdl::Mdl(model)->undoRings[0].slots[
        mikudancestudio::mdl::Mdl(model)->undoState[0]];
}

// Undo slot header (0x43A055..0x43A2D2 / 0x43AC..0x43B0x in both bone
// frame-line commands): dirty/redo flags, cursor advance with the 30-slot
// wrap, type 2, current frame, then the pose snapshot.
void BeginUndoEdit(unsigned char* model, std::uint32_t frame) {
    mdl::Mdl(model)->undoDirty = 1;
    mdl::Mdl(model)->redoDirty = 0;
    std::int32_t cursor = mikudancestudio::mdl::Mdl(model)->undoState[0] + 1;
    if (cursor >= 0x1E) cursor = 0;
    mikudancestudio::mdl::Mdl(model)->undoState[0] = cursor;
    mikudancestudio::mdl::Mdl(model)->undoState[1] = cursor;
    bone_scale::CaptureUndo(model);
    auto& undo = CurrentUndo(model);
    undo.operation = 2;
    undo.frame = frame;
}

// Pose snapshot: boneCount x 0x24 records {boneIdx, pos(3f), quat(4f),
// selection byte} taken from the bone structs at +320/+332 and the
// selection array model+0x2D98.
void SnapshotPose(unsigned char* model) {
    auto& undo = CurrentUndo(model);
    auto*& slot = undo.bonePose;
    if (slot != nullptr) {
        std::free(slot);
        slot = nullptr;
    }
    const std::int32_t count = mdl::Mdl(model)->boneCount;
    slot = static_cast<mikudancestudio::mdl::BonePoseSnapshot*>(std::malloc(
        static_cast<std::size_t>(count) *
        sizeof(mikudancestudio::mdl::BonePoseSnapshot)));
    std::memset(slot, 0, static_cast<std::size_t>(count) * sizeof(*slot));
    mikudancestudio::mdl::BoneRecord* bones = mikudancestudio::mdl::Bones(model);
    unsigned char* selected = mdl::Mdl(model)->boneSelection;
    for (std::int32_t i = 0; i < count; ++i) {
        auto& out = slot[i];
        mikudancestudio::mdl::BoneRecord* bone = &bones[i];
        out.boneIndex = i;
        std::memcpy(out.position, bone->trans, sizeof out.position);
        std::memcpy(out.rotation, bone->rotQuat, sizeof out.rotation);
        out.physicsDisabled = selected[i];
    }
}

// Key-record snapshot buffer: count field zeroed, keys buffer reallocated
// (freed then malloc'ed and zeroed; original 0x43A4DE..0x43A555).
void AllocUndoKeys(unsigned char* model, std::size_t bytes) {
    auto& undo = CurrentUndo(model);
    undo.dirty = 0;
    void*& slot = undo.auxiliaryPose;
    if (slot != nullptr) {
        std::free(slot);
        slot = nullptr;
    }
    slot = static_cast<unsigned char*>(std::malloc(bytes));
    std::memset(slot, 0, bytes);
}

// Clear a removed bone-key record (0x43B1F9..0x43B20D / 0x43B637..):
// frame, used flag, position, quaternion (w reset to 1.0f).  prev/next
// (+4/+8) are cleared separately AFTER the interpolation rebuild.
void ClearBoneKeyRecord(mdl::BoneKey& key) {
    key.frame = 0;
    key.allocated = 0;
    std::memset(key.position, 0, sizeof(key.position));
    key.rotation[0] = 0.0f;
    key.rotation[1] = 0.0f;
    key.rotation[2] = 0.0f;
    key.rotation[3] = 1.0f;
}

// Copy every payload field of a bone-key record into another slot
// (0x43B2..0x43B6x merge-into-predecessor): the 16 interpolation bytes
// (+0xC..+0x1B, copied as the original's individual byte/dword stores),
// position, quaternion, used flag.  frame/prev/next are not copied.
void CopyBoneKeyPayload(mdl::BoneKey& dst, const mdl::BoneKey& src) {
    std::memcpy(dst.interpolation, src.interpolation,
                sizeof(dst.interpolation));
    std::memcpy(dst.position, src.position, sizeof(dst.position));
    std::memcpy(dst.rotation, src.rotation, sizeof(dst.rotation));
    dst.allocated = src.allocated;
    dst.physicsDisabled = src.physicsDisabled;
    std::memcpy(dst.reserved, src.reserved, sizeof(dst.reserved));
}

// Merge the complete camera payload while retaining the destination's place
// in the timeline. Attachment and all six timing channels belong to the key.
void CopyCameraKeyPayload(mdl::CameraKey& dst, const mdl::CameraKey& src) {
    const auto frame = dst.frame;
    const auto previous = dst.previous;
    const auto next = dst.next;
    dst = src;
    dst.frame = frame;
    dst.previous = previous;
    dst.next = next;
}

// Zero a removed camera record (0x43A939..0x43A96F): the exact dword/byte
// set of the original - eye[2](+0x18)/target[2](+0x24) and the per-curve
// 2-byte padding are NOT cleared (frame 0 marks the slot free).
// 插值清零字节集按曲线走：每曲线前 4 字节（x64 0x7FF7CB4AA173..0x7FF7CB4AA2E7，
// 即 +0x28..+0x2B/+0x2E..+0x31/+0x34..+0x37/+0x3A..+0x3D）。
void ClearCameraKeyRecord(mdl::CameraKey& key) {
    key.next = 0;
    key.previous = 0;
    key.frame = 0;
    key.perspective = 0;
    key.distance = 0.0f;
    key.fov = 0;
    key.eye[0] = key.eye[1] = 0.0f;
    key.target[0] = key.target[1] = 0.0f;
    for (auto& row : key.interpolation)
        std::memset(row, 0, 4);
    key.selected = 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Defect 7: the two bone/camera frame-line commands moved key records but
// never refreshed anything the user can see.  At a frame that already carries
// a keyframe the *pose* is unchanged by construction - inserting shifts the
// key one frame to the right and the evaluation at `frame` still returns that
// same key's value (the first record of the track), and deleting leaves the
// neighbours' interpolation - so the only observable effect is the key squares
// in the timeline strip.  Nothing invalidated that strip, so the I/K shortcuts
// looked dead.  The commands now (a) return the number of records they
// moved/removed and (b) redraw + invalidate the timeline strip and the readout
// column with the same TimelineDrawTicks/InvalidateRect pair the timeline
// scroll branch (ui_hscroll.cpp, 428) and the separate-window refresh use.
// ---------------------------------------------------------------------------
void RefreshFrameLineViews(MMDApp* app) {
    curve_editor::NotifyOwnedChanged(app->Hwnd());
    PostViewRefresh(app);                             // 0x40D130 readouts
    const HWND hwnd = static_cast<HWND>(app->Hwnd());
    if (hwnd == nullptr) {
        return;
    }
    if (app->state.waveEnabled != 0) {
        TimelineDrawTicks(app->state.timelineStartFrame, app->SidebarWidth());
    }
    RECT strip;
    strip.left = 6;
    strip.top = 95;
    strip.right = app->SidebarWidth() - 3;
    strip.bottom = 146;
    InvalidateRect(hwnd, &strip, FALSE);
}

// ---- VA 0x00439E40 (was Sub439E40): insert frame line (bone / camera) ----
// Returns the number of key records moved right by one frame (0 = no key at or
// after the current frame, i.e. nothing to insert; the undo ring is untouched).
//
// The bone-mode table surgery (undo snapshot + frame shift) is split out so it
// can be asserted without a device/window (tests/panel_key_tests.cpp); the
// command below keeps the original's UI tail (seek, panel, timeline refresh).
int ShiftBoneKeysForFrameLineInsert(MMDApp* app, std::uint32_t cur) {
    unsigned char* model = ActiveModel(app);
    if (model == nullptr) {
        return 0;
    }
    const std::int32_t boneCount = mdl::Mdl(model)->boneCount;
    mdl::BoneKey* keys = mdl::BoneKeys(model);
    if (keys == nullptr) {
        return 0;
    }
    // Runtime pool capacity, resolved once for both sweeps below.
    const int boneKeyCapacity = static_cast<int>(
        mdl::BoneKeyCapacity(*mdl::Mdl(model)));

    if(!CanInsertFrame(keys,boneKeyCapacity,boneCount,cur))return 0;
    int affected = 0;  // stack0xfffffff8 (0x439E75..0x439F9D)
    for (int i = 0; i < boneKeyCapacity; ++i) {
        const std::uint32_t f = keys[i].frame;
        if (f != 0 && i >= boneCount && f >= cur) ++affected;
    }
    // No key at or after the current frame: there is no frame line to push
    // right, so the command is a no-op (undo ring untouched) and reports 0.
    if (affected == 0) return 0;

    EnableWindow(GetDlgItem(app->state.hwnd, panel::kUndoButton), TRUE);
    EnableWindow(GetDlgItem(app->state.hwnd, panel::kRedoButton), FALSE);

    BeginUndoEdit(model, cur);                     // 0x43A02F..
    SnapshotPose(model);                           // 0x43A275..
    // undo 键缓冲按受影响键数分配：x64 0x7FF7CB4A9F98
    // operator new(saturated_mul(v9, 0x40))，v9 即上面数出的 affected。
    AllocUndoKeys(model,                           // 0x43A4DE..
                  static_cast<std::size_t>(affected) * sizeof(mdl::BoneKeySnapshot));
    mdl::BoneKeyVisitClear(*mdl::Mdl(model));      // 0x43A555

    int moved = 0;
    for (int i = 0; i < boneKeyCapacity; ++i) {             // 0x43A56D..
        mdl::BoneKey& key = keys[i];
        const std::uint32_t f = key.frame;
        if (f != 0 && i >= boneCount && f >= cur) {
            AppendBoneKeyToUndo(model, i);                   // snapshot record
            key.frame = f + 1;
            ++moved;
            if (static_cast<std::int32_t>(mdl::Mdl(model)->maxFrame) <
                static_cast<std::int32_t>(f) + 1)
                mdl::Mdl(model)->maxFrame =
                    static_cast<std::int32_t>(f) + 1;
        }
    }
    return moved;
}

int InsertBoneCameraFrameLine(MMDApp* app) {
    const std::uint32_t cur = static_cast<std::uint32_t>(app->CurrentFrame());

    if (app->state.optflag[0] == 0) {
        // ---- bone mode: shift every non-head key at frame >= cur up one
        unsigned char* model = ActiveModel(app);
        const int moved = ShiftBoneKeysForFrameLineInsert(app, cur);
        if (moved == 0) {
            return 0;   // nothing to insert: no seek, no repaint
        }
        SeekModelFrame(model, static_cast<int>(cur),
                  app->PlaybackPhysicsMode());
        PanelPaint(app);
        SelectionReeval(app);
        app->SceneModified() = 1;
        RefreshFrameLineViews(app);
        return moved;
    }

    // ---- camera mode: shift every camera key at frame >= cur up one
    // (0x439FBB..0x439FAA; only the 0x374 table moves)
    auto* keys = app->CameraKeys();
    if(!CanInsertFrame(keys,mdl::KeyPoolCapacity(keys),1,cur))return 0;
    int moved = 0;
    for (std::size_t r = 0, capacity = mdl::KeyPoolCapacity(keys); r < capacity; ++r) {
        mdl::CameraKey& key = keys[r];
        const std::uint32_t f = key.frame;
        if (f != 0 && f >= cur) {
            key.frame = f + 1;
            ++moved;
            if (app->LastRegisteredFrame() < static_cast<std::int32_t>(f + 1))
                app->LastRegisteredFrame() = static_cast<std::int32_t>(f + 1);
        }
    }
    ReloadModels(app);   // VA 0x0042E640 (camera key seek/evaluate)
    PanelPaint(app);
    app->SceneModified() = 1;
    RefreshFrameLineViews(app);
    return moved;
}

// ---- VA 0x0043A650 (was Sub43A650): delete frame line (bone / camera) ----
// Bone-mode table surgery (undo snapshot + unlink/shift/merge), split out for
// the same headless-assertability reason as the insert twin above.  Returns the
// number of key records changed: 0 = nothing on or above the current frame
// (no undo entry, tables untouched).
int ShiftBoneKeysForFrameLineDelete(MMDApp* app, std::uint32_t cur,
                                    bool autoInterp) {
    unsigned char* model = ActiveModel(app);
    if (model == nullptr) {
        return 0;
    }
    const std::int32_t boneCount = mdl::Mdl(model)->boneCount;
    mdl::BoneKey* keys = mdl::BoneKeys(model);
    if (keys == nullptr) {
        return 0;
    }
    // Runtime pool capacity, resolved once for both sweeps below.
    const int boneKeyCapacity = static_cast<int>(
        mdl::BoneKeyCapacity(*mdl::Mdl(model)));

    // Undo sizing (0x43A675..0x43A72E): a key exactly at the current
    // frame consumes three snapshot slots (prev/self/next), a key
    // above it one.
    int changed = 0;
    int affected = 0;  // uStack_c
    for (int i = 0; i < boneKeyCapacity; ++i) {
        const std::uint32_t f = keys[i].frame;
        if (i >= boneCount && f != 0) {
            if (f == cur) affected += 3;
            else if (cur < f) affected += 1;
        }
    }
    // Nothing on the current frame line and nothing above it: report 0 and
    // leave the undo ring untouched ("no key to delete").
    if (affected == 0) return 0;

    EnableWindow(GetDlgItem(app->state.hwnd, panel::kUndoButton), TRUE);
    EnableWindow(GetDlgItem(app->state.hwnd, panel::kRedoButton), FALSE);

    BeginUndoEdit(model, cur);                     // 0x43AB9C..
    SnapshotPose(model);                           // 0x43ACF2..
    AllocUndoKeys(model,                           // 0x43AF60..
                  static_cast<std::size_t>(affected) * sizeof(mdl::BoneKeySnapshot));
    mdl::BoneKeyVisitClear(*mdl::Mdl(model));      // 0x43AFD3

    for (int i = 0; i < boneKeyCapacity; ++i) {             // 0x43B018..
        mdl::BoneKey& key = keys[i];
        const std::uint32_t f = key.frame;

        if (f != 0 && i >= boneCount && f == cur) {
            // Key exactly at the current frame: unlink and clear.
            ++changed;
            const std::int32_t prev = static_cast<std::int32_t>(key.previous);
            const std::int32_t next = static_cast<std::int32_t>(key.next);
            AppendBoneKeyToUndo(model, prev);                // 0x43B06B
            AppendBoneKeyToUndo(model, i);                   // 0x43B07F
            AppendBoneKeyToUndo(model, next);                // 0x43B09D
            keys[prev].next = next;
            keys[next].previous = prev;
            // The key is gone: its scale matrix and its curve both go, so the
            // slot cannot hand either to a later key (bone_scale::ClearKey).
            bone_scale::ClearKey(model, i);
            ClearBoneKeyRecord(key);               // 0x43B1F9..
            if (autoInterp) {                      // 0x43B20D..
                for (int lane = 0; lane < 4; ++lane) {
                    const std::int32_t n =
                        static_cast<std::int32_t>(key.next);
                    RebuildBoneKeyInterpolation(model, n == 0
                                      ? static_cast<std::int32_t>(
                                            key.previous) : n,
                              lane);
                }
            }
            key.next = 0;
            key.previous = 0;
            continue;
        }

        if (f != 0 && i >= boneCount && cur < f) {
            // Key above the current frame: shift down by one (the frame
            // line at `cur` is pulled down, so this counts as a change).
            ++changed;
            AppendBoneKeyToUndo(model, i);                   // 0x43B28B
            const std::uint32_t nf = f - 1;
            key.frame = nf;
            if (nf == 0) {
                // Landed on frame 0: merge the payload into the
                // predecessor record slot (0x43B2B0..0x43B630).
                const std::int32_t prev =
                    static_cast<std::int32_t>(key.previous);
                mdl::BoneKey& dst = keys[prev];
                dst.next = key.next;
                bone_scale::CopyKey(model, prev, i);
                CopyBoneKeyPayload(dst, key);
                const std::int32_t next =
                    static_cast<std::int32_t>(key.next);
                if (next != 0)
                    keys[next].previous = prev;
                if (autoInterp) {                  // 0x43B68E..
                    for (int lane = 0; lane < 4; ++lane) {
                        const std::int32_t n =
                            static_cast<std::int32_t>(key.next);
                        RebuildBoneKeyInterpolation(model,
                                  n == 0
                                      ? static_cast<std::int32_t>(
                                            key.previous) : n,
                                  lane);
                    }
                }
                bone_scale::ClearKey(model, i);
                ClearBoneKeyRecord(key);           // 0x43B637..
            }
        }
    }
    return changed;
}

int DeleteBoneCameraFrameLine(MMDApp* app) {
    const std::uint32_t cur = static_cast<std::uint32_t>(app->CurrentFrame());

    if (app->state.optflag[0] == 0) {
        // ---- bone mode
        unsigned char* model = ActiveModel(app);
        // Auto-interpolation rebuild gate: checkbox 0x212 (0x43AFF4).
        const bool autoInterp =
            SendMessageA(GetDlgItem(app->state.hwnd,
                                    panel::kPhysicsFrameCheckbox),
                         BM_GETCHECK, 0, 0) == 1;
        const int changed = ShiftBoneKeysForFrameLineDelete(app, cur, autoInterp);
        if (changed == 0) {
            return 0;   // nothing to delete: no seek, no repaint
        }
        SeekModelFrame(model, static_cast<int>(cur),
                  app->PlaybackPhysicsMode());
        PanelPaint(app);
        SelectionReeval(app);
        app->SceneModified() = 1;
        RefreshFrameLineViews(app);
        return changed;
    }

    // ---- camera mode (0x43A905..0x43A9B6)
    auto* keys = app->CameraKeys();
    int changed = 0;
    for (std::size_t r = 0, capacity = mdl::KeyPoolCapacity(keys); r < capacity; ++r) {
        mdl::CameraKey& key = keys[r];
        const std::uint32_t f = key.frame;

        if (f != 0 && f == cur) {
            // Unlink and clear (no undo for the camera table).
            const std::int32_t prev = key.previous;
            const std::int32_t next = key.next;
            keys[prev].next = next;
            keys[next].previous = prev;
            ClearCameraKeyRecord(key);
            ++changed;
            continue;
        }

        const std::uint32_t f2 = key.frame;
        if (f2 != 0 && cur < f2) {
            const std::uint32_t nf = f2 - 1;
            key.frame = nf;
            ++changed;
            if (nf == 0) {
                // Merge into the predecessor slot; the emptied record is
                // left as-is apart from its frame number.
                const std::int32_t prev = key.previous;
                mdl::CameraKey& dst = keys[prev];
                dst.next = key.next;
                CopyCameraKeyPayload(dst, key);
                const std::int32_t next = key.next;
                if (next != 0)
                    keys[next].previous = prev;
            }
        }
    }
    ReloadModels(app);
    PanelPaint(app);
    app->SceneModified() = 1;
    RefreshFrameLineViews(app);
    return changed;
}

// ---- VA 0x0043B720 (was Sub43B720): insert frame line (facial / light) ---
// Returns the number of morph/light key records moved right by one frame.
int InsertFacialLightFrameLine(MMDApp* app) {
    const std::uint32_t cur = static_cast<std::uint32_t>(app->CurrentFrame());
    int changed = 0;

    if (app->state.optflag[0] != 0) {
        // ---- light mode: shift every light key at frame >= cur up one
        // (0x43B72D..0x43B878; only the 0x378 table moves; no undo)
        auto* keys = app->LightKeys();
        if(!CanInsertFrame(keys,mdl::KeyPoolCapacity(keys),1,cur))return 0;
        for (std::size_t r = 0, capacity = mdl::KeyPoolCapacity(keys); r < capacity; ++r) {
            mdl::LightKey& key = keys[r];
            const std::uint32_t f = key.frame;
            if (f != 0 && f >= cur) {
                key.frame = f + 1;
                ++changed;
                if (app->LastRegisteredFrame() < static_cast<std::int32_t>(f + 1))
                    app->LastRegisteredFrame() = static_cast<std::int32_t>(f + 1);
            }
        }
        RefreshLightPanel(app);
        PanelPaint(app);
        app->SceneModified() = 1;
        RefreshFrameLineViews(app);
        return changed;
    }

    // ---- facial mode: morph keys, 0x14 stride x 20000 (no undo)
    unsigned char* model = ActiveModel(app);
    const std::int32_t morphCount = mdl::Mdl(model)->morphCount;
    mdl::MorphKey* keys = mdl::MorphKeys(model);
    const auto morphCapacity1 = mdl::MorphKeyCapacity(mdl::MorphKeys(model));
    if(!CanInsertFrame(keys,morphCapacity1,morphCount,cur))return 0;
    for (int i = 0; i < morphCapacity1; ++i) {
        mdl::MorphKey& key = keys[i];
        const std::uint32_t f = key.frame;
        if (f != 0 && i >= morphCount && f >= cur) {
            key.frame = f + 1;
            ++changed;
            if (static_cast<std::int32_t>(mdl::Mdl(model)->maxFrame) <
                static_cast<std::int32_t>(f) + 1)
                mdl::Mdl(model)->maxFrame =
                    static_cast<std::int32_t>(f) + 1;
        }
    }
    SeekModelFrame(model, static_cast<int>(cur),
              app->PlaybackPhysicsMode());
    PanelPaint(app);
    SelectionReeval(app);
    app->SceneModified() = 1;
    RefreshFrameLineViews(app);
    return changed;
}

// ---- VA 0x0043BB30 (was Sub43BB30): delete frame line (facial / light) ---
// Returns the number of morph/light key records the command changed (removed
// from the frame line, or pulled down by one frame).
int DeleteFacialLightFrameLine(MMDApp* app) {
    const std::uint32_t cur = static_cast<std::uint32_t>(app->CurrentFrame());
    int changed = 0;

    if (app->state.optflag[0] == 0) {
        // ---- facial mode: morph keys, 0x14 stride x 20000
        unsigned char* model = ActiveModel(app);
        const std::int32_t morphCount = mdl::Mdl(model)->morphCount;
        mdl::MorphKey* keys = mdl::MorphKeys(model);
        const auto morphCapacity2 = mdl::MorphKeyCapacity(mdl::MorphKeys(model));
        for (int i = 0; i < morphCapacity2; ++i) {
            mdl::MorphKey& key = keys[i];
            const std::uint32_t f = key.frame;

            if (f != 0 && i >= morphCount && f == cur) {
                // Unlink and clear (0x43BB44..0x43BBB6); no undo.
                ++changed;
                const std::int32_t prev = key.previous;
                const std::int32_t next = key.next;
                keys[prev].next = next;
                keys[next].previous = prev;
                key.frame = 0;
                key.allocated = 0;
                key.value = 0.0f;
                key.next = 0;
                key.previous = 0;
                continue;
            }

            if (f != 0 && i >= morphCount && cur < f) {
                const std::uint32_t nf = f - 1;
                key.frame = nf;
                ++changed;
                if (nf == 0) {
                    // Merge the payload (+0xC value, +0x10 used flag)
                    // into the predecessor slot (0x43BC0E..0x43BC5F);
                    // the emptied record keeps only frame 0.
                    const std::int32_t prev = key.previous;
                    mdl::MorphKey& dst = keys[prev];
                    dst.next = key.next;
                    dst.allocated = key.allocated;
                    std::memcpy(dst.reserved, key.reserved,
                                sizeof(dst.reserved));
                    dst.value = key.value;
                    const std::int32_t next = key.next;
                    if (next != 0)
                        keys[next].previous = prev;
                }
            }
        }
        SeekModelFrame(model, static_cast<int>(cur),
                  app->PlaybackPhysicsMode());
        PanelPaint(app);
        SelectionReeval(app);
        app->SceneModified() = 1;
        RefreshFrameLineViews(app);
        return changed;
    }

    // ---- light mode: 40B records x 10000 (0x43BC64..0x43BC88)
    auto* keys = app->LightKeys();
    for (std::size_t r = 0, capacity = mdl::KeyPoolCapacity(keys); r < capacity; ++r) {
        mdl::LightKey& key = keys[r];
        const std::uint32_t f = key.frame;

        if (f != 0 && f == cur) {
            ++changed;
            const std::int32_t prev = key.previous;
            const std::int32_t next = key.next;
            keys[prev].next = next;
            keys[next].previous = prev;
            key.next = 0;
            key.previous = 0;
            key.frame = 0;
            key.selected = 0;
            std::memset(key.reserved1, 0, sizeof(key.reserved1));
            continue;
        }

        const std::uint32_t f2 = key.frame;
        if (f2 != 0 && cur < f2) {
            const std::uint32_t nf = f2 - 1;
            key.frame = nf;
            ++changed;
            if (nf == 0) {
                const std::int32_t prev = key.previous;
                mdl::LightKey& dst = keys[prev];
                dst.next = key.next;
                std::memcpy(dst.direction, key.direction,
                            sizeof(dst.direction));
                std::memcpy(dst.color, key.color, sizeof(dst.color));
                const std::int32_t next = key.next;
                if (next != 0)
                    keys[next].previous = prev;
            }
        }
    }
    RefreshLightPanel(app);
    PanelPaint(app);
    app->SceneModified() = 1;
    RefreshFrameLineViews(app);
    return changed;
}

// ---- SelectFrameGroup -------------------------------------------------------
// Dispatcher inline blocks 0x4831EA (0xD9) / 0x4832C8 (0xDA) / 0x483258
// (0xDC): clear the used-flag of every record, then mark it selected with
// a full dword store when it is a head record (index < track count, bone
// and facial variants) or carries a key (frame != 0).  The 0xDA variant
// additionally forces record 0's flag after the loop.
void SelectFrameGroup(MMDApp* app, int group) {
    unsigned char* model = ActiveModel(app);

    if (group == 0) {
        // 0xD9: all bone frames (0x26E0 table, 0x3C x the runtime capacity)
        mdl::BoneKey* keys = mdl::BoneKeys(model);
        const std::int32_t boneCount = mdl::Mdl(model)->boneCount;
        const int boneKeyCapacity = static_cast<int>(
            mdl::BoneKeyCapacity(*mdl::Mdl(model)));
        for (int i = 0; i < boneKeyCapacity; ++i) {
            mdl::BoneKey& key = keys[i];
            key.allocated = 0;
            if (i < boneCount || key.frame != 0) {
                key.allocated = 1;
                key.physicsDisabled = 0;
                std::memset(key.reserved, 0, sizeof(key.reserved));
            }
        }
    } else if (group == 1) {
        // 0xDA: all disp/IK/OP frames (0x26E8 table, 0x1C x 1000)
        mdl::DisplayKey* frames = mdl::DisplayKeys(model);
        for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(frames); i < capacity; ++i) {
            mdl::DisplayKey& key = frames[i];
            key.allocated = 0;
            if (key.frame != 0) {
                key.allocated = 1;
                std::memset(key.reserved1, 0, sizeof(key.reserved1));
            }
        }
        frames[0].allocated = 1;
        std::memset(frames[0].reserved1, 0, sizeof(frames[0].reserved1));
    } else {
        // 0xDC: all facial frames (0x26E4 table, 0x14 x 20000)
        mdl::MorphKey* frames = mdl::MorphKeys(model);
        const std::int32_t morphCount = mdl::Mdl(model)->morphCount;
        const auto morphCapacity3 = mdl::MorphKeyCapacity(mdl::MorphKeys(model));
        for (int i = 0; i < morphCapacity3; ++i) {
            mdl::MorphKey& key = frames[i];
            key.allocated = 0;
            if (i < morphCount || key.frame != 0) {
                key.allocated = 1;
                std::memset(key.reserved, 0, sizeof(key.reserved));
            }
        }
    }

    PanelPaint(app);
    SelectionReeval(app);
}

}  // namespace mikudancestudio
