#include "mikumikustudio/build_profile.hpp"
// ===========================================================================
// VA 0x0040D940 / 0x0044D610 - model/camera panel refresh helpers
// ===========================================================================
#include "mikumikustudio/animation_curve_host.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <d3d9.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"

namespace mikumikustudio {

void SelectionReeval(MMDApp* app);  // 0x430510
void ApplyGravityTrack(MMDApp* app);        // 0x412330
void ApplyAccessoryTrack(MMDApp* app, int);   // 0x413120

namespace {

HWND MainControl(MMDApp* app, int id) {
    return GetDlgItem(static_cast<HWND>(app->Hwnd()), id);
}

void ShowControlRange(MMDApp* app, int first, int last, bool show) {
    for (int id = first; id <= last; ++id)
        ShowWindow(MainControl(app, id), show ? SW_SHOWNORMAL : SW_HIDE);
}

unsigned char* ModelAt(MMDApp* app, int slot) {
    return app->ModelSlot(slot);
}

int FindModelSlotByComboId(MMDApp* app, int comboId) {
    // x64 twin sub_7FF7CB486B0: both comboSelIndex (model+0x3108/x86 11644)
    // scans run to 255 (cmp ecx, 0FFh at 0x7FF7CB48724D and 0x7FF7CB48735A).
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        unsigned char* model = ModelAt(app, slot);
        if (model != nullptr && mdl::Mdl(model)->comboSelIndex == comboId)
            return slot;
    }
    return -1;
}

void EnableMenuCommands(HMENU menu, const int* ids, std::size_t count,
                        UINT state) {
    for (std::size_t i = 0; i < count; ++i)
        EnableMenuItem(menu, ids[i], state);
}

mikumikustudio::mdl::BoneRecord* BoneAt(unsigned char* model, int boneIndex) {
    return mikumikustudio::mdl::Bones(model) + boneIndex;
}

void BonePoint(mikumikustudio::mdl::BoneRecord* bone, float out[3]) {
    const float* matrix = reinterpret_cast<const float*>(bone->matInit);
    const float* point = reinterpret_cast<const float*>(bone->position);
    out[0] = matrix[0] * point[0] + matrix[4] * point[1] +
             matrix[8] * point[2] + matrix[12];
    out[1] = matrix[1] * point[0] + matrix[5] * point[1] +
             matrix[9] * point[2] + matrix[13];
    out[2] = matrix[2] * point[0] + matrix[6] * point[1] +
             matrix[10] * point[2] + matrix[14];
}

}  // namespace

void PostModelReload(MMDApp* app) {  // 0x41A650
#ifdef MIKUMIKUSTUDIO_DIAG
    if (getenv("MIKUMIKUSTUDIO_TRACE_REC")) {
        FILE* tf = fopen(getenv("MIKUMIKUSTUDIO_TRACE_REC"), "a");
        if (tf) { fprintf(tf, "PostModelReload targetSlot=%d\n",
                          app->CameraParentModel()); fclose(tf); }
    }
#endif
    // The camera panel applies its explicit parent. Free model view applies
    // that retained parent only when camera/light tracking is enabled.
    if (app->state.followCameraEnabled == 0 &&
        (app->CameraMode() == 0 || app->CameraParentModel() < 0))
        return;

    auto& api = d3dx::Get();
    if (!api.Load())
        return;

    d3dx::D3DXMATRIXF rotationY{};
    d3dx::D3DXMATRIXF rotation{};
    d3dx::D3DXMATRIXF scratch{};
    api.rotY(&rotationY, app->CameraYaw());
    api.rotX(&rotation, app->CameraPitch());
    api.multiply(&rotation, &rotationY, &rotation);
    api.rotZ(&scratch, app->CameraRoll());
    api.multiply(&rotation, &rotation, &scratch);

    const int targetSlot = app->CameraParentModel();
    if (targetSlot >= 0) {
        unsigned char* targetModel = ModelAt(app, targetSlot);
        mikumikustudio::mdl::BoneRecord* targetBone = BoneAt(
            targetModel, app->CameraParentBone());
        auto& targetBasis = reinterpret_cast<d3dx::D3DXMATRIXF&>(
            app->CameraAttachmentBasis());
        const float* boneMatrix =
            reinterpret_cast<const float*>(&targetBone->matInit[0]);
        targetBasis = {};
        targetBasis.m[0][0] = boneMatrix[0];
        targetBasis.m[0][1] = boneMatrix[4];
        targetBasis.m[0][2] = boneMatrix[8];
        targetBasis.m[1][0] = boneMatrix[1];
        targetBasis.m[1][1] = boneMatrix[5];
        targetBasis.m[1][2] = boneMatrix[9];
        targetBasis.m[2][0] = boneMatrix[2];
        targetBasis.m[2][1] = boneMatrix[6];
        targetBasis.m[2][2] = boneMatrix[10];
        targetBasis.m[3][3] = 1.0f;
        api.multiply(&rotation, &targetBasis, &rotation);

        // SetupFrameWorldTransform already translates the world by the parent
        // point. The view eye carries only the editable relative offset;
        // adding the parent again makes the binding move across the image.
        app->ViewOffsetX() = app->CameraPositionX();
        app->ViewOffsetY() = app->CameraPositionY();
        app->CameraDistance() = app->CameraPositionZ();
        return;
    }

    // The world transform already applies the unparented camera target.
    // Adding it to the eye again makes each edit refresh move the camera.
    app->ViewOffsetX() = 0.0f;
    app->ViewOffsetY() = 0.0f;
}

void ApplyCurrentCameraView(MMDApp* app) {
    // The camera panel starts from the current camera key. A retained parent
    // only affects the eye when the user's camera/light tracking option is on.
    float* basis = &app->CameraAttachmentBasis().m[0][0];
    std::memset(basis, 0, 16 * sizeof(float));
    basis[0] = 1.0f;
    basis[5] = 1.0f;
    basis[10] = 1.0f;
    basis[15] = 1.0f;

    app->ViewOffsetX() = 0.0f;
    app->ViewOffsetY() = 0.0f;
    app->CameraAttachmentTransformSuppressed() = 0;
    if ((app->state.followCameraEnabled != 0 || app->CameraMode() != 0) &&
        app->CameraParentModel() >= 0)
        PostModelReload(app);
}

// (Sub41A650 was a local alias wrapper of PostModelReload above - removed;
//  callers now use the PostModelReload name directly.)

void PostModelReload2(MMDApp* app) {  // 0x40D940
    curve_host::ShowControls(app, app->state.optflag[0] != 0 && app->state.optflag[3] != 0);
    const bool cameraMode =
        app->state.optflag[0] != 0;
#if !MMD_ORIGINAL_PROFILE
    ShowWindow(MainControl(app, panel::kAdaptiveShadowCheckbox),
        cameraMode && app->state.optflag[6] != 0 ? SW_SHOWNORMAL : SW_HIDE);
    ShowWindow(MainControl(app, panel::kShadowFilterCheckbox),
        cameraMode && app->state.optflag[6] != 0 ? SW_SHOWNORMAL : SW_HIDE);
#endif

    const struct {
        int first;
        int last;
        int collapsed;
        int flagIndex;
    } cameraGroups[] = {
        {446, 453, 454, 1},
        {455, 469, 470, 2},
        {471, 488, 489, 3},
        {560, 566, 567, 6},
    };
    for (const auto& group : cameraGroups) {
        const bool expanded = cameraMode &&
            app->state.optflag[group.flagIndex] != 0;
        ShowControlRange(app, group.first, group.last, expanded);
        ShowWindow(MainControl(app, group.collapsed),
                   cameraMode && !expanded ? SW_SHOWNORMAL : SW_HIDE);
    }

    const bool modelSelected =
        SendMessageA(MainControl(app, 436), CB_GETCURSEL, 0, 0) != 0;
    if (!modelSelected)
        SendMessageA(MainControl(app, 443), CB_SETCURSEL, 0, 0);
    for (int id = 437; id <= 445; ++id)
        EnableWindow(MainControl(app, id), modelSelected);

    const struct {
        int first;
        int last;
        int collapsed;
        int flagIndex;
    } modelGroups[] = {
        {490, 502, 503, 4},
        {504, 528, 529, 5},
    };
    for (const auto& group : modelGroups) {
        const bool expanded = !cameraMode &&
            app->state.optflag[group.flagIndex] != 0;
        ShowControlRange(app, group.first, group.last, expanded);
        ShowWindow(MainControl(app, group.collapsed),
                   !cameraMode && !expanded ? SW_SHOWNORMAL : SW_HIDE);
    }

    // 0x40DC3A..0x40DC6D: until the bone-copy buffer contains records,
    // paste and reverse-paste remain disabled.  A successful copy command
    // enables both controls and stores the record count at 0x9DA24.
    if (!cameraMode &&
        app->state.optflag[5] != 0 &&
        app->state.copiedBoneCount == 0) {
        EnableWindow(MainControl(app, 497), FALSE);
        EnableWindow(MainControl(app, 498), FALSE);
    }

    HWND viewportWindow =
        app->state.floatingWindow;
    if (viewportWindow == nullptr)
        viewportWindow = static_cast<HWND>(app->Hwnd());

    if (cameraMode) {
        EnableWindow(MainControl(app, 400), FALSE);
        EnableWindow(MainControl(app, 401), FALSE);
        if (app->EnglishUI() != 0) {
            SetUiControlText(app, MainControl(app, 407), "Btm");
            SetUiControlText(app, GetDlgItem(viewportWindow, panel::kModelEditToggle), "To model");
        } else {
            SetWindowTextW(MainControl(app, 407), L"\x4e0b\x9762");
            SetWindowTextW(GetDlgItem(viewportWindow, panel::kModelEditToggle),
                           L"\x30e2\x30c7\x30eb\x7de8");
        }
        ShowWindow(GetDlgItem(viewportWindow, panel::kCameraDistanceButton), SW_SHOW);
        ShowWindow(GetDlgItem(viewportWindow, panel::kReadoutDistEdit), SW_SHOW);
        return;
    }

    auto* model = app->SelectedModel();
    // undoDirty/redoDirty（x64 model+0x3558/+0x3559，0x7FF7CB447074/86）：
    // 撤消/重做按钮的开门标志。
    EnableWindow(MainControl(app, 400),
                 mdl::Mdl(model)->undoDirty != 0);
    EnableWindow(MainControl(app, 401),
                 mdl::Mdl(model)->redoDirty != 0);
    // MMS: 407 is the bottom view in both panels (command_control_400.cpp).
    if (app->EnglishUI() != 0) {
        SetUiControlText(app, MainControl(app, 407), "Btm");
        SetUiControlText(app, GetDlgItem(viewportWindow, panel::kModelEditToggle), "To camera");
    } else {
        SetWindowTextW(MainControl(app, 407), L"\x4e0b\x9762");
        SetWindowTextW(GetDlgItem(viewportWindow, panel::kModelEditToggle),
                       L"\x30ab\x30e1\x30e9\x7de8");
    }
    ShowWindow(GetDlgItem(viewportWindow, panel::kCameraDistanceButton), SW_HIDE);
    ShowWindow(GetDlgItem(viewportWindow, panel::kReadoutDistEdit), SW_HIDE);
}

void RebuildModelModePanel(MMDApp* app) {  // was Sub44D610, 0x44D610
    // Preserve the free view across a pivot change. With Track enabled,
    // keep the evaluated camera view instead of restoring free-view offsets.
    const float eyeOldX = app->ViewOffsetX();
    const float eyeOldY = app->ViewOffsetY();
    const float eyeOldDistance = app->CameraDistance();
    float focusOld[3] = {0.0f, 0.0f, 0.0f};
    ResolveFrameFocus(app, static_cast<int>(app->CameraReferenceMode()), focusOld);
    app->state.optflag[0] = 0;
    HWND combo = MainControl(app, 433);
    SendMessageA(combo, CB_DELETESTRING, 5, 0);
    SendMessageA(combo, CB_DELETESTRING, 4, 0);
    SendMessageA(combo, CB_SETCURSEL, 3, 0);

    if (app->state.followCameraEnabled != 0) {
        // Keep the camera target values while the panel changes from camera
        // editing to model editing. Clearing these fields here makes the
        // first model frame use the origin and produces a visible jump when
        // switching back from the camera view. PostModelReload below
        // re-anchors the retained values to the tracked bone.
        ReloadModels(app);
        RefreshLightPanel(app);
        RefreshSelfShadowPanel(app);
        ApplyGravityTrack(app);
        for (int index = 0; index < 255; ++index) {
            if (app->ObjectSlot(index) != nullptr)
                ApplyAccessoryTrack(app, index);
        }
        SyncAccessoryEditPanel(app);
    }

    app->CameraAttachmentTransformSuppressed() = 0;
    PostModelReload(app);
    SelectionReeval(app);

    // Keep the camera panel's live light, including edits not registered as keys.
    app->ApplyTimelineLightState();
    app->Renderer()->device->SetLight(0, &app->SceneLight());

    // Tracking uses the camera target already applied by the world transform.
    // Only the free view needs compensation for a change of orbit pivot.
    if (app->state.followCameraEnabled != 0) {
        ApplyCurrentCameraView(app);
    } else {
        app->ViewOffsetX() = eyeOldX;
        app->ViewOffsetY() = eyeOldY;
        app->CameraDistance() = eyeOldDistance;
        float focusNew[3] = {0.0f, 0.0f, 0.0f};
        ResolveFrameFocus(app, static_cast<int>(app->CameraReferenceMode()), focusNew);
        CarryEyeForFocusChange(app, focusOld, focusNew);
    }
}

void RebuildCameraModePanel(MMDApp* app) {  // was Sub44D780, 0x44D780
    // Entering the camera panel must show the camera key at the current frame.
    // In particular, do not restore the model-editing eye/distance after the
    // reload below: that used to discard the keyed camera distance and preserve
    // the free viewport instead of switching to the camera view.
    // Rebuild the camera-mode transform combo to match the original exactly.
    // Item four is the model-mode "all" entry; the three camera entries are
    // appended in its place.
    app->state.optflag[0] = 1;
    HWND combo = MainControl(app, 433);
    SendMessageA(combo, CB_DELETESTRING, 4, 0);
    if (app->EnglishUI() != 0) {
        AddUiComboText(app, combo, "distance");
        AddUiComboText(app, combo, "view angle");
        AddUiComboText(app, combo, "all");
    } else {
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"\x8ddd\x96e2"));
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"\x8996\x91ce\x89d2"));
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"\x3059\x3079\x3066"));
    }

    SelectionReeval(app);
    ReloadModels(app);
    RefreshLightPanel(app);
    RefreshSelfShadowPanel(app);
    ApplyGravityTrack(app);
    for (int slot = 0; slot < 255; ++slot) {
        if (app->ObjectSlot(slot) != nullptr)
            ApplyAccessoryTrack(app, slot);
    }
    SyncAccessoryEditPanel(app);

    // The current camera key owns the camera panel's eye and attachment.
    ApplyCurrentCameraView(app);
}

int ResolveModelComboSelection(MMDApp* app) {
    if (!app) return 0;
    const int count = static_cast<int>(SendMessageW(
        MainControl(app, panel::kMainComboModel), CB_GETCOUNT, 0, 0));
    const auto valid = [&](int row) {
        return row > 0 && row < count && FindModelSlotByComboId(app, row) >= 0;
    };
    int row = static_cast<int>(app->MainModelComboSelection());
    if (!valid(row)) {
        const auto* selected = app->SelectedModel();
        row = selected ? mdl::Mdl(selected)->comboSelIndex : 0;
        if (!valid(row)) {
            for (row = 1; row < count && !valid(row); ++row) {}
            if (row >= count) row = 0;
        }
    }
    app->MainModelComboSelection() = row;
    return row;
}

void ApplyModelComboSelection(MMDApp* app) {  // was Sub44D940, 0x44D940
    HWND hwnd = static_cast<HWND>(app->Hwnd());
    const int selection = static_cast<int>(
        SendMessageA(GetDlgItem(hwnd, panel::kMainComboModel), CB_GETCURSEL, 0, 0));
    static constexpr int kModeCommands[] = {
        0xFD, 0xD9, 0xDC, 0xDA, 0xCA, 0xCB, 0xDB, 0xDE,
        0xFB, 0xFC,
    };

    if (selection == 0) {
        // PMM can open in camera mode without ever selecting a model row.
        // Remember a loaded model for the viewport's "To model" command.
        ResolveModelComboSelection(app);
        if (app->state.optflag[0] == 0) {
            SendMessageA(GetDlgItem(hwnd, panel::kIkChainCombo), CB_RESETCONTENT, 0, 0);
            HWND frameCombo = GetDlgItem(hwnd, panel::kRegisterScopeCombo);
            SendMessageA(frameCombo, CB_RESETCONTENT, 0, 0);
            if (app->EnglishUI() != 0) {
                static constexpr const char* kNames[] = {
                    "camera", "light", "s shadow", "gravity",
                };
                for (const char* name : kNames)
                    AddUiComboText(app, frameCombo, name);
            } else {
                static constexpr const wchar_t* kNames[] = {
                    L"\x30ab\x30e1\x30e9", L"\x7167\x660e",
                    L"\x30bb\x30eb\x30d5\x5f71", L"\x91cd\x529b",
                };
                for (const wchar_t* name : kNames) {
                    SendMessageW(frameCombo, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(name));
                }
            }

            HWND accessoryCombo = GetDlgItem(hwnd, panel::kAccessoryCombo);
            const int count = static_cast<int>(
                SendMessageA(accessoryCombo, CB_GETCOUNT, 0, 0));
            for (int index = 0; index < count; ++index) {
                wchar_t name[256]{};
                SendMessageW(accessoryCombo, CB_GETLBTEXT, index,
                             reinterpret_cast<LPARAM>(name));
                SendMessageW(frameCombo, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(name));
            }
            SendMessageA(frameCombo, CB_SETCURSEL, 0, 0);
            SendMessageA(GetDlgItem(hwnd, panel::kModelVisibleCheckbox), BM_SETCHECK, BST_UNCHECKED, 0);
            RebuildCameraModePanel(app);
            app->state.optflag[0] = 1;
            PostModelReload2(app);
            HandleWindowSize(app);
            app->EditMode() = ViewportEditMode::None;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        HMENU menu = GetMenu(hwnd);
        EnableMenuCommands(menu, kModeCommands,
                           sizeof(kModeCommands) / sizeof(kModeCommands[0]),
                           MF_GRAYED);
        EnableMenuItem(menu, 250,
                       app->ClipboardCounts().accessories != 0
                           ? MF_ENABLED
                           : MF_GRAYED);
        for (int id = 224; id <= 231; ++id)
            EnableMenuItem(menu, id, MF_GRAYED);
        for (int id = 273; id <= 275; ++id)
            EnableMenuItem(menu, id, MF_GRAYED);
        for (int id = 0xED; id <= 0xF2; ++id)
            EnableMenuItem(menu, id, MF_ENABLED);

        EnableWindow(GetDlgItem(hwnd, panel::kExpandShrinkButton), FALSE);
        const bool canRegister =
            app->ClipboardCounts().accessories != 0 ||
            app->ClipboardCounts().lights != 0 ||
            app->ClipboardCounts().shadows != 0 ||
            app->ClipboardCounts().cameras != 0 ||
            app->ClipboardCounts().gravity != 0;
        EnableWindow(GetDlgItem(hwnd, panel::kPasteButton), canRegister ? TRUE : FALSE);
        EnableWindow(GetDlgItem(hwnd, panel::kReversePasteButton), FALSE);
        SendMessageA(GetDlgItem(hwnd, panel::kShadowCheckbox), BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageA(GetDlgItem(hwnd, panel::kAddBlendCheckbox), BM_SETCHECK, BST_UNCHECKED, 0);
        HWND optionWindow = app->state.edgeThicknessDialog;
        if (optionWindow != nullptr) {
            DestroyWindow(optionWindow);
            app->state.edgeThicknessDialog = nullptr;
        }
        SetModelExtensionMenuEnabled(app, false);
    } else {
        const int selectedSlot = FindModelSlotByComboId(app, selection);
        if (selectedSlot >= 0) {
            app->SetSelectedModelSlot(static_cast<std::uint8_t>(selectedSlot));
            unsigned char* model = ModelAt(app, selectedSlot);
            PostLoadInit(model);
            mdl::Mdl(model)->lightDir[0] = -1.0f;
            mdl::Mdl(model)->lightDir[1] = 90.0f;
            mdl::Mdl(model)->lightDir[2] = 11.5f;
            mdl::Mdl(model)->legIkXOffset = 1.0f;
        }

        if (app->state.optflag[0] != 0) {
            RebuildModelModePanel(app);
            app->state.optflag[0] = 0;
            PostModelReload2(app);
            HandleWindowSize(app);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (app->CameraParentModel() >= 0 &&
                   app->state.followCameraEnabled != 0) {
            const int oldSelection =
                app->state.mainModelComboSelection;
            int oldSlot = 0;
            if (oldSelection != 0) {
                const int found = FindModelSlotByComboId(app, oldSelection);
                if (found >= 0)
                    oldSlot = found;
            }
            const int trackedSlot = app->CameraParentModel();
            if (oldSlot == trackedSlot ||
                app->SelectedModelSlot() == trackedSlot) {
                // Keep the eye (it is the viewpoint); only the attachment state
                // and the reload chain belong to this branch.
                app->CameraAttachmentTransformSuppressed() = 0;
                ReloadModels(app);
                PostModelReload(app);
            }
        }

        app->state.mainModelComboSelection = selection;
        // MMS: one writer for the mode buttons and the merged select /
        // box-select caption (control 490's caption follows the state).
        app->EditMode() = ViewportEditMode::Bone;
        RefreshBoneModePanel(app);

        HMENU menu = GetMenu(hwnd);
        EnableMenuCommands(menu, kModeCommands,
                           sizeof(kModeCommands) / sizeof(kModeCommands[0]),
                           MF_ENABLED);
        EnableMenuItem(menu, 250,
                       app->ClipboardCounts().bones != 0
                           ? MF_ENABLED
                           : MF_GRAYED);
        for (int id = 224; id <= 231; ++id)
            EnableMenuItem(menu, id, MF_ENABLED);
        for (int id = 273; id <= 275; ++id)
            EnableMenuItem(menu, id, MF_ENABLED);
        for (int id = 0xED; id <= 0xF2; ++id)
            EnableMenuItem(menu, id, MF_GRAYED);

        EnableWindow(GetDlgItem(hwnd, panel::kExpandShrinkButton), TRUE);
        EnableWindow(GetDlgItem(hwnd, panel::kPasteButton), FALSE);
        const bool hasMainSelection =
            app->ClipboardCounts().bones != 0;
        const bool hasOtherSelection =
            app->ClipboardCounts().morphs != 0 ||
            app->ClipboardCounts().displays != 0;
        EnableWindow(GetDlgItem(hwnd, panel::kPasteButton),
                     (hasOtherSelection || hasMainSelection) ? TRUE : FALSE);
        EnableWindow(GetDlgItem(hwnd, panel::kReversePasteButton),
                     hasMainSelection ? TRUE : FALSE);

        unsigned char* model = app->SelectedModel();
        HWND optionWindow = app->state.edgeThicknessDialog;
        if (optionWindow != nullptr && model != nullptr) {
            char text[256]{};
            sprintf_s(text, "%3.2f", mikumikustudio::mdl::Mdl(model)->edgeScale);
            HWND edit = GetDlgItem(optionWindow, panel::kEdgeThicknessEdit);
            SendMessageA(edit, EM_SETSEL, 0, GetWindowTextLengthA(edit));
            SendMessageA(edit, EM_REPLACESEL, 0,
                         reinterpret_cast<LPARAM>(text));
            SendMessageA(GetDlgItem(optionWindow, panel::kEdgeThicknessSlider), TBM_SETPOS, TRUE,
                         static_cast<LPARAM>(
                             mikumikustudio::mdl::Mdl(model)->edgeScale *
                             100.0f));
        }
        SetModelExtensionMenuEnabled(app, model != nullptr);
    }

    if (app->state.optflag[0] != 0) {
        EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), FALSE);
        EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE);
    } else {
        unsigned char* model = app->SelectedModel();
        EnableWindow(GetDlgItem(hwnd, panel::kUndoButton),
                     model != nullptr &&
                         mdl::Mdl(model)->undoDirty != 0);
        EnableWindow(GetDlgItem(hwnd, panel::kRedoButton),
                     model != nullptr &&
                         mdl::Mdl(model)->redoDirty != 0);
    }
    PostLanguageSweep(app);
    PostLanguageSweep2(app);
}

}  // namespace mikumikustudio
