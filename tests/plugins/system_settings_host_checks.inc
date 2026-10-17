namespace system_settings_host_checks {
struct Dialog {
    MMDApp* app;bool record,save;unsigned seen=0;
    BoneNumericDialog hidden;
    HHOOK inspect=nullptr;
    inline static thread_local Dialog* active=nullptr;
    static LRESULT CALLBACK Initialized(int code,WPARAM wp,LPARAM lp) {
        if(code>=0&&active) {
            const auto* call=reinterpret_cast<CWPRETSTRUCT*>(lp);
            if(call->message==WM_INITDIALOG&&GetWindow(call->hwnd,GW_OWNER)==active->app->MainWindow()&&
                GetDlgItem(call->hwnd,101)&&GetDlgItem(call->hwnd,102)) {
                ++active->seen;
                Check(IsDlgButtonChecked(call->hwnd,system_settings_ui::RecordAdjustmentsPreference()?101:102)==BST_CHECKED,
                    "system settings dialog initializes history choice from unified settings");
                CheckRadioButton(call->hwnd,101,102,active->record?101:102);
                if(!active->save) {
                    SendMessageW(call->hwnd,WM_COMMAND,105,0);
                    const auto recommended=GetDlgItemInt(call->hwnd,104,nullptr,FALSE);
                    Check(recommended>=16&&recommended<=256&&GetDlgItemInt(call->hwnd,103,nullptr,FALSE)>=1,
                        "automatic history recommendation stays within conservative memory bounds");
                }
                SetDlgItemInt(call->hwnd,103,12,FALSE);SetDlgItemInt(call->hwnd,104,32,FALSE);
                PostMessageW(call->hwnd,WM_COMMAND,active->save?IDOK:IDCANCEL,0);
            }
        }
        return CallNextHookEx(nullptr,code,wp,lp);
    }
    Dialog(MMDApp* owner,bool all,bool commit):app(owner),record(all),save(commit),hidden(owner,BoneNumericDialog::Cancel) {
        Check(!active,"no nested system settings driver");active=this;
        inspect=SetWindowsHookExW(WH_CALLWNDPROCRET,Initialized,nullptr,GetCurrentThreadId());
        Check(inspect!=nullptr,"observe hidden real system settings dialog");
    }
    ~Dialog(){UnhookWindowsHookEx(inspect);active=nullptr;}
    void Run(){SendMessageW(app->MainWindow(),WM_COMMAND,system_settings_ui::kCommand,0);Check(seen==1,"help system settings opens exactly one dialog");}
};
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()) {
    using Mode=plugins::SharedHistory::RecordingMode;
    system_settings_ui::EnsureItem(app);system_settings_ui::EnsureItem(app);
    Check(GetMenuState(GetMenu(app->MainWindow()),system_settings_ui::kCommand,MF_BYCOMMAND)!=UINT(-1),"help exposes system settings command");
    const auto original=app->CameraFov();const auto frame=app->CurrentFrame();
    const auto before=history.UndoCount();
    {Dialog dialog(app,false,true);dialog.Run();}
    Check(!system_settings_ui::RecordAdjustmentsPreference()&&history.Mode()==Mode::ExplicitOnly&&history.UndoCount()==before&&
        history.MaxEntries()==12&&history.MaxBytes()==32ull*1024*1024&&system_settings_ui::HistoryPreference().steps==12&&system_settings_ui::HistoryPreference().memoryMiB==32,
        "saving explicit-only mode persists and applies without adding history");
    const auto slider=GetDlgItem(app->MainWindow(),panel::kFovSlider);
    const auto scroll=[&](int tick){SendMessageW(slider,TBM_SETPOS,TRUE,tick);SendMessageW(app->MainWindow(),WM_HSCROLL,TB_THUMBTRACK,reinterpret_cast<LPARAM>(slider));};
    scroll(67);Check(app->CameraFov()==67&&!history.UndoCount(),"real explicit-only FOV adjustment remains live without history");
    Menu(app,L"Edit probe once");scroll(75);
    Check(value()==1&&history.UndoCount()==1&&app->CameraFov()==75,"explicit plugin commit still records while later FOV adjustment does not");
    Menu(app,L"Edit probe undo");Check(value()==0&&app->CameraFov()==75,"undo explicit plugin commit preserves unregistered FOV preview");
    {Dialog dialog(app,true,false);dialog.Run();}
    Check(history.Mode()==Mode::ExplicitOnly&&!system_settings_ui::RecordAdjustmentsPreference()&&history.RedoCount()==1,
        "cancelling system settings keeps both recording preference and redo");
    {Dialog dialog(app,true,true);dialog.Run();}
    Check(history.Mode()==Mode::AllOperations&&system_settings_ui::RecordAdjustmentsPreference()&&history.RedoCount()==1,
        "all-operations choice persists and applies without clearing redo");
    Menu(app,L"Edit probe redo");scroll(80);
    Check(history.UndoCount()==2&&app->CameraFov()==80,"real all-operations adjustment adds one history node");
    Menu(app,L"Edit probe undo");Check(app->CameraFov()==75&&value()==1,"all-operations undo returns to last unregistered preview");Menu(app,L"Edit probe undo");
    Check(value()==0&&history.Reset()==MMS_OK&&app->CurrentFrame()==frame,"restore settings test history without seeking");
    app->CameraFov()=original;plugins::RefreshBoundGlobalReadout(*app);
    Check(history.Configure(Mode::AllOperations,128,256ull*1024*1024)==MMS_OK,"restore broad migration-probe history limits");
    std::puts("PASS help system settings dialog save cancel, unified preference and both real history modes");
}
}
