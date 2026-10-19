#include "mikumikustudio/ui_message.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include <array>
#include <cwchar>
#include <string>

namespace mikumikustudio {
namespace {
struct Text { const wchar_t* title; const wchar_t* body; };
// Columns follow kUiJapanese / kUiEnglish / kUiChinese, never a boolean flag.
constexpr Text messages[][3] = {
    {{L"モデル削除", L"モデルと全フレームのデータを削除しますか？\nこの操作は元に戻せません。"},
     {L"Delete model", L"Delete the model and all its frame data?\nThis cannot be undone."},
     {L"删除模型", L"确定删除模型及其全部帧数据吗？\n此操作无法撤销。"}},
    {{L"アクセサリ削除", L"アクセサリと全フレームのデータを削除しますか？\nこの操作は元に戻せません。"},
     {L"Delete accessory", L"Delete the accessory and all its frame data?\nThis cannot be undone."},
     {L"删除配件", L"确定删除配件及其全部帧数据吗？\n此操作无法撤销。"}},
    {{L"リップフレーム削除", L"全てのリップフレームを削除しますか？\nこの操作は元に戻せません。"},
     {L"Delete lip frames", L"Delete all lip frames?\nThis cannot be undone."},
     {L"删除口型帧", L"确定删除全部口型帧吗？\n此操作无法撤销。"}},
    {{L"目フレーム削除", L"全ての目のフレームを削除しますか？\nこの操作は元に戻せません。"},
     {L"Delete eye frames", L"Delete all eye frames?\nThis cannot be undone."},
     {L"删除眼部帧", L"确定删除全部眼部帧吗？\n此操作无法撤销。"}},
    {{L"眉フレーム削除", L"全ての眉のフレームを削除しますか？\nこの操作は元に戻せません。"},
     {L"Delete eyebrow frames", L"Delete all eyebrow frames?\nThis cannot be undone."},
     {L"删除眉毛帧", L"确定删除全部眉毛帧吗？\n此操作无法撤销。"}},
    {{L"新規作成", L"現在の状態を破棄して新規作成しますか？"},
     {L"New scene", L"Discard the current scene and create a new one?"},
     {L"新建场景", L"确定放弃当前场景并新建吗？"}},
    {{L"ファイルを開く", L"保存されていない変更があります。\n変更を破棄してファイルを開きますか？"},
     {L"Open scene", L"There are unsaved changes.\nDiscard them and open another scene?"},
     {L"打开场景", L"当前有未保存的修改。\n确定放弃修改并打开其他场景吗？"}},
    {{L"終了", L"保存されていない変更があります。\n終了してもよろしいですか？"},
     {L"Quit", L"There are unsaved changes.\nQuit anyway?"},
     {L"退出", L"当前有未保存的修改。\n确定退出吗？"}},
    {{L"終了", L"拡張モデルが保存されていません。\n終了してもよろしいですか？"},
     {L"Quit", L"The enhanced model has not been saved.\nQuit anyway?"},
     {L"退出", L"增强模型尚未保存。\n确定退出吗？"}},
    {{L"録画中断確認", L"録画を中断してもよろしいですか？"},
     {L"Stop recording", L"Do you want to stop recording?"},
     {L"停止录制", L"确定停止录制吗？"}},
    {{L"別のボーンへ貼り付け", L"コピーしたフレームを現在選択中のボーンへ貼り付けますか？"},
     {L"Paste to another bone", L"Paste the copied frames to the currently selected bone?"},
     {L"粘贴到其他骨骼", L"确定将复制的帧粘贴到当前选中的骨骼吗？"}},
    {{L"別のアクセサリへ貼り付け", L"コピーしたフレームを現在選択中のアクセサリへ貼り付けますか？"},
     {L"Paste to another accessory", L"Paste the copied frames to the currently selected accessory?"},
     {L"粘贴到其他配件", L"确定将复制的帧粘贴到当前选中的配件吗？"}},
    {{L"フレーム貼り付け", L"コピーしたボーンフレームがありません。\n先にフレームをコピーしてから F キーを押してください。"},
     {L"Paste frames", L"No bone frames have been copied.\nCopy bone frames first, then press F."},
     {L"粘贴帧", L"尚未复制骨骼帧。\n请先复制骨骼帧，再按 F。"}},
    {{L"フレーム貼り付け", L"コピーしたアクセサリフレームがありません。\n先にフレームをコピーしてから F キーを押してください。"},
     {L"Paste frames", L"No accessory frames have been copied.\nCopy accessory frames first, then press F."},
     {L"粘贴帧", L"尚未复制配件帧。\n请先复制配件帧，再按 F。"}},
    {{L"フレーム貼り付け", L"ボーンを選択してください。"},
     {L"Paste frames", L"Please select a bone."}, {L"粘贴帧", L"请先选择骨骼。"}},
    {{L"フレーム貼り付け", L"アクセサリを選択してください。"},
     {L"Paste frames", L"Please select an accessory."}, {L"粘贴帧", L"请先选择配件。"}},
    {{L"ポーズ保存", L"選択したボーンのみ保存します。\nボーンを選択してください。"},
     {L"Save pose", L"Only selected bones will be saved.\nPlease select bones first."},
     {L"保存姿势", L"仅保存选中的骨骼。\n请先选择骨骼。"}},
    {{L"DirectSound エラー", L"DirectSound の初期化に失敗したため、WAVE を再生できません。"},
     {L"DirectSound error", L"Cannot play WAVE because DirectSound initialization failed."},
     {L"DirectSound 错误", L"DirectSound 初始化失败，无法播放 WAVE 音频。"}},
    {{L"モデル読み込み", L"モデルファイルを開けません。"},
     {L"Load model", L"Cannot open the model file."},
     {L"加载模型", L"无法打开模型文件。"}},
    {{L"プロジェクト読み込み", L"プロジェクトファイルを開けません。"},
     {L"Load project", L"Cannot open the project file."},
     {L"加载项目", L"无法打开工程文件。"}},
    {{L"プロジェクト読み込み", L"有効な PMM ファイルではありません。"},
     {L"Load project", L"This is not a valid PMM file."},
     {L"加载项目", L"这不是有效的 PMM 工程文件。"}},
    {{L"プロジェクト読み込み", L"このバージョンの PMM ファイルは読み込めません。"},
     {L"Load project", L"This PMM file version is not supported."},
     {L"加载项目", L"不支持此 PMM 工程文件的版本。"}},
    {{L"プロジェクト読み込み", L"PMM ファイルが途中で終了しています。保存が完了していない可能性があります。読み込みを中止しました。元のファイルは変更していません。"},
     {L"Load project", L"The PMM file ends before a complete record can be read. Saving may have been interrupted. Loading was stopped; the original file was not changed."},
     {L"加载项目", L"PMM 文件提前结束，记录不完整，可能未完成保存。已停止加载，原文件未改动。"}},
    {{L"PMM フレームデータ", L"キーフレームの参照が不正です。最後の有効なフレーム以降の壊れた参照を切り離しました。"},
     {L"PMM frame data", L"A keyframe link is invalid. The broken link after the last valid frame was disconnected."},
     {L"PMM 帧数据", L"关键帧引用无效。已断开最后一个有效帧之后的损坏引用。"}},
    {{L"表情へ強制貼付", L"コピーした表情フレームを選択中の表情へ貼り付けますか？"},
     {L"Force paste expression", L"Paste the copied expression frames to the selected expression?"},
     {L"强制粘贴表情", L"确定将复制的表情关键帧粘贴到当前选中的表情吗？"}},
    {{L"表情へ強制貼付", L"表情フレームを先にコピーしてください。"},
     {L"Force paste expression", L"Copy expression keyframes first."},
     {L"强制粘贴表情", L"请先复制表情关键帧。"}},
    {{L"表情へ強制貼付", L"貼り付け先の表情を一つ選択してください。"},
     {L"Force paste expression", L"Select one destination expression in the timeline or expression panel."},
     {L"强制粘贴表情", L"请在时间轴或表情面板中选择一个目标表情。"}},
};
static_assert(std::size(messages) == static_cast<std::size_t>(UiMessage::Count));

struct ButtonScope {
    HWND owner;
    unsigned char language;
    bool translated = false;
    ButtonScope* previous;
};
thread_local ButtonScope* active = nullptr;

struct ProjectModelPrompt {
    unsigned char language;
    ProjectModelIssue issue;
    const wchar_t* modelName;
};
INT_PTR CALLBACK ProjectModelDialogProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        const auto& prompt = *reinterpret_cast<const ProjectModelPrompt*>(lp);
        const auto language = prompt.language;
        const bool mismatch = prompt.issue == ProjectModelIssue::StructureMismatch;
        constexpr const wchar_t* titles[] = {L"プロジェクト読み込み", L"Load project", L"加载项目"};
        constexpr const wchar_t* missing[] = {
            L"モデルファイルを開けません：", L"Cannot open the model file:", L"找不到或无法打开模型文件："};
        constexpr const wchar_t* different[] = {
            L"モデルの構造が PMM と異なります：", L"Model structure differs from the PMM:", L"模型结构与 PMM 工程不一致："};
        constexpr const wchar_t* locate[] = {
            L"モデルファイルを指定して読み込む", L"Locate and load the model file", L"指定模型文件位置并加载"};
        constexpr const wchar_t* locateAnother[] = {
            L"別のモデルファイルを指定して読み込む", L"Locate another model file", L"指定其他模型文件"};
        constexpr const wchar_t* fit[] = {
            L"PMM のモデル情報を合わせて続行", L"Adapt PMM model data and continue", L"将 PMM 数据适配到此模型并继续"};
        constexpr const wchar_t* skip[] = {
            L"このモデルを飛ばして続行", L"Skip this model and continue", L"跳过此模型并继续加载"};
        constexpr const wchar_t* cancel[] = {
            L"プロジェクトの読み込みを中止", L"Cancel loading this project", L"取消加载整个项目"};
        SetWindowTextW(dialog, titles[language]);
        std::wstring body = mismatch ? different[language] : missing[language];
        if (prompt.modelName && *prompt.modelName) { body += L"\n"; body += prompt.modelName; }
        SetDlgItemTextW(dialog, 686, body.c_str());
        // Keep the name on its own line inside the existing resource layout.
        RECT bodyRect{10, 10, 210, 36};
        MapDialogRect(dialog, &bodyRect);
        SetWindowPos(GetDlgItem(dialog, 686), nullptr, bodyRect.left, bodyRect.top,
                     bodyRect.right - bodyRect.left, bodyRect.bottom - bodyRect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetDlgItemTextW(dialog, 687, mismatch ? fit[language] : locate[language]);
        SetDlgItemTextW(dialog, 688, mismatch ? locateAnother[language] : skip[language]);
        SetDlgItemTextW(dialog, 689, mismatch ? skip[language] : cancel[language]);
        if (mismatch) SetDlgItemTextW(dialog, 690, cancel[language]);
        return TRUE;
    }
    if (message == WM_COMMAND) {
        // Original 0x0040FF80 dispatch, shared by the two PMM dialogs.
        switch (LOWORD(wp)) {
        case 687: EndDialog(dialog, 1); return TRUE;
        case 688: EndDialog(dialog, 3); return TRUE;
        case 689: EndDialog(dialog, 4); return TRUE;
        case 690: EndDialog(dialog, 5); return TRUE;
        case IDCANCEL: EndDialog(dialog, 2); return TRUE;
        }
    }
    return FALSE;
}

LRESULT CALLBACK TranslateButtons(int code, WPARAM wp, LPARAM lp) {
    // Scope only the message box opened by ShowUiMessage, never plugin dialogs.
    if (code == HCBT_ACTIVATE && active && !active->translated) {
        HWND dialog = reinterpret_cast<HWND>(wp);
        wchar_t cls[32]{};
        GetClassNameW(dialog, cls, 32);
        if (std::wcscmp(cls, L"#32770") == 0 && GetWindow(dialog, GW_OWNER) == active->owner) {
            active->translated = true;
            struct Button { int id; const wchar_t* text[3]; };
            static constexpr Button buttons[] = {
                {IDOK, {L"OK", L"OK", L"确定"}},
                {IDCANCEL, {L"キャンセル", L"Cancel", L"取消"}},
                {IDYES, {L"はい(&Y)", L"Yes", L"是(&Y)"}},
                {IDNO, {L"いいえ(&N)", L"No", L"否(&N)"}},
            };
            for (const auto& button : buttons)
                if (HWND control = GetDlgItem(dialog, button.id))
                    SetWindowTextW(control, button.text[active->language]);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
}

INT_PTR ShowProjectModelDialog(MMDApp* app, HINSTANCE instance, HWND owner,
                              ProjectModelIssue issue, const wchar_t* modelName) {
    const unsigned char language = app && app->EnglishUI() <= kUiChinese
        ? app->EnglishUI() : kUiEnglish;
    ProjectModelPrompt prompt{language, issue, modelName};
    // Reuse the resource geometry; every caption is set from the live UI language.
    const int resource = (issue == ProjectModelIssue::MissingFile ? 814 : 816)
                       + (language == kUiJapanese ? 0 : 1);
    const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(resource), owner,
                                          ProjectModelDialogProc, reinterpret_cast<LPARAM>(&prompt));
    return result == -1 ? 2 : result;
}

int ShowUiMessage(MMDApp* app, HWND owner, UiMessage message, UINT flags,
                  const wchar_t* detail) {
    const unsigned char language = app && app->EnglishUI() <= kUiChinese
        ? app->EnglishUI() : kUiEnglish;
    const auto index = static_cast<std::size_t>(message);
    if (index >= std::size(messages)) return 0;
    const auto& text = messages[index][language];
    std::wstring body = text.body;
    if (detail && *detail) { body += L"\n\n"; body += detail; }
    ButtonScope scope{owner, language, false, active};
    active = &scope;
    HHOOK hook = SetWindowsHookExW(WH_CBT, TranslateButtons, nullptr, GetCurrentThreadId());
    const int result = MessageBoxW(owner, body.c_str(), text.title, flags);
    if (hook) UnhookWindowsHookEx(hook);
    active = scope.previous;
    return result;
}
}
