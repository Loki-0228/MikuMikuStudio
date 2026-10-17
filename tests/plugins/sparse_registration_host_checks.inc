// Actual 452 command and DLL edit journal. Setup owns a temporary camera pool;
// it parks/restores the entire pre-existing authoring table without rewriting it.
namespace sparse_registration_host_checks {
using Bytes=std::vector<std::uint8_t>;
using Channels=std::vector<curve_editor::Channel>;
Channels Read(MMDApp& app){Channels channels;std::wstring error;
    Check(curve_host::Read(app,curve_host::Kind::Camera,0,channels,error),"real sparse camera authoring is readable");return channels;}
Bytes Encode(const Channels& channels){
    std::vector<animation_curve::NamedTrack> tracks;for(const auto& channel:channels)tracks.push_back({channel.id,channel.track});
    Bytes bytes;std::string error;Check(animation_curve::Encode(tracks,bytes,error),"encode exact real sparse camera authored keys and handles");return bytes;
}
bool Has(const curve_editor::Channel& channel,int frame){return std::any_of(channel.track.keys.begin(),channel.track.keys.end(),[&](const auto& key){return key.frame==frame;});}
struct Marks {
    struct Pool {void* pointer;std::uint64_t identity;std::size_t stride,offset;Bytes values;};
    std::vector<Pool> pools;std::array<std::uint8_t,255> rows{};
    template<class K>void Add(K* keys){
        Pool saved{keys,mdl::KeyPoolIdentity(keys),sizeof(K),offsetof(K,selected),{}};
        for(std::size_t i=0;i<mdl::KeyPoolCapacity(keys);++i)saved.values.push_back(keys[i].selected);pools.push_back(std::move(saved));
    }
    explicit Marks(MMDApp& app){
        Add(app.LightKeys());Add(app.ShadowKeys());Add(app.GravityKeys());
        for(unsigned slot=0;slot<255;++slot){Add(app.AccessoryKeys(slot));if(const auto* object=app.AccessorySlot(slot))rows[slot]=object->rowSelected;}
    }
    void Restore(MMDApp& app)const{
        for(const auto& pool:pools)for(std::size_t i=0;i<pool.values.size();++i)
            *(static_cast<unsigned char*>(pool.pointer)+i*pool.stride+pool.offset)=pool.values[i];
        for(unsigned slot=0;slot<255;++slot)if(auto* object=app.AccessorySlot(slot))object->rowSelected=rows[slot];
    }
};
struct Original {
    MMDApp& app;plugins::SharedHistory& history;mdl::CameraKey* camera;const std::uint64_t cameraIdentity;
    curve_host::AuthoringTable authoring,verification;Marks marks;
    std::array<float,8> pose;std::array<std::uint8_t,4> selectedTracks;
    const int frame,last,parentModel,parentBone,cursor,active,physicsReset,modified,recorded,messageSeen;
    const std::uint8_t cameraMode,modelSlot,accessorySlot,perspective;
    const bool projectionPending;const plugins::SharedHistory::RecordingMode mode;const LRESULT resetCheckbox;
    Original(MMDApp& a,plugins::SharedHistory& h):app(a),history(h),camera(a.CameraKeys()),cameraIdentity(mdl::KeyPoolIdentity(camera)),marks(a),
        pose{a.CameraPositionX(),a.CameraPositionY(),a.CameraPositionZ(),a.CameraPitch(),a.CameraYaw(),a.CameraRoll(),a.CameraDistance(),a.CameraFov()},
        frame(a.CurrentFrame()),last(a.LastRegisteredFrame()),parentModel(a.CameraParentModel()),parentBone(a.CameraParentBone()),
        cursor(a.CameraTrackCursor()),active(a.CameraTrackActive()),physicsReset(a.PhysicsResetPending()),modified(a.SceneModified()),
        recorded(a.RecordedFrameCountByte1()),messageSeen(a.MessageSeen()),cameraMode(a.CameraMode()),modelSlot(a.SelectedModelSlot()),
        accessorySlot(a.SelectedAccessorySlot()),perspective(a.CameraPerspective()),projectionPending(a.cameraProjectionRefreshPending),mode(h.Mode()),
        resetCheckbox(SendDlgItemMessageW(a.MainWindow(),panel::kPhysicsFrameCheckbox,BM_GETCHECK,0,0)){
        std::memcpy(selectedTracks.data(),a.state.globalTrackSelected,sizeof(a.state.globalTrackSelected));
        std::wstring error;Check(curve_host::CaptureAuthoring(app,verification,error),"capture original authoring before isolated real camera fixture");
        curve_host::SwapAuthoring(app,authoring);
    }
    ~Original(){
        Check(history.Reset()==MMS_OK,"release sparse real-host history before removing its private camera pool");history.Collect();
        curve_host::AuthoringTable discarded;curve_host::SwapAuthoring(app,discarded);
        auto* temporary=app.CameraKeys();app.CameraKeys()=camera;if(temporary!=camera)mdl::KeyPoolDestroy(temporary);
        curve_host::SwapAuthoring(app,authoring);marks.Restore(app);
        for(unsigned axis=0;axis<3;++axis){app.CameraPositionAxis(axis)=pose[axis];app.CameraRotation()[axis]=pose[axis+3];}
        app.CameraDistance()=pose[6];app.CameraFov()=pose[7];app.CameraPerspective()=perspective;
        app.CameraParentModel()=parentModel;app.CameraParentBone()=parentBone;app.CurrentFrame()=frame;app.LastRegisteredFrame()=last;
        app.CameraTrackCursor()=cursor;app.CameraTrackActive()=active;app.PhysicsResetPending()=physicsReset;
        app.SceneModified()=modified;app.RecordedFrameCountByte1()=recorded;app.MessageSeen()=messageSeen;
        app.CameraMode()=cameraMode;app.SelectedModelSlot()=modelSlot;app.SelectedAccessorySlot()=accessorySlot;app.cameraProjectionRefreshPending=projectionPending;
        std::memcpy(app.state.globalTrackSelected,selectedTracks.data(),sizeof(app.state.globalTrackSelected));
        SendDlgItemMessageW(app.MainWindow(),panel::kPhysicsFrameCheckbox,BM_SETCHECK,resetCheckbox,0);
        Check(history.SetMode(mode)==MMS_OK&&app.CameraKeys()==camera&&mdl::KeyPoolIdentity(camera)==cameraIdentity&&curve_host::AuthoringUnchanged(app,verification),
            "real sparse fixture restores original pool identity, authoring, history settings, pose and selection");
    }
};
void Build(MMDApp& app){
    curve_host::AuthoringTable priorFixture;curve_host::SwapAuthoring(app,priorFixture);
    auto* keys=app.CameraKeys();std::memset(keys,0,mdl::KeyPoolCapacity(keys)*sizeof(*keys));
    for(unsigned i=0;i<3;++i){auto& key=keys[i];key.frame=i==0?0:i==1?12:31;key.previous=i?i-1:0;key.next=i<2?i+1:0;
        key.parentModel=-1;key.distance=-45.f-float(i)*3;key.fov=45+int(i)*5;key.eye[0]=float(i);key.eye[1]=10;key.target[1]=float(i)*.1f;
        std::memset(key.interpolation,20,12);std::memset(reinterpret_cast<unsigned char*>(key.interpolation)+12,107,12);}
    auto channels=Read(app);auto& sparse=channels[0].track.keys;
    sparse.erase(std::remove_if(sparse.begin(),sparse.end(),[](const auto& key){return key.frame==12;}),sparse.end());
    sparse.front().outWeight=.19;sparse.front().outSlope=.013;sparse.front().tangent=animation_curve::Tangent::Free;
    sparse.back().inWeight=.31;sparse.back().inSlope=-.017;sparse.back().tangent=animation_curve::Tangent::Free;
    channels[1].track.keys.back().interpolation=animation_curve::Interpolation::Step;
    std::wstring error;Check(curve_host::Write(app,curve_host::Kind::Camera,0,channels,error),"real camera fixture authors a missing lane key while union native frame remains");
    const auto read=Read(app);Check(!Has(read[0],12)&&Has(read[1],12)&&app.CameraKeys()[1].frame==12,"real sparse camera fixture retains native frame twelve in another lane");
}
void SamePose(MMDApp& app){
    const auto& key=app.CameraKeys()[1];for(unsigned axis=0;axis<3;++axis){app.CameraPositionAxis(axis)=key.eye[axis];app.CameraRotation()[axis]=key.target[axis];}
    app.CameraDistance()=key.distance;app.CameraFov()=float(key.fov);app.CameraPerspective()=key.perspective;
    app.CameraParentModel()=key.parentModel;app.CameraParentBone()=key.parentBone;
}
bool NativeSame(MMDApp& app,const std::vector<mdl::CameraKey>& expected,bool marks){
    if(mdl::KeyPoolCapacity(app.CameraKeys())!=expected.size())return false;
    for(std::size_t i=0;i<expected.size();++i){auto a=app.CameraKeys()[i],b=expected[i];if(!marks)a.selected=b.selected=0;
        if(std::memcmp(&a,&b,sizeof(a)))return false;}return true;
}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    using edit_history_host_checks::Menu;
    Check(app&&!history.UndoCount()&&!history.RedoCount(),"real sparse camera fixture begins with a clean shared journal");
    const auto initial=value();Original original(*app,history);app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(8);
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        Build(*app);Check(history.SetMode(mode)==MMS_OK,"real sparse explicit registration configures recording mode");
        app->CameraMode()=1;app->CurrentFrame()=12;app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
        SendDlgItemMessageW(app->MainWindow(),panel::kPhysicsFrameCheckbox,BM_SETCHECK,BST_UNCHECKED,0);SamePose(*app);
        const auto pristine=Encode(Read(*app));const std::vector<mdl::CameraKey> before(app->CameraKeys(),app->CameraKeys()+mdl::KeyPoolCapacity(app->CameraKeys()));
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe prepared");SendMessageW(app->MainWindow(),WM_COMMAND,452,0);
        Check(history.UndoCount()==1&&Encode(Read(*app))==pristine&&NativeSame(*app,before,true),"real pending plugin edit rejects same-valued sparse registration without modifying native data or authored keys");
        Menu(app,L"Edit probe cancel");SendMessageW(app->MainWindow(),WM_COMMAND,452,0);
        const auto afterChannels=Read(*app);const auto completed=Encode(afterChannels);
        Check(std::all_of(afterChannels.begin(),afterChannels.end(),[](const auto& channel){return Has(channel,12);})&&completed!=pristine&&NativeSame(*app,before,false),
            "actual camera command 452 registers the sparse authored lane even when native key payload is unchanged");
        Check(history.UndoCount()==2&&value()==initial+1&&app->CurrentFrame()==12,"real sparse registration is one explicit step between plugin edits in both recording modes");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");app->CameraMode()=0;app->CurrentFrame()=23;
        Menu(app,L"Edit probe undo");
        Check(Encode(Read(*app))==pristine&&NativeSame(*app,before,true)&&value()==initial+1&&app->CurrentFrame()==23,
            "real sparse camera undo restores exact original Free/Step authoring and native marks across model mode without seeking");
        Menu(app,L"Edit probe undo");Check(value()==initial,"real earlier plugin step remains separately undoable");
        Menu(app,L"Edit probe redo");Menu(app,L"Edit probe redo");
        Check(Encode(Read(*app))==completed&&value()==initial+1&&app->CurrentFrame()==23,"real sparse redo restores exact registered curve after preceding plugin redo");
        Menu(app,L"Edit probe redo");Check(value()==initial+2&&history.UndoCount()==3,"real later plugin edit remains separately redoable");
        app->CameraMode()=1;app->CurrentFrame()=12;SamePose(*app);app->SceneModified()=0;SendMessageW(app->MainWindow(),WM_COMMAND,452,0);
        Check(history.UndoCount()==3&&!app->SceneModified()&&Encode(Read(*app))==completed,"actual repeated completed sparse camera registration is a true no-op");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(value()==initial&&history.Reset()==MMS_OK,"real sparse camera fixture restores plugin baseline");history.Collect();
    }
    std::puts("PASS actual sparse camera registration, exact authored undo/redo, plugin interleave and both history modes");
}
}
