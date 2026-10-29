#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/key_pool.hpp"
// ===========================================================================
// Model keyframe edit helpers used by the left frame editor.
//   0x0049D410  append one touched bone-key record to the undo slot
//               (was Sub49D410 -> AppendBoneKeyToUndo)
//   0x004A0080  SnapshotPoseBeforeFrameChange (was Sub4A0080): snapshot
//               the current pose/selection before changing frame
//   0x004A02C0  SyncModelEditControls (was Sub4A02C0): synchronize model
//               manipulation controls
//   0x004A1510  SnapshotSelectedKeysForUndo (was Sub4A1510): create an
//               undo snapshot for selected display keys
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/sequence_scatter.hpp"
#include <Windows.h>
#include <CommCtrl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <memory>
#include "mikumikustudio/mmd_app.hpp"
#include <utility>

#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/bone_pose_safety.hpp"
#include "mikumikustudio/physics_bake.hpp"
#include "mikumikustudio/panel_controls.hpp"

namespace mikumikustudio {
namespace {

mdl::UndoRecord& CurrentUndo(unsigned char* model) {
    return mdl::Mdl(model)->undoRings[0].slots[
        mdl::Mdl(model)->undoState[0]];
}

mdl::UndoRecord& UndoAt(unsigned char* model, int index) {
    return mdl::Mdl(model)->undoRings[0].slots[index];
}

mdl::UndoRecord& RedoAt(unsigned char* model, int index) {
    return mdl::Mdl(model)->undoRings[1].slots[index];
}

void SetFrameEdit(unsigned char* model, int frame) {
    char text[0x100];
    sprintf_s(text, sizeof(text), "%d", frame);
    const HWND edit = GetDlgItem(*reinterpret_cast<HWND*>(model), panel::kCurrentFrameEdit);
    const int length = GetWindowTextLengthA(edit);
    SendMessageA(edit, EM_SETSEL, 0, length);
    SendMessageA(edit, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(text));
}

void CaptureAndApplyPose(unsigned char* model,
                         const mdl::BonePoseSnapshot* source,
                         mdl::BonePoseSnapshot* capture, int count) {
    mikumikustudio::mdl::BoneRecord* bones = mikumikustudio::mdl::Bones(model);
    unsigned char* selected = mdl::Mdl(model)->bonePhysicsState;
    for (int i = 0; i < count; ++i) {
        const auto& in = source[i];
        auto& out = capture[i];
        const int index = in.boneIndex;
        auto* bone = &bones[index];
        out.boneIndex = index;
        std::memcpy(out.position, bone->trans, sizeof out.position);
        std::memcpy(out.rotation, bone->rotQuat, sizeof out.rotation);
        out.physicsDisabled = selected[index];
        std::memcpy(bone->trans, in.position, sizeof in.position);
        std::memcpy(bone->rotQuat, in.rotation, sizeof in.rotation);
        selected[index] = in.physicsDisabled;
    }
}

// The legacy pose flag holds edit dirtiness, not the live solver switch.
// Type-5 commits keep that switch separately without changing the snapshot ABI.
void RestoreBakedPhysics(unsigned char* model, const mdl::BonePoseSnapshot* source,
                         mdl::BonePoseSnapshot* capture, int count, int physicsMode) {
    auto* bones = mdl::Bones(model);
    for (int i = 0; i < count; ++i) {
        auto& bone = bones[source[i].boneIndex];
        if (capture) capture[i].reserved[0] = bone.physicsDisabled;
        const auto disabled = source[i].reserved[0];
        if (bone.hasRigidBody && physicsMode >= 2 && bone.physicsDisabled != disabled)
            NotifyBonePhysicsMode(model, source[i].boneIndex, disabled);
        bone.physicsDisabled = disabled;
    }
}

void ApplyPose(unsigned char* model, const mdl::BonePoseSnapshot* source,
               int count) {
    mikumikustudio::mdl::BoneRecord* bones = mikumikustudio::mdl::Bones(model);
    unsigned char* selected = mdl::Mdl(model)->bonePhysicsState;
    for (int i = 0; i < count; ++i) {
        const auto& in = source[i];
        const int index = in.boneIndex;
        auto* bone = &bones[index];
        std::memcpy(bone->trans, in.position, sizeof in.position);
        std::memcpy(bone->rotQuat, in.rotation, sizeof in.rotation);
        selected[index] = in.physicsDisabled;
    }
}

void AdvanceUndo(unsigned char* model) {
    mdl::ModelRecord& record = *mdl::Mdl(model);
    record.undoState[0] += 1;
    record.undoDirty = 1;
    record.redoDirty = 0;
    if (record.undoState[0] >= 0x1E)
        record.undoState[0] = 0;
    record.undoState[1] = record.undoState[0];
    bone_scale::CaptureUndo(model);
}

void ReplaceBuffer(void*& slot, std::size_t bytes) {
    if (slot != nullptr) {
        std::free(slot);
        slot = nullptr;
    }
    slot = ::operator new(bytes);
    std::memset(slot, 0, bytes);
}

// Type 7 extends a deletion's bone snapshots with the morph and display/IK
// before-image. These channels used to delete without consuming any undo slot,
// so Ctrl+Z restored an older bone operation instead of the latest deletion.
// The tail is pointer-free except for DisplayKey's ignored pointer fields;
// owned IK/selector values are copied after each record and restored in place.
struct DeleteAuxHeader {
    std::size_t morphCapacity, displayCapacity, ikCount, selectorCount;
    unsigned maxFrame;
    int searchCursor;
};
std::size_t DeleteAuxSize(const DeleteAuxHeader& h) {
    return sizeof(h) + h.morphCapacity * sizeof(mdl::MorphKey) +
        h.displayCapacity * (sizeof(mdl::DisplayKey) + h.ikCount + h.selectorCount * sizeof(mdl::BoneReference));
}
std::size_t DeleteAuxOffset(const mdl::UndoRecord& entry) {
    return static_cast<std::size_t>(entry.reserved0) * 3 * sizeof(mdl::BoneKeySnapshot);
}
DeleteAuxHeader DeleteAuxLayout(const mdl::ModelRecord& m, bool morph, bool display) {
    return {morph ? mdl::MorphKeyCapacity(m.morphKeys) : 0,
            display ? static_cast<std::size_t>(mdl::KeyPoolCapacity(m.displayKeys)) : 0,
            m.ikChainCount, m.boneOrderCount, m.maxFrame, m.searchCursor};
}
void CaptureDeleteAux(const mdl::ModelRecord& m, const DeleteAuxHeader& h, void* destination) {
    auto* bytes = static_cast<unsigned char*>(destination);
    std::memcpy(bytes, &h, sizeof(h)); bytes += sizeof(h);
    if (h.morphCapacity) {
        std::memcpy(bytes, m.morphKeys, h.morphCapacity * sizeof(mdl::MorphKey));
        bytes += h.morphCapacity * sizeof(mdl::MorphKey);
    }
    for (std::size_t i = 0; i < h.displayCapacity; ++i) {
        const auto& key = m.displayKeys[i];
        std::memcpy(bytes, &key, sizeof(key)); bytes += sizeof(key);
        if (h.ikCount) {
            if (mdl::IkStates(key)) std::memcpy(bytes, mdl::IkStates(key), h.ikCount);
            else std::memset(bytes, 1, h.ikCount);
        }
        bytes += h.ikCount;
        if (h.selectorCount) {
            if (mdl::SelectorStates(key)) std::memcpy(bytes, mdl::SelectorStates(key), h.selectorCount * sizeof(mdl::BoneReference));
            else std::memset(bytes, 0, h.selectorCount * sizeof(mdl::BoneReference));
        }
        bytes += h.selectorCount * sizeof(mdl::BoneReference);
    }
}
void ApplyDeleteAux(mdl::ModelRecord& m, const void* snapshot) {
    auto* bytes = static_cast<const unsigned char*>(snapshot);
    DeleteAuxHeader h; std::memcpy(&h, bytes, sizeof(h)); bytes += sizeof(h);
    if (h.morphCapacity) {
        std::memcpy(m.morphKeys, bytes, h.morphCapacity * sizeof(mdl::MorphKey));
        bytes += h.morphCapacity * sizeof(mdl::MorphKey);
        // Registration can grow a pool. Keep allocation ownership, but remove
        // keys in the new tail when restoring a pre-growth snapshot.
        std::fill(m.morphKeys + h.morphCapacity,
                  m.morphKeys + mdl::MorphKeyCapacity(m.morphKeys), mdl::MorphKey{});
    }
    for (std::size_t i = 0; i < h.displayCapacity; ++i) {
        auto& key = m.displayKeys[i];
        auto* ik = key.ikStates; auto* selectors = key.selectorStates;
        std::memcpy(&key, bytes, sizeof(key)); bytes += sizeof(key);
        key.ikStates = ik; key.selectorStates = selectors;
        if (h.ikCount && ik) std::memcpy(ik, bytes, h.ikCount);
        bytes += h.ikCount;
        if (h.selectorCount && selectors) std::memcpy(selectors, bytes, h.selectorCount * sizeof(mdl::BoneReference));
        bytes += h.selectorCount * sizeof(mdl::BoneReference);
    }
    if (h.displayCapacity) for (std::size_t i = h.displayCapacity;
            i < std::size_t(mdl::KeyPoolCapacity(m.displayKeys)); ++i) {
        auto& key = m.displayKeys[i];
        auto* ik = key.ikStates; auto* selectors = key.selectorStates;
        key = {}; key.ikStates = ik; key.selectorStates = selectors;
        if (ik) std::fill_n(ik, h.ikCount, 1);
        if (selectors) std::fill_n(selectors, h.selectorCount, mdl::BoneReference{});
    }
    m.maxFrame = h.maxFrame; m.searchCursor = h.searchCursor;
}

void SnapshotPose(unsigned char* model, mdl::UndoRecord& undo) {
    const std::int32_t count = mdl::Mdl(model)->boneCount;
    void*& slot = reinterpret_cast<void*&>(undo.bonePose);
    ReplaceBuffer(slot, static_cast<std::size_t>(count) *
                            sizeof(mdl::BonePoseSnapshot));

    auto* dst = static_cast<mdl::BonePoseSnapshot*>(slot);
    mikumikustudio::mdl::BoneRecord* pose = mikumikustudio::mdl::Bones(model);
    unsigned char* selected = mdl::Mdl(model)->bonePhysicsState;
    for (std::int32_t i = 0; i < count; ++i) {
        auto& out = dst[i];
        auto* src = &pose[i];
        out.boneIndex = i;
        std::memcpy(out.position, src->trans, sizeof out.position);
        std::memcpy(out.rotation, src->rotQuat, sizeof out.rotation);
        out.physicsDisabled = selected[i];
    }
}

float BoneKeyValue(const mdl::BoneKey* keys, int index, int lane) {
    return keys[index].position[lane];
}

void ResetBoneInterpolation(mdl::BoneKey& record, int lane) {
    record.interpolation[lane] = 20;
    record.interpolation[lane + 4] = 20;
    record.interpolation[lane + 8] = 107;
    record.interpolation[lane + 12] = 107;
}

// VA 0x0049D4D0: regenerate one automatically-smoothed position lane after
// inserting/overwriting a bone key.  Rotation (lane 3) always receives the
// original default Bezier tuple.
void RebuildBoneInterpolation(unsigned char* model, int index, int lane) {
    mdl::BoneKey* const keys = mdl::BoneKeys(model);
    mdl::BoneKey& record = keys[index];
    if (lane >= 3 || index < static_cast<int>(mdl::Mdl(model)->boneCount)) {
        ResetBoneInterpolation(record, lane);
        return;
    }

    const int previous = static_cast<int>(record.previous);
    const float currentValue = BoneKeyValue(keys, index, lane);
    float previousValue = BoneKeyValue(keys, previous, lane);
    if (previousValue == currentValue) {
        ResetBoneInterpolation(record, lane);
        return;
    }

    if (previous < static_cast<int>(mdl::Mdl(model)->boneCount)) {
        record.interpolation[lane] = 64;
        record.interpolation[lane + 4] = 0;
    } else {
        mdl::BoneKey& previousRecord = keys[previous];
        const int beforePrevious = static_cast<int>(previousRecord.previous);
        const float beforeValue = BoneKeyValue(keys, beforePrevious, lane);
        if ((previousValue - beforeValue) *
                (currentValue - previousValue) > 0.0f) {
            previousRecord.interpolation[lane + 8] = 87;
            previousRecord.interpolation[lane + 12] = 87;
            record.interpolation[lane] = 40;
            record.interpolation[lane + 4] = 40;
        } else {
            previousRecord.interpolation[lane + 8] = 64;
            previousRecord.interpolation[lane + 12] = 127;
            record.interpolation[lane] = 64;
            record.interpolation[lane + 4] = 0;
        }
    }

    const int next = static_cast<int>(record.next);
    if (next == 0) {
        record.interpolation[lane + 8] = 64;
        record.interpolation[lane + 12] = 127;
        return;
    }
    mdl::BoneKey& nextRecord = keys[next];
    const float nextValue = BoneKeyValue(keys, next, lane);
    if ((nextValue - currentValue) *
            (currentValue - previousValue) <= 0.0f) {
        record.interpolation[lane + 8] = 64;
        record.interpolation[lane + 12] = 127;
        nextRecord.interpolation[lane] = 64;
        nextRecord.interpolation[lane + 4] = 0;
    } else {
        record.interpolation[lane + 8] = 87;
        record.interpolation[lane + 12] = 87;
        nextRecord.interpolation[lane] = 40;
        nextRecord.interpolation[lane + 4] = 40;
    }
}

void BoneKeyOverflow(unsigned char* model) {
    static const char kJpOverflow[] =
        "\x93\x6f\x98\x5e\x83\x7c\x83\x43\x83\x93\x83\x67\x90\x94\x82\xaa%d"
        "\x8c\xc2\x82\xf0\x89\x7a\x82\xa6\x82\xdc\x82\xb5\x82\xbd\n"
        "\x82\xb1\x82\xea\x88\xc8\x8f\xe3\x82\xcc\x93\x6f\x98\x5e\x82\xcd\x8d"
        "\x73\x82\xa6\x82\xdc\x82\xb9\x82\xf1\n"
        "\x81\x75\xcc\xda\xb0\xd1\x95\xd2\x8f\x57\x81\x76\x82\xcc\x81\x75\x95"
        "\x73\x97\x70\xcc\xda\xb0\xd1\x8d\xed\x8f\x9c\x81\x76\x82\xf0\x8e\xc0"
        "\x8d\x73\x82\xb5\x82\xc4\x89\xba\x82\xb3\x82\xa2";
    static const char kJpTitle[] = "\xcc\xda\xb0\xd1\x93\x6f\x98\x5e";
    char text[0x100];
    const int capacity =
        static_cast<int>(mdl::BoneKeyCapacity(*mikumikustudio::mdl::Mdl(model)));
    if (mikumikustudio::mdl::Mdl(model)->physicsFlags != 0) {
        sprintf_s(text, sizeof(text),
                  "You cannot regist over %d point\n"
                  "Please execute 'delete unused frame'",
                  capacity);
        MessageBoxA(*reinterpret_cast<HWND*>(model), text,
                    "register frame", 0);
    } else {
        sprintf_s(text, sizeof(text), kJpOverflow,
                  capacity);
        MessageBoxA(*reinterpret_cast<HWND*>(model), text, kJpTitle, 0);
    }
}

}  // namespace

void AppendBoneKeyToUndo(unsigned char* model, int index,  // was Sub49D410, VA 0x0049D410
                         unsigned char* visited) {
    if (model == nullptr || visited == nullptr)
        return;
    if (visited[index] != 0)
        return;

    visited[index] = 1;
    auto& undo = CurrentUndo(model);
    std::int32_t& count = undo.dirty;
    auto* records = static_cast<mdl::BoneKeySnapshot*>(undo.auxiliaryPose);
    records[count] = {index, mdl::BoneKeys(model)[index]};
    ++count;
}

void AppendBoneKeyToUndo(unsigned char* model, int index) {
    if (model == nullptr)
        return;
    // Resolves the visit map for the pool's current capacity.  Loops that call
    // this per record should use the three-argument form with a hoisted map
    // instead, so the capacity lookup does not run per iteration.
    AppendBoneKeyToUndo(model, index,
                        mdl::BoneKeyVisitMapOf(*mdl::Mdl(model)).bytes);
}

int RegisterBonePoseAtFrame(unsigned char* model, int boneIndex,  // was Sub4B38A0, VA 0x004B38A0
              std::uint32_t frame, int mode) {
    if (model == nullptr)
        return static_cast<int>(frame);

    const HWND hwnd = *reinterpret_cast<HWND*>(model);
    const bool autoInterpolation =
        SendMessageA(GetDlgItem(hwnd, panel::kPhysicsFrameCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
    // Non-const: the free-slot scan below may grow the pool and move it, so the
    // pointer is refreshed before it is indexed again.
    mdl::BoneKey* keys = mdl::BoneKeys(model);
    mdl::BoneRecord* const bone = &mdl::Bones(model)[boneIndex];

    int current = boneIndex;
    if (keys[current].frame < frame) {
        for (;;) {
            const int next = static_cast<int>(keys[current].next);
            if (next == 0)
                break;
            current = next;
            if (keys[current].frame >= frame)
                break;
        }
    }

    auto fill = [&](int recordIndex) {
        mdl::BoneKey& record = keys[recordIndex];
        const bool usePhysicsPose = bone->hasRigidBody != 0 &&
            (mode == 1 || (mode >= 2 && bone->physicsDisabled == 0));
        float position[3], rotation[4];
        std::memcpy(position, usePhysicsPose ? bone->ikBackup : bone->trans,
                    sizeof position);
        std::memcpy(rotation, usePhysicsPose ? bone->ikBackup + 3 : bone->rotQuat,
                    sizeof rotation);
        RepairBonePose(position, rotation, bone->trans, bone->rotQuat);
        std::memcpy(record.position, position, sizeof position);
        std::memcpy(record.rotation, rotation, sizeof rotation);
        std::memcpy(bone->trans, position, sizeof position);
        std::memcpy(bone->rotQuat, rotation, sizeof rotation);

        if (bone->hasRigidBody != 0) {
            if (IsDlgButtonChecked(hwnd, panel::kPhysicsCheckbox) == BST_CHECKED) {
                record.physicsDisabled = 0;
                if (mode >= 2 && bone->physicsDisabled != 0)
                    NotifyBonePhysicsMode(model, boneIndex, 0);
                bone->physicsDisabled = 0;
            } else {
                record.physicsDisabled = 1;
                if (bone->physicsDisabled == 0 && mode >= 2)
                    NotifyBonePhysicsMode(model, boneIndex, 1);
                bone->physicsDisabled = 1;
            }
        }

        if (autoInterpolation) {
            for (int lane = 0; lane < 4; ++lane)
                RebuildBoneInterpolation(model, recordIndex, lane);
        } else {
            for (int lane = 0; lane < 4; ++lane)
                ResetBoneInterpolation(record, lane);
        }
        bone_scale::Register(model, boneIndex, recordIndex);
        record.allocated = 1;
        bone_curve::ReconcileRegisteredFrame(model,boneIndex,int(frame));
    };

    // Index rather than a reference: the free-slot scan below may grow the pool
    // and move it, and a reference taken here would dangle afterwards.
    const unsigned currentSlot = static_cast<unsigned>(current);
    if (keys[currentSlot].frame == frame) {
        AppendBoneKeyToUndo(model, current);
        // Automatic interpolation writes both neighbours as well as this key.
        // Capture them before fill so Undo/Redo restore the complete interval.
        if (autoInterpolation) {
            if (keys[current].frame != 0)
                AppendBoneKeyToUndo(model, static_cast<int>(keys[current].previous));
            if (keys[current].next != 0)
                AppendBoneKeyToUndo(model, static_cast<int>(keys[current].next));
        }
        fill(current);
    } else {
        // x64 sub_7FF7CB4E9DB0 @0x7FF7CB4EA46F：空闲槽扫描从持久游标
        // searchCursor（model+0x22B8）起步，命中占用槽时游标随扫描推进、
        // 只增不减——写法与 RegisterBoneKey（key_registrars.cpp）一致。
        // 扫描走到当前块末尾时按需扩容，再到新增的零填充区继续扫描；只有
        // 扩不动（结构上限或分配失败）才弹满表提示。
        int& freeIndex = mdl::Mdl(model)->searchCursor;
        std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(*mdl::Mdl(model));
        for (;;) {
            while (static_cast<std::size_t>(freeIndex) < boneKeyCapacity &&
                   keys[freeIndex].frame != 0) {
                ++freeIndex;
            }
            if (static_cast<std::size_t>(freeIndex) < boneKeyCapacity) break;
            if (!mdl::BoneKeyPoolEnsure(*mdl::Mdl(model), boneKeyCapacity + 1)) {
                BoneKeyOverflow(model);
                return 0;
            }
            keys = mdl::BoneKeys(model);
            boneKeyCapacity = mdl::BoneKeyCapacity(*mdl::Mdl(model));
        }

        mdl::BoneKey& fresh = keys[freeIndex];
        if (keys[currentSlot].frame < frame) {
            AppendBoneKeyToUndo(model, current);
            AppendBoneKeyToUndo(model, freeIndex);
            keys[currentSlot].next = static_cast<std::uint32_t>(freeIndex);
            fresh.previous = static_cast<std::uint32_t>(current);
        } else {
            const int previous = static_cast<int>(keys[currentSlot].previous);
            mdl::BoneKey& previousRecord = keys[previous];
            AppendBoneKeyToUndo(model, previous);
            AppendBoneKeyToUndo(model, current);
            AppendBoneKeyToUndo(model, freeIndex);
            previousRecord.next = static_cast<std::uint32_t>(freeIndex);
            fresh.previous = static_cast<std::uint32_t>(previous);
            keys[currentSlot].previous = static_cast<std::uint32_t>(freeIndex);
            fresh.next = static_cast<std::uint32_t>(current);
        }
        fresh.frame = frame;
        fill(freeIndex);
    }

    if (frame > mikumikustudio::mdl::Mdl(model)->maxFrame)
        mikumikustudio::mdl::Mdl(model)->maxFrame = frame;
    return static_cast<int>(frame);
}

void RegisterSelectedBoneKeys(unsigned char* model, int frame, int mode) {
    if(model && frame<0) {
        negative_frames::RegisterSelectedBones(model,frame);
        return;
    }  // was Sub4C2080, VA 0x004C2080
    if (model == nullptr)
        return;

    const std::int32_t boneCount = mdl::Mdl(model)->boneCount;
    unsigned char* const selected = mdl::Mdl(model)->boneSelection;
    unsigned char* const secondary = mdl::Mdl(model)->bonePhysicsState;
    std::int32_t selectedCount = 0;
    for (std::int32_t i = 0; i < boneCount; ++i) {
        if (selected[i] != 0)
            ++selectedCount;
    }

    const HWND hwnd = *reinterpret_cast<HWND*>(model);
    EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), TRUE);
    EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE);
    AdvanceUndo(model);

    auto& undo = CurrentUndo(model);
    undo.operation = 2;
    undo.frame = frame;
    SnapshotPose(model, undo);
    undo.dirty = 0;
    void*& keysSlot = undo.auxiliaryPose;
    ReplaceBuffer(keysSlot,
                  static_cast<std::size_t>(selectedCount) * 3 * sizeof(mdl::BoneKeySnapshot));

    mdl::BoneKeyVisitClear(*mdl::Mdl(model));
    mdl::BoneKey* const boneKeys = mdl::BoneKeys(model);
    const std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(*mdl::Mdl(model));
    for (std::size_t i = 0; i < boneKeyCapacity; ++i)
        boneKeys[i].allocated = 0;
    mdl::MorphKey* const morphKeys = mdl::MorphKeys(model);
    const auto morphCapacity1 = mdl::MorphKeyCapacity(morphKeys);
    for (std::size_t i = 0; i < morphCapacity1; ++i)
        morphKeys[i].allocated = 0;
    mdl::DisplayKey* const displayKeys = mdl::DisplayKeys(model);
    for (std::size_t i = 0, poolCapacity = static_cast<std::size_t>(mdl::KeyPoolCapacity(mdl::DisplayKeys(model))); i < poolCapacity; ++i)
        displayKeys[i].allocated = 0;

    // The cursor is shared with morph/display registration and is zero on a
    // freshly loaded model. Bone heads also have frame == 0, but are reserved:
    // allocating one overwrites another bone's track or self-links this one,
    // leaving timeline traversal stuck. Reset once for this bone-key batch.
    ResetBoneKeyCursor(model);
    for (std::int32_t i = 0; i < boneCount; ++i) {
        if (selected[i] == 0)
            continue;
        RegisterBonePoseAtFrame(model, i, static_cast<std::uint32_t>(frame), mode);
        secondary[i] = 0;
    }
}

// A frame paste is one transaction across bone, morph and display/IK lanes.
// Reserve three bone records per inserted/overwritten pose for its neighbours.
void BeginModelFramePasteUndo(unsigned char* model, int frame,
                             std::uint32_t bonePasteCount, bool morph, bool display) {
    if (!model || (!bonePasteCount && !morph && !display)) return;
    const HWND hwnd = *reinterpret_cast<HWND*>(model);
    EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), TRUE);
    EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE);
    AdvanceUndo(model);
    auto& undo = CurrentUndo(model);
    const bool auxiliary = morph || display;
    undo.operation = auxiliary ? 7 : 2;
    undo.dirty = 0;
    undo.frame = frame;
    undo.reserved0 = bonePasteCount;
    SnapshotPose(model, undo);
    const std::size_t boneBytes = std::size_t(bonePasteCount) * 3 * sizeof(mdl::BoneKeySnapshot);
    const auto layout = DeleteAuxLayout(*mdl::Mdl(model), morph, display);
    ReplaceBuffer(undo.auxiliaryPose, boneBytes + (auxiliary ? DeleteAuxSize(layout) : 0));
    if (auxiliary)
        CaptureDeleteAux(*mdl::Mdl(model), layout,
            static_cast<unsigned char*>(undo.auxiliaryPose) + boneBytes);
    mdl::BoneKeyVisitClear(*mdl::Mdl(model));
}

void SnapshotPoseBeforeFrameChange(unsigned char* model, int frame) {  // was Sub4A0080
    if (model == nullptr)
        return;
    const std::int32_t count = mdl::Mdl(model)->boneCount;
    unsigned char* selected = mdl::Mdl(model)->bonePhysicsState;
    std::int32_t first = 0;
    while (first < count && selected[first] == 0)
        ++first;
    if (first >= count)
        return;

    AdvanceUndo(model);
    auto& undo = CurrentUndo(model);
    undo.operation = 3;
    undo.dirty = count;
    undo.frame = frame;
    SnapshotPose(model, undo);
}

void SnapshotSelectedKeysForUndo(unsigned char* model, int frame) {  // was Sub4A1510
    if (model == nullptr)
        return;
    mdl::BoneKey* const keys = mdl::BoneKeys(model);
    const std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(*mdl::Mdl(model));
    std::int32_t selectedCount = 0;
    const std::size_t groups = boneKeyCapacity / 6;
    for (std::int32_t group = 0;
         group < static_cast<std::int32_t>(groups);
         ++group) {
        for (int lane = 0; lane < 6; ++lane) {
            if (keys[group * 6 + lane].allocated != 0)
                ++selectedCount;
        }
    }
    // A runtime capacity is not necessarily a multiple of six (growth rounds to
    // 65536 slots), so the tail past the last full group must be counted too: an
    // under-count would size the undo buffer below what AppendBoneKeyToUndo
    // writes.
    for (std::size_t index = groups * 6; index < boneKeyCapacity; ++index) {
        if (keys[index].allocated != 0)
            ++selectedCount;
    }
    if (selectedCount == 0)
        return;

    const HWND hwnd = *reinterpret_cast<HWND*>(model);
    EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), TRUE);
    EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE);

    AdvanceUndo(model);
    auto& undo = CurrentUndo(model);
    undo.operation = 2;
    undo.dirty = 0;
    undo.frame = frame;
    SnapshotPose(model, undo);

    void*& selectedSlot = undo.auxiliaryPose;
    const bool linkCurves = IsDlgButtonChecked(hwnd,panel::kPhysicsFrameCheckbox)==BST_CHECKED;
    ReplaceBuffer(selectedSlot,
                  static_cast<std::size_t>(selectedCount) * (linkCurves?3:1) * sizeof(mdl::BoneKeySnapshot));
    // One capacity lookup and one visit-map resolution for the whole sweep; the
    // pool does not grow here, so both stay valid across the loop.
    mdl::BoneKeyVisitClear(*mdl::Mdl(model));
    unsigned char* const visited =
        mdl::BoneKeyVisitMapOf(*mdl::Mdl(model), boneKeyCapacity).bytes;
    for (std::size_t index = 0; index < boneKeyCapacity; ++index) {
        if (keys[index].allocated != 0) {
            AppendBoneKeyToUndo(model, static_cast<int>(index), visited);
            if(linkCurves)for(auto neighbor : {keys[index].previous,keys[index].next})
                if(neighbor<boneKeyCapacity&&keys[neighbor].frame>0&&keys[neighbor].frame!=keys[index].frame)
                    AppendBoneKeyToUndo(model,int(neighbor),visited);
        }
    }
}

// Original inline block 0x43F15E..0x43F60D in Sub43E970.  DeleteMarkedKeyframes (was Sub4316B0) has
// just made the deletion snapshot at the current cursor.  Type 4 tells
// Undo/Redo to chain that entry with the type-2 snapshot opened here for the
// transformed records which are about to be inserted.
void BeginRangeScaleBoneUndo(unsigned char* model, int frame,
                             int transformedKeyCount) {
    if (model == nullptr || transformedKeyCount <= 0)
        return;

    CurrentUndo(model).operation = 4;

    const HWND hwnd = *reinterpret_cast<HWND*>(model);
    EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), TRUE);
    EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE);
    AdvanceUndo(model);

    auto& undo = CurrentUndo(model);
    undo.operation = 2;
    undo.dirty = 0;
    undo.frame = frame;
    SnapshotPose(model, undo);

    void*& keySlot = undo.auxiliaryPose;
    ReplaceBuffer(keySlot,
                  static_cast<std::size_t>(transformedKeyCount) * 3 * sizeof(mdl::BoneKeySnapshot));
    mdl::BoneKeyVisitClear(*mdl::Mdl(model));
}

// VA 0x004A09E0: delete every marked model-mode key.  The three key arrays
// retain their fixed-capacity slots; non-root records are unlinked and reset,
// while root records keep their track-head role with default values.
// ---- defect 6: Del must cover the SELECTED bones, not just the marked keys --
// The Delete shortcut used to look only at the mark byte (+0x39 allocated) that
// the timeline click / frame-range selection writes.  "Select all bones (494)
// then press Del" therefore did nothing at a frame that carried keys, because
// nothing had ever been marked.  The original's DeleteMarkedKeyframes still
// owns the removal (undo ring, unlink, interpolation rebuild, panel refresh);
// this helper only marks the records that belong to the current frame and to a
// bone whose selection byte (boneSelection, 0x2D94 x86 / 0x3120 x64 - the same
// array 494 writes and 495 reads) is set.  Returns the number of records
// marked; 0 = nothing to delete, tables untouched.
int MarkSelectedBoneKeysAtFrame(unsigned char* model, int frame) {
    if (model == nullptr) {
        return 0;
    }
    mdl::BoneKey* const keys = mdl::BoneKeys(model);
    const std::int32_t boneCount =
        static_cast<std::int32_t>(mikumikustudio::mdl::Mdl(model)->boneCount);
    const unsigned char* const selected =
        mikumikustudio::mdl::Mdl(model)->boneSelection;
    if (keys == nullptr || boneCount <= 0 || selected == nullptr ||
        frame < 0) {
        return 0;
    }
    const std::uint32_t wanted = static_cast<std::uint32_t>(frame);
    const std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(*mdl::Mdl(model));
    int marked = 0;
    for (std::int32_t bone = 0; bone < boneCount; ++bone) {
        if (selected[bone] == 0) {
            continue;
        }
        // Walk this bone's own chain from its head record (index == bone);
        // frame 0 means "empty slot" everywhere in the key tables, so the head
        // record itself can never match wanted != 0.  The chain terminator is
        // index 0, which is also bone 0's head record - the guard counter stops
        // a corrupt self-referencing chain.
        std::int32_t index = bone;
        for (std::size_t guard = 0; guard <= boneKeyCapacity; ++guard) {
            if (index < 0 ||
                static_cast<std::size_t>(index) >= boneKeyCapacity) {
                break;
            }
            mdl::BoneKey& key = keys[index];
            if (key.allocated == 0 && wanted != 0 && key.frame == wanted) {
                key.allocated = 1;
                ++marked;
            }
            const std::int32_t next = static_cast<std::int32_t>(key.next);
            if (next == 0) {
                break;
            }
            index = next;
        }
    }
    return marked;
}

void DeleteMarkedModelKeys(unsigned char* model, int frame) {  // was Sub4A09E0, VA 0x004A09E0
    if (model == nullptr) return;
    mdl::BoneKey* const boneKeys = mdl::BoneKeys(model);
    mdl::MorphKey* const morphKeys = mdl::MorphKeys(model);
    mdl::DisplayKey* const displayKeys = mdl::DisplayKeys(model);
    const std::size_t boneCount = mikumikustudio::mdl::Mdl(model)->boneCount;
    const std::size_t morphCount = mikumikustudio::mdl::Mdl(model)->morphCount;
    // Hoisted once: the sweeps below cover the pool's whole runtime capacity,
    // and this function never grows it.
    const std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(*mdl::Mdl(model));

    int selectedBoneKeys = 0;
    for (std::size_t i = 0; i < boneKeyCapacity; ++i) {
        if (boneKeys[i].allocated != 0)
            ++selectedBoneKeys;
    }
    const auto marked = [](const auto* keys, std::size_t capacity) {
        for (std::size_t i = 0; keys && i < capacity; ++i) if (keys[i].allocated) return true;
        return false;
    };
    const bool morphMarked = marked(morphKeys, mdl::MorphKeyCapacity(morphKeys));
    const bool displayMarked = marked(displayKeys, mdl::KeyPoolCapacity(displayKeys));
    const bool auxiliary = morphMarked || displayMarked;
    if (selectedBoneKeys != 0 || auxiliary) {
        const HWND hwnd = *reinterpret_cast<HWND*>(model);
        EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), TRUE);
        EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE);
        AdvanceUndo(model);
        auto& undo = CurrentUndo(model);
        undo.operation = auxiliary ? 7 : 2;
        undo.dirty = 0;
        undo.frame = frame;
        SnapshotPose(model, undo);
        void*& keySlot = undo.auxiliaryPose;
        const std::size_t boneBytes = static_cast<std::size_t>(selectedBoneKeys) * 3 * sizeof(mdl::BoneKeySnapshot);
        const auto layout = DeleteAuxLayout(*mdl::Mdl(model), morphMarked, displayMarked);
        ReplaceBuffer(keySlot, boneBytes + (auxiliary ? DeleteAuxSize(layout) : 0));
        if (auxiliary) {
            // Store a count, not a byte offset, so large pools cannot truncate it.
            undo.reserved0 = selectedBoneKeys;
            CaptureDeleteAux(*mdl::Mdl(model), layout, static_cast<unsigned char*>(keySlot) + boneBytes);
        }
        mdl::BoneKeyVisitClear(*mdl::Mdl(model));
    }

    // Reset every display/IK key record.
    for (std::size_t i = 0, poolCapacity = static_cast<std::size_t>(mdl::KeyPoolCapacity(mdl::DisplayKeys(model))); i < poolCapacity; ++i) {
        mdl::DisplayKey& rec = displayKeys[i];
        if (rec.allocated == 0) continue;
        const int prev = static_cast<int>(rec.previous);
        const int next = static_cast<int>(rec.next);
        rec.frame = 0;
        if (i != 0) {
            displayKeys[prev].next = static_cast<std::uint32_t>(next);
            displayKeys[next].previous = static_cast<std::uint32_t>(prev);
        }
        rec.allocated = 0;
        rec.visible = 1;
        auto* ik = mdl::IkStates(rec);
        for (std::uint32_t j = 0;
             j < mikumikustudio::mdl::Mdl(model)->ikChainCount; ++j)
            ik[j] = 1;
        auto* selectors = mdl::SelectorStates(rec);
        for (std::uint32_t j = 0;
             j < mikumikustudio::mdl::Mdl(model)->boneOrderCount; ++j) {
            selectors[j].modelIndex = -1;
            selectors[j].boneIndex = 0;
        }
        if (i != 0) rec.next = 0;
        rec.previous = 0;
    }

    // Reset every morph key record.
    const auto morphCapacity2 = mdl::MorphKeyCapacity(morphKeys);
    for (std::size_t i = 0; i < morphCapacity2; ++i) {
        mdl::MorphKey& rec = morphKeys[i];
        if (rec.allocated == 0) continue;
        const int prev = static_cast<int>(rec.previous);
        const int next = static_cast<int>(rec.next);
        rec.frame = 0;
        if (i >= morphCount) {
            morphKeys[prev].next = static_cast<std::uint32_t>(next);
            morphKeys[next].previous = static_cast<std::uint32_t>(prev);
        }
        rec.allocated = 0;
        rec.value = 0.0f;
        if (i >= morphCount) rec.next = 0;
        rec.previous = 0;
    }

    const HWND hwnd = *reinterpret_cast<HWND*>(model);
    const bool autoInterpolation =
        SendMessageA(GetDlgItem(hwnd, panel::kPhysicsFrameCheckbox), BM_GETCHECK, 0, 0) == BST_CHECKED;
    mdl::BoneRecord* const bones = mdl::Bones(model);
    // One visit-map resolution for the whole sweep: this loop calls
    // AppendBoneKeyToUndo up to three times per record.
    unsigned char* const visited =
        mdl::BoneKeyVisitMapOf(*mdl::Mdl(model), boneKeyCapacity).bytes;
    for (std::size_t i = 0; i < boneKeyCapacity; ++i) {
        mdl::BoneKey& rec = boneKeys[i];
        if (rec.allocated == 0) continue;
        const int prev = static_cast<int>(rec.previous);
        const int next = static_cast<int>(rec.next);
        AppendBoneKeyToUndo(model, static_cast<int>(i), visited);
        AppendBoneKeyToUndo(model, prev, visited);
        AppendBoneKeyToUndo(model, next, visited);
        bone_scale::ClearKey(model, static_cast<int>(i));
        rec.frame = 0;
        if (i >= boneCount) {
            boneKeys[prev].next = static_cast<std::uint32_t>(next);
            boneKeys[next].previous = static_cast<std::uint32_t>(prev);
        }
        rec.allocated = 0;
        rec.position[0] = rec.position[1] = rec.position[2] = 0.0f;
        rec.rotation[0] = rec.rotation[1] = rec.rotation[2] = 0.0f;
        rec.rotation[3] = 1.0f;
        if (autoInterpolation) {
            const int neighbour = next != 0 ? next : prev;
            for (int lane = 0; lane < 4; ++lane)
                RebuildBoneInterpolation(model, neighbour, lane);
        }
        if (i >= boneCount) rec.next = 0;
        rec.previous = 0;
        if (i < boneCount && bones[i].hasRigidBody != 0)
            rec.physicsDisabled = 0;
    }
}

namespace bake {
namespace {
struct PreparedBoneCommit {
    unsigned char* model;const Result& result;std::uint64_t required=0;
    mdl::BoneKeySnapshot* oldKeys=nullptr;mdl::BonePoseSnapshot* oldPose=nullptr;
    bone_scale::StagedBakeState scale;
    PreparedBoneCommit(unsigned char* m,const Result& r):model(m),result(r){}
    ~PreparedBoneCommit(){std::free(oldKeys);std::free(oldPose);}
    bool Prepare(){
        if (!model || result.error != Error::None) return false;
        if (result.changes.empty()) return true;
        auto& m = *mdl::Mdl(model);
        if (!m.boneKeys || !m.boneTable || !m.bonePhysicsState) return false;
        // Slots the result needs.  A bake may append keys past the pool's current
        // end, so the pool is grown transactionally below; until that call the
        // record for a slot past the end reads as an empty key, exactly what a
        // never-used slot contains.
        required = mdl::BoneKeyCapacity(m);
        const std::size_t liveCapacity = static_cast<std::size_t>(required);
        for (const auto& change : result.changes) {
            if (change.keyIndex < 0) return false;
            const std::uint64_t slot = static_cast<std::uint64_t>(change.keyIndex) + 1;
            if (slot > required) required = slot;
        }
        if (required > mdl::kBoneKeyCapacityLimit) return false;
        const std::size_t plannedCapacity =
            required > liveCapacity ? static_cast<std::size_t>(required) : liveCapacity;
        for (const auto& change : result.changes)
            if (static_cast<std::size_t>(change.keyIndex) >= plannedCapacity) return false;
        bone_scale::Snapshot scales;
        try {
            scales=bone_scale::Capture(model);
            for(const auto& value:result.scales) {
                if(value.first<0||static_cast<std::size_t>(value.first)>=plannedCapacity||!bone_scale::Valid(value.second))return false;
                if(bone_scale::Identity(value.second))scales.keys.erase(value.first);
                else scales.keys[value.first]=value.second;
            }
        } catch(const std::bad_alloc&) { return false; }
        // Allocate the entire undo entry before changing the timeline or undo cursor.
        oldKeys = static_cast<mdl::BoneKeySnapshot*>(std::malloc(result.changes.size() * sizeof(mdl::BoneKeySnapshot)));
        oldPose = static_cast<mdl::BonePoseSnapshot*>(std::calloc(m.boneCount, sizeof(mdl::BonePoseSnapshot)));
        if (!oldKeys || !oldPose) { return false; }
        for (std::size_t i=0;i<result.changes.size();++i) {
            const int index=result.changes[i].keyIndex;
            // Past the live pool the old record is the zero-filled empty key, which
            // is also what the rollback below has to write back.
            oldKeys[i] = static_cast<std::size_t>(index) < liveCapacity
                             ? mdl::BoneKeySnapshot{index, m.boneKeys[index]}
                             : mdl::BoneKeySnapshot{index, mdl::BoneKey{}};
        }
        for (unsigned i=0;i<m.boneCount;++i) {
            auto& p=oldPose[i];p.boneIndex=i;
            std::memcpy(p.position,m.boneTable[i].trans,sizeof p.position);
            std::memcpy(p.rotation,m.boneTable[i].rotQuat,sizeof p.rotation);
            p.physicsDisabled=m.bonePhysicsState[i];
            p.reserved[0]=m.boneTable[i].physicsDisabled;
        }
        return bone_scale::StageBakeState(model,std::move(scales),plannedCapacity,scale);
    }
    bool Grow(){return result.changes.empty()||mdl::BoneKeyPoolEnsure(*mdl::Mdl(model),required);}
    void Apply(int undoFrame) noexcept {
        if(result.changes.empty())return;
        auto& m=*mdl::Mdl(model);
        m.undoState[0]=scale.ring;m.undoState[1]=scale.ring;m.undoDirty=1;m.redoDirty=0;
        auto& undo=CurrentUndo(model);std::free(undo.bonePose);std::free(undo.auxiliaryPose);
        undo={};undo.operation=5;undo.dirty=static_cast<int>(result.changes.size());undo.frame=undoFrame;
        undo.bonePose=oldPose;undo.auxiliaryPose=oldKeys;oldPose=nullptr;oldKeys=nullptr;
        undo.reserved0=static_cast<int>(m.maxFrame);undo.reserved1=m.searchCursor;
        bone_scale::PublishBakeState(std::move(scale));
        for(const auto& change:result.changes)m.boneKeys[change.keyIndex]=change.key;
        m.maxFrame=result.maxFrame;ResetBoneKeyCursor(model);
        EnableWindow(GetDlgItem(static_cast<HWND>(m.hwnd),panel::kUndoButton),TRUE);
        EnableWindow(GetDlgItem(static_cast<HWND>(m.hwnd),panel::kRedoButton),FALSE);
    }
};
}
bool Commit(unsigned char* model,const Result& result,int undoFrame){
    try{PreparedBoneCommit prepared(model,result);if(!prepared.Prepare()||!prepared.Grow())return false;prepared.Apply(undoFrame);return true;}
    catch(...){return false;}
}
bool CommitScene(MMDApp& app,const SceneResult& result,int undoFrame){
    if(result.error!=Error::None||result.models.empty())return false;
    try{std::vector<std::unique_ptr<PreparedBoneCommit>> staged;
        for(const auto& entry:result.models){bool found=false;
            for(int slot=0;slot<kModelSlotCount;++slot)if(app.ModelSlot(slot)==entry.model&&entry.model)found=true;
            if(!found)return false;
            for(const auto& previous:staged)if(previous->model==entry.model)return false;
            auto prepared=std::make_unique<PreparedBoneCommit>(entry.model,entry.result);
            if(!prepared->Prepare())return false;staged.push_back(std::move(prepared));}
        // Capacity growth can fail, but has not written keys or consumed undo.
        for(auto& prepared:staged)if(!prepared->Grow())return false;
        for(auto& prepared:staged)prepared->Apply(undoFrame);
        return true;
    }catch(...){return false;}
}

// Independent pre-image restore for the layered bake transaction.  The caller
// keeps its own before-image of exactly the slots it is about to write and the
// maxFrame it is about to replace; this writes them back byte for byte without
// going through - or consuming - the user's undo ring.  A failed combined bake
// therefore never needs the undo history to recover, and the undo entry a
// successful Commit left behind still describes the same before-image.
void RestoreKeys(unsigned char* model, const std::vector<mdl::BoneKeySnapshot>& before,
                 unsigned maxFrame) noexcept {
    if (!model) return;
    auto& m = *mdl::Mdl(model);
    if (!m.boneKeys) return;
    const std::size_t capacity = mdl::BoneKeyCapacity(m);
    for (const auto& snapshot : before) {
        if (snapshot.keyIndex < 0 || static_cast<std::size_t>(snapshot.keyIndex) >= capacity) continue;
        m.boneKeys[snapshot.keyIndex] = snapshot.key;
    }
    m.maxFrame = maxFrame;
    ResetBoneKeyCursor(model);
}
}
// VA 0x004A1870: undo one ring entry, capturing the displaced state into the
// parallel redo ring at 0x2A34. Type 4 entries are chained recursively by MMD.
void UndoModelEdit(unsigned char* model, std::int32_t* frame) {  // was Sub4A1870, VA 0x004A1870
    UndoModelEdit(model, frame, 2);
}

void UndoModelEdit(unsigned char* model, std::int32_t* frame, int physicsMode) {
    if (model == nullptr || frame == nullptr) return;
    // Capture edited curves for redo before native keys and scale roll back.
    bone_curve::Synchronize(model);
    int cursor = static_cast<int>(mdl::Mdl(model)->undoState[0]);
    auto& undo = UndoAt(model, cursor);
    const int type = undo.operation;
    if (type == 6) {
        if (!sequence_scatter::Restore(model, undo.auxiliaryPose)) return;
        *frame = static_cast<int>(undo.frame); SetFrameEdit(model, *frame);
        mdl::Mdl(model)->undoState[0] = cursor == 0 ? 29 : cursor - 1;
        SeekModelFrame(model, *frame, physicsMode);
        return;
    }
    if (type == 0) {
        EnableWindow(GetDlgItem(*reinterpret_cast<HWND*>(model), panel::kUndoButton), FALSE);
        mdl::Mdl(model)->undoDirty = 0;
        return;
    }

    auto& redo = RedoAt(model, cursor);
    const int count = undo.dirty;
    redo.dirty = count;
    if (type == 1 || type == 3) {
        redo.operation = 1;
        void*& redoPose = reinterpret_cast<void*&>(redo.bonePose);
        ReplaceBuffer(redoPose, static_cast<std::size_t>(count) *
                                    sizeof(mdl::BonePoseSnapshot));
        CaptureAndApplyPose(model, undo.bonePose, redo.bonePose, count);
        if (type == 3) {
            *frame = static_cast<int>(undo.frame);
            SetFrameEdit(model, *frame);
        }
    } else if (type == 2 || type == 4 || type == 5 || type == 7) {
        redo.operation = type == 5 || type == 7 ? type : 2;
        if (type == 5) {
            redo.reserved0 = static_cast<int>(mdl::Mdl(model)->maxFrame);
            redo.reserved1 = mdl::Mdl(model)->searchCursor;
            mdl::Mdl(model)->maxFrame = static_cast<unsigned>(undo.reserved0);
            mdl::Mdl(model)->searchCursor = undo.reserved1;
        }
        void*& redoKeys = redo.auxiliaryPose;
        std::size_t bytes = static_cast<std::size_t>(count) * sizeof(mdl::BoneKeySnapshot);
        if (type == 7) {
            DeleteAuxHeader layout;
            std::memcpy(&layout, static_cast<const unsigned char*>(undo.auxiliaryPose) + DeleteAuxOffset(undo), sizeof(layout));
            layout = DeleteAuxLayout(*mdl::Mdl(model), layout.morphCapacity != 0, layout.displayCapacity != 0);
            redo.reserved0 = undo.reserved0;
            bytes = DeleteAuxOffset(undo) + DeleteAuxSize(layout);
        }
        ReplaceBuffer(redoKeys, bytes);
        mdl::BoneKeyVisitClear(*mdl::Mdl(model));
        const int boneCount = static_cast<int>(mdl::Mdl(model)->boneCount);
        void*& redoPose = reinterpret_cast<void*&>(redo.bonePose);
        ReplaceBuffer(redoPose, static_cast<std::size_t>(boneCount) *
                                    sizeof(mdl::BonePoseSnapshot));
        *frame = static_cast<int>(undo.frame);
        SetFrameEdit(model, *frame);
        CaptureAndApplyPose(model, undo.bonePose, redo.bonePose, boneCount);
        mdl::BoneKey* const keys = mdl::BoneKeys(model);
        const auto* source = static_cast<const mdl::BoneKeySnapshot*>(undo.auxiliaryPose);
        auto* capture = static_cast<mdl::BoneKeySnapshot*>(redoKeys);
        for (int i = 0; i < count; ++i) {
            const int index = source[i].keyIndex;
            capture[i] = {index, keys[index]};
            keys[index] = source[i].key;
        }
        if (type == 7) {
            const auto* before = static_cast<const unsigned char*>(undo.auxiliaryPose) + DeleteAuxOffset(undo);
            auto* after = static_cast<unsigned char*>(redoKeys) + DeleteAuxOffset(redo);
            DeleteAuxHeader layout; std::memcpy(&layout, before, sizeof(layout));
            layout = DeleteAuxLayout(*mdl::Mdl(model), layout.morphCapacity != 0, layout.displayCapacity != 0);
            CaptureDeleteAux(*mdl::Mdl(model), layout, after);
            ApplyDeleteAux(*mdl::Mdl(model), before);
        }
    }

    if (type == 5)
        RestoreBakedPhysics(model, undo.bonePose, redo.bonePose,
                            static_cast<int>(mdl::Mdl(model)->boneCount), physicsMode);
    bone_scale::Undo(model, cursor);
    if (type == 7) {
        // Undo callers repaint the UI without seeking. Refresh restored morph
        // weights and IK/display state now, keeping the saved unregistered pose.
        SeekModelFrame(model, *frame, physicsMode);
        ApplyPose(model, undo.bonePose, static_cast<int>(mdl::Mdl(model)->boneCount));
    }
    cursor = cursor == 0 ? 29 : cursor - 1;
    mdl::Mdl(model)->undoState[0] = cursor;
    if (UndoAt(model, cursor).operation == 4)
        UndoModelEdit(model, frame, physicsMode);
}

// VA 0x004A2490: redo the parallel-ring entry selected by advancing the
// cursor. Type 2/4 restores complete 60-byte key records byte-for-byte.
void RedoModelEdit(unsigned char* model, std::int32_t* frame) {  // was Sub4A2490, VA 0x004A2490
    RedoModelEdit(model, frame, 2);
}

void RedoModelEdit(unsigned char* model, std::int32_t* frame, int physicsMode) {
    if (model == nullptr || frame == nullptr) return;
    int cursor = static_cast<int>(mdl::Mdl(model)->undoState[0]) + 1;
    if (cursor >= 30) cursor = 0;
    auto& redo = RedoAt(model, cursor);
    const int type = redo.operation;
    if (type == 6) {
        if (!sequence_scatter::Restore(model, redo.auxiliaryPose)) return;
        mdl::Mdl(model)->undoState[0] = cursor;
        *frame = static_cast<int>(redo.frame); SetFrameEdit(model, *frame);
        SeekModelFrame(model, *frame, physicsMode);
        return;
    }
    mdl::Mdl(model)->undoState[0] = cursor;
    if (type == 0) {
        EnableWindow(GetDlgItem(*reinterpret_cast<HWND*>(model), panel::kRedoButton), FALSE);
        mdl::Mdl(model)->redoDirty = 0;
        return;
    }
    bone_scale::Redo(model, cursor);
    const int count = redo.dirty;
    if (type == 1) {
        ApplyPose(model, redo.bonePose, count);
    } else if (type == 2 || type == 4 || type == 5 || type == 7) {
        mdl::BoneKey* const keys = mdl::BoneKeys(model);
        if (type == 5) {
            mdl::Mdl(model)->maxFrame = static_cast<unsigned>(redo.reserved0);
            mdl::Mdl(model)->searchCursor = redo.reserved1;
        }
        const auto* source = static_cast<const mdl::BoneKeySnapshot*>(redo.auxiliaryPose);
        for (int i = 0; i < count; ++i)
            keys[source[i].keyIndex] = source[i].key;
        if (type == 7)
            ApplyDeleteAux(*mdl::Mdl(model), static_cast<const unsigned char*>(redo.auxiliaryPose) + DeleteAuxOffset(redo));
        *frame = static_cast<int>(UndoAt(model, cursor).frame);
        SetFrameEdit(model, *frame);
    }
    if (type == 5)
        RestoreBakedPhysics(model, redo.bonePose, nullptr,
                            static_cast<int>(mdl::Mdl(model)->boneCount), physicsMode);
    if (type == 7) {
        SeekModelFrame(model, *frame, physicsMode);
        ApplyPose(model, redo.bonePose, static_cast<int>(mdl::Mdl(model)->boneCount));
    }
    if (UndoAt(model, cursor).operation == 4)
        RedoModelEdit(model, frame, physicsMode);
}

void ResetDisplayKeyCursor(unsigned char* model) {  // VA 0x004A4A00, was Sub4A4A00
    if (model == nullptr)
        return;
    mdl::DisplayKey* const displayKeys = mdl::DisplayKeys(model);
    std::int32_t& freeIndex =
        mdl::Mdl(model)->searchCursor;
    freeIndex = 1;
    if (displayKeys == nullptr)
        return;
    while (freeIndex < mdl::KeyPoolCapacity(displayKeys) && displayKeys[freeIndex].frame != 0) {
        ++freeIndex;
    }
}

void SyncModelEditControls(unsigned char* model) {  // was Sub4A02C0
    if (model == nullptr)
        return;
    const HWND hwnd = *reinterpret_cast<HWND*>(model);
    SendMessageA(GetDlgItem(hwnd, panel::kModelVisibleCheckbox), BM_SETCHECK,
                 mikumikustudio::mdl::Mdl(model)->loadComplete != 0 ? BST_CHECKED : BST_UNCHECKED, 0);

    mdl::IkChain* chains = mdl::IkChains(model);
    if (chains != nullptr) {
        const LRESULT sel =
            SendMessageA(GetDlgItem(hwnd, panel::kIkChainCombo), CB_GETCURSEL, 0, 0);
        const bool enabled = chains[sel].enabled != 0;
        CheckRadioButton(hwnd, 0x1BC, 0x1BD, enabled ? 0x1BC : 0x1BD);
    }

    mdl::MorphRecord* morphs = mdl::Morphs(model);
    if (morphs == nullptr)
        return;
    static constexpr int kSliderIds[4] = {0x1F9, 0x1FE, 0x203, 0x208};
    static constexpr int kEditIds[4] = {0x1FA, 0x1FF, 0x204, 0x209};
    char text[0x100];
    for (int lane = 0; lane < 4; ++lane) {
        const std::int32_t index = mdl::Mdl(model)->selectedMorphs[lane];
        if (index < 0)
            continue;
        const float value =
            morphs[static_cast<std::size_t>(index)].value;
        SendMessageA(GetDlgItem(hwnd, kSliderIds[lane]), TBM_SETPOS, TRUE,
                     static_cast<LPARAM>(static_cast<std::int32_t>(value * 100.0f)));
        sprintf_s(text, sizeof(text), "%5.4f", static_cast<double>(value));
        SetWindowTextA(GetDlgItem(hwnd, kEditIds[lane]), text);
    }
}

// Exported entry for the frame-line delete command (0x43A650) - the body
// above is the verbatim port of VA 0x0049D4D0 but lives in the anonymous
// namespace.  VA 0x0049D4D0.
void RebuildBoneKeyInterpolation(unsigned char* model, int index, int lane) {  // was Sub49D4D0, VA 0x0049D4D0
    RebuildBoneInterpolation(model, index, lane);
}

}  // namespace mikumikustudio
