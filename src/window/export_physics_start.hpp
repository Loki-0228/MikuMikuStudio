#pragma once
#include "mikumikustudio/image_sequence_export.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/recording_state.hpp"
#include <Windows.h>

namespace mikumikustudio::export_physics_start {
inline void Initialize(HWND dialog, MMDApp* app, bool enabled = false, int frame = 0) {
    SetDlgItemTextW(dialog, panel::kExportPhysicsStartCheckbox, image_sequence::Text(app,
        L"从指定帧开始计算物理", L"Calculate physics from frame", L"指定フレームから物理計算"));
    CheckDlgButton(dialog, panel::kExportPhysicsStartCheckbox, enabled ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(dialog, panel::kExportPhysicsStartEdit, recording_state::ResolvePhysicsStart(app, frame), TRUE);
    SendDlgItemMessageW(dialog, panel::kExportPhysicsStartEdit, EM_SETLIMITTEXT, 11, 0);
    EnableWindow(GetDlgItem(dialog, panel::kExportPhysicsStartEdit), enabled);
}
inline bool Handle(HWND dialog, UINT message, WPARAM wp) {
    if (message != WM_COMMAND || LOWORD(wp) != panel::kExportPhysicsStartCheckbox || HIWORD(wp) != BN_CLICKED) return false;
    EnableWindow(GetDlgItem(dialog, panel::kExportPhysicsStartEdit),
        IsDlgButtonChecked(dialog, panel::kExportPhysicsStartCheckbox) == BST_CHECKED);
    return true;
}
inline bool Read(HWND dialog, MMDApp* app, int outputStart, bool& enabled, int& frame) {
    enabled = IsDlgButtonChecked(dialog, panel::kExportPhysicsStartCheckbox) == BST_CHECKED;
    frame = 0;
    if (!enabled) return true;
    char text[32]{};
    GetDlgItemTextA(dialog, panel::kExportPhysicsStartEdit, text, sizeof text);
    const bool valid = frame_limits::ParseSigned(text, frame);
    if (valid && recording_state::ValidPhysicsStart(true, frame, outputStart)) {
        frame = recording_state::ResolvePhysicsStart(app, frame);
        SetDlgItemInt(dialog, panel::kExportPhysicsStartEdit, frame, TRUE);
        return true;
    }
    MessageBoxW(dialog, image_sequence::Text(app,
        L"物理计算起始帧必须是有效整数，且不得晚于输出起始帧；可以输入负数。",
        L"Physics start must be a valid whole frame no later than the first output frame; negative frames are allowed.",
        L"物理計算の開始には出力開始フレーム以前の有効な整数を指定してください。負のフレームも指定できます。"),
        L"MMS", MB_OK | MB_ICONWARNING);
    SetFocus(GetDlgItem(dialog, panel::kExportPhysicsStartEdit));
    SendDlgItemMessageW(dialog, panel::kExportPhysicsStartEdit, EM_SETSEL, 0, -1);
    return false;
}
}
