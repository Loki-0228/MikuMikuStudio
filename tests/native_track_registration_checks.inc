// CPU-only TrackAuthoring tests; include after common native history fixtures.
namespace {
template<class K> struct TrackRegistrationFixture {
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    mdl::AccessoryRecord accessory{};
    curve_host::Kind kind;
    int slot=0;
    K*& Pool(){
        if constexpr(std::is_same_v<K,mdl::CameraKey>)return app->CameraKeys();
        else if constexpr(std::is_same_v<K,mdl::LightKey>)return app->LightKeys();
        else if constexpr(std::is_same_v<K,mdl::SelfShadowKey>)return app->ShadowKeys();
        else if constexpr(std::is_same_v<K,mdl::GravityKey>)return app->GravityKeys();
        else return app->AccessoryKeys(slot);
    }
    explicit TrackRegistrationFixture(bool sparse=true){
        ::new(static_cast<void*>(&app->state))MMDAppState{};
        app->EnglishUI()=2;app->CurrentFrame()=13;app->LastRegisteredFrame()=40;
        if constexpr(std::is_same_v<K,mdl::CameraKey>)kind=curve_host::Kind::Camera;
        else if constexpr(std::is_same_v<K,mdl::LightKey>)kind=curve_host::Kind::Light;
        else if constexpr(std::is_same_v<K,mdl::SelfShadowKey>)kind=curve_host::Kind::Shadow;
        else if constexpr(std::is_same_v<K,mdl::GravityKey>)kind=curve_host::Kind::Gravity;
        else{kind=curve_host::Kind::Accessory;slot=7;app->AccessorySlot(slot)=&accessory;}
        Pool()=mdl::KeyPoolCreate<K>(8);
        for(unsigned i=0;i<3;++i){
            auto& key=Pool()[i];key.frame=20*i;key.previous=i?i-1:0;key.next=i<2?i+1:0;
            if constexpr(std::is_same_v<K,mdl::CameraKey>){
                key.parentModel=-1;key.fov=45;key.distance=-33;key.eye[0]=float(i);key.eye[1]=2;
                std::memset(key.interpolation,20,12);std::memset(&key.interpolation[2][0],107,12);
            }else if constexpr(std::is_same_v<K,mdl::LightKey>){key.direction[0]=float(i);for(auto& v:key.color)v=.5f;}
            else if constexpr(std::is_same_v<K,mdl::SelfShadowKey>){key.mode=1;key.distance=.05f;}
            else if constexpr(std::is_same_v<K,mdl::GravityKey>){key.acceleration=9.8f;key.direction[1]=-1;key.noise=3;}
            else{key.parentModel=-1;key.position[0]=float(i);key.scale=1;key.opacity=.5f;key.visible=1;}
        }
        if(!sparse)return;
        auto channels=Read();channels[0].track.keys.erase(channels[0].track.keys.begin()+1);
        if(!channels[0].nativeControls.empty())channels[0].nativeControls.erase(channels[0].nativeControls.begin()+1);
        if(!channels[0].controlGeometry.empty())channels[0].controlGeometry.erase(channels[0].controlGeometry.begin()+1);
        channels[0].selected=false;channels[1].collections.push_back({"retained",L"retained metadata"});
        if(!channels[1].track.discrete){channels[1].track.keys[0].outWeight=.31;channels[1].track.keys[0].outSlope=.625;}
        std::wstring error;
        Check(curve_host::Write(*app,kind,slot,channels,error),"create sparse authored lane with a shared native row");
        Check(Read()[0].track.keys.size()==2,"sparse authored lane starts without frame twenty");
        app->SceneModified()=0;
    }
    ~TrackRegistrationFixture(){curve_host::Reset(app.get());mdl::KeyPoolDestroy(Pool());}
    std::vector<curve_editor::Channel> Read(){
        std::vector<curve_editor::Channel> channels;std::wstring error;
        Check(curve_host::Read(*app,kind,slot,channels,error),"read explicit-registration authoring fixture");return channels;
    }
    curve_host::NativeKeyView View(){return {Pool(),mdl::KeyPoolCapacity(Pool()),[](const void* p,std::uint32_t i)noexcept->const void*{return static_cast<const K*>(p)+i;}};}
    void* Identity(){if constexpr(std::is_same_v<K,mdl::AccessoryKey>)return &accessory;else return Pool();}
};
template<class K> void TrackRegistrationRoundtrip(){
    TrackRegistrationFixture<K> f;
    const auto keys=std::vector<K>(f.Pool(),f.Pool()+mdl::KeyPoolCapacity(f.Pool()));
    curve_host::AuthoringTable original;std::wstring error;
    Check(curve_host::CaptureAuthoring(*f.app,original,error),"capture original sparse authoring");
    const auto before=f.Read();curve_host::TrackAuthoring source(*f.app,f.kind,f.slot);
    const auto budget=source.RetainedBytes();
    std::vector<int> frames{40,20,20,0};
    Check(source.HasRegistrationChanges(frames)&&!source.HasRegistrationChanges({0,40}),
        "explicit registration sees missing authored keys despite unchanged native records");
    for(unsigned repeat=0;repeat<8;++repeat){
        auto target=source.Prepare(f.View(),frames,true);
        Check(target&&target->IsCurrent()&&target->Authored()&&curve_host::AuthoringUnchanged(*f.app,original),
            "explicit registration preparation is private and preserves authored metadata");
        failAfter=0;target->Publish(f.Identity());failAfter=-1;
        const auto after=f.Read();
        Check(after[0].track.keys.size()==3&&after[0].track.keys[1].frame==20,
            "explicit registration fills only the missing authored frame");
        Check(!after[0].selected&&after[1].collections.size()==1&&after[1].collections[0].id=="retained"&&
            after[1].track.keys[0].outSlope==before[1].track.keys[0].outSlope,
            "explicit registration preserves unrelated authoring and editor metadata");
        Check(!std::memcmp(f.Pool(),keys.data(),keys.size()*sizeof(K))&&f.app->CurrentFrame()==13&&!f.app->SceneModified(),
            "authoring publication never edits native keys playhead or scene flags");
        curve_host::TrackAuthoring current(*f.app,f.kind,f.slot);
        Check(!current.HasRegistrationChanges(frames),"registering complete authored lanes is a true no-op");
        auto undo=source.Prepare(f.View(),frames,false);
        Check(undo->IsCurrent(),"original source prepares undo after its own authored publication");
        failAfter=0;undo->Publish(f.Identity());failAfter=-1;
        f.Read(); // Published targets lazily refresh the native fingerprint.
        Check(curve_host::AuthoringUnchanged(*f.app,original)&&source.RetainedBytes()==budget,
            "undo restores exact sparse seed and repeated cycles retain no target caches");
    }
    auto target=source.Prepare(f.View(),frames,true);frames.clear();frames.shrink_to_fit();
    Check(target->IsCurrent(),"prepared target owns all data after its borrowed registration list is destroyed");
    target.reset();Check(curve_host::AuthoringUnchanged(*f.app,original),"discarding a prepared explicit registration changes nothing");
    curve_host::ReconcileRegisteredFrames(*f.app,f.kind,f.slot,{});
    Check(curve_host::AuthoringUnchanged(*f.app,original),"an empty legacy registration batch changes nothing");
    curve_host::ReconcileRegisteredFrames(*f.app,f.kind,f.slot,{40,20,20});
    Check(f.Read()[0].track.keys.size()==3,"legacy batched registration uses the same missing-lane reconciliation");
}
template<class K> void TrackRegistrationBatchChecks(){
    {
        TrackRegistrationFixture<K> f;auto channels=f.Read();
        channels[0].track.keys.resize(1);
        if(!channels[0].nativeControls.empty())channels[0].nativeControls.resize(1);
        if(!channels[0].controlGeometry.empty())channels[0].controlGeometry.resize(1);
        std::wstring error;Check(curve_host::Write(*f.app,f.kind,f.slot,channels,error),"create two missing lane keys for nested legacy registration batching");
        f.app->SceneModified()=0;
        curve_host::AuthoringTable before;
        Check(curve_host::CaptureAuthoring(*f.app,before,error),"capture deferred legacy batch authoring");
        const auto keys=std::vector<K>(f.Pool(),f.Pool()+mdl::KeyPoolCapacity(f.Pool()));
        {
            curve_host::LegacyRegistrationBatch outer(f.app.get());
            curve_host::NotifyLegacyRegistered(f.app.get(),f.kind,f.slot,0);
            {
                curve_host::LegacyRegistrationBatch inner(f.app.get());
                curve_host::NotifyLegacyRegistered(f.app.get(),f.kind,f.slot,20);
                curve_host::NotifyLegacyRegistered(f.app.get(),f.kind,f.slot,20);
                Check(curve_host::AuthoringUnchanged(*f.app,before),"nested legacy registration queues duplicate frames without per-key authoring writes");
            }
            Check(curve_host::AuthoringUnchanged(*f.app,before),"inner same-app batch exit leaves the outer collector deferred");
            const auto first=f.Read();
            Check(first[0].track.keys.size()==2&&first[0].track.keys.back().frame==20,
                "a curve read inside an open legacy batch flushes queued frames before evaluation");
            curve_host::AuthoringTable flushed;
            Check(curve_host::CaptureAuthoring(*f.app,flushed,error),"capture flushed batch authoring");
            curve_host::NotifyLegacyRegistered(f.app.get(),f.kind,f.slot,40);
            curve_host::NotifyLegacyRegistered(f.app.get(),f.kind,f.slot,40);
            Check(curve_host::AuthoringUnchanged(*f.app,flushed),"a flushed outer batch can collect later writes without immediate reconciliation");
            const auto second=f.Read();
            Check(second[0].track.keys.size()==3&&second[0].track.keys.back().frame==40,
                "the next curve read flushes newly queued frames in the same legacy batch");
        }
        Check(!std::memcmp(f.Pool(),keys.data(),keys.size()*sizeof(K))&&f.app->CurrentFrame()==13&&!f.app->SceneModified(),
            "nested legacy batch flushing never edits native keys playhead or scene flags");
    }
    {
        TrackRegistrationFixture<K> f;curve_host::AuthoringTable before;std::wstring error;
        Check(curve_host::CaptureAuthoring(*f.app,before,error),"capture scope-exit legacy batch authoring");
        {
            curve_host::LegacyRegistrationBatch outer(f.app.get());
            curve_host::NotifyLegacyRegistered(f.app.get(),f.kind,f.slot,0);
            curve_host::LegacyRegistrationBatch inner(f.app.get());
            curve_host::NotifyLegacyRegistered(f.app.get(),f.kind,f.slot,20);
            curve_host::NotifyLegacyRegistered(f.app.get(),f.kind,f.slot,20);
            Check(curve_host::AuthoringUnchanged(*f.app,before),"legacy batch with no intervening read stays private until scope exit");
        }
        Check(!curve_host::AuthoringUnchanged(*f.app,before),"outer legacy batch destructor flushes authoring without a curve read");
        Check(f.Read()[0].track.keys.size()==3,"scope-exit flush fills the registered sparse lane exactly once");
    }
}
void NativeTrackRegistrationChecks(){
    std::puts("  track authoring: explicit sparse registration and exact retained undo");
    TrackRegistrationRoundtrip<mdl::CameraKey>();TrackRegistrationRoundtrip<mdl::LightKey>();
    TrackRegistrationRoundtrip<mdl::SelfShadowKey>();TrackRegistrationRoundtrip<mdl::GravityKey>();TrackRegistrationRoundtrip<mdl::AccessoryKey>();
    std::puts("  track authoring: nested legacy batches, read barriers and scope-exit flushing");
    TrackRegistrationBatchChecks<mdl::CameraKey>();TrackRegistrationBatchChecks<mdl::LightKey>();
    TrackRegistrationBatchChecks<mdl::SelfShadowKey>();TrackRegistrationBatchChecks<mdl::GravityKey>();TrackRegistrationBatchChecks<mdl::AccessoryKey>();
    {
        TrackRegistrationFixture<mdl::CameraKey> f;curve_host::TrackAuthoring source(*f.app,f.kind,f.slot);
        curve_host::AuthoringTable original;std::wstring error;
        Check(curve_host::CaptureAuthoring(*f.app,original,error),"capture registration OOM baseline");
        const std::vector<int> frames{20};bool completed=false;unsigned refusals=0;
        for(long limit=0;limit<512;++limit){
            std::unique_ptr<curve_host::TrackAuthoring::Target> target;
            failAfter=limit;
            try{target=source.Prepare(f.View(),frames,true);}catch(const std::bad_alloc&){++refusals;}
            failAfter=-1;
            Check(curve_host::AuthoringUnchanged(*f.app,original),"registration preparation OOM never leaves partial authoring");
            if(target){Check(target->IsCurrent(),"registration target becomes current once preparation fits memory");completed=true;break;}
        }
        Check(completed&&refusals>10,"registration preparation exercises allocations through detached curve storage and map nodes");
        bool legacyCompleted=false;unsigned legacyRefusals=0;
        for(long limit=0;limit<512;++limit){
            failAfter=limit;
            try{curve_host::ReconcileRegisteredFrame(*f.app,f.kind,f.slot,20);legacyCompleted=true;}
            catch(const std::bad_alloc&){++legacyRefusals;}
            failAfter=-1;
            if(legacyCompleted)break;
            Check(curve_host::AuthoringUnchanged(*f.app,original),"legacy reconciliation OOM preserves its complete preexisting authoring");
        }
        Check(legacyCompleted&&legacyRefusals>10&&f.Read()[0].track.keys.size()==3,
            "legacy reconciliation publishes only after every required authoring allocation succeeds");
        // Restore the same captured seed before testing a different author.
        auto restore=source.Prepare(f.View(),frames,false);restore->Publish(f.Identity());
        auto stale=source.Prepare(f.View(),frames,true);auto channels=f.Read();channels[1].track.keys[0].outSlope+=.25;
        Check(curve_host::Write(*f.app,f.kind,f.slot,channels,error)&&!stale->IsCurrent()&&!source.IsCurrent(),
            "an independent author edit invalidates a prepared sparse registration and its source lineage");
    }
    {
        TrackRegistrationFixture<mdl::CameraKey> f;curve_host::TrackAuthoring source(*f.app,f.kind,f.slot);
        for(int invalid:{-1,frame_limits::maximum+1}){
            bool rejected=false;try{source.HasRegistrationChanges({invalid});}catch(const std::invalid_argument&){rejected=true;}
            Check(rejected,"registration-change query rejects out-of-range frames");
        }
        bool rejected=false;try{source.Prepare(f.View(),{21},true);}catch(const std::invalid_argument&){rejected=true;}
        Check(rejected,"explicit registration must identify a row present in the target native track");
        auto target=source.Prepare(f.View(),{21},false);
        Check(target&&target->IsCurrent(),"undo does not force a forward-only frame into the original layer");
        auto* originalPool=f.Pool();f.Pool()=mdl::KeyPoolCreate<mdl::CameraKey>(8);
        Check(!target->IsCurrent()&&!source.IsCurrent(),"replacing the native pool identity invalidates registration authoring");
        mdl::KeyPoolDestroy(f.Pool());f.Pool()=originalPool;
    }
    {
        TrackRegistrationFixture<mdl::CameraKey> f(false);curve_host::TrackAuthoring source(*f.app,f.kind,f.slot);
        Check(!source.HasRegistrationChanges({20}),"a native-only track needs no extra authored layer");
        auto target=source.Prepare(f.View(),{20},true);
        Check(target&&!target->Authored()&&target->IsCurrent(),"explicit registration keeps an unauthored native track unauthored");
        failAfter=0;target->Publish(f.Identity());failAfter=-1;
        Check(!curve_host::Has(f.app.get(),f.kind,f.slot),"native-only registration does not allocate a persistent authoring entry");
    }
}
}
