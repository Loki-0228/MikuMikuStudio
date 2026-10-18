// Include inside edit_history_host_checks after its Menu helper. Run before
// that fixture removes accessory 1. All resources and the bound journal are
// the application's real loaded PMX/PMD and X objects.
namespace global_key_delete_host_checks {
using Kind=curve_host::Kind;
using Mode=plugins::SharedHistory::RecordingMode;
using Channels=std::vector<curve_editor::Channel>;
using Bytes=std::vector<std::uint8_t>;

template<class K>struct Pool {
    K* pointer;std::uint64_t identity;std::vector<K> records;
    explicit Pool(K* p):pointer(p),identity(mdl::KeyPoolIdentity(p)),records(p,p+mdl::KeyPoolCapacity(p)) {
        Check(p&&identity&&records.size()>3,"actual global deletion fixture uses an owned sufficiently sized native pool");
    }
    bool Stable(K* p)const{return p==pointer&&mdl::KeyPoolIdentity(p)==identity&&mdl::KeyPoolCapacity(p)==records.size();}
    bool Same(K* p)const{return Stable(p)&&!std::memcmp(p,records.data(),records.size()*sizeof(K));}
    void Restore(K* p)const{Check(Stable(p),"actual global deletion never replaced a resident key pool");
        std::memcpy(p,records.data(),records.size()*sizeof(K));}
};
struct Keys {
    Pool<mdl::CameraKey> camera;Pool<mdl::LightKey> light;Pool<mdl::SelfShadowKey> shadow;Pool<mdl::GravityKey> gravity;
    std::array<Pool<mdl::AccessoryKey>,2> accessories;
    explicit Keys(MMDApp& a):camera(a.CameraKeys()),light(a.LightKeys()),shadow(a.ShadowKeys()),gravity(a.GravityKeys()),
        accessories{Pool<mdl::AccessoryKey>(a.AccessoryKeys(0)),Pool<mdl::AccessoryKey>(a.AccessoryKeys(1))}{}
    bool Stable(MMDApp& a)const{return camera.Stable(a.CameraKeys())&&light.Stable(a.LightKeys())&&shadow.Stable(a.ShadowKeys())&&
        gravity.Stable(a.GravityKeys())&&accessories[0].Stable(a.AccessoryKeys(0))&&accessories[1].Stable(a.AccessoryKeys(1));}
    bool Same(MMDApp& a)const{return camera.Same(a.CameraKeys())&&light.Same(a.LightKeys())&&shadow.Same(a.ShadowKeys())&&
        gravity.Same(a.GravityKeys())&&accessories[0].Same(a.AccessoryKeys(0))&&accessories[1].Same(a.AccessoryKeys(1));}
    void Restore(MMDApp& a)const{camera.Restore(a.CameraKeys());light.Restore(a.LightKeys());shadow.Restore(a.ShadowKeys());
        gravity.Restore(a.GravityKeys());for(unsigned i=0;i<2;++i)accessories[i].Restore(a.AccessoryKeys(i));}
};
struct Live {
    std::array<double,49> values{};
    explicit Live(MMDApp& a){unsigned n=0;
        for(unsigned i=0;i<3;++i){values[n++]=a.CameraPositionAxis(i);values[n++]=a.CameraRotation()[i];}
        values[n++]=a.CameraDistance();values[n++]=a.CameraFov();values[n++]=a.CameraPerspective();
        values[n++]=a.CameraParentModel();values[n++]=a.CameraParentBone();
        for(unsigned i=0;i<3;++i){values[n++]=a.LightDirection()[i];values[n++]=a.LightColor()[i];}
        values[n++]=a.ShadowDistance();values[n++]=a.ShadowMode();values[n++]=a.GravityMagnitude();
        for(unsigned i=0;i<3;++i)values[n++]=a.GravityDirection()[i];values[n++]=a.GravityNoise();values[n++]=a.GravityNoiseModeWord();
        for(unsigned slot=0;slot<2;++slot){const auto& o=*a.AccessorySlot(slot);
            for(unsigned i=0;i<3;++i){values[n++]=o.position[i];values[n++]=o.rotation[i];}
            values[n++]=o.scale;values[n++]=o.opacity;values[n++]=o.visible;values[n++]=o.shadowEnabled;
            values[n++]=o.parentModel;values[n++]=o.parentBone;}
        if(n!=values.size())Check(false,"capture every unregistered global and two-accessory author value");
    }
    bool Same(MMDApp& a)const{return Live(a).values==values;}
    void Restore(MMDApp& a)const{unsigned n=0;
        for(unsigned i=0;i<3;++i){a.CameraPositionAxis(i)=float(values[n++]);a.CameraRotation()[i]=float(values[n++]);}
        a.CameraDistance()=float(values[n++]);a.CameraFov()=float(values[n++]);a.CameraPerspective()=std::uint8_t(values[n++]);
        a.CameraParentModel()=int(values[n++]);a.CameraParentBone()=int(values[n++]);
        for(unsigned i=0;i<3;++i){a.LightDirection()[i]=float(values[n++]);a.LightColor()[i]=float(values[n++]);}
        a.ShadowDistance()=float(values[n++]);a.ShadowMode()=int(values[n++]);a.GravityMagnitude()=float(values[n++]);
        for(unsigned i=0;i<3;++i)a.GravityDirection()[i]=float(values[n++]);a.GravityNoise()=int(values[n++]);a.GravityNoiseModeWord()=std::uint32_t(values[n++]);
        for(unsigned slot=0;slot<2;++slot){auto& o=*a.AccessorySlot(slot);
            for(unsigned i=0;i<3;++i){o.position[i]=float(values[n++]);o.rotation[i]=float(values[n++]);}
            o.scale=float(values[n++]);o.opacity=float(values[n++]);o.visible=std::uint8_t(values[n++]);o.shadowEnabled=std::uint8_t(values[n++]);
            o.parentModel=int(values[n++]);o.parentBone=int(values[n++]);}
        a.ApplyTimelineLightState();
    }
};
struct View {
    std::uint8_t cameraMode,model,accessory;std::array<std::uint8_t,4> tracks{};std::array<std::uint8_t,2> rows{};
    explicit View(MMDApp& a):cameraMode(a.CameraMode()),model(a.SelectedModelSlot()),accessory(a.SelectedAccessorySlot()){
        std::memcpy(tracks.data(),a.state.globalTrackSelected,sizeof(a.state.globalTrackSelected));
        for(unsigned i=0;i<2;++i)rows[i]=a.AccessorySlot(i)->rowSelected;
    }
    void Restore(MMDApp& a)const{a.CameraMode()=cameraMode;a.SelectedModelSlot()=model;a.SelectedAccessorySlot()=accessory;
        std::memcpy(a.state.globalTrackSelected,tracks.data(),sizeof(a.state.globalTrackSelected));
        for(unsigned i=0;i<2;++i)a.AccessorySlot(i)->rowSelected=rows[i];}
    bool Same(MMDApp& a)const{const View now(a);return now.cameraMode==cameraMode&&now.model==model&&now.accessory==accessory&&now.tracks==tracks&&now.rows==rows;}
};
struct TrackState {
    std::array<std::uint32_t,6> cursors;std::array<std::uint8_t,6> active;
    explicit TrackState(MMDApp& a):cursors{a.CameraTrackCursor(),a.LightTrackCursor(),a.ShadowTrackCursor(),a.GravityTrackCursor(),
        a.AccessoryTrackCursor(0),a.AccessoryTrackCursor(1)},active{a.CameraTrackActive(),a.LightTrackActive(),a.ShadowTrackActive(),
        a.GravityTrackActive(),a.AccessoryTrackActive(0),a.AccessoryTrackActive(1)}{}
    void Restore(MMDApp& a)const{a.CameraTrackCursor()=cursors[0];a.LightTrackCursor()=cursors[1];a.ShadowTrackCursor()=cursors[2];a.GravityTrackCursor()=cursors[3];
        a.CameraTrackActive()=active[0];a.LightTrackActive()=active[1];a.ShadowTrackActive()=active[2];a.GravityTrackActive()=active[3];
        for(unsigned i=0;i<2;++i){a.AccessoryTrackCursor(i)=cursors[4+i];a.AccessoryTrackActive(i)=active[4+i];}}
};
template<class T>void Append(Bytes& bytes,const T& value){const auto* p=reinterpret_cast<const std::uint8_t*>(&value);bytes.insert(bytes.end(),p,p+sizeof(value));}
void Append(Bytes& bytes,const void* value,std::size_t size){if(!size)return;const auto* p=static_cast<const std::uint8_t*>(value);bytes.insert(bytes.end(),p,p+size);}
Bytes ModelState(MMDApp& a){const auto& m=*mdl::Mdl(a.ModelSlot(0));Bytes bytes;
    Append(bytes,m.undoRings);Append(bytes,m.undoState);Append(bytes,m.undoDirty);Append(bytes,m.redoDirty);Append(bytes,m.loadComplete);
    Append(bytes,m.boneKeys,std::size_t(m.boneCount)*sizeof(mdl::BoneKey));
    for(unsigned i=0;i<m.boneCount;++i){Append(bytes,m.boneTable[i].trans);Append(bytes,m.boneTable[i].rotQuat);Append(bytes,m.bonePhysicsState[i]);}
    for(unsigned i=0;i<m.morphCount;++i)Append(bytes,m.morphs[i].value);return bytes;
}
struct Original {
    MMDApp& app;plugins::SharedHistory& history;Keys keys;Live pose;View view;TrackState tracks;
    curve_host::AuthoringTable authoring,verification;
    std::array<std::vector<std::uint8_t>,253> otherMarks;
    std::array<mdl::AccessoryRecord*,2> objects;std::array<mdl::AccessoryRecord,2> objectValues;
    unsigned char* model;std::array<wchar_t,256> modelPath{};Bytes modelState;
    int frame;std::uint32_t last,messageSeen;std::uint8_t modified,recorded,physicsReset;bool projectionPending;
    Mode mode;D3DMATRIX projection{};bool hasProjection=false;
    Original(MMDApp& a,plugins::SharedHistory& h):app(a),history(h),keys(a),pose(a),view(a),tracks(a),objects{a.AccessorySlot(0),a.AccessorySlot(1)},
        objectValues{*objects[0],*objects[1]},model(a.ModelSlot(0)),modelState(ModelState(a)),frame(a.CurrentFrame()),last(a.LastRegisteredFrame()),
        messageSeen(a.MessageSeen()),modified(a.SceneModified()),recorded(a.RecordedFrameCountByte1()),physicsReset(a.PhysicsResetPending()),
        projectionPending(a.cameraProjectionRefreshPending),mode(h.Mode()){
        std::memcpy(modelPath.data(),mdl::Mdl(model)->path,sizeof(mdl::Mdl(model)->path));
        for(unsigned slot=2;slot<255;++slot){auto* p=a.AccessoryKeys(slot);auto& marks=otherMarks[slot-2];marks.reserve(mdl::KeyPoolCapacity(p));
            for(std::size_t i=0;i<mdl::KeyPoolCapacity(p);++i){marks.push_back(p[i].selected);p[i].selected=0;}}
        std::wstring error;Check(curve_host::CaptureAuthoring(a,verification,error),"capture original real-host authoring before global delete fixture");
        curve_host::SwapAuthoring(a,authoring);
        if(auto* r=a.Renderer())hasProjection=r->device&&SUCCEEDED(r->device->GetTransform(D3DTS_PROJECTION,&projection));
    }
    void Stable()const{Check(keys.Stable(app)&&app.ModelSlot(0)==model&&ModelState(app)==modelState,
        "actual global delete and plugin history retain native pools, model pose and both legacy undo rings");
        for(unsigned i=0;i<2;++i)Check(app.AccessorySlot(i)==objects[i]&&objects[i]->mesh==objectValues[i].mesh&&objects[i]->materials==objectValues[i].materials&&
            objects[i]->texturePaths==objectValues[i].texturePaths,"actual multi-accessory key history retains each loaded mesh and resource identity");
    }
    ~Original(){Check(history.Reset()==MMS_OK,"release real global-key fixture history before restoring its resident state");history.Collect();
        Stable();curve_host::AuthoringTable discarded;curve_host::SwapAuthoring(app,discarded);keys.Restore(app);
        for(unsigned i=0;i<2;++i)*objects[i]=objectValues[i];pose.Restore(app);
        std::memcpy(mdl::Mdl(model)->path,modelPath.data(),sizeof(mdl::Mdl(model)->path));
        for(unsigned slot=2;slot<255;++slot){auto* p=app.AccessoryKeys(slot);const auto& marks=otherMarks[slot-2];
            Check(mdl::KeyPoolCapacity(p)==marks.size(),"unrelated accessory pool size survives real global deletion");
            for(std::size_t i=0;i<marks.size();++i)p[i].selected=marks[i];}
        curve_host::SwapAuthoring(app,authoring);view.Restore(app);tracks.Restore(app);
        app.CurrentFrame()=frame;app.LastRegisteredFrame()=last;app.MessageSeen()=messageSeen;app.SceneModified()=modified;
        app.RecordedFrameCountByte1()=recorded;app.PhysicsResetPending()=physicsReset;app.cameraProjectionRefreshPending=projectionPending;
        Check(history.SetMode(mode)==MMS_OK&&keys.Same(app)&&pose.Same(app)&&curve_host::AuthoringUnchanged(app,verification),
            "global deletion host fixture restores all native keys, authoring, pose and history settings");
        plugins::RefreshBoundPoseReadout(app);plugins::RefreshBoundGlobalReadout(app);
        if(hasProjection)Check(SUCCEEDED(app.Renderer()->device->SetTransform(D3DTS_PROJECTION,&projection)),"restore real device projection after global deletion checks");
        app.cameraProjectionRefreshPending=projectionPending;app.MessageSeen()=messageSeen;
    }
};
template<class K>void Chain(K* keys){std::memset(keys,0,mdl::KeyPoolCapacity(keys)*sizeof(K));
    for(unsigned i=0;i<3;++i){keys[i].frame=i==0?0:i==1?12:31;keys[i].previous=i?i-1:0;keys[i].next=i<2?i+1:0;}}
template<class K>unsigned Find(const K* keys,unsigned frame,std::size_t capacity){unsigned slot=0;
    for(std::size_t hops=0;slot<capacity&&hops<capacity;++hops){if(keys[slot].frame==frame)return slot;if(!keys[slot].next)break;slot=keys[slot].next;}
    return UINT_MAX;
}
template<class K>unsigned Find(K* keys,unsigned frame){return Find(keys,frame,mdl::KeyPoolCapacity(keys));}
template<class K>void Mark(K* keys,unsigned frame){const auto slot=Find(keys,frame);Check(slot!=UINT_MAX,"real fixture has the global key selected for deletion");keys[slot].selected=1;}
Channels Read(MMDApp& app,Kind kind,int slot=0){Channels channels;std::wstring error;
    Check(curve_host::Read(app,kind,slot,channels,error),"read actual host global or accessory source for exact authoring checks");return channels;}
Bytes Encode(const Channels& channels){std::vector<animation_curve::NamedTrack> tracks;for(const auto& c:channels)tracks.push_back({c.id,c.track});
    Bytes result;std::string error;Check(animation_curve::Encode(tracks,result,error),"encode real global deletion authored curves");return result;}
bool Same(const Channels& a,const Channels& b){if(a.size()!=b.size()||Encode(a)!=Encode(b))return false;
    for(std::size_t i=0;i<a.size();++i)if(a[i].nativeControls!=b[i].nativeControls||a[i].controlGeometry!=b[i].controlGeometry||
        a[i].preservedCameraIntervals!=b[i].preservedCameraIntervals)return false;return true;
}
struct Authoring {
    std::array<Channels,6> channels;
    explicit Authoring(MMDApp& app){for(unsigned i=0;i<6;++i)channels[i]=Read(app,i<4?Kind(i):Kind::Accessory,i<4?0:int(i-4));}
    bool SameAs(MMDApp& app)const{for(unsigned i=0;i<6;++i)if(!Same(channels[i],Read(app,i<4?Kind(i):Kind::Accessory,i<4?0:int(i-4))))return false;return true;}
};
struct OfflineSources {
    struct Entry{std::filesystem::path source,moved;bool active=false;};std::vector<Entry> entries;
    explicit OfflineSources(MMDApp& app){
        const auto cwd=std::filesystem::current_path();
        const std::array<std::filesystem::path,3> paths{mdl::Mdl(app.ModelSlot(0))->path,app.AccessorySlot(0)->sourcePath,app.AccessorySlot(1)->sourcePath};
        // The driver has copied these resources into its own disposable cwd.
        // Validate every source and destination before moving any file, and
        // deduplicate the two accessories that share the same source X file.
        for(const auto& path:paths){const auto source=std::filesystem::absolute(path).lexically_normal();
            Check(std::filesystem::is_regular_file(source)&&std::filesystem::equivalent(source.parent_path(),cwd),
                "offline global delete sources are regular files inside the isolated host fixture directory");
            bool duplicate=false;for(const auto& entry:entries)duplicate|=std::filesystem::equivalent(source,entry.source);
            if(duplicate)continue;
            const auto moved=std::filesystem::path(source.native()+L".global-delete-offline-"+std::to_wstring(GetCurrentProcessId()));
            Check(std::filesystem::equivalent(moved.parent_path(),cwd)&&!std::filesystem::exists(moved),
                "offline global delete destination stays in the isolated fixture directory and cannot overwrite a file");
            entries.push_back({source,moved,false});
        }
        try{for(auto& entry:entries){std::filesystem::rename(entry.source,entry.moved);entry.active=true;}}
        catch(...){Restore(false);throw;}
        Check(Missing(),"the real loaded model and both accessory source files are offline before global deletion");
    }
    bool Missing()const{for(const auto& entry:entries)if(!entry.active||std::filesystem::exists(entry.source)||!std::filesystem::exists(entry.moved))return false;return true;}
    void Restore(bool checked){for(auto& entry:entries)if(entry.active){std::error_code error;std::filesystem::rename(entry.moved,entry.source,error);
            if(!error)entry.active=false;if(checked)Check(!error&&std::filesystem::is_regular_file(entry.source),"restore original isolated source file after actual global delete history");}}
    ~OfflineSources(){Restore(false);}
};
void Build(MMDApp& app,plugins::SharedHistory& history,unsigned selectedFrame,bool authored){
    curve_host::AuthoringTable previous;curve_host::SwapAuthoring(app,previous);
    Chain(app.CameraKeys());Chain(app.LightKeys());Chain(app.ShadowKeys());Chain(app.GravityKeys());
    for(unsigned slot=0;slot<2;++slot)Chain(app.AccessoryKeys(slot));
    for(unsigned i=0;i<3;++i){auto& c=app.CameraKeys()[i];c.parentModel=-1;c.distance=-30.f-float(i)*3;c.fov=39+int(i)*7;c.perspective=1;
        for(unsigned axis=0;axis<3;++axis){c.eye[axis]=float(axis+i)+.125f;c.target[axis]=float(axis+i)*.1f;
            auto& l=app.LightKeys()[i];l.direction[axis]=float(axis)*.25f-float(i)*.1f-.5f;l.color[axis]=.2f+float(axis+i)*.1f;
            auto& g=app.GravityKeys()[i];g.direction[axis]=axis==1?-.8f:.1f*float(axis+i);}
        for(unsigned lane=0;lane<6;++lane){c.interpolation[0][lane]=std::uint8_t(20+lane);c.interpolation[1][lane]=std::uint8_t(30+lane);
            c.interpolation[2][lane]=std::uint8_t(80+lane);c.interpolation[3][lane]=std::uint8_t(100+lane);}
        c.reserved1[0]=0x45;c.reserved2[2]=0x67;auto& s=app.ShadowKeys()[i];s.mode=std::uint8_t(i%3);s.distance=.03f+.01f*float(i);
        auto& g=app.GravityKeys()[i];g.acceleration=12.f+float(i);g.noise=30+int(i);g.noiseEnabled=1;g.reserved1[0]=0x12;g.reserved1[1]=0x34;
        for(unsigned slot=0;slot<2;++slot){auto& a=app.AccessoryKeys(slot)[i];a.parentModel=-1;a.visible=a.shadowEnabled=1;
            for(unsigned axis=0;axis<3;++axis){a.position[axis]=float(axis+i+slot)+.25f;a.rotation[axis]=float(axis+i)*.1f;}
            a.scale=1.2f+.1f*float(i);a.opacity=.5f+.15f*float(i);}}
    app.CameraMode()=1;app.SelectedModelSlot()=0;app.SelectedAccessorySlot()=0;app.CurrentFrame()=int(selectedFrame);app.LastRegisteredFrame()=31;
    app.SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
    if(authored){for(unsigned i=0;i<6;++i){const auto kind=i<4?Kind(i):Kind::Accessory;const int slot=i<4?0:int(i-4);auto channels=Read(app,kind,slot);
            const unsigned lane=kind==Kind::Shadow?1:0;auto& keys=channels[lane].track.keys;
            keys.erase(std::remove_if(keys.begin(),keys.end(),[](const auto& k){return k.frame==12;}),keys.end());
            keys.front().tangent=keys.back().tangent=animation_curve::Tangent::Free;
            keys.front().outSlope=.013;keys.front().outWeight=.19;keys.back().inSlope=-.017;keys.back().inWeight=.31;
            std::wstring error;Check(curve_host::Write(app,kind,slot,channels,error),"prepare actual sparse Free global/accessory authoring through the production writer");}
        Check(history.Reset()==MMS_OK,"clear only completed authoring preparation before actual deletion history checks");history.Collect();}
    for(unsigned i=0;i<3;++i){app.CameraPositionAxis(i)=77.125f+float(i);app.CameraRotation()[i]=.314159f+float(i)*.1f;
        app.LightDirection()[i]=-.25f+float(i)*.3f;app.LightColor()[i]=.1234567f+float(i)*.2f;app.GravityDirection()[i]=i==1?-.75f:.125f*float(i);}
    app.CameraDistance()=-87.25f;app.CameraFov()=58.125f;app.CameraPerspective()=0;app.CameraParentModel()=-1;app.CameraParentBone()=0;
    app.ShadowDistance()=.067891f;app.ShadowMode()=2;app.GravityMagnitude()=42.125f;app.GravityNoise()=25;app.GravityNoiseModeWord()=0x12345601;
    for(unsigned slot=0;slot<2;++slot){auto& a=*app.AccessorySlot(slot);a.parentModel=-1;a.parentBone=0;
        for(unsigned i=0;i<3;++i){a.position[i]=99.125f+float(i+slot);a.rotation[i]=.314159f+float(i+slot)*.1f;}
        a.scale=1.2345f;a.opacity=.4567f;a.visible=1;a.shadowEnabled=0;a.rowSelected=1;app.AccessoryTrackCursor(slot)=0;app.AccessoryTrackActive(slot)=1;}
    app.CameraTrackCursor()=app.LightTrackCursor()=app.ShadowTrackCursor()=app.GravityTrackCursor()=0;
    app.CameraTrackActive()=app.LightTrackActive()=app.ShadowTrackActive()=app.GravityTrackActive()=1;
    Mark(app.CameraKeys(),selectedFrame);Mark(app.LightKeys(),selectedFrame);Mark(app.ShadowKeys(),selectedFrame);Mark(app.GravityKeys(),selectedFrame);
    for(unsigned i=0;i<2;++i)Mark(app.AccessoryKeys(i),selectedFrame);
    app.SceneModified()=app.RecordedFrameCountByte1()=app.PhysicsResetPending()=0;
    plugins::RefreshBoundPoseReadout(app);plugins::RefreshBoundGlobalReadout(app);
}
template<class K>void CheckRemoved(K* current,const Pool<K>& before,unsigned frame){
    const auto found=Find(before.records.data(),frame,before.records.size());Check(found!=UINT_MAX,"saved real global fixture contains the selected frame");
    if(frame)Check(Find(current,frame)==UINT_MAX&&!current[found].frame&&!current[found].previous&&!current[found].next&&!current[found].selected,
        "actual global command unlinks and clears a selected nonzero native slot");
    else Check(current[0].frame==0&&current[0].next==before.records[0].next&&!current[0].selected,
        "actual global command retains the zero frame head and its unselected successor chain");
    for(unsigned surviving:{0u,12u,31u})if(surviving!=frame)Check(Find(current,surviving)!=UINT_MAX,"actual delete retains every unselected native frame");
}
void CheckRemoved(MMDApp& app,const Keys& before,unsigned frame){
    CheckRemoved(app.CameraKeys(),before.camera,frame);CheckRemoved(app.LightKeys(),before.light,frame);CheckRemoved(app.ShadowKeys(),before.shadow,frame);
    CheckRemoved(app.GravityKeys(),before.gravity,frame);for(unsigned i=0;i<2;++i)CheckRemoved(app.AccessoryKeys(i),before.accessories[i],frame);
    if(!frame){const auto& c=app.CameraKeys()[0];const auto& l=app.LightKeys()[0];const auto& s=app.ShadowKeys()[0];const auto& g=app.GravityKeys()[0];
        Check(c.distance==-45.f&&!c.eye[0]&&c.eye[1]==10.f&&!c.eye[2]&&!c.target[0]&&!c.target[1]&&!c.target[2]&&
            c.parentModel==-1&&!c.parentBone&&c.fov==before.camera.records[0].fov&&c.perspective==before.camera.records[0].perspective&&
            c.reserved1[0]==0x45&&c.reserved2[2]==0x67,"actual zero camera deletion resets native pose and retains FOV, projection and reserved data");
        for(unsigned lane=0;lane<6;++lane)Check(c.interpolation[0][lane]==20&&c.interpolation[1][lane]==20&&c.interpolation[2][lane]==107&&c.interpolation[3][lane]==107,
            "actual zero camera deletion resets all six native interpolation curves");
        Check(l.direction[0]==-.5f&&l.direction[1]==-1.f&&l.direction[2]==.5f&&l.color[0]==.602f&&l.color[1]==.602f&&l.color[2]==.602f&&
            s.mode==1&&s.distance==.01125f&&g.acceleration==9.8000002f&&g.direction[1]==-1.f&&!g.direction[0]&&!g.direction[2]&&
            g.noise==10&&!g.noiseEnabled&&g.reserved1[0]==0x12&&g.reserved1[1]==0x34,"actual zero deletion resets light, shadow and gravity native root payloads");
        for(unsigned i=0;i<2;++i){const auto& a=app.AccessoryKeys(i)[0];Check(a.parentModel==-1&&!a.parentBone&&a.visible==1&&!a.shadowEnabled&&a.scale==1&&a.opacity==1&&
                !a.position[0]&&!a.position[1]&&!a.position[2]&&!a.rotation[0]&&!a.rotation[1]&&!a.rotation[2],"actual zero deletion resets each selected accessory head independently");}}
}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    using edit_history_host_checks::Menu;
    Check(app&&app->ModelSlot(0)&&app->AccessorySlot(0)&&app->AccessorySlot(1)&&!history.UndoCount()&&!history.RedoCount(),
        "real global-key deletion begins with an existing PMX or PMD, two real X objects and the shared empty journal");
    Check(plugins::BoundNativePoseHistory(app)==&history,"real global-key deletion uses the manager's existing controlled binding");
    const auto initial=value();
    {
        Original original(*app,history);OfflineSources offline(*app);
        const auto stable=[&]{original.Stable();Check(offline.Missing(),"actual global/plugin history succeeds while all original fixture sources remain offline");};
        const auto command=[&](unsigned id){SendMessageW(app->MainWindow(),WM_COMMAND,id,0);stable();};
        const auto plugin=[&](const wchar_t* label){Menu(app,label);stable();};
        for(const auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(const unsigned frame:{0u,12u,31u}){
            Check(history.SetMode(mode)==MMS_OK,"configure actual global-key deletion history recording mode");
            Build(*app,history,frame,frame==12);original.Stable();
            const Keys before(*app);const Live pending(*app);const Authoring authoredBefore(*app);const auto last=app->LastRegisteredFrame();
            plugin(L"Edit probe prepared");command(423);
            Check(!history.UndoCount()&&!history.RedoCount()&&value()==initial&&before.Same(*app)&&pending.Same(*app)&&authoredBefore.SameAs(*app)&&
                !app->SceneModified()&&!app->RecordedFrameCountByte1()&&!app->PhysicsResetPending()&&app->LastRegisteredFrame()==last,
                "prepared real plugin transaction blocks actual five-family multi-accessory delete without legacy or partial writes");
            plugin(L"Edit probe cancel");plugin(L"Edit probe once");command(423);
            Check(history.UndoCount()==2&&!history.RedoCount()&&value()==initial+1&&pending.Same(*app)&&app->CurrentFrame()==int(frame)&&
                app->SceneModified()&&app->RecordedFrameCountByte1()&&app->PhysicsResetPending()&&app->LastRegisteredFrame()==last,
                "actual menu 423 deletes five families and both accessories once explicitly in either mode without overwriting unregistered poses");
            const auto* label=history.NextLabel(false);Check(label&&*label=="Delete global keys","actual global deletion has one shared native history node");
            CheckRemoved(*app,before,frame);const Keys after(*app);const Authoring authoredAfter(*app);
            if(frame==12)for(unsigned i=0;i<6;++i){const auto lane=i==2?1u:0u;const auto& source=authoredBefore.channels[i][lane].track.keys;
                const auto& target=authoredAfter.channels[i][lane].track.keys;
                Check(source.size()==2&&target.size()==2&&source.front().outSlope==target.front().outSlope&&source.back().inSlope==target.back().inSlope,
                    "actual deletion preserves an already-sparse Free authoring lane for every global family and both accessories");}
            plugin(L"Edit probe once");command(400);
            Check(value()==initial+1&&history.UndoCount()==2&&history.RedoCount()==1,"native undo first reverses the later real plugin edit");
            app->SceneModified()=app->RecordedFrameCountByte1()=app->PhysicsResetPending()=0;command(423);
            Check(history.UndoCount()==2&&history.RedoCount()==1&&value()==initial+1&&after.Same(*app)&&pending.Same(*app)&&
                !app->SceneModified()&&!app->RecordedFrameCountByte1()&&!app->PhysicsResetPending(),"actual unmarked global deletion is a no-op and retains later plugin redo");
            app->CurrentFrame()=23;app->CameraMode()=0;app->SelectedModelSlot()=0;app->SelectedAccessorySlot()=1;
            app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Gravity);app->AccessorySlot(0)->rowSelected=0;app->AccessorySlot(1)->rowSelected=1;
            const View later(*app);plugin(L"Edit probe undo");
            Check(history.UndoCount()==1&&history.RedoCount()==2&&value()==initial+1&&before.Same(*app)&&authoredBefore.SameAs(*app)&&
                pending.Same(*app)&&app->CurrentFrame()==23&&later.Same(*app),"actual plugin undo restores every deleted key and authoring without seeking, reopening sources or replacing later view selection");
            command(400);Check(!history.UndoCount()&&history.RedoCount()==3&&value()==initial,"native undo reaches the earlier plugin baseline");
            plugin(L"Edit probe redo");command(401);
            Check(history.UndoCount()==2&&history.RedoCount()==1&&value()==initial+1&&after.Same(*app)&&authoredAfter.SameAs(*app)&&
                pending.Same(*app)&&app->CurrentFrame()==23&&later.Same(*app),"actual native redo reapplies complete global deletion from owned data across model mode and a new frame");
            plugin(L"Edit probe redo");Check(value()==initial+2&&history.UndoCount()==3&&!history.RedoCount(),"later real plugin redo remains independently ordered after global deletion");
            command(400);plugin(L"Edit probe undo");command(400);
            Check(value()==initial&&!history.UndoCount()&&before.Same(*app)&&authoredBefore.SameAs(*app)&&pending.Same(*app)&&history.Reset()==MMS_OK,
                "actual global deletion restores plugin baseline and all five key families before the next fixture");history.Collect();
        }
        offline.Restore(true);
    }
    Check(value()==initial&&!history.UndoCount()&&!history.RedoCount(),"real global delete fixture leaves plugin state and the common journal ready for later host checks");
    std::puts("PASS actual menu 423 five-family/multi-accessory deletion, zero roots, sparse Free curves, missing sources and interleaved plugin/native history in both modes");
}
}
