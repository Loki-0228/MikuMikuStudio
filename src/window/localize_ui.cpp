// ===========================================================================
// VA 0x00441AD0 - LocalizeUI  (original: sub_441AD0, 0x13D3 bytes)
// ===========================================================================
// UI language pass + selector population, called from 0x0047A5B0 right
// after CreateWindowExA (controls were created by 0x00466D20 during
// WM_CREATE).  NOT the D3D init - reclassified from the earlier stub name.
//
//   1. combobox 433 (bone-mode): reset + add entries
//      "x/y/z axis move", "rotation" (+ "distance","view angle" in model
//      mode, this+760) + "all"; select index 3.
//   2. per-control texts for ids 400..567 (English literals below; the JP
//      branch re-sets the same strings the controls were created with -
//      restored from ui_controls.inc by id, single source of truth).
//   3. model-slot language flag sweep: for each of the 100 slots at
//      this+1920, *(slot+12740) = english; subobject(this+204)+596 too.
//   4. InvalidateRect + sub_42F1E0 + sub_40D070 [stubs]
//   5. camera-mode branch (this+760): combobox 434 entries
//      gravity/s shadow/light/camera (EN) else sub_49C850(slot).
//   6. selector comboboxes 436/474/449: save cur selection, reset, add
//      "camera/light/accessory" / "ground" / JP/EN third label, add every
//      loaded model's name (slot+8826 EN / slot+8776 JP), restore selection.
// =========================================================================//
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/build_profile.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <string>

#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/bone_combo.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "ui_controls.inc"

namespace mikumikustudio {
namespace {

struct TextEntry { int id; const char* en; const wchar_t* zh; };

// English texts in original call order (0x441AD0 EN branch).  The zh column
// is the port's Simplified-Chinese translation, cross-checked against both
// the EN literal and the JP creation string in ui_controls.inc.
constexpr TextEntry kEnTexts[] = {
    {400, "Undo", L"\u64a4\u6d88"}, {401, "Redo", L"\u91cd\u505a"},
    {418, "<", L"<"}, {419, ">", L">"}, {429, "Current", L"\u5f53\u524d"},
    {416, "V-sel", L"纵选择"}, {423, "Delete", L"\u5220\u9664"},
    {420, "Copy", L"\u590d\u5236"}, {421, "Paste", L"\u7c98\u8d34"},
    {422, "Revers", L"\u53cd\u8f6c"}, {415, "Range-sel", L"\u8303\u56f4\u9009\u62e9"},
    {424, "Expand", L"大小缩放"}, {432, "Liner", L"\u7ebf\u6027"},
    {431, "Paste", L"\u7c98\u8d34"}, {430, "Copy", L"\u590d\u5236"},
    {435, "Load", L"\u8bfb\u53d6"}, {437, "Delete", L"\u5220\u9664"},
    {440, "Shadow", L"\u9634\u5f71"}, {441, "Add-syn", L"\u52a0\u7b97"},
    {442, "OP", L"\u5916\u4eb2"}, {438, "Register", L"\u767b\u8bb0"},
    {451, "Reset", L"\u91cd\u7f6e"}, {446, "Perspect", L"\u900f\u89c6"},
    {452, "Register", L"\u767b\u8bb0"}, {454, "+", L"+"},
    {453, "-", L"-"}, {467, "Reset", L"\u91cd\u7f6e"}, {468, "Register", L"\u767b\u8bb0"},
    {562, "Off", L"\u5173\u95ed"}, {563, "Mode1", L"\u6a21\u5f0f1"},
    {564, "Mode2", L"\u6a21\u5f0f2"}, {565, "Register", L"\u767b\u8bb0"},
    {470, "+", L"+"}, {469, "-", L"-"}, {489, "+", L"+"}, {488, "-", L"-"},
    {472, "Load", L"\u8bfb\u53d6"}, {473, "Delete", L"\u5220\u9664"},
    {477, "Add-syn", L"\u52a0\u7b97"}, {487, "Register", L"\u767b\u8bb0"},
    {402, "Front", L"\u6b63\u9762"}, {403, "Back", L"\u80cc\u9762"},
    {404, "Top", L"\u4e0a\u9762"}, {405, "Left", L"\u5de6\u9762"},
    {406, "Right", L"\u53f3\u9762"}, {407, "Btm", L"\u4e0b\u9762"},
    {408, "Play", L"\u64ad\u653e"}, {535, "Track", L"\u8ffd\u8e2a"},
    {412, "Model", L"\u6a21\u578b"}, {531, "Bone", L"\u9aa8\u9abc"},
    {490, "Select", L"\u9009\u62e9"}, {493, "Rotate", L"\u65cb\u8f6c"},
    {492, "Move", L"\u79fb\u52a8"},
    {491, MMD_ORIGINAL_PROFILE ? "Box-sel" : "Scale", MMD_ORIGINAL_PROFILE ? L"框选" : L"\u7f29\u653e"},
    {494, "Select all", L"\u5168\u9009"}, {501, "Unregisted", L"\u672a\u767b\u8bb0"},
    {496, "Copy", L"\u590d\u5236"}, {497, "Paste", L"\u7c98\u8d34"},
    {498, "Revers", L"\u53cd\u8f6c"}, {500, "Register", L"\u767b\u8bb0"},
    {499, "Physics", L"\u7269\u7406"}, {495, "Reset", L"\u91cd\u7f6e"},
    {503, "+", L"+"}, {502, "-", L"-"}, {529, "+", L"+"}, {528, "-", L"-"},
    {567, "+", L"+"}, {566, "-", L"-"},
    {524, "Register", L"\u767b\u8bb0"}, {525, "Register", L"\u767b\u8bb0"},
    {527, "Register", L"\u767b\u8bb0"}, {526, "Register", L"\u767b\u8bb0"},
    {543, "Dist", L"\u8ddd\u79bb"}, {570, "Scale", L"\u7f29\u653e"},
    {551, "Info", L"\u4fe1\u606f"},
    {552, "Low pow", L"\u7701\u6027\u80fd"}, {557, "Axis", L"\u5750\u6807\u8f74"},
    {555, "Set", L"\u8bbe\u7f6e"}, {556, "Fshadow", L"\u7f8e\u5f71"},
};

HWND Dlg(MMDApp* app, int id) {
    return GetDlgItem(static_cast<HWND>(app->Hwnd()), id);
}

// ---- Japanese captions the generated creation table cannot supply ----------
// Three main-window captions are unusable in ui_controls.inc (the table
// generated from the original's control-creation function):
//
//   494 / 501 - bone-operation panel "select all" / "select unregistered".
//     The original passes both captions through an indirect CreateWindowExW
//     argument the generator could not resolve, so the table carries L""; the
//     first fix fell back to the *English* captions, which left two English
//     labels on the Japanese panel (field report: the panel still has buttons
//     without their text).  The originals are recovered from two independent
//     sources:
//       * the CHS 9.26 reference image kept in this workspace
//         ("MikuMikudance 926 CHS\MikuMikudance.exe") carries the UTF-16
//         creation-caption block, and its translated twins sit in creation
//         order right after 框　選 (= BOX選択, control 491):
//           file 0x12A9D8  選中全部   (= 494, select all)
//           file 0x12A9E8  選未註冊   (= 501, select unregistered)
//         four full-width glyphs each - exactly the width the 60x24 buttons
//         were laid out for (a fifth glyph would overrun them), and proof that
//         the captions exist in every MMD build;
//       * MMD's Japanese documentation names the same two buttons
//         「全て選択」and「未登録選」 (four glyphs each), matching the English
//         "select all" / "unregisted" pair already in kEnTexts.
//
//   562 - self-shadow mode "off".  The table captured it as the ANSI byte
//     string "q_j0W0", the known byte-shifted re-read of a UTF-16 literal:
//     its "j0W0" tail are the bytes of L"なし" (the identical artifact
//     physics_model_dialog.cpp documents for the "none" combo entry).  The
//     original caption is なし, the JP counterpart of the EN "off" caption.
//
//   498 - bone reverse paste.  The table carries 反転ﾏﾈｰｼﾞｬ for this control:
//     two full-width kanji followed by half-width katakana, the same class of
//     byte-shifted re-read as 562, and it measures the full 60 pixels of the
//     bone panel's cell in the Japanese font, so it touches both edges.  The
//     control's own English ("revers") and Chinese (反转) captions name the
//     action; 反転 does the same in the three-glyph budget the row uses
//     (コピー 36 px, ペースト 48 px, 反転 24 px).  Measured by
//     tests/bone_scale_gizmo_tests.cpp.
//
// The table stores UTF-16 because all three controls are Unicode windows; no
// Shift-JIS byte string is invented.
//
//   491 - MMS repurposes this control from the box-select radio into the bone
//     scale mode button of the model panel, so its Japanese caption is 拡大縮小
//     (the wording the frame-range scale dialog already uses) instead of the
//     creation table's BOX選択.  Box selection is Shift + drag in select mode.
//
//   412 / 531 - MMS gives the view panel's model / bone reference checkboxes
//     their own captions; the captions are the labels wm_paint used to draw
//     beside the boxes (0x52EDE0 モデル / 0x52EDD8 ボーン).
struct JpCaptionFix { int id; const wchar_t* jp; };
constexpr JpCaptionFix kJpCaptionFixes[] = {
    {494, L"\u5168\u3066\u9078\u629e"},  // 全て選択  select all bones
    {501, L"\u672a\u767b\u9332\u9078"},  // 未登録選  select unregistered bones
    {562, L"\u306a\u3057"},              // なし      self-shadow mode off
    {498, L"\u53cd\u8ee2"},              // 反転      bone reverse paste (MMS)
    {491, MMD_ORIGINAL_PROFILE ? L"BOX選択" : L"\u62e1\u5927\u7e2e\u5c0f"},
    {570, L"\u30b9\u30b1\u30fc\u30eb"},  // スケール  bottom readout
    {412, L"\u30e2\u30c7\u30eb"},        // モデル    view reference: model (MMS)
    {531, L"\u30dc\u30fc\u30f3"},        // ボーン    view reference: bone (MMS)
};


// ASCII -> wide into one scratch buffer (valid until the next call), matching
// the original's SetWindowTextW re-set of the ASCII creation strings.
const wchar_t* WidenAscii(const char* text) {
    static wchar_t buf[64];
    int i = 0;
    for (; text != nullptr && i < 63 && text[i] != '\0'; ++i)
        buf[i] = static_cast<unsigned char>(text[i]);
    buf[i] = L'\0';
    return buf;
}

const char* EnControlText(int id) {
    for (const TextEntry& t : kEnTexts)
        if (t.id == id)
            return t.en;
    return "";
}

const wchar_t* ZhControlText(int id) {
    for (const TextEntry& t : kEnTexts)
        if (t.id == id)
            return t.zh;
    return L"";
}

const wchar_t* JpControlText(int id) {
    // Recovered originals first (see kJpCaptionFixes).
    for (const JpCaptionFix& fix : kJpCaptionFixes)
        if (fix.id == id)
            return fix.jp;
    // Then the JP creation caption of ui_controls.inc: single source of truth
    // for every control the generator could resolve (wide-variant captions
    // come back verbatim, ANSI-variant captions are ASCII and widened in
    // place, which matches the original's SetWindowTextW re-set of the same
    // strings).
    for (const ui::ControlSpec& c : ui::kControls) {
        if (c.id != id)
            continue;
        if (c.wtext != nullptr && c.wtext[0] != L'\0')
            return c.wtext;
        // An empty creation caption (or an ANSI variant without one) must fall
        // through to the English caption below: re-setting the empty string is
        // exactly what left buttons blank in the Japanese UI.  The old code
        // returned its empty buffer for any non-null atext, including "".
        if (c.atext != nullptr && c.atext[0] != '\0')
            return WidenAscii(c.atext);
        break;
    }
    // No Japanese caption exists in the image for this id: fall back to the
    // English caption rather than clearing the control (never invent SJIS).
    return WidenAscii(EnControlText(id));
}

}  // namespace

// Caption written to a main-window control for `language`.  Never null; empty
// only for ids that carry no caption at all (icon-only toggles, edits,
// trackbars, and the mode-dependent panel toggle 536).
std::wstring UiControlCaption(int id, unsigned char language) {
    if (language == kUiEnglish)
        return WidenAscii(EnControlText(id));
    if (language == kUiChinese)
        return ZhControlText(id);
    return JpControlText(id);
}

// Number of main-window controls whose caption LocalizeUI rewrites in every
// language (the kEnTexts table).  Exposed so the regression test can assert
// that none of them can end up with an empty caption in any language.
int UiLocalizedControlCount() {
    return static_cast<int>(sizeof(kEnTexts) / sizeof(kEnTexts[0]));
}

int UiLocalizedControlId(int index) {
    if (index < 0 || index >= UiLocalizedControlCount())
        return -1;
    return kEnTexts[index].id;
}

// MMS addition (no original VA).  Caption of the select button 490.  Box
// selection is Shift + drag inside select mode, so the button keeps its own
// caption in every mode.
std::wstring BoneModePanelCaption(ViewportEditMode mode, unsigned char language) {
    (void)mode;
    return UiControlCaption(panel::kBoneSelectRadio, language);
}

// MMS addition (no original VA).  The select button toggles select <-> none.
// A loaded box-select state counts as select.  Any other mode (rotate, move,
// scale, the transient tool drag) is left by entering plain select, which is
// what the original's select radio did when it was unchecked.
ViewportEditMode NextBoneSelectMode(ViewportEditMode current) {
    if (current == ViewportEditMode::Bone ||
        (!MMD_ORIGINAL_PROFILE && current == ViewportEditMode::BoneBox))
        return ViewportEditMode::None;
    return ViewportEditMode::Bone;
}

// MMS addition (no original VA).  Sole writer of the model panel's bone-mode
// buttons.  MMDApp::EditMode() is the state: the merged button 490 is latched
// while select or box-select is active and shows that state's caption, 491 is
// the bone scale mode, 492 move (Light) and 493 rotate (Camera).  Every mode
// writer - the command handlers, the letter ladder, the model reload and the
// PMM loaders - calls this after storing the mode, so the check states and the
// merged caption can never disagree with it.
void RefreshBoneModePanel(MMDApp* app) {
    if (app == nullptr)
        return;
    HWND main = static_cast<HWND>(app->Hwnd());
    if (main == nullptr)
        return;
    const ViewportEditMode mode = app->EditMode();
    const bool selectMode = mode == ViewportEditMode::Bone ||
                            (!MMD_ORIGINAL_PROFILE && mode == ViewportEditMode::BoneBox);
    SendMessageA(GetDlgItem(main, panel::kBoneSelectRadio), BM_SETCHECK,
                 selectMode ? 1 : 0, 0);
    SendMessageA(GetDlgItem(main, panel::kBoneMoveRadio), BM_SETCHECK,
                 mode == ViewportEditMode::Light ? 1 : 0, 0);
    SendMessageA(GetDlgItem(main, panel::kBoneRotateRadio), BM_SETCHECK,
                 mode == ViewportEditMode::Camera ? 1 : 0, 0);
    SendMessageA(GetDlgItem(main, panel::kBoxSelectRadio), BM_SETCHECK,
                 mode == (MMD_ORIGINAL_PROFILE ? ViewportEditMode::BoneBox : ViewportEditMode::Scale) ? 1 : 0, 0);
    const std::wstring caption =
        BoneModePanelCaption(mode, app->EnglishUI());
    SetWindowTextW(GetDlgItem(main, panel::kBoneSelectRadio), caption.c_str());
}

// MMS addition: the interpolation-channel selector (control 433).  The model
// panel lists the four bone interpolation channels plus the bone scale channel
// this module adds; the camera/light/accessory panel lists its six channels.
// Both end with the same "all" entry.  Sending the layout from one place keeps
// the channel indices the panel, the drag handler and the copy/paste commands
// use consistent, and EnsureInterpChannelCombo repairs the list when another
// code path truncated it.
namespace {
constexpr int kBoneChannelCount = MMD_ORIGINAL_PROFILE ? 4 : 5;
constexpr int kCameraChannelCount = 6;  // X, Y, Z, rotation, distance, angle

const wchar_t* JpChannelCaption(bool cameraPanel, int channel) {
    static const wchar_t* const kBone[5] = {
        L"\xff38\x79fb\x52d5",             // Ｘ移動 0x52D3D4
        L"\xff39\x79fb\x52d5",             // Ｙ移動 0x52D3CC
        L"\xff3a\x79fb\x52d5",             // Ｚ移動 0x52D3C4
        L"\x56de\x3000\x8ee2",             // 回　転 0x52D470
        L"\x62e1\x5927\x7e2e\x5c0f"};      // 拡大縮小 (bone scale, MMS)
    static const wchar_t* const kCamera[2] = {
        L"\x8ddd\x3000\x96e2",             // 距　離 0x52D3BC
        L"\x8996\x91ce\x89d2"};            // 視野角 0x52D3B4
    if (channel >= 4 && cameraPanel)
        return kCamera[channel - 4];
    return channel >= 0 && channel < 5 ? kBone[channel] : L"";
}

const wchar_t* ZhChannelCaption(bool cameraPanel, int channel) {
    static const wchar_t* const kBone[5] = {
        L"X\u8f74\u79fb\u52a8", L"Y\u8f74\u79fb\u52a8",
        L"Z\u8f74\u79fb\u52a8", L"\u65cb\u8f6c",
        L"\u7f29\u653e"};                  // 缩放 (bone scale, MMS)
    static const wchar_t* const kCamera[2] = {
        L"\u8ddd\u79bb", L"\u89c6\u91ce\u89d2"};
    if (channel >= 4 && cameraPanel)
        return kCamera[channel - 4];
    return channel >= 0 && channel < 5 ? kBone[channel] : L"";
}

const char* EnChannelCaption(bool cameraPanel, int channel) {
    static const char* const kBone[5] = {
        "x axis move", "y axis move", "z axis move", "rotation", "scale"};
    static const char* const kCamera[2] = {"distance", "view angle"};
    if (channel >= 4 && cameraPanel)
        return kCamera[channel - 4];
    return channel >= 0 && channel < 5 ? kBone[channel] : "";
}
}  // namespace

// MMS addition (no original VA): one interpolation-channel caption for the
// language the UI is in.  `cameraPanel` selects the camera layout, which keeps
// distance and view angle in the two slots after rotation.
std::wstring InterpChannelCaption(bool cameraPanel, int channel,
                                  unsigned char language) {
    if (language == kUiChinese)
        return ZhChannelCaption(cameraPanel, channel);
    if (language == kUiJapanese)
        return JpChannelCaption(cameraPanel, channel);
    return WidenAscii(EnChannelCaption(cameraPanel, channel));
}

std::wstring InterpAllChannelsCaption(unsigned char language) {
    if (language == kUiChinese)
        return L"\u5168\u90e8";
    if (language == kUiJapanese)
        return L"\x3059\x3079\x3066";
    return L"all";
}

void EnsureInterpChannelCombo(MMDApp* app, bool force) {
    if (app == nullptr)
        return;
    HWND combo = Dlg(app, panel::kInterpCurveCombo);
    if (combo == nullptr)
        return;
    const bool cameraPanel = app->state.optflag[0] != 0;
    const int channels = cameraPanel ? kCameraChannelCount : kBoneChannelCount;
    const int expected = channels + 1;  // the trailing "all" entry
    if (!force &&
        static_cast<int>(SendMessageA(combo, CB_GETCOUNT, 0, 0)) == expected)
        return;
    LRESULT cursor = SendMessageA(combo, CB_GETCURSEL, 0, 0);
    if (cursor < 0 || cursor >= expected)
        cursor = 3;  // the original default: rotation
    const unsigned char language = app->EnglishUI();
    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    for (int channel = 0; channel < channels; ++channel) {
        const std::wstring caption =
            InterpChannelCaption(cameraPanel, channel, language);
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(caption.c_str()));
    }
    const std::wstring all = InterpAllChannelsCaption(language);
    SendMessageW(combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(all.c_str()));
    SendMessageA(combo, CB_SETCURSEL, static_cast<WPARAM>(cursor), 0);
}

// Refill model selectors without retaining an invalid initial selection.
// Shared with hidden control tests; model names remain display-only.
void RefreshModelSelectorNames(MMDApp* app) {
    auto& s = *app;
    const bool english = s.EnglishUI() == kUiEnglish;
    const bool chinese = s.EnglishUI() == kUiChinese;
    const bool englishNames = UiUsesEnglishNames(s.EnglishUI());
    // selector comboboxes 436/474/449: preserve selection across refill
    HWND combo436 = Dlg(app, 436), combo474 = Dlg(app, 474), combo449 = Dlg(app, 449);
    LRESULT sel436 = SendMessageA(combo436, CB_GETCURSEL, 0, 0);
    LRESULT sel474 = SendMessageA(combo474, CB_GETCURSEL, 0, 0);
    LRESULT sel449 = SendMessageA(combo449, CB_GETCURSEL, 0, 0);
    SendMessageA(combo436, CB_RESETCONTENT, 0, 0);
    SendMessageA(combo474, CB_RESETCONTENT, 0, 0);
    SendMessageA(combo449, CB_RESETCONTENT, 0, 0);
    if (english) {
        SendMessageA(combo436, CB_ADDSTRING, 0, (LPARAM)"camera/light/accessory");  // 0x52D2AC
        SendMessageA(combo474, CB_ADDSTRING, 0, (LPARAM)"ground");                 // 0x52D2A4
        SendMessageA(combo449, CB_ADDSTRING, 0, (LPARAM)"non");                    // 0x52D38C
    } else if (chinese) {
        SendMessageW(combo436, CB_ADDSTRING, 0, (LPARAM)L"\u76f8\u673a/\u7167\u660e/\u914d\u4ef6");
        SendMessageW(combo474, CB_ADDSTRING, 0, (LPARAM)L"\u5730\u9762");
        SendMessageW(combo449, CB_ADDSTRING, 0, (LPARAM)L"\u65e0");
    } else {
        // Verified byte-exact: 0x52D370 ｶﾒﾗ･照明･ｱｸｾｻﾘ / 0x52D368 地面 /
        // 0x52D360 なし (push sequence 0x442CE0..0x442D31).
        SendMessageW(combo436, CB_ADDSTRING, 0,
                     (LPARAM)L"\xff76\xff92\xff97\xff65\x7167\x660e"
                              L"\xff65\xff71\xff78\xff7e\xff7b\xff98");  // 0x52D370
        SendMessageW(combo474, CB_ADDSTRING, 0,
                     (LPARAM)L"\x5730\x9762");                           // 0x52D368
        SendMessageW(combo449, CB_ADDSTRING, 0,
                     (LPARAM)L"\x306a\x3057");                           // 0x52D360
    }

    // add every loaded model's name to all three selectors; x64 twin
    // sub_7FF7CB438E70 runs the comboSelIndex (model+0x3108) slot scan and
    // the order walk to 255 (cmp edx, 0FFh at 0x7FF7CB43A96C / cmp ebp,
    // 0FFh at 0x7FF7CB43AA8D) - same family as model_edge_dialog.cpp.
    int rowBySlot[kModelSlotCount]{};
    for (int order = 1; order <= kModelSlotCount; ++order) {
        int slotIdx = -1;
        for (int i = 0; i < kModelSlotCount; ++i) {
            unsigned char* slot = s.ModelSlot(i);
            if (slot != nullptr &&
                mikumikustudio::mdl::Mdl(slot)->comboSelIndex == order) {
                slotIdx = i;
                break;
            }
        }
        if (slotIdx < 0)
            continue;
        unsigned char* slot = s.ModelSlot(slotIdx);
        const auto label = text_encoding::ModelLabel(*mdl::Mdl(slot), englishNames);
        const LPARAM name = reinterpret_cast<LPARAM>(label.c_str());
        rowBySlot[slotIdx] = static_cast<int>(SendMessageW(combo436, CB_ADDSTRING, 0, name));
        SendMessageW(combo474, CB_ADDSTRING, 0, name);
        SendMessageW(combo449, CB_ADDSTRING, 0, name);
    }

    const int parentSlot = s.CameraParentModel();
    FillBoneCombo(Dlg(app, 450), parentSlot >= 0 && parentSlot < kModelSlotCount
        ? mdl::Mdl(s.ModelSlot(parentSlot)) : nullptr, englishNames);
    SelectBoneCombo(Dlg(app, 450), s.CameraParentBone());
    if (const auto* acc = s.AccessorySlot(s.SelectedAccessorySlot())) {
        const int slot = acc->parentModel;
        FillBoneCombo(Dlg(app, 475), slot >= 0 && slot < kModelSlotCount
            ? mdl::Mdl(s.ModelSlot(slot)) : nullptr, englishNames);
        SelectBoneCombo(Dlg(app, 475), acc->parentBone);
    }

    const auto modelRow = [&](int slot) {
        return slot >= 0 && slot < kModelSlotCount ? rowBySlot[slot] : 0;
    };
    const auto restore = [](HWND combo, LRESULT previous, int fallback) {
        if (!combo) return -1;
        const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
        LRESULT selected = previous;
        if (selected < 0 || selected >= count) selected = fallback;
        if (selected < 0 || selected >= count) selected = count > 0 ? 0 : -1;
        SendMessageW(combo, CB_SETCURSEL, selected, 0);
        // The collapsed caption must repaint even before the first user click.
        InvalidateRect(combo, nullptr, FALSE);
        return static_cast<int>(selected);
    };
    s.state.mainModelComboSelection = restore(combo436, sel436,
        s.CameraMode() == 0 ? modelRow(s.SelectedModelSlot()) : 0);
    const auto* accessory = s.AccessorySlot(s.SelectedAccessorySlot());
    restore(combo474, sel474, accessory ? modelRow(accessory->parentModel) : 0);
    restore(combo449, sel449, modelRow(s.CameraParentModel()));
}

void LocalizeUI(MMDApp* app) {
    RefreshMenuLanguage(app);
    auto& s = *app;
    // Name language follows the shared rule; the CHS pass is not "English".
    const bool english = s.EnglishUI() == kUiEnglish;              // UI captions follow UI language
    const bool chinese = s.EnglishUI() == kUiChinese;
#if !MMD_ORIGINAL_PROFILE
    SetWindowTextW(Dlg(app, panel::kForcePasteButton), chinese ? L"强制粘贴" :
        english ? L"Force paste" : L"強制貼付");
    SetWindowTextW(Dlg(app, panel::kAdaptiveShadowCheckbox), chinese ? L"动态精度" :
        english ? L"Dynamic" : L"動的精度");
    SetWindowTextW(Dlg(app, panel::kShadowFilterCheckbox), chinese ? L"滤波加强" :
        english ? L"Smoothing" : L"平滑化");
#endif
    HFONT previousFont = s.state.hFontUI;
    CreateUiFont(app, app->MainWindow());
    struct FontRefresh { MMDApp* owner; HFONT previous; } fonts{app,previousFont};
    EnumChildWindows(app->MainWindow(), [](HWND child, LPARAM context) -> BOOL {
        auto& refresh = *reinterpret_cast<FontRefresh*>(context);
        // A dialog or editor owns its own point size. Update only main-panel
        // controls that actually borrowed the prior shared font.
        if(GetParent(child)==refresh.owner->MainWindow() &&
            reinterpret_cast<HFONT>(SendMessageW(child,WM_GETFONT,0,0))==refresh.previous)
            ApplyUiFontToControl(refresh.owner,child,GetDlgCtrlID(child));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&fonts));
    const bool modelMode = s.state.optflag[0] != 0;  // 760

    HWND combo433 = Dlg(app, 433);
    SendMessageA(combo433, CB_RESETCONTENT, 0, 0);

    // The channel-selector entries come from the shared layout writer; the
    // per-language control captions stay in their branches below.
    EnsureInterpChannelCombo(app, true);

    if (english) {
        for (const TextEntry& t : kEnTexts)
            SetWindowTextA(Dlg(app, t.id), t.en);
        SetWindowTextA(Dlg(app, 536), modelMode ? "To model" : "Camera");
    } else if (chinese) {
        for (const TextEntry& t : kEnTexts)
            SetWindowTextW(Dlg(app, t.id), t.zh);
        SetWindowTextW(Dlg(app, 536), modelMode ? L"\u5230\u6a21\u578b" : L"\u76f8\u673a");
    } else {
        for (const TextEntry& t : kEnTexts)
            SetWindowTextW(Dlg(app, t.id), JpControlText(t.id));
        SetWindowTextW(Dlg(app, 536), JpControlText(536));
    }
    SendMessageA(combo433, CB_SETCURSEL, 3, 0);

    // model-slot language flag sweep (20 x 5 slots at this+1920); the x64
    // twin sub_7FF7CB438E0 sweeps all 255 slots (mov r8d, 0FFh ... dec r8
    // at 0x7FF7CB43A4E0, writing model+0x3560).
    for (int i = 0; i < kModelSlotCount; ++i) {
        unsigned char* slot = s.ModelSlot(i);
        if (slot != nullptr)
            mdl::Mdl(slot)->physicsFlags = static_cast<unsigned char>(english);
    }
    if (s.Audio() != nullptr)
        s.Audio()->englishUI = static_cast<unsigned char>(english);

    InvalidateRect(static_cast<HWND>(s.Hwnd()), nullptr, FALSE);
    // The viewport HUD atlas is painted with this language's font and strings
    // (DrawUiGlyph picks Arial / MS PGothic / the Chinese translation while the
    // texture is built), so the captions - "1fps or less" and the
    // local/global/view selector - must be repainted here. Without it the
    // switch to Japanese or English kept the previous language's glyphs.
    InitSceneFontTexture(app);                                     // 0x42AE80
    PostLanguageSweep(app);                                        // 0x42F1E0
    PostLanguageSweep2(app);                                       // 0x40D070

    if (modelMode) {
        HWND combo434 = Dlg(app, 434);
        SendMessageA(combo434, CB_DELETESTRING, 0, 0);
        SendMessageA(combo434, CB_DELETESTRING, 0, 0);
        SendMessageA(combo434, CB_DELETESTRING, 0, 0);
        if (english) {
            SendMessageA(combo434, CB_INSERTSTRING, 0, (LPARAM)"gravity");
            SendMessageA(combo434, CB_INSERTSTRING, 0, (LPARAM)"s shadow");
            SendMessageA(combo434, CB_INSERTSTRING, 0, (LPARAM)"light");
            SendMessageA(combo434, CB_INSERTSTRING, 0, (LPARAM)"camera");
        } else if (chinese) {
            SendMessageW(combo434, CB_INSERTSTRING, 0, (LPARAM)L"\u91cd\u529b");
            SendMessageW(combo434, CB_INSERTSTRING, 0, (LPARAM)L"\u81ea\u9634\u5f71");
            SendMessageW(combo434, CB_INSERTSTRING, 0, (LPARAM)L"\u7167\u660e");
            SendMessageW(combo434, CB_INSERTSTRING, 0, (LPARAM)L"\u76f8\u673a");
        } else {
            // JP strings verified byte-exact: 重力(0x52D3A4) セルフ影(0x52D398)
            // 照明(0x52D390) カメラ(0x52B784).
            SendMessageW(combo434, CB_INSERTSTRING, 0,
                         (LPARAM)L"\x91cd\x529b");                 // 0x52D3A4
            SendMessageW(combo434, CB_INSERTSTRING, 0,
                         (LPARAM)L"\x30bb\x30eb\x30d5\x5f71");     // 0x52D398
            SendMessageW(combo434, CB_INSERTSTRING, 0,
                         (LPARAM)L"\x7167\x660e");                 // 0x52D390
            SendMessageW(combo434, CB_INSERTSTRING, 0,
                         (LPARAM)L"\x30ab\x30e1\x30e9");           // 0x52B784
        }
        SendMessageA(combo434, CB_SETCURSEL, 0, 0);
    } else {
        PostLoadInit(s.SelectedModel());                           // 0x49C850
    }

    RefreshModelSelectorNames(app);
    curve_host::Localize(app);

    // MMS addition: the caption pass above writes the select caption onto the
    // merged button 490; this re-applies the caption of the state the panel is
    // actually in (the box-select state has its own wording) and re-derives the
    // four check states from MMDApp::EditMode().
    RefreshBoneModePanel(app);
}

}  // namespace mikumikustudio
