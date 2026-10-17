namespace vpd_import_host_checks {
std::wstring selected, initial, navigation;
MMDApp* active=nullptr;unsigned calls=0;int action=0;
BOOL Pick(LPOPENFILENAMEW ofn){++calls;
    Check(ofn->lpstrInitialDir&&initial==ofn->lpstrInitialDir&&std::filesystem::path(initial).is_absolute(),"VPD picker uses absolute remembered/default directory");
    Check((ofn->Flags&(OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR))==(OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR),"VPD picker preserves process directory");
    Check(SetCurrentDirectoryW(navigation.c_str())!=FALSE,"simulate VPD picker cwd change");
    if(action==1)Menu(active,L"Edit probe once");if(action==2)Menu(active,L"Edit probe prepared");
    if(action==3){const auto before=calls;CmdLoadPose(active);Check(calls==before,"recursive VPD menu does not open second picker");}
    if(selected.empty())return FALSE;wcscpy_s(ofn->lpstrFile,ofn->nMaxFile,selected.c_str());return TRUE;
}
void Drop(const std::wstring& path){const auto size=sizeof(DROPFILES)+(path.size()+2)*sizeof(wchar_t);const auto memory=GlobalAlloc(GHND,size);Check(memory!=nullptr,"allocate native shell VPD drop");
    auto* data=static_cast<DROPFILES*>(GlobalLock(memory));data->pFiles=sizeof(DROPFILES);data->fWide=TRUE;
    std::memcpy(reinterpret_cast<unsigned char*>(data)+sizeof(DROPFILES),path.c_str(),(path.size()+1)*sizeof(wchar_t));GlobalUnlock(memory);HandleDropFiles(reinterpret_cast<HDROP>(memory));}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    auto& m=*mdl::Mdl(app->ModelSlot(0));const auto slot=app->SelectedModelSlot(),mode=app->CameraMode();const auto frame=app->CurrentFrame();app->SelectedModelSlot()=0;app->CameraMode()=0;
    const auto original=m.boneTable[0];const auto dirty=m.bonePhysicsState[0];const auto scale=bone_scale::Pose(app->ModelSlot(0),0);std::vector<unsigned char> selection(m.boneSelection,m.boneSelection+m.boneCount);
    const auto ring=m.undoState[0];const auto directory=std::wstring(app->DirPose());const auto workspace=app->PathWorkspace();const auto cwd=std::filesystem::current_path();const auto menu=GetMenu(app->MainWindow());const auto oldMenu=GetMenuState(menu,0x12D,MF_BYCOMMAND);
    const auto path=std::filesystem::absolute(L"import-pose.vpd");
    for(unsigned i=0;i<m.boneCount;++i)m.boneSelection[i]=i==0;
    m.boneTable[0].hasRigidBody=0;m.boneTable[0].trans[0]=original.trans[0]+8;
    SaveVpdFile(path.c_str());m.boneTable[0]=original;std::copy(selection.begin(),selection.end(),m.boneSelection);
    Check(std::filesystem::exists(path),"native VPD writer creates production fixture");
    Menu(app,L"Edit probe prepared");app->SceneModified()=0;LoadVpdFile(path.c_str());
    Check(!history.UndoCount()&&!app->SceneModified()&&m.boneTable[0].trans[0]==original.trans[0],"pending real plugin transaction blocks direct VPD import");Menu(app,L"Edit probe cancel");
    Menu(app,L"Edit probe once");LoadVpdFile(path.c_str());
    Check(value()==1&&history.UndoCount()==2&&m.boneTable[0].trans[0]==original.trans[0]+8&&m.undoState[0]==ring,"direct production VPD import shares plugin journal without old-ring writes");
    Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");app->SceneModified()=app->RecordedFrameCountByte1()=0;LoadVpdFile(path.c_str());
    Check(history.RedoCount()==1&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),"identical production VPD retains plugin redo");Menu(app,L"Edit probe redo");
    Check(SaveSceneCopy(app,std::filesystem::absolute(L"vpd-import.pmm").c_str()),"PMM accepts committed VPD and plugin state");
    const auto offline=std::filesystem::absolute(L"import-pose.offline");std::filesystem::rename(path,offline);
    Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");app->CameraMode()=1;app->CurrentFrame()=frame+53;
    Menu(app,L"Edit probe redo");SendMessageW(app->MainWindow(),WM_COMMAND,401,0);Menu(app,L"Edit probe redo");
    Check(value()==2&&m.boneTable[0].trans[0]==original.trans[0]+8&&m.undoState[0]==ring&&app->CurrentFrame()==frame+53,"production VPD redo across frame and camera mode never rereads source");
    Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Check(history.Reset()==MMS_OK,"clear VPD mixed history");app->CameraMode()=0;app->CurrentFrame()=frame;std::filesystem::rename(offline,path);
    active=app;navigation=std::filesystem::temp_directory_path().native();Hook(&GetUiOpenFileNameW,&Pick);
    for(bool remember:{false,true}){CheckMenuItem(menu,0x12D,remember?MF_CHECKED:MF_UNCHECKED);wcscpy_s(app->DirPose(),kPathFieldCapacity,L"relative-poses");
        initial=(std::filesystem::path(app->ExeDir())/(remember?L"relative-poses":L"UserFile/Pose")).lexically_normal().native();
        for(int test=0;test<5;++test){selected=test==0?L"":(test==1?std::filesystem::absolute(L"missing-pose.vpd").native():path.native());action=test==2?1:(test==3?2:3);
            const auto count=calls;const auto saved=m.boneTable[0];app->SceneModified()=app->RecordedFrameCountByte1()=0;SendMessageW(app->MainWindow(),WM_COMMAND,202,0);
            Check(calls==count+1&&std::filesystem::current_path()==cwd&&!std::memcmp(&workspace,&app->PathWorkspace(),sizeof workspace),"actual 202 picker restores cwd and leaves path workspace untouched");
            if(test<4){Check(m.boneTable[0].trans[0]==saved.trans[0]&&!wcscmp(app->DirPose(),L"relative-poses"),"cancel failure stale or pending VPD keeps pose and remembered directory");
                if(test==2){Check(value()==1&&history.UndoCount()==1,"modal plugin edit survives stale VPD rejection");Menu(app,L"Edit probe undo");history.Reset();}
                if(test==3)Menu(app,L"Edit probe cancel");
            }else{Check(history.UndoCount()==1&&m.boneTable[0].trans[0]==original.trans[0]+8&&app->PhysicsResetPending(),"successful recursive-safe menu import creates one pose step and requests physics reset");
                Check(std::wstring(app->DirPose())==(remember?path.parent_path().native():L"relative-poses"),"remember pose directory only after successful change");
                Menu(app,L"Edit probe undo");history.Reset();}
        }
    }
    MH_DisableHook(reinterpret_cast<void*>(&GetUiOpenFileNameW));MH_RemoveHook(reinterpret_cast<void*>(&GetUiOpenFileNameW));action=0;active=nullptr;
    CheckMenuItem(menu,0x12D,MF_CHECKED);wcscpy_s(app->DirPose(),kPathFieldCapacity,L"before-drop");Drop(path.native());
    Check(history.UndoCount()==1&&m.boneTable[0].trans[0]==original.trans[0]+8&&std::wstring(app->DirPose())==path.parent_path().native(),"actual shell VPD drop joins history and remembers successful directory");Menu(app,L"Edit probe undo");history.Reset();
    Check(m.boneTable[0].trans[0]==original.trans[0]&&bone_scale::Pose(app->ModelSlot(0),0)==scale,"VPD host restores original author pose and scale");
    m.boneTable[0]=original;m.bonePhysicsState[0]=dirty;std::copy(selection.begin(),selection.end(),m.boneSelection);wcscpy_s(app->DirPose(),kPathFieldCapacity,directory.c_str());CheckMenuItem(menu,0x12D,oldMenu&MF_CHECKED);
    app->SelectedModelSlot()=slot;app->CameraMode()=mode;app->CurrentFrame()=frame;std::filesystem::remove(path);std::puts("PASS actual VPD direct/menu/drop import, modal safety, plugin history, offline redo and PMM save");
}
}
