#include "mikudancestudio/physics_scale.hpp"
#include "mikudancestudio/bone_scale.hpp"
// ===========================================================================
// Physics-model editor (menu 262 "enhance model / physics model", dialog
// 0x2AC EN / 0x2AB JP, x86 dialog proc 0x465020)
// ===========================================================================
// Split out of src/window/command_view_menu.cpp (the menu-251..302 command
// family).  The dialog edits the *selected model's* rigid bodies and joints
// (PMD physics) in two 100000-slot scratch arrays owned by the app object,
// then rebuilds the model tables and the Bullet world contents on OK.
//
// The previous body of PhysicsModelDlgProc in this file was a wrong transplant that
// read the very same record slots as camera/bone *frame* records; it has
// been rewritten from the original binary.  The control-ID call sites,
// array slots and helper VAs were already right - only the record
// semantics had been misread.
//
// x86 <-> x64 helper map (x64 = MikuMikuDanceE_v932x64 recompile):
//   x86 0x465020  dialog proc            x64 sub_7FF7CB4AD800
//   x86 0x41EC50  edit subclass proc     x64 sub_7FF7CB4AE950
//   x86 0x45F670  dialog init            x64 sub_7FF7CB4B1D20
//   x86 0x41FF30  collect body edits     x64 sub_7FF7CB4AFEB0
//   x86 0x45F480  add rigid body         x64 sub_7FF7CB4B17B0
//   x86 0x4204F0  collect joint edits    x64 sub_7FF7CB4B05C0
//   x86 0x43CA50  add joint              x64 sub_7FF7CB4B1A30
//   x86 0x43CCD0  body record -> edits   x64 sub_7FF7CB4B2590
//   x86 0x4214A0  joint record -> edits  x64 sub_7FF7CB4B2D90
//   x86 0x421C20  body/joint page flip   x64 sub_7FF7CB4B3720
//   x86 0x421CE0  shape radio/labels     x64 sub_7FF7CB4B3810
//   x86 0x420DC0  bone-combo pivot pick  x64 sub_7FF7CB4B0FF0
//   x86 0x4220F0  commit model+physics   x64 sub_7FF7CB4B3D40
//   (x86 0x4220C0 seek+re-eval = x64 sub_7FF7CB4B3D00 lives in
//    src/model/model_frame_seek.cpp and is called on close.)
//
// Editor storage (app blob slots, shared with the accessory physics
// editor): rigidScratchArray = rigid-body scratch array
// (mdl::RigidRecord x 100000, index state.selectedRigidIndex), jointScratchArray =
// joint scratch array (mdl::JointRecord x 100000, index state.selectedJointIndex).
// In the scratch copies the RigidRecord::keyData slot holds the combo-box
// index (-1 = deleted slot) and JointRecord::constraint holds the joint
// combo index; RigidRecord::noCollapse holds the *collision* mask while
// editing (bit set = collides with group N+1; inverted back to the PMD
// no-collapse sense on commit).
//
// Dialog control map (template 684_0411.bin / x64 proc):
//   685  add body button          0x2AD      735  add joint button    0x2DF
//   704  body list combo          selchg     736  joint list combo    selchg
//   705  body name edit (20 SJIS) enter      740  joint name edit     enter
//   706  delete body button       0x2C2      737  delete joint button 0x2E1
//   707  related bone combo       selchg     741  joint body-A combo   selchg
//   708  group combo (1..16)      selchg     742  joint body-B combo   selchg
//   709-711 body size x/y/z                  743  place-at-bone combo selchg
//   712-714 body position x/y/z              744-746 joint position x/y/z
//   715-717 body rotation deg                747/758/760 linear lower limit
//   718  body mass                            748/749/750 spring linear xyz
//   719  group list text ("1 5 9 ")           751/752/753 spring angular xyz
//   720-723 damp/damp/restit/friction         754-756 joint rotation deg
//   724  copy body button        0x2D4       757/759/761 linear upper limit
//   725  paste body button       0x2D5       762/764/766 angular lower deg
//   726-728 shape radio sph/box/caps          763/765/767 angular upper deg
//   729  physical radio (mode!=0)             738  copy joint button   0x2E2
//   730  bone-follow radio (mode==0)          739  paste joint button  0x2E3
//   731  bone-alignment checkbox              800/801 body/joint page radio
//   732-734 size labels radius/w+h/depth
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commdlg.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

#include "btBulletDynamicsCommon.h"

#include "mikudancestudio/d3dx_dyn.hpp"
#include "mikudancestudio/global_key_layout.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/ui_translation.hpp"
#include "mikudancestudio/ui_language.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/text_encoding.hpp"

namespace mikudancestudio {

// Forward declarations (twins of the block in command_view_menu.cpp - the
// helpers are defined in this file).
void AddRigidBody(HWND hDlg);                                // was Sub45F480, VA 0x0045F480
LRESULT CALLBACK PhysicsEditSubclassProc(HWND, UINT, WPARAM, LPARAM);  // was Sub41EC50, VA 0x0041EC50
void InitPhysicsModelDialog(HWND hDlg);                      // was Sub45F670, VA 0x0045F670
void CollectBodyEdits(MMDApp* app);                          // was Sub41FF30, VA 0x0041FF30
void CollectJointEdits(MMDApp* app);                         // was Sub4204F0, VA 0x004204F0
void AddJoint(HWND hDlg);                                    // was Sub43CA50, VA 0x0043CA50
void ApplyBodyRecordToEdits(HWND hDlg, int idx);             // was Sub43CCD0, VA 0x0043CCD0
void ApplyJointRecordToEdits(HWND hDlg, int idx);            // was Sub4214A0, VA 0x004214A0
void FlipPhysicsDialogPage(HWND hDlg, int flag);             // was Sub421C20, VA 0x00421C20
void UpdateShapeControls(HWND hDlg, int mode, int idx);      // was Sub421CE0, VA 0x00421CE0
void CommitPhysicsEdits(HWND hDlg);                          // was Sub4220F0, VA 0x004220F0
void PickPivotBone(HWND hDlg);                               // was Sub420DC0, VA 0x00420DC0

// ---- control IDs ----------------------------------------------------------
constexpr int kAddBodyButton = 685;        // 0x2AD
constexpr int kBodyPageFirst = 685;
constexpr int kBodyPageLast = 734;
constexpr int kDeleteBodyButton = 706;     // 0x2C2
constexpr int kBodyListCombo = 704;
constexpr int kBodyNameEdit = 705;
constexpr int kBoneCombo = 707;            // related bone ("none" + bones)
constexpr int kGroupCombo = 708;           // "1".."15", "16(F)"
constexpr int kSizeXEdit = 709;
constexpr int kSizeYEdit = 710;
constexpr int kSizeZEdit = 711;
constexpr int kPosXEdit = 712;
constexpr int kPosYEdit = 713;
constexpr int kPosZEdit = 714;
constexpr int kRotXEdit = 715;
constexpr int kRotYEdit = 716;
constexpr int kRotZEdit = 717;
constexpr int kMassEdit = 718;
constexpr int kGroupTextEdit = 719;
constexpr int kLinDampEdit = 720;
constexpr int kAngDampEdit = 721;
constexpr int kRestitutionEdit = 722;
constexpr int kFrictionEdit = 723;
constexpr int kCopyBodyButton = 724;       // 0x2D4
constexpr int kPasteBodyButton = 725;      // 0x2D5
constexpr int kShapeSphereRadio = 726;     // 0x2D6
constexpr int kShapeBoxRadio = 727;        // 0x2D7
constexpr int kShapeCapsuleRadio = 728;    // 0x2D8
constexpr int kPhysicalRadio = 729;        // 0x2D9
constexpr int kBoneFollowRadio = 730;      // 0x2DA
constexpr int kBoneAlignCheckbox = 731;    // 0x2DB
constexpr int kSizeXLabel = 732;
constexpr int kSizeYLabel = 733;
constexpr int kSizeZLabel = 734;
constexpr int kJointPageFirst = 735;
constexpr int kJointPageLast = 799;
constexpr int kAddJointButton = 735;       // 0x2DF
constexpr int kJointListCombo = 736;
constexpr int kJointNameEdit = 740;
constexpr int kDeleteJointButton = 737;    // 0x2E1
constexpr int kBodyACombo = 741;
constexpr int kBodyBCombo = 742;
constexpr int kPivotBoneCombo = 743;
constexpr int kCopyJointButton = 738;      // 0x2E2
constexpr int kPasteJointButton = 739;     // 0x2E3
constexpr int kJointPosXEdit = 744;
constexpr int kJointPosYEdit = 745;
constexpr int kJointPosZEdit = 746;
constexpr int kLinLowerXEdit = 747;      // linear lower limit x
constexpr int kSpringLinXEdit = 748;     // spring linear x
constexpr int kSpringLinYEdit = 749;
constexpr int kSpringLinZEdit = 750;
constexpr int kSpringAngXEdit = 751;     // spring angular x
constexpr int kSpringAngYEdit = 752;
constexpr int kSpringAngZEdit = 753;
constexpr int kJointRotXEdit = 754;
constexpr int kJointRotYEdit = 755;
constexpr int kJointRotZEdit = 756;
constexpr int kLinUpperXEdit = 757;      // linear upper limit x
constexpr int kLinLowerYEdit = 758;
constexpr int kLinUpperYEdit = 759;
constexpr int kLinLowerZEdit = 760;
constexpr int kLinUpperZEdit = 761;
constexpr int kAngLowerXEdit = 762;      // angular lower limit x (deg)
constexpr int kAngUpperXEdit = 763;      // angular upper limit x (deg)
constexpr int kAngLowerYEdit = 764;      // y axis: clamped to +-80
constexpr int kAngUpperYEdit = 765;      // y axis: clamped to +-80
constexpr int kAngLowerZEdit = 766;
constexpr int kAngUpperZEdit = 767;
constexpr int kBodyPageRadio = 800;        // 0x320
constexpr int kJointPageRadio = 801;       // 0x321

// ---- capacities / numeric literals of the original ------------------------
// The two original builds sized the scratch arrays by a byte constant
// (x86: 1,400,000 / record 140 = 10,000 slots; the x64 recompile doubled the
// record sizes with its pointers and raised the pool tenfold:
// 15,200,000 / 152 = 100,000 slots).
#if MIKUDANCESTUDIO_X64
constexpr int kMaxEditRecords = 100000;    // slots per scratch array
#else
constexpr int kMaxEditRecords = 10000;     // x86 original capacity
#endif
constexpr float kPiF = 3.1415927f;         // float-rounded pi (0x40490FDB)
static_assert(__builtin_bit_cast(std::uint32_t, kPiF) == 0x40490FDBu,
              "float-rounded pi bit-exact");
constexpr float kPiShort = 3.141592f;      // truncated pi used for deg<->rad
static_assert(__builtin_bit_cast(std::uint32_t, kPiShort) == 0x40490FD8u,
              "truncated pi bit-exact");

// ---- JP strings, byte-exact Shift-JIS as in the binary --------------------
// x64 0x7FF7CB54F2A0 / x86 0x52E724:
// "編集結果を'拡張モデル保存'で新しいモデルとして保存して下さい"
static const char kMsgEnhanceModelJp[] =
    "\x95\xd2\x8f\x57\x8c\x8b\x89\xca\x82\xcd\x27\x8a\x67\x92\xa3"
    "\x83\x82\x83\x66\x83\x8b\x95\xdb\x91\xb6\x27\x82\xc5\x90\x56"
    "\x82\xb5\x82\xa2\x83\x82\x83\x66\x83\x8b\x82\xc6\x82\xb5\x82"
    "\xc4\x95\xdb\x91\xb6\x82\xb5\x82\xc4\x89\xba\x82\xb3\x82\xa2";
// x64 0x7FF7CB54F290 / x86 0x52E764: "モデル拡張"
static const char kCaptionEnhanceModelJp[] =
    "\x83\x82\x83\x66\x83\x8b\x8a\x87\x92\xa3";
// x64 0x7FF7CB551198: "Joint角度制限"
static const char kCaptionAngleLimitJp[] =
    "Joint\x8a\x70\x93\x78\x90\xa7\x8c\xc0";
// x64 0x7FF7CB5511A8:
// "Bulletの仕様上、y軸の角度制限は-90<y<90の間しか設定できません"
static const char kMsgAngleLimitJp[] =
    "Bullet\x82\xcc\x8e\x64\x97\x6c\x8f\xe3\x81\x41y\x8e\xb2\x82"
    "\xcc\x8a\x70\x93\x78\x90\xa7\x8c\xc0\x82\xcd-90<y<90\x82\xcc"
    "\x8a\xd4\x82\xb5\x82\xa9\x90\xdd\x92\xe8\x82\xc5\x82\xab\x82"
    "\xdc\x82\xb9\x82\xf1";
// x64 0x7FF7CB551240: "ジョイント追加"
static const char kCaptionAddJointJp[] =
    "\x83\x57\x83\x87\x83\x43\x83\x93\x83\x67\x92\xc7\x89\xc1";
// x64 0x7FF7CB551250:
// "剛体が一つもありません\n先に剛体を追加して下さい"
static const char kMsgNoBodyJp[] =
    "\x8d\x84\x91\xcc\x82\xaa\x88\xea\x82\xc2\x82\xe0\x82\xa0\x82"
    "\xe8\x82\xdc\x82\xb9\x82\xf1\x0a\x90\xe6\x82\xc9\x8d\x84\x91"
    "\xcc\x82\xf0\x92\xc7\x89\xc1\x82\xb5\x82\xc4\x89\xba\x82\xb3"
    "\x82\xa2";
// x64 0x7FF7CB55129C: last group-combo entry (Shift-JIS)
static const char kGroup16Jp[] = "16(\x8f\xb0)";

namespace {

// ---- editor-array views over the shared app slots -------------------------
using Rigid = mdl::RigidRecord;
using Joint = mdl::JointRecord;

Rigid* EditBodies() {
    MMDApp* app = g_Block;
    return static_cast<Rigid*>(app->state.rigidScratchArray);
}
Joint* EditJoints() {
    MMDApp* app = g_Block;
    return static_cast<Joint*>(app->state.jointScratchArray);
}

// The scratch copies reuse RigidRecord::keyData as the body-list combo index
// (-1 marks a deleted slot; the original stores the plain int there).
int BodyComboIndex(const Rigid& rb) {
    return static_cast<int>(reinterpret_cast<std::intptr_t>(rb.keyData));
}
void SetBodyComboIndex(Rigid& rb, int index) {
    rb.keyData = reinterpret_cast<void*>(static_cast<std::intptr_t>(index));
}
int BodySlotForCombo(int row) {
    if (row < 0) return -1;
    for (int i = 0; i < kMaxEditRecords; ++i)
        if (BodyComboIndex(EditBodies()[i]) == row) return i;
    return -1;
}
int JointSlotForCombo(int row) {
    if (row < 0) return -1;
    for (int i = 0; i < kMaxEditRecords; ++i)
        if (EditJoints()[i].constraint == row) return i;
    return -1;
}
bool LiveBodySlot(int slot) {
    return slot >= 0 && slot < kMaxEditRecords && BodyComboIndex(EditBodies()[slot]) >= 0;
}
void SyncBodyMode(Rigid& body) {
    body.staticFlag = body.mode == 0 ? 1 : 0;
    body.kinematicFlag = body.mode == 2 ? 1 : 0;
}
bool ValidatePhysicsEdits(HWND dialog) {
    const Joint* joints = EditJoints();
    for (int i = 0; i < kMaxEditRecords; ++i) {
        if (joints[i].constraint < 0) continue;
        if (LiveBodySlot(joints[i].rigidA) && LiveBodySlot(joints[i].rigidB)) continue;
        const wchar_t* messages[] = {
            L"関節が削除済み、または存在しない剛体を参照しています。剛体 A と B を選択し直してください。",
            L"A joint refers to a deleted or missing rigid body. Select valid bodies A and B before applying the edits.",
            L"关节引用了已删除或不存在的刚体，请重新选择有效的刚体 A 和 B 后再确定。"};
        const unsigned language = g_Block->EnglishUI() <= 2 ? g_Block->EnglishUI() : 1;
        MessageBoxW(dialog, messages[language], L"MikuDanceStudio", MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}
// JointRecord::constraint doubles as the joint-list combo index while the
// dialog owns the records.

// Copy/paste staging.  The original stages the records in the app blob right
// behind the array pointers (x86 0xA0B80 / 0xA0C34); those blob slots were
// sized for the mis-read 172/140 layout, so the staging lives here instead -
// it is private to this dialog either way.
Rigid s_bodyCopyScratch;
Joint s_jointCopyScratch;
// "joint page visible" flag (x64 app+654528, written by the page flip and
// the init; nothing else in the ported window reads it).
unsigned char s_jointPageVisible = 0;

// ---- physics-world removal helpers ----------------------------------------
// Mirrors the ModelDispose teardown (src/model/model_dispose.cpp): the
// original's 0x4260E0/0x426760 twins remove one constraint / rigid body
// from the shared Bullet world.
void RemovePhysJointByUid(PhysicsScene* scene, int uid) {
    if (scene == nullptr || scene->world == nullptr)
        return;
    btDiscreteDynamicsWorld* const world = scene->world;
    for (int i = world->getNumConstraints() - 1; i >= 0; --i) {
        btTypedConstraint* const item = world->getConstraint(i);
        if (item != nullptr && item->getUid() == uid) {
            world->removeConstraint(item);
            delete item;
            return;
        }
    }
}

void RemovePhysRigidBody(PhysicsScene* scene, btRigidBody* body) {
    if (scene == nullptr || scene->world == nullptr || body == nullptr)
        return;
    scene->world->removeRigidBody(body);
    delete body->getMotionState();
    delete body->getCollisionShape();
    delete body;
}

// out = m * point (row-vector convention of the D3DX-style matrices).
void TransformPoint(float out[3], const float point[3], const float m[16]) {
    for (int column = 0; column < 3; ++column) {
        out[column] = point[0] * m[column] + point[1] * m[4 + column] +
                      point[2] * m[8 + column] + m[12 + column];
    }
}

// ---- edit-box float commits (subclass proc 0x41EC50 / collect paths) ------
void ReadEditFloat(HWND hWnd, float& field) {
    char text[256];
    GetWindowTextA(hWnd, text, 256);
    field = static_cast<float>(atof(text));
}
void ReadEditFloatClamped(HWND hWnd, float& field) {
    char text[256];
    GetWindowTextA(hWnd, text, 256);
    if (atof(text) < 0.1) {
        SetWindowTextA(hWnd, "0.1");
        GetWindowTextA(hWnd, text, 256);
    }
    field = static_cast<float>(atof(text));
}
void ReadEditDegrees(HWND hWnd, float& field) {
    char text[256];
    GetWindowTextA(hWnd, text, 256);
    field = static_cast<float>(atof(text)) / 180.0f * kPiF;
}

// Rename-commit for the body name edit: refresh the entry in the list combo
// and the joint body-A/B combos in place (delete + insert at the same slot).
// PMX keeps the original Unicode name alongside its fixed-width CP932 mirror.
// PMD has only the mirror, which may use all 20 bytes without a terminator.
// Never interpret either format using the machine's active ANSI code page.
std::wstring PhysicsLegacyNameWide(const char* text, std::size_t capacity) {
    std::wstring wide;
    const std::string bounded(text, strnlen_s(text, capacity));
    if (!text_encoding::Decode(bounded.c_str(), 932, wide)) {
        const int count = MultiByteToWideChar(932, 0, bounded.c_str(), -1, nullptr, 0);
        if (count > 0) {
            wide.resize(static_cast<std::size_t>(count));
            MultiByteToWideChar(932, 0, bounded.c_str(), -1, wide.data(), count);
            wide.resize(static_cast<std::size_t>(count - 1));
        }
    }
    return wide;
}

template <typename Record>
std::wstring PhysicsNameWide(const Record& record) {
    if (record.jpText && *record.jpText) return record.jpText;
    if (record.enText && *record.enText) return record.enText;
    return PhysicsLegacyNameWide(record.name, sizeof(record.name));
}

template <typename Record>
void AddPhysicsName(HWND combo, const Record& record) {
    const std::wstring wide = PhysicsNameWide(record);
    SendMessageW(combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(wide.c_str()));
}

template <typename Record>
void SetPhysicsNameEdit(HWND edit, const Record& record) {
    const std::wstring wide = PhysicsNameWide(record);
    SetWindowTextW(edit, wide.c_str());
}

std::wstring PhysicsEditText(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

bool PhysicsNameNarrow(const wchar_t* text, std::size_t capacity,
                       std::string& encoded) {
    if (!text_encoding::EncodeExact(text, 932, encoded)) return false;
    // Walk character boundaries from the beginning: a valid trail byte can
    // itself lie in the lead-byte range. PMD permits a full 20-byte name.
    std::size_t length = 0;
    while (length < encoded.size()) {
        const std::size_t next = length +
            (IsDBCSLeadByteEx(932, static_cast<BYTE>(encoded[length])) ? 2 : 1);
        if (next > capacity) break;
        length = next;
    }
    encoded.resize(length);
    return true;
}

template <typename Record>
bool CollectPhysicsName(HWND edit, Record& record) {
    const std::wstring previous = PhysicsNameWide(record);
    const std::wstring requested = PhysicsEditText(edit);
    // Opening, changing pages and OK must not rewrite the PMX mirror or lose
    // the full Unicode source name, including names longer than 256 units.
    if (requested == previous) return false;
    std::string encoded;
    if (!PhysicsNameNarrow(requested.c_str(), sizeof(record.name), encoded)) {
        SetWindowTextW(edit, previous.c_str());
        const unsigned char language = g_Block->EnglishUI();
        const wchar_t* message = language == kUiChinese
            ? L"此名称包含扩展模型保存格式（PMD）无法保存的字符，已保留原名称。请使用可用 Shift-JIS 表示的名称。"
            : language == kUiJapanese
            ? L"拡張モデルの保存形式（PMD）では保存できない文字が含まれています。元の名前を保持しました。Shift-JIS で表せる名前を使用してください。"
            : L"This name contains characters that the enhanced-model format (PMD) cannot save. The original name was kept. Use a name representable in Shift-JIS.";
        const wchar_t* caption = language == kUiChinese ? L"物理模型名称"
            : language == kUiJapanese ? L"物理モデルの名前" : L"Physics model name";
        MessageBoxW(GetParent(edit), message, caption, MB_OK | MB_ICONINFORMATION);
        return false;
    }
    std::memset(record.name, 0, sizeof(record.name));
    std::memcpy(record.name, encoded.data(), encoded.size());
    // Scratch records borrow these pointers. The model still owns them until
    // OK; Cancel must leave them intact. A real rename now displays the new
    // CP932 name rather than the stale PMX source text.
    record.jpText = nullptr;
    record.enText = nullptr;
    SetPhysicsNameEdit(edit, record);
    return true;
}

void CommitNameToCombo(HWND combo, int current, const wchar_t* text) {
    if (current < 0) return;
    const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    SendMessageW(combo, CB_DELETESTRING, current, 0);
    SendMessageW(combo, CB_INSERTSTRING, current,
                 reinterpret_cast<LPARAM>(text));
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

void CollectBodyName(HWND dialog, Rigid& record) {
    if (!CollectPhysicsName(GetDlgItem(dialog, kBodyNameEdit), record)) return;
    const std::wstring name = PhysicsNameWide(record);
    const int index = BodyComboIndex(record);
    for (const int control : {kBodyListCombo, kBodyACombo, kBodyBCombo})
        CommitNameToCombo(GetDlgItem(dialog, control), index, name.c_str());
}

void CollectJointName(HWND dialog, Joint& record) {
    if (!CollectPhysicsName(GetDlgItem(dialog, kJointNameEdit), record)) return;
    const std::wstring name = PhysicsNameWide(record);
    CommitNameToCombo(GetDlgItem(dialog, kJointListCombo), record.constraint,
                      name.c_str());
}

// Joint y-axis angular limit check shared by edits 765/764 (0x4B05C0).
// Returns the (possibly clamped) value and fixes the edit text.
float CheckYAngleLimit(HWND hDlg, HWND hEdit, float value) {
    MMDApp* app = g_Block;
    char text[256];
    if (value <= -90.0f || value >= 90.0f) {
        if (app->state.englishUI != 0) {
            LegacyUiMessageBoxA(hDlg, "y-axis limit must between -90 to 90",
                        "angle limit", 0);
        } else {
            LegacyUiMessageBoxA(hDlg, kMsgAngleLimitJp, kCaptionAngleLimitJp, 0);
        }
        if (value > -90.0f) {
            value = 80.0f;
            strcpy_s(text, sizeof text, "80.00");
        } else {
            value = -80.0f;
            strcpy_s(text, sizeof text, "-80.00");
        }
        SetWindowTextA(hEdit, text);
    }
    return value;
}

}  // namespace

// ---------------------------------------------------------------------------
// 0x41EC50 (x64 sub_7FF7CB4AE950) - subclass wndproc installed on the body
// name edit 705, the body float edits 709..723, the joint name edit 740 and
// the joint float edits 744..767.  Enter (VK_RETURN) commits the edit into
// the selected scratch record; the two name edits additionally rename the
// entry in their list combos (and the joint body-A/B combos for bodies).
// Everything else forwards to the saved edit proc of 705.
// ---------------------------------------------------------------------------
LRESULT CALLBACK PhysicsEditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {  // was Sub41EC50, VA 0x0041EC50
    MMDApp* app = g_Block;
    if (msg != WM_KEYDOWN || wParam != VK_RETURN)
        return CallWindowProcW(app->FrameCopyEditProc(), hWnd, msg, wParam,
                               lParam);
    // dirty write: invalidates the cached time like every WM_COMMAND of
    // the dialog (original app+663064 = state.timeNowHigh)
    app->state.timeNowHigh = 1;
    HWND hDlg = app->state.frameCopyDialog;
    Rigid* bodies = EditBodies();
    Joint* joints = EditJoints();
    const int control = GetDlgCtrlID(hWnd);
    if (control >= kBodyNameEdit && control <= kFrictionEdit) {
        if (!LiveBodySlot(app->state.selectedRigidIndex)) return 0;
        Rigid& rb = bodies[app->state.selectedRigidIndex];
        if (hWnd == GetDlgItem(hDlg, kBodyNameEdit)) {
            CollectBodyName(hDlg, rb);
            return 0;
        }
        if (hWnd == GetDlgItem(hDlg, kSizeXEdit)) { ReadEditFloatClamped(hWnd, rb.size[0]); return 0; }
        if (hWnd == GetDlgItem(hDlg, kSizeYEdit)) { ReadEditFloatClamped(hWnd, rb.size[1]); return 0; }
        if (hWnd == GetDlgItem(hDlg, kSizeZEdit)) { ReadEditFloatClamped(hWnd, rb.size[2]); return 0; }
        if (hWnd == GetDlgItem(hDlg, kPosXEdit)) { ReadEditFloat(hWnd, rb.position[0]); return 0; }
        if (hWnd == GetDlgItem(hDlg, kPosYEdit)) { ReadEditFloat(hWnd, rb.position[1]); return 0; }
        if (hWnd == GetDlgItem(hDlg, kPosZEdit)) { ReadEditFloat(hWnd, rb.position[2]); return 0; }
        if (hWnd == GetDlgItem(hDlg, kRotXEdit)) { ReadEditDegrees(hWnd, rb.rotation[0]); return 0; }
        if (hWnd == GetDlgItem(hDlg, kRotYEdit)) { ReadEditDegrees(hWnd, rb.rotation[1]); return 0; }
        if (hWnd == GetDlgItem(hDlg, kRotZEdit)) { ReadEditDegrees(hWnd, rb.rotation[2]); return 0; }
        if (hWnd == GetDlgItem(hDlg, kMassEdit)) { ReadEditFloatClamped(hWnd, rb.mass); return 0; }
        if (hWnd == GetDlgItem(hDlg, kLinDampEdit)) { ReadEditFloat(hWnd, rb.linearDamping); return 0; }
        if (hWnd == GetDlgItem(hDlg, kAngDampEdit)) { ReadEditFloat(hWnd, rb.angularDamping); return 0; }
        if (hWnd == GetDlgItem(hDlg, kRestitutionEdit)) { ReadEditFloat(hWnd, rb.restitution); return 0; }
        if (hWnd == GetDlgItem(hDlg, kFrictionEdit)) { ReadEditFloat(hWnd, rb.friction); return 0; }
    }
    if (app->state.selectedJointIndex < 0 || app->state.selectedJointIndex >= kMaxEditRecords ||
        joints[app->state.selectedJointIndex].constraint < 0) return 0;
    Joint& jt = joints[app->state.selectedJointIndex];

    if (hWnd == GetDlgItem(hDlg, kJointNameEdit)) {
        CollectJointName(hDlg, jt);
        return 0;
    }
    if (hWnd == GetDlgItem(hDlg, kJointPosXEdit)) { ReadEditFloat(hWnd, jt.position[0]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kJointPosYEdit)) { ReadEditFloat(hWnd, jt.position[1]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kJointPosZEdit)) { ReadEditFloat(hWnd, jt.position[2]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kJointRotXEdit)) { ReadEditDegrees(hWnd, jt.rotation[0]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kJointRotYEdit)) { ReadEditDegrees(hWnd, jt.rotation[1]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kJointRotZEdit)) { ReadEditDegrees(hWnd, jt.rotation[2]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kLinUpperXEdit)) { ReadEditFloat(hWnd, jt.limits[0]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kLinUpperYEdit)) { ReadEditFloat(hWnd, jt.limits[1]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kLinUpperZEdit)) { ReadEditFloat(hWnd, jt.limits[2]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kLinLowerXEdit)) { ReadEditFloat(hWnd, jt.limits[3]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kLinLowerYEdit)) { ReadEditFloat(hWnd, jt.limits[4]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kLinLowerZEdit)) { ReadEditFloat(hWnd, jt.limits[5]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kAngUpperXEdit)) { ReadEditDegrees(hWnd, jt.limits[6]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kAngUpperZEdit)) { ReadEditDegrees(hWnd, jt.limits[8]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kAngLowerXEdit)) { ReadEditDegrees(hWnd, jt.limits[9]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kAngLowerZEdit)) { ReadEditDegrees(hWnd, jt.limits[11]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kSpringLinXEdit)) { ReadEditFloat(hWnd, jt.springs[0]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kSpringLinYEdit)) { ReadEditFloat(hWnd, jt.springs[1]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kSpringLinZEdit)) { ReadEditFloat(hWnd, jt.springs[2]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kSpringAngXEdit)) { ReadEditFloat(hWnd, jt.springs[3]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kSpringAngYEdit)) { ReadEditFloat(hWnd, jt.springs[4]); return 0; }
    if (hWnd == GetDlgItem(hDlg, kSpringAngZEdit)) { ReadEditFloat(hWnd, jt.springs[5]); return 0; }

    // edits 765 / 764 (y-axis angular limits) validate against +-90 here too
    if (hWnd == GetDlgItem(hDlg, kAngUpperYEdit)) {
        char text[256];
        GetWindowTextA(hWnd, text, 256);
        float value = static_cast<float>(atof(text));
        value = CheckYAngleLimit(hDlg, hWnd, value);
        jt.limits[7] = value / 180.0f * kPiShort;
        return 0;
    }
    if (hWnd == GetDlgItem(hDlg, kAngLowerYEdit)) {
        char text[256];
        GetWindowTextA(hWnd, text, 256);
        float value = static_cast<float>(atof(text));
        value = CheckYAngleLimit(hDlg, hWnd, value);
        jt.limits[10] = value / 180.0f * kPiShort;
        return 0;
    }
    return CallWindowProcW(app->FrameCopyEditProc(), hWnd, msg, wParam,
                           lParam);
}

// ---------------------------------------------------------------------------
// 0x45F670 (x64 sub_7FF7CB4B1D20) - WM_INITDIALOG body.  Disables the main
// window's model combo (408), allocates the two 100000-slot scratch arrays,
// seeds them from the selected model's rigid/joint tables (converting the
// mode flags into the editor's 0/1/2 mode byte and inverting the collision
// mask), fills the list/bone/group combos and parks the dialog on the
// rigid-body page.
// ---------------------------------------------------------------------------
void InitPhysicsModelDialog(HWND hDlg) {  // was Sub45F670, VA 0x0045F670
    MMDApp* app = g_Block;
    mdl::ModelRecord* model = mdl::Mdl(app->SelectedModel());

    EnableWindow(GetDlgItem(app->state.hwnd, panel::kPlayButton), FALSE);

    app->state.rigidScratchArray =
        std::calloc(kMaxEditRecords, sizeof(Rigid));
    Rigid* bodies = EditBodies();
    for (int i = 0; i < kMaxEditRecords; ++i) {
        SetBodyComboIndex(bodies[i], -1);
        bodies[i].body = nullptr;
    }
    app->state.jointScratchArray =
        std::calloc(kMaxEditRecords, sizeof(Joint));
    Joint* joints = EditJoints();
    for (int i = 0; i < kMaxEditRecords; ++i) {
        joints[i].rigidA = 0;
        joints[i].rigidB = 0;
        joints[i].constraint = -1;
    }

    for (int id = kJointPageFirst; id <= kJointPageLast; ++id)
        ShowWindow(GetDlgItem(hDlg, id), SW_HIDE);
    CheckRadioButton(hDlg, kBodyPageRadio, kJointPageRadio, kBodyPageRadio);
    s_jointPageVisible = 0;

    SendMessageA(GetDlgItem(hDlg, kBodyListCombo), CB_RESETCONTENT,
                 0, 0);
    for (std::uint32_t i = 0; i < model->rigidCount; ++i) {
        Rigid& dst = bodies[i];
        std::memcpy(&dst, &model->rigidTable[i], sizeof(Rigid));
        // model flags -> editor mode byte (0 bone-follow / 1 physical /
        // 2 physical + bone alignment)
        if (model->rigidTable[i].staticFlag != 0)
            dst.mode = 0;
        else if (model->rigidTable[i].kinematicFlag != 0)
            dst.mode = 2;
        else
            dst.mode = 1;
        // PMD no-collapse mask -> positive collision mask for editing
        dst.noCollapse = static_cast<std::uint16_t>(~dst.noCollapse);
        AddPhysicsName(GetDlgItem(hDlg, kBodyListCombo), dst);
        AddPhysicsName(GetDlgItem(hDlg, kBodyACombo), dst);
        AddPhysicsName(GetDlgItem(hDlg, kBodyBCombo), dst);
        SetBodyComboIndex(dst, static_cast<int>(i));
    }
    int firstJoint = -1;
    int jointComboIndex = 0;
    for (std::uint32_t i = 0; i < model->jointCount; ++i) {
        if (model->jointTable[i].constraint < 0) continue;
        if (firstJoint < 0) firstJoint = static_cast<int>(i);
        Joint& dst = joints[i];
        std::memcpy(&dst, &model->jointTable[i], sizeof(Joint));
        AddPhysicsName(GetDlgItem(hDlg, kJointListCombo), dst);
        dst.constraint = jointComboIndex++;
    }

    SendMessageA(GetDlgItem(hDlg, kBodyListCombo), CB_SETCURSEL, 0, 0);
    SendMessageA(GetDlgItem(hDlg, kJointListCombo), CB_SETCURSEL, 0, 0);
    SendMessageA(GetDlgItem(hDlg, kBodyACombo), CB_SETCURSEL, 0, 0);
    SendMessageA(GetDlgItem(hDlg, kBodyBCombo), CB_SETCURSEL, 0, 0);

    // Related-bone and pivot lists use the same localized bone-name helper
    // as the main editor. The related-bone list also starts with a localized
    // "none" entry.
    SendMessageA(GetDlgItem(hDlg, kBoneCombo), CB_RESETCONTENT, 0, 0);
    const wchar_t* noBone = app->EnglishUI() == 1 ? L"none"
        : app->EnglishUI() == 2 ? L"无" : L"なし";
    SendMessageW(GetDlgItem(hDlg, kBoneCombo), CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(noBone));
    mdl::BoneRecord* bones = model->boneTable;
    for (std::uint32_t b = 0; b < model->boneCount; ++b) {
        // Names follow the shared rule (only the English UI takes nameEn):
        // `englishUI != 0` made the Chinese UI list English bone names here
        // while the main panel's morph combos showed the model's own names.
        const std::wstring wideName = text_encoding::BoneLabel(
            bones[b], UiUsesEnglishNames(app->state.englishUI));
        SendMessageW(GetDlgItem(hDlg, kBoneCombo), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(wideName.c_str()));
        SendMessageW(GetDlgItem(hDlg, kPivotBoneCombo), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(wideName.c_str()));
    }

    // group combo: "1".."15" + "16(F)"
    SendMessageA(GetDlgItem(hDlg, kGroupCombo), CB_RESETCONTENT, 0, 0);
    char text[8];
    for (int n = 1; n <= 15; ++n) {
        sprintf_s(text, sizeof text, "%d", n);
        SendMessageA(GetDlgItem(hDlg, kGroupCombo), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text));
    }
    const std::wstring group16 = app->state.englishUI != 0 ? L"16(F)"
        : PhysicsLegacyNameWide(kGroup16Jp, sizeof(kGroup16Jp));
    SendMessageW(GetDlgItem(hDlg, kGroupCombo), CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(group16.c_str()));

    // neutralize every bone's edit transform before the first refresh
    for (std::uint32_t b = 0; b < model->boneCount; ++b) {
        bones[b].trans[0] = bones[b].trans[1] = bones[b].trans[2] = 0.0f;
        bones[b].rotQuat[0] = bones[b].rotQuat[1] = bones[b].rotQuat[2] = 0.0f;
        bones[b].rotQuat[3] = 1.0f;
    }

    if (model->rigidCount != 0) {
        ApplyBodyRecordToEdits(hDlg, 0);
        app->state.selectedRigidIndex = 0;
    } else {
        ApplyBodyRecordToEdits(hDlg, -1);
        app->state.selectedRigidIndex = -1;
    }
    if (firstJoint >= 0) {
        ApplyJointRecordToEdits(hDlg, firstJoint);
        app->state.selectedJointIndex = firstJoint;
    } else {
        ApplyJointRecordToEdits(hDlg, -1);
        app->state.selectedJointIndex = -1;
    }
}

// ---------------------------------------------------------------------------
// Release the two 100000-slot scratch arrays allocated by
// InitPhysicsModelDialog.  The dialog's OK and Cancel handlers free them, but
// "new scene" (menu 0xCD) and a PMM load tear the modeless dialog down through
// ResetAppState, which called DestroyWindow alone: that path left both
// allocations live (~30 MB) and every such load added another pair.  Both app
// slots are nulled, so this path and the dialog handlers cannot free the same
// block twice.
// Defect B2-06.
// ---------------------------------------------------------------------------
void ReleasePhysicsEditScratch(MMDApp* app) {
    if (app->state.rigidScratchArray != nullptr) {
        std::free(app->state.rigidScratchArray);
        app->state.rigidScratchArray = nullptr;
    }
    if (app->state.jointScratchArray != nullptr) {
        std::free(app->state.jointScratchArray);
        app->state.jointScratchArray = nullptr;
    }
}

// ---------------------------------------------------------------------------
// 0x41FF30 (x64 sub_7FF7CB4AFEB0) - pull the body edits of the selected
// record into its scratch slot: name, size (0.1-clamped), position, rotation
// (deg -> rad), mass, damping/restitution/friction, and the group-list text
// ("1 5 9 " -> 16-bit collision mask, bit N set = group N+1).
// ---------------------------------------------------------------------------
void CollectBodyEdits(MMDApp* app) {  // was Sub41FF30, VA 0x0041FF30
    if (app->state.selectedRigidIndex == -1)
        return;
    Rigid& rb = EditBodies()[app->state.selectedRigidIndex];
    HWND hDlg = app->state.frameCopyDialog;
    char text[256];

    CollectBodyName(hDlg, rb);

    ReadEditFloatClamped(GetDlgItem(hDlg, kSizeXEdit), rb.size[0]);
    ReadEditFloatClamped(GetDlgItem(hDlg, kSizeYEdit), rb.size[1]);
    ReadEditFloatClamped(GetDlgItem(hDlg, kSizeZEdit), rb.size[2]);
    ReadEditFloat(GetDlgItem(hDlg, kPosXEdit), rb.position[0]);
    ReadEditFloat(GetDlgItem(hDlg, kPosYEdit), rb.position[1]);
    ReadEditFloat(GetDlgItem(hDlg, kPosZEdit), rb.position[2]);
    ReadEditDegrees(GetDlgItem(hDlg, kRotXEdit), rb.rotation[0]);
    ReadEditDegrees(GetDlgItem(hDlg, kRotYEdit), rb.rotation[1]);
    ReadEditDegrees(GetDlgItem(hDlg, kRotZEdit), rb.rotation[2]);
    ReadEditFloat(GetDlgItem(hDlg, kMassEdit), rb.mass);
    ReadEditFloat(GetDlgItem(hDlg, kLinDampEdit), rb.linearDamping);
    ReadEditFloat(GetDlgItem(hDlg, kAngDampEdit), rb.angularDamping);
    ReadEditFloat(GetDlgItem(hDlg, kRestitutionEdit), rb.restitution);
    ReadEditFloat(GetDlgItem(hDlg, kFrictionEdit), rb.friction);

    GetWindowTextA(GetDlgItem(hDlg, kGroupTextEdit), text, 100);
    std::uint16_t mask = 0;
    char* cursor = text;
    for (char* space = std::strstr(text, " "); space != nullptr;
         space = std::strstr(space + 1, " ")) {
        *space = '\0';
        const int bit = std::atoi(cursor) - 1;
        if (bit >= 0 && bit <= 15)
            mask |= static_cast<std::uint16_t>(1 << bit);
        cursor = space + 1;
    }
    const int bit = std::atoi(cursor) - 1;
    if (bit >= 0 && bit <= 15)
        mask |= static_cast<std::uint16_t>(1 << bit);
    rb.noCollapse = mask;
}

// ---------------------------------------------------------------------------
// 0x45F480 (x64 sub_7FF7CB4B17B0) - "add body": take the first free scratch
// slot, name it BODY_<slot>, register it in the list and body-A/B combos and
// fill editor defaults (shape sphere, size 2, mass 1, damping 0.5,
// friction 0.5, restitution 0, bone/group 0, mode 0).
// ---------------------------------------------------------------------------
void AddRigidBody(HWND hDlg) {  // was Sub45F480, VA 0x0045F480
    MMDApp* app = g_Block;
    Rigid* bodies = EditBodies();

    int slot = 0;
    while (slot < kMaxEditRecords && BodyComboIndex(bodies[slot]) >= 0)
        ++slot;
    if (slot == kMaxEditRecords) return;

    Rigid& rb = bodies[slot];
    rb.jpText = nullptr;
    rb.enText = nullptr;
    sprintf_s(rb.name, sizeof rb.name, "BODY_%d", slot);
    const LRESULT comboIndex = SendMessageW(
        GetDlgItem(hDlg, kBodyListCombo), CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(PhysicsNameWide(rb).c_str()));
    AddPhysicsName(GetDlgItem(hDlg, kBodyACombo), rb);
    AddPhysicsName(GetDlgItem(hDlg, kBodyBCombo), rb);
    SetBodyComboIndex(rb, static_cast<int>(comboIndex));
    SendMessageA(GetDlgItem(hDlg, kBodyListCombo), CB_SETCURSEL, comboIndex, 0);

    rb.boneIndex = 0;
    rb.mode = 0;
    SyncBodyMode(rb);
    rb.group = 0;
    rb.mass = 1.0f;
    rb.linearDamping = 0.5f;
    rb.angularDamping = 0.5f;
    rb.restitution = 0.0f;
    rb.friction = 0.5f;
    rb.shape = 0;
    rb.position[0] = rb.position[1] = rb.position[2] = 0.0f;
    rb.rotation[0] = rb.rotation[1] = rb.rotation[2] = 0.0f;
    rb.size[0] = rb.size[1] = rb.size[2] = 2.0f;

    app->state.selectedRigidIndex = slot;
    ApplyBodyRecordToEdits(hDlg, slot);
}

// ---------------------------------------------------------------------------
// 0x4204F0 (x64 sub_7FF7CB4B05C0) - pull the joint edits of the selected
// record into its scratch slot.  Both y-axis angular limits (edits 765/764)
// are validated against -90..90 with a message box and clamped to +-80,
// matching the Bullet 6DOF y-rotation restriction.
// ---------------------------------------------------------------------------
void CollectJointEdits(MMDApp* app) {  // was Sub4204F0, VA 0x004204F0
    if (app->state.selectedJointIndex == -1)
        return;
    Joint& jt = EditJoints()[app->state.selectedJointIndex];
    HWND hDlg = app->state.frameCopyDialog;
    char text[256];

    CollectJointName(hDlg, jt);

    ReadEditFloat(GetDlgItem(hDlg, kJointPosXEdit), jt.position[0]);
    ReadEditFloat(GetDlgItem(hDlg, kJointPosYEdit), jt.position[1]);
    ReadEditFloat(GetDlgItem(hDlg, kJointPosZEdit), jt.position[2]);
    ReadEditDegrees(GetDlgItem(hDlg, kJointRotXEdit), jt.rotation[0]);
    ReadEditDegrees(GetDlgItem(hDlg, kJointRotYEdit), jt.rotation[1]);
    ReadEditDegrees(GetDlgItem(hDlg, kJointRotZEdit), jt.rotation[2]);
    ReadEditFloat(GetDlgItem(hDlg, kLinUpperXEdit), jt.limits[0]);
    ReadEditFloat(GetDlgItem(hDlg, kLinUpperYEdit), jt.limits[1]);
    ReadEditFloat(GetDlgItem(hDlg, kLinUpperZEdit), jt.limits[2]);
    ReadEditFloat(GetDlgItem(hDlg, kLinLowerXEdit), jt.limits[3]);
    ReadEditFloat(GetDlgItem(hDlg, kLinLowerYEdit), jt.limits[4]);
    ReadEditFloat(GetDlgItem(hDlg, kLinLowerZEdit), jt.limits[5]);

    ReadEditDegrees(GetDlgItem(hDlg, kAngUpperXEdit), jt.limits[6]);
    {   // 765: angular y - validated
        GetWindowTextA(GetDlgItem(hDlg, kAngUpperYEdit), text, 10);
        float value = static_cast<float>(atof(text));
        value = CheckYAngleLimit(hDlg, GetDlgItem(hDlg, kAngUpperYEdit), value);
        jt.limits[7] = value / 180.0f * kPiShort;
    }
    ReadEditDegrees(GetDlgItem(hDlg, kAngUpperZEdit), jt.limits[8]);
    ReadEditDegrees(GetDlgItem(hDlg, kAngLowerXEdit), jt.limits[9]);
    {   // 764: angular y - validated
        GetWindowTextA(GetDlgItem(hDlg, kAngLowerYEdit), text, 10);
        float value = static_cast<float>(atof(text));
        value = CheckYAngleLimit(hDlg, GetDlgItem(hDlg, kAngLowerYEdit), value);
        jt.limits[10] = value / 180.0f * kPiShort;
    }
    ReadEditDegrees(GetDlgItem(hDlg, kAngLowerZEdit), jt.limits[11]);

    ReadEditFloat(GetDlgItem(hDlg, kSpringLinXEdit), jt.springs[0]);
    ReadEditFloat(GetDlgItem(hDlg, kSpringLinYEdit), jt.springs[1]);
    ReadEditFloat(GetDlgItem(hDlg, kSpringLinZEdit), jt.springs[2]);
    ReadEditFloat(GetDlgItem(hDlg, kSpringAngXEdit), jt.springs[3]);
    ReadEditFloat(GetDlgItem(hDlg, kSpringAngYEdit), jt.springs[4]);
    ReadEditFloat(GetDlgItem(hDlg, kSpringAngZEdit), jt.springs[5]);
}

// ---------------------------------------------------------------------------
// 0x43CA50 (x64 sub_7FF7CB4B1A30) - "add joint": refuses with a message box
// when no live body exists, else takes the first free joint slot, names it
// JOINT_<slot>, registers it in the joint list and zeroes every numeric
// field (both link indices point at the first surviving body).
// ---------------------------------------------------------------------------
void AddJoint(HWND hDlg) {  // was Sub43CA50, VA 0x0043CA50
    MMDApp* app = g_Block;
    Joint* joints = EditJoints();

    const int firstBody = BodySlotForCombo(0);
    if (firstBody < 0) {
        if (app->state.englishUI != 0) {
            LegacyUiMessageBoxA(hDlg, "There is no Body.\nPlease add body first.",
                        "add joint", 0);
        } else {
            LegacyUiMessageBoxA(hDlg, kMsgNoBodyJp, kCaptionAddJointJp, 0);
        }
        return;
    }

    int slot = 0;
    while (slot < kMaxEditRecords && joints[slot].constraint >= 0)
        ++slot;
    if (slot == kMaxEditRecords) return;

    Joint& jt = joints[slot];
    jt.jpText = nullptr;
    jt.enText = nullptr;
    sprintf_s(jt.name, sizeof jt.name, "JOINT_%d", slot);
    const std::wstring wideName = PhysicsNameWide(jt);
    const LRESULT comboIndex = SendMessageW(
        GetDlgItem(hDlg, kJointListCombo), CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(wideName.c_str()));
    jt.constraint = static_cast<int>(comboIndex);
    SendMessageA(GetDlgItem(hDlg, kJointListCombo), CB_SETCURSEL, comboIndex, 0);

    jt.rigidA = firstBody;
    jt.rigidB = firstBody;
    jt.position[0] = jt.position[1] = jt.position[2] = 0.0f;
    jt.rotation[0] = jt.rotation[1] = jt.rotation[2] = 0.0f;
    for (float& limit : jt.limits)
        limit = 0.0f;
    for (float& spring : jt.springs)
        spring = 0.0f;

    app->state.selectedJointIndex = slot;
    ApplyJointRecordToEdits(hDlg, slot);
}

// ---------------------------------------------------------------------------
// 0x421CE0 (x64 sub_7FF7CB4B3810) - shape-dependent UI: checks the
// sphere/box/capsule radio, relabels the size fields (radius / width x
// height x depth / radius x height), shows only the size edits the shape
// uses and refills them from the record.  idx == -1 clears the visible
// edits instead (no selection); the original would sprintf from slot -1
// for the non-sphere shapes, the port skips the refill in that case.
// ---------------------------------------------------------------------------
void UpdateShapeControls(HWND hDlg, int shape, int idx) {  // was Sub421CE0, VA 0x00421CE0
    MMDApp* app = g_Block;
    const unsigned char language = app->EnglishUI();
    const wchar_t* radius = language == kUiChinese ? L"半径"
        : language == kUiJapanese ? L"半径" : L"radius";
    // Preserve the original Japanese full-width space before the width label.
    const wchar_t* width = language == kUiChinese ? L"宽度"
        : language == kUiJapanese ? L"　幅" : L"width";
    const wchar_t* height = language == kUiChinese ? L"高度"
        : language == kUiJapanese ? L"高さ" : L"height";
    const wchar_t* depth = language == kUiChinese ? L"深度"
        : language == kUiJapanese ? L"奥行" : L"depth";
    Rigid* bodies = EditBodies();
    char text[256];

    if (shape == 0) {          // sphere: radius only
        CheckRadioButton(hDlg, kShapeSphereRadio, kShapeCapsuleRadio,
                         kShapeSphereRadio);
        SetWindowTextW(GetDlgItem(hDlg, kSizeXLabel), radius);
        ShowWindow(GetDlgItem(hDlg, kSizeYLabel), SW_HIDE);
        ShowWindow(GetDlgItem(hDlg, kSizeYEdit), SW_HIDE);
        ShowWindow(GetDlgItem(hDlg, kSizeZLabel), SW_HIDE);
        ShowWindow(GetDlgItem(hDlg, kSizeZEdit), SW_HIDE);
        if (idx != -1) {
            sprintf_s(text, sizeof text, "%3.2f", bodies[idx].size[0]);
            SetWindowTextA(GetDlgItem(hDlg, kSizeXEdit), text);
        } else {
            SetWindowTextA(GetDlgItem(hDlg, kSizeXEdit), "");
        }
        return;
    }
    if (shape == 1) {          // box: width x height x depth
        CheckRadioButton(hDlg, kShapeSphereRadio, kShapeCapsuleRadio,
                         kShapeBoxRadio);
        SetWindowTextW(GetDlgItem(hDlg, kSizeXLabel), width);
        SetWindowTextW(GetDlgItem(hDlg, kSizeYLabel), height);
        SetWindowTextW(GetDlgItem(hDlg, kSizeZLabel), depth);
        ShowWindow(GetDlgItem(hDlg, kSizeYLabel), SW_SHOW);
        ShowWindow(GetDlgItem(hDlg, kSizeYEdit), SW_SHOW);
        ShowWindow(GetDlgItem(hDlg, kSizeZLabel), SW_SHOW);
        ShowWindow(GetDlgItem(hDlg, kSizeZEdit), SW_SHOW);
        if (idx >= 0 && idx < kMaxEditRecords) {
            sprintf_s(text, sizeof text, "%3.2f", bodies[idx].size[0]);
            SetWindowTextA(GetDlgItem(hDlg, kSizeXEdit), text);
            sprintf_s(text, sizeof text, "%3.2f", bodies[idx].size[1]);
            SetWindowTextA(GetDlgItem(hDlg, kSizeYEdit), text);
            sprintf_s(text, sizeof text, "%3.2f", bodies[idx].size[2]);
            SetWindowTextA(GetDlgItem(hDlg, kSizeZEdit), text);
        }
        return;
    }
    // capsule: radius x height
    CheckRadioButton(hDlg, kShapeSphereRadio, kShapeCapsuleRadio,
                     kShapeCapsuleRadio);
    SetWindowTextW(GetDlgItem(hDlg, kSizeXLabel), radius);
    SetWindowTextW(GetDlgItem(hDlg, kSizeYLabel), height);
    ShowWindow(GetDlgItem(hDlg, kSizeYLabel), SW_SHOW);
    ShowWindow(GetDlgItem(hDlg, kSizeYEdit), SW_SHOW);
    ShowWindow(GetDlgItem(hDlg, kSizeZLabel), SW_HIDE);
    ShowWindow(GetDlgItem(hDlg, kSizeZEdit), SW_HIDE);
    if (idx >= 0 && idx < kMaxEditRecords) {
        sprintf_s(text, sizeof text, "%3.2f", bodies[idx].size[0]);
        SetWindowTextA(GetDlgItem(hDlg, kSizeXEdit), text);
        sprintf_s(text, sizeof text, "%3.2f", bodies[idx].size[1]);
        SetWindowTextA(GetDlgItem(hDlg, kSizeYEdit), text);
    }
}

// ---------------------------------------------------------------------------
// 0x421C20 (x64 sub_7FF7CB4B3720) - body/joint page flip: shows one page's
// control range (685..734 body, 735..799 joint) and hides the other.
// ---------------------------------------------------------------------------
void FlipPhysicsDialogPage(HWND hDlg, int bodyPage) {  // was Sub421C20, VA 0x00421C20
    if (bodyPage != 0) {
        for (int id = kJointPageFirst; id <= kJointPageLast; ++id)
            ShowWindow(GetDlgItem(hDlg, id), SW_HIDE);
        for (int id = kBodyPageFirst; id <= kBodyPageLast; ++id)
            ShowWindow(GetDlgItem(hDlg, id), SW_SHOW);
        s_jointPageVisible = 0;
    } else {
        for (int id = kBodyPageFirst; id <= kBodyPageLast; ++id)
            ShowWindow(GetDlgItem(hDlg, id), SW_HIDE);
        for (int id = kJointPageFirst; id <= kJointPageLast; ++id)
            ShowWindow(GetDlgItem(hDlg, id), SW_SHOW);
        s_jointPageVisible = 1;
    }
}

// ---------------------------------------------------------------------------
// 0x420DC0 (x64 sub_7FF7CB4B0FF0) - pivot-bone combo: copies the selected
// bone's model-space position into the joint position and refreshes the
// position edits.
// ---------------------------------------------------------------------------
void PickPivotBone(HWND hDlg) {  // was Sub420DC0, VA 0x00420DC0
    MMDApp* app = g_Block;
    mdl::ModelRecord* model = mdl::Mdl(app->SelectedModel());
    const int sel = static_cast<int>(
        SendMessageA(GetDlgItem(hDlg, kPivotBoneCombo), CB_GETCURSEL, 0, 0));
    Joint& jt = EditJoints()[app->state.selectedJointIndex];
    const mdl::BoneRecord& bone = model->boneTable[sel];
    jt.position[0] = bone.position[0];
    jt.position[1] = bone.position[1];
    jt.position[2] = bone.position[2];
    char text[256];
    for (int axis = 0; axis < 3; ++axis) {
        sprintf_s(text, sizeof text, "%5.4f", jt.position[axis]);
        SetWindowTextA(GetDlgItem(hDlg, kJointPosXEdit + axis), text);
    }
}

// ---------------------------------------------------------------------------
// 0x43CCD0 (x64 sub_7FF7CB4B2590) - push the selected body record into the
// body edits: name, related bone (combo index = bone + 1 for "none"),
// group, shape UI, position, rotation (rad -> deg), mass and the
// damping/restitution/friction quartet (%4.3f), the group-list text, and
// the physical/bone radio + bone-alignment checkbox.  idx == -1 disables
// and clears the whole body group instead.
// ---------------------------------------------------------------------------
void ApplyBodyRecordToEdits(HWND hDlg, int idx) {  // was Sub43CCD0, VA 0x0043CCD0
    MMDApp* app = g_Block;
    Rigid* bodies = EditBodies();
    char text[256];

    if (idx == -1) {
        for (int id = kBodyNameEdit; id <= kBoneAlignCheckbox; ++id)
            EnableWindow(GetDlgItem(hDlg, id), FALSE);
        SetWindowTextA(GetDlgItem(hDlg, kBodyNameEdit), "");
        SendMessageA(GetDlgItem(hDlg, kBoneCombo), CB_SETCURSEL, 1, 0);
        SendMessageA(GetDlgItem(hDlg, kGroupCombo), CB_SETCURSEL, 0, 0);
        UpdateShapeControls(hDlg, 0, -1);
        for (int id = kPosXEdit; id <= kFrictionEdit; ++id)
            SetWindowTextA(GetDlgItem(hDlg, id), "");
        CheckRadioButton(hDlg, kPhysicalRadio, kBoneFollowRadio,
                         kBoneFollowRadio);
        return;
    }

    const Rigid& rb = bodies[idx];
    for (int id = kBodyNameEdit; id <= kBoneAlignCheckbox; ++id)
        EnableWindow(GetDlgItem(hDlg, id), TRUE);
    SetPhysicsNameEdit(GetDlgItem(hDlg, kBodyNameEdit), rb);
    SendMessageA(GetDlgItem(hDlg, kBoneCombo), CB_SETCURSEL, rb.boneIndex + 1, 0);
    SendMessageA(GetDlgItem(hDlg, kGroupCombo), CB_SETCURSEL, rb.group, 0);
    UpdateShapeControls(hDlg, rb.shape, idx);

    for (int axis = 0; axis < 3; ++axis) {
        sprintf_s(text, sizeof text, "%3.2f", rb.position[axis]);
        SetWindowTextA(GetDlgItem(hDlg, kPosXEdit + axis), text);
        sprintf_s(text, sizeof text, "%3.2f",
                  rb.rotation[axis] / kPiShort * 180.0f);
        SetWindowTextA(GetDlgItem(hDlg, kRotXEdit + axis), text);
    }
    sprintf_s(text, sizeof text, "%4.3f", rb.mass);
    SetWindowTextA(GetDlgItem(hDlg, kMassEdit), text);
    sprintf_s(text, sizeof text, "%4.3f", rb.linearDamping);
    SetWindowTextA(GetDlgItem(hDlg, kLinDampEdit), text);
    sprintf_s(text, sizeof text, "%4.3f", rb.angularDamping);
    SetWindowTextA(GetDlgItem(hDlg, kAngDampEdit), text);
    sprintf_s(text, sizeof text, "%4.3f", rb.restitution);
    SetWindowTextA(GetDlgItem(hDlg, kRestitutionEdit), text);
    sprintf_s(text, sizeof text, "%4.3f", rb.friction);
    SetWindowTextA(GetDlgItem(hDlg, kFrictionEdit), text);

    // group list text: "1 5 9 " for every set collision-mask bit
    text[0] = '\0';
    char group[16];
    for (int bit = 0; bit < 16; ++bit) {
        sprintf_s(group, sizeof group, "%d ", bit + 1);
        if ((rb.noCollapse & (1u << bit)) != 0)
            strcat_s(text, sizeof text, group);
    }
    SetWindowTextA(GetDlgItem(hDlg, kGroupTextEdit), text);

    if (rb.mode != 0) {
        CheckRadioButton(hDlg, kPhysicalRadio, kBoneFollowRadio,
                         kPhysicalRadio);
        EnableWindow(GetDlgItem(hDlg, kBoneAlignCheckbox), TRUE);
    } else {
        CheckRadioButton(hDlg, kPhysicalRadio, kBoneFollowRadio,
                         kBoneFollowRadio);
        SendMessageA(GetDlgItem(hDlg, kBoneAlignCheckbox),
                     BM_SETCHECK, 0, 0);
        EnableWindow(GetDlgItem(hDlg, kBoneAlignCheckbox), FALSE);
    }
    SendMessageA(GetDlgItem(hDlg, kBoneAlignCheckbox), BM_SETCHECK,
                 rb.mode == 2 ? 1 : 0, 0);
}

// ---------------------------------------------------------------------------
// 0x4214A0 (x64 sub_7FF7CB4B2D90) - push the selected joint record into the
// joint edits: name, body-A/B combos (via their scratch combo indices),
// position (%5.4f), rotation (rad -> deg), the four limit triples and both
// spring triples (%3.2f).  idx == -1 disables and clears the joint group.
// ---------------------------------------------------------------------------
void ApplyJointRecordToEdits(HWND hDlg, int idx) {  // was Sub4214A0, VA 0x004214A0
    MMDApp* app = g_Block;
    Rigid* bodies = EditBodies();
    Joint* joints = EditJoints();
    char text[256];

    if (idx == -1) {
        for (int id = kDeleteJointButton; id <= kAngUpperZEdit; ++id)
            EnableWindow(GetDlgItem(hDlg, id), FALSE);
        SendMessageA(GetDlgItem(hDlg, kBodyACombo), CB_SETCURSEL, 0, 0);
        SendMessageA(GetDlgItem(hDlg, kBodyBCombo), CB_SETCURSEL, 0, 0);
        SendMessageA(GetDlgItem(hDlg, kPivotBoneCombo), CB_SETCURSEL, -1, 0);
        for (int id = kJointPosXEdit; id <= kAngUpperZEdit; ++id)
            SetWindowTextA(GetDlgItem(hDlg, id), "");
        return;
    }

    const Joint& jt = joints[idx];
    for (int id = kDeleteJointButton; id <= kAngUpperZEdit; ++id)
        EnableWindow(GetDlgItem(hDlg, id), TRUE);
    SetPhysicsNameEdit(GetDlgItem(hDlg, kJointNameEdit), jt);
    SendMessageA(GetDlgItem(hDlg, kBodyACombo), CB_SETCURSEL,
                 LiveBodySlot(jt.rigidA) ? BodyComboIndex(bodies[jt.rigidA]) : -1, 0);
    SendMessageA(GetDlgItem(hDlg, kBodyBCombo), CB_SETCURSEL,
                 LiveBodySlot(jt.rigidB) ? BodyComboIndex(bodies[jt.rigidB]) : -1, 0);

    for (int axis = 0; axis < 3; ++axis) {
        sprintf_s(text, sizeof text, "%5.4f", jt.position[axis]);
        SetWindowTextA(GetDlgItem(hDlg, kJointPosXEdit + axis), text);
        sprintf_s(text, sizeof text, "%3.2f",
                  jt.rotation[axis] / kPiShort * 180.0f);
        SetWindowTextA(GetDlgItem(hDlg, kJointRotXEdit + axis), text);
    }
    // linear upper (757/759/761), linear lower (747/758/760)
    const int kLinearUpperEdits[3] = {kLinUpperXEdit, kLinUpperYEdit,
                                      kLinUpperZEdit};
    const int kLinearLowerEdits[3] = {kLinLowerXEdit, kLinLowerYEdit,
                                      kLinLowerZEdit};
    for (int axis = 0; axis < 3; ++axis) {
        sprintf_s(text, sizeof text, "%3.2f", jt.limits[axis]);
        SetWindowTextA(GetDlgItem(hDlg, kLinearUpperEdits[axis]), text);
        sprintf_s(text, sizeof text, "%3.2f", jt.limits[3 + axis]);
        SetWindowTextA(GetDlgItem(hDlg, kLinearLowerEdits[axis]), text);
    }
    // angular upper (763/765/767), angular lower (762/764/766) - degrees
    const int kAngUpperEdits[3] = {kAngUpperXEdit, kAngUpperYEdit,
                                   kAngUpperZEdit};
    const int kAngLowerEdits[3] = {kAngLowerXEdit, kAngLowerYEdit,
                                   kAngLowerZEdit};
    for (int axis = 0; axis < 3; ++axis) {
        sprintf_s(text, sizeof text, "%3.2f",
                  jt.limits[6 + axis] / kPiShort * 180.0f);
        SetWindowTextA(GetDlgItem(hDlg, kAngUpperEdits[axis]), text);
        sprintf_s(text, sizeof text, "%3.2f",
                  jt.limits[9 + axis] / kPiShort * 180.0f);
        SetWindowTextA(GetDlgItem(hDlg, kAngLowerEdits[axis]), text);
    }
    // spring linear (748..750), spring angular (751..753)
    for (int axis = 0; axis < 3; ++axis) {
        sprintf_s(text, sizeof text, "%3.2f", jt.springs[axis]);
        SetWindowTextA(GetDlgItem(hDlg, kSpringLinXEdit + axis), text);
        sprintf_s(text, sizeof text, "%3.2f", jt.springs[3 + axis]);
        SetWindowTextA(GetDlgItem(hDlg, kSpringAngXEdit + axis), text);
    }
}

// ---------------------------------------------------------------------------
// 0x4220F0 (x64 sub_7FF7CB4B3D40) - commit on OK: drop the model's joints
// and rigid bodies out of the Bullet world, compact the live scratch
// records into fresh model tables, recreate every body (world matrix =
// Rz*Rx*Ry*T(position)*T(bonePos)*boneInitMat; inverse stored for the
// physics readback) and every joint (pivots through both bodies' inverse
// transforms, frame quaternions from joint-rotations composed with the
// negated body rotations), renumbering the scratch combo indices to the
// compacted order.
// ---------------------------------------------------------------------------
void CommitPhysicsEdits(HWND hDlg) {  // was Sub4220F0, VA 0x004220F0
    // Reject stale references before releasing any existing Bullet objects.
    if (!ValidatePhysicsEdits(hDlg)) return;
    MMDApp* app = g_Block;
    mdl::ModelRecord* model = mdl::Mdl(app->SelectedModel());
    PhysicsScene* scene = app->Physics();
    d3dx::Api& d3dx = d3dx::Get();
    Rigid* bodies = EditBodies();
    Joint* joints = EditJoints();
    using d3dx::D3DXMATRIXF;

    physics_scale::Forget(app->SelectedModel());
    bone_scale::RestoreTransforms(app->SelectedModel());

    // Scratch slots retain their source index until compaction. Surviving
    // unrenamed records transfer the original owned text pointers; discarded
    // or renamed slots must release theirs before the old tables disappear.
    // Cancel never enters this path and only frees the borrowed scratch arrays.
    // ---- tear the old physics objects out of the world --------------------
    for (std::uint32_t i = 0; i < model->jointCount; ++i) {
        Joint& old = model->jointTable[i];
        RemovePhysJointByUid(scene, old.constraint);
        const bool retained = joints[i].constraint >= 0;
        if (!retained || joints[i].jpText != old.jpText) std::free(old.jpText);
        if (!retained || joints[i].enText != old.enText) std::free(old.enText);
    }
    delete[] model->jointTable;
    model->jointTable = nullptr;
    for (std::uint32_t i = 0; i < model->rigidCount; ++i) {
        Rigid& old = model->rigidTable[i];
        RemovePhysRigidBody(scene, static_cast<btRigidBody*>(old.body));
        const bool retained = BodyComboIndex(bodies[i]) >= 0;
        if (!retained || bodies[i].jpText != old.jpText) std::free(old.jpText);
        if (!retained || bodies[i].enText != old.enText) std::free(old.enText);
    }
    delete[] model->rigidTable;
    model->rigidTable = nullptr;

    // ---- compacted rigid table --------------------------------------------
    int liveBodies = 0;
    for (int i = 0; i < kMaxEditRecords; ++i)
        if (BodyComboIndex(bodies[i]) >= 0)
            ++liveBodies;
    model->rigidCount = static_cast<std::uint32_t>(liveBodies);
    model->rigidTable = new Rigid[liveBodies > 0 ? liveBodies : 1];
    std::memset(model->rigidTable, 0,
                sizeof(Rigid) * static_cast<std::size_t>(liveBodies));

    // clear the bone "has dynamic rigid" flags before re-linking
    mdl::BoneRecord* bones = model->boneTable;
    for (std::uint32_t b = 0; b < model->boneCount; ++b)
        bones[b].hasRigidBody = 0;

    int bodyIndex = 0;
    for (int i = 0; i < kMaxEditRecords; ++i) {
        if (BodyComboIndex(bodies[i]) < 0)
            continue;
        Rigid& ed = bodies[i];
        SyncBodyMode(ed);
        // collision mask -> PMD no-collapse sense for storage
        ed.noCollapse = static_cast<std::uint16_t>(~ed.noCollapse);
        Rigid& dst = model->rigidTable[bodyIndex];
        std::memcpy(&dst, &ed, sizeof(Rigid));

        // world matrix Rz*Rx*Ry * T(position) * T(bonePos) * boneInitMat
        D3DXMATRIXF mat, tmp;
        d3dx.rotZ(&mat, ed.rotation[2]);
        d3dx.rotX(&tmp, ed.rotation[0]);
        d3dx.multiply(&mat, &mat, &tmp);
        d3dx.rotY(&tmp, ed.rotation[1]);
        d3dx.multiply(&mat, &mat, &tmp);
        d3dx.translation(&tmp, ed.position[0], ed.position[1],
                         ed.position[2]);
        d3dx.multiply(&mat, &mat, &tmp);
        // Deliberate hardening: this bone index is copied directly from the
        // model file; the original stores and uses it with no check other than
        // a negative test (verified at 0x1400B4A2C/0x1400B4A3F in the PMD loader
        // and 0x1400C39D2 in the physics writeback), so an index beyond the bone
        // table overwrites adjacent model state. The port clamps the bone
        // lookup to the model's bone count and skips the flag write for such a
        // record; the record itself is still compacted unchanged.
        // Defect B2-08.
        const bool boneIndexInRange =
            ed.boneIndex >= 0 &&
            static_cast<std::uint32_t>(ed.boneIndex) < model->boneCount;
        const mdl::BoneRecord& linkedBone =
            bones[boneIndexInRange ? ed.boneIndex : 0];
        d3dx.translation(&tmp, linkedBone.position[0],
                         linkedBone.position[1], linkedBone.position[2]);
        d3dx.multiply(&mat, &mat, &tmp);
        D3DXMATRIXF boneInit;
        std::memcpy(&boneInit, linkedBone.matInit, sizeof(boneInit));
        d3dx.multiply(&mat, &mat, &boneInit);

        void* bodyOut[2] = {nullptr, nullptr};
        CreateRigidBody(scene, bodyOut, ed.shape, ed.size[0], ed.size[1],
                        ed.size[2], &mat.m[0][0], ed.mode, ed.mass,
                        ed.linearDamping, ed.angularDamping, ed.restitution,
                        ed.friction, static_cast<char>(ed.group),
                        ed.noCollapse);
        dst.keyData = bodyOut[0];
        dst.body = bodyOut[1];
        SetBodyComboIndex(ed, bodyIndex);
        if (ed.mode > 0 && boneIndexInRange)
            bones[ed.boneIndex].hasRigidBody = 1;

        // inverse transform T(-bonePos)*T(-position)*Ry(-y)*Rx(-x)*Rz(-z)
        D3DXMATRIXF inv, t2;
        d3dx.translation(&inv, -linkedBone.position[0],
                         -linkedBone.position[1], -linkedBone.position[2]);
        d3dx.translation(&t2, -ed.position[0], -ed.position[1],
                         -ed.position[2]);
        d3dx.multiply(&inv, &inv, &t2);
        d3dx.rotY(&t2, -ed.rotation[1]);
        d3dx.multiply(&inv, &inv, &t2);
        d3dx.rotX(&t2, -ed.rotation[0]);
        d3dx.multiply(&inv, &inv, &t2);
        d3dx.rotZ(&t2, -ed.rotation[2]);
        d3dx.multiply(&inv, &inv, &t2);
        std::memcpy(dst.invTransform, &inv, sizeof(inv));

        ++bodyIndex;
    }

    // ---- compacted joint table --------------------------------------------
    int liveJoints = 0;
    for (int i = 0; i < kMaxEditRecords; ++i)
        if (joints[i].constraint >= 0)
            ++liveJoints;
    model->jointCount = static_cast<std::uint32_t>(liveJoints);
    model->jointTable = new Joint[liveJoints > 0 ? liveJoints : 1];
    std::memset(model->jointTable, 0,
                sizeof(Joint) * static_cast<std::size_t>(liveJoints));

    int jointIndex = 0;
    for (int i = 0; i < kMaxEditRecords; ++i) {
        if (joints[i].constraint < 0)
            continue;
        Joint& ed = joints[i];
        // relink through the renumbered body slots
        ed.rigidA = BodyComboIndex(bodies[ed.rigidA]);
        ed.rigidB = BodyComboIndex(bodies[ed.rigidB]);
        Joint& dst = model->jointTable[jointIndex];
        std::memcpy(&dst, &ed, sizeof(Joint));

        const Rigid& rbA = model->rigidTable[ed.rigidA];
        const Rigid& rbB = model->rigidTable[ed.rigidB];

        // joint anchors through both bodies' inverse transforms
        float pivotA[3], pivotB[3];
        TransformPoint(pivotA, ed.position, rbA.invTransform);
        TransformPoint(pivotB, ed.position, rbB.invTransform);
        const float distA = std::sqrt(pivotA[0] * pivotA[0] +
                                      pivotA[1] * pivotA[1] +
                                      pivotA[2] * pivotA[2]);
        const float distB = std::sqrt(pivotB[0] * pivotB[0] +
                                      pivotB[1] * pivotB[1] +
                                      pivotB[2] * pivotB[2]);
        dst.radiusBound = distA + distB;

        // limit radius: per-axis max of |linear upper| / |linear lower|
        float maxAbs[3];
        for (int axis = 0; axis < 3; ++axis) {
            float upper = std::fabs(ed.limits[axis]);
            const float lower = std::fabs(ed.limits[3 + axis]);
            if (lower > upper)
                upper = lower;
            maxAbs[axis] = upper;
        }
        dst.radiusBound += std::sqrt(maxAbs[0] * maxAbs[0] +
                                     maxAbs[1] * maxAbs[1] +
                                     maxAbs[2] * maxAbs[2]);

        // frame orientations: joint rotation composed with the negated
        // body rotation, one quaternion per side
        float qa[4], qb[4];
        for (int side = 0; side < 2; ++side) {
            const Rigid& rb = side == 0 ? rbA : rbB;
            D3DXMATRIXF rot, r2;
            d3dx.rotZ(&rot, ed.rotation[2]);
            d3dx.rotX(&r2, ed.rotation[0]);
            d3dx.multiply(&rot, &rot, &r2);
            d3dx.rotY(&r2, ed.rotation[1]);
            d3dx.multiply(&rot, &rot, &r2);
            d3dx.rotY(&r2, -rb.rotation[1]);
            d3dx.multiply(&rot, &rot, &r2);
            d3dx.rotX(&r2, -rb.rotation[0]);
            d3dx.multiply(&rot, &rot, &r2);
            d3dx.rotZ(&r2, -rb.rotation[2]);
            d3dx.multiply(&rot, &rot, &r2);
            if (side == 0)
                d3dx.quatFromMatrix(qa, &rot);
            else
                d3dx.quatFromMatrix(qb, &rot);
        }

        dst.constraint = CreatePhysJoint(
            scene, rbA.body, rbB.body, pivotA[0], pivotA[1], pivotA[2],
            qa[0], qa[1], qa[2], qa[3], pivotB[0], pivotB[1], pivotB[2],
            qb[0], qb[1], qb[2], qb[3], ed.limits[0], ed.limits[1],
            ed.limits[2], ed.limits[3], ed.limits[4], ed.limits[5],
            ed.limits[6], ed.limits[7], ed.limits[8], ed.limits[9],
            ed.limits[10], ed.limits[11], ed.springs[0], ed.springs[1],
            ed.springs[2], ed.springs[3], ed.springs[4], ed.springs[5]);
        ++jointIndex;
    }

    // The dialog suppresses world stepping while it is open. Request the
    // first settle pass after the rebuilt bodies become visible to the main
    // frame loop, including when physics is enabled but playback is paused.
    app->PhysicsResetPending() = 1;
}

// ---------------------------------------------------------------------------
// 0x465020 (x64 sub_7FF7CB4AD800) - the dialog proc.
// WM_INITDIALOG: subclass the name/float edits (705, 709..723, 740,
// 744..767) with PhysicsEditSubclassProc, saving 705's old proc, then InitPhysicsModelDialog init.
// WM_COMMAND (sets the cached-time dirty word first):
//   0x2AD/0x2DF   add body / add joint (collect current edits first)
//   0x2C2         delete body: drop it from the three body combos, drop
//                 every joint attached to it (with combo renumbering),
//                 mark the scratch slot free and renumber the combos
//   0x2E1         delete joint
//   0x2D4/0x2D5   body copy to / paste from the staging record
//   0x2E2/0x2E3   joint copy to / paste from the staging record
//   0x2D6..0x2D8  shape radio sphere/box/capsule (record shape byte)
//   0x2D9..0x2DB  physical radio / bone-follow radio / alignment checkbox:
//                 editor mode byte 0 = bone-follow, 1 = physical,
//                 2 = physical + bone alignment (kinematicFlag)
//   0x320/0x321   body / joint page flip
//   CBN_SELCHANGE of 704/736 (list selection), 741/742 (joint body A/B),
//   707/708 (related bone / group), 743 (pivot bone)
//   id 1 (OK): seek+re-eval (SeekSelectedModelToCurrentFrame), collect both records, commit
//   (CommitPhysicsEdits), free the scratch arrays, close, re-enable the main
//   combos 436/408, show the preserve-model box, set the enhanced-model
//   dirty flag
//   id 2 (Cancel): seek+re-eval, free, close, re-enable
// ---------------------------------------------------------------------------
INT_PTR CALLBACK PhysicsModelDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {  // was Sub465020, VA 0x00465020
    MMDApp* app = g_Block;
    if (msg == WM_INITDIALOG) {
        LocalizeChineseDialog(app, hDlg);
        if (app->state.floatingWindow != 0)
            SetWindowPos(hDlg, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOSIZE | SWP_NOMOVE);  // x64 0x7FF7CB4AE84E: or rdx,-1, flags 3
        HWND nameEdit = GetDlgItem(hDlg, kBodyNameEdit);
        app->FrameCopyEditProc() = reinterpret_cast<WNDPROC>(
            GetWindowLongPtrW(nameEdit, GWLP_WNDPROC));
        SetWindowLongPtrW(nameEdit, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(PhysicsEditSubclassProc));
        for (int id = 709; id <= 723; ++id)
            SetWindowLongPtrW(GetDlgItem(hDlg, id), GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(PhysicsEditSubclassProc));
        for (int id = kJointPosXEdit; id <= kAngUpperZEdit; ++id)
            SetWindowLongPtrW(GetDlgItem(hDlg, id), GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(PhysicsEditSubclassProc));
        SetWindowLongPtrW(GetDlgItem(hDlg, kJointNameEdit), GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(PhysicsEditSubclassProc));
        InitPhysicsModelDialog(hDlg);
        return 0;
    }
    if (msg != WM_COMMAND)
        return 0;

    // dirty write: the dialog deliberately invalidates the cached time
    app->state.timeNowHigh = 1;

    const HWND hCtrl = reinterpret_cast<HWND>(lParam);
    Rigid* bodies = EditBodies();
    Joint* joints = EditJoints();

    switch (LOWORD(wParam)) {
    case 0x2AD:  // add body
        CollectBodyEdits(app);
        AddRigidBody(hDlg);
        return 0;
    case 0x2DF:  // add joint
        CollectJointEdits(app);
        AddJoint(hDlg);
        return 0;
    case 0x2C2: {  // delete body (also deletes its joints)
        const int sel = static_cast<int>(
            SendMessageA(GetDlgItem(hDlg, kBodyListCombo), CB_GETCURSEL, 0, 0));
        const int deleted = BodySlotForCombo(sel);
        if (deleted < 0) return 0;
        SendMessageA(GetDlgItem(hDlg, kBodyListCombo), CB_DELETESTRING, sel, 0);
        SendMessageA(GetDlgItem(hDlg, kBodyACombo), CB_DELETESTRING, sel, 0);
        SendMessageA(GetDlgItem(hDlg, kBodyBCombo), CB_DELETESTRING, sel, 0);
        for (int i = 0; i < kMaxEditRecords; ++i) {
            if (joints[i].constraint >= 0 &&
                (joints[i].rigidA == deleted || joints[i].rigidB == deleted)) {
                SendMessageA(GetDlgItem(hDlg, kJointListCombo), CB_DELETESTRING,
                             joints[i].constraint, 0);
                for (int m = 0; m < kMaxEditRecords; ++m)
                    if (joints[m].constraint > joints[i].constraint)
                        --joints[m].constraint;
                joints[i].constraint = -1;
            }
        }
        SetBodyComboIndex(bodies[deleted], -1);
        for (int n = 0; n < kMaxEditRecords; ++n)
            if (BodyComboIndex(bodies[n]) > sel)
                SetBodyComboIndex(bodies[n], BodyComboIndex(bodies[n]) - 1);
        app->state.selectedRigidIndex = BodySlotForCombo(0);
        SendMessageA(GetDlgItem(hDlg, kBodyListCombo), CB_SETCURSEL, 0, 0);
        ApplyBodyRecordToEdits(hDlg, app->state.selectedRigidIndex);
        app->state.selectedJointIndex = JointSlotForCombo(0);
        SendMessageA(GetDlgItem(hDlg, kJointListCombo), CB_SETCURSEL, 0, 0);
        ApplyJointRecordToEdits(hDlg, app->state.selectedJointIndex);
        return 0;
    }
    case 0x2E1: {  // delete joint
        const int sel = static_cast<int>(
            SendMessageA(GetDlgItem(hDlg, kJointListCombo), CB_GETCURSEL, 0, 0));
        const int deleted = JointSlotForCombo(sel);
        if (deleted < 0) return 0;
        SendMessageA(GetDlgItem(hDlg, kJointListCombo), CB_DELETESTRING, sel, 0);
        joints[deleted].constraint = -1;
        for (int i = 0; i < kMaxEditRecords; ++i)
            if (joints[i].constraint > sel)
                --joints[i].constraint;
        app->state.selectedJointIndex = JointSlotForCombo(0);
        SendMessageA(GetDlgItem(hDlg, kJointListCombo), CB_SETCURSEL, 0, 0);
        ApplyJointRecordToEdits(hDlg, app->state.selectedJointIndex);
        return 0;
    }
    case 0x2D4:  // copy body
        CollectBodyEdits(app);
        std::memcpy(&s_bodyCopyScratch, &bodies[app->state.selectedRigidIndex],
                    sizeof(Rigid));
        return 0;
    case 0x2D5: {  // paste body (restitution/friction stay untouched, as
                   // in the original field list)
        Rigid& rb = bodies[app->state.selectedRigidIndex];
        const Rigid& src = s_bodyCopyScratch;
        rb.boneIndex = src.boneIndex;
        rb.mode = src.mode;
        SyncBodyMode(rb);
        rb.mass = src.mass;
        rb.linearDamping = src.linearDamping;
        rb.angularDamping = src.angularDamping;
        for (int axis = 0; axis < 3; ++axis) {
            rb.position[axis] = src.position[axis];
            rb.rotation[axis] = src.rotation[axis];
            rb.size[axis] = src.size[axis];
        }
        rb.shape = src.shape;
        ApplyBodyRecordToEdits(hDlg, app->state.selectedRigidIndex);
        return 0;
    }
    case 0x2E2:  // copy joint
        CollectJointEdits(app);
        std::memcpy(&s_jointCopyScratch, &joints[app->state.selectedJointIndex],
                    sizeof(Joint));
        return 0;
    case 0x2E3: {  // paste joint
        Joint& jt = joints[app->state.selectedJointIndex];
        const Joint& src = s_jointCopyScratch;
        // A copied body may have been deleted while this dialog stayed open.
        // Keep the destination's valid endpoint rather than reviving that slot.
        if (LiveBodySlot(src.rigidA)) jt.rigidA = src.rigidA;
        if (LiveBodySlot(src.rigidB)) jt.rigidB = src.rigidB;
        jt.position[0] = src.position[0];
        jt.position[1] = src.position[1];
        jt.position[2] = src.position[2];
        jt.rotation[0] = src.rotation[0];
        jt.rotation[1] = src.rotation[1];
        jt.rotation[2] = src.rotation[2];
        for (int f = 0; f < 12; ++f)
            jt.limits[f] = src.limits[f];
        for (int f = 0; f < 6; ++f)
            jt.springs[f] = src.springs[f];
        ApplyJointRecordToEdits(hDlg, app->state.selectedJointIndex);
        return 0;
    }
    case 0x2D6:  // shape: sphere
        bodies[app->state.selectedRigidIndex].shape = 0;
        UpdateShapeControls(hDlg, 0, app->state.selectedRigidIndex);
        return 0;
    case 0x2D7:  // shape: box
        bodies[app->state.selectedRigidIndex].shape = 1;
        UpdateShapeControls(hDlg, 1, app->state.selectedRigidIndex);
        return 0;
    case 0x2D8:  // shape: capsule
        bodies[app->state.selectedRigidIndex].shape = 2;
        UpdateShapeControls(hDlg, 2, app->state.selectedRigidIndex);
        return 0;
    case 0x2D9:  // physical radio clicked
        EnableWindow(GetDlgItem(hDlg, kBoneAlignCheckbox), TRUE);
        if (IsDlgButtonChecked(hDlg, kBoneAlignCheckbox) == 1) {
            bodies[app->state.selectedRigidIndex].mode = 2;
            bodies[app->state.selectedRigidIndex].kinematicFlag = 1;
        } else {
            bodies[app->state.selectedRigidIndex].mode = 1;
            bodies[app->state.selectedRigidIndex].kinematicFlag = 0;
        }
        SyncBodyMode(bodies[app->state.selectedRigidIndex]);
        return 0;
    case 0x2DA:  // bone-follow radio clicked
        EnableWindow(GetDlgItem(hDlg, kBoneAlignCheckbox), FALSE);
        bodies[app->state.selectedRigidIndex].mode = 0;
        SyncBodyMode(bodies[app->state.selectedRigidIndex]);
        return 0;
    case 0x2DB:  // bone-alignment checkbox toggled while physical
        if (bodies[app->state.selectedRigidIndex].mode == 2) {
            bodies[app->state.selectedRigidIndex].mode = 1;
            bodies[app->state.selectedRigidIndex].kinematicFlag = 0;
        } else if (bodies[app->state.selectedRigidIndex].mode == 1) {
            bodies[app->state.selectedRigidIndex].mode = 2;
            bodies[app->state.selectedRigidIndex].kinematicFlag = 1;
        }
        SyncBodyMode(bodies[app->state.selectedRigidIndex]);
        return 0;
    case 0x320:  // body page radio: collect the joint edits being left
        CollectJointEdits(app);
        FlipPhysicsDialogPage(hDlg, 1);
        return 0;
    case 0x321:  // joint page radio: collect the body edits being left
        CollectBodyEdits(app);
        FlipPhysicsDialogPage(hDlg, 0);
        return 0;
    default:
        break;
    }

    if (HIWORD(wParam) != 1 /*CBN_SELCHANGE*/) {
        if (LOWORD(wParam) == 1) {
            // OK: collect, rebuild bodies, then sync the model pose against
            // the newly committed physics records before returning to main.
            CollectBodyEdits(app);
            CollectJointEdits(app);
            if (!ValidatePhysicsEdits(hDlg)) return 1;
            CommitPhysicsEdits(hDlg);
            SeekSelectedModelToCurrentFrame(app);   // 0x4220C0 = x64 sub_7FF7CB4B3D00
            app->PhysicsResetPending() = 1;
            ReleasePhysicsEditScratch(app);
            DestroyWindow(hDlg);
            app->state.frameCopyDialog = nullptr;
            EnableWindow(GetDlgItem(app->state.hwnd, panel::kMainComboModel), TRUE);
            EnableWindow(GetDlgItem(app->state.hwnd, panel::kPlayButton), TRUE);
            if (app->state.englishUI != 0) {
                LegacyUiMessageBoxA(app->state.hwnd,
                            "Please preserve the edit result as a new model "
                            "by 'save enhanced model'.",
                            "enhance model", 0);
            } else {
                LegacyUiMessageBoxA(app->state.hwnd, kMsgEnhanceModelJp,
                            kCaptionEnhanceModelJp, 0);
            }
            app->EnhancedModelDirty() = 1;
            return 1;
        }
        if (LOWORD(wParam) == 2) {
            // Cancel: seek, close (the editor arrays are simply dropped)
            SeekSelectedModelToCurrentFrame(app);   // 0x4220C0 corresponds to x64 sub_7FF7CB4B3D00
            ReleasePhysicsEditScratch(app);
            DestroyWindow(hDlg);
            app->state.frameCopyDialog = nullptr;
            EnableWindow(GetDlgItem(app->state.hwnd, panel::kMainComboModel), TRUE);
            EnableWindow(GetDlgItem(app->state.hwnd, panel::kPlayButton), TRUE);
            return 0;
        }
        return 0;
    }

    if (hCtrl == GetDlgItem(hDlg, kBodyListCombo)) {
        // body list selection
        CollectBodyEdits(app);
        const int sel = static_cast<int>(
            SendMessageA(GetDlgItem(hDlg, kBodyListCombo), CB_GETCURSEL, 0, 0));
        app->state.selectedRigidIndex = BodySlotForCombo(sel);
        ApplyBodyRecordToEdits(hDlg, app->state.selectedRigidIndex);
        return 0;
    }
    if (hCtrl == GetDlgItem(hDlg, kJointListCombo)) {
        // joint list selection
        CollectJointEdits(app);
        const int sel = static_cast<int>(
            SendMessageA(GetDlgItem(hDlg, kJointListCombo), CB_GETCURSEL, 0, 0));
        app->state.selectedJointIndex = JointSlotForCombo(sel);
        ApplyJointRecordToEdits(hDlg, app->state.selectedJointIndex);
        return 0;
    }
    if (hCtrl == GetDlgItem(hDlg, kBodyACombo)) {
        // joint body-A pick: map the combo entry to its scratch slot
        const int sel = static_cast<int>(
            SendMessageA(GetDlgItem(hDlg, kBodyACombo), CB_GETCURSEL, 0, 0));
        for (int idx = 0; idx < kMaxEditRecords; ++idx) {
            if (BodyComboIndex(bodies[idx]) == sel)
                joints[app->state.selectedJointIndex].rigidA = idx;
        }
        return 0;
    }
    if (hCtrl == GetDlgItem(hDlg, kBodyBCombo)) {
        const int sel = static_cast<int>(
            SendMessageA(GetDlgItem(hDlg, kBodyBCombo), CB_GETCURSEL, 0, 0));
        for (int idx = 0; idx < kMaxEditRecords; ++idx) {
            if (BodyComboIndex(bodies[idx]) == sel)
                joints[app->state.selectedJointIndex].rigidB = idx;
        }
        return 0;
    }
    if (hCtrl == GetDlgItem(hDlg, kBoneCombo)) {
        // related bone: combo index 0 is "none" -> stored index -1
        bodies[app->state.selectedRigidIndex].boneIndex = static_cast<int>(
            SendMessageA(GetDlgItem(hDlg, kBoneCombo), CB_GETCURSEL, 0, 0) &
            0xFFFF) - 1;
        return 0;
    }
    if (hCtrl == GetDlgItem(hDlg, kGroupCombo)) {
        bodies[app->state.selectedRigidIndex].group = static_cast<std::uint8_t>(
            SendMessageA(GetDlgItem(hDlg, kGroupCombo), CB_GETCURSEL, 0, 0));
        return 0;
    }
    if (hCtrl == GetDlgItem(hDlg, kPivotBoneCombo)) {
        PickPivotBone(hDlg);
        return 0;
    }
    return 0;
}

}  // namespace mikudancestudio
