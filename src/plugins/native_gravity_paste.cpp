#include "native_global_paste.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include <array>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <map>
#include <stdexcept>
namespace mikudancestudio::plugins {
namespace {
using Key = mdl::GravityKey;
struct GravityPose {
    std::array<float,4> values;
    std::uint32_t noise, mode;
    bool operator==(const GravityPose& b) const noexcept {return values==b.values&&noise==b.noise&&mode==b.mode;}
};
GravityPose Read(MMDApp& app) noexcept {
    return {{app.GravityMagnitude(),app.GravityX(),app.GravityY(),app.GravityZ()},app.state.gravityNoise,app.GravityNoiseModeWord()};
}
bool Finite(const GravityPose& p) noexcept {for(auto v:p.values)if(!std::isfinite(v))return false;return true;}
bool Same(Key a,Key b,bool marks=false) noexcept {if(!marks)a.selected=b.selected=0;return !std::memcmp(&a,&b,sizeof(a));}
template<class ReadKey> std::vector<std::pair<unsigned,Key>> Chain(std::size_t capacity,ReadKey read) {
    if(!capacity)throw std::invalid_argument("empty gravity pool");
    std::vector<unsigned char> visited(capacity,0);std::vector<std::pair<unsigned,Key>> result;
    unsigned slot=0,previous=0,frame=0;
    for(std::size_t hops=0;;++hops){
        if(slot>=capacity||visited[slot])throw std::invalid_argument("gravity key cycle");
        const auto key=read(slot);
        if(key.frame>unsigned(frame_limits::maximum)||(!hops&&key.frame)||(hops&&(key.previous!=previous||key.frame<=frame)))
            throw std::invalid_argument("gravity key link");
        if(!std::isfinite(key.acceleration))throw std::invalid_argument("gravity acceleration");
        for(float value:key.direction)if(!std::isfinite(value))throw std::invalid_argument("gravity direction");
        visited[slot]=1;result.push_back({slot,key});if(!key.next)break;previous=slot;slot=key.next;frame=key.frame;
    }
    for(std::size_t i=1;i<capacity;++i)if(!visited[i]&&read(unsigned(i)).frame)throw std::invalid_argument("orphan gravity key");
    return result;
}
template<class ReadKey> GravityPose Sample(std::size_t capacity,unsigned frame,ReadKey read) {
    const auto chain=Chain(capacity,read);std::size_t index=0;
    while(index+1<chain.size()&&chain[index].second.frame<frame)++index;
    const auto& key=chain[index].second;
    GravityPose result{{key.acceleration,key.direction[0],key.direction[1],key.direction[2]},std::uint32_t(key.noise),0};
    std::memcpy(&result.mode,&key.noiseEnabled,sizeof(result.mode));
    if(key.frame>frame){
        if(!index)throw std::invalid_argument("gravity root frame");
        const auto& previous=chain[index-1].second;
        const float t=float(frame-previous.frame)/float(key.frame-previous.frame);
        const auto delta=std::int64_t(key.noise)-previous.noise;
        if(delta<INT32_MIN||delta>INT32_MAX)throw std::invalid_argument("gravity noise difference overflow");
        const float scaled=t*float(delta);
        if(!std::isfinite(scaled)||double(scaled)<INT32_MIN||double(scaled)>INT32_MAX)
            throw std::invalid_argument("gravity noise interpolation overflow");
        const auto noise=std::int64_t(std::int32_t(scaled))+previous.noise;
        if(noise<INT32_MIN||noise>INT32_MAX)throw std::invalid_argument("gravity noise result overflow");
        result.noise=std::uint32_t(std::int32_t(noise));
        result.values[0]=(key.acceleration-previous.acceleration)*t+previous.acceleration;
        for(unsigned i=0;i<3;++i)result.values[i+1]=(key.direction[i]-previous.direction[i])*t+previous.direction[i];
    }
    if(!Finite(result))throw std::invalid_argument("gravity nonfinite interpolation");return result;
}
struct GravityPart final:GlobalPastePart,std::enable_shared_from_this<GravityPart> {
    MMDApp& app;std::uint64_t identity,generation=1;std::size_t capacity;
    curve_host::TrackAuthoring authoring;
    struct Patch {unsigned slot;Key before,after;};std::vector<Patch> patches;
    std::vector<unsigned char> originalMarks;
    std::vector<int> registrationFrames;
    explicit GravityPart(MMDApp& a):app(a),identity(mdl::KeyPoolIdentity(a.GravityKeys())),capacity(mdl::KeyPoolCapacity(a.GravityKeys())),
        authoring(a,curve_host::Kind::Gravity){for(std::size_t i=0;i<capacity;++i)originalMarks.push_back(a.GravityKeys()[i].selected);}
    struct Candidate final:PreparedGlobalPastePart {
        std::shared_ptr<GravityPart> source;bool forward,consumed=false;std::uint64_t generation;
        Key* pool;Key* grown=nullptr;std::size_t capacity,targetCapacity;int frame;GravityPose before,after;
        std::vector<std::pair<unsigned,Key>> dependencies;std::vector<Key> current;
        std::unique_ptr<curve_host::TrackAuthoring::Target> curves;
        Candidate(std::shared_ptr<GravityPart> s,bool direction):source(std::move(s)),forward(direction),generation(source->generation),
            pool(source->app.GravityKeys()),capacity(mdl::KeyPoolCapacity(pool)),targetCapacity(capacity),frame(source->app.CurrentFrame()),
            before(Read(source->app)),after(before){}
        ~Candidate(){if(grown)mdl::KeyPoolDestroy(grown);}
        const Patch* Find(unsigned slot) const noexcept {
            const auto& p=source->patches;const auto it=std::lower_bound(p.begin(),p.end(),slot,[](const Patch& a,unsigned b){return a.slot<b;});
            return it!=p.end()&&it->slot==slot?&*it:nullptr;
        }
        const Key* Target(unsigned slot) const noexcept {
            if(const auto* p=Find(slot))return forward?&p->after:&p->before;
            static const Key empty{};
            return slot<capacity?pool+slot:slot<targetCapacity?&empty:nullptr;
        }
        Key SampleKey(unsigned slot) const noexcept {
            const auto* p=Find(slot);auto key=*Target(slot);
            key.selected=forward?(p?p->after.selected:slot<source->capacity?0:key.selected):
                slot<source->capacity?source->originalMarks[slot]:p?p->before.selected:key.selected;
            return key;
        }
        bool IsCurrent() const noexcept override {
            if(consumed||source->generation!=generation||source->app.GravityKeys()!=pool||
                mdl::KeyPoolIdentity(pool)!=source->identity||mdl::KeyPoolCapacity(pool)!=capacity||
                source->app.CurrentFrame()!=frame||!(Read(source->app)==before)||!curves->IsCurrent())return false;
            for(const auto& key:dependencies)if(!Same(key.second,pool[key.first],true))return false;
            for(std::size_t i=0;i<source->patches.size();++i){const auto slot=source->patches[i].slot;
                if(!Same(current[i],slot<capacity?pool[slot]:Key{},true))return false;}
            return true;
        }
        void PublishOwned() noexcept override {
            if(grown){mdl::KeyPoolCommitGrowth(source->app.GravityKeys(),grown);grown=nullptr;}
            auto* target=source->app.GravityKeys();
            for(const auto& p:source->patches)target[p.slot]=forward?p.after:p.before;
            curves->Publish(target);
            source->app.GravityMagnitude()=after.values[0];source->app.GravityX()=after.values[1];
            source->app.GravityY()=after.values[2];source->app.GravityZ()=after.values[3];
            source->app.state.gravityNoise=after.noise;source->app.GravityNoiseModeWord()=after.mode;
            ++source->generation;consumed=true;
        }
        void PublishSelection() noexcept override {
            auto* target=source->app.GravityKeys();for(const auto& p:source->patches)
                if(forward)target[p.slot].selected=p.after.selected;
                else if(p.slot>=source->capacity)target[p.slot].selected=p.before.selected;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedGlobalPastePart>& out) override {
        out.reset();
        if(mdl::KeyPoolIdentity(app.GravityKeys())!=identity||mdl::KeyPoolCapacity(app.GravityKeys())<capacity||!authoring.IsCurrent())return MMS_E_STALE;
        if(generation==UINT64_MAX)return MMS_E_NOT_READY;
        return Boundary([&]()->MmsResult {
            if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum||!Finite(Read(app)))return MMS_E_ARGUMENT;
            auto candidate=std::make_unique<Candidate>(shared_from_this(),forward);
            candidate->dependencies=Chain(candidate->capacity,[&](unsigned i){return candidate->pool[i];});
            for(const auto& p:patches){const auto live=p.slot<candidate->capacity?candidate->pool[p.slot]:Key{};
                if(!Same(live,forward?p.before:p.after))return MMS_E_STALE;
                candidate->current.push_back(live);candidate->targetCapacity=(std::max)(candidate->targetCapacity,std::size_t(p.slot)+1);}
            if(candidate->targetCapacity>candidate->capacity){
                candidate->targetCapacity=(std::min)(mdl::KeyPoolLimit<Key>(),(std::max)(candidate->targetCapacity,candidate->capacity+candidate->capacity/2+256));
                candidate->grown=mdl::KeyPoolCreate<Key>(candidate->targetCapacity);
                std::memcpy(candidate->grown,candidate->pool,candidate->capacity*sizeof(Key));}
            curve_host::NativeKeyView view{candidate.get(),candidate->targetCapacity,[](const void* p,unsigned i) noexcept->const void*{
                return static_cast<const Candidate*>(p)->Target(i);}};
            candidate->curves=authoring.Prepare(view,registrationFrames,forward);
            candidate->after=Sample(candidate->targetCapacity,unsigned(app.CurrentFrame()),[&](unsigned i){return candidate->SampleKey(i);});
            if(candidate->curves->Authored()){
                std::array<double,12> values{};if(!candidate->curves->Sample(app.CurrentFrame(),values))return MMS_E_ARGUMENT;
                for(unsigned i=0;i<4;++i)candidate->after.values[i]=float(values[i]);
                if(!std::isfinite(values[4])||values[4]<INT32_MIN||values[4]>INT32_MAX||!std::isfinite(values[5])||values[5]<0||values[5]>255)return MMS_E_ARGUMENT;
                candidate->after.noise=std::uint32_t(std::int32_t(values[4]));
                candidate->after.mode=(candidate->after.mode&0xffffff00u)|std::uint8_t(values[5]);
            }
            if(!Finite(candidate->after)||!candidate->IsCurrent())return MMS_E_STALE;
            out=std::move(candidate);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        auto bytes=std::uint64_t(sizeof(*this)+4*sizeof(void*));const auto add=[&](std::uint64_t n){bytes=n>UINT64_MAX-bytes?UINT64_MAX:bytes+n;};
        add(patches.capacity()*sizeof(Patch));add(originalMarks.capacity());add(registrationFrames.capacity()*sizeof(int));add(authoring.RetainedBytes());return bytes;
    }
};
}
MmsResult PlanGravityKeyPastePart(MMDApp& app,const GlobalPasteSource& source,GlobalPasteFamilyResult& out) noexcept {
    out={};if(source.gravity.empty())return MMS_OK;
    return Boundary([&]()->MmsResult {
        if(!mdl::KeyPoolIdentity(app.GravityKeys()))return MMS_E_NOT_READY;
        const auto capacity=mdl::KeyPoolCapacity(app.GravityKeys());auto* keys=app.GravityKeys();
        Chain(capacity,[&](unsigned i){return keys[i];});
        auto part=std::make_shared<GravityPart>(app);std::map<unsigned,GravityPart::Patch> planned;auto targetCapacity=capacity;
        GlobalPasteFamilyResult result;
        const auto read=[&](unsigned slot){const auto found=planned.find(slot);return found!=planned.end()?found->second.after:slot<capacity?keys[slot]:Key{};};
        const auto write=[&](unsigned slot)->Key& {const auto key=slot<capacity?keys[slot]:Key{};auto after=key;after.selected=0;
            return planned.emplace(slot,GravityPart::Patch{slot,key,after}).first->second.after;};
        for(const auto& record:source.gravity){unsigned frame;
            if(!frame_limits::Add(source.frameOffset,record.frame,frame)||!std::isfinite(record.acceleration))return MMS_E_ARGUMENT;
            for(float v:record.direction)if(!std::isfinite(v))return MMS_E_ARGUMENT;
            part->registrationFrames.push_back(int(frame));
            unsigned current=0;while(read(current).frame<frame&&read(current).next)current=read(current).next;
            const auto old=read(current);unsigned destination=current;
            if(old.frame!=frame){destination=1;while(std::size_t(destination)<targetCapacity&&read(destination).frame)++destination;
                if(std::size_t(destination)>=mdl::KeyPoolLimit<Key>())return MMS_E_MEMORY;
                targetCapacity=(std::max)(targetCapacity,std::size_t(destination)+1);auto& added=write(destination);
                if(old.frame<frame){write(current).next=destination;added.previous=current;added.next=0;}
                else{write(old.previous).next=destination;added.previous=old.previous;write(current).previous=destination;added.next=current;}
                added.frame=frame;result.maximumInsertedFrame=(std::max)(result.maximumInsertedFrame,frame);
            }
            auto& added=write(destination);added.acceleration=record.acceleration;std::memcpy(added.direction,record.direction,sizeof(added.direction));
            added.noise=record.noise;added.noiseEnabled=record.noiseEnabled;added.selected=1;
        }
        Chain(targetCapacity,read);
        std::sort(part->registrationFrames.begin(),part->registrationFrames.end());
        part->registrationFrames.erase(std::unique(part->registrationFrames.begin(),part->registrationFrames.end()),part->registrationFrames.end());
        result.dataChanged=part->authoring.HasRegistrationChanges(part->registrationFrames);
        for(const auto& p:planned){result.dataChanged|=!Same(p.second.before,p.second.after);part->patches.push_back(p.second);}
        result.part=std::move(part);out=std::move(result);return MMS_OK;
    });
}
}
#endif
