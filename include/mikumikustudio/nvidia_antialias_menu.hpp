#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace mikumikustudio {
class MMDApp;
namespace nvidia_antialias_menu {
inline constexpr unsigned kInstallCommand = 34920;
inline constexpr unsigned kUninstallCommand = 34921;
bool HandleMessage(MMDApp* app, UINT message, WPARAM wp, LPARAM lp);
}
}
