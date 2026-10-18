// End-to-end PMM loading, frame evaluation and recovery in the real MME host.
#include <malloc.h>
LRESULT CALLBACK ResolveImportDialog(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) {
        const auto* message = reinterpret_cast<const CWPRETSTRUCT*>(lp);
        if (message->message == WM_INITDIALOG && GetDlgItem(message->hwnd, 687)) {
            ++projectImportWarnings;
            // Adapt a changed model; skip a missing model. Never open a picker.
            PostMessageW(message->hwnd, WM_COMMAND, GetDlgItem(message->hwnd, 690) ? 687 : 688, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
void CheckProjectImport(MMDApp* app, const wchar_t* input, const wchar_t* output) {
    projectImportAudit = true;
    const auto hook = SetWindowsHookExW(WH_CALLWNDPROCRET, ResolveImportDialog, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "resolve import dialogs without activation");
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, input);
    std::puts("IMPORT begin");
    LoadSceneFile();
    std::puts("IMPORT returned");
    if (const auto* expected = std::getenv("MMS_IMPORT_WARNINGS"))
        Check((projectImportWarnings != 0) == (std::atoi(expected) != 0), "import warning matches fixture expectation");
    Check(app->SelectedModelSlot() < kModelSlotCount, "selected model slot stays within the model table");
    Check(app->state.optflag[0] != 0 || app->SelectedModel() != nullptr, "model mode always has a live selected model");
    const auto loadWarnings = projectImportWarnings;
    const auto evaluate = [&] {
        for (const int frame : {0, 1, 44, 200, 0}) {
            app->CurrentFrame() = frame;
            RefreshAfterFrameApply(app);
            app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10;
            FrameDriver(app);
        }
        Check(_heapchk() == _HEAPOK, "import and frame evaluation preserve the heap");
        Check(projectImportWarnings == loadWarnings, "no delayed load errors appear during seeking or MME rendering");
    };
    evaluate();
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, output);
    SaveSceneFile(app);
    Check(std::filesystem::is_regular_file(output), "imported or recovered scene can be saved");
    // Native MME writes the paired EMM on its next frame, like the main loop.
    app->state.messageSeen = 10; FrameDriver(app);
    const auto warnings = projectImportWarnings;
    LoadSceneFile();
    Check(projectImportWarnings == warnings, "saved scene reloads without new warnings");
    evaluate();
    // Rejected candidates preserve the currently open scene's save identity.
    const std::wstring previousFile(app->EnvFileName());
    const std::wstring previousDirectory(app->PathWorkspace().projectDirectory);
    app->SceneModified() = 1;
    const auto reject = [&](const std::wstring& path) {
        LoadSceneFile(path.c_str());
        Check(previousFile == app->EnvFileName() &&
              previousDirectory == app->PathWorkspace().projectDirectory && app->SceneModified() == 1,
              "failed project open preserves the old save target, path base and dirty state");
    };
    reject(std::wstring(output) + L".missing");
    const auto invalid = std::wstring(output) + L".invalid";
    { std::ofstream file(std::filesystem::path(invalid), std::ios::binary); file << "bad"; }
    reject(invalid);
    { std::array<char,30> header{};
      std::ifstream source(std::filesystem::path(output), std::ios::binary); source.read(header.data(), header.size());
      std::memcpy(header.data() + 20, "9999", 4);
      std::ofstream file(std::filesystem::path(invalid), std::ios::binary); file.write(header.data(), header.size()); }
    reject(invalid);
    std::filesystem::remove(invalid);
    UnhookWindowsHookEx(hook);
    std::puts("PASS project import, seeking, MME rendering and save/reload");
}
