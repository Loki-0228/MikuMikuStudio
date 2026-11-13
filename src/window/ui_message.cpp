#include "mikumikustudio/ui_message.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/ui_scale.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "../io/project_relocation_context.hpp"
#include "native_dialog_backdrop.hpp"
#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
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
    {{L"モデル削除", L"モデルと全フレームのデータを削除しますか？\nこの操作は元に戻せます。"},
     {L"Delete model", L"Delete the model and all its frame data?\nYou can undo this operation."},
     {L"删除模型", L"确定删除模型及其全部帧数据吗？\n此操作可以撤销。"}},
    {{L"アクセサリ削除", L"アクセサリと全フレームのデータを削除しますか？\nこの操作は元に戻せます。"},
     {L"Delete accessory", L"Delete the accessory and all its frame data?\nYou can undo this operation."},
     {L"删除配件", L"确定删除配件及其全部帧数据吗？\n此操作可以撤销。"}},
    {{L"新規プロジェクト", L"プロジェクトをリセットできませんでした。編集中の操作を終了してから、もう一度試してください。"},
     {L"New project", L"The project could not be reset. Finish any active editing operation, then try again."},
     {L"新建工程", L"无法重置工程。请结束正在进行的编辑操作后重试。"}},
    {{L"プロジェクトの復旧", L"現在のシーンを復旧できていないため、編集や保存はできません。\n選択したプロジェクトの読み込みは開始していません。\n［ファイル］の［新規作成］または［開く］で復旧を再試行するか、アプリケーションを終了してください。"},
     {L"Project recovery", L"The current scene has not recovered and cannot be edited or saved.\nThe selected project has not started loading.\nUse File > New or File > Open to retry recovery, or exit the application."},
     {L"工程恢复", L"当前场景尚未恢复，暂不能编辑或保存。\n本次选择的工程尚未开始加载。\n可通过“文件 → 新建”或“文件 → 打开”重试恢复，或退出程序。"}},
    {{L"プロジェクトの復旧", L"プロジェクトの読み込みは中止されましたが、編集可能な状態への復旧に失敗しました。\n以前のシーンは復元されておらず、編集や保存はできません。\n［ファイル］の［新規作成］または［開く］で復旧を再試行するか、アプリケーションを終了してください。"},
     {L"Project recovery", L"Project loading stopped, but the scene could not be restored to an editable state.\nThe previous scene was not restored; editing and saving are unavailable.\nUse File > New or File > Open to retry recovery, or exit the application."},
     {L"工程恢复", L"工程加载已停止，但未能恢复到可编辑状态。\n原先的场景未被还原，暂不能编辑或保存。\n可通过“文件 → 新建”或“文件 → 打开”重试恢复，或退出程序。"}},
    {{L"エフェクトの復旧", L"この FX ファイルから移動先を確認できませんでした。同じ名前のコンパイル可能な FX を選ぶか、モデルの場所を指定してください。"},
     {L"Effect recovery", L"Could not use this FX file to locate the moved resources. Choose a compilable FX with the original filename, or locate the model directly."},
     {L"效果恢复", L"无法通过此 FX 文件确认资源的新位置。请选择可以编译的同名 FX，或直接指定模型位置。"}},
    {{L"保存の復旧", L"プロジェクトの保存を復旧できなかったため、読み込みを中止しました。\nプロジェクトとバックアップにアクセスできることを確認し、移動または置き換えられた元のバックアップを戻してから、もう一度開いてください。"},
     {L"Save recovery", L"Opening stopped because recovery of an interrupted project save could not finish.\nCheck that the project and its backups are accessible. If an original backup was moved or replaced, restore it before opening again."},
     {L"保存恢复", L"工程保存恢复未完成，本次打开已停止。\n请确认工程和备份文件可访问。如果原备份被移动或替换，请找回原文件后重试打开。"}},
    {{L"キーフレームの貼り付け", L"貼り付け先に対応するボーンまたは表情トラックがありません。同名トラックはモデル内の順番で対応します。キーフレームは変更されていません。"},
     {L"Paste keyframes", L"The target model is missing a matching bone or morph track. Tracks with the same name are matched by their order in the model. Existing keyframes were preserved."},
     {L"粘贴关键帧", L"目标模型缺少对应的骨骼或表情轨道。同名轨道按其在模型中的先后顺序对应。现有关键帧已保留。"}}
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
    bool offerEffect;
};

void MakeRecoveryTextScrollable(HWND dialog, const std::wstring& body) {
    const HWND label = GetDlgItem(dialog, 686);
    RECT bounds{};
    if (!GetWindowRect(label, &bounds)) return;
    MapWindowPoints(HWND_DESKTOP, dialog, reinterpret_cast<POINT*>(&bounds), 2);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const auto font = SendMessageW(label, WM_GETFONT, 0, 0);
    const HDC dc = GetDC(label);
    if (!dc) return;
    const auto old = font ? SelectObject(dc, reinterpret_cast<HFONT>(font)) : nullptr;
    RECT needed{0, 0, width, 0};
    const int measured = DrawTextW(dc, body.c_str(), static_cast<int>(body.size()), &needed,
        DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    if (old) SelectObject(dc, old);
    ReleaseDC(label, dc);
    if (measured <= 0 || (needed.right <= width && needed.bottom <= height)) return;

    const HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        bounds.left, bounds.top, width, height, dialog, nullptr,
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE)), nullptr);
    if (!edit) return;
    SendMessageW(edit, WM_SETFONT, font, FALSE);
    if (!SetWindowTextW(edit, body.c_str()) ||
        !SetWindowPos(edit, label, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        DestroyWindow(edit);
        return;
    }
    DestroyWindow(label);
    SetWindowLongPtrW(edit, GWLP_ID, 686);
}

INT_PTR CALLBACK ProjectModelDialogProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        const auto& prompt = *reinterpret_cast<const ProjectModelPrompt*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        const auto language = prompt.language;
        const bool mismatch = prompt.issue == ProjectModelIssue::StructureMismatch;
        const bool accessory = prompt.issue == ProjectModelIssue::MissingAccessory;
        const bool v1 = prompt.issue == ProjectModelIssue::MissingV1Model;
        constexpr const wchar_t* titles[] = {L"プロジェクト読み込み", L"Load project", L"加载项目"};
        constexpr const wchar_t* missing[] = {
            L"モデルファイルを開けません：", L"Cannot open the model file:", L"找不到或无法打开模型文件："};
        constexpr const wchar_t* missingAccessory[] = {
            L"アクセサリファイルが見つからないか、開けません：",
            L"Cannot find or open the accessory file:",
            L"找不到或无法打开附件文件："};
        constexpr const wchar_t* different[] = {
            L"モデルの構造が PMM と異なります：", L"Model structure differs from the PMM:", L"模型结构与 PMM 工程不一致："};
        constexpr const wchar_t* locate[] = {
            L"モデルファイルを指定して読み込む", L"Locate and load the model file", L"指定模型文件位置并加载"};
        constexpr const wchar_t* locateOriginal[] = {
            L"元のモデルファイルを指定する", L"Locate the original model file", L"指定原模型文件"};
        constexpr const wchar_t* locateEffect[] = {
            L"先にエフェクトを探す", L"Locate an effect first", L"先找回效果"};
        constexpr const wchar_t* locateAccessory[] = {
            L"アクセサリファイルを選択して読み込む", L"Select and load the accessory file", L"选择附件文件并加载"};
        constexpr const wchar_t* skipAccessory[] = {
            L"このアクセサリを飛ばして続行", L"Skip this accessory and continue", L"跳过此附件并继续加载"};
        constexpr const wchar_t* locateAnother[] = {
            L"別のモデルファイルを指定して読み込む", L"Locate another model file", L"指定其他模型文件"};
        constexpr const wchar_t* fit[] = {
            L"PMM のモデル情報を合わせて続行", L"Adapt PMM model data and continue", L"将 PMM 数据适配到此模型并继续"};
        constexpr const wchar_t* skip[] = {
            L"このモデルを飛ばして続行", L"Skip this model and continue", L"跳过此模型并继续加载"};
        constexpr const wchar_t* cancel[] = {
            L"プロジェクトの読み込みを中止", L"Cancel loading this project", L"取消加载整个项目"};
        SetWindowTextW(dialog, titles[language]);
        std::wstring body = mismatch ? different[language]
                                     : accessory ? missingAccessory[language] : missing[language];
        if (prompt.modelName && *prompt.modelName) { body += L"\r\n"; body += prompt.modelName; }
        const HWND bodyLabel = GetDlgItem(dialog, 686);
        SetWindowLongW(bodyLabel, GWL_STYLE, GetWindowLongW(bodyLabel, GWL_STYLE) | SS_NOPREFIX);
        SetDlgItemTextW(dialog, 686, body.c_str());
        // Keep the name on its own line inside the existing resource layout.
        RECT bodyRect{10, 10, 210, 36};
        MapDialogRect(dialog, &bodyRect);
        SetWindowPos(GetDlgItem(dialog, 686), nullptr, bodyRect.left, bodyRect.top,
                     bodyRect.right - bodyRect.left, bodyRect.bottom - bodyRect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetDlgItemTextW(dialog, 687, v1 ? locateOriginal[language] :
            mismatch ? fit[language] : accessory ? locateAccessory[language] : locate[language]);
        SetDlgItemTextW(dialog, 688, mismatch ? locateAnother[language] : accessory ? skipAccessory[language] : skip[language]);
        SetDlgItemTextW(dialog, 689, mismatch ? skip[language] : cancel[language]);
        if (mismatch) SetDlgItemTextW(dialog, 690, cancel[language]);
        if (prompt.offerEffect) {
            SetDlgItemTextW(dialog, 688, locateEffect[language]);
            if (!v1) {
                SetDlgItemTextW(dialog, 689, skip[language]);
                SetDlgItemTextW(dialog, 690, cancel[language]);
            }
        } else if (v1) {
            const HWND skipButton = GetDlgItem(dialog, 688);
            RECT bounds{};
            if (GetWindowRect(skipButton, &bounds)) {
                MapWindowPoints(HWND_DESKTOP, dialog, reinterpret_cast<POINT*>(&bounds), 2);
                SetWindowPos(GetDlgItem(dialog, 689), nullptr, bounds.left, bounds.top, 0, 0,
                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            EnableWindow(skipButton, FALSE);
            ShowWindow(skipButton, SW_HIDE);
        }
        native_dialog_backdrop::Attach(dialog, ui_scale::Percent());
        MakeRecoveryTextScrollable(dialog, body);
        return TRUE;
    }
    if (message == WM_COMMAND) {
        // Original 0x0040FF80 dispatch, shared by both PMM dialog layouts.
        // MissingFile and MissingAccessory: 1 = locate, 3 = skip, 4 = cancel.
        // StructureMismatch adds 690 = 5. Closing either layout returns 2.
        const auto* prompt = reinterpret_cast<const ProjectModelPrompt*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (prompt && prompt->offerEffect) {
            switch (LOWORD(wp)) {
            case 688: EndDialog(dialog, kLocateProjectEffect); return TRUE;
            case 689: EndDialog(dialog, prompt->issue == ProjectModelIssue::MissingV1Model ? 4 : 3); return TRUE;
            case 690: EndDialog(dialog, 4); return TRUE;
            }
        }
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

constexpr int kSkipMissingEffects = 1003;
constexpr INT_PTR kSkipEffectsResult = -2;

struct MissingEffectPrompt {
    unsigned char language;
    const std::vector<std::wstring>& paths;
    bool openingProject;
};

INT_PTR CALLBACK MissingEffectDialogProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        const auto& prompt = *reinterpret_cast<const MissingEffectPrompt*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        constexpr const wchar_t* title[] = {L"エフェクトの復旧", L"Recover an effect", L"找回效果"};
        constexpr const wchar_t* instruction[] = {L"見つからない元の FX を選んでください。",
            L"Select the original FX that is missing.", L"选择找不到的原 FX 文件。"};
        constexpr const wchar_t* open[] = {L"代替 FX を選ぶ", L"Choose replacement FX", L"选择替代 FX"};
        constexpr const wchar_t* missing[] = {L"見つからない FX があります。復旧する元の FX を選んでください。",
            L"Some FX files are missing. Select an original FX to recover.", L"部分 FX 文件缺失。请选择要找回的原 FX。"};
        constexpr const wchar_t* cancel[] = {L"キャンセル", L"Cancel", L"取消"};
        constexpr const wchar_t* cancelOpen[] = {L"読込を中止", L"Cancel opening", L"取消打开"};
        constexpr const wchar_t* skip[] = {L"復旧せず開く", L"Open without recovery", L"跳过恢复并打开"};
        SetWindowTextW(dialog, title[prompt.language]);
        SetDlgItemTextW(dialog, 1001, (prompt.openingProject ? missing : instruction)[prompt.language]);
        SetDlgItemTextW(dialog, IDOK, open[prompt.language]);
        SetDlgItemTextW(dialog, IDCANCEL, (prompt.openingProject ? cancelOpen : cancel)[prompt.language]);
        const HWND skipButton = GetDlgItem(dialog, kSkipMissingEffects);
        if (prompt.openingProject) SetWindowTextW(skipButton, skip[prompt.language]);
        else ShowWindow(skipButton, SW_HIDE);
        native_dialog_backdrop::Attach(dialog, ui_scale::Percent());
        const auto list = GetDlgItem(dialog, 1002);
        for (const auto& path : prompt.paths) {
            if (SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str())) < 0) {
                EndDialog(dialog, 0);
                return TRUE;
            }
        }
        if (const auto dc = GetDC(list)) {
            const auto font = reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0));
            const auto oldFont = font ? SelectObject(dc, font) : nullptr;
            LONG width = 0;
            for (const auto& path : prompt.paths) {
                SIZE size{};
                if (GetTextExtentPoint32W(dc, path.c_str(), static_cast<int>(path.size()), &size))
                    width = (std::max)(width, size.cx);
            }
            if (oldFont) SelectObject(dc, oldFont);
            ReleaseDC(list, dc);
            SendMessageW(list, LB_SETHORIZONTALEXTENT, width + 8, 0);
        }
        const bool selected = prompt.paths.size() == 1 &&
            SendMessageW(list, LB_SETCURSEL, 0, 0) != LB_ERR;
        EnableWindow(GetDlgItem(dialog, IDOK), selected);
        SetFocus(list);
        return FALSE;
    }
    if (message != WM_COMMAND) return FALSE;
    if (LOWORD(wp) == IDCANCEL) {
        EndDialog(dialog, 0);
        return TRUE;
    }
    if (LOWORD(wp) == kSkipMissingEffects) {
        const auto* prompt = reinterpret_cast<const MissingEffectPrompt*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (prompt && prompt->openingProject) EndDialog(dialog, kSkipEffectsResult);
        return TRUE;
    }
    if (LOWORD(wp) == IDOK || (LOWORD(wp) == 1002 && HIWORD(wp) == LBN_DBLCLK)) {
        const auto selection = SendDlgItemMessageW(dialog, 1002, LB_GETCURSEL, 0, 0);
        if (selection != LB_ERR) EndDialog(dialog, selection + 1);
        return TRUE;
    }
    if (LOWORD(wp) == 1002 && HIWORD(wp) == LBN_SELCHANGE) {
        EnableWindow(GetDlgItem(dialog, IDOK), SendDlgItemMessageW(dialog, 1002, LB_GETCURSEL, 0, 0) != LB_ERR);
        return TRUE;
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
    const bool offerEffect = app && (issue == ProjectModelIssue::MissingFile || issue == ProjectModelIssue::MissingV1Model) &&
        project_relocation::CanRecoverProjectEffect(*app);
    ProjectModelPrompt prompt{language, issue, modelName, offerEffect};
    // Reuse the resource geometry; every caption is set from the live UI language.
    // MissingFile and MissingAccessory share the three-button 814/815 layout.
    const bool fourRows = issue == ProjectModelIssue::StructureMismatch ||
        (issue == ProjectModelIssue::MissingFile && offerEffect);
    const int resource = (fourRows ? 816 : 814)
                       + (language == kUiJapanese ? 0 : 1);
    const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(resource), owner,
                                          ProjectModelDialogProc, reinterpret_cast<LPARAM>(&prompt));
    return result == -1 ? 2 : result;
}

ProjectEffectAction SelectProjectEffectReplacement(MMDApp& app, HWND owner,
    const std::vector<std::wstring>& missing, bool openingProject,
    std::wstring& original, std::wstring& replacement) {
    if (missing.empty()) return ProjectEffectAction::Skip;
    const auto language = app.EnglishUI() <= kUiChinese ? app.EnglishUI() : kUiEnglish;
    MissingEffectPrompt prompt{language, missing, openingProject};
    const auto selected = !openingProject && missing.size() == 1 ? 1 :
        UiDialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(9171),
            owner, MissingEffectDialogProc, reinterpret_cast<LPARAM>(&prompt));
    if (selected == kSkipEffectsResult || selected == -1) return ProjectEffectAction::Skip;
    if (selected <= 0 || static_cast<std::size_t>(selected) > missing.size())
        return ProjectEffectAction::Cancel;

    const auto& path = missing[static_cast<std::size_t>(selected - 1)];
    constexpr const wchar_t* titles[] = {L"代替 FX を選択 - ", L"Choose replacement FX - ", L"选择替代 FX - "};
    const auto name = std::filesystem::path(path).filename().wstring();
    const auto title = titles[language] + path;
    std::vector<wchar_t> file(32768, L'\0');
    std::copy(name.begin(), name.end(), file.begin());
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"FX (*.fx)\0*.fx\0\0";
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.lpstrTitle = title.c_str();
    ofn.lpstrDefExt = L"fx";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetUiOpenFileNameW(&ofn)) return ProjectEffectAction::Back;
    original = path;
    replacement = file.data();
    return ProjectEffectAction::Replace;
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
