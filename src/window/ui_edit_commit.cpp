#include "mikumikustudio/frame_limits.hpp"
// ===========================================================================
// VA 0x00463640 - EditCommit  (original: sub_463640, 0xB2A bytes)
// VA 0x0044BEF0 - EditCommitTail  (original: sub_44BEF0)
// VA 0x00464170 - EditSubclassProc  (original: sub_464170)
// VA 0x0040EC90 - TrackbarSubclassProc  (original: sub_40EC90)
// ===========================================================================
// sub_464170 is the wndproc installed on the 29 numeric EDIT controls of
// sub_466D20 (0x467BE4..0x46A8E7): on VK_RETURN it commits the edit
// (sub_463640) and returns focus to the main window; on WM_KILLFOCUS it
// commits the edit that lost focus. Everything else
// forwards to the captured EDIT class proc at app+0xA08D0 (stored once, at
// the first install site 0x467BD7 - all 29 controls share the class proc).
//
// sub_40EC90 is the wndproc installed on the 13 msctls_trackbar32 controls:
// WM_LBUTTONUP merely refocuses the main window (keyboard shortcuts keep
// working after a slider drag), everything forwards to the trackbar class
// proc captured once at 0x4686E2 into app+0xA0CDC.
//
// sub_463640 dispatches per control id (argument = the edit's HWND):
//   417  frame number  - atol -> app+0x980; >0x80000000 resets to 0/"0";
//        Sub432FA0 + PostViewRefresh tail
//   461-466  center/rotation mirrors - parsed value scaled into the work
//        record (647588..96 / 647540..48) and echoed into the paired
//        trackbar (455..460) via TBM_SETPOS; RefreshRequest(-2) tail
//   448  fov degrees -> app+647656, slider 447 echo, projection install
//        through the camera funnel ApplyFrameCameraProjection
//        (PerspectiveFovLH fov*0.01745329238474369, aspect from the
//        wrapper at +0x1D4EC, zn 1.0, zf 100000.0, SetTransform slot 3;
//        the orthographic override at 0x140027E96..0x140027F1F replaces
//        it when the frame is orthographic); RefreshRequest(-1) tail
//   478-485  light-accessory fields (gated on accessory != null; index
//        byte app+647536 into the 0x9DD70 array): rgb 532/536/540 direct,
//        direction 544/548/552 as deg*PI/180 (PI = 3.141592025756836 -
//        the float-truncated literal), 556 direct, 485 clamps to [0,1]
//        into +1184 and rewrites the edit text "%3.2f"; tail
//        RefreshRequest(acc index)
//   561  -> app+658732 = (10000-v)/100000, slider 560 echo, -3 tail
//   506/511/516/521  morph values into the 136-stride morph records of the
//        active model (slot app+0x910 into app+0x780; index at model+
//        11676/80/84/88, records at model+9924, value at +48), sliders
//        505/510/515/520 echo x100
// Every path ends in sub_44BEF0, which additionally owns the camera/bone
// position-rotation edits 544..550 (bone mode writes the 604-stride bone
// record +320..328/+332 via the Z*X*Y euler composition and the dirty mark
// at model+11672; camera mode writes app+820..828/784..792/657628) and
// finishes with RefreshRequest(-1)+PostViewRefresh when it handled one.
//
// Reference: ../translated/MikuMikuDance/fcn_00463640.cpp (rough), live
// IDA decompilation + disassembly of 0x463640/0x44BEF0/0x464170/0x40EC90
// (the asm is authoritative for the subclass-proc slots: Hex-Rays prints
// decoy indices for both Block+... reads).
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>

#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/bone_parent_coordinates.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "transform_edit_layout.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/plugin_scene.hpp"
#include "../plugins/native_pose_scope.hpp"
#include "../plugins/native_history_commands.hpp"
#include "../plugins/native_global_commands.hpp"
#include "../plugins/native_morph_commands.hpp"
#include "bottom_panel_control_paint.hpp"
#endif

namespace mikumikustudio {

// Dependencies ported in other translation units.
void RefreshAfterFrameApply(MMDApp* app);          // VA 0x00432FA0, was Sub432FA0 (ui_frame_step.cpp)
void PushBoneEditUndo(MMDApp* app);                       // VA 0x0042D6E0 (bone_edit_undo.cpp)
void RefreshRequest(int area);                     // VA 0x00440AC0 (ui_refresh)

// Deg->rad literal for 0x463640/0x44BEF0 (dbl_52BB20, bits
// 0x3F91DF46A0000000) is no longer spelled here: the 448 FOV commit applies
// its projection via ApplyFrameCameraProjection, which owns that same
// literal (ui_viewport_refresh.cpp:59-64).
//
// PI literal used by the euler conversions (float-truncated 24-bit mantissa).
static constexpr double kPiLit = 3.141592025756836;

namespace {

// Active model = slot array app+0x780 indexed by byte app+0x910.
unsigned char* SlotModel(MMDApp* app) {
    return app->SelectedModel();
}

bool IsBoneReadout(int id) noexcept {
    return (id >= panel::kReadoutPosXEdit && id <= panel::kReadoutRotZEdit) ||
        id == panel::kReadoutScaleXEdit || id == panel::kReadoutScaleYEdit ||
        id == panel::kReadoutScaleZEdit;
}

struct BoneReadoutDraft {
    MMDApp* app;
    HWND field, owner;
    std::uint64_t scene;
    int frame;
    unsigned slot;
    unsigned char* model;
    const mdl::BoneRecord* table = nullptr;
    int bone = -1;
#if !MMD_ORIGINAL_PROFILE
    MmsObjectRef identity{};
#endif

    bool Current(MMDApp& live) const noexcept {
        if (app != &live || live.CameraMode() || live.sceneRecoveryPending ||
            live.SceneMutationInProgress() || live.SceneGeneration() != scene ||
            live.CurrentFrame() != frame || live.SelectedModelSlot() != slot ||
            live.SelectedModel() != model || !model || !table || bone < 0 ||
            owner != (live.FloatingWindow() ? live.FloatingWindow() : live.MainWindow()))
            return false;
#if !MMD_ORIGINAL_PROFILE
        if (identity.id.lo || identity.id.hi) {
            MmsObjectRef current{};
            if (!plugins::FindSceneObjectIdentity(model, current) || current.id.lo != identity.id.lo ||
                current.id.hi != identity.id.hi || current.generation != identity.generation)
                return false;
        }
#endif
        const auto& native = *mdl::Mdl(model);
        return native.boneTable == table && native.selectedBone == bone &&
            static_cast<unsigned>(bone) < native.boneCount;
    }
};

// Only one field can receive keyboard focus on this UI thread.
thread_local std::optional<BoneReadoutDraft> boneReadoutDraft;

void BeginBoneReadoutDraft(MMDApp& app, HWND field, int id) {
    const HWND owner = app.FloatingWindow() ? app.FloatingWindow() : app.MainWindow();
    if (!IsBoneReadout(id) || app.CameraMode() || GetDlgItem(owner, id) != field) return;
    auto* model = app.SelectedModel();
    BoneReadoutDraft draft{&app, field, owner, app.SceneGeneration(), app.CurrentFrame(),
        app.SelectedModelSlot(), model};
    if (model) {
        const auto& native = *mdl::Mdl(model);
        draft.table = native.boneTable;
        draft.bone = native.selectedBone;
#if !MMD_ORIGINAL_PROFILE
        plugins::FindSceneObjectIdentity(model, draft.identity);
#endif
    }
    boneReadoutDraft = draft;
}

bool DiscardStaleBoneReadout(MMDApp& app, HWND field) {
    if (!boneReadoutDraft || boneReadoutDraft->field != field) return false;
    const auto draft = *boneReadoutDraft;
    boneReadoutDraft.reset();
    if (draft.Current(app)) return false;
    app.state.enterKeyState = 3;
    if (!SendMessageA(field, EM_GETMODIFY, 0, 0)) return true;
    SendMessageA(field, EM_SETMODIFY, FALSE, 0);
    if (g_Block == &app && IsWindow(field) && !app.sceneRecoveryPending &&
        !app.SceneMutationInProgress() && !app.FrameStepPlayback() &&
        !app.state.recordPlaybackStartPending &&
        GetParent(field) == (app.FloatingWindow() ? app.FloatingWindow() : app.MainWindow()))
        PostViewRefresh(&app);
    return true;
}

// Light accessory = array app+0x9DD70 indexed by byte app+0x9E170.
mdl::AccessoryRecord* LightAccessory(MMDApp* app) {
    return app->AccessorySlot(app->SelectedAccessorySlot());
}

// Z*X*Y euler composition of the 547/548/549 bone-rotation commits:
// D3DXMatrixRotationZ x rotX x rotY, result quaternion into the bone record
// (+332 inside the 604-stride bone array).
void ComposeEulerToBone(MMDApp* app, unsigned char* model, int sel) {
    d3dx::D3DXMATRIXF rot{};
    d3dx::D3DXMATRIXF tmp{};
    auto* d3dx = &d3dx::Get();
    if (!d3dx->Load())
        return;
    d3dx->rotZ(&rot, app->state.eulerZ);
    d3dx->rotX(&tmp, app->state.eulerX);
    d3dx->multiply(&rot, &rot, &tmp);
    d3dx->rotY(&tmp, app->state.eulerY);
    d3dx->multiply(&rot, &rot, &tmp);
    mikumikustudio::mdl::BoneRecord* bones = mikumikustudio::mdl::Bones(model);
    d3dx->quatFromMatrix(reinterpret_cast<float*>(&bones[sel].rotQuat[0]),
                         &rot);
    mikumikustudio::mdl::Mdl(model)->bonePhysicsState[sel] = 1;
}

#if !MMD_ORIGINAL_PROFILE
// This route is enabled only by an explicit internal app/history binding.
// Preparation and publication never call PushBoneEditUndo or window functions.
MmsResult ApplyBoundPoseNumber(MMDApp& app,plugins::SharedHistory& history,int kind,int axis,float value,bool& changed) {
    changed=false;
    return plugins::Boundary([&]() -> MmsResult {
        if(app.SelectedModelSlot()>=kModelSlotCount||app.PlaybackActive()||app.FrameStepPlayback()||
            app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
        auto* model=app.SelectedModel();
        if(!model)return MMS_E_NOT_READY;
        auto& native=*mdl::Mdl(model);const int selected=native.selectedBone;
        if(!native.boneTable||!native.bonePhysicsState||selected<0||std::uint32_t(selected)>=native.boneCount)return MMS_E_NOT_READY;
        auto& bone=native.boneTable[selected];float position=0,rotation[4]{};
        if(kind==0) {
            position=value-BoneParentBindOffset(model,selected,axis);
            if(!std::isfinite(position))return MMS_E_ARGUMENT;
            if(position==bone.trans[axis])return MMS_OK;
        } else if(kind==1) {
            auto& api=d3dx::Get();
            if(!api.Load()||!api.rotX||!api.rotY||!api.rotZ||!api.multiply||!api.quatFromMatrix)return MMS_E_NOT_READY;
            const float x=static_cast<float>((axis==0?value:app.state.eulerX)*kPiLit/180.0);
            const float y=static_cast<float>(-(axis==1?value:app.state.eulerY)*kPiLit/180.0);
            const float z=static_cast<float>(-(axis==2?value:app.state.eulerZ)*kPiLit/180.0);
            if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return MMS_E_ARGUMENT;
            d3dx::D3DXMATRIXF target{},temporary{};
            api.rotZ(&target,z);api.rotX(&temporary,x);api.multiply(&target,&target,&temporary);
            api.rotY(&temporary,y);api.multiply(&target,&target,&temporary);api.quatFromMatrix(rotation,&target);
            for(float component:rotation)if(!std::isfinite(component))return MMS_E_ARGUMENT;
            if(!std::memcmp(rotation,bone.rotQuat,sizeof(rotation)))return MMS_OK;
        } else {
            if(value<=0||!bone_scale::Enabled(&app))return MMS_E_ARGUMENT;
            float current[3]{};
            if(!SelectedBoneScaleReadout(&app,current))return MMS_E_NOT_READY;
            if(current[axis]==value)return MMS_OK;
            // View-coordinate scale uses this API. Resolve any DLL loading
            // before the capture; subsequent Load calls only reuse it.
            d3dx::Get().Load();
        }
        plugins::NativePoseScope scope;
        if(auto result=scope.Begin(app,history,app.SelectedModelSlot(),kind==0?"Bone position":kind==1?"Bone rotation":"Bone scale"))return result;
        if(kind==0)bone.trans[axis]=position;
        else if(kind==1)std::memcpy(bone.rotQuat,rotation,sizeof(rotation));
        else if(!SetSelectedBoneScaleAxis(&app,axis,value))return MMS_E_ARGUMENT;
        native.bonePhysicsState[selected]=1;
        return scope.Commit(changed);
    });
}
bool FindPoseNumber(HWND edit,HWND base,int& kind,int& axis) {
    if(!edit)return false;
    const int ids[3][3]={{panel::kReadoutPosXEdit,panel::kReadoutPosYEdit,panel::kReadoutPosZEdit},
        {panel::kReadoutRotXEdit,panel::kReadoutRotYEdit,panel::kReadoutRotZEdit},
        {panel::kReadoutScaleXEdit,panel::kReadoutScaleYEdit,panel::kReadoutScaleZEdit}};
    kind=-1;axis=-1;
    for(int group=0;group<3;++group)for(int component=0;component<3;++component)
        if(edit==GetDlgItem(base,ids[group][component])){kind=group;axis=component;}
    return kind>=0;
}
int FindCameraNumber(HWND edit, HWND base) {
    if (!edit || !base) return -1;
    for (int field = 0; field < 7; ++field)
        if (edit == GetDlgItem(base, panel::kReadoutPosXEdit + field)) return field;
    return -1;
}
bool CommitBoundCameraNumber(MMDApp* app, HWND edit, HWND base) {
    const int field = FindCameraNumber(edit, base);
    if (!app->CameraMode() || field < 0 || !plugins::BoundNativePoseHistory(app)) return false;
    if (!SendMessageA(edit, EM_GETMODIFY, 0, 0)) return true;
    SendMessageA(edit, EM_SETMODIFY, FALSE, 0);
    char text[256]{}; GetWindowTextA(edit, text, sizeof(text)); char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    const bool consumed = end && end != text;
    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    bool changed = false;
    if (consumed && !*end && GetWindowTextLengthA(edit) < sizeof(text) && std::isfinite(parsed)) {
        // Match the old double intermediates and truncated pi; the modal
        // camera dialog uses a different float conversion and is not reused.
        const auto value = static_cast<float>(field < 3 ? parsed : field == 6 ? -parsed :
            (field == 3 ? -parsed : parsed) * kPiLit / 180.0);
        if (std::isfinite(value) && plugins::ApplyBoundCameraField(*app, unsigned(field), value, changed) == MMS_OK && changed)
            return true;
    }
    plugins::RefreshBoundPoseReadout(*app);
    return true;
}
int FindLightNumber(MMDApp* app, HWND edit) {
    if (!edit || !app->MainWindow()) return -1;
    for (int field = 0; field < 6; ++field) if (edit == GetDlgItem(app->MainWindow(), 461 + field)) return field;
    return -1;
}
bool CommitBoundLightNumber(MMDApp* app, HWND edit) {
    const int field = FindLightNumber(app, edit);
    if (field < 0 || !plugins::BoundNativePoseHistory(app)) return false;
    if (!SendMessageA(edit, EM_GETMODIFY, 0, 0)) return true;
    SendMessageA(edit, EM_SETMODIFY, FALSE, 0);
    char full[256]{}; GetWindowTextA(edit, full, sizeof(full)); char* end = nullptr;
    const double parsed = std::strtod(full, &end); const bool consumed = end && end != full;
    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (consumed && !*end && GetWindowTextLengthA(edit) < sizeof(full) && std::isfinite(parsed)) {
        // Preserve the original seven-character input window and integer RGB
        // conversion, after validating the full user input as numeric.
        char legacy[8]{}; GetWindowTextA(edit, legacy, sizeof(legacy));
        const auto value = static_cast<float>(field < 3 ? double(std::strtol(legacy, nullptr, 10)) * .00390625 : std::strtod(legacy, nullptr));
        bool changed = false;
        if (std::isfinite(value) && plugins::ApplyBoundLightField(*app, unsigned(field), value, changed) == MMS_OK && changed) return true;
    }
    plugins::RefreshBoundGlobalReadout(*app); return true;
}
int FindAccessoryNumber(MMDApp* app, HWND edit) {
    if (!edit || !app->MainWindow()) return -1;
    for (int field = 0; field < 8; ++field) if (edit == GetDlgItem(app->MainWindow(), 478 + field)) return field;
    return -1;
}
bool CommitBoundAccessoryNumber(MMDApp* app, HWND edit) {
    const int field = FindAccessoryNumber(app, edit);
    if (field < 0 || !plugins::BoundNativePoseHistory(app)) return false;
    if (!SendMessageA(edit, EM_GETMODIFY, 0, 0)) return true;
    SendMessageA(edit, EM_SETMODIFY, FALSE, 0);
    char full[256]{}; GetWindowTextA(edit, full, sizeof(full)); char* end = nullptr;
    const double parsed = std::strtod(full, &end); const bool consumed = end && end != full;
    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (consumed && !*end && GetWindowTextLengthA(edit) < sizeof(full) && std::isfinite(parsed)) {
        char legacy[8]{}; GetWindowTextA(edit, legacy, sizeof(legacy));
        const double input = std::strtod(legacy, nullptr);
        const auto value = static_cast<float>(field < 3 || field == 6 ? input : field < 6 ? input / 180.0 * kPiLit :
            input < 0 ? 0 : input > 1 ? 1 : input);
        bool changed = false;
        if (std::isfinite(value) && plugins::ApplyBoundAccessoryField(*app, app->SelectedAccessorySlot(), unsigned(field), value, changed) == MMS_OK && changed) return true;
    }
    plugins::RefreshBoundGlobalReadout(*app); return true;
}
int FindMorphNumber(MMDApp* app,HWND edit) {
    if(!edit || !app->MainWindow())return -1;
    for(int lane=0;lane<4;++lane)if(edit==GetDlgItem(app->MainWindow(),506+lane*5))return lane;
    return -1;
}
bool CommitBoundMorphNumber(MMDApp* app,HWND edit) {
    const int lane=FindMorphNumber(app,edit);
    if(lane<0 || !plugins::BoundNativePoseHistory(app))return false;
    if(!SendMessageA(edit,EM_GETMODIFY,0,0))return true;
    SendMessageA(edit,EM_SETMODIFY,FALSE,0);
    char full[256]{};GetWindowTextA(edit,full,sizeof(full));char* end=nullptr;
    const double parsed=std::strtod(full,&end);const bool consumed=end && end!=full;
    while(end && std::isspace(static_cast<unsigned char>(*end)))++end;
    if(consumed && !*end && GetWindowTextLengthA(edit)<sizeof(full) && std::isfinite(parsed) && app->SelectedModelSlot()<kModelSlotCount) {
        char legacy[8]{};GetWindowTextA(edit,legacy,sizeof(legacy));const double raw=std::strtod(legacy,nullptr);const float value=static_cast<float>(raw);
        const auto* model=mdl::Mdl(app->SelectedModel());
        if(model && model->selectedMorphs[lane]>=0 && std::isfinite(value)) {
            bool changed=false;
            if(plugins::ApplyBoundMorphValue(*app,app->SelectedModelSlot(),unsigned(model->selectedMorphs[lane]),value,changed)==MMS_OK) {
                if(!changed)plugins::RefreshBoundMorphReadout(*app);
                // Successful authoring already refreshed the paired slider.
                // Use that same display conversion for changed and equal
                // values: .29's parsed double otherwise writes 28 after the
                // canonical authored-float readout correctly displayed 29.
                return true;
            }
        }
    }
    plugins::RefreshBoundMorphReadout(*app);return true;
}
bool IsShadowNumber(MMDApp* app, HWND edit) { return edit && app->MainWindow() && edit == GetDlgItem(app->MainWindow(), panel::kSelfShadowRangeEdit); }
bool CommitBoundShadowNumber(MMDApp* app, HWND edit) {
    if (!IsShadowNumber(app, edit) || !plugins::BoundNativePoseHistory(app)) return false;
    if (!SendMessageA(edit, EM_GETMODIFY, 0, 0)) return true;
    SendMessageA(edit, EM_SETMODIFY, FALSE, 0);
    char full[256]{}; GetWindowTextA(edit, full, sizeof(full)); char* end = nullptr;
    const double parsed = std::strtod(full, &end); const bool consumed = end && end != full;
    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (consumed && !*end && GetWindowTextLengthA(edit) < sizeof(full) && std::isfinite(parsed)) {
        char legacy[8]{}; GetWindowTextA(edit, legacy, sizeof(legacy));
        // Enhanced SetShadowSliderPosition truncates to integer then clamps.
        // Bound before conversion to avoid undefined casts for large input.
        const auto position = static_cast<int>((std::clamp)(std::strtod(legacy, nullptr), 0.0, 9999.0));
        const float distance = static_cast<float>((10000.0 - position) / 100000.0);
        bool changed = false;
        if (plugins::ApplyBoundShadowDistance(*app, distance, changed) == MMS_OK && changed) return true;
    }
    plugins::RefreshBoundGlobalReadout(*app); return true;
}
bool IsFovNumber(MMDApp* app, HWND edit) { return edit && app->MainWindow() && edit == GetDlgItem(app->MainWindow(), panel::kFovEdit); }
bool CommitBoundFovNumber(MMDApp* app, HWND edit) {
    if (!IsFovNumber(app,edit) || !plugins::BoundNativePoseHistory(app)) return false;
    if (!SendMessageA(edit, EM_GETMODIFY, 0, 0)) return true;
    SendMessageA(edit, EM_SETMODIFY, FALSE, 0);
    char full[256]{}; GetWindowTextA(edit, full, sizeof(full)); char* end = nullptr;
    const double parsed = std::strtod(full, &end); const bool consumed = end && end != full;
    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (consumed && !*end && GetWindowTextLengthA(edit) < sizeof(full) && std::isfinite(parsed)) {
        char legacy[8]{}; GetWindowTextA(edit, legacy, sizeof(legacy));
        bool changed = false;
        if (plugins::ApplyBoundCameraFov(*app, static_cast<float>(std::strtol(legacy, nullptr, 10)), changed) == MMS_OK && changed) return true;
    }
    plugins::RefreshBoundGlobalReadout(*app); return true;
}
bool CommitBoundPoseNumber(MMDApp* app,HWND edit,HWND base) {
    auto* history=plugins::BoundNativePoseHistory(app);
    int kind=-1,axis=-1;
    if(!history||app->CameraMode()||!FindPoseNumber(edit,base,kind,axis))return false;
    // A readout is rounded for display. Focus changes must not write that
    // rounded text back over the full-precision authored value.
    if(!SendMessageA(edit,EM_GETMODIFY,0,0))return true;
    SendMessageA(edit,EM_SETMODIFY,FALSE,0);
    char text[256]{};GetWindowTextA(edit,text,sizeof(text));char* end=nullptr;
    const double parsed=std::strtod(text,&end);
    const bool consumed=end&&end!=text;
    while(end&&std::isspace(static_cast<unsigned char>(*end)))++end;
    bool changed=false;
    MmsResult result=MMS_E_ARGUMENT;
    if(consumed&&!*end&&GetWindowTextLengthA(edit)<sizeof(text)&&std::isfinite(parsed)&&std::isfinite(static_cast<float>(parsed)))
        result=ApplyBoundPoseNumber(*app,*history,kind,axis,static_cast<float>(parsed),changed);
    // The scope is now finished and its journal lock has been released. Only
    // successful edits publish dirty/UI state; errors leave the old ring alone.
    if(result==MMS_OK&&changed) {
        plugins::PublishBoundPoseEdit(*app,*history);
        return true;
    }
    plugins::RefreshBoundPoseReadout(*app);
    return true;
}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// VA 0x0044BEF0 - per-edit tail; owns ids 544..550 (see file header).
// ---------------------------------------------------------------------------
void CommitEditControlTail(MMDApp* app, HWND edit) {  // was Sub44BEF0
    HWND base = app->FloatingWindow();
    if (base == nullptr)
        base = app->MainWindow();
    const bool cameraMode = app->CameraMode() != 0;
    if (!cameraMode && edit && IsBoneReadout(GetDlgCtrlID(edit)) &&
        GetParent(edit) == base) {
        const auto* model = app->SelectedModel();
        const auto* native = model ? mdl::Mdl(model) : nullptr;
        if (!native || !native->boneTable || !native->bonePhysicsState ||
            native->selectedBone < 0 || static_cast<unsigned>(native->selectedBone) >= native->boneCount)
            return;
    }
#if !MMD_ORIGINAL_PROFILE
    if(CommitBoundCameraNumber(app,edit,base))return;
    if(CommitBoundPoseNumber(app,edit,base))return;
#endif

    if (edit == GetDlgItem(base, panel::kReadoutPosXEdit)) {           // 0x44BF31
        char text[256];
        GetWindowTextA(edit, text, 100);
        const double v = atof(text);
        if (!cameraMode) {
            unsigned char* model = SlotModel(app);
            const int sel = mikumikustudio::mdl::Mdl(model)->selectedBone;
            if (sel >= 0) {
                PushBoneEditUndo(app);
                mikumikustudio::mdl::Bones(model)[sel].trans[0] = static_cast<float>(v) - BoneParentBindOffset(model,sel,0);
                mikumikustudio::mdl::Mdl(model)->bonePhysicsState[sel] = 1;
            }
            PostViewRefresh(app);
            return;
        }
        app->CameraPositionX() = static_cast<float>(v);
        RefreshRequest(-1);
        PostViewRefresh(app);
        return;
    }
    if (edit == GetDlgItem(base, panel::kReadoutPosYEdit)) {           // 0x44BFE5
        char text[256];
        GetWindowTextA(edit, text, 100);
        const double v = atof(text);
        if (!cameraMode) {
            unsigned char* model = SlotModel(app);
            const int sel = mikumikustudio::mdl::Mdl(model)->selectedBone;
            if (sel >= 0) {
                PushBoneEditUndo(app);
                mikumikustudio::mdl::Bones(model)[sel].trans[1] = static_cast<float>(v) - BoneParentBindOffset(model,sel,1);
                mikumikustudio::mdl::Mdl(model)->bonePhysicsState[sel] = 1;
            }
            PostViewRefresh(app);
            return;
        }
        app->CameraPositionY() = static_cast<float>(v);
        RefreshRequest(-1);
        PostViewRefresh(app);
        return;
    }
    if (edit == GetDlgItem(base, panel::kReadoutPosZEdit)) {           // 0x44C099
        char text[256];
        GetWindowTextA(edit, text, 100);
        double v = atof(text);
        if (!cameraMode) {
            unsigned char* model = SlotModel(app);
            const int sel = mikumikustudio::mdl::Mdl(model)->selectedBone;
            if (sel >= 0) {
                PushBoneEditUndo(app);
                mikumikustudio::mdl::Bones(model)[sel].trans[2] = static_cast<float>(v) - BoneParentBindOffset(model,sel,2);
                mikumikustudio::mdl::Mdl(model)->bonePhysicsState[sel] = 1;
            }
            PostViewRefresh(app);
            return;
        }
        app->CameraPositionZ() = static_cast<float>(v);
        RefreshRequest(-1);
        PostViewRefresh(app);
        return;
    }
    if (edit == GetDlgItem(base, panel::kReadoutRotXEdit)) {           // 0x44C158
        char text[256];
        GetWindowTextA(edit, text, 100);
        const double v = atof(text);
        if (!cameraMode) {
            unsigned char* model = SlotModel(app);
            const int sel = mikumikustudio::mdl::Mdl(model)->selectedBone;
            if (sel >= 0) {
                PushBoneEditUndo(app);
                app->state.eulerX =
                    static_cast<float>(v * kPiLit / 180.0);
                app->state.eulerY =
                    static_cast<float>(-static_cast<double>(
                                           app->state.eulerY) *
                                       kPiLit / 180.0);
                app->state.eulerZ =
                    static_cast<float>(kPiLit *
                                       -static_cast<double>(
                                           app->state.eulerZ) /
                                       180.0);
                ComposeEulerToBone(app, model, sel);
            }
            PostViewRefresh(app);
            return;
        }
        app->CameraPitch() = static_cast<float>(-v * kPiLit / 180.0);
        RefreshRequest(-1);
        PostViewRefresh(app);
        return;
    }
    if (edit == GetDlgItem(base, panel::kReadoutRotYEdit)) {           // 0x44C2C7
        char text[256];
        GetWindowTextA(edit, text, 100);
        const double v = atof(text);
        if (cameraMode) {
            app->CameraYaw() = static_cast<float>(v * kPiLit / 180.0);
            RefreshRequest(-1);
            PostViewRefresh(app);
            return;
        }
        unsigned char* model = SlotModel(app);
        const int sel = mikumikustudio::mdl::Mdl(model)->selectedBone;
        if (sel >= 0) {
            PushBoneEditUndo(app);
            app->state.eulerX =      // re-convert stored
                static_cast<float>(                        // degrees
                    static_cast<double>(
                        app->state.eulerX) *
                    kPiLit / 180.0);
            app->state.eulerY =
                static_cast<float>(-v * kPiLit / 180.0);
            app->state.eulerZ =
                static_cast<float>(kPiLit *
                                   -static_cast<double>(
                                       app->state.eulerZ) /
                                   180.0);
            ComposeEulerToBone(app, model, sel);
        }
        PostViewRefresh(app);
        return;
    }
    if (edit == GetDlgItem(base, panel::kReadoutRotZEdit)) {           // 0x44C434
        char text[256];
        GetWindowTextA(edit, text, 100);
        const double v = atof(text);
        if (cameraMode) {
            app->CameraRoll() = static_cast<float>(v * kPiLit / 180.0);
            RefreshRequest(-1);
            PostViewRefresh(app);
            return;
        }
        unsigned char* model = SlotModel(app);
        const int sel = mikumikustudio::mdl::Mdl(model)->selectedBone;
        if (sel >= 0) {
            PushBoneEditUndo(app);
            app->state.eulerX =
                static_cast<float>(
                    static_cast<double>(
                        app->state.eulerX) *
                    kPiLit / 180.0);
            app->state.eulerY =
                static_cast<float>(-static_cast<double>(
                                       app->state.eulerY) *
                                   kPiLit / 180.0);
            app->state.eulerZ =
                static_cast<float>(kPiLit * -v / 180.0);
            ComposeEulerToBone(app, model, sel);
        }
        PostViewRefresh(app);
        return;
    }
    if (edit == GetDlgItem(base, panel::kReadoutDistEdit) && cameraMode) {  // 0x44C561
        char text[256];
        GetWindowTextA(edit, text, 100);
        const double v = atof(text);
        app->CameraDistance() = static_cast<float>(-v);
        RefreshRequest(-1);
        PostViewRefresh(app);
        return;
    }
    const int scaleAxis = edit == GetDlgItem(base, panel::kReadoutScaleXEdit) ? 0
                        : edit == GetDlgItem(base, panel::kReadoutScaleYEdit) ? 1
                        : edit == GetDlgItem(base, panel::kReadoutScaleZEdit) ? 2 : -1;
    if (scaleAxis >= 0 && !cameraMode) {
        char text[256]{};
        GetWindowTextA(edit, text, sizeof(text));
        const float value = static_cast<float>(std::atof(text));
        if (std::isfinite(value) && value > 0.0f &&
            bone_scale::Enabled(app)) {
            PushBoneEditUndo(app);
            if (SetSelectedBoneScaleAxis(app, scaleAxis, value)) {
                unsigned char* model = SlotModel(app);
                auto* record = mdl::Mdl(model);
                const int selected = record->selectedBone;
                if (record->bonePhysicsState)
                    record->bonePhysicsState[selected] = 1;
                app->SceneModified() = 1;
                app->RecordedFrameCountByte1() = 1;
                RefreshRequest(-1);
            }
        }
        PostViewRefresh(app);
        return;
    }
    PostViewRefresh(app);                           // 0x44C5B1 fallthrough
}

// ---------------------------------------------------------------------------
// VA 0x00463640 - edit commit (see file header for the per-id behaviour).
// ---------------------------------------------------------------------------
void CommitEditControl(MMDApp* app, HWND edit) {  // was Sub463640
    auto& s = *app;
    const HWND main = s.MainWindow();
    // Commit consumes the Enter press. A focus-change commit must not create
    // a fresh press for the frame pump's key-registration path.
    s.state.enterKeyState = 3;
#if !MMD_ORIGINAL_PROFILE
    if (CommitBoundMorphNumber(app, edit)) return;
    if (CommitBoundShadowNumber(app, edit)) return;
    if (CommitBoundAccessoryNumber(app, edit)) return;
    if (CommitBoundFovNumber(app, edit)) return;
    if (CommitBoundLightNumber(app, edit)) return;
#endif

    if (edit == GetDlgItem(main, panel::kCurrentFrameEdit)) {            // frame number
        char text[256];
        GetWindowTextA(edit,text,sizeof text);
        std::int32_t frame=0;
        if(GetWindowTextLengthA(edit)>=sizeof(text)||!frame_limits::ParseSigned(text,frame)){PostViewRefresh(app);MessageBeep(MB_ICONWARNING);return;}
        s.CurrentFrame()=frame;
        RefreshAfterFrameApply(app);
        PostViewRefresh(app);
        CommitEditControlTail(app, edit);
        return;
    }
    if (edit == GetDlgItem(main, panel::kLightColorEditR) ||            // center x
        edit == GetDlgItem(main, panel::kLightColorEditG) || edit == GetDlgItem(main, panel::kLightColorEditB)) {
        char text[256];
        GetWindowTextA(edit, text, 8);
        const double v = static_cast<double>(atol(text)) * 0.00390625;
        const int id = edit == GetDlgItem(main, panel::kLightColorEditR) ? 461
                     : edit == GetDlgItem(main, panel::kLightColorEditG) ? 462 : 463;
        s.LightColor()[id - 461] = static_cast<float>(v);
        // On x64 the port mirrors D3DLIGHT9.Ambient separately rather
        // than overlaying lightColor (mmd_app.hpp:1452-1458), so this path
        // previously updated only the compatibility field, leaving the scene light
        // with its old value until the next light-track evaluation. The slider
        // path synchronizes both fields (ui_hscroll.cpp:183-184); the same
        // write is performed here.
        // Defect B1-01.
        reinterpret_cast<float*>(&s.SceneLight().Ambient)[id - 461] =
            static_cast<float>(v);
        SendMessageA(GetDlgItem(main, 455 + (id - 461)), TBM_SETPOS, 1,
                     static_cast<LPARAM>(static_cast<int>(v * 256.0)));
        RefreshRequest(-2);
        CommitEditControlTail(app, edit);
        return;
    }
    if (edit == GetDlgItem(main, panel::kLightDirEditX) ||            // rot x/y/z mirror
        edit == GetDlgItem(main, panel::kLightDirEditY) || edit == GetDlgItem(main, panel::kLightDirEditZ)) {
        char text[256];
        GetWindowTextA(edit, text, 8);
        const double v = atof(text);
        const int id = edit == GetDlgItem(main, panel::kLightDirEditX) ? 464
                     : edit == GetDlgItem(main, panel::kLightDirEditY) ? 465 : 466;
        s.LightDirection()[id - 464] = static_cast<float>(v);
        // On x64 the port stores a separate mirror of D3DLIGHT9 instead of overlaying
        // the PMM light scalars (mmd_app.hpp:1452-1458), so this path
        // previously updated only the compatibility field, leaving the scene
        // light stale until the next light-track evaluation. The slider path
        // applies the same helper (ui_hscroll.cpp:148-150, 225-230), and this code
        // mirrors that logic.
        // Defect B1-01.
        s.ApplyTimelineLightState();
        s.Renderer()->device->SetLight(0, &s.SceneLight());
        SendMessageA(GetDlgItem(main, 458 + (id - 464)), TBM_SETPOS, 1,
                     static_cast<LPARAM>(static_cast<int>(v * 100.0)));
        RefreshRequest(-2);
        CommitEditControlTail(app, edit);
        return;
    }
    if (edit == GetDlgItem(main, panel::kFovEdit)) {            // fov + projection
        char text[256];
        GetWindowTextA(edit, text, 8);
        const float v = static_cast<float>(atol(text));
        s.CameraFov() = v;
        SendMessageA(GetDlgItem(main, panel::kFovSlider), TBM_SETPOS, 1,
                     static_cast<LPARAM>(static_cast<int>(v)));
        // The commit previously rebuilt the perspective matrix inline, which
        // duplicated the 447 slider body (ui_viewport_refresh.cpp:140-149) and
        // bypassed its guards: a non-positive wrapper aspect fell through to
        // the matrix build, a null wrapper or device was dereferenced without
        // a check, a missing d3dx import skipped the SetTransform entirely,
        // and the per-frame orthographic override (x64
        // 0x140027E96..0x140027F1F) was ignored.  The slider path installs the
        // projection through ApplyFrameCameraProjection; this commit now does
        // the same.
        // Defect B1-06.
        ApplyFrameCameraProjection(app);
        RefreshRequest(-1);
        CommitEditControlTail(app, edit);
        return;
    }
    // 478..485: light-accessory fields (id = 0x1DE..0x1E5).
    {
        mdl::AccessoryRecord* acc = LightAccessory(app);
        int lightId = 0;
        if (edit == GetDlgItem(main, panel::kAccPosXEdit)) lightId = 478;
        else if (edit == GetDlgItem(main, panel::kAccPosYEdit)) lightId = 479;
        else if (edit == GetDlgItem(main, panel::kAccPosZEdit)) lightId = 480;
        else if (edit == GetDlgItem(main, panel::kAccRotXEdit)) lightId = 481;
        else if (edit == GetDlgItem(main, panel::kAccRotYEdit)) lightId = 482;
        else if (edit == GetDlgItem(main, panel::kAccRotZEdit)) lightId = 483;
        else if (edit == GetDlgItem(main, panel::kAccScaleXEdit)) lightId = 484;
        else if (edit == GetDlgItem(main, panel::kAccScaleYEdit)) lightId = 485;
        if (lightId != 0 && acc != nullptr) {
            char text[256];
            GetWindowTextA(edit, text, 8);
            const double v = atof(text);
            if (lightId <= 480) {
                acc->position[lightId - 478] = static_cast<float>(v);
            } else if (lightId <= 483) {
                acc->rotation[lightId - 481] =
                    static_cast<float>(v / 180.0 * kPiLit);
            } else if (lightId == 484) {
                acc->scale = static_cast<float>(v);
            } else {                                 // 485: clamp + rewrite
                // v7 = 0; if (v < 0 || (v7 = 1, v > 1)) v = v7
                double out = v;
                if (v < 0.0)
                    out = 0.0;
                else if (v > 1.0)
                    out = 1.0;
                acc->opacity = static_cast<float>(out);
                const HWND same = GetDlgItem(main, panel::kAccScaleYEdit);
                const LPARAM len = GetWindowTextLengthA(same);
                SendMessageA(same, EM_SETSEL, 0, len);
                char fmt[256];
                sprintf_s(fmt, 0x100, "%3.2f", out);
                SendMessageA(same, EM_REPLACESEL, 0,
                             reinterpret_cast<LPARAM>(fmt));
            }
            RefreshRequest(s.SelectedAccessorySlot());
            CommitEditControlTail(app, edit);
            return;
        }
        if (lightId != 0) {                          // acc == null: tail only
            RefreshRequest(s.SelectedAccessorySlot());
            CommitEditControlTail(app, edit);
            return;
        }
    }
    if (edit == GetDlgItem(main, panel::kSelfShadowRangeEdit)) {            // 0x231 shadow range
        char text[256];
        GetWindowTextA(edit, text, 8);
        const double v = atof(text);
#if MMD_ORIGINAL_PROFILE
        s.state.physicsInterval = static_cast<float>((10000.0 - v) / 100000.0);
#else
        SetShadowSliderPosition(app, static_cast<int>(v));
#endif
        SendMessageA(GetDlgItem(main, panel::kSelfShadowRangeSlider), TBM_SETPOS, 1,
                     static_cast<LPARAM>(static_cast<int>(v)));
        RefreshRequest(-3);
        CommitEditControlTail(app, edit);
        return;
    }
    // 506/511/516/521: morph values of the active model.  The four controls
    // address the four selector slots in order; their control IDs are spaced
    // by five, but their model fields are consecutive.
    {
        int morphEdit = 0;
        if (edit == GetDlgItem(main, panel::kMorphEdit0)) morphEdit = 506;
        else if (edit == GetDlgItem(main, panel::kMorphEdit1)) morphEdit = 511;
        else if (edit == GetDlgItem(main, panel::kMorphEdit2)) morphEdit = 516;
        else if (edit == GetDlgItem(main, panel::kMorphEdit3)) morphEdit = 521;
        if (morphEdit != 0) {
            char text[256];
            GetWindowTextA(edit, text, 8);
            const double v = atof(text);
            unsigned char* model = SlotModel(app);
            const int selector =
                mikumikustudio::mdl::Mdl(model)->selectedMorphs[
                    (morphEdit - 506) / 5];
            if (mikumikustudio::mdl::Morphs(model) != nullptr && selector >= 0)
                mikumikustudio::mdl::Morphs(model)[selector].value =
                    static_cast<float>(v);
            SendMessageA(GetDlgItem(main, morphEdit - 1), TBM_SETPOS, 1,
                         static_cast<LPARAM>(static_cast<int>(v * 100.0)));
            CommitEditControlTail(app, edit);
            return;
        }
    }
    CommitEditControlTail(app, edit);                           // default tail
}

// ---------------------------------------------------------------------------
// VA 0x00464170 - EDIT subclass proc.
// ---------------------------------------------------------------------------
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT Msg, WPARAM wParam,
                                  LPARAM lParam) {
    MMDApp* app = g_Block;
    const auto original = app->OriginalEditProc();
    const int controlId = GetDlgCtrlID(hWnd);
    const bool prefixMessage = ui::TransformEditShowsPrefix(controlId) &&
        (Msg == WM_KILLFOCUS || Msg == WM_SETTEXT || Msg == EM_REPLACESEL);
    const HWND owner = prefixMessage ? GetParent(hWnd) : nullptr;
    const auto windowProc = prefixMessage ? GetWindowLongPtrW(hWnd, GWLP_WNDPROC) : 0;
    const DWORD thread = prefixMessage ? GetWindowThreadProcessId(hWnd, nullptr) : 0;

    if (Msg == WM_SETFOCUS) {
        BeginBoneReadoutDraft(*app, hWnd, controlId);
    } else if (Msg == WM_NCDESTROY) {
        if (boneReadoutDraft && boneReadoutDraft->field == hWnd) boneReadoutDraft.reset();
    } else if (Msg == WM_KEYDOWN) {
        if (wParam == 13) {                          // VK_RETURN
            // Moving focus commits through WM_KILLFOCUS. Do not create a
            // second undo entry for the same value when Enter is pressed.
            if (GetFocus() != hWnd && !DiscardStaleBoneReadout(*app, hWnd))
                CommitEditControl(app, hWnd);
            if (g_Block == app) SetFocus(app->MainWindow());
            return 0;
        }
    } else if (Msg == WM_KILLFOCUS) {
        // wParam is the next focus window, not the field being edited.
        // The typing is being committed: let the view refresh echo it back.
        bool boundPose=false;
        bool shouldCommit = !DiscardStaleBoneReadout(*app, hWnd);
        if (g_Block != app || !IsWindow(hWnd)) return 0;
#if !MMD_ORIGINAL_PROFILE
        HWND base=app->FloatingWindow()?app->FloatingWindow():app->MainWindow();
        int kind=-1,axis=-1;
        boundPose=plugins::BoundNativePoseHistory(app) && (app->CameraMode() ?
            FindCameraNumber(hWnd,base)>=0 : FindPoseNumber(hWnd,base,kind,axis));
        boundPose = boundPose || (plugins::BoundNativePoseHistory(app) && FindLightNumber(app,hWnd)>=0);
        boundPose = boundPose || (plugins::BoundNativePoseHistory(app) && IsFovNumber(app,hWnd));
        boundPose = boundPose || (plugins::BoundNativePoseHistory(app) && IsShadowNumber(app,hWnd));
        boundPose = boundPose || (plugins::BoundNativePoseHistory(app) && FindMorphNumber(app,hWnd)>=0);
        boundPose = boundPose || (plugins::BoundNativePoseHistory(app) && FindAccessoryNumber(app,hWnd)>=0);
        // Without a bound history the legacy tail would rewrite the readout
        // text and push a bone undo. A bone field whose modify flag is already
        // clear (whole-skeleton reset dropped the draft) has nothing to commit.
        // Read the flag here, before the legacy clear below consumes it. A
        // real edit keeps the flag set and keeps the original commit.
        if(!plugins::BoundNativePoseHistory(app) && !app->CameraMode() &&
           FindPoseNumber(hWnd,base,kind,axis) &&
           !SendMessageA(hWnd,EM_GETMODIFY,0,0))
            shouldCommit=false;
#endif
        // The bound pose route consumes this flag itself, before changing any
        // model state. Legacy controls retain their existing commit behavior.
        if(!boundPose)SendMessageA(hWnd, EM_SETMODIFY, FALSE, 0);
        // Recording hides/restores the editor while its text may still show
        // the pre-export frame. Those focus changes are not user edits.
        if (shouldCommit && app->FrameStepPlayback() == 0 &&
            app->state.recordPlaybackStartPending == 0)
            CommitEditControl(app, hWnd);
        if(boundPose)SendMessageA(hWnd, EM_SETMODIFY, FALSE, 0);
    }
    const auto result = CallWindowProcA(original, hWnd, Msg, wParam, lParam);
    // Text/commit callbacks can destroy, move or refocus the field. Do not use
    // app after forwarding; presentation must target the same live control.
    const auto current = [&] {
        return g_Block == app && IsWindow(hWnd) && owner && GetParent(hWnd) == owner &&
            GetDlgCtrlID(hWnd) == controlId && GetDlgItem(owner, controlId) == hWnd &&
            GetWindowThreadProcessId(hWnd, nullptr) == thread && thread == GetCurrentThreadId() &&
            GetWindowLongPtrW(hWnd, GWLP_WNDPROC) == windowProc && GetFocus() != hWnd;
    };
    if (prefixMessage && current()) {
        // A selection notification can recursively update this or another
        // field. Permit other fields, but never enter this field twice.
        struct Visit { HWND window; Visit* previous; };
        static thread_local Visit* active = nullptr;
        for (auto* visit = active; visit; visit = visit->previous)
            if (visit->window == hWnd) return result;
        Visit visit{hWnd, active};
        struct Restore { Visit*& active; Visit* previous; ~Restore() { active = previous; } } restore{active, active};
        active = &visit;
        SendMessageA(hWnd, EM_SETSEL, 0, 0);
        if (current()) SendMessageA(hWnd, EM_SCROLLCARET, 0, 0);
    }
    return result;
}

// ---------------------------------------------------------------------------
// VA 0x0040EC90 - trackbar subclass proc.
// ---------------------------------------------------------------------------
#if MMD_ORIGINAL_PROFILE
LRESULT CALLBACK TrackbarSubclassProc(HWND hWnd, UINT Msg, WPARAM wParam,
                                      LPARAM lParam) {
    MMDApp* app = g_Block;
    if (Msg == WM_LBUTTONUP)
        SetFocus(app->MainWindow());
    return CallWindowProcA(
        app->OriginalTrackbarProc(),
        hWnd, Msg, wParam, lParam);
}
#endif

// ---------------------------------------------------------------------------
// The 42 SetWindowLongPtrA(GWLP_WNDPROC) install sites of sub_466D20
// (0x467BE4..0x46A8E7), in original per-site order.  The first EDIT (417)
// additionally captures the EDIT class proc into app+0xA08D0 (0x467BD7) and
// the first trackbar (447) captures the trackbar class proc into
// app+0xA0CDC (0x4686E2); every later site only installs.
// ---------------------------------------------------------------------------
void InstallControlSubclasses(MMDApp* app, HWND hwnd) {
    static const int kEditIds[] = {
        417, 448, 461, 462, 463, 464, 465, 466,
        478, 479, 480, 481, 482, 483, 484, 485,
        506, 511, 516, 521, 544, 545, 546, 547, 548, 549, 550, 554, 561,
        572, 575, 577,
    };
#if MMD_ORIGINAL_PROFILE
    static const int kTrackIds[] = {
        447, 455, 456, 457, 458, 459, 460,
        505, 510, 515, 520, 534, 560,
    };
#endif

    HWND firstEdit = GetDlgItem(hwnd, panel::kCurrentFrameEdit);
    app->OriginalEditProc() = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrA(firstEdit, GWLP_WNDPROC));     // 0x467BD7
    SetWindowLongPtrA(firstEdit, GWLP_WNDPROC,
                   reinterpret_cast<LONG_PTR>(EditSubclassProc));

#if MMD_ORIGINAL_PROFILE
    HWND firstTrack = GetDlgItem(hwnd, panel::kFovSlider);
    app->OriginalTrackbarProc() = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrA(firstTrack, GWLP_WNDPROC));    // 0x4686E2
    SetWindowLongPtrA(firstTrack, GWLP_WNDPROC,
                   reinterpret_cast<LONG_PTR>(TrackbarSubclassProc));
#endif

    for (int id : kEditIds) {
        if (id == 417)
            continue;
        SetWindowLongPtrA(GetDlgItem(hwnd, id), GWLP_WNDPROC,
                       reinterpret_cast<LONG_PTR>(EditSubclassProc));
    }
#if MMD_ORIGINAL_PROFILE
    for (int id : kTrackIds) {
        if (id == 447)
            continue;
        SetWindowLongPtrA(GetDlgItem(hwnd, id), GWLP_WNDPROC,
                       reinterpret_cast<LONG_PTR>(TrackbarSubclassProc));
    }
#else
    for (HWND control = GetWindow(hwnd, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT))
        ui::AttachBottomTrackbar(*app, control);
#endif
}

void InstallControlSubclass(MMDApp* app, HWND control, int id) {
    static const int kEditIds[] = {
        417, 448, 461, 462, 463, 464, 465, 466,
        478, 479, 480, 481, 482, 483, 484, 485,
        506, 511, 516, 521, 544, 545, 546, 547, 548, 549, 550, 554, 561,
        572, 575, 577,
    };
#if MMD_ORIGINAL_PROFILE
    static const int kTrackIds[] = {
        447, 455, 456, 457, 458, 459, 460,
        505, 510, 515, 520, 534, 560,
    };
#endif
    if (control == nullptr)
        return;
    for (int editId : kEditIds) {
        if (id != editId)
            continue;
        if (id == 417) {
            app->OriginalEditProc() = reinterpret_cast<WNDPROC>(
                GetWindowLongPtrA(control, GWLP_WNDPROC));
        }
        SetWindowLongPtrA(control, GWLP_WNDPROC,
                       reinterpret_cast<LONG_PTR>(EditSubclassProc));
        return;
    }
#if MMD_ORIGINAL_PROFILE
    for (int trackId : kTrackIds) {
        if (id != trackId)
            continue;
        if (id == 447) {
            app->OriginalTrackbarProc() = reinterpret_cast<WNDPROC>(
                GetWindowLongPtrA(control, GWLP_WNDPROC));
        }
        SetWindowLongPtrA(control, GWLP_WNDPROC,
                       reinterpret_cast<LONG_PTR>(TrackbarSubclassProc));
        return;
    }
#else
    ui::AttachBottomTrackbar(*app, control);
#endif
}

}  // namespace mikumikustudio
