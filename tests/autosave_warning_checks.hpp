// Included in autosave_tests.cpp's anonymous namespace. Run twice against the
// same disposable profile to exercise a real process restart with full folders.
void CheckFullWarnings(MMDApp* app, const fs::path& config, const std::wstring& mode) {
    const bool rotation = mode == L"--full-rotate";
    const bool crashOnly = mode == L"--full-crash";
    app->EnglishUI() = mode == L"--full-ja" ? 0 : mode == L"--full-en" ? 1 : 2;
    RefreshMenuLanguage(app);
    const bool restart = fs::exists(config);
    if (!restart) {
        auto value = as::Current();
        value.directory = (root / L"自动备份").wstring();
        value.enabled = true;
        value.maxBackups = 1;
        value.overwriteOldest = rotation;
        Configure(app, value);
        app->SceneModified() = 1; // Seed with a real edit; pristine empty scenes are intentionally skipped.
        if (!crashOnly) Check(as::BackupNow(app) == as::Result::Saved, "seed periodic folder to its capacity");
        Check(as::BackupNow(app, true) == as::Result::Saved, "seed crash folder to its capacity");
        value.enabled = false;
        Configure(app, value);
        if (crashOnly) Check(fs::remove(value.directory), "remove empty periodic folder to test independent crash scan");
    }
    Check(!as::Current().enabled && as::Current().maxBackups == 1 && as::Current().overwriteOldest == rotation,
        "full-folder startup reads the persisted backup policy");
    auto now = GetTickCount64();
    app->PlaybackActive() = 1;
    as::Tick(app, now);
    Check(as::FolderFull() && fullWarnings == 0, "full folder is detected during playback without interrupting it");
    now += 6000;
    FailNextAllocation([&] { as::Tick(app, now); },
        "capacity scan allocation failure cannot escape the main loop");
    Check(as::FolderFull() && fullWarnings == 0,
        "failed scan preserves the last known full status without interrupting playback");
    app->PlaybackActive() = 0;
    EnableWindow(app->MainWindow(), FALSE);
    as::Tick(app, now + 1);
    Check(fullWarnings == 0, "disabled owner defers the warning while another dialog is open");
    EnableWindow(app->MainWindow(), TRUE);
    {
        ScopedSceneMutation mutation(*app);
        as::Tick(app, now + 2);
        Check(fullWarnings == 0, "project mutation defers the warning");
    }
    FailNextAllocation([&] { as::Tick(app, now + 3); },
        "capacity message allocation failure cannot escape the main loop");
    Check(fullWarnings == 0 && as::FolderFull(), "failed warning construction keeps the warning eligible for retry");
    failFullWarningCreation = true;
    as::Tick(app, now + 4);
    failFullWarningCreation = false;
    Check(failedFullWarnings == 1 && fullWarnings == 0,
        "failed MessageBox creation does not consume the once-per-launch warning");
    expectedFullWarning = true;
    as::Tick(app, now + 5);
    expectedFullWarning = false;
    Check(fullWarnings == 1, "idle scan warns once even when periodic saving is disabled");
    Check(fullWarningText.find(crashDirectory.wstring()) != std::wstring::npos,
        "full crash directory is named in the warning");
    Check((fullWarningText.find(as::Current().directory) != std::wstring::npos) == !crashOnly,
        "only full folders are listed and a missing periodic folder does not hide crash capacity");
    const auto language = app->EnglishUI();
    Check(fullWarningText.find(rotation ? L"已开启覆盖旧备份" : language == 2 ? L"已关闭覆盖旧备份" :
        language == 0 ? L"古いバックアップの上書きが無効" : L"Overwrite oldest is disabled") != std::wstring::npos,
        "warning describes the actual rotation policy in the selected language");
    Check(fullWarningText.find(language == 2 ? L"本次启动不再提示" : language == 0 ?
        L"再起動するまで" : L"until you restart MMS") != std::wstring::npos,
        "warning explains its once-per-launch frequency");
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        as::Result result = as::Result::Busy;
        FailNextAllocation([&] { result = as::BackupNow(app, true); },
            "crash backup converts allocation failure to a failed attempt");
        Check(result == as::Result::Failed, "failed crash attempt releases its saving guard for the next attempt");
    }
    as::Tick(app, now + 6000);
    as::Tick(app, now + 12000);
    Check(fullWarnings == 1, "later scans of full folders stay quiet");
    auto value = as::Current();
    value.maxBackups = 2;
    Configure(app, value);
    as::Tick(app, now + 18000);
    Check(!as::FolderFull() && !RedMenuPixels(app), "raising capacity clears the menu warning");
    value.maxBackups = 1;
    Configure(app, value);
    as::Tick(app, now + 24000);
    Check(as::FolderFull() && fullWarnings == 1, "becoming full again in the same launch does not repeat the popup");
    as::Shutdown();
    as::Initialize(app);
    as::Tick(app, now + 30000);
    Check(fullWarnings == 1, "reinitializing the subsystem does not reset the process-local latch");
    Check(as::Backups(crashDirectory).size() == 1, "capacity warning does not delete or create backups");
    std::puts(restart ? "PASS fresh process warns again with an already-full folder" : "PASS initial process warns once");
}
