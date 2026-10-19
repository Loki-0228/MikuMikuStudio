#pragma once

#include <Windows.h>

namespace mikumikustudio {
// Preserve UTF-16 captions even after a legacy plugin installs an ANSI proc.
void SetUnicodeWindowTitle(HWND window, const wchar_t* title);
}
