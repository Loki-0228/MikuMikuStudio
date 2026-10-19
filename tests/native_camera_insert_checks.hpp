// Exact native-camera subdivision, without opening windows or touching files.
#include "mikumikustudio/frame_limits.hpp"
static void CheckNativeCameraInsertion() {
    namespace ac=animation_curve;
    const auto stage=[](const char* name){std::fprintf(stderr,"STAGE camera insertion %s\n",name);std::fflush(stderr);};
    stage("fixture");
    struct Fixture {
        std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
        mdl::CameraKey* pool=mdl::KeyPoolCreate<mdl::CameraKey>(16);
        Fixture(){
            Check(pool!=nullptr,"allocate a managed camera pool for authoring identity checks");
            ::new(static_cast<void*>(&app->state)) MMDAppState{};app->CameraKeys()=pool;
            for(int i=0;i<3;++i){auto& key=pool[i];key.frame=i*40;key.previous=i?i-1:0;key.next=i<2?i+1:0;
                key.parentModel=-1;key.fov=35+i*15;key.distance=-25.f-i*6;
                for(int c=0;c<3;++c){key.eye[c]=float(i*7+c);key.target[c]=float(i+c)*.125f;}
                for(int lane=0;lane<6;++lane){key.interpolation[0][lane]=57;key.interpolation[1][lane]=0;
                    key.interpolation[2][lane]=44;key.interpolation[3][lane]=127;}}
        }
        ~Fixture(){curve_host::Reset(app.get());mdl::KeyPoolDestroy(app->CameraKeys());app->CameraKeys()=nullptr;}
    } f;
    std::wstring error;std::vector<curve_editor::Channel> channels;
    Check(curve_host::Read(*f.app,curve_host::Kind::Camera,0,channels,error),"read exact camera insertion fixture");
    const auto original=channels;
    const auto sameSamples=[&](const std::vector<curve_editor::Channel>& current){
        for(std::size_t c=0;c<8;++c)for(int tick=0;tick<=800;++tick){const double frame=tick/10.0;
            const auto before=original[c].sample(original[c],frame),after=current[c].sample(current[c],frame);
            Check(after.value==before.value,"camera subdivision preserves every fractional native float sample exactly");
            Check(std::abs(after.derivative-before.derivative)<=1e-12*std::max(1.0,std::abs(before.derivative)),
                "camera subdivision preserves the original native curve speed");}
    };
    stage("position and rotation subdivision");
    for(int frame:{17,39,7}){std::size_t index=99;
        Check(curve_host::InsertPreservingShape(channels,0,frame,index,error)&&channels[0].track.keys[index].frame==frame,
            "insert position keys through exact native helper");sameSamples(channels);}
    for(int frame:{11,39}){std::size_t index=99;
        Check(curve_host::InsertPreservingShape(channels,3,frame,index,error),"insert linked camera rotation keys");
        for(std::size_t c=3;c<6;++c)Check(std::any_of(channels[c].track.keys.begin(),channels[c].track.keys.end(),
            [&](const auto& key){return key.frame==frame;}),"all shared rotation lanes acquire the inserted frame");
        sameSamples(channels);}
    Check(!channels[1].preservedCameraIntervals.empty(),"peer camera lanes retain native evaluation before shared proxy rows split them");
    const auto count=channels[0].track.keys.size();std::size_t existing=99;
    Check(curve_host::InsertPreservingShape(channels,0,17,existing,error)&&channels[0].track.keys.size()==count,
        "selecting an existing native insertion is a no-op");
    stage("frame-rate mapping");
    {
        auto retimed=channels;
        for(std::size_t c=0;c<retimed.size();++c){
            for(auto& key:retimed[c].track.keys){key.frame*=2;key.inSlope*=.5;key.outSlope*=.5;}
            Check(curve_host::RetimePreservedChannel(channels[c],retimed[c],error),"map exact intervals through the scalar frame-rate converter");
        }
        for(std::size_t c=0;c<8;++c)for(int tick=0;tick<=1600;++tick){
            const auto before=original[c].sample(original[c],tick/20.0),after=retimed[c].sample(retimed[c],tick/10.0);
            Check(after.value==before.value,"double-rate conversion retains newly introduced half-frame values exactly");
            Check(std::abs(after.derivative-before.derivative*.5)<=1e-12*std::max(1.0,std::abs(before.derivative)),
                "frame-rate conversion scales the exact native speed with its time domain");
        }
        auto merged=retimed[0];merged.track.keys.erase(merged.track.keys.begin()+1);
        Check(!curve_host::RetimePreservedChannel(channels[0],merged,error),"merging exact interval boundaries requires explicit baking");
    }
    stage("publication and PMM reload");
    Check(curve_host::Write(*f.app,curve_host::Kind::Camera,0,channels,error),"publish exact subdivided camera");
    std::vector<curve_editor::Channel> stored;
    Check(curve_host::Read(*f.app,curve_host::Kind::Camera,0,stored,error),"read subdivided authoring");sameSamples(stored);
    pmm_extension::Document document;curve_host::Capture(*f.app,document);
    Check(pmm_extension::Find(document,"org.mikumikustudio.animation_curves.v1")&&
        pmm_extension::Find(document,"org.mikumikustudio.camera_intervals.v1"),"exact intervals occupy a separate optional PMM record");
    curve_host::Reset(f.app.get());curve_host::Load(*f.app,document);
    Check(curve_host::Read(*f.app,curve_host::Kind::Camera,0,stored,error),"reload persisted exact native intervals");sameSamples(stored);
#if !MMD_ORIGINAL_PROFILE
    stage("history retiming candidate");
    {
        curve_host::TrackAuthoring authoring(*f.app,curve_host::Kind::Camera);
        std::size_t slices=0;for(const auto& channel:stored)slices+=channel.preservedCameraIntervals.size();
        Check(authoring.RetainedBytes()>=slices*sizeof(curve_editor::NativeCameraInterval),"history budgets include exact camera slice storage");
        const auto detached=stored;detached.front().sample(detached.front(),12.5);
        Check(authoring.CapturedCurrent(),"detached camera reads leave captured authoring current");
        std::vector<int> frames;authoring.RetimingFrames(frames);
        std::vector<mdl::CameraKey> retimed;
        for(std::uint32_t i=0;;){retimed.push_back(f.app->CameraKeys()[i]);if(!f.app->CameraKeys()[i].next)break;i=f.app->CameraKeys()[i].next;}
        for(auto& key:retimed)key.frame*=2;
        curve_host::NativeKeyView view{&retimed,retimed.size(),[](const void* context,std::uint32_t i)noexcept->const void*{
            const auto& keys=*static_cast<const std::vector<mdl::CameraKey>*>(context);return i<keys.size()?&keys[i]:nullptr;}};
        std::vector<std::pair<int,int>> mapping;for(int frame:frames)mapping.push_back({frame,frame*2});
        auto target=authoring.PrepareRetimed(view,mapping,true);
        Check(target&&target->IsCurrent(),"prepare private retiming of exact native camera slices");
        for(int tick=0;tick<=800;++tick){std::array<double,12> values{};
            Check(target->Sample(tick/5.0,values),"sample exact camera retiming candidate");
            for(std::size_t c=0;c<8;++c)Check(values[c]==original[c].sample(original[c],tick/10.0).value,
                "retiming maps preserved source time without changing float easing");}
        Check(authoring.IsCurrent(),"private retiming does not publish or invalidate live authoring");
    }
#endif
    // The detached pre-edit read remains independent of all later host writes.
    Check(original[0].track.keys.size()==3&&original[0].preservedCameraIntervals.empty(),"old channel snapshot retains its own native topology");
    stage("export proxies and preparation budget");
    std::vector<curve_editor::Channel> motion;
    Check(curve_host::PrepareMotionChannels(stored,motion,error),"prepare existing VMD writer's integer proxy channels");
    for(std::size_t c=0;c<8;++c){Check(!motion[c].sample&&motion[c].preservedCameraIntervals.empty()&&motion[c].track.keys.size()==81,
        "export channels use ordinary scalar keys without changing VMD structures");
        for(int frame=0;frame<=80;++frame)Check(ac::Evaluate(motion[c].track,frame).value==original[c].sample(original[c],frame).value,
            "integer export proxy equals original camera sampling");}
    stored[0].selected=false;
    Check(curve_host::PrepareMotionChannels(stored,motion,error)&&!motion[0].preservedCameraIntervals.empty(),
        "hidden camera channels are not needlessly baked for export");
    Check(curve_host::PrepareMotionChannels(stored,motion,error,false)&&motion[0].preservedCameraIntervals.empty(),
        "frame-rate conversion can request all channels regardless of visibility");
    {
        auto enormous=channels[0];
        for(auto& key:enormous.track.keys)key.frame=int(std::int64_t(key.frame)*frame_limits::maximum/80);
        Check(curve_host::RetimePreservedChannel(channels[0],enormous,error),"prepare sparse exact camera at the maximum legal frame");
        const auto originalOutput=motion[0].track.keys.size();
        Check(!curve_host::PrepareMotionChannels({enormous},motion,error)&&error.find(L"预算")!=std::wstring::npos&&
            motion[0].track.keys.size()==originalOutput,"extremely long native proxy export refuses its cumulative sample budget before allocation");
        Check(enormous.preservedCameraIntervals.size()==channels[0].preservedCameraIntervals.size(),
            "sampling budget refusal preserves source exact authoring");
    }
    auto edited=stored;edited[0].track.keys[1].value+=.25;
    const double at=(edited[0].track.keys[0].frame+edited[0].track.keys[1].frame)*.5;
    Check(edited[0].sample(edited[0],at).value==ac::Evaluate(edited[0].track,at).value,
        "intentional endpoint edits release their original native slice");
    stage("malformed record rejection");
    auto malformed=document;auto* record=const_cast<pmm_extension::Record*>(pmm_extension::Find(malformed,"org.mikumikustudio.camera_intervals.v1"));
    record->value[4]=99;curve_host::Load(*f.app,malformed);
    Check(curve_host::Read(*f.app,curve_host::Kind::Camera,0,stored,error)&&stored[0].preservedCameraIntervals.empty(),
        "unknown exact interval versions cannot partially install native slices");
    for(std::size_t length:{std::size_t(0),std::size_t(8),std::size_t(27),record->value.size()-1}){
        malformed=document;record=const_cast<pmm_extension::Record*>(pmm_extension::Find(malformed,"org.mikumikustudio.camera_intervals.v1"));
        record->value.resize(length);curve_host::Load(*f.app,malformed);
        Check(curve_host::Read(*f.app,curve_host::Kind::Camera,0,stored,error)&&stored[0].preservedCameraIntervals.empty(),
            "truncated exact interval records fail transactionally");}
    stage("allocation failures including diagnostic allocation");
    {
        auto candidate=original;std::wstring emptyError;std::size_t index=77;
        failAfter=0;const bool result=curve_host::InsertPreservingShape(candidate,3,17,index,emptyError);failAfter=-1;
        Check(!result&&index==77&&candidate[3].track.keys.size()==3&&candidate[3].preservedCameraIntervals.empty(),
            "failed subdivision and failed diagnostic allocation both return safely without changing input");
        std::vector<curve_editor::Channel> output;emptyError.clear();
        failAfter=0;const bool exported=curve_host::PrepareMotionChannels(channels,output,emptyError);failAfter=-1;
        Check(!exported&&output.empty(),"export preparation survives an allocation failure with no diagnostic capacity");
        auto retimed=channels[0];emptyError.clear();
        failAfter=0;const bool mapped=curve_host::RetimePreservedChannel(channels[0],retimed,emptyError);failAfter=-1;
        Check(!mapped&&retimed.preservedCameraIntervals==channels[0].preservedCameraIntervals,
            "retiming preparation survives an allocation failure with no diagnostic capacity");
    }
    error.reserve(256);unsigned failures=0;
    for(long allocation=0;allocation<256;++allocation){auto candidate=original;std::size_t index=77;
        std::fprintf(stderr,"STAGE camera insertion allocation %ld\n",allocation);std::fflush(stderr);
        failAfter=allocation;const bool result=curve_host::InsertPreservingShape(candidate,3,17,index,error);failAfter=-1;
        if(result){Check(failures>5,"native subdivision allocation sweep exercised private preparation failures");break;}
        ++failures;Check(index==77&&candidate[3].track.keys.size()==3&&candidate[3].preservedCameraIntervals.empty()&&
            candidate[3].nativeControls==original[3].nativeControls,"failed native subdivision leaves all input channels and output index intact");
        Check(allocation<255,"native subdivision allocation sweep reaches success");}
    stage("long native channel");
    {
        auto channel=original[0];channel.track.keys.resize(4097);
        channel.nativeControls.assign(4097,{57,0,44,127});channel.controlGeometry.resize(4097);
        const auto outWeight=original[0].track.keys[0].outWeight,inWeight=original[0].track.keys[1].inWeight;
        for(std::size_t i=0;i<channel.track.keys.size();++i){auto& key=channel.track.keys[i];
            key.frame=int(i*40);key.value=double(i);key.inSlope=key.outSlope=0;key.inWeight=inWeight;key.outWeight=outWeight;
            key.interpolation=ac::Interpolation::Bezier;key.tangent=ac::Tangent::Free;
            channel.controlGeometry[i]={outWeight,0,inWeight,0};}
        std::vector<curve_editor::Channel> longTrack{channel};std::size_t inserted=0;
        Check(curve_host::InsertPreservingShape(longTrack,0,21,inserted,error),"prepare thousands of exact native intervals");
        const auto& values=longTrack[0].preservedCameraIntervals;
        Check(values.size()==4097&&std::is_sorted(values.begin(),values.end(),[](const auto& a,const auto& b){return a.first<b.first;}),
            "long exact camera interval tables remain ordered for logarithmic sampling");
        for(int i=0;i<4096;++i)Check(longTrack[0].sample(longTrack[0],i*40+15.25).value==channel.sample(channel,i*40+15.25).value,
            "long channel sampling resolves the correct preserved interval");
    }
    std::puts("PASS exact native camera subdivision, peer lanes, PMM records and export proxies");
}
