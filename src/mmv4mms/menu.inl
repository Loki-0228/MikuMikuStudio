void Initialize(MMDApp* app) {
    host=app;
    if(attempted)return;
    attempted=true;
    wchar_t executable[32768]{};
    const DWORD count=GetModuleFileNameW(nullptr,executable,DWORD(std::size(executable)));
    if(!count||count>=std::size(executable)){lastError="Cannot resolve MMS executable directory";RefreshLanguage(app);return;}
    directory=std::filesystem::path(executable).parent_path();
    const auto dll=directory/L"MMV4MMS.dll";
    if(GetFileAttributesW(dll.c_str())==INVALID_FILE_ATTRIBUTES){RefreshLanguage(app);return;}
    if(mikumikustudio::LegacyMmeModule()){lastError="Remove the MME proxy before enabling MMV4MMS";RefreshLanguage(app);return;}
    library=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!library){lastError="Cannot load MMV4MMS.dll (Windows error "+std::to_string(GetLastError())+")";RefreshLanguage(app);return;}
    const auto get=reinterpret_cast<MmvMmsGetApiFn>(GetProcAddress(library,"MMV4MMSGetPluginApi"));
    api.structureSize=sizeof(api);
    if(!get||get(MMV_MMS_ABI_VERSION,&api)!=MMV_OK||api.structureSize<sizeof(api)||api.abiVersion!=MMV_MMS_ABI_VERSION||
       api.runtime.abiVersion!=MMV_ABI_VERSION||!api.runtime.create||!api.runtime.destroy||!api.runtime.notify||
       !api.runtime.showEditor||!api.runtime.load||!api.runtime.save||!api.runtime.status||!api.render) {
        lastError="Incompatible MMV4MMS plugin API";ReleasePlugin();RefreshLanguage(app);return;
    }
    const auto getMenu=reinterpret_cast<MmvGetMenuApiFn>(GetProcAddress(library,"MMVGetMenuApi"));
    menuApi={sizeof(menuApi),MMV_MENU_VERSION};
    if(!getMenu||getMenu(MMV_MENU_VERSION,&menuApi)!=MMV_OK||menuApi.version!=MMV_MENU_VERSION||!menuApi.flags||!menuApi.command)menuApi={};
    MmvHostServices services{};services.structureSize=sizeof(services);services.abiVersion=MMV_ABI_VERSION;
    services.ownerWindow=app?app->MainWindow():nullptr;services.pluginDirectory=directory.c_str();
    services.log=Log;services.requestRedraw=Redraw;services.physicsInput=PhysicsInput;
    if(api.runtime.create(&services,&plugin)!=MMV_OK||!plugin){lastError="MMV4MMS initialization failed";ReleasePlugin();}
    else {lastError.clear();if(!openedProject.empty())Notify(MMV_PROJECT_OPENED,openedProject.c_str());}
    RefreshLanguage(app);
}
bool Active(){return plugin&&!(menuApi.flags&&(menuApi.flags(plugin)&MMV_MENU_RENDERER_DISABLED))&&!mikumikustudio::LegacyMmeModule();}
void Shutdown(){ReleasePlugin();physicsSnapshots.clear();generations.clear();openedProject.clear();attempted=false;host=nullptr;}
void ClearScene(){rendererColor.Reset();Notify(MMV_SCENE_RESET);physicsSnapshots.clear();generations.clear();openedProject.clear();}
void Forget(const void* object){Notify(MMV_OBJECT_REMOVED,nullptr,reinterpret_cast<uint64_t>(object));generations.erase(object);physicsSnapshots.erase(reinterpret_cast<uint64_t>(object));}
void ProjectOpened(MMDApp* app,const wchar_t* path){Initialize(app);rendererColor.Reset();physicsSnapshots.clear();generations.clear();Notify(MMV_SCENE_RESET);openedProject=path?path:L"";Notify(MMV_PROJECT_OPENED,path);}
void ProjectSaved(MMDApp* app,const wchar_t* path){Initialize(app);openedProject=path?path:L"";Notify(MMV_PROJECT_SAVED,path);}
void DeviceLost(){rendererColor.Reset();Notify(MMV_DEVICE_LOST);}

void RefreshLanguage(MMDApp* app) {if(app)Notify(MMV_LANGUAGE_CHANGED,nullptr,app->EnglishUI());}
