// Included in the hidden production save/export fixture namespace.
void VersionTests(MMDApp* app) {
    const auto target = root / L"版本配对.pmm";
    auto value = autosave::Current(); const auto previousSettings = value;
    value.keepPrevious = true;
    std::wstring error;
    Check(autosave::Configure(app, value, error), "enable PMM1 independent of periodic autosave");
    Check(autosave::LoadSettings(_wgetenv(L"MIKUMIKUSTUDIO_AUTOSAVE_CONFIG")).keepPrevious,
        "PMM1 option survives settings reload");
    const auto effect = pmm_paths::Effect(target);
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, target.c_str());
    SaveCommand(app, 207); const auto oldest = Bytes(target);
    Check(!fs::exists(target.wstring()+L"1"), "first save has no invented previous version");
    std::ofstream(effect) << "original EMM";
    app->CameraFov() += 3; app->SceneModified() = 1;
    SaveCommand(app, 207); const auto middle = Bytes(target);
    Check(Bytes(target.wstring()+L"1") == oldest && middle != oldest, "PMM1 preserves the previous saved state");
    Check(Bytes(pmm_paths::Effect(target.wstring()+L"1")) == Bytes(effect), "EMM1 is paired with PMM1");
    std::ofstream(effect) << "second EMM";
    app->CameraFov() += 2; app->SceneModified() = 1;
    SaveCommand(app, 207);
    Check(Bytes(target.wstring()+L"1") == middle && Bytes(target.wstring()+L"11") == oldest,
        "repeated saves retain and shift all older versions");
    Check(Bytes(pmm_paths::Effect(target.wstring()+L"11")) == std::vector<char>({'o','r','i','g','i','n','a','l',' ','E','M','M'}),
        "shifting versions preserves the matching old EMM");
    const auto latest = Bytes(target), lastEffect = Bytes(effect);
    const auto prior = Bytes(target.wstring()+L"1");
    HANDLE locked = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock active PMM against replacement");
    expectError = true; app->CameraFov() += 2; app->SceneModified() = 1;
    SaveCommand(app, 207); expectError = false; CloseHandle(locked);
    Check(Bytes(target) == latest && Bytes(effect) == lastEffect && Bytes(target.wstring()+L"1") == prior &&
        !fs::exists(target.wstring()+L"111"), "failed replacement rolls back PMM and EMM generations");
    // No filename suffix can bypass the actual PMM loader.
    for (const auto* suffix : {L".pmm1", L".PmM1111", L".pmm-recovery"}) {
        const auto project = root / (std::wstring(L"read")+suffix);
        fs::copy_file(target, project);
        Check(pmm_paths::IsProject(project), "case-insensitive PMM extension prefix routes projects");
        app->SceneModified() = 0;
        LoadSceneFile(project.c_str());
        Check(std::wstring(app->EnvFileName()) == project.wstring() && !app->SceneModified(),
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
    SaveCommand(app, 207);
    Check(Bytes(reopened.wstring()+L"1") == reopenedBytes && Recent().front() == reopened.wstring(),
        "saving an opened PMM1 creates PMM11 and records the correct recent file");
    const auto invalid = root / L"invalid.pmm111";
    std::ofstream(invalid) << "not a project";
    expectError = true; LoadSceneFile(invalid.c_str()); expectError = false;
    Check(std::wstring(app->EnvFileName()) == reopened.wstring(), "invalid header cannot retarget the active project");
    Check(!pmm_paths::IsProject(root / L"folder.pmm" / L"picture.png"), "folder substring cannot route a non-PMM file");
    Check(autosave::Configure(app, previousSettings, error), "restore autosave configuration");
}
