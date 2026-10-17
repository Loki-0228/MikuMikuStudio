#include "../../src/plugins/native_viewport_history.hpp"
namespace viewport_history_host_checks {
using Values=std::array<float,7>;
Values Read(MMDApp& a){return {a.CameraPositionX(),a.CameraPositionY(),a.CameraPositionZ(),a.CameraPitch(),a.CameraYaw(),a.CameraRoll(),a.CameraDistance()};}
bool Same(const Values& a,const Values& b){return !std::memcmp(a.data(),b.data(),sizeof(a));}
void RunWheel(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    using edit_history_host_checks::Menu;
    const auto previousFloating=app->FloatingWindow();const auto main=app->MainWindow();const auto initial=value();
    const auto mouseX=app->MouseX();const auto sidebar=app->SidebarWidth();const auto parentBone=app->CameraParentBone();
    // The detached production window procedure receives real messages on an
    // owned hidden HWND. Avoid moving the user's controls through the View menu.
    const auto floating=CreateWindowExW(0,L"STATIC",L"ABI wheel detached viewport",WS_OVERLAPPED,0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(floating!=nullptr,"real wheel fixture creates its own detached-message HWND");
    const auto previousProcedure=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(floating,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(MicWndProc)));
    Check(previousProcedure!=nullptr,"real wheel fixture installs production MicWndProc on owned HWND");
    for(bool detached:{false,true})for(int parent:{-1,0})for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        Check(!history.UndoCount()&&!history.RedoCount(),"real wheel fixture starts with empty journal");
        Check(history.SetMode(mode)==MMS_OK,"real wheel fixture configures recording mode");
        app->FloatingWindow()=detached?floating:previousFloating;app->CameraMode()=1;app->CameraParentModel()=parent;app->CameraParentBone()=0;
        app->SidebarWidth()=200;app->MouseX()=detached?0:500;
        Menu(app,L"Edit probe once");const auto before=Read(*app);auto expected=before;
        const auto wheelOwner=detached?floating:main;
        const unsigned axis=!detached&&parent>=0?2:6;
        if(detached)expected[axis]=static_cast<float>(120.0*0.05000000074505806+static_cast<double>(before[axis]));
        else expected[axis]=120.0f*0.05f+before[axis];
        const auto frame=app->CurrentFrame();
        SendMessageW(wheelOwner,WM_MOUSEWHEEL,MAKEWPARAM(0,120),0);const auto after=Read(*app);
        Check(Same(after,expected)&&app->CurrentFrame()==frame&&value()==initial+1,
            "real main/detached wheel preserves respective legacy math, plugin value and playhead");
        Check(history.UndoCount()==(mode==plugins::SharedHistory::RecordingMode::AllOperations?2:1),
            "real WM_MOUSEWHEEL records exactly one adjustment or explicit-only preview");
        if(mode==plugins::SharedHistory::RecordingMode::AllOperations){
            app->CameraMode()=0;app->CurrentFrame()=107;Menu(app,L"Edit probe undo");
            Check(Same(Read(*app),before)&&value()==initial+1,"real wheel undo crosses model mode and preserves preceding plugin edit");
            Menu(app,L"Edit probe redo");
            Check(Same(Read(*app),after)&&app->CurrentFrame()==107,"real wheel redo restores exact pose without moving playhead");
            Menu(app,L"Edit probe undo");app->CameraMode()=1;
        }
        Menu(app,L"Edit probe undo");Check(value()==initial,"real preceding plugin edit stays independently undoable");
        const auto zeroBefore=Read(*app);app->SceneModified()=0;
        SendMessageW(wheelOwner,WM_MOUSEWHEEL,MAKEWPARAM(0,0),0);
        Check(Same(Read(*app),zeroBefore)&&!app->SceneModified()&&history.RedoCount()==(mode==plugins::SharedHistory::RecordingMode::AllOperations?2:1),
            "real zero-delta wheel preserves pose, scene modified flag and existing redo nodes");
        Menu(app,L"Edit probe redo");Check(value()==initial+1,"real plugin redo remains valid after zero-delta wheel");
        Menu(app,L"Edit probe undo");
        Menu(app,L"Edit probe prepared");const auto pendingBefore=Read(*app);const auto pendingUndo=history.UndoCount(),pendingRedo=history.RedoCount();
        SendMessageW(wheelOwner,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-120)),0);
        Check(Same(Read(*app),pendingBefore)&&history.UndoCount()==pendingUndo&&history.RedoCount()==pendingRedo,
            "real pending plugin transaction blocks wheel without falling through to legacy authoring");Menu(app,L"Edit probe cancel");
        app->PlaybackActive()=1;SendMessageW(wheelOwner,WM_MOUSEWHEEL,MAKEWPARAM(0,120),0);app->PlaybackActive()=0;
        Check(Same(Read(*app),pendingBefore),"real playback blocks bound wheel camera mutation");
        app->CameraMode()=0;SendMessageW(wheelOwner,WM_MOUSEWHEEL,MAKEWPARAM(0,120),0);app->CameraMode()=1;
        Check(Same(Read(*app),pendingBefore),"real unmigrated model-mode wheel is consumed instead of old-path mutation");
        Check(history.Reset()==MMS_OK,"real wheel fixture resets its journal");history.Collect();
    }
    app->FloatingWindow()=previousFloating;SetWindowLongPtrW(floating,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(previousProcedure));DestroyWindow(floating);
    app->MouseX()=mouseX;app->SidebarWidth()=sidebar;app->CameraParentBone()=parentBone;
}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    using edit_history_host_checks::Menu;
    const auto oldMode=history.Mode();const auto cameraMode=app->CameraMode();const auto frame=app->CurrentFrame();
    const auto parent=app->CameraParentModel();const auto rectangle=app->ViewportRect();const auto initial=value();const auto original=Read(*app);
    Check(!history.UndoCount(),"real camera gesture fixture starts with empty journal");
    app->CameraMode()=1;app->CameraParentModel()=-1;app->ViewportRect()={0,0,800,600};
    const auto owner=app->MainWindow();
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        Check(history.SetMode(mode)==MMS_OK,"real camera gesture recording mode configured");
        Menu(app,L"Edit probe once");const auto before=Read(*app);
        SendMessageW(owner,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(100,100));
        Check(GetCapture()==owner,"real right-button viewport press owns capture");
        SendMessageW(owner,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(111,106));
        SendMessageW(owner,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(119,108));
        Check(history.UndoCount()==1&&!Same(Read(*app),before),"real right drag previews without per-move journal nodes");
        SendMessageW(owner,WM_RBUTTONUP,0,MAKELPARAM(122,110));const auto after=Read(*app);
        Check(GetCapture()!=owner&&history.UndoCount()==(mode==plugins::SharedHistory::RecordingMode::AllOperations?2:1),"real release includes final delta and commits one adjustment node");
        if(mode==plugins::SharedHistory::RecordingMode::AllOperations){
            app->CameraMode()=0;app->CurrentFrame()=103;Menu(app,L"Edit probe undo");
            Check(Same(Read(*app),before)&&value()==initial+1,"real gesture undo crosses mode without consuming preceding plugin edit");
            Menu(app,L"Edit probe redo");Check(Same(Read(*app),after)&&app->CurrentFrame()==103,"real gesture redo restores exact final pose without seeking");
            Menu(app,L"Edit probe undo");app->CameraMode()=1;
        }
        Menu(app,L"Edit probe undo");Check(value()==initial,"real plugin history remains independently undoable");
        const auto cancelBefore=Read(*app);
        SendMessageW(owner,WM_MBUTTONDOWN,MK_MBUTTON,MAKELPARAM(100,100));
        SendMessageW(owner,WM_MOUSEMOVE,MK_MBUTTON,MAKELPARAM(112,103));
        SendMessageW(owner,WM_CANCELMODE,0,0);
        Check(Same(Read(*app),cancelBefore)&&GetCapture()!=owner,"real middle pan cancellation restores pose and releases capture");
        Check(history.Reset()==MMS_OK,"real viewport fixture resets journal");history.Collect();
    }
    RunWheel(app,history,value);
    plugins::CancelViewportCameraGesture(*app);app->CameraMode()=cameraMode;app->CurrentFrame()=frame;app->CameraParentModel()=parent;app->ViewportRect()=rectangle;
    for(unsigned i=0;i<3;++i){app->CameraPositionAxis(i)=original[i];app->CameraRotation()[i]=original[i+3];}app->CameraDistance()=original[6];
    Check(history.SetMode(oldMode)==MMS_OK,"real viewport fixture restores recording configuration");
}
// Invoke after the host's existing shutdown test and DLL export lease release.
// No caller may retain/use the old runtime's history or plugin function pointers.
void RunLifecycle(MMDApp* app){
    Check(!plugins::NativeHistory(app)&&!plugins::BoundNativePoseHistory(app),"lifecycle fixture starts after real runtime shutdown");
    const auto original=Read(*app);const auto cameraMode=app->CameraMode();const auto rectangle=app->ViewportRect();const auto modified=app->SceneModified();
    app->CameraMode()=1;app->ViewportRect()={0,0,800,600};const auto owner=app->MainWindow();
    for(unsigned attempt=0;attempt<2;++attempt){
        Check(plugins::InitializeHistoryTestRuntime(app),"real runtime can initialize again on the same thread and app");
        auto* history=plugins::NativeHistory(app);Check(history&&plugins::BoundNativePoseHistory(app)==history,"rebuilt runtime binds a new common history");
        SendMessageW(owner,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(100,100));
        SendMessageW(owner,WM_MOUSEMOVE,MK_RBUTTON,MAKELPARAM(113,106));
        Check(GetCapture()==owner&&!Same(Read(*app),original)&&!history->UndoCount(),"runtime shutdown fixture leaves a real gesture unfinished with changed preview and no history node");
        plugins::Shutdown();
        Check(!plugins::NativeHistory(app)&&!plugins::BoundNativePoseHistory(app)&&GetCapture()!=owner&&Same(Read(*app),original),"shutdown cancels gesture and capture before journal destruction without waiting for WM_NCDESTROY");
    }
    app->CameraMode()=cameraMode;app->ViewportRect()=rectangle;app->SceneModified()=modified;
}
}
