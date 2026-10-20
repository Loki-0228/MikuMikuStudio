#include "mikumikustudio/audio_menu.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "../plugins/native_audio_history.hpp"

namespace mikumikustudio::audio_menu {
void EnsureItem(MMDApp* app) noexcept {
    if (!app || !app->MainWindow()) return;
    const auto root = GetMenu(app->MainWindow());
    for (int i = 0; i < GetMenuItemCount(root); ++i) {
        const auto file = GetSubMenu(root, i);
        if (!file || GetMenuState(file, 206, MF_BYCOMMAND) == UINT(-1)) continue;
        const wchar_t* label = app->EnglishUI() == kUiChinese ? L"移除音频" :
            app->EnglishUI() == kUiJapanese ? L"音声を削除" : L"Remove audio";
        if (GetMenuState(file, kRemoveCommand, MF_BYCOMMAND) == UINT(-1)) {
            const int count = GetMenuItemCount(file);
            for (int item = 0; item < count; ++item) if (GetMenuItemID(file, item) == 206) {
                if (!InsertMenuW(file, UINT(item + 1), MF_BYPOSITION | MF_STRING, kRemoveCommand, label)) return;
                break;
            }
        }
        ModifyMenuW(file, kRemoveCommand, MF_BYCOMMAND | MF_STRING |
            (app->WaveEnabled() ? MF_ENABLED : MF_GRAYED), kRemoveCommand, label);
        return;
    }
}
bool HandleCommand(MMDApp* app, unsigned command) noexcept {
    if (command != kRemoveCommand) return false;
    if (!app) return true;
    if (!plugins::TryClearBoundAudio(app)) plugins::ClearLegacyAudio(app);
    EnsureItem(app);
    return true;
}
}
#endif
