#include "mikumikustudio/display_capacity.hpp"
// ===========================================================================
// VA 0x0049C850 - PostLoadInit  (original: sub_49C850, 3007 bytes)
// ===========================================================================
// Model selector panel refresh after a PMD/PMX load.  Rebuilds:
//   combo 443  bone list from the IK-chain array (m+9920, 24 B records -
//              record +0 is a bone index into the 604 B bone records;
//              chain[0]+18 picks radio 444/445);
//   BM_SETCHECK on 439 (m+11661 loadComplete), 441 (m+12734),
//              440 (m+14272);
//   four morph-category groups, category byte at morph+88 (morphs at
//   m+9924, 136 B records, count m+11648):
//       cat 1 -> combo 504 / slider 505 / edit 506 / selIdx m+11676
//       cat 2 -> combo 509 / slider 510 / edit 511 / selIdx m+11680
//       cat 3 -> combo 514 / slider 515 / edit 516 / selIdx m+11684
//       cat 4 -> combo 519 / slider 520 / edit 521 / selIdx m+11688
//     Each: rebuild combo from names (+20 English / +0 SJIS), set the
//     slider to (int)(morph[sel]+48 * 100.0) and "%5.4f" into the edit
//     ("0.0000" when sel < 0), then re-select; when sel < 0 the first
//     matching record index is stored back into the selIdx field.
//   combo 434  frame-registration list.  Models whose SJIS name equals
//              "ダミーボーン" (memcmp 13 bytes vs 0x5310A8, x64 0x5523E0)
//              get the fixed list ("All frame"/"disp/IK/OP"/"Sel Bone"/
//              bone01..bone15);
//              normal models get "All frame", center-bone name (index
//              m+14592), "disp/IK/OP", "Sel Bone", "Sel facial",
//              "All facial", facial-display groups (m+9948, 46 B stride,
//              byte count m+11692), "All bone", then disp frames
//              (m+9944, 46 B stride, count m+11696); JP labels are the
//              wide strings 全ﾌﾚｰﾑ/表示･IK･外親/選択ﾎﾞｰﾝ/選択表情/
//              全表情ﾌﾚｰﾑ/全ボーンﾌﾚｰﾑ sent via SendMessageW and the
//              SJIS format "ﾎﾞｰﾝ%02d" (0x531054).  Final
//              CB_SETCURSEL with m+314608.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/frame_scope.hpp"

namespace mikumikustudio {
namespace {

// JP wide labels (.rdata 0x531090/0x53107C/0x53106C/0x531048/0x531038/
// 0x531024) and the SJIS name/format constants (0x5310A8 / 0x531054).
const wchar_t* kJpAllFrame = L"\x5168\xFF8C\xFF9A\xFF70\xFF91";          // 全ﾌﾚｰﾑ
const wchar_t* kJpDispIkOp =
    L"\x8868\x793A\xFF65I\x004B\xFF65\x5916\x89AA";                      // 表示･IK･外親
const wchar_t* kJpSelBone =
    L"\x9078\x629E\xFF8E\xFF9E\xFF70\xFF9D";                             // 選択ﾎﾞｰﾝ
const wchar_t* kJpSelFacial = L"\x9078\x629E\x8868\x60C5";               // 選択表情
const wchar_t* kJpAllFacial =
    L"\x5168\x8868\x60C5\xFF8C\xFF9A\xFF70\xFF91";                       // 全表情ﾌﾚｰﾑ
const wchar_t* kJpAllBone =
    L"\x5168\x30DC\x30FC\x30F3\xFF8C\xFF9A\xFF70\xFF91";                 // 全ボーンﾌﾚｰﾑ
const char kJpBoneFmt[] =
    "\xCE\xDE\xB0\xDD" "%02d";                                           // ﾎﾞｰﾝ%02d
const char kDummyBoneName[13] = {
    char(0x83), char(0x5F), char(0x83), char(0x7E), char(0x81), char(0x5B),
    char(0x83), char(0x7B), char(0x81), char(0x5B), char(0x83), char(0x93),
    '\0'};                                        // ダミーボーン (0x5310A8 =
                                                  // x64 0x5523E0; the SJIS was
                                                  // previously misread as
                                                  // ミクボーン - 0x835F is ダ)

HWND Dlg(unsigned char* m, int id) {
    return GetDlgItem(*reinterpret_cast<HWND*>(m), id);
}

template<std::size_t N>
void AddLegacyName(HWND combo, const char (&text)[N]) {
    std::wstring name;
    text_encoding::Decode(std::string(text, strnlen_s(text, N)).c_str(), 932, name);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
}

// One morph-category group (see header comment).
void RefreshMorphGroup(unsigned char* m, mdl::MorphPanel panel, int comboId,
                       int sliderId, int editId, std::size_t selector) {
    HWND combo = Dlg(m, comboId);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    auto* model = mdl::Mdl(m);
    int first = -1;
    int selectedRow = -1;
    for (std::uint32_t i = 0; model->morphs != nullptr && i < model->morphCount; ++i) {
        const auto& morph = model->morphs[i];
        if (morph.panel != panel) continue;
        const auto name = text_encoding::MorphLabel(
            morph, g_Block != nullptr && UiUsesEnglishNames(g_Block->EnglishUI()));
        const LRESULT row = SendMessageW(combo, CB_ADDSTRING, 0,
                                         reinterpret_cast<LPARAM>(name.c_str()));
        if (row == CB_ERR || row == CB_ERRSPACE) continue;
        SendMessageW(combo, CB_SETITEMDATA, row, i);
        if (first < 0) first = static_cast<int>(i);
        if (model->selectedMorphs[selector] == static_cast<int>(i))
            selectedRow = static_cast<int>(row);
    }
    // Reject stale selections after a model reload before reading their value.
    if (selectedRow < 0) {
        model->selectedMorphs[selector] = first;
        selectedRow = first < 0 ? -1 : 0;
    }
    SendMessageW(combo, CB_SETCURSEL, selectedRow, 0);
    const int selected = model->selectedMorphs[selector];
    const float value = selected >= 0 ? model->morphs[selected].value : 0.0f;
    SendMessageW(Dlg(m, sliderId), TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(value * 100.0f));
    char text[64];
    sprintf_s(text, "%5.4f", value);
    SetWindowTextA(Dlg(m, editId), text);
}

}  // namespace

void PostLoadInit(unsigned char* m) {
    HWND main = *reinterpret_cast<HWND*>(m);

    // ---- bone combo 443 (from IK chains) ----------------------------------
    HWND boneCombo = Dlg(m, 443);
    SendMessage(boneCombo, CB_RESETCONTENT, 0, 0);
    mdl::ModelRecord* model = mdl::Mdl(m);
    mikumikustudio::mdl::BoneRecord* bones = model->boneTable;
    // The combo names follow the English UI only, exactly like the tree panel
    // (ui_panel_sweep.cpp) and the palette.  Reading the per-model flag here
    // was inconsistent twice over: PMX loads never set it, and pmd_load stored
    // the raw language byte, so CHS/JP could show English bones next to
    // Japanese morph names (and vice versa).  Naming is a UI-language
    // decision, so take it from the live UI language.
    const bool useEnglishNames = g_Block != nullptr && UiUsesEnglishNames(g_Block->EnglishUI());
    const int chainCount = static_cast<int>(model->ikChainCount);
    if (chainCount > 0) {
        for (int i = 0; i < chainCount; ++i) {
            const std::int32_t boneIdx = mdl::IkChains(m)[i].boneIndex;
            const auto name = boneIdx >= 0 && static_cast<std::uint32_t>(boneIdx) < model->boneCount
                ? text_encoding::BoneLabel(bones[boneIdx], useEnglishNames) : std::wstring{};
            SendMessageW(boneCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
    }
    if (mdl::IkChains(m) != nullptr) {
        SendMessageA(boneCombo, CB_SETCURSEL, 0, 0);
        CheckRadioButton(main, 444, 445,
                         mdl::IkChains(m)[0].enabled != 0 ? 444 : 445);
    }

    // ---- three checkboxes ---------------------------------------------------
    SendMessage(Dlg(m, 439), BM_SETCHECK,
                model->loadComplete != 0 ? 1 : 0, 0);
    SendMessage(Dlg(m, 441), BM_SETCHECK,
                model->postLoadFlag2 != 0 ? 1 : 0, 0);
    SendMessage(Dlg(m, 440), BM_SETCHECK,
                model->toonFlag != 0 ? 1 : 0, 0);

    // ---- four morph groups --------------------------------------------------
    RefreshMorphGroup(m, mdl::MorphPanel::eyebrow, 504, 505, 506, 0);
    RefreshMorphGroup(m, mdl::MorphPanel::eye, 509, 510, 511, 1);
    RefreshMorphGroup(m, mdl::MorphPanel::mouth, 514, 515, 516, 2);
    RefreshMorphGroup(m, mdl::MorphPanel::other, 519, 520, 521, 3);

    // ---- frame-registration combo 434 ---------------------------------------
    HWND frameCombo = Dlg(m, 434);
    SendMessage(frameCombo, CB_RESETCONTENT, 0, 0);
    const bool chinese = g_Block != nullptr && g_Block->EnglishUI() == 2;
    const auto addScope = [&](FrameScope scope, const wchar_t* english,
                              const wchar_t* japanese, const wchar_t* zh) {
        const auto row = SendMessageW(frameCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(chinese ? zh : useEnglishNames ? english : japanese));
        if (row != CB_ERR && row != CB_ERRSPACE)
            SendMessageW(frameCombo, CB_SETITEMDATA, row, static_cast<LPARAM>(scope));
    };
    char buf[256];
    if (std::memcmp(model->name, kDummyBoneName, 13) == 0) {
        // bundled "ダミーボーン" dummy-bone model: fixed registration list
        addScope(FrameScope::AllFrames, L"All frame", kJpAllFrame, L"全部帧");
        addScope(FrameScope::Display, L"disp/IK/OP", kJpDispIkOp, L"显示·IK·外亲");
        addScope(FrameScope::SelectedBones, L"Sel Bone", kJpSelBone, L"选中骨骼");
        for (int i = 1; i <= 15; ++i) {
            if (useEnglishNames)
                sprintf_s(buf, 0x100, "bone%02d", i);
            else
                sprintf_s(buf, 0x100, kJpBoneFmt, i);
            AddLegacyName(frameCombo, buf);
        }
    } else {
        addScope(FrameScope::AllFrames, L"All frame", kJpAllFrame, L"全部帧");
        if (bones != nullptr && model->displayRootBone >= 0 &&
            static_cast<std::uint32_t>(model->displayRootBone) < model->boneCount) {
            const auto rootName = text_encoding::BoneLabel(bones[model->displayRootBone], useEnglishNames);
            SendMessageW(frameCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(rootName.c_str()));
        }
        addScope(FrameScope::Display, L"disp/IK/OP", kJpDispIkOp, L"显示·IK·外亲");
        addScope(FrameScope::SelectedBones, L"Sel Bone", kJpSelBone, L"选中骨骼");
        addScope(FrameScope::SelectedMorphs, L"Sel facial", kJpSelFacial, L"选中表情");
        addScope(FrameScope::AllMorphs, L"All facial", kJpAllFacial, L"全部表情");
        const auto* groups = model->displayFrames;
        for (std::uint32_t g = 0; g < mikumikustudio::mdl::FacialFrameCount(*model); ++g) {
            if (!model->morphs || mikumikustudio::mdl::FrameTargetIndex(groups[g]) >= model->morphCount) continue;
            const auto name = text_encoding::MorphLabel(model->morphs[mikumikustudio::mdl::FrameTargetIndex(groups[g])], useEnglishNames);
            SendMessageW(frameCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
        addScope(FrameScope::AllBones, L"All bone", kJpAllBone, L"全部骨骼");
        const auto* frames = static_cast<const mdl::FrameGroup*>(model->rbGroups);
        for (std::uint32_t f = 0; f < model->rigidBodyCount; ++f) {
            if (!bones || mikumikustudio::mdl::FrameTargetIndex(frames[f]) >= model->boneCount) continue;
            const auto name = text_encoding::BoneLabel(bones[mikumikustudio::mdl::FrameTargetIndex(frames[f])], useEnglishNames);
            SendMessageW(frameCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
    }
    SendMessageA(frameCombo, CB_SETCURSEL,
                 model->frameRegistrationSelection, 0);
}

}  // namespace mikumikustudio
