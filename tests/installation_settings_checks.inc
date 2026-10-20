// Run before app initialization and before the ordinary suite's test override.
void SettingsLocationChecks(const fs::path& executable,const std::wstring& mode) {
    const auto folder=executable.parent_path();const auto config=folder/L"data"/L"autosave.ini";
    const bool overrideMode=mode==L"--settings-override";
    if(!overrideMode)SetEnvironmentVariableW(L"MIKUMIKUSTUDIO_AUTOSAVE_CONFIG",nullptr);
    const auto current=as::Current();std::wstring error;
    Check(processor::ConfigPath()==(folder/L"data"/L"processor.ini").wstring(),"processor preferences are local to this installation");
    Check(viewport_corner_settings::SettingsPath()==(folder/L"data"/L"viewport.ini").wstring(),"viewport preferences are local to this installation");
    if(mode==L"--settings-fresh"){
        SetEnvironmentVariableW(L"MMS_ANTIALIAS_CONFIG",nullptr);
        auto probe=std::make_unique<MMDApp>();::new(static_cast<void*>(&probe->state)) MMDAppState{};
        antialias::ReloadUserSettings(probe.get());
        Check(probe->antialiasing.algorithm==antialias::Algorithm::MmdOriginal && probe->antialiasing.enabled,
            "antialias initialization ignores another installation shared system profile");
        fs::create_directories(folder/L"data");const auto antialiasFile=folder/L"data"/L"antialias.ini";
        Check(WritePrivateProfileStringW(L"Antialias",L"Algorithm",L"4",antialiasFile.c_str()) &&
            WritePrivateProfileStringW(L"Antialias",L"Enabled",L"0",antialiasFile.c_str()),"seed this installation antialias preferences");
        antialias::ReloadUserSettings(probe.get());
        Check(probe->antialiasing.algorithm==antialias::Algorithm::Fxaa && !probe->antialiasing.enabled,"antialias reads this installation data file");
        Check(processor::SaveRequested(processor::Configuration{},error),"save processor choices in this installation data");
        Check(viewport_corner_settings::Set(viewport_tool_layout::Corner::TopLeft),"save viewport corner in this installation data");
        SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE",nullptr);
        recent_projects::Record(probe.get(),(folder/L"example.pmm").c_str());
        Check(fs::is_regular_file(folder/L"data"/L"recent-pmm.txt"),"recent project addresses are saved only in this installation data");
        Check(fs::is_regular_file(folder/L"data"/L"processor.ini") && fs::is_regular_file(folder/L"data"/L"viewport.ini"),"device and viewport files remain beside their own EXE");
    }

    if(mode==L"--settings-fresh" || mode==L"--settings-default" || overrideMode){
        Check(!current.enabled && current.intervalMinutes==1 && current.maxBackups==100 && current.directory==(folder/L"backups").wstring(),
            "installation uses its own EXE/backups and ignores the shared legacy profile");
    }
    if(mode==L"--settings-fresh" || overrideMode){
        Check(as::Configure(nullptr,current,error),"save settings without system profile dependency");
        const auto target=overrideMode?root/L"override.ini":config;
        Check(fs::is_regular_file(target),"settings persist in this installation data or explicit test override");
        wchar_t value[64]{};GetPrivateProfileStringW(L"autosave",L"directoryMode",L"",value,64,target.c_str());
        Check(!wcscmp(value,L"executable"),"default directory persists as an executable-relative mode");
        GetPrivateProfileStringW(L"autosave",L"directory",L"",value,64,target.c_str());
        Check(!wcscmp(value,L"backups"),"default backup setting stores no previous EXE absolute address");
        if(overrideMode)Check(!fs::exists(config),"test override does not write production data settings");

        auto trailingSeparator=current;
        trailingSeparator.directory+=L"\\";
        const auto trailingTarget=root/L"trailing-separator.ini";
        Check(as::SaveSettings(trailingTarget,trailingSeparator),"save default backup directory with trailing separator");
        GetPrivateProfileStringW(L"autosave",L"directoryMode",L"",value,64,trailingTarget.c_str());
        Check(!wcscmp(value,L"executable"),"trailing separator keeps the default backup directory executable-relative");
    }
    if(mode==L"--settings-custom"){
        auto custom=current;custom.directory=(root/L"custom").wstring();custom.enabled=true;custom.intervalMinutes=2;custom.maxBackups=7;
        Check(as::Configure(nullptr,custom,error),"save custom per-installation backup directory");
    }
    if(mode==L"--settings-custom" || mode==L"--settings-check-custom"){
        const auto saved=as::LoadSettings(config);
        Check(saved.enabled && saved.directory==(root/L"custom").wstring() && saved.intervalMinutes==2 && saved.maxBackups==7,
            "same installation reloads its explicit custom directory and preferences after EXE rename");
    }
    if(mode==L"--settings-readonly"){
        const auto handle=CreateFileW(config.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        Check(handle!=INVALID_HANDLE_VALUE,"lock this installation settings against replacement");
        auto next=current;next.intervalMinutes=current.intervalMinutes+1;
        const bool saved=as::Configure(nullptr,next,error);CloseHandle(handle);
        Check(!saved && !error.empty() && as::Current().intervalMinutes==current.intervalMinutes &&
            as::LoadSettings(config).intervalMinutes==current.intervalMinutes,
            "failed local settings save preserves old settings instead of falling back to a shared profile");
    }
    std::printf("PASS autosave settings location checks=%u\n",checks);
}
