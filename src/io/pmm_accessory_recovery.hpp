#pragma once

#include <cstring>
#include <iterator>
#include "mikumikustudio/accessory_import.hpp"

#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/ui_message.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "pmm_io_common.hpp"
#include "project_relocation_context.hpp"

namespace mikumikustudio::pmm_io {

// Skip retains the initialized record so the parser can still read its track.
// A cancel leaves the parser responsible for closing input and resetting the scene.
inline bool LoadProjectAccessory(MMDApp* app, mdl::AccessoryRecord* record,
                                 const char* storedPath, const char* name, PathResolutionWorkspace& paths,
                                 const std::filesystem::path& projectDirectory) {
    const auto load = [&](const wchar_t* path) {
        if (!path || !*path) return false;
        const DWORD attributes = GetFileAttributesW(path);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        if (accessory_import::LoadObject(app, record, path, accessory_import::MissingFileNotice::Suppress))
            return true;

        // A failed X import may already own a mesh, material or texture array.
        DisposeAccessory(record);
        std::memset(record, 0, sizeof(*record));
        Sub04B0Init(record);
        return false;
    };
    const auto relocation = project_relocation::LoadScope::Accepted(*app);
    const auto reference = relocation.Locate(storedPath, projectDirectory);
    if (load(reference.exactPath.c_str())) return true;
    if (reference.tryLegacy) {
        wchar_t source[kPathFieldCapacity]{};
        if (accessory_import::ResolveProjectReference(storedPath, source, std::size(source), paths) && load(source))
            return true;
    }

    const HWND main = app->MainWindow();
    const std::wstring label = text_encoding::Display(name);
    for (;;) {
        const INT_PTR answer = ShowProjectModelDialog(app, static_cast<HINSTANCE>(app->HInstance()), main,
            ProjectModelIssue::MissingAccessory, label.c_str());
        if (answer == 3) return true;
        if (answer != 1) return false;

        SetCurrentDirectoryW(reinterpret_cast<const wchar_t*>(app->state.exeDir));
        wchar_t file[256]{}, title[256]{};
        using project_relocation::LoadScope;
        constexpr auto pickerKind = LoadScope::Picker::Accessory;
        const bool remember = LoadScope::RememberPickerDirectory(main);
        OPENFILENAMEW picker{};
        picker.lStructSize = sizeof(picker);
        picker.hwndOwner = app->FloatingWindow() ? app->FloatingWindow() : main;
        picker.lpstrFilter = app->EnglishUI() == kUiJapanese ? kWFilterAccJp : kWFilterAccEn;
        picker.lpstrFile = file;
        picker.nMaxFile = std::size(file);
        picker.lpstrFileTitle = title;
        picker.nMaxFileTitle = std::size(title);
        picker.lpstrInitialDir = remember ? LoadScope::PickerDirectory(*app, pickerKind) : kWUserAcc;
        picker.lpstrDefExt = L"x";
        picker.lpstrTitle = app->EnglishUI() == kUiChinese ? L"加载附件"
            : app->EnglishUI() == kUiJapanese ? L"アクセサリ読み込み" : L"Load accessory";
        picker.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetUiOpenFileNameW(&picker) || !load(file)) continue;

        relocation.Confirm(reference, record->sourcePath);
        if (remember) LoadScope::StagePickerDirectory(*app, pickerKind, file);
        return true;
    }
}

}  // namespace mikumikustudio::pmm_io
