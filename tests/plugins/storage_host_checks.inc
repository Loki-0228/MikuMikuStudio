namespace storage_host_checks {
unsigned FindCommand(HMENU menu){
    for(int i=0;i<GetMenuItemCount(menu);++i){
        if(auto child=GetSubMenu(menu,i)){if(auto id=FindCommand(child))return id;}
        wchar_t label[256]{};GetMenuStringW(menu,i,label,256,MF_BYPOSITION);
        if(std::wcscmp(label,L"ABI GUI probe")==0)return GetMenuItemID(menu,i);
    }return 0;
}
void Pump(){MSG msg{};unsigned count=0;while(count++<4096 && PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
int Value(const mikumikustudio::plugins::PluginProject& p){
    auto it=p.records.find("org.example.gui-storage");Check(it!=p.records.end() && it->second.bytes.size()==sizeof(int),"real DLL serializer saved bytes");int n=0;std::memcpy(&n,it->second.bytes.data(),sizeof(n));return n;
}
void Run(MMDApp* app){
    using namespace mikumikustudio::plugins;
    wchar_t model[32768]{};Check(GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL",model,32768)>0,"storage scene fixture");
    LoadModelFile(app,model,false);Check(app->ModelSlot(0)!=nullptr,"load storage model");
    Pump();const auto command=FindCommand(GetMenu(app->MainWindow()));Check(command!=0,"actual plugin menu registered");
    SendMessageW(app->MainWindow(),WM_COMMAND,command,0);
    const auto copy=std::filesystem::absolute(L"copy.pmm"),manual=std::filesystem::absolute(L"manual.pmm"),restored=std::filesystem::absolute(L"restored.pmm");
    const auto dirty=static_cast<std::uint8_t>(app->SceneModified()),layout=app->WindowLayoutReady();
    Check(SaveSceneCopy(app,copy.c_str()),"save real PMM and serializer sidecar");
    auto p=ReadPluginProject(copy);Check(Value(p)==1,"menu changed serialized plugin value");
    Check(app->SceneModified()==dirty && app->WindowLayoutReady()==layout,"copy preserves native dirty/layout state");
    p.records["missing.example"]={333,{0,255,8,0,99}};
    {std::ofstream file(PluginSidecar(copy),std::ios::binary);file<<EncodeProject(p);}
    ResetAppState(app);Pump();
    wcscpy_s(app->state.envFileName,copy.c_str());LoadSceneFile();Pump();
    Check(app->ModelSlot(0)!=nullptr,"native PMM reopened before plugin publication");
    Check(SaveSceneCopy(app,restored.c_str()) && Value(ReadPluginProject(restored))==1,"serializer state restored by real loader");
    Check(ReadPluginProject(restored).records.at("missing.example").bytes==p.records.at("missing.example").bytes,"missing plugin bytes retained through native load and save");
    SendMessageW(app->MainWindow(),WM_COMMAND,command,0);
    wcscpy_s(app->state.envFileName,manual.c_str());app->SceneModified()=1;SaveSceneFile(app);Pump();
    auto saved=ReadPluginProject(manual);Check(Value(saved)==2 && !app->SceneModified(),"manual save commits latest serializer state and clears dirty");
    Check(saved.projectId==p.projectId && saved.saveId!=p.saveId,"save as keeps project identity and creates a save identity");
    const auto before=FileHash(manual);SetEnvironmentVariableW(L"MMS_TEST_SERIALIZER_FAIL",L"1");
    Check(!SaveSceneCopy(app,std::filesystem::absolute(L"refused.pmm").c_str()),"serializer failure refuses native PMM publication");
    SetEnvironmentVariableW(L"MMS_TEST_SERIALIZER_FAIL",nullptr);
    Check(FileHash(manual)==before && !std::filesystem::exists(L"refused.pmm") && !std::filesystem::exists(L"refused.pmm.mms-plugins.json"),"serializer failure preserves existing pair and creates no partial output");
    Pump();std::puts("Production storage checks passed");
}
}
