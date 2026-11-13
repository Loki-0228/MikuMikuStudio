#include "mikumikustudio/picture_menu.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/mmd_app.hpp"
#include "../plugins/native_picture_history.hpp"
namespace mikumikustudio::picture_menu {
bool EnsureItem(MMDApp* app) noexcept {
    if (!app || !app->MainWindow()) return false;
    const auto root = GetMenu(app->MainWindow());
    for (int i = 0; i < GetMenuItemCount(root); ++i) {
        const auto menu = GetSubMenu(root, i);
        if (!menu || GetMenuState(menu, 233, MF_BYCOMMAND) == UINT(-1)) continue;
        const wchar_t* label = app->EnglishUI() == 2 ? L"移除背景图片" :
            app->EnglishUI() ? L"Remove background picture" : L"背景画像を削除";
        if (GetMenuState(menu, kRemoveCommand, MF_BYCOMMAND) == UINT(-1)) {
            for (int index = 0; index < GetMenuItemCount(menu); ++index) if (GetMenuItemID(menu, index) == 233) {
                if (!InsertMenuW(menu, UINT(index + 1), MF_BYPOSITION | MF_STRING, kRemoveCommand, label)) return false;
                break;
            }
        }
        return ModifyMenuW(menu, kRemoveCommand, MF_BYCOMMAND | MF_STRING |
            (app->PictureBackgroundTexture() ? MF_ENABLED : MF_GRAYED), kRemoveCommand, label) != FALSE;
    }
    return false;
}
bool HandleCommand(MMDApp* app, unsigned command) noexcept {
    if (command != kRemoveCommand) return false;
    if (!app) return true;
    if (!plugins::TryClearBoundPicture(app)) plugins::ClearLegacyPicture(app);
    EnsureItem(app); return true;
}
}
#endif
