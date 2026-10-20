#pragma once
#include "mikumikustudio/autosave.hpp"
namespace mikumikustudio::autosave {
inline constexpr int kDialog = 9120;
enum Control { Folder = 9121, Browse, Interval, Limit, Overwrite, FolderLabel,
    IntervalLabel, LimitLabel, Explanation, Enabled, KeepPrevious };
struct DialogContext { MMDApp* app; Settings value; };
INT_PTR CALLBACK DialogProc(HWND, UINT, WPARAM, LPARAM);
}
