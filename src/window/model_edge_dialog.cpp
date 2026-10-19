#include "mikudancestudio/extended_names.hpp"
#include "mikudancestudio/text_encoding.hpp"
#include "mikudancestudio/display_capacity.hpp"
// ===========================================================================
// Model-edge / English-name dialog (menu 259, sub_464BD0 family)
// ===========================================================================
// Split out of src/window/command_view_menu.cpp (the menu-251..302 command
// family).  This is MMD's "input English notation" dialog (template
// 0x29A EN / 0x299 JP, modeless at app slot 0xA0B50): it edits the English
// model name / comment and the English names of the bones, morphs and
// display groups of the active model.
//
// Dialog layout (control ids, template res 666/665):
//   667 0x29B edit  model English name      668 0x29C edit  model EN comment
//   669 0x29D combo bone list               672 0x2A0 edit  bone EN name
//   673 0x2A1 combo morph list              676 0x2A4 edit  morph EN name
//   677 0x2A5 combo display-group list      680 0x2A8 edit  group EN name
//   0x29E/0x29F, 0x2A2/0x2A3, 0x2A6/0x2A7 next/prev buttons of the combos.
//
// The dialog procedure ModelEdgeDlgProc (was Sub464BD0) and the edit
// subclass ModelEdgeEditSubclassProc (was Sub45ECC0) live in
// command_view_menu.cpp; the subclass commits an edit on Enter through the
// collector CollectEnglishNameEdit below.  Every function keeps its original x86 VA.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstring>

#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/ported_funcs.hpp"

namespace mikudancestudio {
namespace {

// ---- dialog controls (template 0x29A EN / 0x299 JP) ----------------------
constexpr int kEditModelNameEn    = 667;  // 0x29B
constexpr int kEditModelCommentEn = 668;  // 0x29C
constexpr int kComboBoneName      = 669;  // 0x29D
constexpr int kEditBoneNameEn     = 672;  // 0x2A0
constexpr int kComboMorphName     = 673;  // 0x2A1
constexpr int kEditMorphNameEn    = 676;  // 0x2A4
constexpr int kComboGroupName     = 677;  // 0x2A5
constexpr int kEditGroupNameEn    = 680;  // 0x2A8

// ---- main-window controls touched while the dialog is up -----------------
constexpr int kMainComboModel  = 436;  // 0x1B4 model combo (disabled on open)
constexpr int kMainComboGround = 474;  // 0x1DA
constexpr int kMainComboNormal = 449;  // 0x1C1
constexpr int kMainRadioFirst  = 490;  // BM_SETCHECK: 490 on, 491..493 off

// Fixed heads of the three main-window combos rebuilt by the collector
// (x64 .rdata 0x54B6B8 / 0x54B768 / 0x551074).
const char kHeadModelCombo[] = "camera/light/accessory";
const char kHeadGroundCombo[] = "ground";
const char kHeadNormalCombo[] = "normal";

// The morph combo lists morphs[1..]; the group combo lists display groups
// starting after the fixed "Root"/"表情" slots - slot 3 when the model owns
// facial frame groups, slot 2 otherwise.
int MorphNameSlot(MMDApp* app) {
    return app->ModelEdgeComboCursor(1) + 1;
}

int GroupNameSlot(MMDApp* app, mdl::ModelRecord* model) {
    return app->ModelEdgeComboCursor(2) +
           (mikudancestudio::mdl::FacialFrameCount(*model) != 0 ? 3 : 2);
}

// Mirror the English names into the facial display-frame rows (each row's
// targetIndex addresses the morph table).
void MirrorFacialFrameNames(mdl::ModelRecord* model) {
    mdl::FrameGroup* frames = model->displayFrames;
    mdl::MorphRecord* morphs = model->morphs;
    for (std::uint32_t i = 0; i < mikudancestudio::mdl::FacialFrameCount(*model); ++i) {
        if (mikudancestudio::mdl::FrameTargetIndex(frames[i]) < model->morphCount)
            strcpy_s(frames[i].nameEn, 0x14,
                     morphs[mikudancestudio::mdl::FrameTargetIndex(frames[i])].nameEn);
    }
}

// Same mirror for the bone display-frame rows (the 46-byte table behind
// model->rbGroups; targetIndex addresses the bone table).  The row count is
// the dword also used by the loaders (model->rigidBodyCount).
void MirrorBoneFrameNames(mdl::ModelRecord* model) {
    mdl::FrameGroup* frames =
        static_cast<mdl::FrameGroup*>(model->rbGroups);
    mdl::BoneRecord* bones = model->boneTable;
    const std::uint32_t count = model->rigidBodyCount;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (mikudancestudio::mdl::FrameTargetIndex(frames[i]) < model->boneCount)
            strcpy_s(frames[i].nameEn, 0x14,
                     bones[mikudancestudio::mdl::FrameTargetIndex(frames[i])].nameEn);
    }
}

// Select-all + replace + select-all + focus: the edit refresh shared by the
// three combo helpers (EM_SETSEL(0, len), EM_REPLACESEL, EM_SETSEL, focus).
void RefreshNameEdit(HWND edit, const wchar_t* text) {
    SendMessageW(edit, EM_SETSEL, 0, GetWindowTextLengthW(edit));
    SendMessageW(edit, EM_REPLACESEL, 0, reinterpret_cast<LPARAM>(text));
    SendMessageW(edit, EM_SETSEL, 0, GetWindowTextLengthW(edit));
    SetFocus(edit);
}

}  // namespace

// ===========================================================================
// 0x0043BED0 - English-name collector (original: sub_43BED0)
// ===========================================================================
// Commits one English-name edit into the model.  idx picks the target
// (matching the subclassed edits 0x29B/0x2A0/0x2A4/0x2A8):
//   0: model English name (nameEn, 20 bytes).  In English UI mode the three
//      main-window combos 436/474/449 are also rebuilt from the model slots
//      in combo order (the 1-based order byte comboSelIndex, orders 1..254,
//      first matching slot per order), restoring their old cursors.
//   1: bone English name of combo cursor 0; the bone display-frame rows
//      mirror the new name from their target bone.
//   2: morph English name (combo cursor 1 -> morph index cursor+1), only
//      when the model has facial frame groups; the facial rows mirror, then
//      PostLanguageSweep + PostLoadInit refresh the panel.
//   3: display-group English name (combo cursor 2 -> group slot cursor+3 /
//      cursor+2), capped at the 50-byte group record.
// Edit texts are capped at the record widths before the copy (19 chars for
// the 20-byte names, 49 for the 50-byte group name).
//
// NOTE: command_view_menu.cpp carries only a forward declaration of this
// function for its subclass call sites; this body is the real port used by
// the dialog helpers in this file.
// =========================================================================//
void CollectEnglishNameEdit(MMDApp* app, HWND hEdit, int idx) {  // was Sub43BED0, VA 0x0043BED0
    auto* raw=app->SelectedModel();if(!raw)return;
    auto& model=*mdl::Mdl(raw);
    try {
        const int length=GetWindowTextLengthW(hEdit);
        std::wstring value(static_cast<std::size_t>(length)+1,L'\0');
        const int read=GetWindowTextW(hEdit,value.data(),static_cast<int>(value.size()));value.resize(read);
        wchar_t** wide=nullptr;char* mirror=nullptr;std::size_t capacity=0;
        if(idx==0){wide=&model.pmxTextBuffers[1];mirror=model.nameEn;capacity=sizeof model.nameEn;}
        else if(idx==1){const int i=app->ModelEdgeComboCursor(0);if(i<0||std::uint32_t(i)>=model.boneCount)return;
            wide=&model.boneTable[i].enText;mirror=model.boneTable[i].nameEn;capacity=sizeof model.boneTable[i].nameEn;}
        else if(idx==2){const int i=MorphNameSlot(app);if(i<0||std::uint32_t(i)>=model.morphCount)return;
            wide=&model.morphs[i].enText;mirror=model.morphs[i].nameEn;capacity=sizeof model.morphs[i].nameEn;}
        else if(idx==3){const int i=GroupNameSlot(app,&model);if(i<0||std::uint32_t(i)>=mdl::DisplayGroupCount(model))return;
            auto& group=mdl::DisplayGroups(raw)[i];const auto native=text_encoding::DisplayGroupName(group,false);
            text_encoding::SetDisplayGroupName(model,group,native.c_str(),value.c_str());mirror=group.nameEn;capacity=sizeof group.nameEn;}
        else return;
        if(wide&&!extended_names::Assign(*wide,value))throw std::bad_alloc();
        // Fixed fields remain plugin mirrors. The complete name drives the UI
        // and project persistence even when this short CP932 mirror cannot fit.
        std::string bytes;
        if(text_encoding::EncodeExact(value.c_str(),932,bytes)&&bytes.size()<capacity){
            std::memset(mirror,0,capacity);std::memcpy(mirror,bytes.data(),bytes.size());
        }
        app->SceneModified()=1;
        MirrorBoneFrameNames(&model);MirrorFacialFrameNames(&model);
        PostLanguageSweep(app);PostLoadInit(raw);
    }catch(const std::exception&){
        const wchar_t* messages[]={L"名前を保存するメモリが不足しています。変更を適用できません。",L"Not enough memory to store the name. The edit could not be applied.",L"内存不足，无法保存该名称，未应用此次修改。"};
        MessageBoxW(hEdit,messages[app->EnglishUI()<=2?app->EnglishUI():1],L"MMS",MB_OK|MB_ICONERROR);
    }
}

// ===========================================================================
// 0x0043C430 - model-edge dialog init (original: sub_43C430)
// ===========================================================================
// WM_INITDIALOG body of the English-name dialog:
//   edit 667  <- model nameEn; edit 668 <- model commentEn with line breaks
//               normalized to CRLF ("\r" pairs and lone "\n" both become
//               "\r\n" for the multiline edit);
//   combo 669 <- bone English names bones[0..boneCount-1], select 0;
//   edit 672  <- bones[0].nameEn;
//   combo 673 <- morph English names morphs[1..morphCount-1], select 0;
//   edit 676  <- morphs[1].nameEn (only when facial frame groups exist);
//   combo 677 <- display-group English names starting at group slot 3/2
//               (with/without facial frame groups) up to groupCount-1;
//   edit 680  <- groups[first].nameEn; combo 677 select 0.
// Main window: model combo 436 disabled, radio 490 checked and 491..493
// cleared, editMode reset to 0.  Model: bone selection flags cleared, bone 0
// marked selected (selectedBone = 0).  Finishes with the panel sweep and
// zeroes the three combo cursors.
// =========================================================================//
void InitModelEdgeDialog(HWND hDlg) {  // was Sub43C430, VA 0x0043C430
    MMDApp* app = g_Block;
    mdl::ModelRecord* model = mdl::Mdl(app->SelectedModel());
    mdl::BoneRecord* bones = model->boneTable;
    mdl::MorphRecord* morphs = model->morphs;
    char buf[512];

    // model English name
    for(int id:{kEditModelNameEn,kEditBoneNameEn,kEditMorphNameEn,kEditGroupNameEn})
        SendDlgItemMessageW(hDlg,id,EM_SETLIMITTEXT,0,0);
    SetDlgItemTextW(hDlg,kEditModelNameEn,text_encoding::ModelName(*model,true).c_str());

    // model English comment: expand CR / LF to CRLF for the edit control
    std::size_t out = 0;
    for (const char* p = model->commentEn; *p != '\0';) {
        if (*p == '\r') {  // stored CRLF pair -> one CRLF
            buf[out++] = '\r';
            buf[out++] = '\n';
            p += 2;
        } else if (*p == '\n') {  // lone LF -> CRLF
            buf[out++] = '\r';
            buf[out++] = '\n';
            ++p;
        } else {
            buf[out++] = *p++;
        }
    }
    buf[out] = '\0';
    SendMessageA(GetDlgItem(hDlg, kEditModelCommentEn), EM_REPLACESEL, 0,
                 reinterpret_cast<LPARAM>(buf));

    // bone combo + edit
    HWND boneCombo = GetDlgItem(hDlg, kComboBoneName);
    SendMessageA(boneCombo, CB_RESETCONTENT, 0, 0);
    for (std::uint32_t j = 0; j < model->boneCount; ++j)
        SendMessageW(boneCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text_encoding::BoneName(bones[j],true).c_str()));
    SendMessageA(boneCombo, CB_SETCURSEL, 0, 0);
    HWND boneEdit = GetDlgItem(hDlg, kEditBoneNameEn);
    if (model->boneCount != 0) {
        SetWindowTextW(boneEdit,text_encoding::BoneName(bones[0],true).c_str());
    } else {
        EnableWindow(boneCombo, FALSE);
        EnableWindow(boneEdit, FALSE);
    }

    // morph combo + edit (combo skips morph slot 0)
    HWND morphCombo = GetDlgItem(hDlg, kComboMorphName);
    SendMessageA(morphCombo, CB_RESETCONTENT, 0, 0);
    for (std::uint32_t k = 1; k < model->morphCount; ++k)
        SendMessageW(morphCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text_encoding::MorphName(morphs[k],true).c_str()));
    SendMessageA(morphCombo, CB_SETCURSEL, 0, 0);
    HWND morphEdit = GetDlgItem(hDlg, kEditMorphNameEn);
    if (mikudancestudio::mdl::FacialFrameCount(*model) != 0 && model->morphCount > 1) {
        SetWindowTextW(morphEdit,text_encoding::MorphName(morphs[1],true).c_str());
    } else {
        EnableWindow(morphCombo, FALSE);
        EnableWindow(morphEdit, FALSE);
    }

    // display-group combo + edit (starts after the fixed slots)
    HWND groupCombo = GetDlgItem(hDlg, kComboGroupName);
    SendMessageA(groupCombo, CB_RESETCONTENT, 0, 0);
    mdl::DisplayGroup* groups = mdl::DisplayGroups(app->SelectedModel());
    const std::uint32_t firstGroup =
        mikudancestudio::mdl::FacialFrameCount(*model) != 0 ? 3 : 2;
    for (std::uint32_t g = firstGroup; g < mikudancestudio::mdl::DisplayGroupCount(*model); ++g)
        SendMessageW(groupCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text_encoding::DisplayGroupName(groups[g],true).c_str()));
    HWND groupEdit = GetDlgItem(hDlg, kEditGroupNameEn);
    if (firstGroup < mikudancestudio::mdl::DisplayGroupCount(*model)) {
        SetWindowTextW(groupEdit,text_encoding::DisplayGroupName(groups[firstGroup],true).c_str());
    } else {
        EnableWindow(groupCombo, FALSE);
        EnableWindow(groupEdit, FALSE);
    }
    SendMessageA(groupCombo, CB_SETCURSEL, 0, 0);

    // main window: lock the model combo, select the "model" radio group
    EnableWindow(GetDlgItem(app->state.hwnd, kMainComboModel), FALSE);
    SendMessageA(GetDlgItem(app->state.hwnd, kMainRadioFirst + 1),
                 BM_SETCHECK, 0, 0);
    SendMessageA(GetDlgItem(app->state.hwnd, kMainRadioFirst + 2),
                 BM_SETCHECK, 0, 0);
    SendMessageA(GetDlgItem(app->state.hwnd, kMainRadioFirst + 3),
                 BM_SETCHECK, 0, 0);
    SendMessageA(GetDlgItem(app->state.hwnd, kMainRadioFirst),
                 BM_SETCHECK, 1, 0);
    app->state.editMode = 0;

    // model: select bone 0
    unsigned char* selection = model->boneSelection;
    for (std::uint32_t i = 0; i < model->boneCount; ++i)
        selection[i] = 0;
    if (model->boneCount != 0) {
        selection[0] = 1;
        model->selectedBone = 0;
    } else {
        model->selectedBone = -1;
    }

    PostLanguageSweep(app);                                // 0x42F1E0
    app->ModelEdgeComboCursor(0) = 0;
    app->ModelEdgeComboCursor(1) = 0;
    app->ModelEdgeComboCursor(2) = 0;
}

// ===========================================================================
// 0x0045F240 - model-edge dialog apply (original: sub_45F240)
// ===========================================================================
// OK/close body of the English-name dialog.  Reads the English comment,
// strips the CRs of the edit's CRLF line breaks and stores it into the
// model's commentEn (256 bytes).  Commits the three name edits through the
// collector (0x43BED0 idx 0/1/2), then the group name edit (identical to
// collector idx 3: cursor 2 -> group slot +3/+2, capped at 49 chars).
// Re-runs both frame-row name mirrors (facial rows from the morph table,
// bone rows from the bone table - the collector already did them; the
// original repeats both loops) and finishes with the panel sweep, the panel
// rebuild PostLoadInit and the second-stage sweep.
// =========================================================================//
void ApplyModelEdgeDialog(HWND hDlg) {  // was Sub45F240, VA 0x0045F240
    MMDApp* app = g_Block;
    mdl::ModelRecord* model = mdl::Mdl(app->SelectedModel());
    char text[256];

    // English comment: model stores LF line breaks, edit uses CRLF
    GetWindowTextA(GetDlgItem(hDlg, kEditModelCommentEn), text, 256);
    if (std::strlen(text) >= 0x100)
        text[255] = '\0';
    char clean[256];
    char* d = clean;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p != '\r')
            *d++ = *p;
    }
    *d = '\0';
    strcpy_s(model->commentEn, 0x100, clean);

    // model / bone / morph English names
    CollectEnglishNameEdit(app, GetDlgItem(hDlg, kEditModelNameEn), 0);    // 0x43BED0
    if (model->boneCount != 0)
        CollectEnglishNameEdit(app, GetDlgItem(hDlg, kEditBoneNameEn), 1); // 0x43BED0
    if (mikudancestudio::mdl::FacialFrameCount(*model) != 0 && model->morphCount > 1)
        CollectEnglishNameEdit(app, GetDlgItem(hDlg, kEditMorphNameEn), 2); // 0x43BED0

    CollectEnglishNameEdit(app,GetDlgItem(hDlg,kEditGroupNameEn),3);

    // frame rows mirror the English names (repeat of the collector loops)
    MirrorFacialFrameNames(model);
    MirrorBoneFrameNames(model);

    PostLanguageSweep(app);                                // 0x42F1E0
    PostLoadInit(app->SelectedModel());                    // 0x49C850
    PostLanguageSweep2(app);                               // 0x40D070
}

// ===========================================================================
// 0x0045F050 - model-edge combo 669 refresh (original: sub_45F050)
// ===========================================================================
// Bone-combo change: commits the bone-name edit (collector idx 1), stores
// the new combo selection as cursor 0, clears the model's bone selection
// flags and marks the bone selected - except for bones whose type byte is
// 6, 7 or 9 (special/invisible bones), which leave the model without a
// selected bone (selectedBone = -1).  Panel sweep, then the bone-name edit
// is refreshed from bones[cursor].nameEn and focused.
// =========================================================================//
void SelectModelEdgeBone(HWND hDlg) {  // was Sub45F050, VA 0x0045F050
    MMDApp* app = g_Block;
    mdl::ModelRecord* model = mdl::Mdl(app->SelectedModel());

    CollectEnglishNameEdit(app, GetDlgItem(hDlg, kEditBoneNameEn), 1);    // 0x43BED0

    const int cursor = static_cast<int>(
        SendMessageA(GetDlgItem(hDlg, kComboBoneName), CB_GETCURSEL, 0, 0));
    app->ModelEdgeComboCursor(0) = cursor;

    unsigned char* selection = model->boneSelection;
    for (std::uint32_t i = 0; i < model->boneCount; ++i)
        selection[i] = 0;

    const mdl::BoneType kind = model->boneTable[cursor].type;
    if (kind == mdl::BoneType::Effector || kind == mdl::BoneType::InertTip ||
        kind == mdl::BoneType::CoRotate) {
        model->selectedBone = -1;
    } else {
        selection[cursor] = 1;
        model->selectedBone = cursor;
    }
    PostLanguageSweep(app);                                // 0x42F1E0

    RefreshNameEdit(GetDlgItem(hDlg,kEditBoneNameEn),text_encoding::BoneName(model->boneTable[cursor],true).c_str());
}

// ===========================================================================
// 0x0045EF10 - model-edge combo 673 refresh (original: sub_45EF10)
// ===========================================================================
// Morph-combo change, effective only while the model has facial frame
// groups: commits the morph-name edit (collector idx 2 - which already runs
// the panel sweep and PostLoadInit), stores the new selection as cursor 1
// and refreshes the morph-name edit from morphs[cursor+1].nameEn.
// =========================================================================//
void SelectModelEdgeMorph(HWND hDlg) {  // was Sub45EF10, VA 0x0045EF10
    MMDApp* app = g_Block;
    mdl::ModelRecord* model = mdl::Mdl(app->SelectedModel());
    if (mikudancestudio::mdl::FacialFrameCount(*model) == 0)
        return;

    CollectEnglishNameEdit(app, GetDlgItem(hDlg, kEditMorphNameEn), 2);   // 0x43BED0

    app->ModelEdgeComboCursor(1) = static_cast<int>(
        SendMessageA(GetDlgItem(hDlg, kComboMorphName), CB_GETCURSEL, 0, 0));
    PostLanguageSweep(app);                                // 0x42F1E0

    RefreshNameEdit(GetDlgItem(hDlg,kEditMorphNameEn),text_encoding::MorphName(model->morphs[MorphNameSlot(app)],true).c_str());
}

// ===========================================================================
// 0x0045EDC0 - model-edge combo 677 refresh (original: sub_45EDC0)
// ===========================================================================
// Group-combo change: first commits the current edit text into the group
// slot named by the OLD cursor 2 (slot +3/+2, capped at 49 chars), then
// stores the new selection as cursor 2, runs the panel sweep and refreshes
// the group-name edit from the newly selected group slot.
// =========================================================================//
void SelectModelEdgeGroup(HWND hDlg) {  // was Sub45EDC0, VA 0x0045EDC0
    MMDApp* app = g_Block;
    mdl::ModelRecord* model = mdl::Mdl(app->SelectedModel());

    CollectEnglishNameEdit(app,GetDlgItem(hDlg,kEditGroupNameEn),3);
    auto* groups=mdl::DisplayGroups(app->SelectedModel());
    app->ModelEdgeComboCursor(2) = static_cast<int>(
        SendMessageA(GetDlgItem(hDlg, kComboGroupName), CB_GETCURSEL, 0, 0));
    PostLanguageSweep(app);                                // 0x42F1E0

    RefreshNameEdit(GetDlgItem(hDlg,kEditGroupNameEn),text_encoding::DisplayGroupName(groups[GroupNameSlot(app, model)],true).c_str());
}

}  // namespace mikudancestudio
