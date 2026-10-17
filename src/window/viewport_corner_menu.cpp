// ===========================================================================
// 帮助菜单里的操作柄停靠位置轮换项。接口与约束见同名头文件。
// 点击一次按左上、右上、左下、右下顺序轮换，与模型名称语言菜单项一致。
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ui_language.hpp"
#include "viewport_corner_menu.hpp"
#include "mikudancestudio/viewport_corner_settings.hpp"

namespace mikudancestudio {
namespace viewport_corner_menu {
namespace {

using Corner = viewport_tool_layout::Corner;

// 帮助 popup 的稳定识别命令（「关于」，两个菜单模板里都是 201）。
constexpr UINT kHelpAboutCommand = 201;

HMENU g_helpPopup = nullptr;

int PopupPositionWithCommand(HMENU menu, UINT command) {
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW entry{sizeof(entry)};
        entry.fMask = MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, UINT(i), TRUE, &entry) ||
            entry.hSubMenu == nullptr)
            continue;
        if (GetMenuState(entry.hSubMenu, command, MF_BYCOMMAND) != UINT(-1))
            return i;
    }
    return -1;
}

int ItemIndexById(HMENU popup, UINT id) {
    const int count = GetMenuItemCount(popup);
    for (int i = 0; i < count; ++i) {
        if (GetMenuItemID(popup, UINT(i)) == id)
            return i;
    }
    return -1;
}

const wchar_t* MenuLabel(MMDApp* app, Corner corner) {
    int position = static_cast<int>(corner);
    if (position < 0 || position > 3)
        position = static_cast<int>(viewport_tool_layout::kDefaultCorner);
    const unsigned char lang = app != nullptr ? app->EnglishUI() : kUiEnglish;
    static constexpr const wchar_t* kChinese[] = {
        L"操作柄位置：左上", L"操作柄位置：右上",
        L"操作柄位置：左下", L"操作柄位置：右下"};
    static constexpr const wchar_t* kJapanese[] = {
        L"操作ハンドル位置：左上", L"操作ハンドル位置：右上",
        L"操作ハンドル位置：左下", L"操作ハンドル位置：右下"};
    static constexpr const wchar_t* kEnglish[] = {
        L"Handle position: Upper left", L"Handle position: Upper right",
        L"Handle position: Lower left", L"Handle position: Lower right"};
    if (lang == kUiChinese) return kChinese[position];
    if (lang == kUiJapanese) return kJapanese[position];
    return kEnglish[position];
}

void UpdateMenuLabel(MMDApp* app) {
    if (g_helpPopup == nullptr)
        return;
    const int index = ItemIndexById(g_helpPopup, kViewportCornerMenuCommand);
    if (index < 0)
        return;
    MENUITEMINFOW item{sizeof(item)};
    item.fMask = MIIM_FTYPE | MIIM_STRING;
    item.fType = MFT_STRING;
    item.dwTypeData = const_cast<wchar_t*>(
        MenuLabel(app, viewport_corner_settings::Current()));
    SetMenuItemInfoW(g_helpPopup, UINT(index), TRUE, &item);
}

void ApplyCorner(MMDApp* app, Corner corner) {
    const bool saved = viewport_corner_settings::Set(corner);
    UpdateMenuLabel(app);
    if (!saved && app != nullptr) {
        const unsigned char lang = app->EnglishUI();
        const wchar_t* title = lang == kUiChinese ? L"操作柄位置"
                              : lang == kUiJapanese ? L"操作ハンドル位置"
                                                    : L"Handle position";
        const wchar_t* body =
            lang == kUiChinese
                ? L"操作柄位置在本次运行已经生效，但写入本程序的设置失败。\n"
                  L"下次启动会回到之前保存的位置，请检查 "
                  L"程序旁的 data 目录是否可写。"
                : lang == kUiJapanese
                      ? L"操作ハンドル位置は今回の実行には反映されましたが、\n"
                        L"ユーザー設定への保存に失敗しました。\n"
                        L"次回起動時は以前に保存された位置に戻ります。"
                      : L"The handle position now applies to this run, but "
                        L"saving it to this installation's settings failed.\n"
                        L"The next start returns to the previously saved "
                        L"position; check that data beside the EXE "
                        L"is writable.";
        MessageBoxW(app->MainWindow(), body, title, MB_OK | MB_ICONWARNING);
    }
    if (app == nullptr)
        return;
    HWND main = app->MainWindow();
    if (main != nullptr)
        InvalidateRect(main, &app->ViewportRect(), FALSE);
    HWND floating = app->FloatingWindow();
    if (floating != nullptr)
        InvalidateRect(floating, nullptr, FALSE);
}

}  // namespace

void EnsureItem(MMDApp* app) {
    if (app == nullptr || app->MainWindow() == nullptr)
        return;
    HMENU menu = GetMenu(app->MainWindow());
    if (menu == nullptr)
        return;
    const int help = PopupPositionWithCommand(menu, kHelpAboutCommand);
    if (help < 0)
        return;
    HMENU popup = GetSubMenu(menu, help);
    if (popup == nullptr)
        return;
    g_helpPopup = popup;
    if (ItemIndexById(popup, kViewportCornerMenuCommand) < 0) {
        const int count = GetMenuItemCount(popup);
        if (count > 0 && GetMenuItemID(popup, UINT(count - 1)) != 0)
            AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING, kViewportCornerMenuCommand,
                    const_cast<wchar_t*>(MenuLabel(
                        app, viewport_corner_settings::Current())));
    }
    UpdateMenuLabel(app);
}

bool HandleCommand(MMDApp* app, unsigned command) {
    if (command != kViewportCornerMenuCommand)
        return false;
    const int current = static_cast<int>(viewport_corner_settings::Current());
    const int next = current >= 0 && current < 3 ? current + 1 : 0;
    ApplyCorner(app, static_cast<Corner>(next));
    return true;
}

}  // namespace viewport_corner_menu
}  // namespace mikudancestudio
