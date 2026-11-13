// Included by autosave_tests.cpp for the isolated directory-permission mode.

class DenyNewFiles {
public:
    explicit DenyNewFiles(const fs::path& path) {
        directory = CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        Check(directory != INVALID_HANDLE_VALUE && GetFileInformationByHandle(directory, &info) &&
              !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
              "open ACL for the fixture directory itself");
        Check(GetSecurityInfo(directory, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
              nullptr, nullptr, &original, nullptr, &descriptor) == ERROR_SUCCESS && original,
              "save fixture directory DACL");
        HANDLE token = nullptr;
        DWORD size = 0;
        Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), "read test process token");
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> user(size);
        Check(size && GetTokenInformation(token, TokenUser, user.data(), size, &size),
              "read the current test user's SID");
        CloseHandle(token);
        EXPLICIT_ACCESSW deny{};
        deny.grfAccessPermissions = FILE_ADD_FILE;
        deny.grfAccessMode = DENY_ACCESS;
        BuildTrusteeWithSidW(&deny.Trustee, reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
        PACL restricted = nullptr;
        Check(SetEntriesInAclW(1, &deny, original, &restricted) == ERROR_SUCCESS,
              "deny file creation for the fixture user on this directory only");
        const DWORD result = SetSecurityInfo(directory, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, restricted, nullptr);
        LocalFree(restricted);
        Check(result == ERROR_SUCCESS, "apply the fixture's non-inherited denial");
    }
    DenyNewFiles(const DenyNewFiles&) = delete;
    DenyNewFiles& operator=(const DenyNewFiles&) = delete;
    ~DenyNewFiles() { Restore(); }

    void Restore() {
        if (directory == INVALID_HANDLE_VALUE) return;
        const auto result = SetSecurityInfo(directory, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, original, nullptr);
        LocalFree(descriptor);
        CloseHandle(directory);
        directory = INVALID_HANDLE_VALUE;
        Check(result == ERROR_SUCCESS, "restore the fixture directory DACL before retry");
    }

private:
    HANDLE directory = INVALID_HANDLE_VALUE;
    PACL original = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
};

auto DirectoryContents(const fs::path& directory) {
    std::map<fs::path, std::string> entries;
    for (const auto& entry : fs::directory_iterator(directory)) {
        Check(entry.is_regular_file(), "the save fixture contains only ordinary files");
        entries.emplace(entry.path().filename(), Bytes(entry.path()));
    }
    return entries;
}

void CheckCreateDenied(const fs::path& directory) {
    const auto path = directory / L"new-file-probe";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    const DWORD error = GetLastError();
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    std::printf("ACL_CREATE error=%lu\n", error);
    Check(file == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED,
          "Windows really rejects creation in this fixture");
}

void CheckDirectoryPermissions(MMDApp* app) {
    const auto manualDir = root / L"manual";
    const auto backupDir = root / L"periodic";
    Check(fs::create_directory(manualDir) && fs::create_directory(backupDir),
          "create both ACL fixtures in the new test root");
    as::Settings value;
    value.enabled = true;
    value.maxBackups = 1;
    value.overwriteOldest = true;
    value.directory = backupDir.wstring();
    Configure(app, value);
    const auto manual = manualDir / L"project.pmm";
    LoadModelFile(app, (root / L"fixture" / L"fixture.pmd").c_str(), false);
    Check(app->ModelSlot(0) != nullptr, "load synthetic PMD for the save workflow");
    wcscpy_s(app->EnvFileName(), 256, manual.c_str());
    app->CurrentFrame() = 11;
    app->SceneModified() = 1;
    Check(SaveSceneFile(app), "save the original project");
    SeedPluginData(app, manual);
    const auto original = DirectoryContents(manualDir);
    Check(as::QueueBackup(app), "queue a first periodic backup");
    background_save::Wait(app);
    const auto first = as::Backups(backupDir);
    Check(first.size() == 1, "one complete backup exists before the permission change");
    CheckPluginData(first.front());
    const auto previousBackups = DirectoryContents(backupDir);
    app->CurrentFrame() = 28;
    app->CameraDistance() = -77;
    app->SceneModified() = 1;
    {
        DenyNewFiles deny(manualDir);
        CheckCreateDenied(manualDir);
        expectPermissionSaveError = true;
        Check(background_save::Request(app, manual.c_str()), "queue manual save against the denied directory");
        background_save::Wait(app);
        expectPermissionSaveError = false;
        Check(permissionSaveErrors == 1 && !SceneSaveInProgress() && app->SceneModified() &&
              fs::path(app->EnvFileName()) == manual && app->CurrentFrame() == 28 &&
              DirectoryContents(manualDir) == original,
              "failed manual save keeps the original pair and current edit");
    }
    Check(background_save::Request(app, manual.c_str()), "retry the same manual target after ACL restoration");
    background_save::Wait(app);
    Check(!SceneSaveInProgress() && !app->SceneModified() && DirectoryContents(manualDir) != original,
          "the retried manual save publishes the edited project");
    CheckPluginData(manual);
    LoadSceneFile();
    Check(app->CurrentFrame() == 28 && app->CameraDistance() == -77, "reopen the retried manual save");
    const auto saved = DirectoryContents(manualDir);

    app->CurrentFrame() = 42;
    app->CameraDistance() = -96;
    app->SceneModified() = 1;
    const auto due = GetTickCount64() + 60001;
    {
        DenyNewFiles deny(backupDir);
        CheckCreateDenied(backupDir);
        expectedFullWarning = true;
        as::Tick(app, due);
        expectedFullWarning = false;
        Check(fullWarnings == 1, "the expected one-time capacity warning remains available");
        Check(SceneSaveInProgress(), "timer queues a worker while backup file creation is denied");
        background_save::Wait(app);
        Check(!SceneSaveInProgress() && std::wstring_view(background_save::StatusText(app)) ==
              L"备份失败，请检查备份位置。", "timer reports a failed backup rather than a completed overwrite");
        Check(DirectoryContents(backupDir) == previousBackups && DirectoryContents(manualDir) == saved &&
              app->SceneModified() && fs::path(app->EnvFileName()) == manual,
              "failed periodic backup keeps all original files and the active edit");
    }
    as::Tick(app, due + 60001);
    Check(SceneSaveInProgress(), "next due interval queues a retry after permission restoration");
    background_save::Wait(app);
    const auto retried = as::Backups(backupDir);
    Check(retried.size() == 1 && retried.front() != first.front() && !fs::exists(first.front()) &&
          DirectoryContents(backupDir).size() == previousBackups.size() &&
          std::wstring_view(background_save::StatusText(app)) == L"备份完成，已覆盖旧备份",
          "successful backup replaces exactly the previous generation");
    CheckPluginData(retried.front());
    Check(!SceneSaveInProgress() && app->SceneModified() && fs::path(app->EnvFileName()) == manual &&
          DirectoryContents(manualDir) == saved, "backup does not become the active project");
    wcscpy_s(app->EnvFileName(), 256, retried.front().c_str());
    LoadSceneFile();
    Check(app->CurrentFrame() == 42 && app->CameraDistance() == -96 && app->ModelSlot(0) &&
          DirectoryContents(manualDir) == saved, "reopen the retried backup and preserve the manual project");
    std::printf("PASS real directory permission denial, manual and timed backup retry and reopen (%u checks)\n", checks);
}

INT_PTR CALLBACK AcceptBackupDirectory(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    const auto result = as::DialogProc(dialog, message, wp, lp);
    if (message == WM_INITDIALOG) {
        auto& context = *reinterpret_cast<as::DialogContext*>(lp);
        Check(context.value.directory == (root / L"unavailable").native(),
              "backup settings opens with the failed directory still selected");
        // Supply only the folder-picker result; the actual OK command writes settings.
        context.value.directory = (root / L"replacement").native();
        SetDlgItemTextW(dialog, as::Folder, as::DisplayDirectory(context.value.directory).c_str());
        PostMessageW(dialog, WM_COMMAND, IDOK, 0);
    }
    return result;
}

void ReopenRetriedBackup(MMDApp* app, const fs::path& backup) {
#if !MMD_ORIGINAL_PROFILE
    const auto main = app->MainWindow();
    SendMessageW(main, WM_ENTERMENULOOP, FALSE, 0);
    const auto file = GetSubMenu(GetMenu(main), 0);
    MENUITEMINFOW open{sizeof(open)};
    open.fMask = MIIM_SUBMENU;
    Check(GetMenuItemInfoW(file, 205, FALSE, &open) && open.hSubMenu,
          "open the production recent projects menu");
    SendMessageW(main, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(open.hSubMenu), 0);
    UINT command = 0;
    for (int i = 0; i < GetMenuItemCount(open.hSubMenu); ++i) {
        wchar_t label[512]{};
        GetMenuStringW(open.hSubMenu, i, label, std::size(label), MF_BYPOSITION);
        if (!std::wcsstr(label, backup.c_str())) continue;
        Check(!(GetMenuState(open.hSubMenu, i, MF_BYPOSITION) & (MF_DISABLED | MF_GRAYED)),
              "the backup already opened once remains actionable in recent projects");
        command = GetMenuItemID(open.hSubMenu, i);
        break;
    }
    SendMessageW(main, WM_EXITMENULOOP, FALSE, 0);
    Check(command && command != UINT(-1), "find the backup by full path in recent projects");
    SendMessageW(main, WM_COMMAND, command, 0);
#else
    Check(LoadSceneFile(backup.c_str()), "reopen the backup through the original profile's project reader");
#endif
}

void CheckBackupDirectoryRetry(MMDApp* app) {
    const auto manual = root / L"manual.pmm";
    const auto oldDir = root / L"unavailable";
    const auto heldDir = root / L"previous-backups";
    const auto newDir = root / L"replacement";
    const auto history = root / L"recent.txt";
    LoadModelFile(app, (root / L"fixture" / L"fixture.pmd").c_str(), false);
    Check(app->ModelSlot(0), "load the isolated PMD used by both backup generations");
    wcscpy_s(app->EnvFileName(), 256, manual.c_str());
    app->CurrentFrame() = 11;
    app->SceneModified() = 1;
    Check(SaveSceneFile(app), "save the real source project before starting the timer");
    SeedPluginData(app, manual);
    const auto original = Bytes(manual);
    const auto recentBefore = Bytes(history);
    as::Settings value;
    value.enabled = true;
    value.directory = oldDir.native();
    Configure(app, value);
    Check(as::QueueBackup(app), "queue a successful backup before the location fails");
    background_save::Wait(app);
    const auto oldFiles = DirectoryContents(oldDir);
    Check(!oldFiles.empty() && as::Backups(oldDir).size() == 1, "retain a real previous backup");
    fs::rename(oldDir, heldDir);
    std::ofstream(oldDir, std::ios::binary) << "unavailable folder";

    app->CurrentFrame() = 42;
    app->CameraDistance() = -96;
    app->SceneModified() = 1;
    const auto generation = app->SceneGeneration();
    as::Tick(app, GetTickCount64() + 60001);
    background_save::Wait(app);
    Check(std::wstring_view(background_save::StatusText(app)) == L"备份失败，请检查备份位置。",
          "an unavailable folder reports failure even before a worker can be started");
    Check(!SceneSaveInProgress() && app->SceneGeneration() == generation && app->SceneModified() &&
          fs::path(app->EnvFileName()) == manual && Bytes(manual) == original && Bytes(history) == recentBefore &&
          Bytes(oldDir) == "unavailable folder" && DirectoryContents(heldDir) == oldFiles,
          "the failed timer leaves the active edit, project, recent list and previous backup intact");

    as::DialogContext settings{app, as::Current()};
    Check(UiDialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(as::kDialog), app->MainWindow(),
              AcceptBackupDirectory, reinterpret_cast<LPARAM>(&settings)) == IDOK &&
          as::Current().directory == newDir.native(), "the settings OK command commits the replacement directory");
    as::Tick(app, GetTickCount64() + 60001);
    background_save::Wait(app);
    const auto backups = as::Backups(newDir);
    Check(backups.size() == 1 && app->SceneModified() && app->SceneGeneration() == generation &&
          fs::path(app->EnvFileName()) == manual && Bytes(history) == recentBefore &&
          std::wstring_view(background_save::StatusText(app)) == L"备份完成",
          "the next scheduled backup saves to the new folder without changing the current project or history");
    const auto backup = backups.front();
    CheckPluginData(backup);
    Check(LoadSceneFile(backup.c_str()) && app->CurrentFrame() == 42 && app->CameraDistance() == -96,
          "the first explicit open restores the saved backup edit and adds it to recent projects where supported");
    Check(LoadSceneFile(manual.c_str()) && app->CurrentFrame() == 11 && !app->SceneModified(),
          "switch back to the unchanged original before testing the reopen command");
    ReopenRetriedBackup(app, backup);
    Check(fs::path(app->EnvFileName()) == backup && app->CurrentFrame() == 42 && app->CameraDistance() == -96 &&
          app->ModelSlot(0) && !app->SceneModified() && Bytes(manual) == original && DirectoryContents(heldDir) == oldFiles,
          "the retried project reopens with its edit, model and unchanged source versions");
#if !MMD_ORIGINAL_PROFILE
    const auto roundtrip = root / L"reopened-copy.pmm";
    Check(SaveSceneCopy(app, roundtrip.c_str()), "serialize the restored live plugin project after reopening from recent projects");
    CheckPluginData(roundtrip);
#endif
    std::printf("PASS failed backup location, settings change, timed retry and reopen (%u checks)\n", checks);
}
