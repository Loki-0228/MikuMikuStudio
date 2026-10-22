// Included in the hidden production save/export fixture namespace.
void NumberedVersionTests(MMDApp* app) {
    std::puts("SAVE_TEST phase=numbered-versions");
    auto settings = autosave::Current(); std::wstring error;
    settings.enabled = false; settings.keepPrevious = false; settings.numberedManualVersions = false;
    Check(autosave::Configure(app, settings, error), "disable both manual-version switches independently of periodic backups");
    const auto plain = root / L"不保留版本.pmm";
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, plain.c_str()); app->SceneModified() = 1;
    SaveCommand(app, 207); const auto plainBefore = Bytes(plain);
    app->CameraFov() += 1; app->SceneModified() = 1; SaveCommand(app, 207);
    Check(Bytes(plain) != plainBefore && !fs::exists(plain.wstring()+L"1") && !fs::exists(plain.wstring()+L"2"),
        "actual manual save with both switches off updates current without generating versions");
    struct Generation {
        std::vector<char> project, effect, side; float fov = 0;
        std::string projectId, saveId;
    };
    const auto capture = [&](const fs::path& path) {
        Generation value; value.project = Bytes(path); value.effect = Bytes(pmm_paths::Effect(path)); value.fov = app->CameraFov();
#if !MMD_ORIGINAL_PROFILE
        value.side = Bytes(plugins::PluginSidecar(path)); const auto pair = plugins::ReadPluginProject(path);
        value.projectId = pair.projectId; value.saveId = pair.saveId;
        Check(!value.side.empty() && !value.saveId.empty() && pair.mainHash == plugins::FileHash(path),
            "numbered-save fixture starts from a real verified ABI project pair");
#endif
        return value;
    };
    const auto matches = [&](const fs::path& path, const Generation& value) {
        Check(Bytes(path) == value.project && Bytes(pmm_paths::Effect(path)) == value.effect,
            "numeric version retains the exact original PMM and corresponding existing EMM bytes");
#if !MMD_ORIGINAL_PROFILE
        const auto pair = plugins::ReadPluginProject(path);
        Check(Bytes(plugins::PluginSidecar(path)) == value.side && pair.projectId == value.projectId && pair.saveId == value.saveId &&
            pair.mainHash == plugins::FileHash(path), "numeric version preserves the correct-suffix sidecar original identity and verified PMM hash");
#endif
    };
    for (bool keep : {false, true}) {
        settings.keepPrevious = keep; settings.numberedManualVersions = true;
        Check(autosave::Configure(app, settings, error), "enable numbered manual versions with or without the legacy switch");
        const auto target = root / (keep ? L"编号优先.pmm" : L"仅编号.pmm");
        const auto at = [&](unsigned number) { return fs::path(target.wstring()+std::to_wstring(number)); };
        wcscpy_s(app->EnvFileName(), kPathFieldCapacity, target.c_str());
        app->CameraFov() = keep ? 52.f : 42.f; app->SceneModified() = 1; SaveCommand(app, 207);
        Check(!fs::exists(at(1)), "first numbered save does not fabricate a previous version");
        { std::ofstream stream(pmm_paths::Effect(target)); stream << "numbered EMM alpha"; }
        const auto oldest = capture(target);
        for (unsigned sparse : {7u, 11u}) {
            Check(fs::copy_file(target, at(sparse)) && fs::copy_file(pmm_paths::Effect(target), pmm_paths::Effect(at(sparse))),
                "seed valid sparse numeric PMM and EMM generations through owned fixture files");
#if !MMD_ORIGINAL_PROFILE
            Check(fs::copy_file(plugins::PluginSidecar(target), plugins::PluginSidecar(at(sparse))), "seed exact matching sparse ABI sidecar");
#endif
        }
        app->CameraFov() += 2; app->SceneModified() = 1; SaveCommand(app, 207);
        matches(at(1), oldest); matches(at(8), oldest); matches(at(12), oldest);
        Check(!fs::exists(at(7)) && !fs::exists(at(11)) && !fs::exists(at(111)),
            "all sparse decimal versions advance numerically rather than appending one even when legacy mode is also enabled");
        { std::ofstream stream(pmm_paths::Effect(target)); stream << "numbered EMM beta"; }
        const auto middle = capture(target);
        app->CameraFov() += 3; app->SceneModified() = 1; SaveCommand(app, 207);
        matches(at(1), middle); matches(at(2), oldest); matches(at(9), oldest); matches(at(13), oldest);
        Check(!fs::exists(at(8)) && !fs::exists(at(12)) && !fs::exists(at(11)),
            "successive Save commands shift one to two and preserve sparse numbering gaps");
        const auto latest = capture(target);
#if !MMD_ORIGINAL_PROFILE
        Check(oldest.projectId == middle.projectId && middle.projectId == latest.projectId &&
            oldest.saveId != middle.saveId && middle.saveId != latest.saveId,
            "numeric manual saves retain one project identity and mint distinct current save identities");
#endif
        HANDLE locked = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(locked != INVALID_HANDLE_VALUE, "lock current numbered project against replacement");
        expectError = true; app->CameraFov() += 1; app->SceneModified() = 1; SaveCommand(app, 207);
        expectError = false; CloseHandle(locked);
        matches(target, latest); matches(at(1), middle); matches(at(2), oldest); matches(at(9), oldest); matches(at(13), oldest);
        Check(!fs::exists(at(3)) && !fs::exists(at(10)) && !fs::exists(at(14)),
            "failed numbered replacement cannot leave partly shifted generations");
        for (const auto& saved : {std::pair<fs::path, const Generation*>{at(1), &middle}, {at(2), &oldest}, {at(13), &oldest}}) {
            app->SceneModified() = 0;
            Check(LoadSceneFile(saved.first.c_str()), "real loader reopens numeric retained project");
            Check(std::wstring(app->EnvFileName()) == saved.first.wstring() && !app->SceneModified() && app->CameraFov() == saved.second->fov,
                "numeric retained project reload restores its own original camera state");
            matches(saved.first, *saved.second);
        }
    }
}
void VersionTests(MMDApp* app) {
    std::puts("SAVE_TEST phase=versions-configure");
    const auto target = root / L"版本配对.pmm";
    auto value = autosave::Current(); const auto previousSettings = value;
    value.keepPrevious = true;
    value.numberedManualVersions = false;
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
    NumberedVersionTests(app);
    Check(autosave::Configure(app, previousSettings, error), "restore autosave configuration");
    std::puts("SAVE_TEST phase=versions-complete");
}
