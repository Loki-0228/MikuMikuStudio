// Runs in its own hidden host so no earlier model load or test edit marks the scene dirty.
void CheckEmptyProjectBackup(MMDApp* app) {
    Check(!*app->EnvFileName() && !app->SceneModified(), "new project starts unnamed and unmodified");
    as::Settings value; value.enabled = true; value.maxBackups = 20;
    value.directory = (root / L"periodic").wstring(); Configure(app, value);
    const auto count = [&] { return as::Backups(value.directory).size(); };
    const auto tick = [&] {
        const auto now = GetTickCount64();
        as::Initialize(app); as::Tick(app, now + 60001);
        background_save::Wait(app);
    };
    tick();
    std::printf("EMPTY_AUTOSAVE fresh_backup_count=%zu\n", count());
    Check(count() == 0, "unmodified empty project does not trigger a periodic backup");
    app->CurrentFrame() = 35; tick();
    Check(count() == 0, "seeking an empty project does not create a backup");
    Check(!as::QueueBackup(app), "direct periodic queue also skips an empty project");
    Check(as::BackupNow(app) == as::Result::Empty, "synchronous periodic entry also skips an empty project");
    Check(!fs::exists(fs::path(value.directory) / L".mms-autosave.lock"), "empty project creates no worker or folder lock");
    app->CameraPositionX() = 2.5f; app->SceneModified() = 1;
    tick();
    Check(count() == 1 && app->SceneModified(), "camera-only edits are backed up without clearing dirty state");
    // Removing the last model still represents an edit, not a fresh empty scene.
    LoadModelFile(app, (root / L"fixture" / L"fixture.pmd").c_str(), false);
    Check(app->ModelSlot(0) != nullptr, "load a real model before deleting it");
    expectedWarning = true;
    CommandDispatch(nullptr, MAKEWPARAM(437, 0));
    expectedWarning = false;
    bool models = false;
    for (int i = 0; i < kModelSlotCount; ++i) models |= app->ModelSlot(i) != nullptr;
    Check(!models && app->SceneModified(), "deleting the last model retains modified state");
    tick();
    Check(count() == 2, "deleting all models does not suppress recovery of that edit");
    ResetAppState(app);
    Check(!*app->EnvFileName() && !app->SceneModified(), "New returns to an unnamed unmodified project");
    tick();
    Check(count() == 2, "New stops empty backups after a previously edited project");
    const auto manual = root / L"explicitly-saved-empty.pmm";
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, manual.c_str());
    Check(SaveSceneFile(app), "user may still manually save an empty project");
    tick();
    Check(count() == 3, "an explicitly saved project retains periodic backup behavior");
    ResetAppState(app);
    Check(as::BackupNow(app, true) == as::Result::Saved, "emergency backup path remains available");
    Check(!IsWindowVisible(app->MainWindow()), "empty-project regression stays hidden");
}
