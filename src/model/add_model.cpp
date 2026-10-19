// ===========================================================================
// VA 0x00460430 - LoadModelFile / AddModel  (original: sub_460430)
// ===========================================================================
// Model-slot orchestrator: finds a free slot among the kModelSlotCount at
// this+1920,
// allocates the 0x4CCF4 block (operator new + memset + default init
// 0x4A8DC0), runs the loader 0x4BF3E0, and on success registers the model
// name in the three comboboxes (436/474/449 via CB_ADDSTRING), selects it
// (CB_SETCURSEL), refreshes the bone/morph selectors (0x42F1E0 chain),
// resets the manipulation radios (BM_SETCHECK), enables the model-dependent
// menu items, and toggles the physics menu-item state (SetMenuItemInfoA).
// On failure the model is disposed (0x48F830) and the slot cleared.
// When all kModelSlotCount slots are taken the original shows
// "you cannot add models over %d!" (JP: 0x52E198/0x52E18C).
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/ui_language.hpp"

namespace mikumikustudio {
namespace {

#include "model_labels.inc"  // GENERATED (kMsgModelLimitJp / kTitleAddModelJp)

// The original Help popup, located through a command of its own submenu instead
// of a top-level index.  "About" carries command 201 in both menu templates of
// res/templates/menu/menus.rc, and the port keeps its baking popup directly in
// front of Help, so index 7 names the baking popup and not Help.
HMENU HelpMenuFromBar(HMENU menu) {
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW entry{};
        entry.cbSize = sizeof(entry);
        entry.fMask = MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, UINT(i), TRUE, &entry) || entry.hSubMenu == nullptr)
            continue;
        if (GetMenuState(entry.hSubMenu, 201, MF_BYCOMMAND) != UINT(-1))
            return entry.hSubMenu;
    }
    return nullptr;
}

}  // namespace

void LoadModelFile(MMDApp* app, const wchar_t* path, bool showModelInfo) { // 0x460430
    runtime_log::WritePath("LOAD_MODEL", path);
    HWND hwnd = static_cast<HWND>(app->Hwnd());
    int slot = 0;
    while (app->ModelSlot(slot) != nullptr) {
        ++slot;
        if (slot >= kModelSlotCount) {  // x64 槽扫描界 255 @0x7FF7CB4B6705
            app->state.enterKeyState = 1;
            char text[256];
            if (app->EnglishUI() == 0) {
                sprintf_s(text, 0x100, kMsgModelLimitJp, kModelSlotCount);
                MessageBoxA(hwnd, text, kTitleAddModelJp, 0);
            } else {
                sprintf_s(text, 0x100, "you cannot add models over %d!",
                          kModelSlotCount);  // 255 @0x7FF7CB4B6733
                MessageBoxA(hwnd, text, "add model", 0);
            }
            return;
        }
    }

    void* block = operator new(mdl::kSize);   // architecture-correct model ABI
    if (block == nullptr)                     //  thunk 0x4C46F0 in original)
        return;
    unsigned char* model = static_cast<unsigned char*>(block);
    app->ModelSlot(slot) = model;
    std::memset(model, 0, mdl::kSize);
    ModelInitDefaults(model);                                 // 0x4A8DC0

    if (ModelLoadPMD(model, hwnd, path,
                     app->Renderer(), 1,
                     showModelInfo ? 1 : 0,                // a6: box gate
                     app->EnglishUI(),                     // a7: application UI language
                     app->Physics(),                      // a8: -> m+60 scene
                     app->PathWorkspace())) {       // 0x4BF3E0
        app->SceneModified() = 1;
        const auto name = text_encoding::ModelLabel(*mdl::Mdl(model), UiUsesEnglishNames(app->EnglishUI()));
        mdl::Mdl(model)->comboSelIndex = static_cast<std::uint8_t>(SendMessageW(
            GetDlgItem(hwnd, panel::kMainComboModel), CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(name.c_str())));
        SendMessageW(GetDlgItem(hwnd, panel::kMainComboGround), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(name.c_str()));
        SendMessageW(GetDlgItem(hwnd, panel::kMainComboNormal), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(name.c_str()));
        mdl::Mdl(model)->comboSelIndex2 = mdl::Mdl(model)->comboSelIndex;
        SendMessageA(GetDlgItem(hwnd, panel::kMainComboModel), CB_SETCURSEL,
                     mdl::Mdl(model)->comboSelIndex, 0);
        app->state.mainModelComboSelection =
            mdl::Mdl(model)->comboSelIndex;
        app->SetSelectedModelSlot(static_cast<std::uint8_t>(slot));
        // A newly loaded model adds Bullet bodies, but paused playback mode
        // only runs its initial settle pass when this request is pending.
        // Without it, physics starts only after the user presses Play.
        app->PhysicsResetPending() = 1;
        // CB_SETCURSEL below is programmatic and does not send CBN_SELCHANGE.
        // Refresh model-dependent menu state here so the first loaded model
        // can open Model Extension immediately.
        SetModelExtensionMenuEnabled(app, true);
        mikumikustudio::mdl::Mdl(model)->displayState = app->state.characterTransparentMode;

        if (app->state.optflag[0] != 0) {
            RebuildModelModePanel(app);                                   // 0x44D610
            app->state.optflag[0] = 0;
            PostLanguageSweep(app);                           // 0x42F1E0
            PostModelReload2(app);                            // 0x40D940
            HandleWindowSize(app);                            // 0x443300
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            PostLanguageSweep(app);                           // 0x42F1E0
        }

        // MMS: RefreshBoneModePanel writes the four mode buttons (491 scale,
        // 492 move, 493 rotate, 490 select) and the merged select / box-select
        // caption from the stored mode.
        SendMessageA(GetDlgItem(hwnd, panel::kShadowCheckbox), BM_SETCHECK, 1, 0);
        app->EditMode() = ViewportEditMode::Bone;
        RefreshBoneModePanel(app);

        HMENU menu = GetMenu(hwnd);
        static const UINT kEnable[] = {0xFD, 0xD9, 0xDC, 0xDA, 0xCA, 0xCB,
                                       0xDB, 0xDE, 0xFB, 0xFC, 0x120, 0x121};
        for (UINT item : kEnable)
            EnableMenuItem(menu, item, 0);
        for (UINT j = 224; j <= 231; ++j)
            EnableMenuItem(GetMenu(hwnd), j, 0);
        for (UINT k = 273; k <= 275; ++k)
            EnableMenuItem(GetMenu(hwnd), k, 0);
        // x64 在 273..275 启用循环后从 0xED 起依次禁用 237..242
        // (@0x7FF7CB4B6CBC..0x4B6D75)，无 EnableMenuItem(menu, 1, 0) 调用。
        static const UINT kDisable[] = {0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2};
        for (UINT item : kDisable)
            EnableMenuItem(GetMenu(hwnd), item, 1);
        EnableWindow(GetDlgItem(hwnd, panel::kExpandShrinkButton), 1);

        // x64 @0x7FF7CB4B6DBD..0x4B6DFC：cbSize=80(=sizeof on x64)、fMask=MIIM_STATE，
        // fState 按 physicsMode==2 置 MFS_GRAYED|MFS_DISABLED，按位置 2 寻址。
        MENUITEMINFOA mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_STATE;
        unsigned char* curModel = app->SelectedModel();
        mii.fState = mdl::Mdl(curModel)->physicsMode == 2
                         ? (MFS_GRAYED | MFS_DISABLED)
                         : 0;
        SetModelExtensionMenuEnabled(app, mii.fState == MFS_ENABLED);
        DrawMenuBar(hwnd);

        EnableWindow(GetDlgItem(hwnd, panel::kPasteButton), 0);
        if (app->ClipboardCounts().morphs != 0 ||
            app->ClipboardCounts().displays != 0) {
            EnableWindow(GetDlgItem(hwnd, panel::kPasteButton), 1);
            if (app->ClipboardCounts().bones != 0) {
                EnableWindow(GetDlgItem(hwnd, panel::kReversePasteButton), 1);
                PostLanguageSweep2(app);                      // 0x40D070
                return;
            }
        } else if (app->ClipboardCounts().bones != 0) {
            EnableWindow(GetDlgItem(hwnd, panel::kPasteButton), 1);
            EnableWindow(GetDlgItem(hwnd, panel::kReversePasteButton), 1);
            PostLanguageSweep2(app);                          // 0x40D070
            return;
        }
        EnableWindow(GetDlgItem(hwnd, panel::kReversePasteButton), 0);
        // A programmatic CB_SETCURSEL does not notify the regular model
        // selection path. Run it once after registration so the first loaded
        // model receives the same post-load and panel setup as a user-selected
        // model, then paint the refreshed controls before returning to the UI.
        ApplyModelComboSelection(app);
        RedrawWindow(hwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    } else {
        void* oldModel = app->ModelSlot(slot);
        if (oldModel != nullptr) {
            ModelDispose(static_cast<unsigned char*>(oldModel));    // 0x48F830
            free(oldModel);
            app->ModelSlot(slot) = nullptr;
        }
    }
}

}  // namespace mikumikustudio
