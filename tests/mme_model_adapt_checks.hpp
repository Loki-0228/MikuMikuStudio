// Exercise the real PMM structure-mismatch dialog, followed by MME rendering.
unsigned modelAdaptPrompts = 0;
LRESULT CALLBACK AcceptModelAdapt(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) {
        const auto* message = reinterpret_cast<const CWPRETSTRUCT*>(lp);
        if (message->message == WM_INITDIALOG && GetDlgItem(message->hwnd, 690)) {
            ++modelAdaptPrompts;
            std::puts("ADAPT selecting first model-mismatch option");
            PostMessageW(message->hwnd, WM_COMMAND, 687, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void CheckMmeModelAdapt(MMDApp* app, const wchar_t* directory) {
    const auto frames = [&] {
        for (int i = 0; i < 5; ++i) {
            app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10;
            FrameDriver(app);
        }
    };
    const auto project = std::filesystem::path(directory) / L"adapt.pmm";
    const auto backup = std::filesystem::path(directory) / L"backups/adapt_autosave.pmm";
    std::filesystem::create_directories(backup.parent_path());
    auto* model = app->SelectedModel();
    Check(model && mdl::Mdl(model)->boneCount > 0, "adapt fixture has a real bone");
    const auto modelPath = std::filesystem::path(mdl::Mdl(model)->path);
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, project.c_str());
    SaveSceneFile(app);
    Check(app->SceneModified() == 0 && std::filesystem::is_regular_file(project), "save mismatched PMM fixture");
    frames();
    // Only disposable fixtures supplied by the runner are replaced.
    Check(modelPath.parent_path() == std::filesystem::path(directory), "model replacement is inside the isolated test directory");
    std::filesystem::copy_file(std::filesystem::path(directory) / L"changed.pmx", modelPath,
        std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(project, backup);
    auto emm = project; emm.replace_extension(L".emm");
    auto backupEmm = backup; backupEmm.replace_extension(L".emm");
    if (std::filesystem::is_regular_file(emm)) std::filesystem::copy_file(emm, backupEmm);
    auto hook = SetWindowsHookExW(WH_CALLWNDPROCRET, AcceptModelAdapt, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "select real mismatch dialog without activation");
    for (const auto& path : {project, backup, project}) {
        wcscpy_s(app->EnvFileName(), kPathFieldCapacity, path.c_str());
        LoadSceneFile();
        Check(app->ModelSlot(0) != nullptr, "adapt keeps the model loaded");
        frames();
        Check(HeapValidate(GetProcessHeap(), 0, nullptr) != FALSE, "adapt leaves heap valid after rendered frames");
    }
    UnhookWindowsHookEx(hook);
    Check(modelAdaptPrompts == 3, "normal and backup PMM use the real adapt option");
    Check(!IsWindowVisible(app->MainWindow()), "adapt regression stays hidden");
}
