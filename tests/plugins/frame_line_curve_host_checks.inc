// Include inside edit_history_host_checks after the common menu helper.
namespace frame_line_curve_host_checks {
using Channels=std::vector<curve_editor::Channel>;
using Bytes=std::vector<std::uint8_t>;
curve_host::Kind Kind(unsigned family){return family?curve_host::Kind::Light:curve_host::Kind::Camera;}
Channels Read(MMDApp& app,unsigned family){Channels channels;std::wstring error;
    Check(curve_host::Read(app,Kind(family),0,channels,error),"read real frame-line free authoring");return channels;}
Bytes Encode(const Channels& channels){std::vector<animation_curve::NamedTrack> tracks;
    for(const auto& channel:channels)tracks.push_back({channel.id,channel.track});Bytes bytes;std::string error;
    Check(animation_curve::Encode(tracks,bytes,error),"encode exact real retimed free curves");return bytes;}
const animation_curve::Key* At(const animation_curve::Track& track,int frame){const auto found=std::find_if(track.keys.begin(),track.keys.end(),[&](const auto& key){return key.frame==frame;});
    return found==track.keys.end()?nullptr:&*found;}
std::array<float,14> Pose(MMDApp& app){return {app.CameraPositionX(),app.CameraPositionY(),app.CameraPositionZ(),app.CameraPitch(),app.CameraYaw(),app.CameraRoll(),
    app.CameraDistance(),app.CameraFov(),app.LightDirection()[0],app.LightDirection()[1],app.LightDirection()[2],app.LightColor()[0],app.LightColor()[1],app.LightColor()[2]};}
struct Original {
    MMDApp& app;plugins::SharedHistory& history;mdl::CameraKey* camera;mdl::LightKey* light;
    const std::uint64_t cameraIdentity,lightIdentity;curve_host::AuthoringTable authoring,verification;
    const std::array<float,14> pose;D3DLIGHT9 sceneLight;
    const int frame,last,parentModel,parentBone,cameraCursor,lightCursor,cameraActive,lightActive,physicsReset,modified,recorded,messageSeen;
    const std::uint8_t cameraMode,modelSlot,perspective;const bool projectionPending;const plugins::SharedHistory::RecordingMode mode;
    Original(MMDApp& a,plugins::SharedHistory& h):app(a),history(h),camera(a.CameraKeys()),light(a.LightKeys()),
        cameraIdentity(mdl::KeyPoolIdentity(camera)),lightIdentity(mdl::KeyPoolIdentity(light)),pose(Pose(a)),sceneLight(a.SceneLight()),
        frame(a.CurrentFrame()),last(a.LastRegisteredFrame()),parentModel(a.CameraParentModel()),parentBone(a.CameraParentBone()),
        cameraCursor(a.CameraTrackCursor()),lightCursor(a.LightTrackCursor()),cameraActive(a.CameraTrackActive()),lightActive(a.LightTrackActive()),
        physicsReset(a.PhysicsResetPending()),modified(a.SceneModified()),recorded(a.RecordedFrameCountByte1()),messageSeen(a.MessageSeen()),
        cameraMode(a.CameraMode()),modelSlot(a.SelectedModelSlot()),perspective(a.CameraPerspective()),projectionPending(a.cameraProjectionRefreshPending),mode(h.Mode()){
        std::wstring error;Check(curve_host::CaptureAuthoring(app,verification,error),"capture original real authoring before isolated frame-line fixture");curve_host::SwapAuthoring(app,authoring);
        app.CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(8);app.LightKeys()=mdl::KeyPoolCreate<mdl::LightKey>(8);
    }
    ~Original(){
        Check(history.Reset()==MMS_OK,"release retimed host records before restoring original global pools");history.Collect();
        curve_host::AuthoringTable discarded;curve_host::SwapAuthoring(app,discarded);
        auto* tempCamera=app.CameraKeys();auto* tempLight=app.LightKeys();app.CameraKeys()=camera;app.LightKeys()=light;
        mdl::KeyPoolDestroy(tempCamera);mdl::KeyPoolDestroy(tempLight);curve_host::SwapAuthoring(app,authoring);
        for(unsigned axis=0;axis<3;++axis){app.CameraPositionAxis(axis)=pose[axis];app.CameraRotation()[axis]=pose[axis+3];app.LightDirection()[axis]=pose[axis+8];app.LightColor()[axis]=pose[axis+11];}
        app.CameraDistance()=pose[6];app.CameraFov()=pose[7];app.SceneLight()=sceneLight;app.CameraParentModel()=parentModel;app.CameraParentBone()=parentBone;
        app.CurrentFrame()=frame;app.LastRegisteredFrame()=last;app.CameraTrackCursor()=cameraCursor;app.LightTrackCursor()=lightCursor;app.CameraTrackActive()=cameraActive;app.LightTrackActive()=lightActive;
        app.PhysicsResetPending()=physicsReset;app.SceneModified()=modified;app.RecordedFrameCountByte1()=recorded;app.MessageSeen()=messageSeen;
        app.CameraMode()=cameraMode;app.SelectedModelSlot()=modelSlot;app.CameraPerspective()=perspective;app.cameraProjectionRefreshPending=projectionPending;
        Check(history.SetMode(mode)==MMS_OK&&mdl::KeyPoolIdentity(camera)==cameraIdentity&&mdl::KeyPoolIdentity(light)==lightIdentity&&curve_host::AuthoringUnchanged(app,verification),
            "real frame-line fixture restores original global pool identities, authoring, recording mode and poses");
    }
};
void Build(MMDApp& app,unsigned family,bool collision){
    curve_host::AuthoringTable old;curve_host::SwapAuthoring(app,old);
    std::memset(app.CameraKeys(),0,mdl::KeyPoolCapacity(app.CameraKeys())*sizeof(mdl::CameraKey));
    std::memset(app.LightKeys(),0,mdl::KeyPoolCapacity(app.LightKeys())*sizeof(mdl::LightKey));
    for(unsigned i=0;i<3;++i){auto& camera=app.CameraKeys()[i];auto& light=app.LightKeys()[i];
        camera.frame=light.frame=i==0?0:i==1?(collision?1:12):31;camera.previous=light.previous=i?i-1:0;camera.next=light.next=i<2?i+1:0;
        camera.parentModel=-1;camera.distance=-45;camera.fov=45;camera.eye[0]=float(i);camera.eye[1]=10;camera.target[1]=float(i)*.1f;
        std::memset(camera.interpolation,20,12);std::memset(&camera.interpolation[2][0],107,12);
        for(unsigned axis=0;axis<3;++axis){light.direction[axis]=float(i+axis)*.1f;light.color[axis]=.5f;}}
    auto channels=Read(app,family);auto& sparse=channels[0];sparse.track.keys.erase(sparse.track.keys.begin()+1);
    sparse.nativeControls.clear();sparse.controlGeometry.clear();sparse.sample={};
    sparse.track.keys.front().value=.123456789012;sparse.track.keys.back().value=3.987654321098;
    for(auto& key:sparse.track.keys){key.tangent=animation_curve::Tangent::Free;key.interpolation=animation_curve::Interpolation::Bezier;
        key.inSlope=-.017;key.outSlope=.013;key.inWeight=.31;key.outWeight=.19;}
    channels[0].selected=false;channels[0].collections={{"host-retime",L"host free retime"}};
    channels[1].track.keys.back().interpolation=animation_curve::Interpolation::Step;
    std::wstring error;Check(curve_host::Write(app,Kind(family),0,channels,error),"real host authors a sparse lane with custom Free handles before frame-line commands");
    app.CameraParentModel()=-1;app.CameraParentBone()=0;app.CameraPositionX()=-71.25f;app.LightDirection()[0]=-.21737f;
}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    using edit_history_host_checks::Menu;Check(app&&!history.UndoCount()&&!history.RedoCount(),"real free frame-line fixture starts with empty shared history");
    Original original(*app,history);const auto initial=value();
    for(unsigned family=0;family<2;++family)for(bool insert:{false,true})for(bool collision:{false,true})
        for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        Build(*app,family,collision);Check(history.SetMode(mode)==MMS_OK,"configure real explicit authored frame-line history mode");
        app->CameraMode()=1;app->SelectedModelSlot()=0;app->CurrentFrame()=collision?0:12;const int frame=app->CurrentFrame();
        const auto beforeChannels=Read(*app,family);const auto before=Encode(beforeChannels);const auto pose=Pose(*app);
        const std::vector<mdl::CameraKey> camera(app->CameraKeys(),app->CameraKeys()+mdl::KeyPoolCapacity(app->CameraKeys()));
        const std::vector<mdl::LightKey> light(app->LightKeys(),app->LightKeys()+mdl::KeyPoolCapacity(app->LightKeys()));
        const unsigned command=family?(insert?257:258):(insert?255:256);
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe prepared");SendMessageW(app->MainWindow(),WM_COMMAND,command,0);
        Check(history.UndoCount()==1&&Encode(Read(*app,family))==before&&!std::memcmp(app->CameraKeys(),camera.data(),camera.size()*sizeof(camera[0]))&&
            !std::memcmp(app->LightKeys(),light.data(),light.size()*sizeof(light[0])),"real pending plugin transaction rejects mapped frame-line changes atomically");
        Menu(app,L"Edit probe cancel");SendMessageW(app->MainWindow(),WM_COMMAND,command,0);
        const auto afterChannels=Read(*app,family);const auto after=Encode(afterChannels);const int end=31+(insert?1:-1);
        Check(after!=before&&afterChannels[0].track.keys.back().frame==end&&afterChannels[0].track.keys.back().value==beforeChannels[0].track.keys.back().value&&
            history.UndoCount()==2&&value()==initial+1&&Pose(*app)==pose&&app->CurrentFrame()==frame,
            "actual camera/light frame-line commands preserve exact free authoring and dirty poses in one plugin-interleaved step");
        if(!insert&&collision){const auto* root=At(afterChannels[0].track,0);
            Check(root&&root->value==animation_curve::Evaluate(beforeChannels[0].track,1).value,
                "actual delete-zero command retains the sparse lane's exact original frame-one sample");}
        else Check(afterChannels[0].track.keys.size()==beforeChannels[0].track.keys.size()&&afterChannels[0].track.keys.front().outWeight==.19,
            "actual frame-line keeps sparse lanes and normalized custom handle weights");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");app->CameraMode()=0;app->SelectedModelSlot()=1;app->CurrentFrame()=23;
        Menu(app,L"Edit probe undo");
        Check(Encode(Read(*app,family))==before&&!std::memcmp(app->CameraKeys(),camera.data(),camera.size()*sizeof(camera[0]))&&
            !std::memcmp(app->LightKeys(),light.data(),light.size()*sizeof(light[0]))&&value()==initial+1&&app->CurrentFrame()==23&&Pose(*app)==pose,
            "real mapped frame-line undo restores exact original native and sparse free keys at an unrelated UI target");
        Menu(app,L"Edit probe undo");Check(value()==initial,"earlier real plugin transaction remains separately undoable");
        Menu(app,L"Edit probe redo");Menu(app,L"Edit probe redo");
        Check(Encode(Read(*app,family))==after&&value()==initial+1&&app->CurrentFrame()==23&&Pose(*app)==pose,
            "real mapped redo restores retained authoring after the preceding plugin redo without moving the playhead");
        Menu(app,L"Edit probe redo");Check(value()==initial+2&&history.UndoCount()==3,"later real plugin transaction remains separately redoable");
        for(unsigned i=0;i<3;++i)Menu(app,L"Edit probe undo");Check(value()==initial&&history.Reset()==MMS_OK,"real mapped frame-line fixture returns to the plugin baseline");history.Collect();
    }
    std::puts("PASS actual camera/light Free frame-line retime, sparse zero collision, exact undo/redo and plugin interleave in both modes");
}
}
