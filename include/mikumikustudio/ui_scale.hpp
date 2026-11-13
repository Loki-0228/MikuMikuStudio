#pragma once

namespace mikumikustudio {
class MMDApp;
namespace ui_scale {

inline constexpr unsigned kMenuCommand = 32817;
inline constexpr int kDialog = 9170;
inline constexpr int kSlider = 9171;
inline constexpr int kReadout = 9172;

int Percent() noexcept;
int Pixels(int designPixels) noexcept;
void ApplyAtStartup();
void RefreshMenu(MMDApp*);
bool HandleCommand(MMDApp*, unsigned command);

}  // namespace ui_scale
}  // namespace mikumikustudio
