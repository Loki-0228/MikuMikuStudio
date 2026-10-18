// Include after numpad_view_host_checks.inc. Uses the actual host's main
// dispatcher, production MicWndProc, plugin journal and native controls.
// The aggregate includes native_camera_follow headers at global scope.
namespace camera_follow_host_checks {
// Call once after real UI creation and journal binding, before other checks
// intentionally place user drafts in the native numeric edits.
void CheckInitializationReadout(MMDApp* app){
    Check(plugins::BoundNativePoseHistory(app)!=nullptr,"real initialization readout uses the native history binding");
    std::array<float,6> before{};const auto light=app->SceneLight();
    for(unsigned field=0;field<6;++field){const auto edit=GetDlgItem(app->MainWindow(),461+int(field));
        Check(edit&&!SendMessageW(edit,EM_GETMODIFY,0,0),"real UI numeric initialization does not leave light output marked as a user draft");
        before[field]=field<3?app->LightColor()[field]:app->LightDirection()[field-3];}
    for(unsigned axis=0;axis<3;++axis){app->LightColor()[axis]=.8125f-float(axis)*.2f;app->LightDirection()[axis]=-.7135f+float(axis)*.1f;}
    app->ApplyTimelineLightState();plugins::RefreshBoundGlobalReadout(*app);
    for(unsigned field=0;field<6;++field){char expected[64]{},actual[64]{};const auto edit=GetDlgItem(app->MainWindow(),461+int(field));
        if(field<3)std::snprintf(expected,sizeof(expected),"%3d",int(double(app->LightColor()[field])*256.0));
        else std::snprintf(expected,sizeof(expected),"%+.1f",double(app->LightDirection()[field-3]));
        GetWindowTextA(edit,actual,sizeof(actual));
        Check(!std::strcmp(actual,expected)&&!SendMessageW(edit,EM_GETMODIFY,0,0),
            "actual initialized light controls refresh every live value after binding instead of retaining startup text");}
    for(unsigned field=0;field<6;++field)(field<3?app->LightColor()[field]:app->LightDirection()[field-3])=before[field];
    app->SceneLight()=light;plugins::RefreshBoundGlobalReadout(*app);
}
struct State {
    numpad_view_host_checks::View view;
    std::array<float,11> scalars{};
    D3DLIGHT9 light{};
    std::uint32_t noise=0,modeWord=0;
    int shadow=0;
    std::uint8_t follow=0;
    std::vector<std::pair<unsigned,mdl::AccessoryRecord>> objects;
    explicit State(MMDApp& app):view(numpad_view_host_checks::Read(app)),light(app.SceneLight()),
        noise(std::uint32_t(app.GravityNoise())),modeWord(app.GravityNoiseModeWord()),shadow(app.ShadowMode()),follow(app.state.followCameraEnabled){
        for(unsigned i=0;i<3;++i){scalars[i]=app.LightDirection()[i];scalars[i+3]=app.LightColor()[i];scalars[i+7]=app.GravityDirection()[i];}
        scalars[6]=app.GravityMagnitude();scalars[10]=app.ShadowDistance();
        for(unsigned slot=0;slot<kModelSlotCount;++slot)if(const auto* object=app.AccessorySlot(slot))objects.emplace_back(slot,*object);
    }
    bool Same(MMDApp& app)const{
        const State now(app);
        if(!numpad_view_host_checks::Same(view,now.view)||std::memcmp(scalars.data(),now.scalars.data(),sizeof scalars)||
            std::memcmp(&light,&now.light,sizeof light)||noise!=now.noise||modeWord!=now.modeWord||shadow!=now.shadow||follow!=now.follow||objects.size()!=now.objects.size())return false;
        for(unsigned i=0;i<objects.size();++i)if(objects[i].first!=now.objects[i].first||
            std::memcmp(&objects[i].second,&now.objects[i].second,sizeof(mdl::AccessoryRecord)))return false;
        return true;
    }
    void Restore(MMDApp& app)const{
        numpad_view_host_checks::Write(app,view);app.SceneLight()=light;app.GravityNoise()=int(noise);app.GravityNoiseModeWord()=modeWord;
        app.ShadowMode()=shadow;app.state.followCameraEnabled=follow;
        for(unsigned i=0;i<3;++i){app.LightDirection()[i]=scalars[i];app.LightColor()[i]=scalars[i+3];app.GravityDirection()[i]=scalars[i+7];}
        app.GravityMagnitude()=scalars[6];app.ShadowDistance()=scalars[10];
        for(const auto& row:objects){Check(app.AccessorySlot(row.first)!=nullptr,"follow fixture retains the same loaded accessories");*app.AccessorySlot(row.first)=row.second;}
    }
};
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    using edit_history_host_checks::Menu;
    using Mode=plugins::SharedHistory::RecordingMode;
    Check(!history.UndoCount()&&!history.RedoCount(),"real follow fixture starts with an empty shared journal");
    const State original(*app);
    const auto initial=value();
    const auto main=app->MainWindow();
    const auto field=GetDlgItem(main,panel::kFovEdit);
    Check(field!=nullptr,"real follow fixture has the native FOV edit");
    wchar_t originalText[256]{};GetWindowTextW(field,originalText,256);
    const auto modified=SendMessageW(field,EM_GETMODIFY,0,0);
    {
        // Preserve original authoring and camera pool exactly, and use an
        // isolated ordinary camera source without changing its public format.
        numpad_view_host_checks::Original isolation(*app,history);
        const auto floating=CreateWindowExW(0,L"STATIC",L"follow command probe",WS_OVERLAPPED,0,0,320,120,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(floating!=nullptr,"create an owned hidden detached follow window");
        const auto checkbox=CreateWindowExW(0,L"BUTTON",L"follow",WS_CHILD|BS_CHECKBOX,0,0,80,24,floating,
            reinterpret_cast<HMENU>(INT_PTR(panel::kFollowCameraCheckbox)),GetModuleHandleW(nullptr),nullptr);
        Check(checkbox!=nullptr,"create the real detached follow checkbox");
        const auto previous=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(floating,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(MicWndProc)));
        Check(previous!=nullptr,"install the production detached window dispatcher");
        app->FloatingWindow()=floating;app->CameraMode()=1;app->CurrentFrame()=7;app->CameraParentModel()=-1;
        auto& camera=app->CameraKeys()[0];camera.parentModel=-1;camera.distance=-45;camera.fov=45;
        std::memset(camera.interpolation,20,12);std::memset(&camera.interpolation[2][0],107,12);
        const auto invoke=[&](unsigned entry){
            if(entry==2)SendMessageW(main,WM_COMMAND,247,0);
            else{const auto owner=entry?floating:main;const auto button=GetDlgItem(owner,panel::kFollowCameraCheckbox);
                Check(button!=nullptr,"actual follow command checkbox exists");CheckDlgButton(owner,panel::kFollowCameraCheckbox,BST_CHECKED);
                SendMessageW(owner,WM_COMMAND,MAKEWPARAM(panel::kFollowCameraCheckbox,BN_CLICKED),reinterpret_cast<LPARAM>(button));}
        };
        for(unsigned entry=0;entry<3;++entry)for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
            original.Restore(*app);app->CameraMode()=1;app->CurrentFrame()=7;app->CameraParentModel()=-1;app->state.followCameraEnabled=0;
            app->LightDirection()[0]+=123.5f;app->ViewOffsetX()=3.75f;
            Check(history.SetMode(mode)==MMS_OK,"configure actual follow adjustment recording mode");
            const State before(*app);Menu(app,L"Edit probe once");
            SetWindowTextW(field,L"尚未提交的输入");SendMessageW(field,EM_SETMODIFY,TRUE,0);SendMessageW(field,EM_SETSEL,1,5);SetFocus(field);
            const auto focus=GetFocus();invoke(entry);const State after(*app);
            wchar_t pending[64]{};GetWindowTextW(field,pending,64);
            Check(app->state.followCameraEnabled&&!before.Same(*app)&&value()==initial+1&&
                history.UndoCount()==(mode==Mode::AllOperations?2u:1u)&&app->CurrentFrame()==7&&GetFocus()==focus&&
                !std::wcscmp(pending,L"尚未提交的输入")&&SendMessageW(field,EM_GETMODIFY,0,0),
                "real main535 detached535 and menu247 publish one complete follow adjustment while preserving pending input and focus");
            Check(IsDlgButtonChecked(main,panel::kFollowCameraCheckbox)==BST_CHECKED&&IsDlgButtonChecked(floating,panel::kFollowCameraCheckbox)==BST_CHECKED&&
                (GetMenuState(GetMenu(main),247,MF_BYCOMMAND)&MF_CHECKED),"real successful follow synchronizes both checkboxes and menu");
            if(mode==Mode::AllOperations){
                app->CurrentFrame()=53;app->CameraMode()=0;app->SelectedModelSlot()=0;app->PhysicsResetPending()=0;
                Menu(app,L"Edit probe undo");Check(before.Same(*app)&&value()==initial+1&&app->CurrentFrame()==53&&!app->PhysicsResetPending(),
                    "plugin undo restores the complete native follow snapshot across frame and editor mode without settling physics");
                Menu(app,L"Edit probe redo");Check(after.Same(*app)&&app->CurrentFrame()==53&&!app->PhysicsResetPending(),
                    "plugin redo replays the captured follow result without resampling current keys");
                Menu(app,L"Edit probe undo");
            }
            Menu(app,L"Edit probe undo");Check(value()==initial,"preceding plugin edit remains independently undoable");
            Check(history.Reset()==MMS_OK,"reset isolated follow command history");history.Collect();
            SendMessageW(field,EM_SETMODIFY,FALSE,0);SetFocus(main);app->CameraMode()=1;
        }
        original.Restore(*app);app->FloatingWindow()=nullptr;
        SetWindowLongPtrW(floating,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(previous));DestroyWindow(floating);
        SetWindowTextW(field,originalText);SendMessageW(field,EM_SETMODIFY,modified,0);
    }
    plugins::RefreshBoundGlobalReadout(*app);
    std::puts("PASS actual camera-follow commands, complete snapshots, plugin history and pending native input");
}
}
