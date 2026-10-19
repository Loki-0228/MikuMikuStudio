// Bone-edit undo snapshot (original VA 0x0042D6E0; was Sub42D6E0).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/bone_scale.hpp"
#include <Windows.h>

#include <cstdlib>
#include <cstring>
#include <new>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"

namespace mikumikustudio {

void PushBoneEditUndo(MMDApp* app) {  // was Sub42D6E0, VA 0x0042D6E0
    if (app == nullptr)
        return;
    const unsigned modelSlot = app->state.slotIdx;
    auto* model = static_cast<unsigned char*>(
        app->state.modelSlots[modelSlot]);
    if (model == nullptr)
        return;

    const int boneCount = mikumikustudio::mdl::Mdl(model)->boneCount;
    auto* selected = mikumikustudio::mdl::Mdl(model)->boneSelection;
    auto* dirty = mikumikustudio::mdl::Mdl(model)->bonePhysicsState;
    auto* bones = mikumikustudio::mdl::Bones(model);
    if (boneCount <= 0 || selected == nullptr || bones == nullptr)
        return;

    int selectedCount = 0;
    for (int i = 0; i < boneCount; ++i)
        selectedCount += selected[i] != 0;
    if (selectedCount == 0)
        return;

    HWND window = app->state.hwnd;
    if (window == nullptr)
        window = static_cast<HWND>(app->Hwnd());
    EnableWindow(GetDlgItem(window, panel::kUndoButton), TRUE);
    EnableWindow(GetDlgItem(window, panel::kRedoButton), FALSE);

    // x64 0x7FF7CB447074/0x7FF7CB44708A：undo/redo 脏标志 = model+0x3558/
    // +0x3559，紧跟 undoState 环游标（+0x3550，环深 30）。x86 旧偏移
    // 12732/12733 在 x64 落进 gap15 死垫片会让 Ctrl+Z/Y 门失效，必须走
    // typed 字段（双架构偏移由 model_layout.hpp 钉死）。
    mikumikustudio::mdl::Mdl(model)->undoDirty = 1;
    mikumikustudio::mdl::Mdl(model)->redoDirty = 0;
    auto& ringIndex = mikumikustudio::mdl::Mdl(model)->undoState[0];
    if (++ringIndex >= 30)
        ringIndex = 0;
    mikumikustudio::mdl::Mdl(model)->undoState[1] = ringIndex;
    bone_scale::CaptureUndo(model);

    auto& undo = mikumikustudio::mdl::Mdl(model)->undoRings[0].slots[ringIndex];
    undo.operation = 1;
    undo.dirty = selectedCount;
    auto*& oldSnapshot = undo.bonePose;
    if (oldSnapshot != nullptr) {
        std::free(oldSnapshot);
        oldSnapshot = nullptr;
    }

    auto* snapshot = static_cast<mikumikustudio::mdl::BonePoseSnapshot*>(
        ::operator new(sizeof(mikumikustudio::mdl::BonePoseSnapshot) * selectedCount));
    oldSnapshot = snapshot;
    std::memset(snapshot, 0, static_cast<std::size_t>(36) * selectedCount);

    int output = 0;
    for (int i = 0; i < boneCount; ++i) {
        if (selected[i] == 0)
            continue;
        auto& record = snapshot[output++];
        mikumikustudio::mdl::BoneRecord* bone = &bones[i];
        record.boneIndex = i;
        std::memcpy(record.position, bone->trans, sizeof(record.position));
        std::memcpy(record.rotation, bone->rotQuat, sizeof(record.rotation));
        record.physicsDisabled = dirty != nullptr ? dirty[i] : 0;
    }
}

}  // namespace mikumikustudio
