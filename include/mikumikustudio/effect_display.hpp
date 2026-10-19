#pragma once
#include <Windows.h>
#include <string>
namespace mikumikustudio {
class MMDApp;
// Read the plugin's unchanged item data and text; return a paint-only label.
std::wstring EffectListDisplayLabel(MMDApp* app, HWND list, int row);
void InstallEffectListDisplay(HWND editor, MMDApp* app);
}
