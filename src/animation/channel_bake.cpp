#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
// Native channel baking.  See channel_bake.hpp for the contract.
//
// Two families of tracks live here:
//   * the scene-wide camera / light / self-shadow / gravity tracks and every
//     .x accessory timeline, which are read and written through curve_host so
//     the floating curve editor, the PMM extension payload and the native
//     timeline never drift apart;
//   * the selected model's morph pool and its 表示/IK display pool, which are
//     read and written in place inside the model's own native key pools.
// The bone pools are deliberately absent: bake::Compute/Commit owns them, and
// this module only reports the bone channel in its enumeration.
#define NOMINMAX
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/channel_bake.hpp"
#include "mikudancestudio/global_key_layout.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/model_layout.hpp"
#include "mikudancestudio/text_encoding.hpp"
#include "mikudancestudio/ui_language.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <set>
#include <utility>

namespace mikudancestudio::channel_bake {
namespace {
namespace ac = animation_curve;
using Channel = curve_editor::Channel;
constexpr int kMaximumFrame = frame_limits::maximum;

std::wstring Text(MMDApp& app, const wchar_t* ja, const wchar_t* en, const wchar_t* zh) {
    return app.EnglishUI() == 0 ? ja : app.EnglishUI() == 1 ? en : zh;
}

bool IsSceneKind(Kind kind) {
    return kind == Kind::Camera || kind == Kind::Light || kind == Kind::SelfShadow ||
           kind == Kind::Gravity || kind == Kind::Accessory;
}

curve_host::Kind HostKind(Kind kind) {
    switch (kind) {
    case Kind::Camera: return curve_host::Kind::Camera;
    case Kind::Light: return curve_host::Kind::Light;
    case Kind::SelfShadow: return curve_host::Kind::Shadow;
    case Kind::Gravity: return curve_host::Kind::Gravity;
    default: return curve_host::Kind::Accessory;
    }
}

// ---- resampling -----------------------------------------------------------
// The value and key rule is animation_curve::Bake's: sample Evaluate at every
// step frame and give the key a Step or a Linear interpolation according to the
// track's discrete flag.  Bake itself refuses a range that does not cover the
// whole track, while this bake must keep every key outside [first,last] exactly
// as it is, so the splice is done here on top of the same evaluation.
//
// Splicing alone is not enough for a cubic: the interval that now ends at the
// first baked key would be relabelled Linear, and the interval that starts at
// the last baked key would lose its outgoing handle, so the curve *outside* the
// range would change even though its keys survived.  Both boundaries are
// therefore split exactly, with the same normalised-time cubic the curve core
// evaluates, so the two outside pieces reproduce the original curve sample for
// sample and tangent for tangent.

namespace {

constexpr double kMinimumWeight = 0.0001;
constexpr double kMaximumWeight = 0.9999;
// animation_curve::Validate refuses a key whose outgoing weight plus the next
// key's incoming weight exceeds this, because such an interval is ambiguous in
// time.  A subdivided piece can exceed it even when the original interval does
// not; when that happens the exact piece is not representable in this key
// format and the boundary keeps its sampled keys instead of failing the bake.
constexpr double kMaximumWeightSum = 0.9999;

// The cubic animation_curve evaluates for one interval: control abscissae
// 0, x1, x2, 1 with x1 = start.outWeight and x2 = 1 - end.inWeight, and control
// ordinates taken from the values and the per-frame slopes.
struct Cubic {
    double x1 = 0, x2 = 0, y0 = 0, y1 = 0, y2 = 0, y3 = 0;
};

Cubic Controls(const ac::Key& start, const ac::Key& end) {
    const double span = static_cast<double>(end.frame) - static_cast<double>(start.frame);
    Cubic cubic;
    cubic.x1 = start.outWeight;
    cubic.x2 = 1.0 - end.inWeight;
    cubic.y0 = start.value;
    cubic.y1 = start.value + start.outSlope * start.outWeight * span;
    cubic.y2 = end.value - end.inSlope * end.inWeight * span;
    cubic.y3 = end.value;
    return cubic;
}

// The same root finder animation_curve uses: the parameter of the x cubic whose
// value is `u`.  The caller guarantees 0 < x1 < x2 < 1, so the root is unique.
double CubicParameter(double x1, double x2, double u) {
    const double cubic = 1.0 - 3.0 * x2 + 3.0 * x1;
    const double quadratic = 3.0 * x2 - 6.0 * x1;
    const double linear = 3.0 * x1;
    const auto x = [&](double s) { return ((cubic * s + quadratic) * s + linear) * s; };
    const auto dx = [&](double s) { return (3.0 * cubic * s + 2.0 * quadratic) * s + linear; };
    double low = 0.0, high = 1.0, s = u;
    for (int iteration = 0; iteration < 96; ++iteration) {
        const double residual = x(s) - u;
        if (residual > 0.0)
            high = s;
        else
            low = s;
        if (high - low <= 1e-16) break;
        const double slope = dx(s);
        double next = slope > 1e-12 ? s - residual / slope : 0.5 * (low + high);
        if (!(next > low && next < high)) next = 0.5 * (low + high);
        s = next;
    }
    return s;
}

// Both halves of an interval split at one frame, in the key representation: the
// start key's outgoing handle, the split key's value and its two handles, and
// the end key's incoming handle.
struct BoundarySplit {
    double startOutWeight = 0, startOutSlope = 0;
    double value = 0;
    double inWeight = 0, inSlope = 0;
    double outWeight = 0, outSlope = 0;
    double endInWeight = 0, endInSlope = 0;
};

// The subdivision of one interval, or a refusal when the exact pieces cannot be
// written in this key format.  It never approximates: a piece whose handle
// weight leaves [0.0001, 0.9999], or whose pair with its neighbour would exceed
// the sum the format allows, is reported instead of being clamped.
bool SplitInterval(const ac::Key& start, const ac::Key& end, int frame, BoundarySplit& out) {
    if (end.frame <= start.frame || frame <= start.frame || frame >= end.frame) return false;
    const double span = static_cast<double>(end.frame) - start.frame;
    const double u = (frame - start.frame) / span;
    const Cubic cubic = Controls(start, end);
    if (!(cubic.x1 > 0.0) || !(cubic.x2 < 1.0) || !(cubic.x1 < cubic.x2)) return false;
    const double s = CubicParameter(cubic.x1, cubic.x2, u);
    const auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
    const double ax = lerp(0.0, cubic.x1, s), ay = lerp(cubic.y0, cubic.y1, s);
    const double bx = lerp(cubic.x1, cubic.x2, s), by = lerp(cubic.y1, cubic.y2, s);
    const double cx = lerp(cubic.x2, 1.0, s), cy = lerp(cubic.y2, cubic.y3, s);
    const double dx = lerp(ax, bx, s), dy = lerp(ay, by, s);
    const double ex = lerp(bx, cx, s), ey = lerp(by, cy, s);
    const double splitX = lerp(dx, ex, s), splitY = lerp(dy, ey, s);
    // The left piece spans x in [0,splitX], the right piece x in [splitX,1]; both
    // are renormalised to their own [0,1] range, which is what the key weights
    // express.  The ordinates stay absolute.
    const double leftSpan = static_cast<double>(frame - start.frame);
    const double rightSpan = static_cast<double>(end.frame - frame);
    const double leftOut = ax / splitX;
    const double leftIn = 1.0 - dx / splitX;
    const double rightOut = (ex - splitX) / (1.0 - splitX);
    const double rightIn = 1.0 - (cx - splitX) / (1.0 - splitX);
    if (!(leftOut > 0.0) || !(leftIn > 0.0) || !(rightOut > 0.0) || !(rightIn > 0.0)) return false;
    if (leftOut < kMinimumWeight || leftOut > kMaximumWeight) return false;
    if (leftIn < kMinimumWeight || leftIn > kMaximumWeight) return false;
    if (rightOut < kMinimumWeight || rightOut > kMaximumWeight) return false;
    if (rightIn < kMinimumWeight || rightIn > kMaximumWeight) return false;
    if (!(leftOut + leftIn <= kMaximumWeightSum)) return false;
    if (!(rightOut + rightIn <= kMaximumWeightSum)) return false;
    // The key at the range start is followed by a sampled key, whose incoming
    // weight is the floor; the piece's outgoing handle has to fit under it.
    if (!(rightOut + kMinimumWeight <= kMaximumWeightSum)) return false;
    out.startOutWeight = leftOut;
    out.startOutSlope = (ay - cubic.y0) / (out.startOutWeight * leftSpan);
    out.value = splitY;
    out.inWeight = leftIn;
    out.inSlope = (splitY - dy) / (out.inWeight * leftSpan);
    out.outWeight = rightOut;
    out.outSlope = (ey - splitY) / (out.outWeight * rightSpan);
    out.endInWeight = rightIn;
    out.endInSlope = (cubic.y3 - cy) / (out.endInWeight * rightSpan);
    return std::isfinite(out.value) && std::isfinite(out.startOutSlope) &&
           std::isfinite(out.inSlope) && std::isfinite(out.outSlope) &&
           std::isfinite(out.endInSlope);
}

// The message shown when a cubic boundary cannot be written exactly.  It names
// the frame, so the caller can move the range or fix the curve.
std::wstring BoundaryMessage(MMDApp& app, int frame) {
    const std::wstring number = std::to_wstring(frame);
    const std::wstring ja =
        L"フレーム " + number +
        L" の三次曲線の境界はキーフレーム形式で正確に表現できません（分割後の制御点の重みが許容範囲外）。何も書き込みませんでした。範囲か曲線を調整してください。";
    const std::wstring en =
        L"The cubic boundary at frame " + number +
        L" cannot be reproduced exactly in the keyframe format (the subdivided handle weights leave the allowed range). Nothing was written; move the range or fix the curve.";
    const std::wstring zh =
        L"第 " + number +
        L" 帧处的三次曲线边界无法用关键帧格式精确表示（细分后的控制柄权重超出允许范围）。未写入任何内容，请调整烧录范围或曲线。";
    return app.EnglishUI() == 0 ? ja : app.EnglishUI() == 1 ? en : zh;
}

// Restores the shape of the interval that now ends at the first baked key.
// It never touches the key after the range: that key's incoming handle belongs
// to the piece the right boundary restores.
Error RestoreLeftBoundary(MMDApp& app, ac::Track& out, const ac::Track& source, int first,
                          bool stepKeys, std::wstring& message) {
    auto at = std::lower_bound(out.keys.begin(), out.keys.end(), first,
                               [](const ac::Key& key, int value) { return key.frame < value; });
    if (at == out.keys.end() || at->frame != first) return Error::None;
    auto after = std::lower_bound(source.keys.begin(), source.keys.end(), first,
                                  [](const ac::Key& key, int value) { return key.frame < value; });
    if (after != source.keys.end() && after->frame == first) {
        // A key sat exactly on the range start: the interval before it is
        // unchanged, so it has to keep its own incoming handle and type.
        at->interpolation = after->interpolation;
        at->tangent = after->tangent;
        at->inSlope = after->inSlope;
        at->inWeight = after->inWeight;
        return Error::None;
    }
    if (after == source.keys.begin() || after == source.keys.end()) return Error::None;
    const ac::Key& start = *(after - 1);
    const ac::Key& end = *after;
    if (!stepKeys && end.interpolation == ac::Interpolation::Bezier) {
        BoundarySplit split;
        if (!SplitInterval(start, end, first, split)) {
            // Going ahead with sampled keys here would change the curve before
            // the range, so the bake is refused instead of quietly deforming it.
            message = BoundaryMessage(app, first);
            return Error::Unrepresentable;
        }
        for (auto& key : out.keys)
            if (key.frame == start.frame) {
                key.outWeight = split.startOutWeight;
                key.outSlope = split.startOutSlope;
                key.tangent = ac::Tangent::Free;
                break;
            }
        at->value = split.value;
        at->interpolation = ac::Interpolation::Bezier;
        at->tangent = ac::Tangent::Free;
        at->inWeight = split.inWeight;
        at->inSlope = split.inSlope;
        // Only a single-frame range reads these; a longer range replaces the
        // interval after this key with sampled Linear keys.
        at->outWeight = split.outWeight;
        at->outSlope = split.outSlope;
        return Error::None;
    }
    // A Linear or Step interval keeps its shape when an intermediate key is
    // inserted with the sampled value: only the type of the piece changes.
    at->interpolation = stepKeys ? ac::Interpolation::Step : end.interpolation;
    at->tangent =
        at->interpolation == ac::Interpolation::Step ? ac::Tangent::Flat : ac::Tangent::Linear;
    if (at->interpolation == ac::Interpolation::Step) at->value = start.value;
    return Error::None;
}

// Restores the shape of the interval that now starts at the last baked key, by
// handing that key the original outgoing handle and the key after the range the
// incoming handle of the same piece.
Error RestoreRightBoundary(MMDApp& app, ac::Track& out, const ac::Track& source, int last,
                           bool stepKeys, std::wstring& message) {
    auto at = std::lower_bound(out.keys.begin(), out.keys.end(), last,
                               [](const ac::Key& key, int value) { return key.frame < value; });
    if (at == out.keys.end() || at->frame != last) return Error::None;
    if (stepKeys) return Error::None;  // a hold keeps the value, so its handles never matter
    auto after = std::lower_bound(source.keys.begin(), source.keys.end(), last,
                                  [](const ac::Key& key, int value) { return key.frame < value; });
    if (after != source.keys.end() && after->frame == last) {
        // A key sat exactly on the range end: the interval after it is unchanged,
        // so it has to keep its own outgoing handle.
        at->outSlope = after->outSlope;
        at->outWeight = after->outWeight;
        return Error::None;
    }
    if (after == source.keys.begin() || after == source.keys.end()) return Error::None;
    const ac::Key& start = *(after - 1);
    const ac::Key& end = *after;
    if (end.interpolation != ac::Interpolation::Bezier) return Error::None;
    BoundarySplit split;
    if (!SplitInterval(start, end, last, split)) {
        message = BoundaryMessage(app, last);
        return Error::Unrepresentable;
    }
    at->value = split.value;
    at->outWeight = split.outWeight;
    at->outSlope = split.outSlope;
    auto tail = std::lower_bound(out.keys.begin(), out.keys.end(), end.frame,
                                 [](const ac::Key& key, int value) { return key.frame < value; });
    if (tail != out.keys.end() && tail->frame == end.frame) {
        tail->inWeight = split.endInWeight;
        tail->inSlope = split.endInSlope;
        tail->tangent = ac::Tangent::Free;
    }
    return Error::None;
}
}  // namespace

Error Resample(MMDApp& app, const ac::Track& source, int first, int last, int step, ac::Track& out,
               std::wstring& message, const Channel* channel = nullptr) {
    out = ac::Track{};
    if (source.keys.empty()) {
        message = L"The track has no keys to resample.";
        return Error::InvalidRequest;
    }
    // curve_host reads some channels (accessory position and rotation) as Step
    // keys without marking the whole track discrete.  Keeping that shape matters
    // at the range edges: the interval ending at the first baked key would turn
    // a hold into a ramp if it were relabelled Linear.
    const bool stepKeys =
        source.discrete || std::all_of(source.keys.begin(), source.keys.end(), [](const ac::Key& key) {
            return key.interpolation == ac::Interpolation::Step;
        });
    std::vector<ac::Key> baked;
    baked.reserve(static_cast<std::size_t>((last - first) / step) + 2);
    const auto sample = [&](int frame) {
        ac::Key key;
        key.frame = frame;
        key.value = channel ? curve_editor::EvaluateChannel(*channel, double(frame)).value :
            ac::Evaluate(source, static_cast<double>(frame)).value;
        key.interpolation = stepKeys ? ac::Interpolation::Step : ac::Interpolation::Linear;
        key.tangent = stepKeys ? ac::Tangent::Flat : ac::Tangent::Linear;
        // Linear and Step intervals read values only, so the handles of a sampled
        // key stay at the floor: that keeps every neighbouring pair inside the
        // weight sum the key format allows, and leaves the boundary splits free
        // to install their exact handles.
        key.inWeight = kMinimumWeight;
        key.outWeight = kMinimumWeight;
        return key;
    };
    for (std::int64_t frame = first; frame <= last; frame += step)
        baked.push_back(sample(static_cast<int>(frame)));
    if (baked.back().frame != last) baked.push_back(sample(last));
    out.discrete = source.discrete;
    out.keys.reserve(source.keys.size() + baked.size());
    for (const auto& key : source.keys)
        if (key.frame < first || key.frame > last) out.keys.push_back(key);
    for (const auto& key : baked) {
        auto at = std::lower_bound(out.keys.begin(), out.keys.end(), key.frame,
                                   [](const ac::Key& a, int frame) { return a.frame < frame; });
        if (at != out.keys.end() && at->frame == key.frame)
            *at = key;
        else
            out.keys.insert(at, key);
    }
    // Both boundaries are restored from the original track.  They touch
    // different fields of a shared key when the range is a single frame, so both
    // calls are always safe.  A refused boundary leaves the whole track unused:
    // the caller must not write it.
    const Error left = RestoreLeftBoundary(app, out, source, first, stepKeys, message);
    if (left != Error::None) return left;
    return RestoreRightBoundary(app, out, source, last, stepKeys, message);
}

// The checks curve_host::Write performs on a channel set, so that a bake which
// Write would refuse is refused here instead - before any track is written.
Error ValidChannels(const std::vector<Channel>& channels, const std::vector<Field>& fields,
                    std::wstring& message) {
    if (channels.size() != fields.size()) {
        message = L"The channel list does not match its field list.";
        return Error::InvalidRequest;
    }
    std::set<int> frames{0};
    for (std::size_t c = 0; c < channels.size(); ++c) {
        const auto& track = channels[c].track;
        std::string detail;
        if (track.discrete != fields[c].discrete || !ac::Validate(track, detail) || track.keys.empty() ||
            track.keys.front().frame != 0) {
            message = L"Every baked track keeps a valid frame-zero key.";
            return Error::InvalidRequest;
        }
        for (const auto& key : track.keys) {
            if (key.frame > mikudancestudio::frame_limits::maximum || !std::isfinite(key.value) || key.value < fields[c].minimum ||
                key.value > fields[c].maximum ||
                (fields[c].discrete && std::floor(key.value) != key.value)) {
                message = L"A baked value or frame is outside the allowed range.";
                return Error::InvalidRequest;
            }
            frames.insert(key.frame);
        }
    }
    if (frames.size() > mdl::KeyPoolLimit<mdl::CameraKey>()) {
        message = L"The native key capacity would be exceeded.";
        return Error::Capacity;
    }
    return Error::None;
}

// ---- native morph pool ----------------------------------------------------
// One key per frame of a morph track; the native pool stores linear morph keys.
bool ReadMorph(unsigned char* model, int morph, ac::Track& out, std::wstring& message) {
    auto* keys = mdl::MorphKeys(model);
    const unsigned count = mdl::Mdl(model)->morphCount;
    if (!keys || morph < 0 || static_cast<unsigned>(morph) >= count) {
        message = L"The model has no such morph.";
        return false;
    }
    out = ac::Track{};
    unsigned index = static_cast<unsigned>(morph);
    std::uint32_t previous = 0;
    const auto morphCapacity1 = mdl::MorphKeyCapacity(keys);
    for (std::size_t visited = 0; visited <= morphCapacity1; ++visited) {
        if (index >= morphCapacity1 || keys[index].frame > mikudancestudio::frame_limits::maximum ||
            (visited && keys[index].frame <= previous)) {
            message = L"The morph track is not a valid key chain.";
            return false;
        }
        ac::Key key;
        key.frame = static_cast<int>(keys[index].frame);
        key.value = keys[index].value;
        key.interpolation = ac::Interpolation::Linear;
        key.tangent = ac::Tangent::Linear;
        if (!std::isfinite(key.value)) {
            message = L"The morph track holds a non-finite value.";
            return false;
        }
        out.keys.push_back(key);
        previous = keys[index].frame;
        if (keys[index].next == 0) return true;
        index = keys[index].next;
    }
    message = L"The morph track is longer than the native pool.";
    return false;
}

// ---- native display (表示/IK) pool ----------------------------------------
struct DisplayRecord {
    int frame = 0;
    unsigned char visible = 1;
    std::vector<unsigned char> ik;
    std::vector<unsigned char> selectors;
};

bool ReadDisplay(unsigned char* model, std::vector<DisplayRecord>& out, int& chains,
                 std::size_t& selectorSlots, std::wstring& message) {
    auto* keys = mdl::DisplayKeys(model);
    if (!keys) {
        message = L"The model has no display track.";
        return false;
    }
    chains = static_cast<int>(mdl::Mdl(model)->ikChainCount);
    selectorSlots = static_cast<std::size_t>(mdl::Mdl(model)->boneOrderCount);
    out.clear();
    unsigned index = 0;
    std::uint32_t previous = 0;
    for (std::size_t visited = 0; visited <= mdl::KeyPoolCapacity(keys); ++visited) {
        if (index >= mdl::KeyPoolCapacity(keys) || keys[index].frame > mikudancestudio::frame_limits::maximum ||
            (visited && keys[index].frame <= previous)) {
            message = L"The display track is not a valid key chain.";
            return false;
        }
        DisplayRecord record;
        record.frame = static_cast<int>(keys[index].frame);
        record.visible = keys[index].visible != 0 ? 1 : 0;
        if (chains > 0) {
            const auto* ik = mdl::IkStates(keys[index]);
            if (!ik) {
                message = L"The display track is missing its IK state storage.";
                return false;
            }
            record.ik.assign(ik, ik + chains);
        }
        if (selectorSlots > 0) {
            const auto* pairs = mdl::SelectorStates(keys[index]);
            if (!pairs) {
                message = L"The display track is missing its selector storage.";
                return false;
            }
            const auto* bytes = reinterpret_cast<const unsigned char*>(pairs);
            record.selectors.assign(bytes, bytes + selectorSlots * sizeof(mdl::BoneReference));
        }
        out.push_back(std::move(record));
        previous = keys[index].frame;
        if (keys[index].next == 0) return true;
        index = keys[index].next;
    }
    message = L"The display track is longer than the native pool.";
    return false;
}

// ---- object identity ------------------------------------------------------
struct ModelIdentity {
    unsigned char* model = nullptr;
    mdl::BoneKey* boneKeys = nullptr;
    mdl::MorphKey* morphKeys = nullptr;
    mdl::DisplayKey* displayKeys = nullptr;
    unsigned boneCount = 0;
    unsigned morphCount = 0;
    unsigned ikChainCount = 0;
    unsigned boneOrderCount = 0;
    bool operator==(const ModelIdentity& other) const {
        return model == other.model && boneKeys == other.boneKeys && morphKeys == other.morphKeys &&
               displayKeys == other.displayKeys && boneCount == other.boneCount &&
               morphCount == other.morphCount && ikChainCount == other.ikChainCount &&
               boneOrderCount == other.boneOrderCount;
    }
    bool operator!=(const ModelIdentity& other) const { return !(*this == other); }
};

ModelIdentity Identify(unsigned char* model) {
    ModelIdentity identity;
    if (!model) return identity;
    identity.model = model;
    identity.boneKeys = mdl::BoneKeys(model);
    identity.morphKeys = mdl::MorphKeys(model);
    identity.displayKeys = mdl::DisplayKeys(model);
    identity.boneCount = mdl::Mdl(model)->boneCount;
    identity.morphCount = mdl::Mdl(model)->morphCount;
    identity.ikChainCount = mdl::Mdl(model)->ikChainCount;
    identity.boneOrderCount = mdl::Mdl(model)->boneOrderCount;
    return identity;
}

// ---- plan storage ---------------------------------------------------------
// One scene track: the channel view curve_host reads and writes, plus a copy of
// the native pool itself so a rollback can put back the exact bytes, including
// the key selection flags and the camera interpolation lanes that the channel
// view does not carry.
struct SceneTarget {
    Kind kind = Kind::Camera;
    int slot = 0;
    void* object = nullptr;         // the accessory record, or the track pool
    std::vector<Channel> original;  // what was there before the bake
    std::vector<Channel> baked;
    std::vector<unsigned char> pool;
    std::uint32_t cursor = 0;
    unsigned char active = 0;
};

std::size_t PoolBytes(MMDApp& app, Kind kind, int slot) {
    switch (kind) {
    case Kind::Camera: return sizeof(mdl::CameraKey) * mdl::KeyPoolCapacity(app.CameraKeys());
    case Kind::Light: return sizeof(mdl::LightKey) * mdl::KeyPoolCapacity(app.LightKeys());
    case Kind::SelfShadow: return sizeof(mdl::SelfShadowKey) * mdl::KeyPoolCapacity(app.ShadowKeys());
    case Kind::Gravity: return sizeof(mdl::GravityKey) * mdl::KeyPoolCapacity(app.GravityKeys());
    default: return sizeof(mdl::AccessoryKey) * mdl::KeyPoolCapacity(app.AccessoryKeys(slot));
    }
}

bool SnapshotScene(MMDApp& app, Kind kind, int slot, SceneTarget& target) {
    const void* pool = nullptr;
    switch (kind) {
    case Kind::Camera: pool = app.CameraKeys(); target.cursor = app.CameraTrackCursor(); target.active = app.CameraTrackActive(); break;
    case Kind::Light: pool = app.LightKeys(); target.cursor = app.LightTrackCursor(); target.active = app.LightTrackActive(); break;
    case Kind::SelfShadow: pool = app.ShadowKeys(); target.cursor = app.ShadowTrackCursor(); target.active = app.ShadowTrackActive(); break;
    case Kind::Gravity: pool = app.GravityKeys(); target.cursor = app.GravityTrackCursor(); target.active = app.GravityTrackActive(); break;
    default: pool = app.AccessoryKeys(slot); target.cursor = app.AccessoryTrackCursor(slot); target.active = app.AccessoryTrackActive(slot); break;
    }
    if (!pool) return false;
    const std::size_t bytes = PoolBytes(app, kind, slot);
    target.pool.assign(static_cast<const unsigned char*>(pool), static_cast<const unsigned char*>(pool) + bytes);
    return true;
}

// Puts one scene track back byte for byte.  The curve-host write is best effort:
// it allocates while mirroring, so it can throw, and that must not stop the
// native pool from being restored - the raw snapshot below is what makes the
// restore exact.  This never throws, because it also runs from noexcept paths
// and from inside other handlers.
void RestoreSceneTarget(MMDApp& app, SceneTarget& target) noexcept {
    try {
        // Writing the original channels also returns the curve_host state of this
        // track to what it was, so a later Read cannot serve baked values.
        std::wstring detail;
        curve_host::Write(app, HostKind(target.kind), target.slot, target.original, detail);
    } catch (...) {
        // fall through: the pool snapshot is the authoritative restore
    }
    void* pool = nullptr;
    switch (target.kind) {
    case Kind::Camera: pool = app.CameraKeys(); break;
    case Kind::Light: pool = app.LightKeys(); break;
    case Kind::SelfShadow: pool = app.ShadowKeys(); break;
    case Kind::Gravity: pool = app.GravityKeys(); break;
    default: pool = app.AccessoryKeys(target.slot); break;
    }
    if (pool && !target.pool.empty()) {
        const auto bytes = PoolBytes(app, target.kind, target.slot);
        if (bytes >= target.pool.size()) {
            std::memset(pool, 0, bytes);
            std::memcpy(pool, target.pool.data(), target.pool.size());
        }
    }
    switch (target.kind) {
    case Kind::Camera: app.CameraTrackCursor() = target.cursor; app.CameraTrackActive() = target.active; break;
    case Kind::Light: app.LightTrackCursor() = target.cursor; app.LightTrackActive() = target.active; break;
    case Kind::SelfShadow: app.ShadowTrackCursor() = target.cursor; app.ShadowTrackActive() = target.active; break;
    case Kind::Gravity: app.GravityTrackCursor() = target.cursor; app.GravityTrackActive() = target.active; break;
    default: app.AccessoryTrackCursor(target.slot) = target.cursor; app.AccessoryTrackActive(target.slot) = target.active; break;
    }
}

struct MorphTarget {
    int morph = 0;
    ac::Track baked;
    std::vector<int> drop;  // original frames inside the range the bake replaces
};

struct DisplayTarget {
    int chains = 0;
    std::size_t selectorSlots = 0;
    std::vector<DisplayRecord> records;  // every frame the baked track must hold
    std::vector<int> drop;
};

struct SavedSlot {
    int index = 0;
    mdl::DisplayKey record{};
    std::vector<unsigned char> ik;
    std::vector<unsigned char> selectors;
};

struct ModelPlan {
    ModelIdentity identity;
    std::vector<MorphTarget> morphs;
    std::vector<mdl::MorphKey> morphPool;  // whole-pool snapshot, restored on rollback
    std::uint64_t morphRequired = 0;
    bool display = false;
    std::uint64_t displayRequired = 0;
    DisplayTarget displayTarget;
    std::vector<SavedSlot> displaySaved;   // only the slots the write can touch
    std::vector<int> displayCreated;
    std::int32_t searchCursor = 0;
    std::uint32_t maxFrame = 0;
    // The bone channel is not written here; the resolved index set is recorded
    // so the caller drives bake::Compute/Commit and the FX toggle keys with
    // exactly the checked bones.
    bool bones = false;
    std::vector<int> boneIndices;
};
}  // namespace

struct Plan::Impl {
    MMDApp* app = nullptr;
    Scope scope = Scope::Model;
    Request request;
    bool prepared = false;
    // Set before the first write of a commit, cleared only by a rollback that
    // ran.  It is what makes a partially applied plan recoverable: a commit that
    // reports a failure, or that unwinds through an exception, must not leave
    // writes behind that no rollback is willing to undo.
    bool applied = false;
    std::vector<SceneTarget> scene;
    std::uint32_t lastRegisteredFrame = 0;
    unsigned char sceneModified = 0;
    ModelPlan model;
};

// ---- selection ------------------------------------------------------------
bool Selection::Empty() const {
    for (const auto& choice : choices)
        if (Active(choice.kind, choice.slot)) return false;
    return true;
}

const Choice* Selection::Find(Kind kind, int slot) const {
    for (const auto& choice : choices)
        if (choice.kind == kind && choice.slot == slot) return &choice;
    return nullptr;
}

bool Selection::Has(Kind kind, int slot) const { return Find(kind, slot) != nullptr; }

Choice& Selection::Touch(Kind kind, int slot, std::size_t fieldCount) {
    for (auto& choice : choices)
        if (choice.kind == kind && choice.slot == slot) {
            if (choice.fields.size() != fieldCount) choice.fields.assign(fieldCount, true);
            return choice;
        }
    Choice fresh;
    fresh.kind = kind;
    fresh.slot = slot;
    fresh.fields.assign(fieldCount, true);
    choices.push_back(std::move(fresh));
    return choices.back();
}

void Selection::SetField(Kind kind, int slot, std::size_t field, bool on) {
    for (auto it = choices.begin(); it != choices.end(); ++it) {
        if (it->kind != kind || it->slot != slot) continue;
        if (it->fields.size() <= field) it->fields.resize(field + 1, false);
        it->fields[field] = on;
        if (std::none_of(it->fields.begin(), it->fields.end(), [](bool value) { return value; }))
            choices.erase(it);
        return;
    }
    if (!on) return;
    Choice fresh;
    fresh.kind = kind;
    fresh.slot = slot;
    fresh.fields.assign(field + 1, false);
    fresh.fields[field] = true;
    choices.push_back(std::move(fresh));
}

void Selection::Remove(Kind kind, int slot) {
    for (auto it = choices.begin(); it != choices.end(); ++it)
        if (it->kind == kind && it->slot == slot) {
            choices.erase(it);
            return;
        }
}

bool Selection::Active(Kind kind, int slot) const {    const Choice* choice = Find(kind, slot);
    if (!choice) return false;
    if (choice->fields.empty()) return true;
    return std::any_of(choice->fields.begin(), choice->fields.end(), [](bool value) { return value; });
}

Plan::Plan() : impl_(new Impl()) {}
Plan::~Plan() = default;
Plan::Plan(Plan&&) noexcept = default;
Plan& Plan::operator=(Plan&&) noexcept = default;
bool Plan::Valid() const noexcept { return impl_ && impl_->prepared; }

void Discard(Plan& plan) noexcept { plan.impl_.reset(); }

// ---- enumeration ----------------------------------------------------------
Scope CurrentScope(MMDApp& app) { return app.CameraMode() != 0 ? Scope::Scene : Scope::Model; }

namespace {
Group Named(MMDApp& app, Kind kind, int slot, const wchar_t* ja, const wchar_t* en,
            const wchar_t* zh) {
    Group group;
    group.kind = kind;
    group.slot = slot;
    group.label = Text(app, ja, en, zh);
    return group;
}

bool FieldsOf(MMDApp& app, Kind kind, int slot, Group& group, std::wstring& message) {
    std::vector<Channel> channels;
    if (!curve_host::Read(app, HostKind(kind), slot, channels, message)) return false;
    group.fields.clear();
    group.fields.reserve(channels.size());
    for (const auto& channel : channels) {
        Field field;
        field.id = std::wstring(channel.id.begin(), channel.id.end());
        field.label = channel.label;
        field.discrete = channel.track.discrete;
        field.minimum = channel.minimum;
        field.maximum = channel.maximum;
        group.fields.push_back(std::move(field));
    }
    return !group.fields.empty();
}

const Group* FindGroup(const std::vector<Group>& groups, Kind kind, int slot) {
    for (const auto& group : groups)
        if (group.kind == kind && group.slot == slot) return &group;
    return nullptr;
}

// An empty mask means every sub-parameter of that track.
bool FieldOn(const Choice& choice, std::size_t index) {
    if (choice.fields.empty()) return true;
    return index < choice.fields.size() ? choice.fields[index] : true;
}

void* SceneObject(MMDApp& app, Kind kind, int slot) {
    switch (kind) {
    case Kind::Camera: return app.CameraKeys();
    case Kind::Light: return app.LightKeys();
    case Kind::SelfShadow: return app.ShadowKeys();
    case Kind::Gravity: return app.GravityKeys();
    default: return app.AccessorySlot(slot);
    }
}

template <class K>
int LastFrameOfPool(const K* pool, std::size_t capacity, unsigned head) {
    if (!pool) return 0;
    unsigned index = head;
    int last = 0;
    for (std::size_t visited = 0; visited < capacity; ++visited) {
        if (index >= capacity) return last;
        last = std::max(last, static_cast<int>(pool[index].frame));
        if (pool[index].next == 0) return last;
        index = pool[index].next;
    }
    return last;
}
}  // namespace

Error Enumerate(MMDApp& app, Scope scope, std::vector<Group>& groups, std::wstring& message) {
    groups.clear();
    message.clear();
    if (scope == Scope::Scene) {
        const Kind kinds[4] = {Kind::Camera, Kind::Light, Kind::SelfShadow, Kind::Gravity};
        const wchar_t* ja[4] = {L"カメラ", L"照明", L"セルフ影", L"重力"};
        const wchar_t* en[4] = {L"Camera", L"Light", L"Self shadow", L"Gravity"};
        const wchar_t* zh[4] = {L"摄像机", L"灯光", L"自阴影", L"重力"};
        for (int i = 0; i < 4; ++i) {
            Group group = Named(app, kinds[i], 0, ja[i], en[i], zh[i]);
            if (FieldsOf(app, kinds[i], 0, group, message)) groups.push_back(std::move(group));
        }
        for (int slot = 0; slot < 255; ++slot) {
            if (!app.AccessorySlot(slot)) continue;
            const std::wstring label =
                Text(app, L"アクセサリ ", L"Accessory ", L"附件 ") + std::to_wstring(slot + 1);
            Group group;
            group.kind = Kind::Accessory;
            group.slot = slot;
            group.label = label;
            if (FieldsOf(app, Kind::Accessory, slot, group, message)) groups.push_back(std::move(group));
        }
        if (groups.empty()) {
            message = Text(app, L"焼き込めるシーントラックがありません。",
                           L"There is no scene track to bake.", L"没有可烧录的场景轨道。");
            return Error::Unavailable;
        }
        return Error::None;
    }
    unsigned char* model = app.SelectedModel();
    if (!model) {
        message =
            Text(app, L"モデルが選択されていません。", L"No model is selected.", L"没有选中模型。");
        return Error::Unavailable;
    }
    const auto& record = *mdl::Mdl(model);
    if (record.boneTable && record.boneKeys && record.boneCount > 0) {
        // One sub-parameter per bone, in bone order: the field index is the bone
        // index, so a caller can tick individual bones instead of only switching
        // the whole model on or off.  Nothing is filtered by name or type here.
        Group group = Named(app, Kind::Bones, 0, L"ボーン", L"Bones", L"骨骼");
        group.fields.reserve(record.boneCount);
        for (unsigned bone = 0; bone < record.boneCount; ++bone) {
            Field field;
            field.id = L"bone" + std::to_wstring(bone);
            field.label = text_encoding::BoneLabel(record.boneTable[bone], UiUsesEnglishNames(app.EnglishUI()));
            group.fields.push_back(std::move(field));
        }
        groups.push_back(std::move(group));
    }
    if (record.morphs && record.morphKeys && record.morphCount > 0) {
        for (unsigned index = 0; index < record.morphCount; ++index) {
            Group group;
            group.kind = Kind::Morph;
            group.slot = static_cast<int>(index);
            group.label = text_encoding::MorphLabel(record.morphs[index], UiUsesEnglishNames(app.EnglishUI()));
            Field field;
            field.id = L"weight";
            field.label = Text(app, L"ウェイト", L"Weight", L"权重");
            group.fields.push_back(std::move(field));
            groups.push_back(std::move(group));
        }
    }
    if (record.displayKeys && (record.ikChainCount > 0 || record.boneOrderCount > 0)) {
        Group group = Named(app, Kind::Display, 0, L"表示・IK", L"Visibility / IK", L"显示・IK");
        Field visible;
        visible.id = L"visible";
        visible.label = Text(app, L"表示", L"Visibility", L"显示");
        visible.discrete = true;
        visible.minimum = 0;
        visible.maximum = 1;
        group.fields.push_back(std::move(visible));
        // One field per IK chain, in chain order: the field index is the lane
        // index the bake uses, so a chain whose bone cannot be named still gets
        // its own entry instead of shifting every later one.
        for (unsigned chain = 0; chain < record.ikChainCount; ++chain) {
            const int bone = record.ikChains[chain].boneIndex;
            const bool named = record.boneTable && bone >= 0 &&
                               static_cast<unsigned>(bone) < record.boneCount;
            Field field;
            field.id = L"ik" + std::to_wstring(chain);
            field.label = std::wstring(L"IK ") + std::to_wstring(chain) +
                          (named ? std::wstring(L": ") +
                                       text_encoding::BoneLabel(record.boneTable[bone],
                                                               UiUsesEnglishNames(app.EnglishUI()))
                                 : std::wstring());
            field.discrete = true;
            field.minimum = 0;
            field.maximum = 1;
            group.fields.push_back(std::move(field));
        }
        groups.push_back(std::move(group));
    }
    if (groups.empty()) {
        message = Text(app, L"焼き込めるモデルトラックがありません。",
                       L"The selected model has no track to bake.", L"当前模型没有可烧录的轨道。");
        return Error::Unavailable;
    }
    return Error::None;
}

void DefaultSelection(MMDApp& app, Scope scope, const std::vector<Group>& groups,
                      Selection& selection) {
    (void)app;
    selection.choices.clear();
    const Kind wanted = scope == Scope::Scene ? Kind::Camera : Kind::Bones;
    for (const auto& group : groups)
        if (group.kind == wanted) selection.Touch(group.kind, group.slot, group.fields.size());
}

std::wstring Summary(MMDApp& app, Scope scope, const std::vector<Group>& groups,
                     const Selection& selection) {
    std::vector<std::wstring> labels;
    for (const auto& group : groups)
        if (selection.Active(group.kind, group.slot)) labels.push_back(group.label);
    if (labels.empty())
        return Text(app, L"チャンネル未選択", L"No channel selected", L"未选择通道");
    std::wstring list;
    for (std::size_t i = 0; i < labels.size() && i < 4; ++i) {
        if (i) list += app.EnglishUI() == 1 ? L", " : L"、";
        list += labels[i];
    }
    if (labels.size() > 4) list += app.EnglishUI() == 1 ? L", ..." : L"…";
    // A bone scope other than the whole model is part of what the line must say:
    // it is the difference between "this model" and "the checked sources".
    const Choice* bone = selection.Find(Kind::Bones, 0);
    if (scope == Scope::Model && bone && bone->coverage != Coverage::Whole) {
        std::vector<int> bones;
        std::wstring detail;
        if (ResolveBones(app, selection, bones, detail) == Error::None && !bones.empty()) {
            if (app.EnglishUI() == 1)
                list += L" (" + std::to_wstring(bones.size()) + L" bones checked)";
            else if (app.EnglishUI() == 0)
                list += L"（ボーン " + std::to_wstring(bones.size()) + L" 本を選択）";
            else
                list += L"（骨骼已勾选 " + std::to_wstring(bones.size()) + L" 根）";
        }
    }
    if (app.EnglishUI() == 1) return std::to_wstring(labels.size()) + L" channel(s): " + list;
    if (app.EnglishUI() == 0) return std::to_wstring(labels.size()) + L" チャンネル：" + list;
    return std::to_wstring(labels.size()) + L" 个通道：" + list;
}

Error ResolveBones(MMDApp& app, const Selection& selection, std::vector<int>& bones,
                   std::wstring& message) {
    bones.clear();
    const Choice* choice = selection.Find(Kind::Bones, 0);
    if (!choice) return Error::None;  // no bone channel: nothing to resolve
    unsigned char* model = app.SelectedModel();
    if (!model) {
        message =
            Text(app, L"モデルが選択されていません。", L"No model is selected.", L"没有选中模型。");
        return Error::Unavailable;
    }
    const auto& record = *mdl::Mdl(model);
    if (!record.boneTable || !record.boneKeys || record.boneCount == 0) {
        message = Text(app, L"モデルにボーンがありません。", L"The model has no bones.",
                       L"该模型没有骨骼。");
        return Error::Unavailable;
    }
    if (choice->coverage == Coverage::Selected) {
        const unsigned char* marked = record.boneSelection;
        if (!marked) return Error::None;  // nothing is marked, so nothing is covered
        for (unsigned bone = 0; bone < record.boneCount; ++bone)
            if (marked[bone]) bones.push_back(static_cast<int>(bone));
        return Error::None;
    }
    if (choice->coverage == Coverage::Explicit) {
        // The recorded list is checked against this model, never clamped and
        // never widened: an index that is not a bone of the model is refused.
        std::vector<int> sorted = choice->indices;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        for (const int bone : sorted)
            if (bone < 0 || static_cast<unsigned>(bone) >= record.boneCount) {
                message = Text(app, L"ボーン番号がモデルの範囲外です。",
                               L"A bone index is outside the model.", L"骨骼序号超出该模型范围。");
                return Error::InvalidRequest;
            }
        bones = std::move(sorted);
        return Error::None;
    }
    bones.reserve(record.boneCount);
    for (unsigned bone = 0; bone < record.boneCount; ++bone) bones.push_back(static_cast<int>(bone));
    return Error::None;
}

int LastFrame(MMDApp& app, Scope scope) {
    if (scope == Scope::Model) {
        unsigned char* model = app.SelectedModel();
        return model ? static_cast<int>(mdl::Mdl(model)->maxFrame) : 0;
    }
    int last = std::max(LastFrameOfPool(app.CameraKeys(), mdl::KeyPoolCapacity(app.CameraKeys()), 0u),
                        LastFrameOfPool(app.LightKeys(), mdl::KeyPoolCapacity(app.LightKeys()), 0u));
    last = std::max(last, LastFrameOfPool(app.ShadowKeys(), mdl::KeyPoolCapacity(app.ShadowKeys()), 0u));
    last = std::max(last, LastFrameOfPool(app.GravityKeys(), mdl::KeyPoolCapacity(app.GravityKeys()), 0u));
    for (int slot = 0; slot < 255; ++slot)
        if (app.AccessorySlot(slot))
            last = std::max(last, LastFrameOfPool(app.AccessoryKeys(slot), mdl::KeyPoolCapacity(app.AccessoryKeys(slot)), 0u));
    return last;
}

// ---- prepare --------------------------------------------------------------
int SceneLastFrame(MMDApp& app) {
    int last=LastFrame(app,Scope::Scene);
    for(int slot=0;slot<kModelSlotCount;++slot)
        if(auto* model=app.ModelSlot(slot))
            last=std::max(last,static_cast<int>(mdl::Mdl(model)->maxFrame));
    return last;
}

namespace {
Error PrepareScene(Plan::Impl& impl, MMDApp& app, const std::vector<Group>& groups,
                   std::wstring& message, bool& any) {
    for (const auto& choice : impl.request.selection.choices) {
        if (!IsSceneKind(choice.kind)) continue;
        if (!impl.request.selection.Active(choice.kind, choice.slot)) continue;
        const Group* group = FindGroup(groups, choice.kind, choice.slot);
        if (!group) {
            message = Text(app, L"対象のトラックがありません。", L"A selected track is unavailable.",
                           L"所选的轨道不存在。");
            return Error::Unavailable;
        }
        SceneTarget target;
        target.kind = choice.kind;
        target.slot = choice.slot;
        target.object = SceneObject(app, choice.kind, choice.slot);
        if (!target.object) {
            message = Text(app, L"トラックの保存領域がありません。", L"The track storage is unavailable.",
                           L"轨道存储不可用。");
            return Error::Unavailable;
        }
        if(!curve_host::Read(app,HostKind(choice.kind),choice.slot,target.original,message))
            return Error::Unavailable;
        if(!SnapshotScene(app,choice.kind,choice.slot,target)){
            message = Text(app, L"トラックの保存領域がありません。", L"The track storage is unavailable.",
                           L"轨道存储不可用。");
            return Error::Unavailable;
        }
        auto boundaries = target.original;
        if (choice.kind == Kind::Camera) for (std::size_t c=0;c<boundaries.size();++c) {
            if (!FieldOn(choice,c)) continue;
            for (int frame : {impl.request.firstFrame,impl.request.lastFrame})
                if (!curve_host::InsertCameraKey(boundaries,c,frame,message)) return Error::Unrepresentable;
        }
        target.baked = target.original;
        for (std::size_t c = 0; c < target.baked.size(); ++c) {
            if (!FieldOn(choice, c)) continue;
            const Error sampled =
                Resample(app, boundaries[c].track, impl.request.firstFrame,
                         impl.request.lastFrame, impl.request.step, target.baked[c].track, message, &target.original[c]);
            if (sampled != Error::None) return sampled;
            auto& baked=target.baked[c];
            const auto& original=boundaries[c];
            baked.nativeControls.assign(baked.track.keys.size(),{});
            baked.controlGeometry.assign(baked.track.keys.size(),{});
            baked.nativeOneFrameHolds.assign(baked.track.keys.size(),true);
            for(std::size_t end=1;end<baked.track.keys.size();++end){
                const auto& a=baked.track.keys[end-1];const auto& b=baked.track.keys[end];
                auto at=std::lower_bound(original.track.keys.begin(),original.track.keys.end(),b.frame,
                    [](const auto& key,int frame){return key.frame<frame;});
                const auto n=std::size_t(at-original.track.keys.begin());
                if(at==original.track.keys.end()||!n||at->frame!=b.frame||original.track.keys[n-1].frame!=a.frame||
                    n>=original.nativeControls.size()||n>=original.controlGeometry.size())continue;
                const std::array<double,4> geometry={a.outWeight,a.outSlope,b.inWeight,b.inSlope};
                if(geometry!=original.controlGeometry[n]||b.interpolation!=ac::Interpolation::Bezier)continue;
                baked.nativeControls[end]=original.nativeControls[n];baked.controlGeometry[end]=geometry;
                if(n<original.nativeOneFrameHolds.size())baked.nativeOneFrameHolds[end]=original.nativeOneFrameHolds[n];
            }
        }
        const Error valid = ValidChannels(target.baked, group->fields, message);
        if (valid != Error::None) {
            const std::wstring detail = message;
            message = Text(app, L"焼き込めない値または容量です。",
                           L"The baked values or the key capacity are invalid.",
                           L"烧录值或关键帧容量无效。") +
                      L" " + detail;
            return valid;
        }
        any = true;
        impl.scene.push_back(std::move(target));
    }
    return Error::None;
}

Error PrepareMorph(Plan::Impl& impl, MMDApp& app, int morph, std::wstring& message) {
    unsigned char* model = app.SelectedModel();
    ac::Track original;
    if (!ReadMorph(model, morph, original, message)) return Error::Unavailable;
    MorphTarget target;
    target.morph = morph;
    if (impl.request.morphSource) {
        // The values are the MMV layer cutoff OUTPUT (for example a morph the
        // layers changed); only the shape of the untouched outside curve comes
        // from the original track, through the same boundary restores the
        // resample uses.  A frame the layer cannot supply refuses the bake
        // instead of silently keeping the old value.
        ac::Track sampled;
        sampled.discrete = false;
        for (const auto& key : original.keys)
            if (key.frame < impl.request.firstFrame || key.frame > impl.request.lastFrame)
                sampled.keys.push_back(key);
        for (std::int64_t frame = impl.request.firstFrame; frame <= impl.request.lastFrame;
             frame += impl.request.step) {
            float value = 0.0f;
            if (!impl.request.morphSource(morph, static_cast<int>(frame), value) ||
                !std::isfinite(value)) {
                message = Text(app, L"この層の出力に該当モーフの値がありません。",
                               L"The layer output has no value for a selected morph.",
                               L"层输出中缺少所选表情的数值。");
                return Error::InvalidRequest;
            }
            ac::Key key;
            key.frame = static_cast<int>(frame);
            key.value = value;
            key.interpolation = ac::Interpolation::Linear;
            key.tangent = ac::Tangent::Linear;
            key.inWeight = kMinimumWeight;
            key.outWeight = kMinimumWeight;
            auto at = std::lower_bound(sampled.keys.begin(), sampled.keys.end(), key.frame,
                                       [](const ac::Key& a, int f) { return a.frame < f; });
            if (at != sampled.keys.end() && at->frame == key.frame)
                *at = key;
            else
                sampled.keys.insert(at, key);
        }
        if (sampled.keys.empty() || sampled.keys.back().frame != impl.request.lastFrame) {
            float value = 0.0f;
            if (!impl.request.morphSource(morph, impl.request.lastFrame, value) ||
                !std::isfinite(value)) {
                message = Text(app, L"この層の出力に該当モーフの値がありません。",
                               L"The layer output has no value for a selected morph.",
                               L"层输出中缺少所选表情的数值。");
                return Error::InvalidRequest;
            }
            ac::Key key;
            key.frame = impl.request.lastFrame;
            key.value = value;
            key.interpolation = ac::Interpolation::Linear;
            key.tangent = ac::Tangent::Linear;
            key.inWeight = kMinimumWeight;
            key.outWeight = kMinimumWeight;
            auto at = std::lower_bound(sampled.keys.begin(), sampled.keys.end(), key.frame,
                                       [](const ac::Key& a, int f) { return a.frame < f; });
            if (at != sampled.keys.end() && at->frame == key.frame)
                *at = key;
            else
                sampled.keys.insert(at, key);
        }
        const Error left = RestoreLeftBoundary(app, sampled, original, impl.request.firstFrame,
                                               false, message);
        if (left != Error::None) return left;
        const Error right = RestoreRightBoundary(app, sampled, original, impl.request.lastFrame,
                                                 false, message);
        if (right != Error::None) return right;
        target.baked = std::move(sampled);
    } else {
        const Error sampled =
            Resample(app, original, impl.request.firstFrame, impl.request.lastFrame, impl.request.step,
                     target.baked, message);
        if (sampled != Error::None) return sampled;
    }
    // Only the original keys the bake does not re-create have to be unlinked:
    // a frame the baked track still holds keeps its slot.
    std::set<int> baked;
    for (const auto& key : target.baked.keys) baked.insert(key.frame);
    for (const auto& key : original.keys)
        if (key.frame >= impl.request.firstFrame && key.frame <= impl.request.lastFrame &&
            !baked.count(key.frame))
            target.drop.push_back(key.frame);
    impl.model.morphs.push_back(std::move(target));
    return Error::None;
}

Error PrepareDisplay(Plan::Impl& impl, MMDApp& app, const Choice& choice, std::wstring& message) {
    unsigned char* model = app.SelectedModel();
    std::vector<DisplayRecord> records;
    int chains = 0;
    std::size_t selectorSlots = 0;
    if (!ReadDisplay(model, records, chains, selectorSlots, message)) return Error::Unavailable;
    // Lane 0 is visibility; every following lane is one IK chain.
    std::vector<ac::Track> lanes(static_cast<std::size_t>(chains) + 1);
    for (auto& lane : lanes) lane.discrete = true;
    for (const auto& record : records) {
        ac::Key visible;
        visible.frame = record.frame;
        visible.value = record.visible;
        visible.interpolation = ac::Interpolation::Step;
        visible.tangent = ac::Tangent::Flat;
        lanes[0].keys.push_back(visible);
        for (int chain = 0; chain < chains; ++chain) {
            ac::Key key = visible;
            key.value = record.ik[static_cast<std::size_t>(chain)];
            lanes[static_cast<std::size_t>(chain) + 1].keys.push_back(key);
        }
    }
    for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
        if (!FieldOn(choice, lane)) continue;
        ac::Track baked;
        const Error sampled =
            Resample(app, lanes[lane], impl.request.firstFrame, impl.request.lastFrame,
                     impl.request.step, baked, message);
        if (sampled != Error::None) return sampled;
        lanes[lane] = std::move(baked);
    }
    std::set<int> frames;
    for (const auto& lane : lanes)
        for (const auto& key : lane.keys) frames.insert(key.frame);
    // Selector pairs (外親) are not a baked lane: an existing frame keeps its own
    // state, a new frame inherits the state that was in effect there.
    std::size_t governing = 0;
    for (const int frame : frames) {
        DisplayRecord out;
        out.frame = frame;
        out.visible = ac::Evaluate(lanes[0], frame).value != 0.0 ? 1 : 0;
        for (int chain = 0; chain < chains; ++chain)
            out.ik.push_back(ac::Evaluate(lanes[static_cast<std::size_t>(chain) + 1], frame).value != 0.0
                                 ? 1
                                 : 0);
        const auto found = std::find_if(records.begin(), records.end(),
                                        [&](const DisplayRecord& r) { return r.frame == frame; });
        if (found != records.end()) {
            out.selectors = found->selectors;
            governing = static_cast<std::size_t>(found - records.begin());
        } else {
            out.selectors = records[governing].selectors;
        }
        impl.model.displayTarget.records.push_back(std::move(out));
    }
    for (const auto& record : records)
        if (record.frame >= impl.request.firstFrame && record.frame <= impl.request.lastFrame &&
            !frames.count(record.frame))
            impl.model.displayTarget.drop.push_back(record.frame);
    impl.model.displayTarget.chains = chains;
    impl.model.displayTarget.selectorSlots = selectorSlots;
    impl.model.display = true;
    return Error::None;
}

Error PrepareModel(Plan::Impl& impl, MMDApp& app, const std::vector<Group>& groups,
                   std::wstring& message, bool& any) {
    unsigned char* model = app.SelectedModel();
    if (!model) {
        message =
            Text(app, L"モデルが選択されていません。", L"No model is selected.", L"没有选中模型。");
        return Error::Unavailable;
    }
    impl.model.identity = Identify(model);
    impl.model.searchCursor = mdl::Mdl(model)->searchCursor;
    impl.model.maxFrame = mdl::Mdl(model)->maxFrame;
    for (const auto& choice : impl.request.selection.choices) {
        if (IsSceneKind(choice.kind)) continue;
        if (!impl.request.selection.Active(choice.kind, choice.slot)) continue;
        if (choice.kind == Kind::Bones) {
            // The bone pools stay with bake::Compute/Commit; this module only
            // resolves and records the exact index set the caller ticked, so a
            // checked source is never widened to the whole model.
            std::vector<int> bones;
            const Error resolved = ResolveBones(app, impl.request.selection, bones, message);
            if (resolved != Error::None) return resolved;
            impl.model.bones = true;
            impl.model.boneIndices = std::move(bones);
            if (!impl.model.boneIndices.empty()) any = true;
            continue;
        }
        if (!FindGroup(groups, choice.kind, choice.slot)) {
            message = Text(app, L"対象のトラックがありません。", L"A selected track is unavailable.",
                           L"所选的轨道不存在。");
            return Error::Unavailable;
        }
        if (choice.kind == Kind::Morph) {
            const Error prepared = PrepareMorph(impl, app, choice.slot, message);
            if (prepared != Error::None) return prepared;
        } else if (choice.kind == Kind::Display) {
            const Error prepared = PrepareDisplay(impl, app, choice, message);
            if (prepared != Error::None) return prepared;
        }
        any = true;
    }
    return Error::None;
}

// Verifies the capacities of both model pools before anything is written.
Error CheckModelCapacity(Plan::Impl& impl, MMDApp& app, std::wstring& message) {
    unsigned char* model = app.SelectedModel();
    if (!impl.model.morphs.empty()) {
        auto* keys = mdl::MorphKeys(model);
        if (!keys) {
            message = Text(app, L"モーフキーの保存領域がありません。", L"The morph pool is unavailable.",
                           L"表情关键帧存储不可用。");
            return Error::Unavailable;
        }
        std::size_t free = 0;
        const auto morphCapacity2 = mdl::MorphKeyCapacity(keys);
        for (std::size_t index = impl.model.identity.morphCount; index < morphCapacity2; ++index)
            if (keys[index].frame == 0) ++free;
        std::size_t needed = 0;
        for (const auto& target : impl.model.morphs) {
            std::set<int> present;
            unsigned index = static_cast<unsigned>(target.morph);
            const auto morphCapacity3 = mdl::MorphKeyCapacity(keys);
            for (std::size_t visited = 0; visited <= morphCapacity3; ++visited) {
                if (index >= morphCapacity3) break;
                present.insert(static_cast<int>(keys[index].frame));
                if (keys[index].next == 0) break;
                index = keys[index].next;
            }
            for (const auto& key : target.baked.keys)
                if (!present.count(key.frame)) ++needed;
            free += target.drop.size();
        }
        const std::uint64_t required = std::uint64_t(mdl::MorphKeyCapacity(keys)) +
            (needed > free ? needed - free : 0);
        if (required > mdl::kMorphKeyCapacityLimit) {
            message = Text(app, L"モーフキーのインデックス範囲を超えました。",
                           L"The morph key indices would overflow.", L"表情关键帧索引超出可表示范围。");
            return Error::Capacity;
        }
        // Preparation only snapshots. Reserve the extra slots at commit so
        // cancelling the dialog never changes the live pool.
        impl.model.morphRequired = required;
        impl.model.morphPool.assign(keys, keys + mdl::MorphKeyCapacity(keys));
    }
    if (impl.model.display) {
        auto* keys = mdl::DisplayKeys(model);
        if (!keys) {
            message = Text(app, L"表示キーの保存領域がありません。", L"The display pool is unavailable.",
                           L"显示关键帧存储不可用。");
            return Error::Unavailable;
        }
        std::vector<DisplayRecord> records;
        int chains = 0;
        std::size_t selectorSlots = 0;
        if (!ReadDisplay(model, records, chains, selectorSlots, message)) return Error::Unavailable;
        std::size_t free = 0;
        for (std::size_t index = 1; index < mdl::KeyPoolCapacity(keys); ++index)
            if (keys[index].frame == 0) ++free;
        std::size_t needed = 0;
        for (const auto& out : impl.model.displayTarget.records)
            if (std::none_of(records.begin(), records.end(),
                             [&](const DisplayRecord& r) { return r.frame == out.frame; }))
                ++needed;
        free += impl.model.displayTarget.drop.size();
        const auto capacity = mdl::KeyPoolCapacity(keys);
        impl.model.displayRequired = std::uint64_t(capacity) + (needed > free ? needed - free : 0);
        if (impl.model.displayRequired > mdl::KeyPoolLimit<mdl::DisplayKey>()) return Error::Capacity;
        impl.model.displayCreated.reserve(impl.model.displayTarget.records.size());
        // Snapshot the records the write can touch, so a rollback restores the
        // pool without ever having to move a key to another slot.
        for (std::size_t index = 0; index < mdl::KeyPoolCapacity(keys); ++index) {
            if (index != 0 && keys[index].frame == 0) continue;
            SavedSlot saved;
            saved.index = static_cast<int>(index);
            saved.record = keys[index];
            if (chains > 0 && mdl::IkStates(keys[index]))
                saved.ik.assign(mdl::IkStates(keys[index]), mdl::IkStates(keys[index]) + chains);
            if (selectorSlots > 0 && mdl::SelectorStates(keys[index])) {
                const auto* bytes =
                    reinterpret_cast<const unsigned char*>(mdl::SelectorStates(keys[index]));
                saved.selectors.assign(bytes, bytes + selectorSlots * sizeof(mdl::BoneReference));
            }
            impl.model.displaySaved.push_back(std::move(saved));
        }
    }
    return Error::None;
}
}  // namespace

Error Prepare(MMDApp& app, const Request& request, Plan& plan, std::wstring& message) {
    message.clear();
    Discard(plan);
    if (request.step < 1 || request.firstFrame < 0 || request.lastFrame < request.firstFrame ||
        request.lastFrame > kMaximumFrame) {
        message = Text(app, L"範囲またはステップが不正です。", L"The range or the step is invalid.",
                       L"范围或步长无效。");
        return Error::InvalidRequest;
    }
    std::vector<Group> groups;
    const Error listed = Enumerate(app, request.scope, groups, message);
    if (listed != Error::None) return listed;
    std::unique_ptr<Plan::Impl> impl;
    try {
        impl = std::make_unique<Plan::Impl>();
        impl->app = &app;
        impl->scope = request.scope;
        impl->request = request;
        impl->lastRegisteredFrame = app.LastRegisteredFrame();
        impl->sceneModified = app.SceneModified();
        bool any = false;
        const Error prepared = request.scope == Scope::Scene
                                   ? PrepareScene(*impl, app, groups, message, any)
                                   : PrepareModel(*impl, app, groups, message, any);
        if (prepared != Error::None) return prepared;
        if (!any) {
            message = Text(app, L"焼き込むチャンネルが選択されていません。",
                           L"No channel is selected to bake.", L"没有选择要烧录的通道。");
            return Error::NoSelection;
        }
        const Error capacity = CheckModelCapacity(*impl, app, message);
        if (capacity != Error::None) return capacity;
    } catch (const std::bad_alloc&) {
        message = Text(app, L"メモリが不足しています。", L"Not enough memory.", L"内存不足。");
        return Error::Memory;
    }
    impl->prepared = true;
    plan.impl_ = std::move(impl);
    return Error::None;
}

// ---- commit ---------------------------------------------------------------
namespace {
// `cursor` is the running free-slot scan; it only ever advances, so the whole
// commit stays linear in the number of written keys.
bool WriteMorph(unsigned char* model, int frame, float value, std::size_t& cursor, int& current) {
    auto* keys = mdl::MorphKeys(model);
    const auto allocate = [&]() -> int {
        const auto morphCapacity4 = mdl::MorphKeyCapacity(keys);
        while (cursor < morphCapacity4 && keys[cursor].frame != 0) ++cursor;
        if (cursor >= morphCapacity4) return -1;
        return static_cast<int>(cursor);
    };
    if (keys[current].frame < static_cast<std::uint32_t>(frame)) {
        for (;;) {
            const int next = static_cast<int>(keys[current].next);
            if (next == 0) break;
            current = next;
            if (keys[current].frame >= static_cast<std::uint32_t>(frame)) break;
        }
        if (keys[current].frame < static_cast<std::uint32_t>(frame)) {
            const int fresh = allocate();
            if (fresh < 0) return false;
            keys[current].next = static_cast<std::uint32_t>(fresh);
            keys[fresh] = mdl::MorphKey{};
            keys[fresh].previous = static_cast<std::uint32_t>(current);
            keys[fresh].frame = static_cast<std::uint32_t>(frame);
            keys[fresh].value = value;
            keys[fresh].allocated = 1;
            current = fresh;
            if (frame > static_cast<int>(mdl::Mdl(model)->maxFrame))
                mdl::Mdl(model)->maxFrame = static_cast<std::uint32_t>(frame);
            return true;
        }
    }
    if (keys[current].frame == static_cast<std::uint32_t>(frame)) {
        keys[current].value = value;
        keys[current].allocated = 1;
        return true;
    }
    const int fresh = allocate();
    if (fresh < 0) return false;
    const int previous = static_cast<int>(keys[current].previous);
    keys[previous].next = static_cast<std::uint32_t>(fresh);
    keys[fresh] = mdl::MorphKey{};
    keys[fresh].previous = static_cast<std::uint32_t>(previous);
    keys[fresh].next = static_cast<std::uint32_t>(current);
    keys[current].previous = static_cast<std::uint32_t>(fresh);
    keys[fresh].frame = static_cast<std::uint32_t>(frame);
    keys[fresh].value = value;
    keys[fresh].allocated = 1;
    current = fresh;
    if (frame > static_cast<int>(mdl::Mdl(model)->maxFrame))
        mdl::Mdl(model)->maxFrame = static_cast<std::uint32_t>(frame);
    return true;
}

bool UnlinkMorph(unsigned char* model, int morph, int frame) {
    auto* keys = mdl::MorphKeys(model);
    unsigned index = static_cast<unsigned>(morph);
    const auto morphCapacity5 = mdl::MorphKeyCapacity(keys);
    for (std::size_t visited = 0; visited <= morphCapacity5; ++visited) {
        if (index >= morphCapacity5) return false;
        const unsigned next = keys[index].next;
        if (static_cast<int>(keys[index].frame) == frame) {
            if (index == static_cast<unsigned>(morph)) return false;  // never the head
            const unsigned previous = keys[index].previous;
            keys[previous].next = next;
            if (next) keys[next].previous = previous;
            keys[index] = mdl::MorphKey{};
            return true;
        }
        if (next == 0) return false;
        index = next;
    }
    return false;
}

void ClearDisplaySlot(unsigned char* model, unsigned index) {
    auto* keys = mdl::DisplayKeys(model);
    auto* ik = mdl::IkStates(keys[index]);
    auto* pairs = mdl::SelectorStates(keys[index]);
    const unsigned chains = mdl::Mdl(model)->ikChainCount;
    const unsigned orders = mdl::Mdl(model)->boneOrderCount;
    keys[index].frame = 0;
    keys[index].previous = 0;
    keys[index].next = 0;
    keys[index].visible = 1;
    keys[index].allocated = 0;
    for (unsigned chain = 0; chain < chains && ik; ++chain) ik[chain] = 1;
    for (unsigned order = 0; order < orders && pairs; ++order) {
        pairs[order].modelIndex = -1;
        pairs[order].boneIndex = 0;
    }
}

bool UnlinkDisplay(unsigned char* model, int frame) {
    auto* keys = mdl::DisplayKeys(model);
    if (frame == 0) return false;  // the head keeps frame zero
    unsigned index = 0;
    for (std::size_t visited = 0; visited <= mdl::KeyPoolCapacity(keys); ++visited) {
        if (index >= mdl::KeyPoolCapacity(keys)) return false;
        const unsigned next = keys[index].next;
        if (static_cast<int>(keys[index].frame) == frame) {
            if (index == 0) return false;
            const unsigned previous = keys[index].previous;
            keys[previous].next = next;
            if (next) keys[next].previous = previous;
            ClearDisplaySlot(model, index);
            return true;
        }
        if (next == 0) return false;
        index = next;
    }
    return false;
}

bool WriteDisplay(unsigned char* model, const DisplayRecord& record, std::size_t& cursor,
                  std::vector<int>& created) {
    auto* keys = mdl::DisplayKeys(model);
    unsigned current = 0;
    for (;;) {
        const unsigned next = keys[current].next;
        if (next == 0) break;
        if (static_cast<int>(keys[next].frame) > record.frame) break;
        current = next;
    }
    const auto fill = [&](unsigned index) {
        keys[index].visible = record.visible;
        auto* ik = mdl::IkStates(keys[index]);
        auto* pairs = mdl::SelectorStates(keys[index]);
        for (std::size_t chain = 0; chain < record.ik.size() && ik; ++chain)
            ik[chain] = record.ik[chain];
        if (!record.selectors.empty() && pairs)
            std::memcpy(pairs, record.selectors.data(), record.selectors.size());
        keys[index].allocated = 1;
    };
    if (static_cast<int>(keys[current].frame) == record.frame) {
        fill(current);
        return true;
    }
    if (static_cast<int>(keys[current].frame) > record.frame) return false;
    while (cursor < mdl::KeyPoolCapacity(keys) && (cursor < 1 || keys[cursor].frame != 0)) ++cursor;
    if (cursor >= mdl::KeyPoolCapacity(keys)) return false;
    const unsigned fresh = static_cast<unsigned>(cursor);
    keys[fresh].frame = static_cast<std::uint32_t>(record.frame);
    keys[fresh].previous = current;
    keys[fresh].next = keys[current].next;
    if (keys[fresh].next) keys[keys[fresh].next].previous = fresh;
    keys[current].next = fresh;
    fill(fresh);
    created.push_back(static_cast<int>(fresh));
    if (record.frame > static_cast<int>(mdl::Mdl(model)->maxFrame))
        mdl::Mdl(model)->maxFrame = static_cast<std::uint32_t>(record.frame);
    return true;
}

// Restores the model pools from the plan: the morph pool as a whole, the
// display pool slot by slot, then the slots this bake allocated.  Never throws:
// it only copies buffers and walks the plan's own vectors.
void RestoreModel(MMDApp& app, Plan::Impl& impl, unsigned char* model) noexcept {
    (void)app;
    if (!impl.model.morphPool.empty()) {
        auto* keys = mdl::MorphKeys(model);
        const auto capacity = mdl::MorphKeyCapacity(keys);
        if (capacity > impl.model.morphPool.size())
            std::memset(keys + impl.model.morphPool.size(), 0,
                        (capacity - impl.model.morphPool.size()) * sizeof(mdl::MorphKey));
        std::memcpy(mdl::MorphKeys(model), impl.model.morphPool.data(),
                    impl.model.morphPool.size() * sizeof(mdl::MorphKey));
    }
    // The frame extent and the shared allocation cursor move for morph *and*
    // display writes, so they are restored whenever the model was touched - not
    // only when a morph pool was recorded.
    mdl::Mdl(model)->maxFrame = impl.model.maxFrame;
    mdl::Mdl(model)->searchCursor = impl.model.searchCursor;
    for (const auto& saved : impl.model.displaySaved) {
        if (saved.index < 0 || saved.index >= static_cast<int>(mdl::KeyPoolCapacity(mdl::DisplayKeys(model)))) continue;
        mdl::DisplayKey& record = mdl::DisplayKeys(model)[saved.index];
        record = saved.record;
        auto* ik = mdl::IkStates(record);
        auto* pairs = mdl::SelectorStates(record);
        if (!saved.ik.empty() && ik) std::memcpy(ik, saved.ik.data(), saved.ik.size());
        if (!saved.selectors.empty() && pairs)
            std::memcpy(pairs, saved.selectors.data(), saved.selectors.size());
    }
    for (const int index : impl.model.displayCreated) {
        if (index <= 0 || index >= static_cast<int>(mdl::KeyPoolCapacity(mdl::DisplayKeys(model)))) continue;
        const bool saved = std::any_of(impl.model.displaySaved.begin(), impl.model.displaySaved.end(),
                                       [&](const SavedSlot& slot) { return slot.index == index; });
        if (!saved) ClearDisplaySlot(model, static_cast<unsigned>(index));
    }
    impl.model.displayCreated.clear();
}

// Best-effort undo of everything a commit may have touched, plus the scene
// bookkeeping it moved.  Every target is restored (the raw snapshots make that
// exact even for a target the commit only partly wrote), and it never throws, so
// it is safe inside a noexcept rollback and inside a commit that is unwinding.
void RestoreApplied(MMDApp& app, Plan::Impl& impl) noexcept {
    if (impl.scope == Scope::Model) {
        unsigned char* model = app.SelectedModel();
        if (model && Identify(model) == impl.model.identity) RestoreModel(app, impl, model);
    } else {
        for (auto& target : impl.scene) RestoreSceneTarget(app, target);
    }
    app.LastRegisteredFrame() = impl.lastRegisteredFrame;
    app.SceneModified() = impl.sceneModified;
}
}  // namespace

Error Commit(MMDApp& app, Plan& plan, std::wstring& message) {
    message.clear();
    if (!plan.impl_ || !plan.impl_->prepared) {
        message = Text(app, L"準備されていない焼き込みです。", L"The bake was not prepared.",
                       L"该烧录未准备。");
        return Error::InvalidRequest;
    }
    Plan::Impl& impl = *plan.impl_;
    if (impl.app != &app) {
        message = Text(app, L"アプリケーションが変わりました。", L"The application changed.",
                       L"应用程序已改变。");
        return Error::Identity;
    }
    // Every target object is identified again: a model or accessory that was
    // swapped or deleted while the dialog was open never receives baked keys.
    if (impl.scope == Scope::Model) {
        if (Identify(app.SelectedModel()) != impl.model.identity) {
            message = Text(app, L"モデルが切り替えられたか削除されました。",
                           L"The model was switched or deleted.", L"模型已切换或被删除。");
            return Error::Identity;
        }
    } else {
        for (const auto& target : impl.scene)
            if (SceneObject(app, target.kind, target.slot) != target.object) {
                message = Text(app, L"対象オブジェクトが切り替えられました。",
                               L"A target object changed.", L"目标对象已改变。");
                return Error::Identity;
            }
    }
    // From the first write on, the plan has to stay recoverable: an exception
    // anywhere in the write phase (curve_host allocating while it mirrors, a
    // container growing) must not leave keys behind that no rollback will undo,
    // so `applied` is set before anything is written and every failure path -
    // including an exception - restores every target and the bookkeeping.
    if (impl.model.morphRequired) {
        auto& model = *mdl::Mdl(app.SelectedModel());
        if (!mdl::MorphKeyPoolEnsure(model, impl.model.morphRequired)) {
            message = Text(app, L"モーフの焼き込みに必要なメモリが不足しています。",
                           L"Not enough memory for the morph bake.", L"内存不足，无法烘焙表情关键帧。");
            return Error::Memory;
        }
        impl.model.identity.morphKeys = model.morphKeys;
    }
    if (impl.model.displayRequired) {
        auto& model = *mdl::Mdl(app.SelectedModel());
        if (!mdl::DisplayKeyPoolEnsure(model, impl.model.displayRequired)) {
            message = Text(app, L"表示キーのメモリを確保できません。",
                           L"Unable to allocate display key storage.", L"无法扩展显示关键帧存储，烧录已取消。");
            return Error::Memory;
        }
        impl.model.identity.displayKeys = model.displayKeys;
    }
    impl.applied = true;
    try {
        for (const auto& target : impl.scene) {
            if (!curve_host::Write(app, HostKind(target.kind), target.slot, target.baked, message)) {
                RestoreApplied(app, impl);
                return Error::Unavailable;
            }
        }
        if (!impl.model.morphs.empty() || impl.model.display) {
            unsigned char* model = app.SelectedModel();
            bool ok = true;
            // Unlink every replaced key first, so a slot freed by one morph is
            // available to every morph that follows, exactly as Prepare counted it.
            for (const auto& target : impl.model.morphs)
                for (const int frame : target.drop) UnlinkMorph(model, target.morph, frame);
            std::size_t morphCursor = impl.model.identity.morphCount;
            for (const auto& target : impl.model.morphs) {
                int current = target.morph;
                for (const auto& key : target.baked.keys)
                    if (!WriteMorph(model, key.frame, static_cast<float>(key.value),
                                    morphCursor, current)) {
                        ok = false;
                        break;
                    }
                if (!ok) break;
            }
            if (ok && impl.model.display) {
                for (const int frame : impl.model.displayTarget.drop) UnlinkDisplay(model, frame);
                std::size_t cursor = 1;
                for (const auto& record : impl.model.displayTarget.records)
                    if (!WriteDisplay(model, record, cursor, impl.model.displayCreated)) {
                        ok = false;
                        break;
                    }
            }
            if (!ok) {
                RestoreApplied(app, impl);
                message = Text(app, L"焼き込みを完了できず、元の状態に戻しました。",
                               L"The bake could not be completed and was rolled back.",
                               L"烧录未能完成，已恢复原状。");
                return Error::Capacity;
            }
        }
    } catch (const std::bad_alloc&) {
        // Restore first, then report: the message itself allocates.
        RestoreApplied(app, impl);
        message = Text(app, L"メモリが不足しています。", L"Not enough memory.", L"内存不足。");
        return Error::Memory;
    } catch (...) {
        RestoreApplied(app, impl);
        message = Text(app, L"焼き込み中にエラーが発生したため、元の状態に戻しました。",
                       L"The bake failed while writing, so the original state was restored.",
                       L"写入过程中出错，已恢复原状。");
        return Error::Unavailable;
    }
    return Error::None;
}

bool PreparedBones(const Plan& plan, std::vector<int>& bones) {
    bones.clear();
    if (!plan.impl_ || !plan.impl_->prepared || plan.impl_->scope != Scope::Model) return false;
    if (!plan.impl_->model.bones) return false;
    bones = plan.impl_->model.boneIndices;
    return true;
}

// noexcept: this runs while the caller is unwinding, so it must never throw and
// never terminate.  A plan that has been written to stays rollback-able even
// when its commit reported a failure, so a caller that unwinds through an
// exception can still finish the undo.
bool Rollback(MMDApp& app, Plan& plan) noexcept {
    if (!plan.impl_ || !plan.impl_->applied) return false;
    Plan::Impl& impl = *plan.impl_;
    if (impl.app != &app) return false;
    if (impl.scope == Scope::Model) {
        // A model that was swapped or deleted cannot be undone without writing
        // into the wrong object; the plan stays applied so a later call can.
        unsigned char* model = app.SelectedModel();
        if (!model || Identify(model) != impl.model.identity) return false;
    }
    try {
        RestoreApplied(app, impl);
    } catch (...) {
        // RestoreApplied is written not to throw; if it ever does, the plan must
        // not claim a clean undo.
        return false;
    }
    impl.applied = false;
    return true;
}

const wchar_t* ErrorText(MMDApp& app, Error error) {
    switch (error) {
    case Error::None: return L"";
    case Error::InvalidRequest:
        return app.EnglishUI() == 0   ? L"焼き込みの範囲またはステップが不正です。"
               : app.EnglishUI() == 1 ? L"The bake range or the step is invalid."
                                      : L"烧录范围或步长无效。";
    case Error::Unavailable:
        return app.EnglishUI() == 0   ? L"対象のトラックを読み書きできません。"
               : app.EnglishUI() == 1 ? L"A target track cannot be read or written."
                                      : L"无法读写目标轨道。";
    case Error::NoSelection:
        return app.EnglishUI() == 0   ? L"焼き込むチャンネルが選択されていません。"
               : app.EnglishUI() == 1 ? L"No channel is selected to bake."
                                      : L"没有选择要烧录的通道。";
    case Error::Capacity:
        return app.EnglishUI() == 0   ? L"キー容量を超えるため、何も書き込みませんでした。"
               : app.EnglishUI() == 1 ? L"The key capacity would be exceeded, so nothing was written."
                                      : L"超过关键帧容量，因此未写入任何内容。";
    case Error::Identity:
        return app.EnglishUI() == 0 ? L"対象オブジェクトが切り替えられたため、何も書き込みませんでした。"
               : app.EnglishUI() == 1
                   ? L"The target object changed, so nothing was written."
                   : L"目标对象已改变，因此未写入任何内容。";
    case Error::Unrepresentable:
        return app.EnglishUI() == 0
                   ? L"範囲境界の三次曲線をキーフレーム形式で正確に表現できないため、何も書き込みませんでした。範囲か曲線を調整してください。"
               : app.EnglishUI() == 1
                   ? L"The range boundary cannot be reproduced exactly in the keyframe format, so nothing was written. Move the range or fix the curve."
                   : L"范围边界的三次曲线无法用关键帧格式精确表示，因此未写入任何内容。请调整范围或曲线。";
    default:
        return app.EnglishUI() == 0   ? L"メモリが不足しています。"
               : app.EnglishUI() == 1 ? L"Not enough memory."
                                      : L"内存不足。";
    }
}

}  // namespace mikudancestudio::channel_bake
