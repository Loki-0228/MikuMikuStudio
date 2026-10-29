#include "mikumikustudio/sequence_scatter.hpp"
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
using Buffer = std::unique_ptr<void, decltype(&std::free)>;
Buffer Snapshot(unsigned char* model) {
    const auto& m = *mdl::Mdl(model);
    morph_curve::AuthoredLayers layers;
    std::vector<ac::NamedTrack> tracks;
    std::vector<std::uint8_t> encoded;
    std::string error;
    if (!morph_curve::CaptureLayers(model, layers, error) ||
        !morph_curve::LayersTracks(layers, tracks, error) ||
        !ac::Encode(tracks, encoded, error)) throw std::runtime_error(error);
    SnapshotHeader h{mdl::MorphKeyCapacity(m.morphKeys), encoded.size(), m.maxFrame, m.searchCursor};
    const auto scales=bone_scale::Capture(model);
    h.boneCapacity=mdl::BoneKeyCapacity(m);h.poses=scales.pose.size();h.scales=scales.keys.size();h.scaleCurves=scales.curves.size();
    const auto poolBytes = h.capacity * sizeof(mdl::MorphKey);
    Buffer result(std::malloc(sizeof(h) + poolBytes + encoded.size()+h.boneCapacity*sizeof(mdl::BoneKey)
        +(h.poses+h.scales)*sizeof(ScaleEntry)+h.scaleCurves*sizeof(CurveEntry)), &std::free);
    if (!result) throw std::bad_alloc();
    auto* bytes = static_cast<unsigned char*>(result.get());
    std::memcpy(bytes, &h, sizeof(h));
    std::memcpy(bytes + sizeof(h), m.morphKeys, poolBytes);
    std::memcpy(bytes + sizeof(h) + poolBytes, encoded.data(), encoded.size());
    auto* tail=bytes+sizeof(h)+poolBytes+encoded.size();
    std::memcpy(tail,m.boneKeys,h.boneCapacity*sizeof(mdl::BoneKey));tail+=h.boneCapacity*sizeof(mdl::BoneKey);
    for(const auto* values:{&scales.pose,&scales.keys})for(const auto& [index,value]:*values){ScaleEntry e{index,value};std::memcpy(tail,&e,sizeof(e));tail+=sizeof(e);}
    for(const auto& [index,value]:scales.curves){CurveEntry e{index,value};std::memcpy(tail,&e,sizeof(e));tail+=sizeof(e);}
    return result;
}

}

bool Capture(unsigned char* model, Source& out, std::string& error) {
    out = {}; error.clear();
    if (!model) { error = "Select a model and bone or expression keys first."; return false; }
    try {
        const auto& m = *mdl::Mdl(model);
        if (!m.boneKeys || !m.morphKeys) return false;
        Source source; source.model = model; source.first = frame_limits::maximum;
        const auto capacity = mdl::MorphKeyCapacity(m.morphKeys);
        source.pool.assign(m.morphKeys, m.morphKeys + capacity);
        const auto boneCapacity=mdl::BoneKeyCapacity(m);
        source.bonePool.assign(m.boneKeys,m.boneKeys+boneCapacity);
        source.scales=bone_scale::Capture(model);
        for(unsigned bone=0;bone<m.boneCount;++bone){
            Source::BoneChannel channel;channel.bone=static_cast<int>(bone);
            unsigned index=bone;std::size_t guard=0;
            do{
                if(index>=boneCapacity || ++guard>boneCapacity){error="The bone key chain is damaged.";return false;}
                const auto& key=m.boneKeys[index];channel.original.push_back(index);
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
        std::size_t freeBone=source.model?mdl::Mdl(source.model)->boneCount:0;
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

bool Restore(unsigned char* model, const void* snapshot) noexcept {
    if (!model || !snapshot) return false;
    try {
        const auto* bytes = static_cast<const unsigned char*>(snapshot);
        SnapshotHeader h; std::memcpy(&h, bytes, sizeof(h));
        std::string error; std::vector<ac::NamedTrack> tracks;
        if (!ac::Decode(bytes + sizeof(h) + h.capacity * sizeof(mdl::MorphKey), h.curveBytes, tracks, error)) return false;
        morph_curve::AuthoredLayers layers;
        if (!morph_curve::RebuildLayers(model, tracks, layers, error)) return false;
        auto& m = *mdl::Mdl(model);
        auto* tail=bytes+sizeof(h)+h.capacity*sizeof(mdl::MorphKey)+h.curveBytes;
        const auto* boneBytes=tail;tail+=h.boneCapacity*sizeof(mdl::BoneKey);
        bone_scale::Snapshot scales;
        for(auto* values:{&scales.pose,&scales.keys}){const auto count=values==&scales.pose?h.poses:h.scales;for(std::size_t i=0;i<count;++i){ScaleEntry e;std::memcpy(&e,tail,sizeof(e));tail+=sizeof(e);values->emplace(e.index,e.value);}}
        for(std::size_t i=0;i<h.scaleCurves;++i){CurveEntry e;std::memcpy(&e,tail,sizeof(e));tail+=sizeof(e);scales.curves.emplace(e.index,e.value);}
        if (!mdl::MorphKeyPoolEnsure(m, h.capacity) || !mdl::BoneKeyPoolEnsure(m,h.boneCapacity)) return false;
        if(!bone_scale::Restore(model,std::move(scales)))return false;
        std::memcpy(m.boneKeys,boneBytes,h.boneCapacity*sizeof(mdl::BoneKey));
        std::fill(m.boneKeys+h.boneCapacity,m.boneKeys+mdl::BoneKeyCapacity(m),mdl::BoneKey{});
        std::memcpy(m.morphKeys, bytes + sizeof(h), h.capacity * sizeof(mdl::MorphKey));
        std::fill(m.morphKeys + h.capacity, m.morphKeys + mdl::MorphKeyCapacity(m.morphKeys), mdl::MorphKey{});
        m.maxFrame = h.maxFrame; m.searchCursor = h.cursor;
        morph_curve::SwapLayers(model, layers);
        return true;
    } catch (...) { return false; }
}

bool Apply(MMDApp& app, const Source& source, const Plan& plan, std::string& error) {
    error.clear();
    if (!source.model || app.SelectedModel() != source.model || app.CameraMode() || app.PlaybackActive() || !plan.copies) {
        error = "Select a model, stop playback and prepare at least one placement."; return false;
    }
    auto& m = *mdl::Mdl(source.model);
    if (source.pool.size() != mdl::MorphKeyCapacity(m.morphKeys) ||
        std::memcmp(source.pool.data(), m.morphKeys, source.pool.size() * sizeof(mdl::MorphKey)) ||
        source.bonePool.size()!=mdl::BoneKeyCapacity(m) ||
        std::memcmp(source.bonePool.data(),m.boneKeys,source.bonePool.size()*sizeof(mdl::BoneKey))) {
        error = "The source keys changed. Close this window and select the sequence again."; return false;
    }
    try {
        auto before = Snapshot(source.model);
        Buffer after(nullptr, &std::free);
        try {
            if(!mdl::BoneKeyPoolEnsure(m,plan.bonePool.size()))throw std::bad_alloc();
            if(!bone_scale::Restore(source.model,plan.scales))throw std::bad_alloc();
            std::copy(plan.bonePool.begin(),plan.bonePool.end(),m.boneKeys);
            for(const auto& key:plan.bonePool)m.maxFrame=std::max(m.maxFrame,key.frame);
            if (!plan.tracks.empty() && !morph_curve::Write(source.model, plan.tracks, error)) throw std::runtime_error(error);
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
        } catch (...) {
            Restore(source.model,before.get());
            if (error.empty()) error = "Not enough memory. No keys were inserted.";
            return false;
        }
        const unsigned ring = (m.undoState[0] + 1) % 30;
        for (int side = 0; side < 2; ++side) {
            auto& entry = m.undoRings[side].slots[ring];
            std::free(entry.bonePose); std::free(entry.auxiliaryPose); entry = {};
            entry.operation = 6; entry.frame = app.CurrentFrame();
            entry.auxiliaryPose = side == 0 ? before.release() : after.release();
        }
        m.undoState[0] = m.undoState[1] = ring; m.undoDirty = 1; m.redoDirty = 0;
        EnableWindow(GetDlgItem(app.MainWindow(), panel::kUndoButton), TRUE);
        EnableWindow(GetDlgItem(app.MainWindow(), panel::kRedoButton), FALSE);
        app.LastRegisteredFrame() = std::max(app.LastRegisteredFrame(), m.maxFrame);
        app.SceneModified() = 1;
        SeekModelFrame(source.model, app.CurrentFrame(), app.PlaybackPhysicsMode());
        InvalidateRect(app.MainWindow(), nullptr, FALSE);
        return true;
    } catch (...) { error = "Not enough memory. No keys were inserted."; return false; }
}
}
