#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "image_sequence_dialog.hpp"
#include "dialog_scaffold.hpp"
#include "export_save_prompt.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include <ShObjIdl.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>

namespace mikumikustudio::image_sequence {
namespace {
const wchar_t* Title(MMDApp* app) {
    return Text(app, L"导出图片序列", L"Export image sequence", L"連番画像出力");
}
void Error(HWND owner, MMDApp* app, const std::wstring& error) {
    MessageBoxW(owner, error.c_str(), Title(app), MB_OK | MB_ICONWARNING);
}
bool PickFolder(HWND owner, MMDApp* app, std::wstring& directory) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> picker;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&picker));
    if (SUCCEEDED(hr)) {
        DWORD flags = 0;
        hr = picker->GetOptions(&flags);
        if (SUCCEEDED(hr)) hr = picker->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        picker->SetTitle(Text(app, L"导出图片序列：选择文件夹", L"Choose image sequence output folder", L"連番画像の出力フォルダーを選択"));
        Microsoft::WRL::ComPtr<IShellItem> folder;
        if (!directory.empty() && SUCCEEDED(SHCreateItemFromParsingName(directory.c_str(),
            nullptr, IID_PPV_ARGS(&folder)))) picker->SetFolder(folder.Get());
        if (SUCCEEDED(hr)) hr = picker->Show(owner);
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return false;
        if (SUCCEEDED(hr)) {
            folder.Reset();
            hr = picker->GetResult(&folder);
            PWSTR path = nullptr;
            if (SUCCEEDED(hr)) hr = folder->GetDisplayName(SIGDN_FILESYSPATH, &path);
            if (SUCCEEDED(hr)) directory = path;
            CoTaskMemFree(path);
        }
    }
    if (FAILED(hr)) Error(owner, app, Text(app, L"无法打开文件夹选择窗口，请重试。",
        L"Could not open the folder picker. Please try again.", L"フォルダー選択を開けません。もう一度お試しください。"));
    return SUCCEEDED(hr);
}
std::wstring ReadText(HWND dialog, int id) {
    const HWND control = GetDlgItem(dialog, id);
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    text.resize(GetWindowTextW(control, text.data(), static_cast<int>(text.size())));
    return text;
}
void UpdateExample(HWND dialog, DialogContext& c) {
    Options o = c.options;
    o.prefix = ReadText(dialog, Prefix);
    o.format = static_cast<int>(SendDlgItemMessageW(dialog, FormatCombo, CB_GETCURSEL, 0, 0));
    BOOL valid = FALSE;
    const int end = static_cast<int>(GetDlgItemInt(dialog, panel::kAviEndFrameEdit, &valid, TRUE));
    if (valid) o.end = end;
    const int start = static_cast<int>(GetDlgItemInt(dialog, panel::kAviStartFrameEdit, &valid, TRUE));
    if (valid) o.start = start;
    o.fps = wcstof(ReadText(dialog, panel::kAviFpsEdit).c_str(), nullptr);
    const auto sample = std::wstring(Text(c.app, L"文件名示例：", L"Filename example: ", L"ファイル名の例：")) +
        std::filesystem::path(FramePath(o, 1)).filename().wstring();
    SetDlgItemTextW(dialog, Example, sample.c_str());
}
bool ReadSettings(HWND dialog, DialogContext& c, Options& o) {
    o = c.options;
    o.prefix = ReadText(dialog, Prefix);
    o.format = static_cast<int>(SendDlgItemMessageW(dialog, FormatCombo, CB_GETCURSEL, 0, 0));
    bool valid = true;
    auto integer = [&](int id) {
        BOOL ok = FALSE;
        int n = static_cast<int>(GetDlgItemInt(dialog, id, &ok, TRUE));
        valid = valid && ok;
        return n;
    };
    o.width = integer(panel::kAviWidthEdit);
    o.height = integer(panel::kAviHeightEdit);
    o.start = integer(panel::kAviStartFrameEdit);
    o.end = integer(panel::kAviEndFrameEdit);
    const auto fps = ReadText(dialog, panel::kAviFpsEdit);
    wchar_t* tail = nullptr;
    o.fps = wcstof(fps.c_str(), &tail);
    while (tail && iswspace(*tail)) ++tail;
    valid = valid && tail != fps.c_str() && tail && *tail == L'\0';
    o.closeAfterExport = IsDlgButtonChecked(dialog, panel::kAviCloseAfterExportCheckbox) == BST_CHECKED;
    if (!valid) {
        Error(dialog, c.app, Text(c.app, L"请填写有效的尺寸、起止帧和帧率。", L"Enter valid dimensions, frame range and frame rate.", L"有効な画像サイズ、フレーム範囲、FPSを入力してください。"));
        return false;
    }
    std::wstring error;
    if (!Validate(c.app, o, error)) { Error(dialog, c.app, error); return false; }
    return true;
}
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    auto* c = reinterpret_cast<DialogContext*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        c = reinterpret_cast<DialogContext*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(c));
        MakeDialogTopmostIfRequested(c->app, dialog);
        SetWindowTextW(dialog, Title(c->app));
        const auto label = [&](int id, const wchar_t* zh, const wchar_t* en, const wchar_t* ja) {
            SetDlgItemTextW(dialog, id, Text(c->app, zh, en, ja));
        };
        label(FolderLabel, L"输出文件夹", L"Output folder", L"出力フォルダー");
        label(Browse, L"浏览…", L"Browse…", L"参照…");
        label(PrefixLabel, L"文件名前缀", L"Filename prefix", L"ファイル名の接頭辞");
        label(FormatLabel, L"图片格式", L"Image format", L"画像形式");
        label(SizeLabel, L"图片尺寸", L"Image size", L"画像サイズ");
        label(FpsLabel, L"帧率", L"Frame rate", L"フレームレート");
        label(RangeLabel, L"输出帧范围", L"Frame range", L"出力フレーム範囲");
        label(panel::kAviCloseAfterExportCheckbox, L"导出成功后关闭 MMS", L"Close MMS after successful export", L"出力完了後に MMS を終了");
        label(IDOK, L"导出", L"Export", L"出力開始");
        label(IDCANCEL, L"取消", L"Cancel", L"キャンセル");
        c->updating = true;
        SendDlgItemMessageW(dialog, Prefix, EM_SETLIMITTEXT, 128, 0);
        SetDlgItemTextW(dialog, Folder, c->options.directory.c_str());
        SetDlgItemTextW(dialog, Prefix, c->options.prefix.c_str());
        for (const auto& format : kFormats)
            SendDlgItemMessageW(dialog, FormatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(format.name));
        SendDlgItemMessageW(dialog, FormatCombo, CB_SETCURSEL, c->options.format, 0);
        SetDlgItemInt(dialog, panel::kAviWidthEdit, c->options.width, FALSE);
        SetDlgItemInt(dialog, panel::kAviHeightEdit, c->options.height, FALSE);
        SetDlgItemInt(dialog, panel::kAviStartFrameEdit, c->options.start, FALSE);
        SetDlgItemInt(dialog, panel::kAviEndFrameEdit, c->options.end, FALSE);
        wchar_t fps[32]{}; swprintf_s(fps, L"%g", c->options.fps);
        SetDlgItemTextW(dialog, panel::kAviFpsEdit, fps);
        CheckDlgButton(dialog, panel::kAviCloseAfterExportCheckbox, c->options.closeAfterExport ? BST_CHECKED : BST_UNCHECKED);
        export_save_prompt::Initialize(dialog, c->app);
        c->updating = false;
        UpdateExample(dialog, *c);
        return TRUE;
    }
    if (!c) return FALSE;
    if (const auto handled = export_save_prompt::HandleMessage(dialog, c->app, message, wp, lp)) return handled;
    if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    if (message != WM_COMMAND) return FALSE;
    const int id = LOWORD(wp), notification = HIWORD(wp);
    if (id == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    if (id == Browse) {
        if (PickFolder(dialog, c->app, c->options.directory))
            SetDlgItemTextW(dialog, Folder, c->options.directory.c_str());
        return TRUE;
    }
    if (id == IDOK) {
        Options options;
        if (!ReadSettings(dialog, *c, options)) return TRUE;
        std::wstring error;
        if (!Prepare(c->app, options, error)) { Error(dialog, c->app, error); return TRUE; }
        c->options = std::move(options);
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if (!c->updating && (((id == Prefix || id == panel::kAviStartFrameEdit ||
        id == panel::kAviEndFrameEdit || id == panel::kAviFpsEdit) && notification == EN_CHANGE) ||
        (id == FormatCombo && notification == CBN_SELCHANGE))) UpdateExample(dialog, *c);
    if (!c->updating && notification == EN_CHANGE &&
        (id == panel::kAviWidthEdit || id == panel::kAviHeightEdit)) {
        BOOL valid = FALSE;
        const int entered = static_cast<int>(GetDlgItemInt(dialog, id, &valid, TRUE));
        if (valid && entered >= 2 && entered <= 16384) {
            const int baseW = std::max(1, c->options.width), baseH = std::max(1, c->options.height);
            const bool width = id == panel::kAviWidthEdit;
            const auto scaled = (static_cast<std::int64_t>(entered) * (width ? baseH : baseW) +
                (width ? baseW : baseH) / 2) / (width ? baseW : baseH);
            c->updating = true;
            SetDlgItemInt(dialog, width ? panel::kAviHeightEdit : panel::kAviWidthEdit,
                static_cast<UINT>(std::max<std::int64_t>(2, scaled)), FALSE);
            c->updating = false;
        }
    }
    return FALSE;
}

void RefreshMenu(MMDApp* app) {
    const HMENU menu = GetMenu(app->MainWindow());
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        const HMENU popup = GetSubMenu(menu, i);
        if (!popup || GetMenuState(popup, 223, MF_BYCOMMAND) == UINT(-1)) continue;
        const wchar_t* label = Text(app, L"导出图片序列(&I)…", L"Export image sequence(&I)…", L"連番画像出力(&I)…");
        if (GetMenuState(popup, kMenuCommand, MF_BYCOMMAND) == UINT(-1))
            InsertMenuW(popup, 276, MF_BYCOMMAND | MF_STRING, kMenuCommand, label);
        else {
            MENUITEMINFOW item{sizeof(item)};
            item.fMask = MIIM_STRING;
            item.dwTypeData = const_cast<wchar_t*>(label);
            SetMenuItemInfoW(popup, kMenuCommand, FALSE, &item);
        }
        return;
    }
}

bool HandleCommand(MMDApp* app, unsigned command) {
    if (command != kMenuCommand) return false;
    // Export the scene in either editing mode, just like video export.
    // CameraMode is an editor selection, not a playback/recording guard.
    if (app->PlaybackActive() || app->RecordingWindow() || Active(app)) return true;
    DialogContext context{app};
    context.options.width = app->RenderWidth();
    context.options.height = app->RenderHeight();
    context.options.end = static_cast<int>(SceneEndFrame(app));
    context.options.directory = app->DirUser();
    if (!PickFolder(app->MainWindow(), app, context.options.directory)) return true;
    app->state.enterKeyState = 1;
    const auto result = UiDialogBoxParamA(static_cast<HINSTANCE>(app->HInstance()),
        MAKEINTRESOURCEA(kDialog), app->MainWindow(), DialogProc, reinterpret_cast<LPARAM>(&context));
    if (result != IDOK) { Abort(app); return true; }
    const auto& o = context.options;
    app->AviOutputWidth() = o.width;
    app->AviOutputHeight() = o.height;
    app->AviRecordStartFrame() = o.start;
    app->AviRecordEndFrame() = o.end;
    app->AviRecordFps() = o.fps;
    app->AviIncludeWave() = 0;
    app->AviStereoOutput() = 0;
    app->AviCloseAfterExport() = o.closeAfterExport;
    StartAviRecordWindow(app);
    if (!app->state.recordPlaybackStartPending) Abort(app);
    return true;
}
} // namespace mikumikustudio::image_sequence
