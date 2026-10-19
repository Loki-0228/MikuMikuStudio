// Include after native_numpad_view_checks.inc and native_camera_hud_history_checks.inc.
// CPU fixtures use owned native pools and real hidden controls; the final suite
// also routes actual queued mouse-up/capture messages through MicWndProc.
#include "../src/plugins/native_accessory_hud_history.hpp"
#include "../src/plugins/scene_history.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/viewport_tool_layout.hpp"
#include "mikumikustudio/viewport_corner_settings.hpp"
namespace mikumikustudio { int ViewportToolAtPointForTest(MMDApp*); }
namespace {
using AccessoryHudValues=std::array<float,6>;
AccessoryHudValues AccessoryHudRead(const mdl::AccessoryRecord& a){return {a.position[0],a.position[1],a.position[2],a.rotation[0],a.rotation[1],a.rotation[2]};}
bool AccessoryHudSame(const mdl::AccessoryRecord& a,const mdl::AccessoryRecord& b){return !std::memcmp(&a,&b,sizeof(a));}
void AccessoryHudSet(mdl::AccessoryRecord& a,unsigned field,float value){if(field<3)a.position[field]=value;else a.rotation[field-3]=value;}
struct AccessoryHudFixture:CameraHudFixture{
    mdl::AccessoryRecord other{};
    AccessoryHudFixture(){
        app->AccessorySlot(8)=&other;app->SelectedAccessorySlot()=7;app->state.coordinateSystem=2;app->EditMode()=ViewportEditMode::Camera;
        for(unsigned slot:{7u,8u}){auto& a=*app->AccessorySlot(slot);a.parentModel=-1;a.parentBone=0;a.scale=1.2345f;a.opacity=.4567f;
            a.visible=1;a.shadowEnabled=0;a.additiveBlend=3;a.rowSelected=1;a.order=std::uint8_t(slot);
            for(unsigned axis=0;axis<3;++axis){a.position[axis]=3.125f+float(axis+slot);a.rotation[axis]=.23f-float(axis)*.3f;}
            app->AccessoryKeys(slot)=mdl::KeyPoolCreate<mdl::AccessoryKey>(4);
            for(unsigned i=0;i<3;++i){auto& k=app->AccessoryKeys(slot)[i];k.frame=i==0?0:i==1?12:31;k.previous=i?i-1:0;k.next=i<2?i+1:0;
                k.parentModel=-1;k.visible=1;k.scale=1.f+float(i)*.125f;k.opacity=.8f-float(i)*.1f;k.position[0]=float(i);k.selected=std::uint8_t(i);}
            plugins::SceneObjectIdentity(&a);
        }
        for(int id:{478,479,480,481,482,483})Check(CreateWindowExA(0,"EDIT","0",WS_CHILD,0,0,80,20,app->MainWindow(),
            reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr)!=nullptr,"create native accessory HUD numeric echo control");
    }
    ~AccessoryHudFixture(){plugins::CancelAccessoryHudGesture(*app);history.Reset();history.Collect();
        for(unsigned slot:{7u,8u}){plugins::ForgetSceneObject(app->AccessorySlot(slot));mdl::KeyPoolDestroy(app->AccessoryKeys(slot));}
        app->AccessorySlot(8)=nullptr;
    }
};
struct AccessoryHudInvariant{
    NumpadView camera;mdl::AccessoryRecord other;std::vector<mdl::CameraKey> cameras;
    std::array<std::vector<mdl::AccessoryKey>,2> accessories;std::array<std::uint64_t,2> identities;
    std::uint64_t first,second,ring;
    explicit AccessoryHudInvariant(AccessoryHudFixture& f):camera(NumpadRead(*f.app)),other(f.other),cameras(GlobalRegistrationKeys(f.app->CameraKeys())),
        accessories{GlobalRegistrationKeys(f.app->AccessoryKeys(7)),GlobalRegistrationKeys(f.app->AccessoryKeys(8))},
        identities{mdl::KeyPoolIdentity(f.app->AccessoryKeys(7)),mdl::KeyPoolIdentity(f.app->AccessoryKeys(8))},first(f.first.Digest()),second(f.second.Digest()),ring(PoseRingDigest(f.first)){}
    void CheckUnchanged(AccessoryHudFixture& f)const{Check(NumpadSame(camera,NumpadRead(*f.app))&&AccessoryHudSame(other,f.other)&&
        GlobalRegistrationSame(f.app->CameraKeys(),cameras)&&GlobalRegistrationSame(f.app->AccessoryKeys(7),accessories[0])&&
        GlobalRegistrationSame(f.app->AccessoryKeys(8),accessories[1])&&mdl::KeyPoolIdentity(f.app->AccessoryKeys(7))==identities[0]&&
        mdl::KeyPoolIdentity(f.app->AccessoryKeys(8))==identities[1]&&f.first.Digest()==first&&f.second.Digest()==second&&PoseRingDigest(f.first)==ring,
        "accessory HUD preserves camera, every key, both model poses/physics/old rings and the unrelated accessory");}
};
void AccessoryHudSample(MMDApp& app,int operation,int dy,unsigned modifier=0){
    app.PreviousMouseX()=100;app.PreviousMouseY()=100;app.MouseX()=103;app.MouseY()=100+dy;
    app.ShiftModifierState()=(modifier&1)?3:0;app.CtrlModifierState()=(modifier&2)?3:0;
    if(operation<18)ModeCameraAdjust(&app,operation-15);else ModeAngleAdjust(&app,operation-18);
}
void AccessoryHudMathAndHistory(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(int operation=15;operation<=20;++operation)for(int parent:{-1,1})for(unsigned modifier:{0u,1u,2u,3u})
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        AccessoryHudFixture f,oracle;f.accessory.parentModel=oracle.accessory.parentModel=parent;
        plugins::NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"configure accessory HUD recording policy");
        const auto before=f.accessory;const AccessoryHudInvariant invariants(f);
        Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,operation)==MMS_OK,"reserve a complete selected-accessory six-axis HUD gesture before authoring");
        Check(f.history.SetMode(mode)==MMS_E_BUSY,"accessory HUD owns a single shared journal reservation");
        for(int dy:{3,-7,11,2}){AccessoryHudSample(*oracle.app,operation,dy,modifier);failAfter=0;AccessoryHudSample(*f.app,operation,dy,modifier);failAfter=-1;
            Check(AccessoryHudSame(f.accessory,oracle.accessory)&&!f.history.UndoCount(),"accessory HUD matches unchanged native subtract-position/add-angle double-to-float math and modifier precedence without allocating");}
        bool changed=false;failAfter=0;const auto result=plugins::EndAccessoryHudGesture(*f.app,changed);failAfter=-1;const auto after=f.accessory;
        Check(result==MMS_OK&&changed&&f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u),"whole accessory HUD gesture commits exactly one adjustment or an explicit-only preview");
        invariants.CheckUnchanged(f);
        if(mode==Mode::AllOperations){auto undo=before,redo=after;const unsigned field=unsigned(operation-15),foreign=(field+1)%6;
            AccessoryHudSet(f.accessory,foreign,AccessoryHudRead(f.accessory)[foreign]+8.f);
            f.accessory.scale=2.375f;f.accessory.opacity=.73125f;f.accessory.visible=0;f.accessory.additiveBlend=7;f.accessory.shadowEnabled=1;
            f.accessory.name[0]='X';f.accessory.order=4;wcscpy_s(f.accessory.sourcePath,L"Z:\\missing-HUD-source.x");
            for(auto* target:{&undo,&redo}){AccessoryHudSet(*target,foreign,AccessoryHudRead(f.accessory)[foreign]);target->scale=f.accessory.scale;target->opacity=f.accessory.opacity;
                target->visible=f.accessory.visible;target->additiveBlend=f.accessory.additiveBlend;target->shadowEnabled=f.accessory.shadowEnabled;
                target->name[0]=f.accessory.name[0];target->order=f.accessory.order;wcscpy_s(target->sourcePath,f.accessory.sourcePath);}
            f.app->CurrentFrame()=91;f.app->CameraMode()=0;f.app->state.coordinateSystem=0;f.app->SelectedAccessorySlot()=8;f.app->SelectedModelSlot()=1;
            Check(plugins::RunBoundObjectHistory(*f.app,f.history,false,true)==MMS_OK&&AccessoryHudSame(f.accessory,undo)&&f.app->CurrentFrame()==91&&
                f.app->SelectedAccessorySlot()==8&&f.app->SelectedModelSlot()==1,"authorized accessory HUD undo crosses frame, editor and object selection while preserving independent unregistered fields and source metadata");
            Check(plugins::RunBoundObjectHistory(*f.app,f.history,true,true)==MMS_OK&&AccessoryHudSame(f.accessory,redo),"authorized accessory HUD redo writes only frozen changed fields without disk, keys or seeking");
            invariants.CheckUnchanged(f);
        }
    }
}
void AccessoryHudCancellationAndFailure(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        AccessoryHudFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.history.SetMode(mode);const auto before=f.accessory;
        for(UINT message:{UINT(WM_CANCELMODE),UINT(WM_KILLFOCUS),UINT(WM_CAPTURECHANGED),UINT(WM_KEYDOWN),UINT(WM_ACTIVATEAPP),UINT(WM_CLOSE),UINT(WM_DESTROY),UINT(WM_NCDESTROY)}){
            Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,18)==MMS_OK,"begin accessory HUD before a lifecycle cancellation");AccessoryHudSample(*f.app,18,7);
            failAfter=0;plugins::HandleBoundViewportMessage(*f.app,nullptr,message,message==WM_KEYDOWN?VK_ESCAPE:0,message==WM_CAPTURECHANGED?LPARAM(1):0);failAfter=-1;
            Check(!plugins::AccessoryHudGestureActive(*f.app)&&AccessoryHudSame(f.accessory,before)&&!f.history.UndoCount(),"escape/focus/capture/deactivation/close/destruction cancels accessory HUD without allocation or partial history");
        }
        for(int operation:{15,18})for(bool sameField:{false,true}){
            Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,operation)==MMS_OK,"begin accessory HUD before an independent author conflict");
            AccessoryHudSample(*f.app,operation,3);const unsigned owned=unsigned(operation-15),foreign=sameField?owned:(owned+1)%6;
            AccessoryHudSet(f.accessory,foreign,AccessoryHudRead(f.accessory)[foreign]+8.f);auto expected=before;
            AccessoryHudSet(expected,foreign,AccessoryHudRead(f.accessory)[foreign]);f.accessory.scale=expected.scale=2.75f;
            Check(!plugins::AllowAccessoryHudPreview(*f.app,operation)&&!plugins::AccessoryHudGestureActive(*f.app)&&AccessoryHudSame(f.accessory,expected)&&!f.history.UndoCount(),
                "accessory cancellation restores each still-owned preview field and preserves the foreign pose field and unregistered scale");f.accessory=before;
        }
        for(unsigned ui=0;ui<3;++ui){Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,15)==MMS_OK,"begin accessory HUD before UI context changes");
            AccessoryHudSample(*f.app,15,3);if(ui==0)f.app->state.coordinateSystem=1;else if(ui==1)f.app->CameraMode()=0;else f.app->SelectedAccessorySlot()=8;
            Check(!plugins::AllowAccessoryHudPreview(*f.app,15)&&AccessoryHudSame(f.accessory,before)&&!plugins::AccessoryHudGestureActive(*f.app),
                "coordinate, editor and selected-accessory changes cancel owned preview without reverting the new UI choice");
            Check(ui==0?f.app->state.coordinateSystem==1:ui==1?f.app->CameraMode()==0:f.app->SelectedAccessorySlot()==8,"accessory HUD cancellation preserves the changed UI context");
            f.app->CameraMode()=1;f.app->state.coordinateSystem=2;f.app->SelectedAccessorySlot()=7;
        }
    }
    AccessoryHudFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=f.accessory;unsigned failed=0;bool ready=false;
    for(long allowance=0;allowance<80;++allowance){failAfter=allowance;const auto result=plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,15);failAfter=-1;
        Check(AccessoryHudSame(f.accessory,before)&&!f.history.UndoCount(),"accessory HUD OOM preparation leaves every source byte and journal node unchanged");
        if(result==MMS_OK){ready=true;plugins::CancelAccessoryHudGesture(*f.app);break;}
        Check(result==MMS_E_MEMORY&&!plugins::AccessoryHudGestureActive(*f.app),"accessory HUD allocation refusal never exposes a half-started gesture");++failed;}
    Check(ready&&failed>3,"accessory HUD allocation sweep reaches reservation and complete preparation");
    plugins::StorageStore storage;plugins::SharedHistory tiny(storage,{},1,1);
    Check(plugins::BeginAccessoryHudGesture(*f.app,tiny,nullptr,15)==MMS_E_MEMORY&&AccessoryHudSame(f.accessory,before)&&!plugins::AccessoryHudGestureActive(*f.app),
        "accessory HUD retained budget refuses before the first native preview");
    Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,18)==MMS_OK,"begin accessory HUD before nonfinite sample");AccessoryHudSample(*f.app,18,3);
    f.accessory.rotation[0]=std::numeric_limits<float>::infinity();failAfter=0;const auto accepted=plugins::AcceptAccessoryHudPreview(*f.app);failAfter=-1;
    Check(!accepted&&!plugins::AccessoryHudGestureActive(*f.app)&&AccessoryHudSame(f.accessory,before),"nonfinite accessory sample cancels back to the complete original pose without allocation");
    Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,15)==MMS_OK,"begin accessory HUD before an independent nonfinite author change");AccessoryHudSample(*f.app,15,3);
    auto foreignNonfinite=before;f.accessory.rotation[1]=foreignNonfinite.rotation[1]=std::numeric_limits<float>::infinity();
    failAfter=0;const auto foreignAccepted=plugins::AcceptAccessoryHudPreview(*f.app);failAfter=-1;
    Check(!foreignAccepted&&!plugins::AccessoryHudGestureActive(*f.app)&&AccessoryHudSame(f.accessory,foreignNonfinite),
        "invalid independent author data is preserved while only the accessory HUD's owned axis is rolled back");f.accessory=before;
    f.accessory.position[0]=std::numeric_limits<float>::infinity();
    Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,15)==MMS_E_ARGUMENT&&std::isinf(f.accessory.position[0]),"nonfinite accessory source is refused without replacing it");f.accessory=before;
    Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,15)==MMS_OK,"begin accessory HUD to seed undo and no-op redo");AccessoryHudSample(*f.app,15,3);bool changed=false;
    Check(plugins::EndAccessoryHudGesture(*f.app,changed)==MMS_OK&&changed,"commit complete accessory HUD source for failure/no-op checks");const auto after=f.accessory;
    failAfter=0;const auto undoResult=f.history.History(false);failAfter=-1;
    Check(undoResult==MMS_E_MEMORY&&AccessoryHudSame(f.accessory,after)&&f.history.UndoCount()==1&&!f.history.RedoCount(),"accessory HUD undo preparation OOM preserves the complete live pose and original history direction");
    Check(f.history.History(false)==MMS_OK&&AccessoryHudSame(f.accessory,before),"retry accessory HUD undo after allocation recovery");
    Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,15)==MMS_OK&&plugins::EndAccessoryHudGesture(*f.app,changed)==MMS_OK&&!changed&&
        f.history.RedoCount()==1&&AccessoryHudSame(f.accessory,before),"empty accessory gesture preserves the existing common redo branch");
}
void AccessoryHudIdentityAndAdmission(){
    for(unsigned reason=0;reason<12;++reason){AccessoryHudFixture f;f.accessory.parentModel=1;plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,15)==MMS_OK,"begin parented accessory HUD before an evaluation/identity change");AccessoryHudSample(*f.app,15,3);
        auto* originalPool=f.app->AccessoryKeys(7);mdl::AccessoryKey* replacement=nullptr;auto* parent=f.app->ModelSlot(1);auto* bones=f.second.m->boneTable;const auto count=f.second.m->boneCount;
        switch(reason){case 0:++f.app->CurrentFrame();break;case 1:f.accessory.parentBone=1;break;
        case 2:plugins::ForgetSceneObject(&f.accessory);plugins::SceneObjectIdentity(&f.accessory);break;
        case 3:plugins::ForgetSceneObject(parent);plugins::SceneObjectIdentity(parent);break;
        case 4:f.app->ModelSlot(1)=f.first.raw();break;case 5:f.second.m->boneTable=bones+1;break;case 6:--f.second.m->boneCount;break;
        case 7:replacement=mdl::KeyPoolCreate<mdl::AccessoryKey>(4);std::memcpy(replacement,originalPool,4*sizeof(*replacement));f.app->AccessoryKeys(7)=replacement;break;
        case 8:f.app->AccessorySlot(7)=nullptr;break;case 9:f.app->AccessorySlot(7)=&f.other;break;
        case 10:{const auto id=plugins::SceneObjectIdentity(&f.accessory).id;
            auto gone=plugins::PrepareObjectPresence(&f.accessory,id,false);gone->Publish();
            auto restored=plugins::PrepareObjectPresence(&f.accessory,id,true);restored->Publish();break;}
        case 11:f.accessory.parentModel=-1;break;}
        const auto foreign=f.accessory;
        Check(!plugins::AllowAccessoryHudPreview(*f.app,15)&&!plugins::AccessoryHudGestureActive(*f.app)&&AccessoryHudSame(f.accessory,foreign)&&!f.history.UndoCount(),
            "changed frame, attachment, stable identity, model/bone table/count, pool or membership never writes over the new accessory evaluation domain");
        f.app->AccessorySlot(7)=&f.accessory;f.app->ModelSlot(1)=parent;f.second.m->boneTable=bones;f.second.m->boneCount=count;
        if(replacement){f.app->AccessoryKeys(7)=originalPool;mdl::KeyPoolDestroy(replacement);}
    }
    AccessoryHudFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=f.accessory;
    AccessoryHudSample(*f.app,15,3);AccessoryHudSample(*f.app,18,3);
    Check(AccessoryHudSame(f.accessory,before)&&!f.history.UndoCount(),"bound accessory frame-mode samples require an owned gesture and never fall through to old untracked writers");
    for(int operation:{1,2,9,12,14,21,22,26})Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,operation)==MMS_E_NOT_READY&&AccessoryHudSame(f.accessory,before),"unmigrated accessory and model HUD families remain closed");
    f.app->state.coordinateSystem=1;Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,15)==MMS_E_NOT_READY,"camera coordinate target cannot enter accessory HUD history");f.app->state.coordinateSystem=2;
    f.app->SelectedAccessorySlot()=255;Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,15)==MMS_E_NOT_READY,"out-of-range selected accessory is refused without slot dereference");f.app->SelectedAccessorySlot()=7;
    f.app->SelectedAccessorySlot()=9;Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,15)==MMS_E_NOT_READY,"missing selected accessory is refused while other real accessory coordinate targets exist");f.app->SelectedAccessorySlot()=7;
    f.accessory.parentBone=-1;f.accessory.parentModel=1;Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,15)==MMS_E_ARGUMENT,"invalid accessory parent bone is refused before capture or authoring");f.accessory=before;
    f.app->PlaybackActive()=1;Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,15)==MMS_E_BUSY,"playback cannot begin accessory HUD authoring");f.app->PlaybackActive()=0;
    Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,18)==MMS_OK,"begin accessory HUD before a nested binding");AccessoryHudSample(*f.app,18,3);
    {NumpadFixture other;plugins::NativePoseTestBinding nested(*other.app,other.history);
        Check(!plugins::AccessoryHudGestureActive(*f.app)&&AccessoryHudSame(f.accessory,before),"rebind cancels accessory HUD while the old app/object/journal are still alive");}
    Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,18)==MMS_OK,"begin accessory HUD before reset/shutdown cancellation chain");AccessoryHudSample(*f.app,18,3);
    Check(plugins::CancelViewportCameraGesture(*f.app)&&!plugins::AccessoryHudGestureActive(*f.app)&&AccessoryHudSame(f.accessory,before),"existing reset and shutdown cancellation chain includes accessory HUD");
    Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,nullptr,18)==MMS_OK,"begin accessory HUD before pending journal reset");AccessoryHudSample(*f.app,18,3);
    Check(f.history.Reset()==MMS_OK,"reset pending accessory HUD journal");bool changed=true;
    Check(plugins::EndAccessoryHudGesture(*f.app,changed)==MMS_E_STALE&&!changed&&AccessoryHudSame(f.accessory,before),"reset's stale accessory HUD token rolls back its owned preview with no partial record");
    for(unsigned reason=0;reason<4;++reason){AccessoryHudFixture replay;replay.accessory.parentModel=1;plugins::NativePoseTestBinding replayBinding(*replay.app,replay.history);
        Check(plugins::BeginAccessoryHudGesture(*replay.app,replay.history,nullptr,18)==MMS_OK,"begin parented accessory replay identity test");AccessoryHudSample(*replay.app,18,3);
        Check(plugins::EndAccessoryHudGesture(*replay.app,changed)==MMS_OK&&changed,"commit parented accessory HUD before replay identity change");
        if(reason==0){plugins::ForgetSceneObject(&replay.accessory);plugins::SceneObjectIdentity(&replay.accessory);}
        if(reason==1){plugins::ForgetSceneObject(replay.second.raw());plugins::SceneObjectIdentity(replay.second.raw());}
        if(reason==2)replay.accessory.parentBone=1;if(reason==3)replay.app->AccessorySlot(7)=&replay.other;
        const auto current=replay.accessory;Check(replay.history.History(false)==MMS_E_STALE&&AccessoryHudSame(replay.accessory,current)&&replay.history.UndoCount()==1,
            "published accessory HUD cannot replay against another object, recycled parent identity or different attachment");replay.app->AccessorySlot(7)=&replay.accessory;
    }
}
struct AccessoryHudWindow{
    AccessoryHudFixture& fixture;WNDPROC previous;HWND floating;RECT bounds{},originalView{};POINT cursor{};bool cursorValid;viewport_tool_layout::Corner corner;
    explicit AccessoryHudWindow(AccessoryHudFixture& f):fixture(f),previous(nullptr),floating(f.app->FloatingWindow()),originalView(f.app->ViewportRect()),cursorValid(GetCursorPos(&cursor)!=FALSE),corner(viewport_corner_settings::Current()){
        const auto owner=f.app->MainWindow();GetWindowRect(owner,&bounds);SetWindowPos(owner,nullptr,0,0,800,600,SWP_NOZORDER|SWP_NOACTIVATE);
        previous=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(owner,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(MicWndProc)));
        Check(previous!=nullptr,"window fixture installs the real detached viewport procedure");f.app->FloatingWindow()=owner;HudFixtureGeometry(*f.app);f.app->ViewportRect()=FrameOverlayRect(f.app.get());
        viewport_corner_settings::ResetCacheForTest(viewport_tool_layout::Corner::BottomRight);
    }
    ~AccessoryHudWindow(){plugins::CancelAccessoryHudGesture(*fixture.app);if(GetCapture()==fixture.app->MainWindow())ReleaseCapture();
        SetWindowLongPtrW(fixture.app->MainWindow(),GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(previous));fixture.app->FloatingWindow()=floating;fixture.app->ViewportRect()=originalView;
        SetWindowPos(fixture.app->MainWindow(),nullptr,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOZORDER|SWP_NOACTIVATE);
        viewport_corner_settings::ResetCacheForTest(corner);if(cursorValid)SetCursorPos(cursor.x,cursor.y);}
    POINT Hit(int operation){auto& app=*fixture.app;const auto hud=viewport_tool_layout::Make(FrameOverlayRect(&app),FrameOverlayScale(&app),viewport_corner_settings::Current());
        const float columns[]={110.f,70.f,30.f};POINT point{LONG(hud.x-columns[(operation-15)%3]*hud.scale),LONG(hud.y-(operation<18?65.f:105.f)*hud.scale)};
        app.MouseX()=point.x;app.MouseY()=point.y;Check(ViewportToolAtPointForTest(&app)==operation,"real accessory HUD window hits the displayed native axis");return point;}
    void Cursor(POINT point){HudFixtureCursor(*fixture.app,fixture.app->MainWindow(),point,
        "place real OS cursor for accessory HUD release fixture",
        "real accessory HUD cursor converts through its actual window/client coordinates");}
    void DispatchUp(){MSG message{};Check(PeekMessageW(&message,fixture.app->MainWindow(),WM_LBUTTONUP,WM_LBUTTONUP,PM_REMOVE),"retrieve real queued accessory HUD release");DispatchMessageW(&message);}
};
void AccessoryHudQueuedRelease(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(int operation=15;operation<=20;++operation){AccessoryHudFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);AccessoryHudWindow window(f);
        f.history.SetMode(mode);const auto before=f.accessory;const auto owner=f.app->MainWindow();const auto press=window.Hit(operation);window.Cursor(press);
        SendMessageW(owner,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(press.x,press.y));
        Check(plugins::AccessoryHudGestureActive(*f.app)&&GetCapture()==owner&&f.app->MouseX()==f.app->PreviousMouseX()&&f.app->MouseY()==f.app->PreviousMouseY(),"real accessory HUD press reserves and centers both native mouse snapshots");
        Check(PostMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(press.x,press.y)),"queue unchanged pre-warp accessory release coordinates");window.DispatchUp();
        Check(AccessoryHudSame(f.accessory,before)&&!f.history.UndoCount()&&!plugins::AccessoryHudGestureActive(*f.app)&&GetCapture()!=owner,"queued same-position accessory click is empty despite center warp and stale lParam");
        f.app->LeftMouseButtonState()=0;const auto next=window.Hit(operation);window.Cursor(next);SendMessageW(owner,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(next.x,next.y));
        POINT release{};Check(GetCursorPos(&release)&&ScreenToClient(owner,&release),"sample actual centered accessory cursor before final movement");release.y+=7;window.Cursor(release);
        Check(PostMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(next.x,next.y)),"queue accessory release carrying old button coordinates after real cursor movement");window.DispatchUp();
        auto expected=before;const auto field=unsigned(operation-15);const double scale=operation<18?0.05000000074505806:0.019999999552965164;
        AccessoryHudSet(expected,field,float(double(AccessoryHudRead(before)[field])+(operation<18?-7.0:7.0)*scale));
        Check(AccessoryHudSame(f.accessory,expected)&&!plugins::AccessoryHudGestureActive(*f.app)&&GetCapture()!=owner&&f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u),
            "real detached accessory release consumes live seven-pixel OS delta once and preserves native subtraction/rotation math");
        if(mode==Mode::AllOperations){Check(f.history.History(false)==MMS_OK&&AccessoryHudSame(f.accessory,before),"one accessory HUD undo includes final coalesced native release sample");}
        Check(f.history.Reset()==MMS_OK,"clear queued accessory release history");f.history.Collect();f.accessory=before;
        f.app->LeftMouseButtonState()=0;const auto stolen=window.Hit(operation);window.Cursor(stolen);SendMessageW(owner,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(stolen.x,stolen.y));AccessoryHudSample(*f.app,operation,3);
        const auto other=CreateWindowExW(0,L"STATIC",L"Accessory HUD capture replacement",WS_OVERLAPPED,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(other&&PostMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(stolen.x,stolen.y)),"queue old accessory mouse-up before a real capture takeover");SetCapture(other);
        Check(!plugins::AccessoryHudGestureActive(*f.app)&&AccessoryHudSame(f.accessory,before)&&GetCapture()==other,"real accessory capture takeover cancels preview and preserves the new owner");window.DispatchUp();
        Check(GetCapture()==other&&AccessoryHudSame(f.accessory,before)&&!f.history.UndoCount(),"late old-viewport accessory mouse-up cannot release the replacement capture or republish cancelled preview");ReleaseCapture();DestroyWindow(other);
    }
}
struct AccessoryHudReentrantEcho{
    inline static thread_local AccessoryHudReentrantEcho* active=nullptr;
    AccessoryHudFixture& fixture;HWND owner,edit,other,floating;WNDPROC ownerProc,editProc;RECT originalView{};
    bool armed=false,observed=false,restart=false;unsigned field,foreign=0;mdl::AccessoryRecord native{},foreignPose{},original{},replacement{};
    int replacementOperation=0;std::shared_ptr<const plugins::HistoryChange> replacementSource;
    explicit AccessoryHudReentrantEcho(AccessoryHudFixture& f,int operation):fixture(f),owner(f.app->MainWindow()),
        edit(GetDlgItem(owner,478+operation-15)),other(nullptr),floating(f.app->FloatingWindow()),ownerProc(nullptr),editProc(nullptr),originalView(f.app->ViewportRect()),field(unsigned(operation-15)){
        Check(!active&&edit,"accessory echo cancellation fixture owns the actual native axis EDIT");active=this;
        ownerProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(owner,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(MicWndProc)));
        Check(ownerProc!=nullptr,"accessory echo cancellation installs the actual detached viewport procedure");f.app->FloatingWindow()=owner;HudFixtureGeometry(*f.app);
        other=CreateWindowExW(0,L"STATIC",L"accessory echo capture replacement",WS_OVERLAPPED,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        editProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(EditProc)));
        Check(other&&editProc,"create foreign capture and subclass WM_SETTEXT while forwarding the native EDIT procedure");
    }
    ~AccessoryHudReentrantEcho(){armed=false;plugins::CancelAccessoryHudGesture(*fixture.app);
        if(GetCapture()==owner||GetCapture()==other)ReleaseCapture();
        SetWindowLongPtrW(edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(editProc));DestroyWindow(other);
        SetWindowLongPtrW(owner,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ownerProc));fixture.app->FloatingWindow()=floating;fixture.app->ViewportRect()=originalView;active=nullptr;}
    static LRESULT CALLBACK EditProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        auto* self=active;const auto original=self->editProc;
        if(message==WM_SETTEXT&&self->armed){self->armed=false;self->observed=true;
            Check(AccessoryHudSame(self->fixture.accessory,self->native),"real EDIT WM_SETTEXT observes the complete actual native accessory axis store before capture transfer");
            if(self->foreign==1)AccessoryHudSet(self->fixture.accessory,self->field,AccessoryHudRead(self->fixture.accessory)[self->field]+9.f);
            if(self->foreign==2){const unsigned independent=(self->field+1)%6;AccessoryHudSet(self->fixture.accessory,independent,AccessoryHudRead(self->fixture.accessory)[independent]+9.f);}
            if(self->foreign==3)self->fixture.app->CameraFov()+=3.f;
            self->foreignPose=self->fixture.accessory;SetCapture(self->other);
            if(self->restart){
                Check(!plugins::AccessoryHudGestureActive(*self->fixture.app)&&AccessoryHudSame(self->fixture.accessory,self->original),
                    "actual accessory EDIT capture callback cancels and restores the old release before same-owner replacement");ReleaseCapture();
                Check(plugins::BeginAccessoryHudGesture(*self->fixture.app,self->fixture.history,self->owner,self->replacementOperation)==MMS_OK,
                    "actual accessory EDIT callback starts an independent same-owner HUD reservation");SetCapture(self->owner);
                AccessoryHudSample(*self->fixture.app,self->replacementOperation,3);self->replacement=self->fixture.accessory;
                self->replacementSource=plugins::AccessoryHudPreviewSource(*self->fixture.app);
                self->fixture.app->MouseX()=333;self->fixture.app->MouseY()=444;self->fixture.app->PreviousMouseX()=300;self->fixture.app->PreviousMouseY()=400;
                self->fixture.app->ViewToolDragOperation()=static_cast<ViewportToolAction>(self->replacementOperation);
                self->fixture.app->InteractionDragMode()=static_cast<ViewportDragMode>(self->replacementOperation-2);
                self->fixture.app->LeftMouseButtonState()=3;self->fixture.app->state.a06B5=1;
            }
        }
        return CallWindowProcA(original,hwnd,message,wp,lp);
    }
};
void AccessoryHudReentrantCancellation(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(int operation=15;operation<=20;++operation)
    for(unsigned foreign=0;foreign<4;++foreign)for(bool prior:{false,true}){
        AccessoryHudFixture f;AccessoryHudReentrantEcho window(f,operation);bool changed=false;
        Check(plugins::AdjustViewportCameraWheel(*f.app,f.history,120,true,changed)==MMS_OK&&changed&&f.history.History(false)==MMS_OK,
            "seed an independent real redo branch before accessory EDIT reentrancy");
        Check(f.history.SetMode(mode)==MMS_OK,"configure accessory EDIT cancellation recording policy");plugins::NativePoseTestBinding binding(*f.app,f.history);
        const auto before=f.accessory;const auto camera=NumpadRead(*f.app);const auto first=f.first.Digest(),second=f.second.Digest();
        const auto cameraKeys=GlobalRegistrationKeys(f.app->CameraKeys());
        const auto keys=GlobalRegistrationKeys(f.app->AccessoryKeys(7)),otherKeys=GlobalRegistrationKeys(f.app->AccessoryKeys(8));
        const auto other=f.other;const auto bytes=f.history.RetainedBytes();std::uint64_t revision=0;f.history.Revision(revision);
        Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,window.owner,operation)==MMS_OK,"begin a real accessory gesture before native EDIT synchronous cancellation");SetCapture(window.owner);
        if(prior)AccessoryHudSample(*f.app,operation,3);
        window.native=f.accessory;const unsigned field=unsigned(operation-15);const double scale=operation<18?-0.05000000074505806:0.019999999552965164;
        AccessoryHudSet(window.native,field,float(double(AccessoryHudRead(window.native)[field])+7.0*scale));window.foreign=foreign;window.armed=true;
        failAfter=0;AccessoryHudSample(*f.app,operation,7);failAfter=-1;
        auto expected=before;if(foreign==1)AccessoryHudSet(expected,field,AccessoryHudRead(window.foreignPose)[field]);
        if(foreign==2){const auto independent=(field+1)%6;AccessoryHudSet(expected,independent,AccessoryHudRead(window.foreignPose)[independent]);}
        auto expectedCamera=camera;if(foreign==3)expectedCamera.values[9]+=3.f;
        std::uint64_t afterRevision=0;f.history.Revision(afterRevision);
        Check(window.observed&&!plugins::AccessoryHudGestureActive(*f.app)&&GetCapture()==window.other&&AccessoryHudSame(f.accessory,expected)&&
            AccessoryHudSame(f.other,other)&&NumpadSame(NumpadRead(*f.app),expectedCamera)&&!f.history.UndoCount()&&f.history.RedoCount()==1&&
            f.history.RetainedBytes()==bytes&&afterRevision==revision&&f.first.Digest()==first&&f.second.Digest()==second&&
            GlobalRegistrationSame(f.app->CameraKeys(),cameraKeys)&&GlobalRegistrationSame(f.app->AccessoryKeys(7),keys)&&GlobalRegistrationSame(f.app->AccessoryKeys(8),otherKeys),
            "EDIT WM_SETTEXT cancellation restores the current and prior owned accessory samples, preserves same/other-axis and camera authors, foreign capture, redo, budget, models and keys");
        ReleaseCapture();
    }
}
void AccessoryHudReleaseReentrantReplacement(){
    using Mode=plugins::SharedHistory::RecordingMode;
    struct CursorRestore{POINT point{};bool valid=GetCursorPos(&point)!=FALSE;~CursorRestore(){if(valid)SetCursorPos(point.x,point.y);}} cursorRestore;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(int operation:{15,18}){
        AccessoryHudFixture f;AccessoryHudReentrantEcho window(f,operation);bool changed=false;
        Check(plugins::AdjustViewportCameraWheel(*f.app,f.history,120,true,changed)==MMS_OK&&changed&&f.history.History(false)==MMS_OK,
            "seed redo before a same-owner accessory release replacement");
        Check(f.history.SetMode(mode)==MMS_OK,"configure same-owner accessory release recording policy");plugins::NativePoseTestBinding binding(*f.app,f.history);
        window.original=f.accessory;const auto camera=NumpadRead(*f.app);const auto other=f.other;const auto first=f.first.Digest(),second=f.second.Digest();
        const auto keys=GlobalRegistrationKeys(f.app->AccessoryKeys(7)),otherKeys=GlobalRegistrationKeys(f.app->AccessoryKeys(8));const auto cameras=GlobalRegistrationKeys(f.app->CameraKeys());
        const auto bytes=f.history.RetainedBytes();std::uint64_t revision=0;f.history.Revision(revision);
        Check(plugins::BeginAccessoryHudGesture(*f.app,f.history,window.owner,operation)==MMS_OK,"reserve old accessory HUD before real release EDIT notification");SetCapture(window.owner);
        const auto oldSource=plugins::AccessoryHudPreviewSource(*f.app);f.app->PreviousMouseX()=f.app->MouseX()=100;f.app->PreviousMouseY()=f.app->MouseY()=100;
        HudFixtureCursor(*f.app,window.owner,{100,107},"place actual seven-pixel accessory release cursor",
            "accessory release replacement cursor round-trips through actual screen and client coordinates");
        window.native=f.accessory;const unsigned field=unsigned(operation-15);const double scale=operation<18?-0.05000000074505806:0.019999999552965164;
        AccessoryHudSet(window.native,field,float(double(AccessoryHudRead(window.native)[field])+7.0*scale));
        window.restart=true;window.replacementOperation=operation==15?18:15;window.armed=true;
        // Before the handler's source guard, EchoEdit correctly returned after
        // replacement, but this old release still copied PreviousMouse and
        // End'ed the new token, then MicWndProc cleared its same-owner capture.
        SendMessageW(window.owner,WM_LBUTTONUP,0,MAKELPARAM(1,1));std::uint64_t afterRevision=0;f.history.Revision(afterRevision);
        Check(window.observed&&window.replacementSource&&window.replacementSource!=oldSource&&
            plugins::AccessoryHudPreviewSource(*f.app)==window.replacementSource&&GetCapture()==window.owner&&
            f.app->MouseX()==333&&f.app->MouseY()==444&&f.app->PreviousMouseX()==300&&f.app->PreviousMouseY()==400&&
            f.app->ViewToolDragOperation()==static_cast<ViewportToolAction>(window.replacementOperation)&&
            f.app->InteractionDragMode()==static_cast<ViewportDragMode>(window.replacementOperation-2)&&f.app->LeftMouseButtonState()==3&&f.app->state.a06B5==1&&
            AccessoryHudSame(f.accessory,window.replacement)&&AccessoryHudSame(f.other,other)&&NumpadSame(NumpadRead(*f.app),camera)&&
            !f.history.UndoCount()&&f.history.RedoCount()==1&&f.history.RetainedBytes()==bytes&&afterRevision==revision&&f.history.SetMode(mode)==MMS_E_BUSY&&
            f.first.Digest()==first&&f.second.Digest()==second&&GlobalRegistrationSame(f.app->AccessoryKeys(7),keys)&&
            GlobalRegistrationSame(f.app->AccessoryKeys(8),otherKeys)&&GlobalRegistrationSame(f.app->CameraKeys(),cameras),
            "old accessory mouse-up is swallowed without consuming replacement source/token, overwriting its mouse snapshots, clearing its tools/capture or changing redo");
        Check(plugins::EndAccessoryHudGesture(*f.app,changed)==MMS_OK&&changed&&f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u),
            "preserved accessory replacement token still commits exactly its own complete native sample");ReleaseCapture();
        if(mode==Mode::AllOperations)Check(f.history.History(false)==MMS_OK&&AccessoryHudSame(f.accessory,window.original),
            "replacement accessory undo proves the old release did not finalize or discard the new reservation");
    }
}
}
void NativeAccessoryHudHistoryChecks(){AccessoryHudMathAndHistory();AccessoryHudCancellationAndFailure();AccessoryHudIdentityAndAdmission();AccessoryHudQueuedRelease();AccessoryHudReentrantCancellation();AccessoryHudReleaseReentrantReplacement();}
