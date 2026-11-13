// Included after native_curve_write_checks.inc in the full-profile history suite.
#include <algorithm>
#include <cmath>
#include <limits>
namespace {
namespace global_copy_authoring_checks {
namespace ch = curve_host;
namespace ac = animation_curve;
using Channel = curve_editor::Channel;
using Channels = std::vector<Channel>;
using Target = ch::CopyAuthoringTarget;
using Targets = std::vector<Target>;
using Copies = std::vector<ch::CopyAuthoring>;
using Sampler = ac::Sample (*)(const Channel&, double);

Targets All() {
    return {{ch::Kind::Accessory,254},{ch::Kind::Gravity,0},{ch::Kind::Camera,0},
            {ch::Kind::Light,0},{ch::Kind::Accessory,0},{ch::Kind::Shadow,0}};
}
bool Same(const ac::Key& a,const ac::Key& b) {
    return a.frame==b.frame&&a.value==b.value&&a.inSlope==b.inSlope&&a.outSlope==b.outSlope&&
        a.inWeight==b.inWeight&&a.outWeight==b.outWeight&&a.interpolation==b.interpolation&&a.tangent==b.tangent;
}
bool Same(const curve_editor::NativeCameraInterval& a,const curve_editor::NativeCameraInterval& b) {
    // List every field explicitly, including the two timing flags.
    return a.first==b.first&&a.last==b.last&&a.sourceFirst==b.sourceFirst&&a.sourceLast==b.sourceLast&&
        a.sampleFirst==b.sampleFirst&&a.sampleLast==b.sampleLast&&a.sourceValueFirst==b.sourceValueFirst&&
        a.sourceValueLast==b.sourceValueLast&&a.controls==b.controls&&a.geometry==b.geometry&&
        a.oneFrameHold==b.oneFrameHold&&a.transportTiming==b.transportTiming;
}
bool Same(const Channels& a,const Channels& b) {
    if(a.size()!=b.size())return false;
    for(std::size_t c=0;c<a.size();++c) {
        const auto& x=a[c];const auto& y=b[c];
        if(x.id!=y.id||x.label!=y.label||x.unit!=y.unit||x.base!=y.base||x.minimum!=y.minimum||
           x.maximum!=y.maximum||x.selected!=y.selected||x.group!=y.group||x.interpolationGroup!=y.interpolationGroup||
           x.track.discrete!=y.track.discrete||x.track.keys.size()!=y.track.keys.size()||
           x.nativeControls!=y.nativeControls||x.controlGeometry!=y.controlGeometry||
           x.collections.size()!=y.collections.size()||x.preservedCameraIntervals.size()!=y.preservedCameraIntervals.size()||
           bool(x.sample)!=bool(y.sample))return false;
        if(x.sample) {
            const auto p=x.sample.target<Sampler>(),q=y.sample.target<Sampler>();
            if(!p||!q||*p!=*q)return false;
        }
        for(std::size_t i=0;i<x.track.keys.size();++i)if(!Same(x.track.keys[i],y.track.keys[i]))return false;
        for(std::size_t i=0;i<x.collections.size();++i)
            if(x.collections[i].id!=y.collections[i].id||x.collections[i].label!=y.collections[i].label)return false;
        for(std::size_t i=0;i<x.preservedCameraIntervals.size();++i)
            if(!Same(x.preservedCameraIntervals[i],y.preservedCameraIntervals[i]))return false;
    }
    return true;
}
bool Same(const Copies& a,const Copies& b) {
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)
        if(a[i].kind!=b[i].kind||a[i].slot!=b[i].slot||!Same(a[i].channels,b[i].channels))return false;
    return true;
}
Copies Sentinel() {
    Copies out(1);out[0].kind=ch::Kind::Gravity;out[0].slot=73;
    out[0].channels.resize(1);out[0].channels[0].track.keys.push_back({});return out;
}
struct Model : CurveWriteFixture {
    mdl::AccessoryRecord lastAccessory{};
    template<class K> static void Enlarge(K*& pool) {
        auto* grown=mdl::KeyPoolCreate<K>(64);
        std::memcpy(grown,pool,mdl::KeyPoolCapacity(pool)*sizeof(K));
        mdl::KeyPoolDestroy(pool);pool=grown;
    }
    Model() {
        Enlarge(app->CameraKeys());Enlarge(app->LightKeys());Enlarge(app->ShadowKeys());
        Enlarge(app->GravityKeys());Enlarge(app->AccessoryKeys(0));
        lastAccessory=accessory;lastAccessory.order=1;
        app->AccessoryKeys(254)=mdl::KeyPoolCreate<mdl::AccessoryKey>(64);
        std::memcpy(app->AccessoryKeys(254),app->AccessoryKeys(0),64*sizeof(mdl::AccessoryKey));
        app->AccessorySlot(254)=&lastAccessory;app->AccessoryTrackCursor(254)=2;app->AccessoryTrackActive(254)=1;
        for(int i=0;i<3;++i) {
            auto& key=app->CameraKeys()[i];key.frame=unsigned(i*40);
            for(int c=0;c<3;++c){key.eye[c]=float(i*7+c);key.target[c]=float(i+c)*.125f;}
            for(int lane=0;lane<6;++lane) {
                key.interpolation[0][lane]=57;key.interpolation[1][lane]=0;
                key.interpolation[2][lane]=44;key.interpolation[3][lane]=127;
            }
        }
    }
    ~Model() {
        ch::Reset(app.get());mdl::KeyPoolDestroy(app->AccessoryKeys(254));app->AccessorySlot(254)=nullptr;
    }
    std::uint64_t Digest() const {
        auto hash=CurveWriteFixture::Digest();
        const auto add=[&](const void* data,std::size_t size) {
            const auto* bytes=static_cast<const unsigned char*>(data);
            while(size--){hash^=*bytes++;hash*=1099511628211ull;}
        };
        add(&lastAccessory,sizeof lastAccessory);
        add(app->AccessoryKeys(254),mdl::KeyPoolCapacity(app->AccessoryKeys(254))*sizeof(mdl::AccessoryKey));
        const auto revision=app->SceneRevision();add(&revision,sizeof revision);return hash;
    }
};
struct Observation {
    ch::AuthoringTable raw;
    std::vector<std::unique_ptr<ch::TrackAuthoring>> authors;
    std::uint64_t native,historyRevision=0,historyBytes,historyEpoch;
    std::size_t undo,redo;
    explicit Observation(Model& f):native(f.Digest()),historyBytes(f.history.RetainedBytes()),
        historyEpoch(f.history.NativeEpoch()),undo(f.history.UndoCount()),redo(f.history.RedoCount()) {
        std::wstring error;Check(ch::CaptureAuthoring(*f.app,raw,error),"global copy captures read-only raw author oracle");
        for(const auto target:All())authors.push_back(std::make_unique<ch::TrackAuthoring>(*f.app,target.kind,target.slot));
        Check(f.history.Revision(historyRevision)==MMS_OK,"global copy captures history revision");
    }
    void Expect(Model& f) const {
        std::uint64_t revision=0;Check(f.history.Revision(revision)==MMS_OK,"global copy reads history revision");
        Check(f.Digest()==native&&f.history.UndoCount()==undo&&f.history.RedoCount()==redo&&revision==historyRevision&&
              f.history.RetainedBytes()==historyBytes&&f.history.NativeEpoch()==historyEpoch,
            "global copy preserves complete native pools unused bytes pose selection cursors app state and history");
        Check(ch::AuthoringUnchanged(*f.app,raw),"global copy never changes raw authors baseline or fingerprint");
        for(const auto& author:authors)Check(author->CapturedCurrent(),"global copy never refreshes or prunes a live author revision");
    }
};
Copies Capture(Model& f,const Targets& targets) {
    auto out=Sentinel();std::wstring error=L"previous diagnostic";
    Check(ch::CaptureCopyAuthoring(*f.app,targets,out,error)&&error.empty(),"global copy captures requested complete authors");
    return out;
}
void Refused(Model& f,const Targets& targets) {
    const Observation before(f);auto out=Sentinel();std::wstring error;
    Check(!ch::CaptureCopyAuthoring(*f.app,targets,out,error)&&out.empty()&&!error.empty(),
        "global copy rejects invalid request atomically and clears populated output");before.Expect(f);
}
Copies Oracle(Model& f,const Targets& targets) {
    ch::AuthoringTable raw;std::wstring error;std::vector<ch::AuthoringEntry> entries;
    Check(ch::CaptureAuthoring(*f.app,raw,error)&&ch::AuthoringEntries(raw,entries),
        "global copy oracle reconciles a private table without reading through the live author path");
    Copies out;
    for(const auto target:targets) {
        if(std::any_of(out.begin(),out.end(),[&](const auto& e){return e.kind==target.kind&&e.slot==target.slot;}))continue;
        for(const auto& entry:entries)if(entry.kind==target.kind&&entry.slot==target.slot)
            out.push_back({entry.kind,entry.slot,entry.channels});
    }
    Check(ch::AuthoringUnchanged(*f.app,raw),"global copy oracle leaves original author baseline intact");return out;
}
Channels Seed(Model& f,Target target,bool exact=true) {
    Channels channels;std::wstring error;
    Check(ch::Read(*f.app,target.kind,target.slot,channels,error),"global copy seed reads native channels");
    if(target.kind==ch::Kind::Camera&&exact) {
        std::size_t index=0;
        for(int frame:{17,7})Check(ch::InsertPreservingShape(channels,0,frame,index,error),"seed exact crossed-X camera subdivisions");
        Check(ch::InsertPreservingShape(channels,3,11,index,error),"seed linked rotation and independent camera lane frames");
        auto retimed=channels;
        for(std::size_t c=0;c<channels.size();++c) {
            for(auto& key:retimed[c].track.keys){key.frame*=2;key.inSlope*=.5;key.outSlope*=.5;}
            Check(ch::RetimePreservedChannel(channels[c],retimed[c],error),"seed absolute retimed native source slices");
            for(auto& slice:retimed[c].preservedCameraIntervals)if(c%2){slice.oneFrameHold=false;slice.transportTiming=true;}
        }
        channels=std::move(retimed);
    } else if(target.kind!=ch::Kind::Camera) {
        for(std::size_t c=0;c<channels.size();++c) {
            auto& track=channels[c].track;track.keys[1].frame=5+int(c);
            for(std::size_t i=0;i<track.keys.size();++i) {
                auto& key=track.keys[i];key.tangent=ac::Tangent::Free;
                if(!track.discrete) {
                    key.interpolation=ac::Interpolation::Bezier;
                    key.inWeight=.21+.01*i;key.outWeight=.31+.01*i;
                    key.inSlope=.001*(c+1);key.outSlope=-.002*(c+1);
                }
            }
        }
    }
    for(std::size_t c=0;c<channels.size();++c) {
        auto& lane=channels[c];lane.selected=c%2==0;lane.group=int(c%4);
        lane.collections={{"copy-root",L"author root"},{"lane-"+std::to_string(c),L"author lane"}};
    }
    Check(ch::Write(*f.app,target.kind,target.slot,channels,error),"global copy seed writes actual stored author");
    return channels;
}
void SeedAll(Model& f) {for(const auto target:All())Seed(f,target);}
template<class K> void SelectOne(K* pool) {
    const auto selected=pool[0].next;
    for(std::size_t i=0;i<mdl::KeyPoolCapacity(pool);++i)pool[i].selected=std::uint8_t(i==selected);
}
void InstallAuthor(Model& f,Target target,const Channels& channels) {
    // Public retime-table exchange is deliberately used only while seeding.
    ch::AuthoringTable raw,replacement;std::wstring error;std::vector<ch::AuthoringEntry> entries;
    Check(ch::CaptureAuthoring(*f.app,raw,error)&&ch::AuthoringEntries(raw,entries),"read seed table through public API");
    bool found=false;
    for(auto& entry:entries)if(entry.kind==target.kind&&entry.slot==target.slot){entry.channels=channels;found=true;}
    Check(found&&ch::RebuildAuthoring(*f.app,entries,replacement,error),"seed author using public detached table builder");
    ch::SwapAuthoring(*f.app,replacement);
}
void FieldsOrderAndHistory() {
    Model f;SeedAll(f);
    // Keep both history directions populated before any capture under test.
    auto edit=Oracle(f,{{ch::Kind::Light,0}})[0].channels;edit[0].track.keys[1].value+=.5;
    f.Commit(f.Plan(1,edit));edit[0].track.keys[1].value+=.5;f.Commit(f.Plan(1,edit));
    Check(f.history.History(false)==MMS_OK&&f.history.UndoCount()==1&&f.history.RedoCount()==1,
        "global copy seed retains a real undo record and redo record");
    SelectOne(f.app->CameraKeys());SelectOne(f.app->LightKeys());SelectOne(f.app->ShadowKeys());
    SelectOne(f.app->GravityKeys());SelectOne(f.app->AccessoryKeys(0));SelectOne(f.app->AccessoryKeys(254));
    auto targets=All();targets.insert(targets.begin()+2,{ch::Kind::Accessory,254});
    targets.push_back({ch::Kind::Camera,0});targets.push_back({ch::Kind::Accessory,0});
    const Observation before(f);const auto expected=Oracle(f,targets),out=Capture(f,targets);
    Check(out.size()==6&&Same(out,expected),"all five families and slot 254 retain full fields in stable first-request order");
    const std::array<std::size_t,5> counts{11,6,2,6,12};
    for(const auto& entry:out) {
        Check(entry.channels.size()==counts[unsigned(entry.kind)],"global copy includes every lane including discrete and hidden lanes");
        for(const auto& lane:entry.channels)Check(lane.track.keys.front().frame==0&&lane.track.keys.back().frame>=30,
            "global copy keeps full absolute authors beyond selected native frames and playhead");
    }
    Check(out[2].channels[0].track.keys.size()!=out[2].channels[1].track.keys.size(),
        "camera copy preserves independent sparse lanes rather than filling native proxy rows");
    before.Expect(f);
    // A second call must not refresh a fingerprint as an invisible side effect.
    Check(Same(Capture(f,targets),out),"repeated global copy is stable");before.Expect(f);
}
void NoAllocation(Model& f,const Targets& targets) {
    auto out=Sentinel();std::wstring error=L"old";bool ok=false,threw=false;
    failAfter=0;
    try{ok=ch::CaptureCopyAuthoring(*f.app,targets,out,error);}catch(...){threw=true;}
    const auto remaining=failAfter;failAfter=-1;
    Check(ok&&!threw&&out.empty()&&error.empty()&&remaining==0,
        "empty missing and stale author copies allocate nothing and clear old output and diagnostics");
}
void MissingStaleAndIndices() {
    for(bool authored:{false,true}) {
        Model f;if(authored)SeedAll(f);const Observation before(f);
        NoAllocation(f,{});
        auto* camera=f.app->CameraKeys();auto* light=f.app->LightKeys();auto* shadow=f.app->ShadowKeys();
        auto* gravity=f.app->GravityKeys();auto* accessory=f.app->AccessoryKeys(0);auto* last=f.app->AccessoryKeys(254);
        f.app->CameraKeys()=reinterpret_cast<mdl::CameraKey*>(std::uintptr_t(1));
        f.app->LightKeys()=reinterpret_cast<mdl::LightKey*>(std::uintptr_t(1));
        f.app->ShadowKeys()=reinterpret_cast<mdl::SelfShadowKey*>(std::uintptr_t(1));
        f.app->GravityKeys()=reinterpret_cast<mdl::GravityKey*>(std::uintptr_t(1));
        f.app->AccessoryKeys(0)=reinterpret_cast<mdl::AccessoryKey*>(std::uintptr_t(1));
        f.app->AccessoryKeys(254)=reinterpret_cast<mdl::AccessoryKey*>(std::uintptr_t(1));
        const auto* first=reinterpret_cast<const unsigned char*>(&f.app->state);
        const std::vector<unsigned char> poisoned(first,first+sizeof f.app->state);
        NoAllocation(f,All());NoAllocation(f,{});
        Check(std::memcmp(poisoned.data(),&f.app->state,poisoned.size())==0,"missing and stale capture never touches app state or poisoned pools");
        f.app->CameraKeys()=camera;f.app->LightKeys()=light;f.app->ShadowKeys()=shadow;f.app->GravityKeys()=gravity;
        f.app->AccessoryKeys(0)=accessory;f.app->AccessoryKeys(254)=last;
        before.Expect(f);
        if(authored)Check(Same(Capture(f,All()),Oracle(f,All())),"restored identities recover unpruned original authors");
    }
    Model f;Seed(f,{ch::Kind::Camera,0});const Observation before(f);
    f.app->LightKeys()[0].next=999;
    const Observation malformedMissing(f);
    NoAllocation(f,{{ch::Kind::Light,0},{ch::Kind::Light,0},{ch::Kind::Accessory,1}});
    Check(Capture(f,{{ch::Kind::Light,0},{ch::Kind::Camera,0},{ch::Kind::Accessory,1}}).size()==1,
        "missing authors are omitted even when their unselected native payload is corrupt");malformedMissing.Expect(f);
    f.app->LightKeys()[0].next=1;before.Expect(f);
    for(int kind=0;kind<4;++kind)for(int slot:{-1,1,254,255})Refused(f,{{ch::Kind::Camera,0},{ch::Kind(kind),slot}});
    for(int slot:{-1,255,(std::numeric_limits<int>::max)()})Refused(f,{{ch::Kind::Accessory,slot}});
    for(int kind:{-1,5,(std::numeric_limits<int>::max)()})Refused(f,{{ch::Kind::Camera,0},{ch::Kind(kind),0}});
    Seed(f,{ch::Kind::Accessory,0});const Observation accessoryBefore(f);
    for(auto* identity:std::array<mdl::AccessoryRecord*,2>{nullptr,&f.lastAccessory}) {
        f.app->AccessorySlot(0)=identity;NoAllocation(f,{{ch::Kind::Accessory,0}});
    }
    f.app->AccessorySlot(0)=&f.accessory;accessoryBefore.Expect(f);
}
void EditNative(Model& f,Target target) {
    switch(target.kind) {
    case ch::Kind::Camera:f.app->CameraKeys()[1].eye[0]+=1.25f;break;
    case ch::Kind::Light:f.app->LightKeys()[1].direction[0]+=.25f;break;
    case ch::Kind::Shadow:f.app->ShadowKeys()[1].distance+=.025f;break;
    case ch::Kind::Gravity:f.app->GravityKeys()[1].acceleration+=2.f;break;
    case ch::Kind::Accessory:f.app->AccessoryKeys(target.slot)[1].position[0]+=3.f;break;
    }
}
void NativePendingEdits() {
    for(const auto target:All()) {
        Model f;SeedAll(f);const auto original=Capture(f,All());
        const auto saved=f.Native(unsigned(target.kind));
        std::vector<mdl::AccessoryKey> last(f.app->AccessoryKeys(254),f.app->AccessoryKeys(254)+64);
        EditNative(f,target);const Observation before(f);const auto out=Capture(f,All());
        Check(!Same(original,out)&&Same(out,Oracle(f,All())),"pending native edits are reconciled only in detached copy for every family");
        before.Expect(f);
        switch(target.kind) {
        case ch::Kind::Camera:std::memcpy(f.app->CameraKeys(),saved.data(),saved.size());break;
        case ch::Kind::Light:std::memcpy(f.app->LightKeys(),saved.data(),saved.size());break;
        case ch::Kind::Shadow:std::memcpy(f.app->ShadowKeys(),saved.data(),saved.size());break;
        case ch::Kind::Gravity:std::memcpy(f.app->GravityKeys(),saved.data(),saved.size());break;
        case ch::Kind::Accessory:
            if(target.slot==254)std::memcpy(f.app->AccessoryKeys(254),last.data(),last.size()*sizeof(last[0]));
            else std::memcpy(f.app->AccessoryKeys(0),saved.data(),saved.size());break;
        }
        Check(Same(Capture(f,All()),original),"restoring native bytes recovers original full author without baseline refresh");
        for(const auto& author:before.authors)Check(author->CapturedCurrent(),"private reconciliation never advances source revision");
    }
}
void NativeTopologyAndTiming() {
    for(unsigned variant=0;variant<3;++variant) {
        Model f;SeedAll(f);const auto original=Capture(f,All());
        if(variant==0) {
            auto* keys=f.app->CameraKeys();unsigned previous=0;
            while(keys[previous].next&&keys[keys[previous].next].frame<20)previous=keys[previous].next;
            const auto next=keys[previous].next;
            Check(next&&keys[previous].frame<20&&keys[next].frame>20,"native insertion seed brackets a missing absolute frame");
            keys[63]=keys[previous];keys[63].frame=20;keys[63].previous=previous;keys[63].next=next;
            keys[63].eye[0]+=.75f;keys[previous].next=63;keys[next].previous=63;
        } else if(variant==1) {
            auto* keys=f.app->ShadowKeys();const auto removed=keys[0].next,next=keys[removed].next;
            Check(next!=0,"native deletion seed has a following row");keys[0].next=next;keys[next].previous=0;
        } else f.app->CameraKeys()[1].interpolation[1][0]=31;
        const Observation before(f);const auto expected=Oracle(f,All()),out=Capture(f,All());
        Check(!Same(out,original)&&Same(out,expected),"private reconcile follows native insertion deletion and timing edits");
        before.Expect(f);
    }
}
template<class K> void BreakLinks(K* keys,unsigned variant) {
    const auto next=keys[0].next;Check(next!=0,"bad-chain seed has a successor");
    if(variant==0)keys[0].next=unsigned(mdl::KeyPoolCapacity(keys));
    if(variant==1)keys[next].next=next;
    if(variant==2)keys[next].previous=next;
    if(variant==3)keys[0].frame=1;
    if(variant==4)keys[next].frame=keys[0].frame;
    if(variant==7)keys[0].previous=1;
}
void BadChains() {
    for(const auto target:All())for(unsigned variant=0;variant<8;++variant) {
        Model f;SeedAll(f);
        switch(target.kind) {
        case ch::Kind::Camera:BreakLinks(f.app->CameraKeys(),variant);if(variant==5||variant==6)f.app->CameraKeys()[1].eye[0]=variant==5?std::numeric_limits<float>::quiet_NaN():1e13f;break;
        case ch::Kind::Light:BreakLinks(f.app->LightKeys(),variant);if(variant==5||variant==6)f.app->LightKeys()[1].color[0]=variant==5?std::numeric_limits<float>::quiet_NaN():2.f;break;
        case ch::Kind::Shadow:BreakLinks(f.app->ShadowKeys(),variant);if(variant==5||variant==6)f.app->ShadowKeys()[1].distance=variant==5?std::numeric_limits<float>::quiet_NaN():2.f;break;
        case ch::Kind::Gravity:BreakLinks(f.app->GravityKeys(),variant);if(variant==5||variant==6)f.app->GravityKeys()[1].acceleration=variant==5?std::numeric_limits<float>::quiet_NaN():1e13f;break;
        case ch::Kind::Accessory:BreakLinks(f.app->AccessoryKeys(target.slot),variant);if(variant==5||variant==6)f.app->AccessoryKeys(target.slot)[1].scale=variant==5?std::numeric_limits<float>::quiet_NaN():-1.f;break;
        }
        const Target good{target.kind==ch::Kind::Light?ch::Kind::Camera:ch::Kind::Light,0};
        const Observation before(f);Check(Capture(f,{good}).size()==1,"unrequested corrupt authored chain does not block good family");
        before.Expect(f);Refused(f,{good,target});
    }
    Model f;SeedAll(f);f.app->CameraKeys()[1].interpolation[0][0]=255;
    Refused(f,{{ch::Kind::Light,0},{ch::Kind::Camera,0}});
}
struct OpaqueSampler {
    // Counters distinguish inspecting a callable from copying or invoking it.
    std::shared_ptr<std::array<unsigned,2>> counters;
    explicit OpaqueSampler(std::shared_ptr<std::array<unsigned,2>> value):counters(std::move(value)){}
    OpaqueSampler(const OpaqueSampler& other):counters(other.counters){++(*counters)[0];}
    ac::Sample operator()(const Channel& channel,double frame)const {
        ++(*counters)[1];return ac::Evaluate(channel.track,frame);
    }
};
void InvalidAuthorsAndOpaque() {
    for(unsigned variant=0;variant<12;++variant) {
        Model f;auto channels=Seed(f,{ch::Kind::Camera,0});Seed(f,{ch::Kind::Light,0});
        auto& lane=channels[0];
        if(variant==0)lane.id="unknown";
        if(variant==1)lane.track.discrete=true;
        if(variant==2)lane.track.keys[1].value=std::numeric_limits<double>::quiet_NaN();
        if(variant==3)lane.track.keys[1].outSlope=std::numeric_limits<double>::infinity();
        if(variant==4)lane.track.keys[1].inWeight=0;
        if(variant==5)lane.track.keys[1].interpolation=ac::Interpolation(99);
        if(variant==6)lane.nativeControls[1][0]=128;
        if(variant==7)lane.controlGeometry[1][0]=std::numeric_limits<double>::quiet_NaN();
        Check(!lane.preservedCameraIntervals.empty(),"invalid-author seed has preserved intervals");
        if(variant==8)lane.preservedCameraIntervals[0].sampleLast=std::numeric_limits<double>::infinity();
        if(variant==9)lane.preservedCameraIntervals[0].controls[0]=128;
        if(variant==10)lane.preservedCameraIntervals[0].geometry[0]+=1;
        if(variant==11){lane.preservedCameraIntervals[0].oneFrameHold=false;lane.preservedCameraIntervals[0].transportTiming=false;}
        InstallAuthor(f,{ch::Kind::Camera,0},channels);
        const Observation before(f);Check(Capture(f,{{ch::Kind::Light,0}}).size()==1,"unrequested invalid author does not poison good author");
        before.Expect(f);Refused(f,{{ch::Kind::Light,0},{ch::Kind::Camera,0}});
    }
    for(const auto kind:{ch::Kind::Camera,ch::Kind::Light}) {
        Model f;auto channels=Seed(f,{kind,0});auto counters=std::make_shared<std::array<unsigned,2>>();
        channels[0].sample=OpaqueSampler(counters);
        std::wstring error;Check(ch::Write(*f.app,kind,0,channels,error),"public unbound write can seed an opaque stored sampler");
        const Observation before(f);auto out=Sentinel();const auto counts=*counters;
        Check(!ch::CaptureCopyAuthoring(*f.app,{{kind,0}},out,error)&&out.empty()&&!error.empty(),"opaque sampler refuses atomically");
        Check(*counters==counts,"opaque refusal neither invokes nor copies owner-capturing callback");before.Expect(f);
    }
}
void PendingLegacyBatches() {
    for(const auto target:All()) {
        Model f,other;SeedAll(f);SeedAll(other);
        {
            ch::LegacyRegistrationBatch outer(f.app.get());
            ch::LegacyRegistrationBatch unrelated(other.app.get());
            {
                ch::LegacyRegistrationBatch nested(f.app.get());
                ch::NotifyLegacyRegistered(f.app.get(),target.kind,target.slot,0);
                const Observation before(f),otherBefore(other);
                Refused(f,{target,target});Refused(f,{target});
                const Target good{target.kind==ch::Kind::Light?ch::Kind::Camera:ch::Kind::Light,0};
                Check(Capture(f,{good}).size()==1,"unrelated family remains copyable during pending legacy batch");
                Check(Capture(other,{target}).size()==1,"another application's batch is independent");
                before.Expect(f);otherBefore.Expect(other);
            }
            Refused(f,{target});
        }
        const Observation after(f);Check(Capture(f,{target}).size()==1,"completed legacy registration permits copy again");after.Expect(f);
    }
}
ac::Sample Sample(const Channel& channel,double frame) {
    return channel.sample?channel.sample(channel,frame):ac::Evaluate(channel.track,frame);
}
void RawCameraControls() {
    for(bool endpoints:{false,true}) {
        Model f;
        for(int component=0;component<4;++component)for(int lane=0;lane<6;++lane)
            f.app->CameraKeys()[0].interpolation[component][lane]=255;
        if(endpoints)for(int i=1;i<3;++i)for(int lane=0;lane<6;++lane) {
            f.app->CameraKeys()[i].interpolation[0][lane]=127;
            f.app->CameraKeys()[i].interpolation[2][lane]=0;
        }
        const auto raw=Seed(f,{ch::Kind::Camera,0},false);const Observation before(f);
        const auto out=Capture(f,{{ch::Kind::Camera,0}});
        Check(out.size()==1&&Same(out[0].channels,raw),"raw crossed and endpoint X controls plus unsampled root bytes remain verbatim");
        for(std::size_t c=0;c<8;++c)for(int tick=0;tick<=800;++tick) {
            const auto expected=Sample(raw[c],tick/10.0),actual=Sample(out[0].channels[c],tick/10.0);
            Check(actual.value==expected.value&&actual.derivative==expected.derivative,"raw camera copy preserves every fractional native sample");
        }
        before.Expect(f);
    }
}
void FractionalCameraAndLifetime() {
    Copies held,saved;std::vector<ac::Sample> samples;
    {
        Model f;Channels raw;std::wstring error;
        Check(ch::Read(*f.app,ch::Kind::Camera,0,raw,error),"seed reads original crossed-X camera samples");
        const auto canonical=raw[0].sample.target<Sampler>();Check(canonical&&*canonical,"camera source uses canonical pure function");
        SeedAll(f);const Observation before(f);held=Capture(f,All());saved=held;
        const auto camera=std::find_if(held.begin(),held.end(),[](const auto& e){return e.kind==ch::Kind::Camera;});
        Check(camera!=held.end()&&camera->channels.size()==11,"owned camera snapshot includes all transform and discrete lanes");
        bool heldFlag=false,continuousFlag=false,transportFlag=false,sourceDomain=false;
        for(std::size_t c=0;c<8;++c) {
            const auto& lane=camera->channels[c];const auto sampler=lane.sample.target<Sampler>();
            Check(sampler&&*sampler==*canonical,"owned camera keeps only the canonical sampler function");
            Check(!lane.nativeControls.empty()&&!lane.controlGeometry.empty()&&!lane.preservedCameraIntervals.empty(),
                "owned camera retains raw controls scalar geometry and exact native slices");
            for(const auto& interval:lane.preservedCameraIntervals) {
                heldFlag|=interval.oneFrameHold;continuousFlag|=!interval.oneFrameHold;
                transportFlag|=interval.transportTiming;sourceDomain|=interval.first!=interval.sampleFirst||interval.last!=interval.sampleLast;
            }
            for(int tick=0;tick<=1600;++tick) {
                const auto old=Sample(raw[c],tick/20.0),current=Sample(lane,tick/10.0);
                Check(current.value==old.value&&std::abs(current.derivative-old.derivative*.5)<=1e-12*std::max(1.0,std::abs(old.derivative)),
                    "fractional camera copy preserves native float sampling crossed controls and retimed derivative");
            }
        }
        Check(heldFlag&&continuousFlag&&transportFlag&&sourceDomain,"camera fixtures exercise both timing flags and distinct absolute source domains");
        for(const auto& entry:held)for(const auto& lane:entry.channels)for(double frame:{0.0,2.25,13.75,39.9,80.125,159.5,200.0})
            samples.push_back(Sample(lane,frame));
        before.Expect(f);
        auto changed=Oracle(f,{{ch::Kind::Light,0}})[0].channels;changed[0].track.keys[1].value+=5;
        Check(ch::Write(*f.app,ch::Kind::Light,0,changed,error),"seed later source author replacement");
        f.app->CameraKeys()[1].eye[0]+=100;ch::Reset(f.app.get());
        Check(Same(held,saved),"owned copies survive source author edits native edits and Reset");
    }
    Check(Same(held,saved),"all five owned author families survive complete source destruction");
    std::size_t i=0;
    for(const auto& entry:held)for(const auto& lane:entry.channels)for(double frame:{0.0,2.25,13.75,39.9,80.125,159.5,200.0}) {
        const auto actual=Sample(lane,frame),expected=samples[i++];
        Check(actual.value==expected.value&&actual.derivative==expected.derivative,"snapshot callbacks and scalar samples remain usable after source destruction");
    }
    held[0].channels[0].track.keys[0].value+=1;
    Check(!Same(held,saved),"returned channel and key vectors own independent storage");
    auto camera=std::find_if(held.begin(),held.end(),[](const auto& e){return e.kind==ch::Kind::Camera;});
    const auto pristine=std::find_if(saved.begin(),saved.end(),[](const auto& e){return e.kind==ch::Kind::Camera;});
    camera->channels[0].nativeControls[0][0]+=1;
    camera->channels[0].controlGeometry[0][0]+=1;
    camera->channels[0].preservedCameraIntervals[0].geometry[0]+=1;
    camera->channels[0].collections[0].label=L"independent result";
    Check(camera->channels[0].nativeControls!=pristine->channels[0].nativeControls&&
          camera->channels[0].controlGeometry!=pristine->channels[0].controlGeometry&&
          camera->channels[0].preservedCameraIntervals!=pristine->channels[0].preservedCameraIntervals&&
          camera->channels[0].collections[0].label!=pristine->channels[0].collections[0].label,
          "camera timing arrays exact intervals and collection strings own independent storage");
}
void AllocationFailures(const Targets& targets,bool reconcile) {
    Model f;SeedAll(f);if(reconcile)for(const auto target:targets)EditNative(f,target);
    const Observation before(f);const auto expected=Oracle(f,targets);
    auto measured=Sentinel();std::wstring error;constexpr long allowance=1000000;
    bool success=false,unexpected=false;failAfter=allowance;
    try{success=ch::CaptureCopyAuthoring(*f.app,targets,measured,error);}catch(...){unexpected=true;}
    const auto spent=allowance-failAfter;failAfter=-1;
    Check(success&&!unexpected&&spent>0&&spent<allowance&&Same(measured,expected),"measure every allocation in successful full author capture");before.Expect(f);
    for(long limit=0;limit<=spent;++limit) {
        auto out=Sentinel();std::wstring detail;bool memory=false,other=false,returned=false;failAfter=limit;
        try{returned=ch::CaptureCopyAuthoring(*f.app,targets,out,detail);}
        catch(const std::bad_alloc&){memory=true;}catch(...){other=true;}
        const auto remaining=failAfter;failAfter=-1;
        if(limit<spent)Check(memory&&!other&&!returned&&remaining==0&&out.empty(),
            "every measured allocation failure propagates bad_alloc clears output and never returns partial authors");
        else Check(returned&&!memory&&!other&&detail.empty()&&Same(out,expected),"exact allocation budget reaches full success after exhaustive OOM sweep");
        before.Expect(f);
    }
}
}
void NativeGlobalCopyAuthoringChecks() {
    using namespace global_copy_authoring_checks;
    FieldsOrderAndHistory();MissingStaleAndIndices();NativePendingEdits();NativeTopologyAndTiming();BadChains();
    InvalidAuthorsAndOpaque();PendingLegacyBatches();RawCameraControls();FractionalCameraAndLifetime();
    for(bool reconcile:{false,true}) {
        for(const auto target:All())AllocationFailures({target,target},reconcile);
        auto targets=All();targets.push_back({ch::Kind::Camera,0});AllocationFailures(targets,reconcile);
    }
}
}
