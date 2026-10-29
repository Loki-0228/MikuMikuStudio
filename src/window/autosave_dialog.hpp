#pragma once
#include "mikumikustudio/autosave.hpp"
namespace mikumikustudio::autosave {
inline constexpr int kDialog = 9120;
enum Control { Folder = 9121, Browse, Interval, Limit, Overwrite, FolderLabel,
    IntervalLabel, LimitLabel, Explanation, Enabled, KeepPrevious, NumberedManual, BackupNaming, IncrementNaming };
inline constexpr int kNamingDialog = 9162; // Distinct from the viewport render scale dialog (9140).
enum NamingControl { Pattern = 9141, Preview, PatternLabel, PreviewLabel, NamingHint, TokenFirst = 9150 };
struct NamingContext { MMDApp* app; std::wstring pattern, name; bool backup; HFONT previewFont = nullptr; };
INT_PTR CALLBACK NamingProc(HWND, UINT, WPARAM, LPARAM);
struct DialogContext { MMDApp* app; Settings value; HWND tooltip = nullptr; };
INT_PTR CALLBACK DialogProc(HWND, UINT, WPARAM, LPARAM);
}
