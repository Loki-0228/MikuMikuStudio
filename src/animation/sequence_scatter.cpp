#include "mikumikustudio/sequence_scatter.hpp"
#include "mikumikustudio/undo_storage.hpp"
#include "../plugins/scene_resource_budget.hpp"
#include "mikumikustudio/native_edit_history.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>

namespace mikumikustudio::sequence_scatter {
namespace ac = animation_curve;
namespace {
constexpr std::size_t kMaxInsertedKeys = 100000;
constexpr std::size_t kMaxAttempts = 10000;
struct SnapshotHeader {
    std::size_t capacity, curveBytes;
    std::uint32_t maxFrame;
    std::int32_t cursor;
    std::size_t boneCapacity, poses, scales, scaleCurves;
};
struct ScaleEntry { int index; bone_scale::Matrix value; };
struct CurveEntry { int index; bone_scale::Curve value; };
std::size_t SnapshotBytes(const SnapshotHeader& h) {
    std::size_t total=sizeof(h);
    const auto add=[&](std::size_t count,std::size_t stride) {
        if(count>((std::numeric_limits<std::size_t>::max)()-total)/stride)throw std::length_error("Sequence snapshot size");
        total+=count*stride;
    };
    add(h.capacity,sizeof(mdl::MorphKey));add(h.curveBytes,1);
    add(h.boneCapacity,sizeof(mdl::BoneKey));add(h.poses,sizeof(ScaleEntry));
    add(h.scales,sizeof(ScaleEntry));add(h.scaleCurves,sizeof(CurveEntry));
    return total;
}
using Buffer = std::unique_ptr<void, decltype(&undo_storage::Free)>;
Buffer Snapshot(unsigned char* model) {
    const auto& m = *mdl::Mdl(model);
    morph_curve::AuthoredLayers layers;
    std::vector<ac::NamedTrack> tracks;
    std::vector<std::uint8_t> encoded;
    std::string error;
    if (!morph_curve::CaptureLayers(model, layers, error) ||
        !morph_curve::LayersTracks(layers, tracks, error) ||
        !ac::Encode(tracks, encoded, error)) throw std::runtime_error(error);
    const auto scales=bone_scale::Capture(model);
    SnapshotHeader h{mdl::MorphKeyCapacity(m.morphKeys),encoded.size(),m.maxFrame,m.searchCursor,
        mdl::BoneKeyCapacity(m),scales.pose.size(),scales.keys.size(),scales.curves.size()};
    Buffer result(undo_storage::Malloc(SnapshotBytes(h)), &undo_storage::Free);
    if (!result) throw std::bad_alloc();
    auto* bytes=static_cast<unsigned char*>(result.get());
    const auto append=[&](const void* source,std::size_t count) { if(count)std::memcpy(bytes,source,count);bytes+=count; };
    append(&h,sizeof(h));append(m.morphKeys,h.capacity*sizeof(mdl::MorphKey));
    append(encoded.data(),encoded.size());append(m.boneKeys,h.boneCapacity*sizeof(mdl::BoneKey));
    for(const auto& item:scales.pose){const ScaleEntry entry{item.first,item.second};append(&entry,sizeof(entry));}
    for(const auto& item:scales.keys){const ScaleEntry entry{item.first,item.second};append(&entry,sizeof(entry));}
    for(const auto& item:scales.curves){const CurveEntry entry{item.first,item.second};append(&entry,sizeof(entry));}
    return result;
}
void RestorePool(unsigned char* model, const std::vector<mdl::MorphKey>& pool,
                 unsigned maxFrame, int cursor) noexcept {
    auto& m = *mdl::Mdl(model);
    std::memcpy(m.morphKeys, pool.data(), pool.size() * sizeof(mdl::MorphKey));
    std::fill(m.morphKeys + pool.size(), m.morphKeys + mdl::MorphKeyCapacity(m.morphKeys), mdl::MorphKey{});
    m.maxFrame = maxFrame;
    m.searchCursor = cursor;
}
}

bool Capture(unsigned char* model, Source& out, std::string& error) {
    out = {}; error.clear();
    if (!model) { error = "Select a model and bone or expression keys first."; return false; }
    try {
        const auto& m = *mdl::Mdl(model);
        if (!m.boneKeys || !m.morphKeys || (m.morphCount&&!m.morphs) || m.boneCount>mdl::BoneKeyCapacity(m)) return false;
        Source source; source.model = model; source.first = frame_limits::maximum;
        const auto capacity = mdl::MorphKeyCapacity(m.morphKeys);
        source.pool.assign(m.morphKeys, m.morphKeys + capacity);
        source.boneCount=m.boneCount;
        const auto boneCapacity=mdl::BoneKeyCapacity(m);
        source.bonePool.assign(m.boneKeys,m.boneKeys+boneCapacity);
        source.scales=bone_scale::Capture(model);
        for(unsigned bone=0;bone<m.boneCount;++bone){
            Source::BoneChannel channel;channel.bone=static_cast<int>(bone);
            unsigned index=bone;std::size_t guard=0;
            do{
                if(index>=boneCapacity || ++guard>boneCapacity){error="The bone key chain is damaged.";return false;}
                const auto& key=m.boneKeys[index];
                // Native slots are unsigned, but sequence timing is signed. Reject
                // every malformed chain entry before conversion, including keys
                // outside the selection that still participate in conflict checks.
                if(key.frame>static_cast<std::uint32_t>(frame_limits::maximum)){
                    error="A bone key exceeds the supported frame range.";return false;
                }
                channel.original.push_back(index);
                if(key.allocated){channel.clip.push_back(index);source.first=std::min(source.first,int(key.frame));source.last=std::max(source.last,int(key.frame));++source.keyCount;}
                index=key.next;
            }while(index);
            if(!channel.clip.empty())source.bones.push_back(std::move(channel));
        }
        for (unsigned morph = 0; morph < m.morphCount; ++morph) {
            if (static_cast<unsigned>(m.morphs[morph].panel) == 0) continue;
            std::set<int> marked;
            unsigned index = morph;
            std::size_t guard = 0;
            do {
                if (index >= capacity || ++guard > capacity) { error = "The expression key chain is damaged."; return false; }
                const auto& key = m.morphKeys[index];
                if (key.allocated) marked.insert(static_cast<int>(key.frame));
                index = key.next;
            } while (index);
            if (marked.empty()) continue;
            Channel channel; channel.morph = static_cast<int>(morph);
            channel.original.id = morph_curve::StableId(morph, m.morphs[morph].name);
            if (!morph_curve::Read(model, morph, channel.original.track, error)) return false;
            for (const auto& key : channel.original.track.keys) if (marked.count(key.frame)) {
                channel.clip.keys.push_back(key);
                source.first = std::min(source.first, key.frame);
                source.last = std::max(source.last, key.frame);
                ++source.keyCount;
            }
            source.channels.push_back(std::move(channel));
        }
        if (source.keyCount < 2 || source.first == source.last) {
            error = "Select bone or expression keys on at least two different frames."; return false;
        }
        out = std::move(source); return true;
    } catch (...) { error = "Not enough memory to read the selected sequence."; return false; }
}

bool Prepare(const Source& source, const Settings& settings, Plan& out, std::string& error) {
    out = {}; error.clear();
    const auto valid = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if ((source.channels.empty() && source.bones.empty()) || source.first >= source.last) { error = "Select a bone or expression sequence first."; return false; }
    if (settings.first < 0 || settings.last < settings.first || settings.last > frame_limits::maximum ||
        !valid(settings.gapMean, 1, frame_limits::maximum) || !valid(settings.gapVariance, 0, 1e12) ||
        !valid(settings.speedMean, .05, 20) || !valid(settings.speedVariance, 0, 100)) {
        error = "Check the frame range, gap (at least 1), speed (0.05–20) and nonnegative variances."; return false;
    }
    try {
        Plan plan;
        plan.bonePool=source.bonePool;plan.scales=source.scales;
        std::size_t freeBone=source.boneCount;
        for (const auto& channel : source.channels) plan.tracks.push_back(channel.original);
        std::mt19937 random(settings.seed);
        std::normal_distribution<double> normal(0, 1);
        std::set<int> offsets;
        for (const auto& channel : source.channels) for (const auto& key : channel.clip.keys)
            offsets.insert(key.frame - source.first);
        for(const auto& channel:source.bones)for(int index:channel.clip)offsets.insert(int(source.bonePool[index].frame)-source.first);
        std::int64_t start = settings.first;
        std::size_t attempts = 0;
        while (start <= settings.last) {
            if (++attempts > kMaxAttempts) { error = "Too many placements. Shorten the range or increase the gap."; return false; }
            const double speed = std::clamp(settings.speedMean + std::sqrt(settings.speedVariance) * normal(random), .05, 20.);
            const auto duration = static_cast<std::int64_t>(std::llround((source.last - source.first) / speed));
            const auto end = start + duration;
            const auto gap = static_cast<std::int64_t>(std::llround(std::clamp(
                settings.gapMean + std::sqrt(settings.gapVariance) * normal(random), 1., double(frame_limits::maximum))));
            // A long random instance may not fit, but a later, shorter one can.
            if (end > settings.last) { start += gap; continue; }
            std::map<int, int> mapped;
            int previous = -1;
            bool compressed = false;
            for (int offset : offsets) {
                const int time = static_cast<int>(std::llround(offset / speed));
                if (time <= previous) compressed = true;
                previous = time; mapped[offset] = static_cast<int>(start) + time;
            }
            bool conflict = start <= source.last && end >= source.first;
            for (const auto& channel : source.channels) {
                const auto& keys = channel.original.track.keys;
                const auto found = std::lower_bound(keys.begin(), keys.end(), start,
                    [](const ac::Key& key, std::int64_t frame) { return key.frame < frame; });
                if (found != keys.end() && found->frame <= end) conflict = true;
            }
            for(const auto& channel:source.bones)for(int index:channel.original){const auto frame=source.bonePool[index].frame;if(frame>=start && frame<=end)conflict=true;}
            if (compressed) ++plan.compressed;
            else if (conflict) ++plan.conflicts;
            else {
                if (plan.keys + source.keyCount > kMaxInsertedKeys) { error = "More than 100,000 new keys. Shorten the range or increase the gap."; return false; }
                for (std::size_t c = 0; c < source.channels.size(); ++c) {
                    auto copied = source.channels[c].clip.keys;
                    for (std::size_t k = 0; k < copied.size(); ++k) {
                        copied[k].frame = mapped.at(copied[k].frame - source.first);
                        copied[k].tangent = ac::Tangent::Free;
                    }
                    for (std::size_t k = 1; k < copied.size(); ++k) {
                        const auto& original = source.channels[c].clip.keys;
                        const double ratio = double(original[k].frame - original[k-1].frame) /
                            (copied[k].frame - copied[k-1].frame);
                        copied[k-1].outSlope *= ratio; copied[k].inSlope *= ratio;
                    }
                    // The gap connects linearly; copied internal ease is retained.
                    copied.front().interpolation = ac::Interpolation::Linear;
                    auto& keys = plan.tracks[c].track.keys;
                    keys.insert(keys.end(), copied.begin(), copied.end());
                }
                for(const auto& channel:source.bones)for(int original:channel.clip){
                    while(freeBone<plan.bonePool.size() && plan.bonePool[freeBone].frame!=0)++freeBone;
                    if(freeBone>=mdl::kBoneKeyCapacityLimit)throw std::runtime_error("Too many bone keys.");
                    if(freeBone==plan.bonePool.size())plan.bonePool.emplace_back();
                    const int index=static_cast<int>(freeBone++);
                    auto key=source.bonePool[original];key.frame=mapped.at(int(key.frame)-source.first);key.allocated=1;
                    unsigned previous=channel.bone;
                    while(plan.bonePool[previous].next && plan.bonePool[plan.bonePool[previous].next].frame<key.frame)previous=plan.bonePool[previous].next;
                    key.previous=previous;key.next=plan.bonePool[previous].next;
                    if(key.next)plan.bonePool[key.next].previous=index;
                    plan.bonePool[previous].next=index;plan.bonePool[index]=key;
                    plan.scales.keys.erase(index);plan.scales.curves.erase(index);
                    if(auto it=source.scales.keys.find(original);it!=source.scales.keys.end())plan.scales.keys[index]=it->second;
                    if(auto it=source.scales.curves.find(original);it!=source.scales.curves.end())plan.scales.curves[index]=it->second;
                }
                ++plan.copies; plan.keys += source.keyCount;
            }
            start = end + gap;
        }
        for (auto& track : plan.tracks) {
            std::sort(track.track.keys.begin(), track.track.keys.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
            if (!ac::Validate(track.track, error)) return false;
        }
        out = std::move(plan); return true;
    } catch (...) { error = "Not enough memory to prepare the placements."; return false; }
}

#if !MMD_ORIGINAL_PROFILE
void MeasureSnapshot(const void* snapshot, plugins::resource_budget::Collector& out) {
    if (!snapshot) return;
    SnapshotHeader h{}; std::memcpy(&h, snapshot, sizeof(h));
    out.Add(snapshot,1,SnapshotBytes(h));
}
#endif
namespace {
// Own every allocating part of replay before the model's shared-history publish.
// Bone pools never shrink on undo, so restoring an older snapshot only clears its
// now-unused tail; no public pointer is replaced during this side-state exchange.
class HistoryLayers final:public native_history::SideState {
    unsigned char* model_;
    mdl::BoneKey* bonePool_;
    std::size_t boneCapacity_;
    std::vector<mdl::BoneKey> beforeBones_,afterBones_;
    morph_curve::AuthoredLayers before_,after_;
    bool hasMorphStorage_=false;
    std::unique_ptr<bone_scale::PoseStorageLease> scaleLease_;
    bone_scale::Values beforePose_,afterPose_;
    bone_scale::CurveTableState afterScales_;
    std::uint64_t scaleRevision_=0;
public:
    HistoryLayers(unsigned char* model,const std::vector<ac::NamedTrack>& tracks,
                  const unsigned char* bones,std::size_t boneCount,bone_scale::Snapshot scales):model_(model) {
        auto& m=*mdl::Mdl(model);bonePool_=m.boneKeys;boneCapacity_=mdl::BoneKeyCapacity(m);
        if(boneCount>boneCapacity_)throw std::runtime_error("Invalid bone history capacity");
        beforeBones_.assign(bonePool_,bonePool_+boneCapacity_);afterBones_.resize(boneCapacity_);
        std::memcpy(afterBones_.data(),bones,boneCount*sizeof(mdl::BoneKey));
        std::string error;
        hasMorphStorage_=morph_curve::HasLayerStorage(model);
        if((!hasMorphStorage_&&!tracks.empty())||!morph_curve::CaptureLayers(model,before_,error)||
           !morph_curve::RebuildLayers(model,tracks,after_,error))throw std::runtime_error("Invalid morph history layers");
        scaleLease_=bone_scale::PoseStorageLease::Prepare(model);
        beforePose_=bone_scale::Capture(model).pose;afterPose_=std::move(scales.pose);
        if(!bone_scale::CaptureCurveTable(model,afterScales_))throw std::runtime_error("Missing scale history storage");
        scaleRevision_=afterScales_.revision;
        afterScales_.keys=std::move(scales.keys);afterScales_.curves=std::move(scales.curves);
        ++afterScales_.revision;
    }
    bool IsCurrent() const noexcept override {
        const auto& m=*mdl::Mdl(model_);
        return m.boneKeys==bonePool_&&mdl::BoneKeyCapacity(m)==boneCapacity_&&
            !std::memcmp(bonePool_,beforeBones_.data(),boneCapacity_*sizeof(mdl::BoneKey))&&
            bone_scale::HasPoseStorage(model_)&&bone_scale::Revision(model_)==scaleRevision_&&
            bone_scale::PoseValuesEqual(model_,beforePose_)&&
            morph_curve::HasLayerStorage(model_)==hasMorphStorage_&&morph_curve::LayersUnchanged(model_,before_);
    }
    void Publish() noexcept override {
        std::memcpy(bonePool_,afterBones_.data(),boneCapacity_*sizeof(mdl::BoneKey));
        mdl::BoneKeyVisitClear(*mdl::Mdl(model_));
        bone_scale::SwapCurveTable(model_,afterScales_);
        bone_scale::ExchangePoseValues(model_,afterPose_);
        morph_curve::SwapLayers(model_,after_);
    }
};
}
bool PrepareHistory(unsigned char* model,const void* snapshot,std::vector<mdl::MorphKey>& pool,
                    std::unique_ptr<native_history::SideState>& layers,std::uint32_t& maxFrame,std::int32_t& cursor) {
    if(!model||!snapshot)return false;
    undo_storage::Allocation allocation;
    if(!undo_storage::Find(snapshot,allocation)||allocation.bytes<sizeof(SnapshotHeader))return false;
    const auto* bytes=static_cast<const unsigned char*>(snapshot);
    SnapshotHeader h;std::memcpy(&h,bytes,sizeof(h));
    const auto& m=*mdl::Mdl(model);const auto capacity=mdl::MorphKeyCapacity(m.morphKeys);
    if(!h.capacity||h.capacity>capacity||h.boneCapacity<m.boneCount||h.boneCapacity>mdl::BoneKeyCapacity(m)||
       h.curveBytes>morph_curve::kMaximumPayloadBytes||SnapshotBytes(h)!=allocation.bytes)return false;
    bytes+=sizeof(h);
    const auto* morphs=bytes;bytes+=h.capacity*sizeof(mdl::MorphKey);
    std::string error;std::vector<ac::NamedTrack> tracks;
    if(!ac::Decode(bytes,h.curveBytes,tracks,error))return false;
    bytes+=h.curveBytes;const auto* bones=bytes;bytes+=h.boneCapacity*sizeof(mdl::BoneKey);
    bone_scale::Snapshot scales;
    const auto readScales=[&](std::size_t count,bone_scale::Values& values,std::size_t limit) {
        for(std::size_t i=0;i<count;++i){ScaleEntry entry;std::memcpy(&entry,bytes,sizeof(entry));bytes+=sizeof(entry);
            if(entry.index<0||static_cast<std::size_t>(entry.index)>=limit||!values.emplace(entry.index,entry.value).second)return false;}
        return true;
    };
    if(!readScales(h.poses,scales.pose,m.boneCount)||!readScales(h.scales,scales.keys,h.boneCapacity))return false;
    for(std::size_t i=0;i<h.scaleCurves;++i){CurveEntry entry;std::memcpy(&entry,bytes,sizeof(entry));bytes+=sizeof(entry);
        if(entry.index<0||static_cast<std::size_t>(entry.index)>=h.boneCapacity||!scales.curves.emplace(entry.index,entry.value).second)return false;}
    auto prepared=std::make_unique<HistoryLayers>(model,tracks,bones,h.boneCapacity,std::move(scales));
    pool.assign(capacity,mdl::MorphKey{});
    std::memcpy(pool.data(),morphs,h.capacity*sizeof(mdl::MorphKey));
    maxFrame=h.maxFrame;cursor=h.cursor;layers=std::move(prepared);
    return true;
}
bool Restore(unsigned char* model,const void* snapshot) noexcept {
    try {
        std::vector<mdl::MorphKey> pool;std::unique_ptr<native_history::SideState> layers;
        std::uint32_t maximum=0;std::int32_t cursor=0;
        if(!PrepareHistory(model,snapshot,pool,layers,maximum,cursor)||!layers->IsCurrent())return false;
        auto& m=*mdl::Mdl(model);
        std::memcpy(m.morphKeys,pool.data(),pool.size()*sizeof(mdl::MorphKey));
        layers->Publish();m.maxFrame=maximum;m.searchCursor=cursor;
        return true;
    }catch(...){return false;}
}

bool Apply(MMDApp& app, const Source& source, const Plan& plan, std::string& error) {
    error.clear();
    if (!source.model || app.SelectedModel() != source.model || app.CameraMode() || app.PlaybackActive() || !plan.copies) {
        error = "Select a model, stop playback and prepare at least one placement."; return false;
    }
    auto& m = *mdl::Mdl(source.model);
    if (source.boneCount!=m.boneCount||source.bonePool.size()!=mdl::BoneKeyCapacity(m)||
        source.pool.size()!=mdl::MorphKeyCapacity(m.morphKeys)||
        std::memcmp(source.pool.data(),m.morphKeys,source.pool.size()*sizeof(mdl::MorphKey))||
        std::memcmp(source.bonePool.data(),m.boneKeys,source.bonePool.size()*sizeof(mdl::BoneKey))) {
        error = "The source keys changed. Close this window and select the sequence again."; return false;
    }
    try {
        const auto currentScales=bone_scale::Capture(source.model);
        if(currentScales.keys!=source.scales.keys||currentScales.curves!=source.scales.curves){
            error="The source scale keys changed. Close this window and select the sequence again.";return false;
        }
        if(plan.bonePool.size()<m.boneCount||plan.bonePool.size()>mdl::kBoneKeyCapacityLimit){error="Invalid bone sequence plan.";return false;}
        auto before = Snapshot(source.model);
        std::unique_ptr<mdl::BoneKeyPoolGrowth> growth;
        if(!mdl::BoneKeyPoolGrowth::Prepare(m,plan.bonePool.size(),growth))throw std::bad_alloc();
        auto scaleLease=bone_scale::PoseStorageLease::Prepare(source.model);
        bone_scale::CurveTableState scaleExchange;
        if(!bone_scale::CaptureCurveTable(source.model,scaleExchange))throw std::bad_alloc();
        scaleExchange.keys=plan.scales.keys;scaleExchange.curves=plan.scales.curves;++scaleExchange.revision;
        bool exchanged=false;
        morph_curve::AuthoredLayers originalLayers;
        if (!morph_curve::CaptureLayers(source.model, originalLayers, error)) return false;
        const auto maxFrame = m.maxFrame; const auto cursor = m.searchCursor;
        Buffer after(nullptr, &undo_storage::Free);
        try {
            if(!growth->Publish())throw std::runtime_error("The bone pool changed during sequence preparation.");
            std::memcpy(m.boneKeys,plan.bonePool.data(),plan.bonePool.size()*sizeof(mdl::BoneKey));
            std::fill(m.boneKeys+plan.bonePool.size(),m.boneKeys+mdl::BoneKeyCapacity(m),mdl::BoneKey{});
            for(const auto& key:plan.bonePool)m.maxFrame=std::max(m.maxFrame,key.frame);
            bone_scale::SwapCurveTable(source.model,scaleExchange);exchanged=true;
            if (!plan.tracks.empty()&&!morph_curve::Write(source.model, plan.tracks, error)) throw std::runtime_error(error);
            // Preserve old marks, selecting only the new keys in addition to them.
            for (const auto& channel : source.channels) {
                std::map<unsigned, unsigned char> oldMarks;
                unsigned index = channel.morph;
                do { const auto& key = source.pool[index]; oldMarks[key.frame] = key.allocated; index = key.next; } while (index);
                index = channel.morph;
                do { auto& key = m.morphKeys[index]; const auto found = oldMarks.find(key.frame);
                    key.allocated = found == oldMarks.end() ? 1 : found->second; index = key.next; } while (index);
            }
            after = Snapshot(source.model);
            if(!growth->Accept())throw std::runtime_error("The bone pool changed before sequence publication.");
        } catch (...) {
            RestorePool(source.model, source.pool, maxFrame, cursor);
            morph_curve::SwapLayers(source.model, originalLayers);
            if(exchanged)bone_scale::SwapCurveTable(source.model,scaleExchange);
            // Rollback does not allocate, even if a grown pool was published.
            if(!growth->Grows())std::memcpy(m.boneKeys,source.bonePool.data(),source.bonePool.size()*sizeof(mdl::BoneKey));
            growth->Rollback();
            if (error.empty()) error = "Not enough memory. No keys were inserted.";
            return false;
        }
        const unsigned ring = (m.undoState[0] + 1) % 30;
        for (int side = 0; side < 2; ++side) {
            auto& entry = m.undoRings[side].slots[ring];
            undo_storage::Free(entry.bonePose); undo_storage::Free(entry.auxiliaryPose); entry = {};
            entry.operation = 6; entry.frame = app.CurrentFrame();
            entry.auxiliaryPose = side == 0 ? before.release() : after.release();
        }
        m.undoState[0] = m.undoState[1] = ring; m.undoDirty = 1; m.redoDirty = 0;
        EnableWindow(GetDlgItem(app.MainWindow(), panel::kUndoButton), TRUE);
        EnableWindow(GetDlgItem(app.MainWindow(), panel::kRedoButton), FALSE);
        app.LastRegisteredFrame() = std::max(app.LastRegisteredFrame(), m.maxFrame);
        app.SceneModified() = 1;
        mdl::BoneKeyVisitClear(m);
        SeekModelFrame(source.model, app.CurrentFrame(), app.PlaybackPhysicsMode());
        InvalidateRect(app.MainWindow(), nullptr, FALSE);
        return true;
    } catch (...) { error = "Not enough memory. No keys were inserted."; return false; }
}
}
