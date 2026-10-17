#include "mikudancestudio/clipboard_layout.hpp"
namespace global_paste_host_checks {
template<class K> std::vector<K> Capture(K* pool){return {pool,pool+mdl::KeyPoolCapacity(pool)};}
template<class K> bool Same(K* pool,const std::vector<K>& saved){return mdl::KeyPoolCapacity(pool)>=saved.size()&&!std::memcmp(pool,saved.data(),saved.size()*sizeof(K));}
struct Keys {
    std::vector<mdl::CameraKey> camera;std::vector<mdl::LightKey> light;std::vector<mdl::SelfShadowKey> shadow;
    std::vector<mdl::GravityKey> gravity;std::array<std::vector<mdl::AccessoryKey>,255> accessories;
    explicit Keys(MMDApp& a):camera(Capture(a.CameraKeys())),light(Capture(a.LightKeys())),shadow(Capture(a.ShadowKeys())),gravity(Capture(a.GravityKeys())){
        for(unsigned i=0;i<255;++i)accessories[i]=Capture(a.AccessoryKeys(i));
    }
    bool Equals(MMDApp& a)const {
        if(!Same(a.CameraKeys(),camera)||!Same(a.LightKeys(),light)||!Same(a.ShadowKeys(),shadow)||!Same(a.GravityKeys(),gravity))return false;
        for(unsigned i=0;i<255;++i)if(!Same(a.AccessoryKeys(i),accessories[i]))return false;return true;
    }
};
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()) {
    using edit_history_host_checks::Menu;
    Check(app->ModelSlot(0)&&app->AccessorySlot(0)&&!history.UndoCount(),"real global paste fixture has PMX or PMD plus an accessory and empty history");
    const auto mode=history.Mode();const auto frame=app->CurrentFrame();const auto slot=app->SelectedModelSlot();const auto cameraMode=app->CameraMode();
    const auto selectedAccessory=app->SelectedAccessorySlot();const auto counts=app->ClipboardCounts();const auto last=app->LastRegisteredFrame();
    auto* oldCamera=app->CameraClipboard();auto* oldLight=app->LightClipboard();auto* oldShadow=app->ShadowClipboard();
    auto* oldGravity=app->GravityClipboard();auto* oldAccessory=app->AccessoryClipboard();
    const auto initial=value();
    std::array<mdl::CameraClipboardRecord,3> cameras{};std::array<mdl::LightClipboardRecord,3> lights{};
    std::array<mdl::ShadowClipboardRecord,3> shadows{};std::array<mdl::GravityClipboardRecord,3> gravity{};
    std::array<mdl::AccessoryClipboardKey,3> accessories{};
    for(unsigned i=0;i<3;++i) {
        cameras[i].frame=i==1?6:0;cameras[i].fov=53+int(i);cameras[i].distance=-33-float(i);cameras[i].parentModel=-1;cameras[i].perspective=1;
        cameras[i].eye[1]=10+float(i);std::memset(cameras[i].interpolation,20,12);std::memset(cameras[i].interpolation+12,107,12);cameras[i].interpolation[5]=std::uint8_t(25+i);
        lights[i].frame=cameras[i].frame;lights[i].direction[1]=-1;lights[i].color[0]=.23f+float(i)*.1f;lights[i].color[1]=lights[i].color[2]=.6f;
        shadows[i].frame=cameras[i].frame;shadows[i].mode=std::uint8_t(i);shadows[i].distance=.03f+.01f*float(i);
        gravity[i].frame=cameras[i].frame;gravity[i].acceleration=9.8f+float(i);gravity[i].direction[1]=-1;gravity[i].noise=3+int(i);gravity[i].noiseEnabled=1;
        accessories[i].frameOffset=cameras[i].frame;accessories[i].slot=0;accessories[i].parentModel=-1;accessories[i].visible=accessories[i].shadowEnabled=1;
        accessories[i].position[0]=3+float(i);accessories[i].scale=1.1f+float(i)*.1f;accessories[i].opacity=.5f+float(i)*.1f;
    }
    const auto bind=[&](bool five){app->CameraClipboard()=cameras.data();app->LightClipboard()=lights.data();app->ShadowClipboard()=shadows.data();
        app->GravityClipboard()=five?gravity.data():nullptr;app->AccessoryClipboard()=five?accessories.data():nullptr;app->ClipboardCounts()={};
        auto& n=app->ClipboardCounts();n.cameras=n.lights=n.shadows=3;if(five)n.gravity=n.accessories=3;};
    const auto clear=[&]{app->CameraClipboard()=nullptr;app->LightClipboard()=nullptr;app->ShadowClipboard()=nullptr;
        app->GravityClipboard()=nullptr;app->AccessoryClipboard()=nullptr;app->ClipboardCounts()={};};
    for(bool five:{false,true})for(auto recording:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        Check(history.SetMode(recording)==MMS_OK,"real global paste recording mode configured");
        bind(five);app->CameraMode()=1;app->CurrentFrame()=349;app->SelectedModelSlot()=0;
        const Keys before(*app);const auto beforeLast=app->LastRegisteredFrame();
        Menu(app,L"Edit probe prepared");SendMessageW(app->MainWindow(),WM_COMMAND,421,0);
        Check(!history.UndoCount()&&before.Equals(*app)&&app->LastRegisteredFrame()==beforeLast,"pending plugin rejects actual mixed global paste atomically");Menu(app,L"Edit probe cancel");
        Menu(app,L"Edit probe once");SendMessageW(app->MainWindow(),WM_COMMAND,421,0);
        Check(history.UndoCount()==2&&value()==initial+1&&app->CurrentFrame()==349,"actual command 421 mixed global paste enters one explicit node");
        unsigned at=0;while(app->CameraKeys()[at].frame<349&&app->CameraKeys()[at].next)at=app->CameraKeys()[at].next;
        Check(app->CameraKeys()[at].frame==349&&app->CameraKeys()[at].fov==55&&app->CameraKeys()[at].interpolation[0][5]==27,"real global repeated source keeps final camera record and full interpolation");
        const Keys after(*app);SendMessageW(app->MainWindow(),WM_COMMAND,421,0);
        Check(history.UndoCount()==2&&after.Equals(*app),"actual identical global paste adds no history");
        clear();Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(before.Equals(*app)&&value()==initial+1&&app->LastRegisteredFrame()==beforeLast,"actual mixed paste undo restores keys selection and metadata between plugin edits");
        app->CurrentFrame()=29;app->CameraMode()=0;app->SelectedModelSlot()=1;app->SelectedAccessorySlot()=1;Menu(app,L"Edit probe redo");
        Check(after.Equals(*app)&&app->CurrentFrame()==29,"actual global redo owns clipboard bytes across time and selection changes");
        Menu(app,L"Edit probe redo");for(unsigned i=0;i<3;++i)Menu(app,L"Edit probe undo");
        Check(value()==initial&&history.Reset()==MMS_OK,"actual global paste restores plugin history baseline");history.Collect();
    }
    app->CameraClipboard()=oldCamera;app->LightClipboard()=oldLight;app->ShadowClipboard()=oldShadow;app->GravityClipboard()=oldGravity;app->AccessoryClipboard()=oldAccessory;
    app->ClipboardCounts()=counts;app->CurrentFrame()=frame;app->SelectedModelSlot()=slot;app->CameraMode()=cameraMode;app->SelectedAccessorySlot()=selectedAccessory;
    app->LastRegisteredFrame()=last;Check(history.SetMode(mode)==MMS_OK,"restore real global paste history mode");
    app->SceneModified()=0;
    std::puts("PASS actual command 421 three-family and five-family global paste, clipboard ownership and interleaved plugin history in both modes");
}
}
