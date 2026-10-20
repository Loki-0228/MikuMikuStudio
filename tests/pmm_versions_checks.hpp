// Included in the hidden production save/export fixture namespace.
void VersionTests(MMDApp* app) {
    std::puts("SAVE_TEST phase=versions-configure");
    const auto target = root / L"版本配对.pmm";
    auto value = autosave::Current(); const auto previousSettings = value;
    value.keepPrevious = true;
    std::wstring error;
    Check(autosave::Configure(app, value, error), "enable PMM1 independent of periodic autosave");
    // wmain sets the Win32 override with SetEnvironmentVariableW; that does
    // not update the CRT's cached _wenviron used by _wgetenv. Read the exact
    // isolated path the fixture configured instead of constructing path(nullptr).
    const auto config = root / L"autosave.ini";
    Check(fs::is_regular_file(config) && autosave::LoadSettings(config).keepPrevious,
        "PMM1 option survives settings reload");
    std::puts("SAVE_TEST phase=versions-first-save");
    const auto effect = pmm_paths::Effect(target);
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, target.c_str());
    app->SceneModified() = 1;
    SaveCommand(app, 207); const auto oldest = Bytes(target);
    const float oldestFov = app->CameraFov();
#if !MMD_ORIGINAL_PROFILE
    const auto oldestSide = Bytes(plugins::PluginSidecar(target));
    const auto oldestPair = plugins::ReadPluginProject(target);
    Check(!oldestSide.empty() && !oldestPair.saveId.empty() && oldestPair.mainHash == plugins::FileHash(target),
        "initial PMM has its valid ABI sidecar pair");
#endif
    Check(!fs::exists(target.wstring()+L"1"), "first save has no invented previous version");
    std::ofstream(effect) << "original EMM";
    app->CameraFov() += 3; app->SceneModified() = 1;
    SaveCommand(app, 207); const auto middle = Bytes(target);
    const float middleFov = app->CameraFov();
#if !MMD_ORIGINAL_PROFILE
    const auto middleSide = Bytes(plugins::PluginSidecar(target));
    const auto middlePair = plugins::ReadPluginProject(target);
    const auto firstPair = plugins::ReadPluginProject(target.wstring()+L"1");
    Check(Bytes(plugins::PluginSidecar(target.wstring()+L"1")) == oldestSide &&
        firstPair.projectId == oldestPair.projectId && firstPair.saveId == oldestPair.saveId &&
        firstPair.mainHash == plugins::FileHash(target.wstring()+L"1") && middlePair.saveId != oldestPair.saveId,
        "PMM1 retains the exact old plugin bytes, identity and PMM hash");
#endif
    Check(Bytes(target.wstring()+L"1") == oldest && middle != oldest, "PMM1 preserves the previous saved state");
    Check(Bytes(pmm_paths::Effect(target.wstring()+L"1")) == Bytes(effect), "EMM1 is paired with PMM1");
    std::ofstream(effect) << "second EMM";
    app->CameraFov() += 2; app->SceneModified() = 1;
    SaveCommand(app, 207);
    const float latestFov = app->CameraFov();
    Check(Bytes(target.wstring()+L"1") == middle && Bytes(target.wstring()+L"11") == oldest,
        "repeated saves retain and shift all older versions");
    Check(Bytes(pmm_paths::Effect(target.wstring()+L"11")) == std::vector<char>({'o','r','i','g','i','n','a','l',' ','E','M','M'}),
        "shifting versions preserves the matching old EMM");
#if !MMD_ORIGINAL_PROFILE
    const auto version1 = plugins::ReadPluginProject(target.wstring()+L"1");
    const auto version11 = plugins::ReadPluginProject(target.wstring()+L"11");
    Check(Bytes(plugins::PluginSidecar(target.wstring()+L"1")) == middleSide &&
        Bytes(plugins::PluginSidecar(target.wstring()+L"11")) == oldestSide &&
        version1.saveId == middlePair.saveId && version11.saveId == oldestPair.saveId &&
        version1.projectId == oldestPair.projectId && version11.projectId == oldestPair.projectId &&
        version1.mainHash == plugins::FileHash(target.wstring()+L"1") &&
        version11.mainHash == plugins::FileHash(target.wstring()+L"11"),
        "each shifted PMM generation retains its own exact ABI sidecar and verified hash");
    const auto latestSide = Bytes(plugins::PluginSidecar(target));
#endif
    const auto latest = Bytes(target), lastEffect = Bytes(effect);
    const auto prior = Bytes(target.wstring()+L"1");
    HANDLE locked = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    std::puts("SAVE_TEST phase=versions-failed-replacement");
    Check(locked != INVALID_HANDLE_VALUE, "lock active PMM against replacement");
    expectError = true; app->CameraFov() += 2; app->SceneModified() = 1;
    SaveCommand(app, 207); expectError = false; CloseHandle(locked);
    Check(Bytes(target) == latest && Bytes(effect) == lastEffect && Bytes(target.wstring()+L"1") == prior &&
        !fs::exists(target.wstring()+L"111"), "failed replacement rolls back PMM and EMM generations");
#if !MMD_ORIGINAL_PROFILE
    Check(Bytes(plugins::PluginSidecar(target)) == latestSide &&
        Bytes(plugins::PluginSidecar(target.wstring()+L"1")) == middleSide &&
        Bytes(plugins::PluginSidecar(target.wstring()+L"11")) == oldestSide &&
        !fs::exists(plugins::PluginSidecar(target.wstring()+L"111")) &&
        !fs::exists(pmm_paths::Effect(target.wstring()+L"111")) &&
        plugins::ReadPluginProject(target).mainHash == plugins::FileHash(target),
        "failed replacement rolls back all ABI sidecar generations with PMM and EMM");
#endif
    std::puts("SAVE_TEST phase=versions-reload");
    for (const auto& saved : {std::pair<fs::path,float>{target.wstring()+L"1", middleFov},
                             std::pair<fs::path,float>{target.wstring()+L"11", oldestFov}}) {
        app->SceneModified() = 0;
        Check(LoadSceneFile(saved.first.c_str()), "real loader commits a retained previous PMM generation");
        Check(std::wstring(app->EnvFileName()) == saved.first.wstring() && !app->SceneModified() &&
            app->CameraFov() == saved.second, "retained generation reload restores its matching saved camera state");
#if !MMD_ORIGINAL_PROFILE
        const auto reloaded = plugins::ReadPluginProject(saved.first);
        Check(reloaded.projectId == oldestPair.projectId && reloaded.mainHash == plugins::FileHash(saved.first) &&
            reloaded.saveId == (saved.second == middleFov ? middlePair.saveId : oldestPair.saveId),
            "retained generation remains associated with its original ABI save identity after host reload");
#endif
    }
    // No filename suffix can bypass the actual PMM loader.
    for (const auto* suffix : {L".pmm1", L".PmM1111", L".pmm-recovery"}) {
        const auto project = root / (std::wstring(L"read")+suffix);
        fs::copy_file(target, project);
#if !MMD_ORIGINAL_PROFILE
        fs::copy_file(plugins::PluginSidecar(target), plugins::PluginSidecar(project));
#endif
        Check(pmm_paths::IsProject(project), "case-insensitive PMM extension prefix routes projects");
        app->SceneModified() = 0;
        Check(LoadSceneFile(project.c_str()), "real loader commits PMM with an extended suffix");
        Check(std::wstring(app->EnvFileName()) == project.wstring() && !app->SceneModified() && app->CameraFov() == latestFov,
            "real loader accepts a valid PMM with an extended suffix");
    }
    const auto reopened = root / L"read.pmm1";
    const auto text = reopened.wstring();
    HGLOBAL drop = GlobalAlloc(GHND, sizeof(DROPFILES) + (text.size()+2) * sizeof(wchar_t));
    Check(drop != nullptr, "allocate native drag-and-drop payload");
    auto* data = static_cast<DROPFILES*>(GlobalLock(drop));
    data->pFiles = sizeof(DROPFILES); data->fWide = TRUE;
    std::memcpy(reinterpret_cast<char*>(data)+sizeof(DROPFILES), text.c_str(), (text.size()+1)*sizeof(wchar_t));
    GlobalUnlock(drop); app->SceneModified() = 0;
    HandleDropFiles(reinterpret_cast<HDROP>(drop)); // Handler owns DragFinish.
    Check(std::wstring(app->EnvFileName()) == reopened.wstring(), "native drop handler opens PMM1 and updates the scene");
    const auto reopenedBytes = Bytes(reopened);
#if !MMD_ORIGINAL_PROFILE
    const auto reopenedSide = Bytes(plugins::PluginSidecar(reopened));
    const auto reopenedPair = plugins::ReadPluginProject(reopened);
#endif
    SaveCommand(app, 207);
    Check(Bytes(reopened.wstring()+L"1") == reopenedBytes && Recent().front() == reopened.wstring(),
        "saving an opened PMM1 creates PMM11 and records the correct recent file");
#if !MMD_ORIGINAL_PROFILE
    const auto savedVersion = plugins::ReadPluginProject(reopened.wstring()+L"1");
    Check(Bytes(plugins::PluginSidecar(reopened.wstring()+L"1")) == reopenedSide &&
        savedVersion.saveId == reopenedPair.saveId && savedVersion.mainHash == plugins::FileHash(reopened.wstring()+L"1"),
        "saving reopened PMM1 preserves its ABI pair under PMM11 rather than a sidecar suffix alias");
#endif
    const auto invalid = root / L"invalid.pmm111";
    std::ofstream(invalid) << "not a project";
    expectError = true; const bool invalidLoaded = LoadSceneFile(invalid.c_str()); expectError = false;
    Check(!invalidLoaded && std::wstring(app->EnvFileName()) == reopened.wstring(), "invalid header cannot retarget the active project");
    Check(!pmm_paths::IsProject(root / L"folder.pmm" / L"picture.png"), "folder substring cannot route a non-PMM file");
    Check(autosave::Configure(app, previousSettings, error), "restore autosave configuration");
    std::puts("SAVE_TEST phase=versions-complete");
}
