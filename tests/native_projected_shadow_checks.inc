namespace mikudancestudio {void CmdFileMenu(MMDApp*,HWND,std::uint16_t,std::uint16_t);}
namespace {
std::uint8_t& ShadowDisplayFlag(MMDApp& app,unsigned field){return field?app.ProjectedShadowBlendEnabled():app.GroundShadowEnabled();}
std::array<std::uint8_t,3> ShadowDisplayValues(MMDApp& app){return {app.GroundGridEnabled(),app.GroundShadowEnabled(),app.ProjectedShadowBlendEnabled()};}
struct ProjectedShadowFixture : GridDisplayFixture {
    ProjectedShadowFixture(){Check(AppendMenuW(menu,MF_STRING,221,L"ground shadow")&&AppendMenuW(menu,MF_STRING,254,L"transparent ground shadow"),"create projected shadow menu entries");
        app->GroundGridEnabled()=1;app->GroundShadowEnabled()=1;app->ProjectedShadowBlendEnabled()=1;}
    void Toggle(unsigned field){if(field)CmdViewMenu(app.get(),app->MainWindow(),254,0);else CmdFileMenu(app.get(),app->MainWindow(),221,0);}
    bool ShadowEcho(){const auto flags=ShadowDisplayValues(*app);const UINT ids[]={215,221,254};for(unsigned i=0;i<3;++i)if(bool(GetMenuState(menu,ids[i],MF_BYCOMMAND)&MF_CHECKED)!=(flags[i]!=0))return false;return Echo();}
};
void ProjectedShadowRoutingChecks(){
    for(unsigned field=0;field<2;++field)for(std::uint8_t initial:{std::uint8_t(0),std::uint8_t(1),std::uint8_t(7)}){
        ProjectedShadowFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);ShadowDisplayFlag(*f.app,field)=initial;
        const auto before=ShadowDisplayValues(*f.app);auto target=before;target[field+1]=initial?0:1;
        const auto pose=NumericCameraPose(*f.app);const auto ring=PoseRingDigest(f.first);const auto keys=GlobalRegistrationKeys(f.app->CameraKeys());
        const auto accessory=f.accessory;f.app->state.dialogFlags[8]=0;f.Toggle(field);
        Check(ShadowDisplayValues(*f.app)==target&&f.history.UndoCount()==1&&f.app->SceneModified()&&f.ShadowEcho(),"projected shadow visibility and transparency menus publish one independent delta and synchronize display menus");
        Check(f.app->state.dialogFlags[8]==(field?0:1),"ground-shadow menu preserves original transient keyboard signal separately from history");
        Check(f.SelectionUnchanged()&&NumericCameraPose(*f.app)==pose&&PoseRingDigest(f.first)==ring&&GlobalRegistrationSame(f.app->CameraKeys(),keys)&&
            !std::memcmp(&f.accessory,&accessory,sizeof(accessory)),"projected shadow flags preserve tracks keys model ring camera and accessory pose");
        f.app->GroundGridEnabled()=0;ShadowDisplayFlag(*f.app,1-field)=0;f.app->CameraMode()=1;f.app->CurrentFrame()=43;f.app->state.dialogFlags[8]=0;
        Check(f.history.History(false)==MMS_OK&&ShadowDisplayFlag(*f.app,field)==initial&&!f.app->GroundGridEnabled()&&!ShadowDisplayFlag(*f.app,1-field)&&
            f.app->CurrentFrame()==43&&!f.app->state.dialogFlags[8],"shadow flag undo restores original byte and preserves other later display flags and input signals");
        plugins::RefreshBoundGlobalReadout(*f.app);Check(f.ShadowEcho(),"shadow history safe readout synchronizes all display menus");
        Check(f.history.History(true)==MMS_OK&&ShadowDisplayFlag(*f.app,field)==target[field+1],"shadow flag redo is independent of mode and frame");
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;bool changed=true;
        Check(plugins::ApplyBoundProjectedShadowFlag(*f.app,field,target[field+1]!=0,changed)==MMS_OK&&!changed&&f.history.UndoCount()==1&&!f.app->SceneModified(),"same shadow flag does not create history or dirty scene");
        f.Toggle(1-field);Check(f.history.UndoCount()==2&&f.history.History(false)==MMS_OK,"two shadow flags share chronological history");
        Check(plugins::ApplyBoundProjectedShadowFlag(*f.app,field,target[field+1]!=0,changed)==MMS_OK&&!changed&&f.history.RedoCount()==1,"shadow no-op retains later redo");
    }
}
void ProjectedShadowFailureChecks(){
    for(unsigned field=0;field<2;++field)for(bool initial:{false,true}){
        ProjectedShadowFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);ShadowDisplayFlag(*f.app,field)=initial;
        f.Toggle(field);Check(f.history.History(false)==MMS_OK,"seed projected shadow redo");const auto before=ShadowDisplayValues(*f.app);const auto pose=NumericCameraPose(*f.app);const auto ring=PoseRingDigest(f.first);
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;unsigned failures=0;bool succeeded=false;
        for(long limit=0;limit<128;++limit){CheckMenuItem(f.menu,field?254:221,initial?MF_UNCHECKED:MF_CHECKED);f.app->state.dialogFlags[8]=0;
            failAfter=limit;f.Toggle(field);failAfter=-1;if(f.history.UndoCount()){succeeded=true;break;}++failures;
            Check(ShadowDisplayValues(*f.app)==before&&f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&f.SelectionUnchanged()&&
                NumericCameraPose(*f.app)==pose&&PoseRingDigest(f.first)==ring&&f.ShadowEcho()&&f.app->state.dialogFlags[8]==(field?0:1),
                "shadow menu allocation failure preserves authoring redo dirty selection and pose while restoring menu and retaining native input signal");
            Check(f.history.Enter()==MMS_OK,"shadow flag failure releases journal gate");f.history.Leave();}
        Check(succeeded&&failures>=5,"projected shadow allocation sweep reaches successful commit");
    }
}
void ProjectedShadowGuardAndIdentityChecks(){
    for(unsigned field=0;field<2;++field){ProjectedShadowFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=ShadowDisplayValues(*f.app);
        f.app->PlaybackActive()=1;f.Toggle(field);f.app->PlaybackActive()=0;Check(ShadowDisplayValues(*f.app)==before&&!f.history.UndoCount()&&f.ShadowEcho(),"playback blocks both projected shadow menu writers");
        Check(f.history.Enter()==MMS_OK,"hold journal for shadow flags");f.Toggle(field);f.history.Leave();Check(ShadowDisplayValues(*f.app)==before&&!f.history.UndoCount(),"busy journal blocks shadow flags without legacy fallback");
        f.app->GroundShadowColorDialog()=reinterpret_cast<HWND>(INT_PTR(1));f.Toggle(field);f.app->GroundShadowColorDialog()=nullptr;
        Check(ShadowDisplayValues(*f.app)==before&&!f.history.UndoCount(),"open shadow ambient editor excludes concurrent shadow flag edit");
        f.Toggle(field);const auto target=ShadowDisplayValues(*f.app);auto* grown=mdl::KeyPoolCreate<mdl::CameraKey>(12);
        std::memcpy(grown,f.app->CameraKeys(),mdl::KeyPoolCapacity(f.app->CameraKeys())*sizeof(mdl::CameraKey));mdl::KeyPoolCommitGrowth(f.app->CameraKeys(),grown);
        Check(f.history.History(false)==MMS_OK&&ShadowDisplayValues(*f.app)==before&&f.history.History(true)==MMS_OK,"projected shadow records survive normal scene key pool growth");
        mdl::KeyPoolDestroy(f.app->CameraKeys());f.app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(12);
        Check(f.history.History(false)==MMS_E_STALE&&ShadowDisplayValues(*f.app)==target&&f.history.UndoCount()==1,"replaced scene root rejects projected shadow undo without data or cursor change");
    }
    ProjectedShadowFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);bool changed=true;
    Check(plugins::ApplyBoundProjectedShadowFlag(*f.app,2,true,changed)==MMS_E_ARGUMENT&&!changed&&!f.history.UndoCount(),"invalid projected shadow flag rejected");
}
void ProjectedShadowMixedDisplayChecks(){
    ProjectedShadowFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=ShadowDisplayValues(*f.app);
    f.Toggle(0);f.Toggle(1);f.GridDisplayFixture::Toggle(1);const auto target=ShadowDisplayValues(*f.app);
    Check(f.history.UndoCount()==3,"grid and two projected shadow flags use one common display history type");
    for(int i=0;i<3;++i)Check(f.history.History(false)==MMS_OK,"undo mixed project display delta");
    Check(ShadowDisplayValues(*f.app)==before,"mixed display undo restores all original flags");
    for(int i=0;i<3;++i)Check(f.history.History(true)==MMS_OK,"redo mixed project display delta");
    Check(ShadowDisplayValues(*f.app)==target,"mixed display redo restores exact final flags");
}
}
void NativeProjectedShadowChecks(){ProjectedShadowRoutingChecks();ProjectedShadowFailureChecks();ProjectedShadowGuardAndIdentityChecks();ProjectedShadowMixedDisplayChecks();}
