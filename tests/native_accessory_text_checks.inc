namespace {
struct AccessoryTextFixture : AccessoryRegistrationFixture {
    AccessoryTextFixture() {
        Check(CreateWindowExA(0, "EDIT", "0", WS_CHILD, 0, 0, 80, 20, app->MainWindow(),
            reinterpret_cast<HMENU>(INT_PTR(485)), GetModuleHandleW(nullptr), nullptr) != nullptr, "accessory opacity control exists");
    }
    HWND Field(unsigned field) { return GetDlgItem(app->MainWindow(), 478 + int(field)); }
    void TypeField(unsigned field, const char* text) { SetWindowTextA(Field(field), text); SendMessageA(Field(field), EM_SETMODIFY, TRUE, 0); }
    void CommitField(unsigned field, const char* text) { TypeField(field, text); CommitEditControl(app.get(), Field(field)); }
    void LoseField(unsigned field) { EditSubclassProc(Field(field), WM_KILLFOCUS, reinterpret_cast<WPARAM>(app->MainWindow()), 0); }
};
std::array<float,8> AccessoryTextPose(const mdl::AccessoryRecord& a) {
    return {a.position[0],a.position[1],a.position[2],a.rotation[0],a.rotation[1],a.rotation[2],a.scale,a.opacity};
}
void AccessoryTextRoutingChecks() {
    for (unsigned field=0;field<8;++field) {
        AccessoryTextFixture f; plugins::NativePoseTestBinding binding(*f.app,f.history);
        const auto before=AccessoryTextPose(f.accessory); auto target=before;
        target[field]=field<3||field==6?12.3456f:field<6?float(12.3456/180.0*3.141592025756836):1.f;
        const auto keys=AccessoryRegistrationKeys(*f.app);const auto marks=AccessoryRegistrationMarks(*f.app);
        const auto other=f.other;const auto ring=PoseRingDigest(f.first);
        f.TypeField(field,field==7?"1.5":"12.345678");f.LoseField(field);
        Check(AccessoryTextPose(f.accessory)==target&&f.history.UndoCount()==1&&f.accessory.rowSelected,
            "accessory text uses legacy truncation rotation conversion and opacity clamp in one history step");
        f.LoseField(field);CommitEditControl(f.app.get(),f.Field(field));
        Check(AccessoryTextPose(f.accessory)==target&&f.history.UndoCount()==1,
            "accessory rounded echo does not overwrite precise authoring on repeated focus or commit");
        if(field==7) {char echo[32]{};GetWindowTextA(f.Field(field),echo,sizeof(echo));Check(!std::strcmp(echo,"1.00"),"accessory opacity echo retains two decimals");}
        f.app->CurrentFrame()=37;f.app->SelectedAccessorySlot()=8;f.app->CameraMode()=1;
        Check(f.history.History(false)==MMS_OK&&AccessoryTextPose(f.accessory)==before&&f.app->CurrentFrame()==37,
            "accessory undo resolves original object after selection mode and frame change");
        f.app->SelectedAccessorySlot()=7;
        for(const char* invalid:{"NaN","inf","","3 junk","1e999"}) {
            f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;f.CommitField(field,invalid);
            Check(AccessoryTextPose(f.accessory)==before&&f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),
                "invalid accessory text preserves authoring dirty flags and redo");
        }
        const std::string longInput(270,'1');f.CommitField(field,longInput.c_str());
        Check(AccessoryTextPose(f.accessory)==before&&f.history.RedoCount()==1,"oversized accessory input preserves redo");
        Check(f.history.History(true)==MMS_OK&&AccessoryTextPose(f.accessory)==target,"accessory redo survives invalid inputs");
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;f.CommitField(field,field==7?"1.5":"12.345678");
        Check(f.history.UndoCount()==1&&!f.app->SceneModified(),"same accessory input does not append history or dirty scene");
        Check(AccessoryRegistrationKeysEqual(*f.app,keys)&&AccessoryRegistrationMarks(*f.app)==marks&&
            !std::memcmp(&f.other,&other,sizeof(other))&&PoseRingDigest(f.first)==ring,
            "accessory scalar history preserves key pools key selection other objects and model undo ring");
    }
}
void AccessoryTextFailureChecks() {
    for(unsigned field=0;field<8;++field) {
        AccessoryTextFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        f.CommitField(field,field==7?".7":"5");Check(f.history.History(false)==MMS_OK,"seed accessory redo");
        const auto before=AccessoryTextPose(f.accessory);const auto keys=AccessoryRegistrationKeys(*f.app);
        const auto marks=AccessoryRegistrationMarks(*f.app);const auto ring=PoseRingDigest(f.first);
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
        unsigned failures=0;bool succeeded=false;
        for(long limit=0;limit<128;++limit) {
            f.TypeField(field,field==7?".25":"12.345678");failAfter=limit;CommitEditControl(f.app.get(),f.Field(field));failAfter=-1;
            if(f.history.UndoCount()){succeeded=true;break;}++failures;
            Check(AccessoryTextPose(f.accessory)==before&&AccessoryRegistrationKeysEqual(*f.app,keys)&&AccessoryRegistrationMarks(*f.app)==marks&&
                f.history.RedoCount()==1&&PoseRingDigest(f.first)==ring&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),
                "accessory allocation failure preserves pose keys selection old ring dirty flags and redo");
            Check(f.history.Enter()==MMS_OK,"accessory allocation failure releases journal gate");f.history.Leave();
        }
        Check(succeeded&&failures>=5,"accessory failure sweep reaches successful atomic publication");
    }
}
void AccessoryTextIdentityChecks() {
    AccessoryTextFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
    const auto before=AccessoryTextPose(f.accessory);f.CommitField(0,"12");
    const auto capacity=mdl::KeyPoolCapacity(f.app->AccessoryKeys(7));auto* grown=mdl::KeyPoolCreate<mdl::AccessoryKey>(capacity+4);
    std::memcpy(grown,f.app->AccessoryKeys(7),capacity*sizeof(mdl::AccessoryKey));mdl::KeyPoolCommitGrowth(f.app->AccessoryKeys(7),grown);
    Check(f.history.History(false)==MMS_OK&&AccessoryTextPose(f.accessory)==before,"accessory scalar undo survives identity preserving pool growth");
    Check(f.history.History(true)==MMS_OK,"accessory scalar redo survives pool growth");
    f.app->AccessorySlot(7)=&f.other;
    Check(f.history.History(false)==MMS_E_STALE&&f.history.UndoCount()==1&&f.accessory.position[0]==12,"slot replacement rejects undo without writing another object");
    f.app->AccessorySlot(7)=&f.accessory;
    auto* replacement=mdl::KeyPoolCreate<mdl::AccessoryKey>(capacity+4);auto* original=f.app->AccessoryKeys(7);f.app->AccessoryKeys(7)=replacement;
    Check(f.history.History(false)==MMS_E_STALE&&f.accessory.position[0]==12,"different pool identity rejects accessory undo");
    f.app->AccessoryKeys(7)=original;mdl::KeyPoolDestroy(replacement);
    // Forget/recreate simulates reuse at the same address, not controlled restoration.
    plugins::ForgetSceneObject(&f.accessory);plugins::SceneObjectIdentity(&f.accessory);
    Check(f.history.History(false)==MMS_E_STALE&&f.accessory.position[0]==12&&f.history.UndoCount()==1,"new stable ID at same address rejects old accessory edit");
}
void AccessoryTextGuardChecks() {
    AccessoryTextFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
    const auto before=AccessoryTextPose(f.accessory);bool changed=true;
    f.app->PlaybackActive()=1;f.CommitField(0,"12");f.app->PlaybackActive()=0;
    Check(AccessoryTextPose(f.accessory)==before&&!f.history.UndoCount(),"playback blocks accessory text without legacy fallback");
    Check(f.history.Enter()==MMS_OK,"reserve busy journal for accessory edit");f.CommitField(0,"12");f.history.Leave();
    Check(AccessoryTextPose(f.accessory)==before&&!f.history.UndoCount(),"busy journal blocks accessory text");
    Check(plugins::ApplyBoundAccessoryField(*f.app,255,0,1,changed)==MMS_E_ARGUMENT&&!changed,"invalid accessory slot rejected");
    Check(plugins::ApplyBoundAccessoryField(*f.app,7,8,1,changed)==MMS_E_ARGUMENT&&!changed,"invalid accessory component rejected");
    f.CommitField(6,"-2");Check(f.accessory.scale==-2&&f.history.UndoCount()==1,"legacy negative accessory scale remains supported");
    f.CommitField(7,"-2");Check(f.accessory.opacity==0&&f.history.UndoCount()==2,"accessory opacity lower clamp preserved");
}
}
void NativeAccessoryTextChecks(){AccessoryTextRoutingChecks();AccessoryTextFailureChecks();AccessoryTextIdentityChecks();AccessoryTextGuardChecks();}
