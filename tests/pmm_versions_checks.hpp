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

void NumberedVersionTests(MMDApp* app) {
    const auto original=autosave::Current();auto settings=original;
    settings.enabled=false;settings.keepPrevious=true;settings.numberedManualVersions=true;
    settings.incrementPattern=L"[/NAME]_[/N]";
    settings.backupPattern=L"backup_[/NAME]_[/YYYY]_[/N]";
    settings.maxBackups=1;settings.overwriteOldest=true;std::wstring error;
    Check(autosave::Configure(app,settings,error),"configure incremental saving independently of periodic backups");
    const auto persisted=autosave::LoadSettings(_wgetenv(L"MIKUMIKUSTUDIO_AUTOSAVE_CONFIG"));
    Check(persisted.numberedManualVersions && persisted.incrementPattern==settings.incrementPattern &&
        persisted.backupPattern==settings.backupPattern,"patterns and independent toggle survive reload");
    const auto target=root/L"手动历史.pmm";
    std::ofstream(target)<<"original untouched";
    wcscpy_s(app->EnvFileName(),kPathFieldCapacity,target.c_str());
    std::vector<std::pair<fs::path,std::vector<char>>> saved;
    for(unsigned version=1;version<=5;++version) {
        app->CameraFov()+=1;app->SceneModified()=1;SaveCommand(app,207);
        const auto expected=root/(L"手动历史_"+save_naming::Number(version,3)+L".pmm");
        Check(fs::path(app->EnvFileName())==expected && fs::exists(expected),"each save creates a new named PMM without nesting names");
        for(const auto& old:saved)Check(Bytes(old.first)==old.second,"incremental saves never rename or rewrite older PMMs");
        saved.push_back({expected,Bytes(expected)});
        std::ofstream(pmm_paths::Effect(expected))<<"EMM "<<version;
        Check(!app->SceneModified() && Recent().front()==expected.wstring(),"new increment becomes the saved active project");
    }
    Check(Bytes(target)==std::vector<char>({'o','r','i','g','i','n','a','l',' ','u','n','t','o','u','c','h','e','d'}),"original remains untouched");
    Check(!fs::exists(target.wstring()+L"1") && !fs::exists(target.wstring()+L"2"),"incremental saves do not shift PMM1 or PMM2 history");
    LoadSceneFile(saved.back().first.c_str());
    SaveCommand(app,207);
    Check(fs::path(app->EnvFileName()).filename()==L"手动历史_006.pmm","reopening an increment retains its original naming stem");
    const auto orphan=root/L"手动历史_007.emm";std::ofstream(orphan)<<"orphan effect";
    SaveCommand(app,207);
    Check(fs::path(app->EnvFileName()).filename()==L"手动历史_008.pmm" && !fs::exists(root/L"手动历史_007.pmm"),"orphan EMM reserves the corresponding PMM name");
    for(const auto& old:saved)Check(Bytes(old.first)==old.second,"reopen and collision handling preserve history");
    for(unsigned i=0;i<saved.size();++i) {
        const auto expected=std::string("EMM ")+std::to_string(i+1);
        Check(Bytes(pmm_paths::Effect(saved[i].first))==std::vector<char>(expected.begin(),expected.end()),"all existing EMM companions keep their name and content");
    }
    const auto renamed=root/L"另一个工程.pmm";
    wcscpy_s(app->EnvFileName(),kPathFieldCapacity,renamed.c_str());SaveCommand(app,207);
    Check(fs::path(app->EnvFileName()).filename()==L"另一个工程_001.pmm","a new Save As name starts a fresh increment series");
    settings.incrementPattern=L"固定名称";
    Check(autosave::Configure(app,settings,error),"a pattern without sequence is allowed");
    SaveCommand(app,207);SaveCommand(app,207);
    Check(fs::exists(root/L"固定名称.pmm") && fs::exists(root/L"固定名称_001.pmm"),"same-name saves receive a collision suffix instead of overwriting");
    const auto copy=root/L"independent-copy.pmm";
    Check(SaveSceneCopy(app,copy.c_str()) && fs::exists(copy),"automatic/copy snapshots do not use the manual naming pattern");
    Check(autosave::BackupNow(app)==autosave::Result::Disabled,"incremental saving does not enable periodic backups");
    Check(autosave::Configure(app,original,error),"restore save settings after incremental regression");
}
