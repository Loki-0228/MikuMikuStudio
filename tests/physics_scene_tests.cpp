// MAINTENANCE: integration-manual; run with an existing PMM or PMD path.
// --physics-modes requires a PMM with physics disabled on its initial bones.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include <btBulletDynamicsCommon.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
using namespace mikumikustudio;
namespace {
int dialogs = 0;
LRESULT CALLBACK BlockDialog(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* info = reinterpret_cast<CBT_CREATEWNDW*>(lp);
        info->lpcs->style &= ~WS_VISIBLE;
    }
    if (code == HCBT_ACTIVATE) {
        auto window = reinterpret_cast<HWND>(wp);
        wchar_t kind[32]{}; GetClassNameW(window,kind,32);
        if (wcscmp(kind,L"#32770")==0) {
            ++dialogs; ShowWindow(window,SW_HIDE);
            PostMessageW(window,WM_COMMAND,IDCANCEL,0);
        }
        return 1;
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
bool Finite(const float* value,int count) {
    for(int i=0;i<count;++i)if(!std::isfinite(value[i]))return false;
    return true;
}
bool Finite(const btVector3& value) {
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}
int CheckPhysicsModes(MMDApp& app, const char* stage) {
    int mismatches=0, disabled=0, dynamic=0;
    const int mode=app.PlaybackPhysicsMode();
    for(int slot=0;slot<kModelSlotCount;++slot) {
        auto* raw=app.ModelSlot(slot); if(!raw)continue;
        const auto& model=*mdl::Mdl(raw);
        for(unsigned i=0;i<model.rigidCount;++i) {
            const auto& rigid=model.rigidTable[i];
            if(rigid.staticFlag || rigid.boneIndex<0 ||
               static_cast<unsigned>(rigid.boneIndex)>=model.boneCount)continue;
            const auto& bone=model.boneTable[rigid.boneIndex];
            if(!bone.hasRigidBody)continue;
            auto* body=static_cast<btRigidBody*>(rigid.body);
            if(!body) { ++mismatches; continue; }
            const bool off=mode==0 || (mode>=2 && bone.physicsDisabled);
            const int expected=off?0:(rigid.kinematicFlag?2:1);
            disabled+=bone.physicsDisabled!=0;
            dynamic+=rigid.mode!=0;
            if(rigid.mode!=expected || body->isKinematicObject()!=off)++mismatches;
        }
    }
    std::printf("physics_modes stage=%s mode=%d disabled=%d dynamic=%d mismatches=%d\n",
                stage,mode,disabled,dynamic,mismatches);
    // This runner needs a PMM with disabled physics bones to cover load order.
    return mismatches || !disabled;
}
int PhysicsModeTransitions(MMDApp& app) {
    int failed=CheckPhysicsModes(app,"loaded");
    const int savedMode=app.PlaybackPhysicsMode();
    for(unsigned id : {265u,269u,270u,272u,269u}) {
        SendMessageW(app.MainWindow(),WM_COMMAND,id,0);
        failed+=CheckPhysicsModes(app,"menu");
    }
    // Reload while the previous scene is in always-on mode. Saved per-bone
    // flags must reach Bullet even when the first frame does not change them.
    SendMessageW(app.MainWindow(),WM_COMMAND,265,0);
    LoadSceneFile();
    failed+=CheckPhysicsModes(app,"reloaded");
    failed+=app.PlaybackPhysicsMode()!=savedMode;
    SendMessageW(app.MainWindow(),WM_COMMAND,269,0);
    // Add an on/off pair to one loaded bone in memory only. The PMM on disk
    // stays untouched, and seeking must still activate later dynamic keys.
    bool testedKeys=false;
    for(int slot=0;slot<kModelSlotCount && !testedKeys;++slot) {
        auto* raw=app.ModelSlot(slot); if(!raw)continue;
        auto& model=*mdl::Mdl(raw);
        for(unsigned i=0;i<model.boneCount;++i) {
            auto& bone=model.boneTable[i];
            if(!bone.hasRigidBody || !bone.physicsDisabled ||
               model.boneKeys[i].frame!=0 || !model.boneKeys[i].physicsDisabled)continue;
            std::memset(model.boneSelection,0,model.boneCount);
            model.boneSelection[i]=1;
            CheckDlgButton(app.MainWindow(),panel::kPhysicsCheckbox,BST_CHECKED);
            RegisterSelectedBoneKeys(raw,15,2);
            CheckDlgButton(app.MainWindow(),panel::kPhysicsCheckbox,BST_UNCHECKED);
            RegisterSelectedBoneKeys(raw,30,2);
            for(int frame : {0,15,30,0}) {
                SeekModelFrame(raw,frame,2);
                failed+=(bone.physicsDisabled!=0)!=(frame!=15);
                failed+=CheckPhysicsModes(app,"keyframe");
            }
            testedKeys=true;
            break;
        }
    }
    failed+=!testedKeys;
    std::printf("physics_mode_failures=%d\n",failed);
    return failed;
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2 && !(argc==3 && wcscmp(argv[2],L"--physics-modes")==0))return 2;
    std::setvbuf(stdout,nullptr,_IONBF,0);
    auto* app=new MMDApp(); g_Block=app; app->state=MMDAppState{};app->InitDefaults();
    HHOOK hook=SetWindowsHookExW(WH_CBT,BlockDialog,nullptr,GetCurrentThreadId());
    if(!InitMainWindowAndD3D(app,GetModuleHandleW(nullptr),SW_HIDE))return 3;
    ShowWindow(app->MainWindow(),SW_HIDE);
    wcscpy_s(app->EnvFileName(),256,argv[1]);LoadSceneFile();
    auto* raw=app->ModelSlot(0);
    if(!raw || dialogs) { std::printf("load_failed dialogs=%d\n",dialogs);return 4; }
    if(argc==3) {
        const int failed=PhysicsModeTransitions(*app);
        ShutdownCleanup(app);delete app;g_Block=nullptr;UnhookWindowsHookEx(hook);
        return failed || dialogs?1:0;
    }
    app->SelectedModelSlot()=0;
    auto& model=*mdl::Mdl(raw);
    std::printf("loaded bones=%u rigid_bodies=%u joints=%u\n",model.boneCount,model.rigidCount,model.jointCount);
    int failed=0;
    for(int frame : {0,10,14,15,16,24,25,26,15,0}) {
        const HWND edit=GetDlgItem(app->MainWindow(),panel::kCurrentFrameEdit);
        SetWindowTextA(edit,std::to_string(frame).c_str());CommitEditControl(app,edit);
        for(int tick=0;tick<2;++tick) {
            app->DeltaTime()=1.f/30.f;app->state.messageSeen=10;FrameDriver(app);
        }
        int badBones=0,badBodies=0;
        for(unsigned i=0;i<model.boneCount;++i) {
            const auto& b=model.boneTable[i];
            if(!Finite(b.trans,3)||!Finite(b.rotQuat,4)||!Finite(b.matInit,16)||
               !Finite(b.ikBackup,7)) ++badBones;
        }
        for(unsigned i=0;i<model.rigidCount;++i) {
            auto* body=static_cast<btRigidBody*>(model.rigidTable[i].body);if(!body)continue;
            const auto& tr=body->getWorldTransform();
            if(!Finite(tr.getOrigin())||!Finite(tr.getBasis()[0])||!Finite(tr.getBasis()[1])||
               !Finite(tr.getBasis()[2])||!Finite(body->getLinearVelocity())||!Finite(body->getAngularVelocity()))++badBodies;
        }
        std::printf("frame=%d actual=%d bad_bones=%d bad_bodies=%d\n",frame,app->CurrentFrame(),badBones,badBodies);
        if(badBones || badBodies || app->CurrentFrame()!=frame) { ++failed; break; }
    }
    ShutdownCleanup(app);delete app;g_Block=nullptr;UnhookWindowsHookEx(hook);
    std::printf("scene_failures=%d dialogs=%d\n",failed,dialogs);return failed||dialogs?1:0;
}
