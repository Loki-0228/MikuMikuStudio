#pragma once
#include <Windows.h>
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/text_encoding.hpp"

namespace mikumikustudio {
inline void FillBoneCombo(HWND combo, const mdl::ModelRecord* model, bool english) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    if (!model || !model->boneTable) return;
    for (std::uint32_t i = 0; i < model->boneCount; ++i) {
        const auto& bone = model->boneTable[i];
        if (bone.type >= mdl::BoneType::InertTip && bone.type != mdl::BoneType::FixedAxis) continue;
        const auto name = text_encoding::BoneLabel(bone, english);
        const auto row = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        if (row >= 0) SendMessageW(combo, CB_SETITEMDATA, row, i);
    }
}
inline void SelectBoneCombo(HWND combo, int bone) {
    const auto count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (LRESULT row = 0; row < count; ++row)
        if (SendMessageW(combo, CB_GETITEMDATA, row, 0) == bone) {
            SendMessageW(combo, CB_SETCURSEL, row, 0);
            return;
        }
    SendMessageW(combo, CB_SETCURSEL, -1, 0);
}
inline int SelectedComboBone(HWND combo, const mdl::ModelRecord* model) {
    const auto row = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (!model || row < 0) return -1;
    const auto bone = SendMessageW(combo, CB_GETITEMDATA, row, 0);
    return bone >= 0 && static_cast<std::uint64_t>(bone) < model->boneCount
        ? static_cast<int>(bone) : -1;
}
}
