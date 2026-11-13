#pragma once
#include "mikumikustudio/image_sequence_export.hpp"
#include <Windows.h>
#include <cstdint>

namespace mikumikustudio::image_sequence {
constexpr int kDialog = 9100;
enum Control {
    Folder = 9101, Browse, Prefix, FormatCombo, Example,
    FolderLabel, PrefixLabel, FormatLabel, SizeLabel, FpsLabel, RangeLabel,
};
struct DialogContext {
    MMDApp* app;
    std::uint64_t scene;
    Options options;
    bool updating = false;
};
INT_PTR CALLBACK DialogProc(HWND, UINT, WPARAM, LPARAM);
}
