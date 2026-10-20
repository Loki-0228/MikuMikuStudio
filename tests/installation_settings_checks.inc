// Exercise the same default-directory classifier through both production JSON
// and the explicit legacy-file seam. Never create backups in a volume root.
void SettingsDirectoryNormalizationChecks(const fs::path& folder,const as::Settings& current,bool overrideMode){
    std::wstring error;
    if(!overrideMode)Check(system_settings::WriteSection("history","{\"recordAdjustments\":true,\"steps\":47,\"memoryMiB\":96}")==system_settings::Result::Ok,
        "seed history mode and limits before autosave preference updates");
    unsigned ordinal=0;
    for(const wchar_t* suffix:{L"\\",L"/",L"\\\\",L"/./",L"\\.\\"}){
        auto trailing=current;trailing.directory+=suffix;trailing.keepPrevious=true;
        const auto legacyTarget=root/(L"trailing-"+std::to_wstring(++ordinal)+L".ini");
        Check(as::SaveSettings(legacyTarget,trailing),"save default backup path with trailing separator through legacy helper");
        wchar_t mode[32]{},directory[64]{};
        GetPrivateProfileStringW(L"autosave",L"directoryMode",L"",mode,32,legacyTarget.c_str());
        GetPrivateProfileStringW(L"autosave",L"directory",L"",directory,64,legacyTarget.c_str());
        const auto legacy=as::LoadSettings(legacyTarget);
        Check(!wcscmp(mode,L"executable")&&!wcscmp(directory,L"data/backups")&&legacy.directory==(folder/L"data"/L"backups").wstring()&&legacy.keepPrevious,
            "trailing separators preserve portable default mode and never persist an installation absolute address");
        Check(as::Configure(nullptr,trailing,error),"configure trailing default directory through production persistence path");
        Check(as::Current().keepPrevious&&as::Current().enabled==current.enabled,
            "previous-version preservation is independent of the periodic autosave switch");
        if(!overrideMode){
            std::string body;Check(system_settings::ReadSection("autosave",body)==system_settings::Result::Ok,"read normalized central autosave preferences");
            const auto saved=settings_json::Object(body);
            Check(settings_json::String(saved,"directoryMode")=="executable"&&settings_json::String(saved,"directory")=="data/backups"&&settings_json::Bool(saved,"keepPrevious",false),
                "central JSON uses the same portable default path normalization as explicit INI");
            Check(system_settings::ReadSection("history",body)==system_settings::Result::Ok,"history section survives autosave JSON updates");
            const auto history=settings_json::Object(body);
            Check(settings_json::Bool(history,"recordAdjustments",false)&&settings_json::UInt(history,"steps",0)==47&&settings_json::UInt(history,"memoryMiB",0)==96,
                "autosave updates preserve history mode step limit and memory budget");
        }else {
            Check(as::LoadSettings(root/L"override.ini").keepPrevious,
                "explicit override writes the previous-version switch to its INI");
            Check(!fs::exists(folder/L"data"/L"settings.json"),"legacy test override never creates central production settings");
        }
    }
    Check(as::Configure(nullptr,current,error),"restore original cached autosave directory after separator checks");
    auto custom=current;custom.directory=fs::path(current.directory).root_path().wstring();
    const auto rootTarget=root/L"custom-volume-root.ini";
    Check(as::SaveSettings(rootTarget,custom),"serialize an explicitly selected volume root only into a disposable INI");
    wchar_t mode[32]{};GetPrivateProfileStringW(L"autosave",L"directoryMode",L"",mode,32,rootTarget.c_str());
    Check(!wcscmp(mode,L"custom")&&as::LoadSettings(rootTarget).directory==custom.directory,
        "directory normalization preserves a custom drive root instead of walking above it");
}

// The runner places this EXE in a disposable installation. Exercise the real
// public preference writer/reader with no HWND, renderer or shader compiler.
void OutlineModeSettingsChecks(MMDApp& probe,const fs::path& folder,bool migrationMode){
    using Mode=antialias::OutlineMode;
    Check(probe.antialiasing.outlineMode==Mode::FxaaAdaptive,
        "automatic outline softening replaces a missing JSON value or retired local INI choice");
    std::string autosaveBefore,body;
    Check(system_settings::ReadSection("autosave",autosaveBefore)==system_settings::Result::Ok,
        "capture another central section before updating the outline preference");
    Check(system_settings::WriteSection("antialias","{\"futureOutlineKey\":{\"value\":19}}")==system_settings::Result::Ok,
        "schema accepts an additional antialias object field without replacing unknown metadata");
    probe.CameraMode()=1;probe.CameraParentModel()=-1;probe.SceneModified()=0;
    probe.CurrentFrame()=47;probe.LastRegisteredFrame()=91;
    std::wstring error;
    Check(!probe.antialiasing.enabled&&antialias::SelectOutlineMode(&probe,Mode::FxaaAdaptive,error)&&
        antialias::SelectOutlineMode(&probe,Mode::Multisample,error),
        "outline preference remains writable while scene antialiasing is disabled and no GPU is present");
    Check(system_settings::ReadSection("antialias",body)==system_settings::Result::Ok,
        "the public outline selector writes the production centralized setting");
    auto saved=settings_json::Object(body);
    Check(settings_json::UInt(saved,"outlineMode",99)==1&&settings_json::UInt(saved,"algorithm",99)==4&&
        !settings_json::Bool(saved,"enabled",true)&&saved.At("futureOutlineKey").At("value").UInt()==19,
        "outline writes preserve the disabled algorithm and unknown fields under schema version one");
    std::string autosaveAfter;
    Check(system_settings::ReadSection("autosave",autosaveAfter)==system_settings::Result::Ok&&autosaveAfter==autosaveBefore&&
        !probe.SceneModified()&&probe.CurrentFrame()==47&&probe.LastRegisteredFrame()==91,
        "outline preferences change neither unrelated settings nor project dirty state or transport");
    auto restarted=std::make_unique<MMDApp>();::new(static_cast<void*>(&restarted->state)) MMDAppState{};
    const auto beforeRestart=body;
    antialias::ReloadUserSettings(restarted.get());
    Check(restarted->antialiasing.outlineMode==Mode::FxaaAdaptive&&restarted->antialiasing.algorithm==antialias::Algorithm::Fxaa&&
        !restarted->antialiasing.enabled&&system_settings::ReadSection("antialias",body)==system_settings::Result::Ok&&body==beforeRestart,
        "restart uses automatic outline softening while preserving algorithm, disabled state and stored legacy metadata");
    Check(system_settings::WriteSection("antialias","{\"outlineMode\":99}")==system_settings::Result::Ok,
        "future numeric outline values are valid schema data");
    antialias::ReloadUserSettings(restarted.get());
    Check(restarted->antialiasing.outlineMode==Mode::FxaaAdaptive&&system_settings::ReadSection("antialias",body)==system_settings::Result::Ok&&
        settings_json::UInt(settings_json::Object(body),"outlineMode",0)==99,
        "an unknown outline enum uses the safe display default without rewriting the stored future value");
    // Retain a retired stored choice for later process/re-name checks, which
    // must normalize it at read time without rewriting unrelated metadata.
    Check(antialias::SelectOutlineMode(&probe,Mode::FxaaAdaptive,error)&&antialias::SelectOutlineMode(&probe,Mode::Multisample,error),
        "restore a persisted non-default outline choice for process restart checks");
    Check(system_settings::ReadSection("antialias",body)==system_settings::Result::Ok,"capture persisted valid outline before an invalid public request");
    const auto persisted=body;
    Check(!antialias::SelectOutlineMode(&probe,Mode(99),error)&&!error.empty()&&probe.antialiasing.outlineMode==Mode::Multisample&&
        system_settings::ReadSection("antialias",body)==system_settings::Result::Ok&&body==persisted,
        "an invalid public outline selection cannot change the current preference");
    if(!migrationMode)Check(!fs::exists(folder/L"data"/L"antialias.ini"),"outline preference updates never create a separate production INI");
    antialias::Release(restarted.get());antialias::Release(&probe);
}

// Run before app initialization and before the ordinary suite's test override.
void SettingsLocationChecks(const fs::path& executable,const std::wstring& mode) {
    const auto folder=executable.parent_path();const auto config=folder/L"data"/L"settings.json";
    const bool overrideMode=mode==L"--settings-override";
    const bool migrationMode=mode==L"--settings-migrate";
    if(!overrideMode)SetEnvironmentVariableW(L"MIKUMIKUSTUDIO_AUTOSAVE_CONFIG",nullptr);
    const auto legacyAutosave=folder/L"data"/L"autosave.ini";
    if(mode==L"--settings-fresh"){
        Check(system_settings::WriteSection("autosave","{\"enabled\":false}")==system_settings::Result::Ok,
            "seed a pre-existing central autosave section from before the previous-version option");
        { std::ofstream out(legacyAutosave); out << "[autosave]\nkeepPrevious=1\ndirectoryMode=executable\n"; }
        Check(as::LoadSettings(legacyAutosave).keepPrevious,"local INI fixture requests previous-version preservation");
    }
    if (migrationMode) {
        Check(!fs::exists(config), "migration fixture starts without central settings");
        fs::create_directories(config.parent_path());
        { std::ofstream out(legacyAutosave); out << "[autosave]\nenabled=0\nintervalMinutes=3\nmaxBackups=9\nkeepPrevious=1\ndirectoryMode=executable\n"; }
        { std::ofstream out(folder/L"data"/L"antialias.ini"); out << "[Antialias]\nAlgorithm=4\nEnabled=0\nOutlineMode=1\n"; }
        { std::ofstream out(folder/L"data"/L"processor.ini"); out << "[Vertices]\nMode=cpu\n"; }
        { std::ofstream out(folder/L"data"/L"viewport.ini"); out << "[viewport]\ncorner=tl\n"; }
        { std::ofstream out(folder/L"data"/L"adaptive_shadow.ini"); out << "1 9999\n"; }
    }
    const auto current=as::Current();std::wstring error;
    if(mode==L"--settings-fresh"){
        Check(!current.keepPrevious,"an existing central section defaults a missing key off instead of importing the old INI again");
        Check(fs::remove(legacyAutosave),"remove the disposable migration-precedence fixture");
    }
    Check(processor::ConfigPath()==config.wstring(),"processor preferences use this installation central settings");
    Check(viewport_corner_settings::SettingsPath()==config.wstring(),"viewport preferences use this installation central settings");
    if(mode==L"--settings-fresh" || migrationMode){
        SetEnvironmentVariableW(L"MMS_ANTIALIAS_CONFIG",nullptr);
        auto probe=std::make_unique<MMDApp>();::new(static_cast<void*>(&probe->state)) MMDAppState{};
        wcscpy_s(probe->state.exeDir, folder.c_str());
        antialias::ReloadUserSettings(probe.get());
        if (migrationMode) {
            Check(!current.enabled && current.keepPrevious && current.intervalMinutes==3 && current.maxBackups==9 &&
                current.directory==(folder/L"data"/L"backups").wstring(), "autosave imports local INI once");
            std::string migrated;
            Check(system_settings::ReadSection("autosave",migrated)==system_settings::Result::Ok&&
                settings_json::Bool(settings_json::Object(migrated),"keepPrevious",false),
                "INI migration stores previous-version preservation in the central autosave section even with periodic saving disabled");
            Check(probe->antialiasing.algorithm==antialias::Algorithm::Fxaa && !probe->antialiasing.enabled&&
                probe->antialiasing.outlineMode==antialias::OutlineMode::FxaaAdaptive,
                "antialias imports the local INI algorithm and enable state while retiring the independent outline choice");
            Check(processor::Initialize(error) && processor::Requested().vertices.mode==processor::Mode::Cpu,
                "processor imports local INI once");
            Check(viewport_corner_settings::Current()==viewport_tool_layout::Corner::TopLeft,
                "viewport corner imports local INI once");
            LoadShadowResolutionPreference(probe.get());
            Check(probe->AdaptiveShadowResolution() && !probe->ShadowFilterEnhanced(),
                "legacy numeric shadow precision does not enable enhanced filtering during first migration");
            { std::ofstream out(folder/L"data"/L"adaptive_shadow.ini"); out << "0\nfilter_enhanced=1\n"; }
            LoadShadowResolutionPreference(probe.get());
            Check(probe->AdaptiveShadowResolution() && !probe->ShadowFilterEnhanced(),
                "central shadow settings take precedence over later legacy changes");
            { std::ofstream out(folder/L"data"/L"antialias.ini"); out << "[Antialias]\nAlgorithm=0\nEnabled=1\nOutlineMode=0\n"; }
            antialias::ReloadUserSettings(probe.get());
            Check(probe->antialiasing.algorithm==antialias::Algorithm::Fxaa && !probe->antialiasing.enabled&&
                probe->antialiasing.outlineMode==antialias::OutlineMode::FxaaAdaptive,
                "central antialias settings take precedence over later legacy changes");
        } else {
            Check(probe->antialiasing.algorithm==antialias::Algorithm::MmdOriginal && probe->antialiasing.enabled,
                "antialias initialization ignores another installation shared system profile");
            Check(system_settings::WriteSection("antialias", "{\"algorithm\":4,\"enabled\":false}")==system_settings::Result::Ok,
                "seed this installation central antialias preferences");
            antialias::ReloadUserSettings(probe.get());
            Check(probe->antialiasing.algorithm==antialias::Algorithm::Fxaa && !probe->antialiasing.enabled,
                "antialias reads this installation central settings");
            Check(processor::SaveRequested(processor::Configuration{},error),"save processor choices in central settings");
            Check(viewport_corner_settings::Set(viewport_tool_layout::Corner::TopLeft),"save viewport corner in central settings");
            SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE",nullptr);
            recent_projects::Record(probe.get(),(folder/L"example.pmm").c_str());
            std::string recentBody;
            Check(system_settings::ReadSection("recentProjects",recentBody)==system_settings::Result::Ok,
                "recent project addresses use this installation central settings");
            const auto recent=settings_json::Object(recentBody);
            const auto& recentPaths=recent.At("paths").Items();
            Check(recentPaths.size()==1 && plugins::Wide(recentPaths.front().String())==(folder/L"example.pmm").wstring() &&
                !fs::exists(folder/L"data"/L"recent-pmm.txt"),
                "central recentProjects.paths stores the project without recreating separate history file");
            Check(fs::is_regular_file(config) && !fs::exists(folder/L"data"/L"processor.ini") &&
                !fs::exists(folder/L"data"/L"viewport.ini") && !fs::exists(folder/L"data"/L"antialias.ini"),
                "fresh settings do not recreate separate preference files");
        }
        OutlineModeSettingsChecks(*probe,folder,migrationMode);
    }
    if(mode==L"--settings-default"||mode==L"--settings-check-custom"){
        SetEnvironmentVariableW(L"MMS_ANTIALIAS_CONFIG",nullptr);
        auto restarted=std::make_unique<MMDApp>();::new(static_cast<void*>(&restarted->state)) MMDAppState{};
        antialias::ReloadUserSettings(restarted.get());
        std::string antialiasBody;
        Check(restarted->antialiasing.outlineMode==antialias::OutlineMode::FxaaAdaptive&&!restarted->antialiasing.enabled&&
            system_settings::ReadSection("antialias",antialiasBody)==system_settings::Result::Ok&&
            settings_json::UInt(settings_json::Object(antialiasBody),"outlineMode",99)==1,
            "a later process including an EXE rename uses automatic outlines without rewriting its installation's retired choice");
        antialias::Release(restarted.get());
    }
    if(mode==L"--settings-fresh" || mode==L"--settings-default" || overrideMode){
        Check(!current.enabled && !current.keepPrevious && current.intervalMinutes==1 && current.maxBackups==100 && current.directory==(folder/L"data"/L"backups").wstring(),
            "installation uses its own data/backups and ignores the shared legacy profile");
    }
    if(mode==L"--settings-fresh" || overrideMode){
        Check(as::Configure(nullptr,current,error),"save settings without system profile dependency");
        const auto target=overrideMode?root/L"override.ini":config;
        Check(fs::is_regular_file(target),"settings persist in this installation data or explicit test override");
        if (overrideMode) {
            wchar_t value[64]{};GetPrivateProfileStringW(L"autosave",L"directoryMode",L"",value,64,target.c_str());
            Check(!wcscmp(value,L"executable"),"override default directory persists as an executable-relative mode");
            GetPrivateProfileStringW(L"autosave",L"directory",L"",value,64,target.c_str());
            Check(!wcscmp(value,L"data/backups"),"override backup setting stores no previous EXE absolute address");
            Check(GetPrivateProfileIntW(L"autosave",L"keepPrevious",-1,target.c_str())==0,
                "explicit override serializes the disabled previous-version default");
            Check(!fs::exists(config),"test override does not write production data settings");
        } else {
            std::string body;
            Check(system_settings::ReadSection("autosave",body)==system_settings::Result::Ok,"central autosave section exists");
            const auto saved=settings_json::Object(body);
            Check(settings_json::String(saved,"directoryMode")=="executable","default directory persists as executable-relative mode");
            Check(settings_json::String(saved,"directory")=="data/backups","default backup stores no previous EXE absolute address");
            Check(!settings_json::Bool(saved,"keepPrevious",true),"central settings serialize the disabled previous-version default as a boolean");
        }
        SettingsDirectoryNormalizationChecks(folder,current,overrideMode);
    }
    if(mode==L"--settings-custom"){
        auto custom=current;custom.directory=(root/L"custom").wstring();custom.enabled=true;custom.keepPrevious=true;custom.intervalMinutes=2;custom.maxBackups=7;
        Check(as::Configure(nullptr,custom,error),"save custom per-installation backup directory");
    }
    if(mode==L"--settings-custom" || mode==L"--settings-check-custom"){
        std::string body;
        Check(system_settings::ReadSection("autosave",body)==system_settings::Result::Ok,"reload central autosave section");
        const auto saved=settings_json::Object(body);
        Check(as::Current().enabled&&as::Current().keepPrevious&&settings_json::Bool(saved,"enabled",false)&&settings_json::Bool(saved,"keepPrevious",false) && plugins::Wide(settings_json::String(saved,"directory"))==(root/L"custom").wstring() &&
            settings_json::UInt(saved,"intervalMinutes",0)==2 && settings_json::UInt(saved,"maxBackups",0)==7,
            "same installation reloads its custom directory and preferences after EXE rename");
    }
    if(mode==L"--settings-readonly"){
        const auto handle=CreateFileW(config.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        Check(handle!=INVALID_HANDLE_VALUE,"lock this installation settings against replacement");
        auto next=current;next.intervalMinutes=current.intervalMinutes+1;next.keepPrevious=!current.keepPrevious;
        const bool saved=as::Configure(nullptr,next,error);CloseHandle(handle);
        std::string body;
        Check(system_settings::ReadSection("autosave",body)==system_settings::Result::Ok,"read unchanged settings after failed save");
        Check(!saved && !error.empty() && as::Current().intervalMinutes==current.intervalMinutes&&as::Current().keepPrevious==current.keepPrevious &&
            settings_json::UInt(settings_json::Object(body),"intervalMinutes",0)==current.intervalMinutes&&
            settings_json::Bool(settings_json::Object(body),"keepPrevious",!current.keepPrevious)==current.keepPrevious,
            "failed local settings save preserves old settings instead of falling back to a shared profile");
    }
    std::printf("PASS autosave settings location checks=%u\n",checks);
}
