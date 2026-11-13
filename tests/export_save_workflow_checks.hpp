#pragma once

#include "mikumikustudio/background_save.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include <commdlg.h>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

namespace export_save_workflow {
namespace fs = std::filesystem;
namespace panel = mikumikustudio::panel;

struct Parameter {
    int id;
    enum Type { Text, Choice, CheckBox } type = Text;
};

inline std::vector<std::wstring> Capture(HWND dialog, std::initializer_list<Parameter> fields) {
    std::vector<std::wstring> values;
    for (const auto& field : fields) {
        const auto control = GetDlgItem(dialog, field.id);
        if (field.type == Parameter::CheckBox) {
            values.push_back(std::to_wstring(IsDlgButtonChecked(dialog, field.id)));
        } else if (field.type == Parameter::Choice) {
            const auto selected = SendMessageW(control, CB_GETCURSEL, 0, 0);
            values.push_back(std::to_wstring(selected));
        } else {
            const auto length = GetWindowTextLengthW(control);
            std::wstring value(length + 1, L'\0');
            value.resize(GetWindowTextW(control, value.data(), length + 1));
            values.push_back(std::move(value));
        }
    }
    return values;
}

class SaveRetry {
public:
    using Require = void(*)(bool, const char*);

    bool OwnsPicker(const OPENFILENAMEW* picker) const {
        return dialog_ && picker && picker->hwndOwner == dialog_ && thread_ == GetCurrentThreadId();
    }

    BOOL Pick(OPENFILENAMEW* picker) {
        require_(picker->lpstrDefExt && std::wcscmp(picker->lpstrDefExt, L"pmm") == 0 &&
                 (picker->Flags & OFN_OVERWRITEPROMPT), "project Save As is owned by the export settings");
        ++picks_;
        return wcscpy_s(picker->lpstrFile, picker->nMaxFile, target_.c_str()) == 0;
    }

    bool AcceptFailure(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
        if (!expectFailure_ || owner != main_ || thread_ != GetCurrentThreadId() || !title ||
            std::wcscmp(title, L"PMM") != 0) return false;
        require_(text && *text && flags == (MB_OK | MB_ICONERROR) && ++errors_ == 1,
                 "the real PMM failure is shown once");
        return true;
    }

    void Drive(mikumikustudio::MMDApp& app, HWND dialog, const fs::path& folder,
               std::initializer_list<Parameter> fields, Require require) {
        require_ = require;
        dialog_ = dialog;
        main_ = app.MainWindow();
        thread_ = GetCurrentThreadId();
        picks_ = errors_ = 0;
        const auto shown = [](HWND control) { return (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0; };
        const auto warning = GetDlgItem(dialog, panel::kExportUnsavedWarning);
        const auto save = GetDlgItem(dialog, panel::kExportSaveProjectButton);
        const auto click = [dialog](int id) {
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dialog, id)));
        };
        click(panel::kAviCloseAfterExportCheckbox);
        require_(app.SceneModified() && !app.EnvFileName()[0] && shown(warning) && shown(save) && IsWindowEnabled(save),
                 "close-after-export shows Save for the unsaved project");
        const auto savedParameters = Capture(dialog, fields);
        const auto generation = app.SceneGeneration();
        require_(fs::create_directory(folder), "own a fresh project-save fixture directory");
        const auto blocker = folder / L"blocked";
        constexpr char marker[] = "owned file occupying the requested folder name";
        {
            std::ofstream file(blocker, std::ios::binary);
            file << marker;
            file.close();
            require_(bool(file), "put a real file at the failed target's parent path");
        }
        const auto saveProject = [&](bool fail) {
            expectFailure_ = fail;
            click(panel::kExportSaveProjectButton);
            require_(mikumikustudio::SceneSaveInProgress() && !IsWindowEnabled(save),
                     "the save button is disabled while the real background job is active");
            const auto deadline = GetTickCount64() + 15000;
            do {
                MSG message{};
                if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    require_(message.message != WM_QUIT, "the editor does not quit during project save");
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                } else {
                    Sleep(1);
                }
                require_(IsWindow(dialog) && IsWindow(main_), "the same settings dialog remains open through save");
                require_(GetTickCount64() < deadline, "wait for the actual save completion and dialog timer");
            } while (mikumikustudio::SceneSaveInProgress() || (fail ? !IsWindowEnabled(save) : shown(warning)));
            expectFailure_ = false;
            require_(app.SceneGeneration() == generation && Capture(dialog, fields) == savedParameters,
                     "the same project and all entered export settings survive the save");
        };

        target_ = blocker / L"failed.pmm";
        saveProject(true);
        std::ifstream original(blocker, std::ios::binary);
        const std::string bytes(std::istreambuf_iterator<char>(original), {});
        require_(bytes == marker && !fs::exists(target_) && picks_ == 1 && errors_ == 1 &&
                 app.SceneModified() && !app.EnvFileName()[0] && shown(warning) && shown(save),
                 "failed Save As preserves the file, unsaved warning, and unnamed project");

        target_ = folder / L"saved-project.pmm";
        saveProject(false);
        require_(picks_ == 2 && errors_ == 1 && !app.SceneModified() && !shown(warning) && !shown(save) &&
                 fs::path(app.EnvFileName()) == target_ && fs::is_regular_file(target_),
                 "retry saves the project and dismisses the warning without reopening settings");
        std::puts("EXPORT-SAVE retry saved PMM; parameters retained through failure and success");
        dialog_ = nullptr;
    }

private:
    HWND dialog_ = nullptr, main_ = nullptr;
    DWORD thread_ = 0;
    fs::path target_;
    Require require_ = nullptr;
    unsigned picks_ = 0, errors_ = 0;
    bool expectFailure_ = false;
};
} // namespace export_save_workflow
