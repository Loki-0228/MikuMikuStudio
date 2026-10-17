#include "mikudancestudio/adaptive_shadow.hpp"
namespace mikudancestudio { void CmdControl500(MMDApp*,HWND,std::uint16_t,std::uint16_t); }
namespace {
struct ShadowParameterFixture : AccessoryRegistrationFixture {
    ShadowParameterFixture() {
        app->ShadowDistance()=.07512345f;app->SelfShadowMode()=1;
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_BAR_CLASSES};InitCommonControlsEx(&common);
        Check(CreateWindowExA(0,"EDIT","0",WS_CHILD,0,0,80,20,app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(561)),GetModuleHandleW(nullptr),nullptr)!=nullptr,"shadow range EDIT exists");
        const auto slider=CreateWindowExA(0,TRACKBAR_CLASSA,"",WS_CHILD,0,0,80,20,app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(560)),GetModuleHandleW(nullptr),nullptr);
        Check(slider!=nullptr,"shadow range slider exists");SendMessageW(slider,TBM_SETRANGE,FALSE,MAKELPARAM(0,9999));
        for(int id=562;id<=564;++id)Check(CreateWindowExA(0,"BUTTON","",WS_CHILD|BS_CHECKBOX,0,0,80,20,app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr)!=nullptr,"shadow mode button exists");
    }
    HWND Field(){return GetDlgItem(app->MainWindow(),561);}
    void Type(const char* input){SetWindowTextA(Field(),input);SendMessageA(Field(),EM_SETMODIFY,TRUE,0);}
    void Commit(const char* input){Type(input);CommitEditControl(app.get(),Field());}
    void Lose(){EditSubclassProc(Field(),WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);}
    void Mode(int mode){CmdControl500(app.get(),app->MainWindow(),std::uint16_t(562+mode),0);}
    bool Echo(int mode,int range){char text[32]{};GetWindowTextA(Field(),text,sizeof(text));
        if(std::atoi(text)!=range||SendDlgItemMessageW(app->MainWindow(),560,TBM_GETPOS,0,0)!=range)return false;
        for(int i=0;i<3;++i)if(SendDlgItemMessageW(app->MainWindow(),562+i,BM_GETCHECK,0,0)!=(i==mode?BST_CHECKED:BST_UNCHECKED))return false;
        return true;}
};
void ShadowParameterDistanceChecks() {
    ShadowParameterFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
    const float before=f.app->ShadowDistance();const auto ring=PoseRingDigest(f.first);
    const auto keys=AccessoryRegistrationMarks(*f.app);const auto accessory=f.accessory;
    const auto capacity=mdl::KeyPoolCapacity(f.app->ShadowKeys());const std::vector<mdl::SelfShadowKey> shadow(f.app->ShadowKeys(),f.app->ShadowKeys()+capacity);
    f.Type("4321.875");f.Lose();const float target=float((10000.0-4321)/100000.0);
    Check(f.app->ShadowDistance()==target&&f.history.UndoCount()==1&&f.Echo(1,4321)&&f.app->GlobalTrackSelected(GlobalTimelineTrack::SelfShadow),
        "shadow text truncates legacy input to integer then maps enhanced range and echoes controls");
    f.Lose();CommitEditControl(f.app.get(),f.Field());
    Check(f.app->ShadowDistance()==target&&f.history.UndoCount()==1,"shadow rounded echo does not rewrite distance or add history");
    f.app->SelfShadowMode()=2;f.app->CurrentFrame()=37;f.app->SelectedAccessorySlot()=8;f.app->CameraMode()=1;
    Check(f.history.History(false)==MMS_OK&&f.app->ShadowDistance()==before&&f.app->SelfShadowMode()==2&&f.app->CurrentFrame()==37,
        "shadow distance undo restores exact float while preserving later mode selection and frame");
    for(const char* invalid:{"NaN","inf","","3 junk","1e999"}) {
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;f.Commit(invalid);
        Check(f.app->ShadowDistance()==before&&f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),"invalid shadow text preserves authoring dirty flags and redo");
    }
    const std::string oversized(270,'1');f.Commit(oversized.c_str());
    Check(f.app->ShadowDistance()==before&&f.history.RedoCount()==1,"oversized shadow text rejected");
    Check(f.history.History(true)==MMS_OK&&f.app->ShadowDistance()==target,"shadow distance redo survives invalid input");
    f.app->SceneModified()=0;f.Commit("4321.875");Check(f.history.UndoCount()==1&&!f.app->SceneModified(),"same integer shadow input is no-op");
    f.Commit("12345678");Check(f.app->ShadowDistance()==.00001f&&f.Echo(2,9999),"shadow seven character out of range input clamps to enhanced upper endpoint");
    f.Commit("-999999");Check(f.app->ShadowDistance()==.1f&&f.Echo(2,0),"negative shadow range clamps to enhanced lower endpoint");
    f.Commit("1e100");Check(f.app->ShadowDistance()==.00001f&&f.Echo(2,9999),"finite huge shadow input clamps before safe integer conversion");
    Check(PoseRingDigest(f.first)==ring&&AccessoryRegistrationMarks(*f.app)==keys&&AccessoryRegistrationPoseEqual(f.accessory,accessory)&&
        !std::memcmp(f.app->ShadowKeys(),shadow.data(),shadow.size()*sizeof(mdl::SelfShadowKey)),"shadow distance history leaves keys model ring and other authoring unchanged");
}
void ShadowParameterModeChecks() {
    for(int mode=0;mode<3;++mode) {
        ShadowParameterFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        const int before=(mode+1)%3;f.app->SelfShadowMode()=before;const auto distance=f.app->ShadowDistance();
        f.Mode(mode);
        Check(f.history.UndoCount()==1&&f.app->SelfShadowMode()==mode&&f.app->ShadowDistance()==distance&&f.Echo(mode,ShadowSliderPosition(f.app.get())),
            "native shadow mode command publishes one delta and synchronizes radio group");
        const float later=.03456789f;f.app->ShadowDistance()=later;f.app->CurrentFrame()=29;
        Check(f.history.History(false)==MMS_OK&&f.app->SelfShadowMode()==before&&f.app->ShadowDistance()==later&&f.app->CurrentFrame()==29,
            "shadow mode undo preserves unrelated distance and frame");
        Check(f.history.History(true)==MMS_OK&&f.app->SelfShadowMode()==mode&&f.app->ShadowDistance()==later,"shadow mode redo changes only its own field");
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;f.Mode(mode);
        Check(f.history.UndoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),"same shadow mode is no-op");
        Check(f.history.History(false)==MMS_OK,"seed shadow mode redo");
        f.Mode(before);Check(f.history.RedoCount()==1,"no-op mode preserves redo");
    }
    ShadowParameterFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
    f.app->SelfShadowMode()=-1;f.Mode(0);Check(f.history.History(false)==MMS_OK&&f.app->SelfShadowMode()==-1,"shadow mode history preserves exact preexisting signed mode");
}
void ShadowParameterFailureChecks() {
    for(int field=0;field<2;++field) {
        ShadowParameterFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        if(field)f.Mode(0);else f.Commit("3000");Check(f.history.History(false)==MMS_OK,"seed shadow parameter redo");
        const auto before=f.app->ShadowDistance();const auto mode=f.app->SelfShadowMode();const auto keys=AccessoryRegistrationMarks(*f.app);const auto ring=PoseRingDigest(f.first);
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
        unsigned failures=0;bool succeeded=false;
        for(long limit=0;limit<128;++limit) {
            if(!field)f.Type("4321.875");failAfter=limit;
            if(field)f.Mode(2);else CommitEditControl(f.app.get(),f.Field());failAfter=-1;
            if(f.history.UndoCount()){succeeded=true;break;}++failures;
            Check(f.app->ShadowDistance()==before&&f.app->SelfShadowMode()==mode&&f.history.RedoCount()==1&&AccessoryRegistrationMarks(*f.app)==keys&&
                PoseRingDigest(f.first)==ring&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),"shadow allocation failure preserves both fields keys ring dirty and redo");
            Check(f.history.Enter()==MMS_OK,"shadow failure releases journal gate");f.history.Leave();
        }
        Check(succeeded&&failures>=5,"shadow allocation sweep reaches successful publication");
    }
}
void ShadowParameterIdentityAndGuardChecks() {
    ShadowParameterFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
    const auto before=f.app->ShadowDistance();f.app->PlaybackActive()=1;f.Commit("4321");f.Mode(2);f.app->PlaybackActive()=0;
    Check(f.app->ShadowDistance()==before&&f.app->SelfShadowMode()==1&&!f.history.UndoCount(),"playback blocks shadow fields without legacy fallback");
    Check(f.history.Enter()==MMS_OK,"hold journal for shadow busy test");f.Commit("4321");f.Mode(2);f.history.Leave();
    Check(f.app->ShadowDistance()==before&&f.app->SelfShadowMode()==1&&!f.history.UndoCount(),"busy journal blocks both shadow fields");
    SendDlgItemMessageW(f.app->MainWindow(),564,BM_SETCHECK,BST_CHECKED,0);f.Mode(1);
    Check(f.Echo(1,ShadowSliderPosition(f.app.get()))&&!f.history.UndoCount(),"no-op restores already toggled native shadow buttons");
    bool changed=true;
    Check(plugins::ApplyBoundShadowDistance(*f.app,std::numeric_limits<float>::infinity(),changed)==MMS_E_ARGUMENT&&!changed,"nonfinite direct shadow distance rejected");
    Check(plugins::TrySetBoundShadowMode(f.app.get(),3)&&f.app->SelfShadowMode()==1&&!f.history.UndoCount(),"invalid shadow mode consumed without mutation");
    f.Commit("4321");f.Mode(2);const auto count=mdl::KeyPoolCapacity(f.app->ShadowKeys());auto* grown=mdl::KeyPoolCreate<mdl::SelfShadowKey>(count+4);
    std::memcpy(grown,f.app->ShadowKeys(),count*sizeof(mdl::SelfShadowKey));mdl::KeyPoolCommitGrowth(f.app->ShadowKeys(),grown);
    Check(f.history.History(false)==MMS_OK&&f.history.History(false)==MMS_OK&&f.app->ShadowDistance()==before&&f.app->SelfShadowMode()==1,"both shadow deltas survive pool growth");
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK,"both shadow deltas redo after growth");
    auto* original=f.app->ShadowKeys();f.app->ShadowKeys()=mdl::KeyPoolCreate<mdl::SelfShadowKey>(count+4);
    Check(f.history.History(false)==MMS_E_STALE&&f.history.UndoCount()==2&&f.app->SelfShadowMode()==2,"replaced shadow pool rejects mode undo with cursor intact");
    auto* replacement=f.app->ShadowKeys();f.app->ShadowKeys()=original;mdl::KeyPoolDestroy(replacement);
    Check(f.history.History(false)==MMS_OK,"restore pool to inspect distance stale protection");
    replacement=mdl::KeyPoolCreate<mdl::SelfShadowKey>(count+4);f.app->ShadowKeys()=replacement;
    Check(f.history.History(false)==MMS_E_STALE&&f.history.UndoCount()==1&&f.app->ShadowDistance()==float((10000.0-4321)/100000.0),"replaced shadow pool rejects distance undo");
    f.app->ShadowKeys()=original;mdl::KeyPoolDestroy(replacement);
}
}
void NativeShadowParameterChecks(){ShadowParameterDistanceChecks();ShadowParameterModeChecks();ShadowParameterFailureChecks();ShadowParameterIdentityAndGuardChecks();}
