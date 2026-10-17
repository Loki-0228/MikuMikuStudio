#include "native_accessory_registration.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_global_registration.hpp"
#include "native_global_paste.hpp"
#include "scene_object_history.hpp"
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include <array>
#include <cmath>
#include <map>
#include <set>

namespace mikudancestudio::plugins {
namespace {
using Key = mdl::AccessoryKey;
struct Patch { unsigned slot; Key before, after; };
struct Dependency { unsigned slot; Key key; };
bool Equal(Key a, Key b, bool selection = true) noexcept {
    if (!selection) a.selected = b.selected = 0;
    return !std::memcmp(&a, &b, sizeof(Key));
}
Key Pose(const mdl::AccessoryRecord& object) noexcept {
    Key key{}; key.visible = object.visible; key.shadowEnabled = object.shadowEnabled;
    key.parentModel = object.parentModel; key.parentBone = object.parentBone;
    std::memcpy(key.position, object.position, sizeof(key.position));
    std::memcpy(key.rotation, object.rotation, sizeof(key.rotation));
    key.scale = object.scale; key.opacity = object.opacity; return key;
}
void ApplyPose(mdl::AccessoryRecord& object, const Key& key) noexcept {
    object.visible = key.visible; object.shadowEnabled = key.shadowEnabled;
    object.parentModel = key.parentModel; object.parentBone = key.parentBone;
    std::memcpy(object.position, key.position, sizeof(key.position));
    std::memcpy(object.rotation, key.rotation, sizeof(key.rotation));
    object.scale = key.scale; object.opacity = key.opacity;
}
void Fill(Key& key, const Key& pose) noexcept {
    key.visible = pose.visible; key.shadowEnabled = pose.shadowEnabled;
    key.parentModel = pose.parentModel; key.parentBone = pose.parentBone;
    std::memcpy(key.position, pose.position, sizeof(key.position));
    std::memcpy(key.rotation, pose.rotation, sizeof(key.rotation));
    key.scale = pose.scale; key.opacity = pose.opacity; key.selected = 1;
}
bool Finite(const Key& key) noexcept {
    for (int i = 0; i < 3; ++i) if (!std::isfinite(key.position[i]) || !std::isfinite(key.rotation[i])) return false;
    return std::isfinite(key.scale) && std::isfinite(key.opacity);
}
template<class Read> std::vector<Dependency> Chain(std::size_t capacity, Read read) {
    std::vector<Dependency> result;
    std::vector<unsigned char> visited(capacity, 0);
    unsigned index = 0, previous = 0, frame = 0;
    for (std::size_t hop = 0; hop < capacity; ++hop) {
        if (index >= capacity || visited[index]) throw std::invalid_argument("accessory key outside pool or cycle");
        const auto key = read(index);
        if (!Finite(key) || key.frame > unsigned(frame_limits::maximum) || key.previous >= capacity || key.next >= capacity ||
            (!hop && key.frame) || (hop && (key.previous != previous || key.frame <= frame)))
            throw std::invalid_argument("invalid accessory key chain");
        visited[index] = 1; result.push_back({index, key});
        if (!key.next) {
            for (std::size_t i = 1; i < capacity; ++i)
                if (!visited[i] && read(unsigned(i)).frame) throw std::invalid_argument("orphan accessory key");
            return result;
        }
        previous = index; frame = key.frame; index = key.next;
    }
    throw std::invalid_argument("unterminated accessory key chain");
}
void Add(std::uint64_t& bytes, std::uint64_t count, std::uint64_t stride) noexcept {
    if (!stride) return;
    if (count > (UINT64_MAX - bytes) / stride) bytes = UINT64_MAX;
    else bytes += std::uint64_t(count) * stride;
}
struct Parent {
    unsigned slot, boneCount;
    unsigned char* model;
    mdl::BoneRecord* bones;
    std::shared_ptr<RetainedSceneObject> owner;
    bool Current(MMDApp& app) const noexcept {
        return owner->Present() && app.ModelSlot(slot) == model && SceneObjectPresent(model, owner->Id()) &&
            mdl::Mdl(model)->boneTable == bones && mdl::Mdl(model)->boneCount == boneCount;
    }
};
void CaptureParent(MMDApp& app, const Key& key, std::vector<Parent>& parents) {
    if (key.parentModel == -1) return;
    if (key.parentModel < 0 || key.parentModel >= kModelSlotCount || key.parentBone < 0)
        throw std::invalid_argument("invalid accessory parent");
    auto* model = app.ModelSlot(key.parentModel);
    if (!model || !mdl::Mdl(model)->boneTable || unsigned(key.parentBone) >= mdl::Mdl(model)->boneCount)
        throw std::invalid_argument("missing accessory parent bone");
    const auto found = std::find_if(parents.begin(), parents.end(), [&](const Parent& parent) { return parent.slot == unsigned(key.parentModel); });
    if (found == parents.end()) parents.push_back({unsigned(key.parentModel), mdl::Mdl(model)->boneCount, model,
        mdl::Mdl(model)->boneTable, RetainSceneObject(app, MMS_OBJECT_MODEL, unsigned(key.parentModel))});
}
struct Growth {
    MMDApp& app; unsigned slot; Key* source; Key* grown = nullptr;
    std::size_t capacity, targetCapacity; std::uint64_t identity;
    Growth(MMDApp& a, unsigned s, std::size_t required) : app(a), slot(s), source(a.AccessoryKeys(s)),
        capacity(mdl::KeyPoolCapacity(source)), targetCapacity(capacity), identity(mdl::KeyPoolIdentity(source)) {
        if (!identity) throw std::invalid_argument("borrowed accessory key pool");
        if (required <= capacity) return;
        if (required > mdl::KeyPoolLimit<Key>()) throw std::bad_alloc();
        targetCapacity = (std::min)(mdl::KeyPoolLimit<Key>(), (std::max)(required, capacity + capacity / 2 + 256));
        grown = mdl::KeyPoolCreate<Key>(targetCapacity);
        std::memcpy(grown, source, capacity * sizeof(Key));
    }
    ~Growth() { if (grown) mdl::KeyPoolDestroy(grown); }
    bool Current() const noexcept {
        return app.AccessoryKeys(slot) == source && mdl::KeyPoolCapacity(source) == capacity && mdl::KeyPoolIdentity(source) == identity;
    }
    void Publish() noexcept {
        if (grown) { mdl::KeyPoolCommitGrowth(app.AccessoryKeys(slot), grown); grown = nullptr; }
    }
};
Key NativePose(const std::vector<Dependency>& chain, unsigned frame) {
    const auto upper = std::lower_bound(chain.begin(), chain.end(), frame,
        [](const Dependency& entry, unsigned value) { return entry.key.frame < value; });
    const auto& next = upper == chain.end() ? chain.back().key : upper->key;
    Key result{};
    if (upper == chain.end() || next.frame == frame) Fill(result, next);
    else {
        if (upper == chain.begin()) throw std::invalid_argument("accessory track without root");
        const auto& previous = (upper - 1)->key; Fill(result, previous);
        // ApplyAccessoryTrack divides two float frame deltas, then performs
        // its multiply/add in float. Position and rotation are step values.
        const float t = float(frame - previous.frame) / float(next.frame - previous.frame);
        result.scale = previous.scale + t * (next.scale - previous.scale);
        result.opacity = previous.opacity + t * (next.opacity - previous.opacity);
    }
    result.selected = 0; return result;
}
Key AuthoredPose(const std::array<double,12>& values) {
    Key result{};
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i < 3; ++i) { result.position[i] = float(values[i]); result.rotation[i] = float(values[i + 3] * pi / 180); }
    result.scale = float(values[6]); result.opacity = float(values[7]); result.visible = std::uint8_t(values[8]);
    result.shadowEnabled = std::uint8_t(values[9]); result.parentModel = int(values[10]); result.parentBone = int(values[11]);
    return result;
}
struct Registration final : HistoryChange, std::enable_shared_from_this<Registration> {
    MMDApp& app; unsigned slot; mdl::AccessoryRecord* object;
    std::shared_ptr<RetainedSceneObject> owner;
    std::uint64_t identity, generation = 1; std::size_t capacity;
    int frame; Key beforePose, afterPose;
    std::uint32_t lastBefore, lastAfter;
    bool published = false;
    unsigned registeredSlot = 0;
    std::vector<Patch> patches;
    std::vector<Dependency> original;
    std::vector<Parent> parents;
    GlobalKeySelection selection;
    curve_host::TrackAuthoring curves;
    Registration(MMDApp& a, unsigned s, const Key& pose, std::vector<Dependency> chain)
        : app(a), slot(s), object(a.AccessorySlot(s)), owner(RetainSceneObject(a, MMS_OBJECT_ACCESSORY, s)),
          identity(mdl::KeyPoolIdentity(a.AccessoryKeys(s))), capacity(mdl::KeyPoolCapacity(a.AccessoryKeys(s))),
          frame(a.CurrentFrame()), beforePose(Pose(*object)), afterPose(pose), lastBefore(a.LastRegisteredFrame()), lastAfter(lastBefore),
          original(std::move(chain)), selection(a), curves(a, curve_host::Kind::Accessory, int(s)) {
        CaptureParent(a, beforePose, parents); CaptureParent(a, afterPose, parents);
        for (const auto& entry : original) CaptureParent(a, entry.key, parents);
    }
    bool Structure() const noexcept {
        if (!owner || !owner->Present() || app.AccessorySlot(slot) != object || !SceneObjectPresent(object, owner->Id()) ||
            mdl::KeyPoolIdentity(app.AccessoryKeys(slot)) != identity || mdl::KeyPoolCapacity(app.AccessoryKeys(slot)) < capacity) return false;
        for (const auto& parent : parents) if (!parent.Current(app)) return false;
        return true;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<Registration> source;
        bool forward, first, consumed = false;
        std::uint64_t generation;
        int frame;
        Key beforePose, afterPose;
        Key* pool; std::size_t capacity;
        std::vector<Key> current, target;
        std::vector<Parent> parents;
        std::unique_ptr<Growth> growth;
        std::unique_ptr<GlobalKeySelection::Candidate> selection;
        std::unique_ptr<curve_host::TrackAuthoring::Target> curves;
        Candidate(std::shared_ptr<Registration> s, bool f)
            : source(std::move(s)), forward(f), first(!source->published), generation(source->generation), frame(source->app.CurrentFrame()),
              beforePose(Pose(*source->object)), afterPose(beforePose), pool(source->app.AccessoryKeys(source->slot)), capacity(mdl::KeyPoolCapacity(pool)),
              current(pool, pool + capacity), target(current) {}
        bool IsCurrent() const noexcept override {
            if (consumed || source->generation != generation || !source->Structure() || first == source->published ||
                source->app.CurrentFrame() != frame || !Equal(beforePose, Pose(*source->object)) ||
                source->app.LastRegisteredFrame() != (forward ? source->lastBefore : source->lastAfter) ||
                !growth || !growth->Current() || !selection || !selection->IsCurrent() || !curves || !curves->IsCurrent()) return false;
            if (source->app.AccessoryKeys(source->slot) != pool || mdl::KeyPoolCapacity(pool) != capacity ||
                std::memcmp(pool, current.data(), capacity * sizeof(Key))) return false;
            for (const auto& parent : parents) if (!parent.Current(source->app)) return false;
            return true;
        }
        void Publish() noexcept override {
            // All participants are checked before the first write. Their
            // publication methods intentionally do not revalidate old pools.
            if (!IsCurrent()) std::terminate();
            growth->Publish(); selection->Publish();
            auto* keys = source->app.AccessoryKeys(source->slot);
            for (const auto& patch : source->patches) keys[patch.slot] = forward ? patch.after : patch.before;
            if (forward) keys[source->registeredSlot].selected = 1;
            ApplyPose(*source->object, afterPose);
            source->app.LastRegisteredFrame() = forward ? source->lastAfter : source->lastBefore;
            curves->Publish(source->object);
            source->published = true; ++source->generation; consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();
        if (!Structure() || !curves.IsCurrent()) return MMS_E_STALE;
        if (generation == UINT64_MAX) return MMS_E_NOT_READY;
        if (!published && (!forward || app.CurrentFrame() != frame || !Equal(beforePose, Pose(*object)))) return MMS_E_STALE;
        if (app.CurrentFrame() < 0 || app.CurrentFrame() > frame_limits::maximum) return MMS_E_ARGUMENT;
        return Boundary([&]() -> MmsResult {
            if (app.LastRegisteredFrame() != (forward ? lastBefore : lastAfter)) return MMS_E_STALE;
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            if (!published) for (const auto& entry : original)
                if (entry.slot >= candidate->capacity || !Equal(candidate->pool[entry.slot], entry.key, false)) return MMS_E_STALE;
            std::size_t required = candidate->capacity;
            for (const auto& patch : patches) {
                const auto live = patch.slot < candidate->capacity ? candidate->pool[patch.slot] : Key{};
                if (!Equal(live, forward ? patch.before : patch.after, false)) return MMS_E_STALE;
                required = (std::max)(required, std::size_t(patch.slot) + 1);
            }
            candidate->growth = std::make_unique<Growth>(app, slot, required);
            candidate->target.resize(required);
            for (auto& key : candidate->target) key.selected = 0;
            if (!forward) for (const auto& entry : original) candidate->target[entry.slot].selected = entry.key.selected;
            for (const auto& patch : patches) candidate->target[patch.slot] = forward ? patch.after : patch.before;
            if (forward) candidate->target[registeredSlot].selected = 1;
            const auto chain = Chain(candidate->target.size(), [&](unsigned index) { return candidate->target[index]; });
            for (const auto& entry : chain) CaptureParent(app, entry.key, candidate->parents);
            try { candidate->selection = selection.Prepare(forward, !published); }
            catch (const std::invalid_argument&) { return MMS_E_STALE; }
            if (!candidate->selection) return MMS_E_STALE;
            curve_host::NativeKeyView view;
            view.context = &candidate->target; view.capacity = candidate->target.size();
            view.key = [](const void* context, std::uint32_t index) noexcept -> const void* {
                const auto& keys = *static_cast<const std::vector<Key>*>(context); return index < keys.size() ? &keys[index] : nullptr;
            };
            candidate->curves = curves.Prepare(view);
            if (!candidate->curves) return MMS_E_STALE;
            if (candidate->first) candidate->afterPose = afterPose;
            else if (candidate->curves->Authored()) {
                std::array<double,12> values{};
                if (!candidate->curves->Sample(candidate->frame, values)) return MMS_E_ARGUMENT;
                candidate->afterPose = AuthoredPose(values);
            } else candidate->afterPose = NativePose(chain, unsigned(candidate->frame));
            if (!Finite(candidate->afterPose)) return MMS_E_ARGUMENT;
            CaptureParent(app, candidate->afterPose, candidate->parents);
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        std::uint64_t bytes = sizeof(*this) + 4 * sizeof(void*);
        Add(bytes, patches.capacity(), sizeof(Patch)); Add(bytes, original.capacity(), sizeof(Dependency));
        Add(bytes, parents.capacity(), sizeof(Parent)); Add(bytes, 1, selection.RetainedBytes()); Add(bytes, 1, curves.RetainedBytes());
        return bytes;
    }
};
struct AccessoryPaste final : GlobalPastePart, std::enable_shared_from_this<AccessoryPaste> {
    MMDApp& app;unsigned slot;mdl::AccessoryRecord* object;
    std::shared_ptr<RetainedSceneObject> owner;
    std::uint64_t identity,generation=1;std::size_t capacity;
    int frame;Key initialPose;bool published=false;
    std::vector<Patch> patches;std::vector<Dependency> original;
    std::vector<Parent> parents;
    curve_host::TrackAuthoring curves;
    AccessoryPaste(MMDApp& a,unsigned s,std::vector<Dependency> chain)
        :app(a),slot(s),object(a.AccessorySlot(s)),owner(RetainSceneObject(a,MMS_OBJECT_ACCESSORY,s)),
         identity(mdl::KeyPoolIdentity(a.AccessoryKeys(s))),capacity(mdl::KeyPoolCapacity(a.AccessoryKeys(s))),
         frame(a.CurrentFrame()),initialPose(Pose(*object)),original(std::move(chain)),
         curves(a,curve_host::Kind::Accessory,int(s)) {
        CaptureParent(a,initialPose,parents);
        for(const auto& entry:original)CaptureParent(a,entry.key,parents);
    }
    bool Structure()const noexcept {
        if(!owner||!owner->Present()||app.AccessorySlot(slot)!=object||!SceneObjectPresent(object,owner->Id())||
            mdl::KeyPoolIdentity(app.AccessoryKeys(slot))!=identity||mdl::KeyPoolCapacity(app.AccessoryKeys(slot))<capacity)return false;
        for(const auto& parent:parents)if(!parent.Current(app))return false;
        return true;
    }
    struct Candidate final : PreparedGlobalPastePart {
        std::shared_ptr<AccessoryPaste> source;bool forward,owned=false,finished=false;
        std::uint64_t generation;int frame;Key beforePose,afterPose;
        Key* pool;std::size_t capacity;
        std::vector<Key> current,target;std::vector<Parent> parents;
        std::unique_ptr<Growth> growth;
        std::unique_ptr<curve_host::TrackAuthoring::Target> curves;
        Candidate(std::shared_ptr<AccessoryPaste> s,bool f)
            :source(std::move(s)),forward(f),generation(source->generation),frame(source->app.CurrentFrame()),
             beforePose(Pose(*source->object)),afterPose(beforePose),pool(source->app.AccessoryKeys(source->slot)),
             capacity(mdl::KeyPoolCapacity(pool)),current(pool,pool+capacity),target(current) {}
        bool IsCurrent()const noexcept override {
            if(owned||finished||source->generation!=generation||!source->Structure()||source->app.CurrentFrame()!=frame||
                !Equal(beforePose,Pose(*source->object))||source->app.AccessoryKeys(source->slot)!=pool||
                mdl::KeyPoolCapacity(pool)!=capacity||std::memcmp(pool,current.data(),capacity*sizeof(Key))||
                !growth||!growth->Current()||!curves||!curves->IsCurrent())return false;
            for(const auto& parent:parents)if(!parent.Current(source->app))return false;
            return true;
        }
        void PublishOwned()noexcept override {
            // The aggregate validated every part before the first mutation.
            // Never recheck shared selection or another participant here.
            growth->Publish();auto* keys=source->app.AccessoryKeys(source->slot);
            for(const auto& patch:source->patches) {
                const auto selected=keys[patch.slot].selected;
                keys[patch.slot]=forward?patch.after:patch.before;keys[patch.slot].selected=selected;
            }
            ApplyPose(*source->object,afterPose);curves->Publish(source->object);
            source->published=true;++source->generation;owned=true;
        }
        void PublishSelection()noexcept override {
            auto* keys=source->app.AccessoryKeys(source->slot);
            for(const auto& patch:source->patches)
                if(forward||patch.slot>=source->capacity)
                    keys[patch.slot].selected=forward?patch.after.selected:patch.before.selected;
            finished=true;
        }
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedGlobalPastePart>& out)override {
        out.reset();
        if(!Structure()||!curves.IsCurrent())return MMS_E_STALE;
        if(generation==UINT64_MAX)return MMS_E_NOT_READY;
        if(!published&&(!forward||app.CurrentFrame()!=frame||!Equal(initialPose,Pose(*object))))return MMS_E_STALE;
        if(app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
        return Boundary([&]()->MmsResult {
            auto candidate=std::make_unique<Candidate>(shared_from_this(),forward);
            if(!published)for(const auto& entry:original)
                if(entry.slot>=candidate->capacity||!Equal(candidate->pool[entry.slot],entry.key,false))return MMS_E_STALE;
            std::size_t required=candidate->capacity;
            for(const auto& patch:patches) {
                const auto live=patch.slot<candidate->capacity?candidate->pool[patch.slot]:Key{};
                if(!Equal(live,forward?patch.before:patch.after,false))return MMS_E_STALE;
                required=(std::max)(required,std::size_t(patch.slot)+1);
            }
            candidate->growth=std::make_unique<Growth>(app,slot,required);candidate->target.resize(required);
            for(const auto& patch:patches)candidate->target[patch.slot]=forward?patch.after:patch.before;
            const auto chain=Chain(required,[&](unsigned index){return candidate->target[index];});
            for(const auto& entry:chain)CaptureParent(app,entry.key,candidate->parents);
            curve_host::NativeKeyView view;view.context=&candidate->target;view.capacity=required;
            view.key=[](const void* context,std::uint32_t index)noexcept->const void* {
                const auto& keys=*static_cast<const std::vector<Key>*>(context);return index<keys.size()?&keys[index]:nullptr;
            };
            candidate->curves=curves.Prepare(view);if(!candidate->curves)return MMS_E_STALE;
            if(candidate->curves->Authored()) {
                std::array<double,12> values{};if(!candidate->curves->Sample(candidate->frame,values))return MMS_E_ARGUMENT;
                candidate->afterPose=AuthoredPose(values);
            } else candidate->afterPose=NativePose(chain,unsigned(candidate->frame));
            if(!Finite(candidate->beforePose)||!Finite(candidate->afterPose))return MMS_E_ARGUMENT;
            CaptureParent(app,candidate->afterPose,candidate->parents);
            if(!candidate->IsCurrent())return MMS_E_STALE;
            out=std::move(candidate);return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes()const noexcept override {
        std::uint64_t bytes=sizeof(*this)+4*sizeof(void*);
        Add(bytes,patches.capacity(),sizeof(Patch));Add(bytes,original.capacity(),sizeof(Dependency));
        Add(bytes,parents.capacity(),sizeof(Parent));Add(bytes,1,curves.RetainedBytes());return bytes;
    }
};
}
MmsResult PlanAccessoryKeyPastePart(MMDApp& app,unsigned slot,const GlobalPasteSource& source,GlobalPasteFamilyResult& out)noexcept {
    out={};
    return Boundary([&]()->MmsResult {
        if(slot>=255||!app.AccessorySlot(slot)||app.PlaybackActive()||app.FrameStepPlayback()||
            app.state.recordPlaybackStartPending||app.SceneMutationInProgress())return MMS_E_NOT_READY;
        if(source.frameOffset>unsigned(frame_limits::maximum)||app.CurrentFrame()<0||app.CurrentFrame()>frame_limits::maximum)return MMS_E_ARGUMENT;
        auto* pool=app.AccessoryKeys(slot);const auto capacity=mdl::KeyPoolCapacity(pool);
        if(!mdl::KeyPoolIdentity(pool))return MMS_E_NOT_READY;
        auto chain=Chain(capacity,[&](unsigned index){return pool[index];});
        if(!Finite(Pose(*app.AccessorySlot(slot))))return MMS_E_ARGUMENT;
        auto change=std::make_shared<AccessoryPaste>(app,slot,std::move(chain));
        std::map<unsigned,Patch> patches;
        const auto read=[&](unsigned index) {
            const auto entry=patches.find(index);return entry!=patches.end()?entry->second.after:index<capacity?pool[index]:Key{};
        };
        const auto write=[&](unsigned index)->Key& {
            auto entry=patches.find(index);
            if(entry==patches.end()) {const auto key=index<capacity?pool[index]:Key{};
                entry=patches.emplace(index,Patch{index,key,key}).first;entry->second.after.selected=0;}
            return entry->second.after;
        };
        GlobalPasteFamilyResult result;bool any=false;
        for(const auto& record:source.accessories) {
            if(record.slot!=slot)continue;any=true;unsigned frame;
            if(!frame_limits::Add(int(source.frameOffset),record.frameOffset,frame))return MMS_E_ARGUMENT;
            Key pose{};pose.visible=record.visible;pose.shadowEnabled=record.shadowEnabled;
            pose.parentModel=record.parentModel;pose.parentBone=record.parentBone;
            std::memcpy(pose.position,record.position,sizeof(pose.position));std::memcpy(pose.rotation,record.rotation,sizeof(pose.rotation));
            pose.scale=record.scale;pose.opacity=record.opacity;
            if(!Finite(pose))return MMS_E_ARGUMENT;CaptureParent(app,pose,change->parents);
            unsigned current=0;std::size_t hops=0;
            while(read(current).frame<frame&&read(current).next) {
                if(++hops>=capacity+source.accessories.size())return MMS_E_ARGUMENT;current=read(current).next;
            }
            const auto old=read(current);unsigned target=current;
            if(old.frame!=frame) {
                std::size_t free=1;while(free<mdl::KeyPoolLimit<Key>()&&read(unsigned(free)).frame)++free;
                if(free>=mdl::KeyPoolLimit<Key>())return MMS_E_MEMORY;target=unsigned(free);
                auto& key=write(target);
                if(old.frame<frame) {write(current).next=target;key.previous=current;key.next=0;}
                else {write(old.previous).next=target;key.previous=old.previous;write(current).previous=target;key.next=current;}
                key.frame=frame;result.maximumInsertedFrame=(std::max)(result.maximumInsertedFrame,frame);
            }
            auto& destination=write(target);Fill(destination,pose);
            // Unlike ordinary registration, clipboard paste copies the full
            // visible/shadow/reserved dword and clears selection padding.
            std::memcpy(destination.reserved,record.reserved1,sizeof(destination.reserved));
            std::memset(destination.reservedSelection,0,sizeof(destination.reservedSelection));
        }
        if(!any)return MMS_OK;
        for(const auto& entry:patches) {
            result.dataChanged|=!Equal(entry.second.before,entry.second.after,false);
            change->patches.push_back(entry.second);
        }
        const auto required=patches.empty()?capacity:(std::max)(capacity,std::size_t(patches.rbegin()->first)+1);
        Chain(required,read);result.part=std::move(change);out=std::move(result);return MMS_OK;
    });
}
MmsResult PlanAccessoryRegistration(MMDApp& app, unsigned slot, const AccessoryRegistrationInput& input,
    std::shared_ptr<HistoryChange>& out) noexcept {
    out.reset();
    if (slot >= 255 || !app.AccessorySlot(slot) || app.PlaybackActive() || app.FrameStepPlayback() ||
        app.state.recordPlaybackStartPending || app.SceneMutationInProgress()) return MMS_E_NOT_READY;
    if (app.CurrentFrame() < 0 || app.CurrentFrame() > frame_limits::maximum) return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        auto* pool = app.AccessoryKeys(slot); const auto capacity = mdl::KeyPoolCapacity(pool);
        if (!mdl::KeyPoolIdentity(pool)) return MMS_E_NOT_READY;
        auto chain = Chain(capacity, [&](unsigned index) { return pool[index]; });
        const auto before = Pose(*app.AccessorySlot(slot)); auto pose = before;
        std::memcpy(pose.position, input.position, sizeof(pose.position));
        std::memcpy(pose.rotation, input.rotation, sizeof(pose.rotation)); pose.scale = input.scale;
        if (!Finite(before) || !Finite(pose)) return MMS_E_ARGUMENT;
        const unsigned frame = unsigned(app.CurrentFrame());
        auto found = std::lower_bound(chain.begin(), chain.end(), frame,
            [](const Dependency& entry, unsigned value) { return entry.key.frame < value; });
        const auto current = found == chain.end() ? chain.back().slot : found->slot;
        Key filled = pool[current]; Fill(filled, pose);
        if (pool[current].frame == frame && Equal(pool[current], filled, false) && Equal(before, pose)) return MMS_OK;
        auto change = std::make_shared<Registration>(app, slot, pose, std::move(chain));
        std::map<unsigned, Patch> patches;
        const auto write = [&](unsigned index) -> Key& {
            auto entry = patches.find(index);
            if (entry == patches.end()) { const auto key = index < capacity ? pool[index] : Key{};
                entry = patches.emplace(index, Patch{index, key, key}).first; }
            return entry->second.after;
        };
        if (pool[current].frame == frame) {
            change->registeredSlot = current; Fill(write(current), pose);
        } else {
            std::size_t free = 1; while (free < capacity && pool[free].frame) ++free;
            if (free >= mdl::KeyPoolLimit<Key>()) return MMS_E_ARGUMENT;
            change->registeredSlot = unsigned(free); auto& key = write(unsigned(free));
            if (pool[current].frame < frame) { write(current).next = unsigned(free); key.previous = current; }
            else { const auto previous = pool[current].previous; write(previous).next = unsigned(free); key.previous = previous;
                write(current).previous = unsigned(free); key.next = current; }
            key.frame = frame; Fill(key, pose); change->lastAfter = (std::max)(change->lastBefore, frame);
        }
        // Global marks are cleared separately; patches must carry the same
        // post-clear selection as the native clear-then-register command.
        for (auto& entry : patches) {
            entry.second.after.selected = entry.first == change->registeredSlot ? 1 : 0;
            if (!Equal(entry.second.before, entry.second.after) || entry.first == change->registeredSlot)
                change->patches.push_back(entry.second);
        }
        const auto targetCapacity = (std::max)(capacity, std::size_t(change->registeredSlot) + 1);
        Chain(targetCapacity, [&](unsigned index) {
            const auto entry = patches.find(index); return entry == patches.end() ? pool[index] : entry->second.after;
        });
        out = std::move(change); return MMS_OK;
    });
}
}
#endif
