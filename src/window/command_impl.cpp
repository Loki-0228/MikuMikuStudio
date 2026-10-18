#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/ui_translation.hpp"
// ===========================================================================
// File 菜单八命令 + 保存 PMM：WM_COMMAND 202..206/209/210/213（及 208）
// ===========================================================================
// x64 sub_7FF7CB45F550 的 0xCA..0xD5 案（jumptable 0x7FF7CB45F5DD）一比一还原，
// 以 x64 为行为基准（x86 仅对意图）。共用骨架：
//   * 入口写 dialogFlags[i]=1 / enterKeyState=1（x64 dword [app+0x34+4i] /
//     [app+0xC0]，索引经 221/233/248 三案校准：0x34+4*12=0x64、0x34+4*17=0x78）。
//   * SetCurrentDirectoryW(exe 目录 x64 app+0xA16EE) -> swprintf_s(空路径)
//     -> OPENFILENAMEW（filter/标题用原串，nMaxFile=nMaxFileTitle=0x100，
//     lpstrFileTitle 指向栈缓冲）。
//   * lpstrInitialDir：菜单 0x12D（GetMenuState&8，"记录打开过的文件目录"）
//     勾选时用 per-type 目录字段，否则用 "UserFile\<Type>" 字面量。
//   * 确定后目录回写仅在 0x12D 勾选时（路径拷入 pathWorkspace 的
//     projectDirectory 暂存 -> ExtractDirFromPath 剥文件名 -> 拷回目录字段，
//     x64 0x7FF7CB429B80 内联对）。
//   * 打开对话框 Flags=0x1000（仅 FILEMUSTEXIST）、owner=浮动窗(x64
//     app+0xA1DE0)否则主窗；保存对话框 Flags=6、nFilterIndex=1、owner=主窗。
//   * 尾部写集各案不同（详见各函数内注释）：载入 VPD 只置 reseat 待处理
//     （x64 0x9FCC1），载入 VMD 置脏+reseat，载入 WAV/AVI 置脏；保存
//     VPD/VMD、打开/保存 PMM 之后无任何标志写。
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commdlg.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/accessory_motion_io.hpp"
#include "mikudancestudio/ui_message.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/bone_key_pool.hpp"

namespace mikudancestudio {
namespace {

// 主窗口句柄（x64 app+0xA16C8）。文件内部使用；command_file_menu.cpp 另有
// 同名非静态定义，故此处保持内部链接。
HWND MainHwnd(MMDApp* app) {
    return static_cast<HWND>(app->Hwnd());
}

// 模型帧表的"已分配"扫描（保存 VMD 的选中帧点门用）。
template <typename Key>
bool AnyAllocatedKey(const Key* keys, std::size_t count) {
    if (keys == nullptr)
        return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (keys[i].allocated != 0)
            return true;
    }
    return false;
}

// 0x12D 菜单项："记录打开过的文件目录"。勾选（MF_CHECKED=8）时各文件
// 对话框以 per-type 目录为初始目录，并在确定后回写该目录。
bool RememberFileDialogDirs(MMDApp* app) {
    return (GetMenuState(GetMenu(MainHwnd(app)), 0x12D, 0) & 8) != 0;
}

// -----------------------------------------------------------------------
// 日文文案，与 x64 .rdata 字节一致（Shift-JIS）。
// -----------------------------------------------------------------------
// x64 0x7FF7CB54E790: "ポーズデータ保存"（保存 VPD 选骨提示标题）
const char kCaptionSavePoseJp[] =
    "\x83\x7C\x81\x5B\x83\x59\x83\x66\x81\x5B\x83\x5E"  // ポーズデータ
    "\x95\xDB\x91\xB6";                                  // 保存
// x64 0x7FF7CB54E7B0: "ポーズデータは選択されたボーンのみがセーブされます\n
//                      ボーンの一部、もしくは全てを選択して下さい"
const char kMsgSavePoseJp[] =
    "\x83\x7C\x81\x5B\x83\x59\x83\x66\x81\x5B\x83\x5E\x82\xCD\x91\x49\x91\xF0"
    "\x82\xB3\x82\xEA\x82\xBD\x83\x7B\x81\x5B\x83\x93\x82\xCC\x82\xDD"
    "\x82\xAA\x83\x5A\x81\x5B\x83\x75\x82\xB3\x82\xEA\x82\xDC\x82\xB7\n"
    "\x83\x7B\x81\x5B\x83\x93\x82\xCC\x88\xEA\x95\x94\x81\x41\x82\xE0"
    "\x82\xB5\x82\xAD\x82\xCD\x91\x53\x82\xC4\x82\xF0\x91\x49\x91\xF0"
    "\x82\xB5\x82\xC4\x82\xAD\x82\xBE\x82\xB3\x82\xA2";
// x64 0x7FF7CB54E970: "モーションデータ保存"（保存 VMD 选帧提示标题）
const char kCaptionSaveMotionJp[] =
    "\x83\x82\x81\x5B\x83\x56\x83\x87\x83\x93\x83\x66\x81\x5B\x83\x5E"  // モーションデータ
    "\x95\xDB\x91\xB6";                                                 // 保存
// x64 0x7FF7CB54E990: "モーションデータは選択されたフレームポイントが
//                      セーブされます\nフレームの一部、もしくは全てを
//                      選択して下さい"
const char kMsgSaveMotionJp[] =
    "\x83\x82\x81\x5B\x83\x56\x83\x87\x83\x93\x83\x66\x81\x5B\x83\x5E"
    "\x82\xCD\x91\x49\x91\xF0\x82\xB3\x82\xEA\x82\xBD"
    "\x83\x74\x83\x8C\x81\x5B\x83\x80\x83\x7C\x83\x43\x83\x93\x83\x67"
    "\x82\xAA\x83\x5A\x81\x5B\x83\x75\x82\xB3\x82\xEA\x82\xDC\x82\xB7\n"
    "\x83\x74\x83\x8C\x81\x5B\x83\x80\x82\xCC\x88\xEA\x95\x94\x81\x41"
    "\x82\xE0\x82\xB5\x82\xAD\x82\xCD\x91\x53\x82\xC4\x82\xF0\x91\x49"
    "\x91\xF0\x82\xB5\x82\xC4\x82\xAD\x82\xBE\x82\xB3\x82\xA2";
// x64 0x7FF7CB54ED60: "新規作成"（File: New 确认框 JP 标题）
const char kCaptionNewJp[] =
    "\x90\x56\x8B\x4B\x8D\xEC\x90\xAC";
// x64 0x7FF7CB54EFF0: "現在の状態は破棄されます\n\nよろしいですか？"
const char kMsgNewJp[] =
    "\x8C\xBB\x8D\xDD\x82\xCC\x8F\xF3\x91\xD4\x82\xCD\x94\x6A\x8A\xFC"
    "\x82\xB3\x82\xEA\x82\xDC\x82\xB7\n\n"
    "\x82\xE6\x82\xEB\x82\xB5\x82\xA2\x82\xC5\x82\xB7\x82\xA9\x81\x48";
// x64 0x7FF7CB54F068: "ファイルを開く"（打开 PMM 脏场景确认 JP 标题）
const char kCaptionOpenFileJp[] =
    "\x83\x74\x83\x40\x83\x43\x83\x8B\x82\xF0\x8A\x4A\x82\xAD";
// x64 0x7FF7CB54F080: "保存していない変更点があります\n\nこのままロー
//                      ドしてよろしいですか？"
const char kMsgOpenDirtyJp[] =
    "\x95\xDB\x91\xB6\x82\xB5\x82\xC4\x82\xA2\x82\xC8\x82\xA2\x95\xCF\x8D\x58"
    "\x93\x5F\x82\xAA\x82\xA0\x82\xE8\x82\xDC\x82\xB7\n\n"
    "\x82\xB1\x82\xCC\x82\xDC\x82\xDC\x83\x8D\x81\x5B\x83\x68\x82\xB5"
    "\x82\xC4\x82\xE6\x82\xEB\x82\xB5\x82\xA2\x82\xC5\x82\xB7\x82\xA9"
    "\x81\x48";

// -----------------------------------------------------------------------
// 对话框标题（宽字符，OPENFILENAMEW.lpstrTitle）。日文串与 x64 .rdata
// 的 UTF-16 字节一致；x64 原串就是"読込"缩略形（无 を/み/む）。
// -----------------------------------------------------------------------
// x64 0x7FF7CB54E8E8: L"ポーズデータ読込"（载入 VPD）
const wchar_t kTitleLoadPoseJp[] =
    L"\x30DD\x30FC\x30BA\x30C7\x30FC\x30BF\x8AAD\x8FBC";
// x64 0x7FF7CB54E8B0: L"ポーズデータ保存"（保存 VPD）
const wchar_t kTitleSavePoseJp[] =
    L"\x30DD\x30FC\x30BA\x30C7\x30FC\x30BF\x4FDD\x5B58";
// x64 0x7FF7CB54EAF0: L"モーションデータ読込"（载入 VMD）
const wchar_t kTitleLoadMotionJp[] =
    L"\x30E2\x30FC\x30B7\x30E7\x30F3\x30C7\x30FC\x30BF\x8AAD\x8FBC";
// x64 0x7FF7CB54EAB0: L"モーションデータ保存"（保存 VMD）
const wchar_t kTitleSaveMotionJp[] =
    L"\x30E2\x30FC\x30B7\x30E7\x30F3\x30C7\x30FC\x30BF\x4FDD\x5B58";
// x64 0x7FF7CB54D840: L"ファイルを読込"（打开 PMM）
const wchar_t kTitleOpenFileJp[] =
    L"\x30D5\x30A1\x30A4\x30EB\x3092\x8AAD\x8FBC";
// x64 0x7FF7CB54F188: L"ファイルを保存する"（保存 PMM）
const wchar_t kTitleSaveFileJp[] =
    L"\x30D5\x30A1\x30A4\x30EB\x3092\x4FDD\x5B58\x3059\x308B";
// x64 aWaveFile / aAvi_0：WAV/AVI 的 JP 标题就是无修饰的 ASCII 串
const wchar_t kTitleOpenWaveJp[] = L"WAVE File";
const wchar_t kTitleLoadAviJp[] = L"AVI";

// x64 0x7FF7CB54A3BC: swprintf_s 格式 L"\0\0%s%s" —— 前导 NUL 使不带实参
// 的调用写出空串（各文件命令的路径缓冲清零惯用法）。
const wchar_t kFmtEmptyPath[] = L"\x0\x0%s%s";

// 文件对话框通用尾参：nMaxFile/nMaxFileTitle 均为 0x100。
constexpr DWORD kDialogPathLen = 0x100;

}  // namespace

// ---------------------------------------------------------------------------
// 202 (0xCA, 入口 0x7FF7CB46BCB5): File: 载入 VPD 姿势。
//   [dialogFlags[12]]=1; enterKeyState=1 -> 相机/配件模式门（x64 app+0x328
//   非 0 直接退出）-> 打开对话框 -> 载入。尾部只置 reseat 待处理
//   （0x9FCC1，泵内 3 次重置刚性体消费）——不置脏。
// ---------------------------------------------------------------------------
void CmdLoadPose(MMDApp* app) {
    app->state.dialogFlags[12] = 1;             // 0x46BCB5 [app+0x64]
    app->state.enterKeyState = 1;               // 0x46BCBC [app+0xC0]
    if (app->state.optflag[0] != 0) {           // 0x46BCC6 门：相机/配件模式不弹框
        return;
    }
    SetCurrentDirectoryW(app->ExeDir());        // 0xA16EE
    wchar_t path[kDialogPathLen];
    swprintf_s(path, kDialogPathLen, kFmtEmptyPath, L"", L"");
    wchar_t fileTitle[kDialogPathLen];
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app->FloatingWindow() != nullptr  // 0xA1DE0 否则主窗
                        ? app->FloatingWindow()
                        : MainHwnd(app);
    ofn.lpstrFilter = L"Vocaloid Pose Data files(*.vpd)\0*.vpd\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = kDialogPathLen;
    ofn.Flags = OFN_FILEMUSTEXIST;              // 0x1000
    ofn.lpstrInitialDir = RememberFileDialogDirs(app)
                              ? app->DirPose()  // 0xA3D5C
                              : L"UserFile\\Pose";
    ofn.lpstrDefExt = L"vpd";
    ofn.nMaxFileTitle = kDialogPathLen;
    ofn.lpstrFileTitle = fileTitle;
    ofn.lpstrTitle = app->EnglishUI() != 0
                         ? L"load pose data"
                         : kTitleLoadPoseJp;
    if (!GetUiOpenFileNameW(&ofn)) {
        return;
    }
    if (RememberFileDialogDirs(app)) {          // 目录回写仅在 0x12D 勾选时
        wchar_t* dir = ExtractDirFromPath(
            app->PathWorkspace().projectDirectory, path, kPathFieldCapacity);
        CopyDirPathW(app->DirPose(), dir);      // 0x42AE20
    }
    LoadVpdFile(path);                          // 0x46BE46 -> 0x418A10
    app->PhysicsResetPending() = 1;             // 0x469B0D [0x9FCC1]，不置脏
}

// ---------------------------------------------------------------------------
// 203 (0xCB, 入口 0x7FF7CB46BAAA): File: 保存 VPD 姿势。
//   相机/配件模式门在最前（0x46BAB1）-> 扫描当前模型选骨位图（模型+0x3120，
//   个数 +0x3110）-> enterKeyState=1 -> 无选骨则 0x40000 提示退出；否则保存
//   对话框（owner=主窗，nFilterIndex=1，Flags=6）。SaveVpdFile 之后无任何
//   标志写（不置脏）。
// ---------------------------------------------------------------------------
void CmdSavePose(MMDApp* app) {
    if (app->state.optflag[0] != 0) {           // 0x46BAAA 门在最前
        return;
    }
    // 选骨扫描：活动模型（x64 app+0xBE8 槽表、槽号 +0x13E0）的选骨位图
    // 逐字节非 0 即有选骨。
    bool anySelected = false;
    unsigned char* const model = app->SelectedModel();
    if (model != nullptr) {
        const int count = static_cast<int>(mdl::Mdl(model)->boneCount);
        unsigned char* const selBits = mdl::Mdl(model)->boneSelection;
        if (selBits != nullptr) {
            for (int b = 0; b < count; ++b) {
                if (selBits[b] != 0) {
                    anySelected = true;
                    break;
                }
            }
        }
    }
    app->state.enterKeyState = 1;               // 0x46BAF4 [app+0xC0]
    if (!anySelected) {
        ShowUiMessage(app, MainHwnd(app), UiMessage::SavePoseSelection, MB_TOPMOST);
        return;
    }
    SetCurrentDirectoryW(app->ExeDir());
    wchar_t path[kDialogPathLen];
    swprintf_s(path, kDialogPathLen, kFmtEmptyPath, L"", L"");
    wchar_t fileTitle[kDialogPathLen];
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = MainHwnd(app);              // 保存框 owner 恒为主窗
    ofn.lpstrFilter = L"Vocaloid Pose Data files(*.vpd)\0*.vpd\0";
    ofn.lpstrFile = path;
    ofn.nFilterIndex = 1;
    ofn.nMaxFile = kDialogPathLen;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;  // 6
    ofn.lpstrInitialDir = RememberFileDialogDirs(app)
                              ? app->DirPose()
                              : L"UserFile\\Pose";
    ofn.lpstrDefExt = L"vpd";
    ofn.nMaxFileTitle = kDialogPathLen;
    ofn.lpstrFileTitle = fileTitle;
    ofn.lpstrTitle = app->EnglishUI() != 0
                         ? L"save pose data"
                         : kTitleSavePoseJp;
    if (!GetUiSaveFileNameW(&ofn)) {
        return;
    }
    if (RememberFileDialogDirs(app)) {
        wchar_t* dir = ExtractDirFromPath(
            app->PathWorkspace().projectDirectory, path, kPathFieldCapacity);
        CopyDirPathW(app->DirPose(), dir);
    }
    SaveVpdFile(path);                          // 0x46BCAB -> 0x418750，无标志写
}

// ---------------------------------------------------------------------------
// 204 (0xCC, 入口 0x7FF7CB46DE02): File: New。
//   enterKeyState=1 -> MessageBox(主窗, 0x40001)：
//     EN  标题 "new"（0x7FF7CB54EFB0）/ 正文 "The existing state will be
//         annulled.\n\nAre you OK?"
//     JP  标题 "新規作成"（0x54ED60）/ 正文 0x54EFF0
//   ==IDOK 才走 ResetAppState；之后无标志写。
// ---------------------------------------------------------------------------
void CmdResetState(MMDApp* app) {
    app->state.enterKeyState = 1;               // 0x46DE02 [app+0xC0]
    const int r = ShowUiMessage(app, MainHwnd(app), UiMessage::NewScene,
                                MB_OKCANCEL | MB_TOPMOST);
    if (r == IDOK) {
        ResetAppState(app);                     // 0x46DE52 -> 0x44E540
    }
}

// ---------------------------------------------------------------------------
// 205 (0xCD, 入口 0x7FF7CB46DE5C): File: 打开 PMM 场景。
//   enterKeyState=1 -> 脏场景门（0xA1B31 非 0 时 0x40001 确认，EN 标题
//   "open file data"/JP 0x54F068，!=IDOK 退出）-> 打开对话框（owner=浮动窗
//   否则主窗，per-type 目录 DirUser/0xA25EC 或 "UserFile"，defExt "pmm"）
//   -> 确定后回写目录、整路径存 EnvFileName、LoadSceneFile。
//   LoadSceneFile 之后派发层无任何标志写（不置脏）。
// ---------------------------------------------------------------------------
void CmdOpenScene(MMDApp* app) {
    app->state.enterKeyState = 1;               // 0x46DE5C [app+0xC0]
    if (app->SceneModified() != 0) {            // 0x46DE66 脏场景确认
        const int r = ShowUiMessage(app, MainHwnd(app), UiMessage::OpenUnsaved,
                                    MB_OKCANCEL | MB_TOPMOST);
        if (r != IDOK) {
            return;
        }
    }
    SetCurrentDirectoryW(app->ExeDir());
    wchar_t path[kDialogPathLen];
    swprintf_s(path, kDialogPathLen, kFmtEmptyPath, L"", L"");
    wchar_t fileTitle[kDialogPathLen];
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app->FloatingWindow() != nullptr
                        ? app->FloatingWindow()
                        : MainHwnd(app);
    ofn.lpstrFilter = L"PolygonMovieMaker files(*.pmm)\0*.pmm\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = kDialogPathLen;
    ofn.Flags = OFN_FILEMUSTEXIST;              // 0x1000
    ofn.lpstrInitialDir = RememberFileDialogDirs(app)
                              ? app->DirUser()  // 0xA25EC
                              : L"UserFile";
    ofn.lpstrDefExt = L"pmm";
    ofn.nMaxFileTitle = kDialogPathLen;
    ofn.lpstrFileTitle = fileTitle;
    ofn.lpstrTitle = app->EnglishUI() != 0
                         ? L"open file data"
                         : kTitleOpenFileJp;
    if (!GetUiOpenFileNameW(&ofn)) {
        return;
    }
    if (RememberFileDialogDirs(app)) {
        wchar_t directory[kPathFieldCapacity]{};
        wchar_t* dir = ExtractDirFromPath(directory, path, kPathFieldCapacity);
        CopyDirPathW(app->DirUser(), dir);
    }
    LoadSceneFile(path);
}

// ---------------------------------------------------------------------------
// 206 (0xCE, 入口 0x7FF7CB46B8BC): File: 打开 WAV。
//   enterKeyState=1 -> 打开对话框 -> 先整路径入 wavPath、再回写
//   DirWave（0xA452C）-> LoadWaveFile -> 置脏。不置 reseat。
//   音频设备在实际导入后创建；取消对话框无需初始化 DirectSound。
// ---------------------------------------------------------------------------
void CmdOpenWave(MMDApp* app) {
    app->state.enterKeyState = 1;               // 0x46B8BC [app+0xC0]
    SetCurrentDirectoryW(app->ExeDir());
    wchar_t path[kDialogPathLen];
    swprintf_s(path, kDialogPathLen, kFmtEmptyPath, L"", L"");
    wchar_t fileTitle[kDialogPathLen];
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app->FloatingWindow() != nullptr
                        ? app->FloatingWindow()
                        : MainHwnd(app);
#if MMD_ORIGINAL_PROFILE
    ofn.lpstrFilter = L"WAV files (*.wav)\0*.wav\0";
#else
    ofn.lpstrFilter = L"Audio (*.wav;*.mp3;*.aac;*.adts;*.m4a;*.wma)\0*.wav;*.mp3;*.aac;*.adts;*.m4a;*.wma\0";
#endif
    ofn.lpstrFile = path;
    ofn.nMaxFile = kDialogPathLen;
    ofn.Flags = OFN_FILEMUSTEXIST;              // 0x1000
    ofn.lpstrInitialDir = RememberFileDialogDirs(app)
                              ? app->DirWave()  // 0xA452C
                              : L"UserFile\\Wave";
    ofn.lpstrDefExt = L"wav";
    ofn.nMaxFileTitle = kDialogPathLen;
    ofn.lpstrFileTitle = fileTitle;
    ofn.lpstrTitle = app->EnglishUI() != 0
                         ? L"Open audio"
                         : L"音声ファイルを開く";
    if (!GetUiOpenFileNameW(&ofn)) {
        return;
    }
    wcscpy_s(app->WavePath(), kDialogPathLen, path);  // 0x46BA13 -> app+0xD8
    if (RememberFileDialogDirs(app)) {
        wchar_t* dir = ExtractDirFromPath(
            app->PathWorkspace().projectDirectory, path, kPathFieldCapacity);
        CopyDirPathW(app->DirWave(), dir);
    }
    LoadWaveFile(app);                          // 0x46BA99 -> 0x418500
    app->SceneModified() = 1;                   // 0x46BA9E [0xA1B31]
}

// ---------------------------------------------------------------------------
// 208 (0xD0, 入口 0x7FF7CB46E05F): File: 另存 PMM 场景（0xCF 快存无名时
//   也落入本案）。
//   [dialogFlags[5]]=1; enterKeyState=1 -> 保存对话框（owner=主窗，
//   nFilterIndex=1，Flags=6，DirUser/0xA25EC 或 "UserFile"，defExt "pmm"，
//   EN 标题 "save file"/JP 0x54F188）-> 确定后回写目录、整路径存
//   EnvFileName、SaveSceneFile。派发层无任何标志写（不清脏——脏标志由
//   SaveSceneFile 内部处理）。
// ---------------------------------------------------------------------------
void CmdSaveScene(MMDApp* app) {
    app->state.dialogFlags[5] = 1;              // 0x46E05F [app+0x48]
    app->state.enterKeyState = 1;               // 0x46E066 [app+0xC0]
    SetCurrentDirectoryW(app->ExeDir());
    wchar_t path[kDialogPathLen];
    swprintf_s(path, kDialogPathLen, kFmtEmptyPath, L"", L"");
    wchar_t fileTitle[kDialogPathLen];
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = MainHwnd(app);              // 保存框 owner 恒为主窗
    ofn.lpstrFilter = L"PolygonMovieMaker files(*.pmm)\0*.pmm\0";
    ofn.lpstrFile = path;
    ofn.nFilterIndex = 1;
    ofn.nMaxFile = kDialogPathLen;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;  // 6
    ofn.lpstrInitialDir = RememberFileDialogDirs(app)
                              ? app->DirUser()
                              : L"UserFile";
    ofn.lpstrDefExt = L"pmm";
    ofn.nMaxFileTitle = kDialogPathLen;
    ofn.lpstrFileTitle = fileTitle;
    ofn.lpstrTitle = app->EnglishUI() != 0
                         ? L"save file"
                         : kTitleSaveFileJp;
    if (!GetUiSaveFileNameW(&ofn)) {
        return;
    }
    if (RememberFileDialogDirs(app)) {
        wchar_t* dir = ExtractDirFromPath(
            app->PathWorkspace().projectDirectory, path, kPathFieldCapacity);
        CopyDirPathW(app->DirUser(), dir);
    }
    wcscpy_s(app->EnvFileName(), kDialogPathLen, path);  // 0x46E1CE
    SaveSceneFile(app);                         // 0x46E1EA -> 0x4950A0，无标志写
}

// ---------------------------------------------------------------------------
// 209 (0xD1, 入口 0x7FF7CB46C11E): File: 载入 VMD 动作。
//   enterKeyState=1（无模式门）-> 打开对话框（owner=浮动窗否则主窗，
//   DirMotion/0xA358C 或 "UserFile\Motion"，defExt "vmd"）-> 载入。
//   尾部先置脏（0x46C2A0 [0xA1B31]）再置 reseat（0x469B0D [0x9FCC1]）。
// ---------------------------------------------------------------------------
void CmdLoadMotion(MMDApp* app) {
    app->state.enterKeyState = 1;               // 0x46C11E [app+0xC0]
    SetCurrentDirectoryW(app->ExeDir());
    wchar_t path[kDialogPathLen];
    swprintf_s(path, kDialogPathLen, kFmtEmptyPath, L"", L"");
    wchar_t fileTitle[kDialogPathLen];
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app->FloatingWindow() != nullptr
                        ? app->FloatingWindow()
                        : MainHwnd(app);
    ofn.lpstrFilter = L"Vocaloid Motion Data files(*.vmd)\0*.vmd\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = kDialogPathLen;
    ofn.Flags = OFN_FILEMUSTEXIST;              // 0x1000
    ofn.lpstrInitialDir = RememberFileDialogDirs(app)
                              ? app->DirMotion()  // 0xA358C
                              : L"UserFile\\Motion";
    ofn.lpstrDefExt = L"vmd";
    ofn.nMaxFileTitle = kDialogPathLen;
    ofn.lpstrFileTitle = fileTitle;
    ofn.lpstrTitle = app->EnglishUI() != 0
                         ? L"load motion data"
                         : kTitleLoadMotionJp;
    if (!GetUiOpenFileNameW(&ofn)) {
        return;
    }
    if (RememberFileDialogDirs(app)) {
        wchar_t* dir = ExtractDirFromPath(
            app->PathWorkspace().projectDirectory, path, kPathFieldCapacity);
        CopyDirPathW(app->DirMotion(), dir);
    }
    LoadVmdFile(path);                          // 0x46C29B -> 0x434B60
    app->SceneModified() = 1;                   // 0x46C2A0 [0xA1B31]
    app->PhysicsResetPending() = 1;             // 0x469B0D [0x9FCC1]
}

// ---------------------------------------------------------------------------
// 210 (0xD2, 入口 0x7FF7CB46BE50): File: 保存 VMD 动作。
//   [dialogFlags[17]]=1; enterKeyState=1 -> 有选中帧点门：
//     * 相机/配件模式（optflag[0] 非 0）：扫相机（+0x48/0x54 步长，1 万）、
//       照明（+0x24/0x28，1 万）、自影（+0x14/0x18，1 万）三张 app 表，
//       自影命中直接进对话框；
//     * 模型模式：扫活动模型骨（+0x38/0x3C 步长）、表情（+0x10/0x14，上限
//       20000）、显示帧（+0x18/0x28，上限 1000）三张表。
//   无选中帧点则 0x40000 提示退出；否则保存对话框（owner=主窗，
//   nFilterIndex=1，Flags=6，DirMotion 或 "UserFile\Motion"，defExt
//   "vmd"）。SaveVmdFile 之后无任何标志写（不置脏）。
// ---------------------------------------------------------------------------
void CmdSaveMotion(MMDApp* app) {
    app->state.dialogFlags[17] = 1;             // 0x46BE50 [app+0x78]
    app->state.enterKeyState = 1;               // 0x46BE57 [app+0xC0]
    // 选中帧点扫描（0x46BE61..0x46BF64）。
    auto anySelectedByte = [](unsigned char* table, std::size_t off,
                              std::size_t stride, int cap) -> bool {
        if (table == nullptr) {
            return false;
        }
        for (int i = 0; i < cap; ++i) {
            if (table[off + stride * i] != 0) {
                return true;
            }
        }
        return false;
    };
    auto anyAllocatedKey = [](const auto* keys, std::size_t count) {
        return AnyAllocatedKey(keys, count);
    };
    bool any;
    if (app->state.optflag[0] != 0) {
        // 相机/配件模式：只扫三张 app 表（0x46BE76 起），模型表不扫。
        any = anySelectedByte(reinterpret_cast<unsigned char*>(
                                  app->state.cameraKeyTrack), 0x48, 0x54, mdl::KeyPoolCapacity(app->CameraKeys())) ||
              anySelectedByte(reinterpret_cast<unsigned char*>(
                                  app->state.lightKeyTrack), 0x24, 0x28, mdl::KeyPoolCapacity(app->LightKeys())) ||
              anySelectedByte(reinterpret_cast<unsigned char*>(
                                  app->state.selfShadowKeyTrack), 0x14, 0x18, mdl::KeyPoolCapacity(app->ShadowKeys())) ||
              // MMS: selected `.x` keys are saved behind the standard sections.
              accessory_motion_io::AnySelected(app);
    } else {
        // 模型模式：骨/表情/显示三张模型帧表（x64 模型 +0x2790/+0x2798/+0x27A0）。
        any = false;
        unsigned char* const model = app->SelectedModel();
        if (model != nullptr) {
            any = anyAllocatedKey(mdl::BoneKeys(model),
                                  mdl::BoneKeyCapacity(*mdl::Mdl(model))) ||
                  anyAllocatedKey(mdl::MorphKeys(model), mdl::MorphKeyCapacity(mdl::MorphKeys(model))) ||
                  anyAllocatedKey(mdl::DisplayKeys(model), mdl::KeyPoolCapacity(mdl::DisplayKeys(model)));
        }
    }
    if (!any) {
        // 0x46BF66：提示前 enterKeyState 再置一次（入口已置，幂等）。
        LegacyUiMessageBoxA(MainHwnd(app),
                    app->EnglishUI() != 0
                        ? "Motion data will be saved only selected frame point.\n"
                          "Please select frame point."
                        : kMsgSaveMotionJp,
                    app->EnglishUI() != 0 ? "save motion data"
                                          : kCaptionSaveMotionJp,
                    MB_TOPMOST);                // 0x40000
        return;
    }
    SetCurrentDirectoryW(app->ExeDir());
    wchar_t path[kDialogPathLen];
    swprintf_s(path, kDialogPathLen, kFmtEmptyPath, L"", L"");
    wchar_t fileTitle[kDialogPathLen];
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = MainHwnd(app);              // 保存框 owner 恒为主窗
    ofn.lpstrFilter = L"Vocaloid Motion Data files(*.vmd)\0*.vmd\0";
    ofn.lpstrFile = path;
    ofn.nFilterIndex = 1;
    ofn.nMaxFile = kDialogPathLen;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;  // 6
    ofn.lpstrInitialDir = RememberFileDialogDirs(app)
                              ? app->DirMotion()
                              : L"UserFile\\Motion";
    ofn.lpstrDefExt = L"vmd";
    ofn.nMaxFileTitle = kDialogPathLen;
    ofn.lpstrFileTitle = fileTitle;
    ofn.lpstrTitle = app->EnglishUI() != 0
                         ? L"save motion data"
                         : kTitleSaveMotionJp;
    if (!GetUiSaveFileNameW(&ofn)) {
        return;
    }
    if (RememberFileDialogDirs(app)) {
        wchar_t* dir = ExtractDirFromPath(
            app->PathWorkspace().projectDirectory, path, kPathFieldCapacity);
        CopyDirPathW(app->DirMotion(), dir);
    }
    SaveVmdFile(path);                          // 0x46C114 -> 0x419370，无标志写
}

// ---------------------------------------------------------------------------
// 213 (0xD5, 入口 0x7FF7CB46B1BF): File: 载入 AVI 背景。
//   [dialogFlags[17]]=1; enterKeyState=1（无模式门）-> 打开对话框（owner=
//   浮动窗否则主窗，DirBg/0xA4CFC 或 "UserFile\BackGround"，defExt "avi"，
//   EN 标题 "load AVI file"/JP 即 L"AVI"）-> 回写目录、整路径入
//   aviBackgroundPath（0x9F0B8）-> LoadAviFile -> 置脏。不置 reseat。
// ---------------------------------------------------------------------------
void CmdLoadAvi(MMDApp* app) {
    app->state.dialogFlags[17] = 1;             // 0x46B1BF [app+0x78]
    app->state.enterKeyState = 1;               // 0x46B1C6 [app+0xC0]
    SetCurrentDirectoryW(app->ExeDir());
    wchar_t path[kDialogPathLen];
    swprintf_s(path, kDialogPathLen, kFmtEmptyPath, L"", L"");
    wchar_t fileTitle[kDialogPathLen];
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app->FloatingWindow() != nullptr
                        ? app->FloatingWindow()
                        : MainHwnd(app);
#if MMD_ORIGINAL_PROFILE
    ofn.lpstrFilter = L"AVI files (*.avi)\0*.avi\0";
#else
    ofn.lpstrFilter = L"Video (*.avi;*.mp4;*.mov)\0*.avi;*.mp4;*.mov\0";
#endif
    ofn.lpstrFile = path;
    ofn.nMaxFile = kDialogPathLen;
    ofn.Flags = OFN_FILEMUSTEXIST;              // 0x1000
    ofn.lpstrInitialDir = RememberFileDialogDirs(app)
                              ? app->DirBg()    // 0xA4CFC
                              : L"UserFile\\BackGround";
    ofn.lpstrDefExt = L"avi";
    ofn.nMaxFileTitle = kDialogPathLen;
    ofn.lpstrFileTitle = fileTitle;
    ofn.lpstrTitle = app->EnglishUI() != 0
                         ? L"Open background video"
                         : L"背景動画を開く";
    if (!GetUiOpenFileNameW(&ofn)) {
        return;
    }
    if (RememberFileDialogDirs(app)) {
        wchar_t* dir = ExtractDirFromPath(
            app->PathWorkspace().projectDirectory, path, kPathFieldCapacity);
        CopyDirPathW(app->DirBg(), dir);
    }
    wcscpy_s(app->AviBackgroundPath(), kDialogPathLen, path);  // 0x46B339
    LoadAviFile(app);                           // 0x46B355 -> 0x433250
    app->SceneModified() = 1;                   // 0x46B35A [0xA1B31]
}

}  // namespace mikudancestudio
