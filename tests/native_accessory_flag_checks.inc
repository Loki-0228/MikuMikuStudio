namespace {
std::array<std::uint8_t,3> AccessoryFlags(const mdl::AccessoryRecord& a){return {a.visible,a.additiveBlend,a.shadowEnabled};}
struct AccessoryFlagFixture : AccessoryTextFixture {
    static constexpr int ids[3]={476,477,486};
    AccessoryFlagFixture(){for(int id:ids)Check(CreateWindowExA(0,"BUTTON","",WS_CHILD|BS_CHECKBOX,0,0,80,20,app->MainWindow(),
        reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr)!=nullptr,"accessory checkbox exists");}
    void Toggle(unsigned field){CmdControl450(app.get(),app->MainWindow(),std::uint16_t(ids[field]),0);}
    bool Echo(){const auto flags=AccessoryFlags(*app->AccessorySlot(app->SelectedAccessorySlot()));
        for(unsigned i=0;i<3;++i)if(SendDlgItemMessageW(app->MainWindow(),ids[i],BM_GETCHECK,0,0)!=(flags[i]?BST_CHECKED:BST_UNCHECKED))return false;return true;}
};
void AccessoryFlagRoutingChecks() {
    for(unsigned field=0;field<3;++field)for(std::uint8_t initial:{std::uint8_t(0),std::uint8_t(1),std::uint8_t(7)}) {
        AccessoryFlagFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        if(field==0)f.accessory.visible=initial;else if(field==1)f.accessory.additiveBlend=initial;else f.accessory.shadowEnabled=initial;
        const auto before=AccessoryFlags(f.accessory);auto target=before;target[field]=initial?0:1;
        const auto numeric=AccessoryTextPose(f.accessory);const auto keys=AccessoryRegistrationKeys(*f.app);const auto marks=AccessoryRegistrationMarks(*f.app);
        const auto ring=PoseRingDigest(f.first);const auto other=f.other;
        f.accessory.rowSelected=0;f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light);
        f.Toggle(field);
        Check(AccessoryFlags(f.accessory)==target&&f.history.UndoCount()==1&&f.Echo(),"native accessory checkbox toggles author data and records exact original byte");
        Check(field==1?(!f.accessory.rowSelected&&f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)):
            (f.accessory.rowSelected&&!f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)),"additive flag preserves original selection difference from display and shadow");
        f.app->SelectedAccessorySlot()=8;f.app->CurrentFrame()=39;f.app->CameraMode()=1;
        Check(f.history.History(false)==MMS_OK&&AccessoryFlags(f.accessory)==before&&f.app->CurrentFrame()==39,"accessory flag undo restores original byte across selection mode and frame");
        Check(f.history.History(true)==MMS_OK&&AccessoryFlags(f.accessory)==target,"accessory flag redo targets original accessory");
        f.app->SelectedAccessorySlot()=7;f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;bool changed=true;
        Check(plugins::ApplyBoundAccessoryFlag(*f.app,7,field,target[field]!=0,changed)==MMS_OK&&!changed&&f.history.UndoCount()==1&&!f.app->SceneModified(),"same accessory flag value does not append history or dirty scene");
        f.Toggle(field);Check(f.history.UndoCount()==2,"second toggle records separate user operation");
        Check(f.history.History(false)==MMS_OK,"seed accessory flag redo");
        Check(plugins::ApplyBoundAccessoryFlag(*f.app,7,field,target[field]!=0,changed)==MMS_OK&&!changed&&f.history.RedoCount()==1,"flag no-op preserves redo");
        Check(AccessoryTextPose(f.accessory)==numeric&&AccessoryRegistrationKeysEqual(*f.app,keys)&&AccessoryRegistrationMarks(*f.app)==marks&&
            PoseRingDigest(f.first)==ring&&!std::memcmp(&f.other,&other,sizeof(other)),"accessory flag edits preserve numeric pose keys key selection old ring and other object");
    }
}
void AccessoryFlagFailureChecks() {
    for(unsigned field=0;field<3;++field) {
        AccessoryFlagFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        f.Toggle(field);Check(f.history.History(false)==MMS_OK,"seed accessory flag failure redo");
        const auto before=AccessoryFlags(f.accessory);const auto numeric=AccessoryTextPose(f.accessory);const auto keys=AccessoryRegistrationKeys(*f.app);
        const auto marks=AccessoryRegistrationMarks(*f.app);const auto ring=PoseRingDigest(f.first);
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;f.accessory.rowSelected=0;f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light);
        unsigned failures=0;bool succeeded=false;
        for(long limit=0;limit<128;++limit) {
            SendDlgItemMessageW(f.app->MainWindow(),AccessoryFlagFixture::ids[field],BM_SETCHECK,before[field]?BST_UNCHECKED:BST_CHECKED,0);
            failAfter=limit;f.Toggle(field);failAfter=-1;
            if(f.history.UndoCount()){succeeded=true;break;}++failures;
            Check(AccessoryFlags(f.accessory)==before&&AccessoryTextPose(f.accessory)==numeric&&AccessoryRegistrationKeysEqual(*f.app,keys)&&AccessoryRegistrationMarks(*f.app)==marks&&
                PoseRingDigest(f.first)==ring&&f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&!f.accessory.rowSelected&&f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)&&f.Echo(),
                "flag allocation failure restores checkbox and preserves pose keys selection dirty ring and redo");
            Check(f.history.Enter()==MMS_OK,"flag failure releases journal gate");f.history.Leave();
        }
        Check(succeeded&&failures>=5,"accessory flag allocation sweep reaches successful commit");
    }
}
void AccessoryFlagGuardAndIdentityChecks() {
    for(unsigned field=0;field<3;++field) {
        AccessoryFlagFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=AccessoryFlags(f.accessory);
        f.app->PlaybackActive()=1;f.Toggle(field);f.app->PlaybackActive()=0;
        Check(AccessoryFlags(f.accessory)==before&&!f.history.UndoCount()&&f.Echo(),"playback blocks accessory toggle without legacy fallback");
        Check(f.history.Enter()==MMS_OK,"hold journal for accessory toggle");f.Toggle(field);f.history.Leave();
        Check(AccessoryFlags(f.accessory)==before&&!f.history.UndoCount(),"busy journal blocks accessory toggle");
        f.Toggle(field);const auto target=AccessoryFlags(f.accessory);const auto count=mdl::KeyPoolCapacity(f.app->AccessoryKeys(7));
        auto* grown=mdl::KeyPoolCreate<mdl::AccessoryKey>(count+4);std::memcpy(grown,f.app->AccessoryKeys(7),count*sizeof(mdl::AccessoryKey));mdl::KeyPoolCommitGrowth(f.app->AccessoryKeys(7),grown);
        Check(f.history.History(false)==MMS_OK&&AccessoryFlags(f.accessory)==before&&f.history.History(true)==MMS_OK,"accessory flags survive pool growth");
        f.app->AccessorySlot(7)=&f.other;
        Check(f.history.History(false)==MMS_E_STALE&&AccessoryFlags(f.accessory)==target&&f.history.UndoCount()==1,"slot replacement cannot redirect flag undo");
        f.app->AccessorySlot(7)=&f.accessory;auto* original=f.app->AccessoryKeys(7);auto* replacement=mdl::KeyPoolCreate<mdl::AccessoryKey>(count+4);f.app->AccessoryKeys(7)=replacement;
        Check(f.history.History(false)==MMS_E_STALE&&AccessoryFlags(f.accessory)==target,"pool replacement rejects flag undo");
        f.app->AccessoryKeys(7)=original;mdl::KeyPoolDestroy(replacement);
        plugins::ForgetSceneObject(&f.accessory);plugins::SceneObjectIdentity(&f.accessory);
        Check(f.history.History(false)==MMS_E_STALE&&AccessoryFlags(f.accessory)==target,"same address with new ID cannot receive old flag undo");
    }
    AccessoryFlagFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);bool changed=true;
    Check(plugins::ApplyBoundAccessoryFlag(*f.app,255,0,true,changed)==MMS_E_ARGUMENT&&!changed,"invalid accessory flag slot rejected");
    Check(plugins::ApplyBoundAccessoryFlag(*f.app,7,3,true,changed)==MMS_E_ARGUMENT&&!changed,"invalid accessory flag component rejected");
    f.app->SelectedAccessorySlot()=9;f.Toggle(0);Check(!f.history.UndoCount(),"empty accessory slot consumes flag toggle without fallback");
}
void AccessoryFlagFieldIsolationChecks() {
    AccessoryFlagFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto original=AccessoryFlags(f.accessory);
    f.CommitField(0,"12.345678");f.Toggle(0);f.Toggle(1);f.Toggle(2);
    Check(f.history.UndoCount()==4,"accessory numeric and flags share chronological history");
    f.accessory.position[1]=17;f.accessory.parentModel=1;f.accessory.parentBone=1;
    for(int i=0;i<3;++i)Check(f.history.History(false)==MMS_OK,"undo separate accessory flag delta");
    Check(AccessoryFlags(f.accessory)==original&&f.accessory.position[0]==12.3456f&&f.accessory.position[1]==17&&f.accessory.parentModel==1&&f.accessory.parentBone==1,"flag undo preserves unrelated live numeric and binding fields");
    Check(f.history.History(false)==MMS_OK&&f.accessory.position[0]==9&&f.accessory.position[1]==17,"numeric undo still changes only its own field after shared record extension");
    for(int i=0;i<4;++i)Check(f.history.History(true)==MMS_OK,"redo mixed accessory numeric and flag records");
}
}
void NativeAccessoryFlagChecks(){AccessoryFlagRoutingChecks();AccessoryFlagFailureChecks();AccessoryFlagGuardAndIdentityChecks();AccessoryFlagFieldIsolationChecks();}
