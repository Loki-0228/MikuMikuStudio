#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
// MAINTENANCE: the native half of the frame-rate conversion.  The contract and
// the vocabulary are in include/mikumikustudio/native_frame_retime.hpp; this
// file is the implementation and repeats only what the code cannot say.
//
// Pipeline
// --------
// Prepare walks every animation table a project actually has loaded and builds
// a patch per chain:
//
//   * the chain's slots and their original bytes (the rollback source),
//   * the frame mapping and the collision groups, computed once per chain by
//     animation_retime::GroupFrames, which is also what makes the counters
//     per-record instead of per-component,
//   * one retimed scalar curve per lane, from animation_retime::Retime, and its
//     quantised 0..127 control tuple,
//   * the new record bytes for every surviving slot,
//   * one per-frame change entry per original key (see Changes below).//
// Commit compares every recorded identity and content digest with the live
// project, writes the new bytes, calls the caller's finalize callback, and then
// either completes or rewrites the recorded bytes.  Rolling back is a byte copy
// back from the patch, so it cannot fail and does not allocate.
//
// Occupancy
// ---------
// Bone and morph chains are rooted at the record whose index is the bone or
// morph index; the display chain is rooted at slot 0; every other chain is
// rooted at slot 0 of its own pool.  A chain carries no key when its root is
// the untouched head, which is `frame == 0 && next == 0 && allocated == 0` -
// the same test src/animation/morph_curve.cpp uses.  `allocated` is never an
// occupancy test here; it is a mark, and a merge ORs the group's marks.
//
// Which value a merged group takes
// --------------------------------
// A group that holds the original frame-zero key and lands on frame zero keeps
// that key's value.  A discrete group, and a continuous group whose folded
// intervals are all Step, takes the group's LAST record - the state in force at
// the merged time.  Anything else is sampled on the group's own curve at the
// inverse-mapped frame, which is exactly what animation_retime::MergeValue does
// for a scalar track.  A bone rotation is the exception the header promises: it
// is sampled as a quaternion, by slerp between the two original keys either
// side of the merged time with the eased fraction the rotation channel's own
// controls produce.
//
// The change list
// ---------------
// Every original key that a conversion moves contributes one Rounded entry and
// every key that loses its own time slot contributes one Merged entry; a key
// that both moved and was merged appears once for each cause, because the two
// are different losses.  Counters count causes, so roundedKeys is the number of
// Rounded entries and mergedKeys the number of Merged entries, and the detail
// list holds exactly that many entries unless the module's own budget stopped
// it - in which case detailsTruncated is set and omittedDetailCount says how
// many were left out.
#include "mikumikustudio/native_frame_retime.hpp"

#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/animation_retime.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ui_language.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace mikumikustudio::native_frame_retime {
namespace {

namespace ac = animation_curve;
namespace art = animation_retime;
namespace fc = frame_rate_correction;

using Bytes = std::vector<std::uint8_t>;

// Frame domain of every native key table, and the frame bound one conversion
// accepts.  Both come from the port's own layouts.
constexpr std::int32_t kMaximumFrame = mikumikustudio::frame_limits::maximum;
constexpr std::size_t kAccessorySlots = 255;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::uint64_t HashBytes(std::uint64_t hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t HashValue(std::uint64_t hash, std::uint64_t value) {
    return HashBytes(hash, &value, sizeof(value));
}

std::wstring Localized(const MMDApp& app, const wchar_t* ja, const wchar_t* en, const wchar_t* zh) {
    const unsigned language = app.EnglishUI();
    return language == 0 ? ja : language == 1 ? en : zh;
}

// The four 0..127 controls of one interval, in the order the native records
// store them, plus whether the interval is a Step hold.
struct Controls {
    std::array<std::uint8_t, 4> value{0, 0, 0, 0};
    bool step = false;
    bool exact = true;  // false when the closest tuple was not the exact one
};

std::uint8_t Quantise(double value) {
    const double rounded = std::floor(value + 0.5);
    if (rounded < 0.0) return 0;
    if (rounded > 127.0) return 127;
    return static_cast<std::uint8_t>(rounded);
}

bool IsLinearTuple(const std::uint8_t tuple[4]) {
    // The port's own linear test (src/model/model_keyframe_advance.cpp:160 and
    // include/mikumikustudio/bone_scale.hpp): both polynomial pairs agree.
    return tuple[0] == tuple[1] && tuple[2] == tuple[3];
}

void SetLinearTuple(std::uint8_t tuple[4]) {
    tuple[0] = bone_scale::kLinearHandleA;
    tuple[1] = bone_scale::kLinearHandleA;
    tuple[2] = bone_scale::kLinearHandleB;
    tuple[3] = bone_scale::kLinearHandleB;
}

// Reads the record's frame out of a pool whose records all start with the frame
// and the two chain links at offsets 0, 4 and 8.
std::uint32_t FrameAt(const std::uint8_t* pool, std::size_t stride, std::uint32_t slot) {
    const std::uint8_t* record = pool + static_cast<std::size_t>(slot) * stride;
    std::uint32_t frame = 0;
    std::memcpy(&frame, record, sizeof(frame));
    return frame;
}

// Walks one next-linked chain from its root.  Fails on a chain that leaves the
// pool, does not end, is not strictly increasing, or carries a frame outside
// the supported range, so the caller never has to defend against a damaged
// pool half way through a write.
bool ReadChain(const std::uint8_t* pool, std::size_t capacity, std::size_t stride,
               std::uint32_t root, std::vector<std::uint32_t>& slots, std::string& error) {
    slots.clear();
    if (pool == nullptr) {
        error = "the key pool is missing";
        return false;
    }
    if (root >= capacity) {
        error = "the chain root leaves the key pool";
        return false;
    }
    std::uint32_t index = root;
    std::uint32_t previousFrame = 0;
    for (std::size_t visited = 0; visited <= capacity; ++visited) {
        if (index >= capacity) {
            error = "the key chain leaves the pool";
            return false;
        }
        const std::uint32_t frame = FrameAt(pool, stride, index);
        if (frame > static_cast<std::uint32_t>(kMaximumFrame)) {
            error = "a key frame is outside the supported range";
            return false;
        }
        if (!slots.empty() && frame <= previousFrame) {
            error = "the key chain is not in increasing frame order";
            return false;
        }
        previousFrame = frame;
        slots.push_back(index);
        std::uint32_t next = 0;
        std::memcpy(&next, pool + static_cast<std::size_t>(index) * stride + 8, sizeof(next));
        if (next == 0) return true;
        index = next;
        if (visited == capacity) {
            error = "the key chain does not end";
            return false;
        }
    }
    error = "the key chain does not end";
    return false;
}

// Roots are frame-zero baselines even when their export/selection mark is off.
// Reachability, not a byte at a type-dependent offset, establishes occupancy.
bool ChainCarriesKey(const std::uint8_t* pool, std::size_t stride, const std::vector<std::uint32_t>& slots) {
    (void)pool;
    (void)stride;
    return !slots.empty();
}

// ---------------------------------------------------------------------------
// The per-frame change list
// ---------------------------------------------------------------------------
//
// The list is COMPLETE.  There is no budget and no truncation: one entry is
// kept for every key whose frame needed rounding and one for every key that lost
// its slot to a merge, so the list length always equals roundedKeys + mergedKeys
// and detailsTruncated stays false.  If the memory for it cannot be obtained,
// Prepare fails - recording fewer entries and carrying on would be exactly the
// silent omission the Report exists to prevent.

// The frame arithmetic of animation_retime, repeated here because the Report
// has to agree with it frame for frame: a key counts as ROUNDED when its exact
// scaled position is not an integer, with the same 1e-12 tolerance and the same
// exact-rational path for whole rates, so a proportional move that lands on a
// frame (30 -> 15, 30 -> 12) is a normal move and is not reported at all.
struct FrameMath {
    bool integer = false;
    long double ratio = 1.0L;
    std::int64_t numerator = 1;
    std::int64_t denominator = 1;

    long double Exact(std::int64_t frame) const {
        if (integer) return static_cast<long double>(frame * numerator) / static_cast<long double>(denominator);
        return static_cast<long double>(frame) * ratio;
    }
    bool ExactPosition(std::int64_t frame) const {
        const long double exact = Exact(frame);
        return std::fabs(exact - std::floor(exact + 0.5L)) <= 1e-12L;
    }
};

FrameMath MakeFrameMath(double originalFPS, double currentFPS) {
    FrameMath math;
    math.ratio = static_cast<long double>(currentFPS) / static_cast<long double>(originalFPS);
    const double roundedOriginal = std::floor(originalFPS + 0.5);
    const double roundedCurrent = std::floor(currentFPS + 0.5);
    const double tolerance = 1e-9;
    const bool whole = std::fabs(originalFPS - roundedOriginal) <= tolerance &&
                       std::fabs(currentFPS - roundedCurrent) <= tolerance;
    if (whole && roundedOriginal >= 1.0 && roundedCurrent >= 1.0 && roundedOriginal <= 1.0e6 &&
        roundedCurrent <= 1.0e6) {
        auto gcd = [](std::int64_t a, std::int64_t b) {
            while (b != 0) {
                const std::int64_t remainder = a % b;
                a = b;
                b = remainder;
            }
            return a == 0 ? 1 : a;
        };
        const std::int64_t divisor =
            gcd(static_cast<std::int64_t>(roundedCurrent), static_cast<std::int64_t>(roundedOriginal));
        math.integer = true;
        math.numerator = static_cast<std::int64_t>(roundedCurrent) / divisor;
        math.denominator = static_cast<std::int64_t>(roundedOriginal) / divisor;
        // The mapping ratio is the reduced fraction, exactly as
        // animation_retime::BuildMapping defines it, so the inverse-mapped
        // merged time below is bit-for-bit the one the retime core samples at.
        math.ratio = static_cast<long double>(math.numerator) / static_cast<long double>(math.denominator);
    }
    return math;
}

struct Change {
    std::wstring channel;
    std::int32_t originalFrame = 0;
    std::int32_t newFrame = 0;
    double exactFrame = 0.0;
    bool merged = false;
    std::int32_t mergeFirstFrame = 0;
    std::int32_t mergeLastFrame = 0;
};

class Changes {
public:
    void Reset(const FrameMath& math) { math_ = math; }

    // One original key.  `groupFrame` is the frame the key's collision group
    // lands on, which is the key's own rounded frame whether or not it shares
    // the group; `mergedMember` is true from the group's second key on.
    void Add(const std::wstring& channel, std::int32_t originalFrame, std::int32_t groupFrame,
             bool mergedMember, std::int32_t groupFirst, std::int32_t groupLast) {
        const double exact = static_cast<double>(math_.Exact(originalFrame));
        if (!math_.ExactPosition(originalFrame)) {
            ++rounded_;
            Change entry;
            entry.channel = channel;
            entry.originalFrame = originalFrame;
            entry.newFrame = groupFrame;
            entry.exactFrame = exact;
            entry.merged = false;
            items_.push_back(std::move(entry));
        }
        if (mergedMember) {
            ++merged_;
            Change entry;
            entry.channel = channel;
            entry.originalFrame = originalFrame;
            entry.newFrame = groupFrame;
            entry.exactFrame = exact;
            entry.merged = true;
            entry.mergeFirstFrame = groupFirst;
            entry.mergeLastFrame = groupLast;
            items_.push_back(std::move(entry));
        }
    }

    void Publish(fc::Report& report) const {
        report.roundedKeys += rounded_;
        report.mergedKeys += merged_;
        report.changes.reserve(report.changes.size() + items_.size());
        for (const Change& entry : items_) {
            fc::FrameChange out;
            out.channel = entry.channel;
            out.originalFrame = entry.originalFrame;
            out.newFrame = entry.newFrame;
            out.exactFrame = entry.exactFrame;
            out.kind = entry.merged ? fc::FrameChange::ChangeKind::Merged
                                    : fc::FrameChange::ChangeKind::Rounded;
            out.mergeFirstFrame = entry.mergeFirstFrame;
            out.mergeLastFrame = entry.mergeLastFrame;
            report.changes.push_back(std::move(out));
        }
        // Nothing is ever dropped, so the two fields say exactly that.
        report.detailsTruncated = false;
        report.omittedDetailCount = 0;
    }

private:
    FrameMath math_;
    std::vector<Change> items_;
    std::uint64_t rounded_ = 0;
    std::uint64_t merged_ = 0;
};

// ---------------------------------------------------------------------------
// Chain planning: frames, collisions, and the retimed scalar per lane
// ---------------------------------------------------------------------------

struct ChainPlan {
    bool empty = true;  // the root is an untouched head, so there is no key
    bool changed = false;
    std::vector<std::uint32_t> slots;
    std::vector<std::int32_t> frames;
    std::vector<art::Group> groups;
    std::vector<std::uint32_t> outSlots;  // survivor slot per output key
    std::vector<bool> outMerged;          // the group holds more than one key
    art::Report counters;                 // per-record counters from GroupFrames
};

// LanePlan holds one scalar channel's retimed curve.
struct LanePlan {
    std::vector<double> values;                 // one per output key
    std::vector<Controls> controls;             // index >= 1; index 0 unused
    bool approximated = false;                  // a tuple was not exact
    double sampledError = 0.0;
    bool shapePreserved = true;
};

bool PlanChain(const std::uint8_t* pool, std::size_t capacity, std::size_t stride,
               std::uint32_t root, const art::Rates& rates, const art::Options& options,
               ChainPlan& out, std::string& error) {
    out = ChainPlan{};
    if (!ReadChain(pool, capacity, stride, root, out.slots, error)) return false;
    if (!ChainCarriesKey(pool, stride, out.slots)) {
        out.empty = true;
        return true;
    }
    out.empty = false;
    out.frames.reserve(out.slots.size());
    for (std::uint32_t slot : out.slots) {
        out.frames.push_back(static_cast<std::int32_t>(FrameAt(pool, stride, slot)));
    }
    if (!art::GroupFrames(out.frames, rates, options, out.groups, out.counters, error)) return false;
    out.outSlots.reserve(out.groups.size());
    out.outMerged.reserve(out.groups.size());
    for (const art::Group& group : out.groups) {
        out.outSlots.push_back(out.slots[group.first]);
        out.outMerged.push_back(group.last > group.first);
    }
    // "Integral mapping" only means every key landed exactly on its scaled
    // position; that is still a move unless the ratio was 1.  A chain changes
    // when a key's frame changes or when a collision merges keys away, which is
    // what the group heads and the merge count say.
    out.changed = out.counters.mergeGroups != 0;
    for (const art::Group& group : out.groups) {
        if (out.frames[group.first] != group.frame) {
            out.changed = true;
            break;
        }
    }
    return true;
}

// Native MMD permits endpoint and crossing X handles. The shared editable
// curve representation uses ordered handles; fit only those exceptional input
// intervals locally, then measure the final output against the original curve.
std::array<double, 4> FitNativeControls(const Controls& control) {
    auto cubic = [](double a, double b, double u) {
        const double v = 1.0 - u;
        return 3.0*v*v*u*a + 3.0*v*u*u*b + u*u*u;
    };
    std::array<double, 4> best{42, 42, 85, 85};
    double bestError = std::numeric_limits<double>::infinity();
    for (double x1 : {.05, .15, 1.0/3.0, .45}) for (double x2 : {.55, 2.0/3.0, .85, .95}) {
        double aa=0, ab=0, bb=0, ay=0, by=0;
        std::array<double, 63> a{}, b{}, c{}, y{};
        for (std::size_t i=0;i<a.size();++i) {
            const double t = double(i+1)/64.0;
            double low=0, high=1;
            for (int iteration=0;iteration<32;++iteration) {
                const double u=(low+high)*.5;
                if(cubic(x1,x2,u)<t)low=u;else high=u;
            }
            const double u=(low+high)*.5, v=1-u;
            a[i]=3*v*v*u;b[i]=3*v*u*u;c[i]=u*u*u;
            y[i]=bone_scale::CurveTime(control.value,static_cast<float>(t));
            aa+=a[i]*a[i];ab+=a[i]*b[i];bb+=b[i]*b[i];
            ay+=a[i]*(y[i]-c[i]);by+=b[i]*(y[i]-c[i]);
        }
        const double determinant=aa*bb-ab*ab;
        if(determinant<=0)continue;
        const double y1=std::clamp((ay*bb-by*ab)/determinant,0.0,1.0);
        const double y2=std::clamp((by*aa-ay*ab)/determinant,0.0,1.0);
        double squaredError=0;
        for(std::size_t i=0;i<a.size();++i) {
            const double delta=a[i]*y1+b[i]*y2+c[i]-y[i];
            squaredError+=delta*delta;
        }
        if(squaredError<bestError){bestError=squaredError;best={127*x1,127*y1,127*x2,127*y2};}
    }
    return best;
}

double NativeLaneValue(const ac::Track& input, const std::vector<Controls>& controls, double frame) {
    if(input.keys.empty())return 0;
    if(frame<=input.keys.front().frame)return input.keys.front().value;
    const auto end=std::upper_bound(input.keys.begin(),input.keys.end(),frame,
        [](double value,const ac::Key& key){return value<key.frame;});
    if(end==input.keys.end())return input.keys.back().value;
    const auto index=static_cast<std::size_t>(end-input.keys.begin());
    const auto& start=input.keys[index-1];
    if(controls[index].step)return start.value;
    const float t=static_cast<float>((frame-start.frame)/(end->frame-start.frame));
    return start.value+(end->value-start.value)*bone_scale::CurveTime(controls[index].value,t);
}

// Builds the scalar input curve of one lane: the chain's values with the lane's
// own native controls on every interval.
bool BuildTrack(const ChainPlan& chain, const std::vector<double>& values,
                const std::vector<Controls>& intervalControls, bool discrete, ac::Track& out,
                std::string& error) {
    out = ac::Track{};
    out.discrete = discrete;
    out.keys.reserve(chain.frames.size());
    for (std::size_t index = 0; index < chain.frames.size(); ++index) {
        ac::Key key;
        key.frame = chain.frames[index];
        key.value = values[index];
        key.interpolation = discrete ? ac::Interpolation::Step : ac::Interpolation::Linear;
        key.tangent = ac::Tangent::Free;
        out.keys.push_back(key);
    }
    if (!discrete) {
        if (intervalControls.empty()) return true;
        for (std::size_t index = 1; index < out.keys.size(); ++index) {
            const Controls& controls = intervalControls[index];
            if (controls.step) {
                out.keys[index].interpolation = ac::Interpolation::Step;
                continue;
            }
            // Native easing has no effect on a scalar interval with equal
            // endpoints. Keep it constant here; the original tuple is retained
            // separately for untouched intervals and timing-only lanes.
            if (out.keys[index].value == out.keys[index - 1].value) continue;
            if (IsLinearTuple(controls.value.data())) continue;  // already Linear
            std::array<double, 4> domain{};
            for (std::size_t j = 0; j < 4; ++j) domain[j] = controls.value[j];
            if (!ac::SetMmdControls(out, index, domain, error)) {
                if (std::any_of(controls.value.begin(), controls.value.end(),
                                [](std::uint8_t value) { return value > 127; })) return false;
                const auto fitted = FitNativeControls(controls);
                if (!ac::SetMmdControls(out, index, fitted, error)) return false;
            }
        }
    }
    return true;
}

// Retimes one scalar lane and reads the 0..127 tuple back out of the result.
bool PlanLane(const ac::Track& input, const ChainPlan& chain, const art::Rates& rates,
              const art::Options& options, LanePlan& out, std::string& error,
              const std::vector<Controls>* nativeControls = nullptr) {
    ac::Track result;
    art::Report report;
    if (!art::Retime(input, rates, options, result, report, error)) return false;
    if (result.keys.size() != chain.outSlots.size()) {
        error = "the retimed track and the collision plan disagree on the key count";
        return false;
    }
    // The frame list is per chain, so every lane must have produced the same one.
    std::vector<std::int32_t> frames;
    frames.reserve(chain.groups.size());
    for (const art::Group& group : chain.groups) frames.push_back(group.frame);
    for (std::size_t index = 0; index < result.keys.size(); ++index) {
        if (result.keys[index].frame != frames[index]) {
            error = "the retimed track and the collision plan disagree on a frame";
            return false;
        }
        if (nativeControls && chain.outMerged[index]) {
            const auto& group = chain.groups[index];
            if (chain.frames[group.first] == 0 && group.frame == 0) continue;
            bool step = true;
            for (std::size_t member=group.first+1; member<=group.last; ++member)
                step = step && (*nativeControls)[member].step;
            if (step) result.keys[index].value = input.keys[group.last].value;
            else result.keys[index].value = NativeLaneValue(input, *nativeControls,
                static_cast<double>(static_cast<long double>(group.frame) * rates.originalFPS / rates.currentFPS));
        }
    }
    out = LanePlan{};
    out.values.reserve(result.keys.size());
    out.controls.assign(result.keys.size(), Controls{});
    for (std::size_t index = 0; index < result.keys.size(); ++index) {
        out.values.push_back(result.keys[index].value);
    }
    for (std::size_t index = 1; index < result.keys.size(); ++index) {
        Controls& controls = out.controls[index];
        if (result.keys[index].interpolation == ac::Interpolation::Step) {
            controls.step = true;
            continue;
        }
        std::array<double, 4> domain{};
        if (ac::GetMmdControls(result, index, domain)) {
            for (std::size_t j = 0; j < 4; ++j) controls.value[j] = Quantise(domain[j]);
            continue;
        }
        // The interval is not exactly representable on the 0..127 grid.  Keep
        // the closest expressible tuple: the same relations SetMmdControls
        // establishes, read backwards.
        const ac::Key& start = result.keys[index - 1];
        const ac::Key& end = result.keys[index];
        const double span = static_cast<double>(end.frame) - static_cast<double>(start.frame);
        const double delta = end.value - start.value;
        if (span <= 0.0 || !std::isfinite(span) || !std::isfinite(delta)) {
            error = "the retimed interval cannot be expressed";
            return false;
        }
        controls.exact = false;
        out.approximated = true;
        const double x1 = start.outWeight * 127.0;
        const double x2 = (1.0 - end.inWeight) * 127.0;
        if (delta == 0.0) {
            controls.value[0] = bone_scale::kLinearHandleA;
            controls.value[1] = bone_scale::kLinearHandleA;
            controls.value[2] = bone_scale::kLinearHandleB;
            controls.value[3] = bone_scale::kLinearHandleB;
            controls.exact = true;
        } else {
            const double y1 = 127.0 * start.outSlope * span * start.outWeight / delta;
            const double y2 = 127.0 * (1.0 - end.inSlope * span * end.inWeight / delta);
            controls.value[0] = Quantise(x1);
            controls.value[1] = Quantise(y1);
            controls.value[2] = Quantise(x2);
            controls.value[3] = Quantise(y2);
        }
    }
    out.sampledError = report.maxSampledValueError;
    out.shapePreserved = report.curveShapePreserved;
    if (nativeControls) {
        // Intervals untouched by a merge retain their native bytes. Measure
        // the final integer tuples, not only the intermediate double curve.
        for (std::size_t i = 0; i < chain.groups.size(); ++i) {
            if (i == 0 || (!chain.outMerged[i] && !chain.outMerged[i - 1]))
                out.controls[i] = (*nativeControls)[chain.groups[i].first];
        }
        const long double ratio = static_cast<long double>(rates.currentFPS) / rates.originalFPS;
        for (std::size_t i = 1; i < result.keys.size(); ++i) {
            const double first = result.keys[i - 1].frame;
            const double span = result.keys[i].frame - first;
            const auto& tuple = out.controls[i].value;
            for (int sample = 0; sample <= 32; ++sample) {
                const double t = static_cast<double>(sample) / 32.0;
                const double frame = first + span * t;
                const double eased = out.controls[i].step ? (sample == 32 ? 1.0 : 0.0)
                    : bone_scale::CurveTime(tuple, static_cast<float>(t));
                const double actual = out.values[i - 1] + (out.values[i] - out.values[i - 1]) * eased;
                const double wanted = NativeLaneValue(input, *nativeControls, static_cast<double>(frame / ratio));
                out.sampledError = std::max(out.sampledError, std::abs(actual - wanted));
                if (std::abs(actual - ac::Evaluate(result, frame).value) > 1e-7) {
                    out.controls[i].exact = false;
                    out.approximated = true;
                }
            }
        }
        out.shapePreserved = out.shapePreserved && !out.approximated;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Quaternion slerp for a merged bone rotation
// ---------------------------------------------------------------------------

// The dot product clamp both original builds use
// (src/model/model_keyframe_advance.cpp:145).
constexpr double kQuatClamp = 0.999999;

struct Quaternion {
    double v[4]{0, 0, 0, 1};
};

Quaternion Slerp(Quaternion start, Quaternion end, double t) {
    double dot = 0.0;
    for (int i = 0; i < 4; ++i) dot += start.v[i] * end.v[i];
    // Same hemisphere first: a merged quaternion must never flip its sign
    // because the two keys were written with opposite signs.
    if (dot < 0.0) {
        for (int i = 0; i < 4; ++i) end.v[i] = -end.v[i];
        dot = -dot;
    }
    if (dot > kQuatClamp) dot = kQuatClamp;
    const double theta = std::acos(dot);
    const double sine = std::sin(theta);
    Quaternion out;
    if (sine <= 1e-12) {
        for (int i = 0; i < 4; ++i) out.v[i] = start.v[i];
    } else {
        const double a = std::sin((1.0 - t) * theta) / sine;
        const double b = std::sin(t * theta) / sine;
        for (int i = 0; i < 4; ++i) out.v[i] = a * start.v[i] + b * end.v[i];
    }
    double length = 0.0;
    for (int i = 0; i < 4; ++i) length += out.v[i] * out.v[i];
    if (length > 0.0) {
        const double inverse = 1.0 / std::sqrt(length);
        for (int i = 0; i < 4; ++i) out.v[i] *= inverse;
    }
    return out;
}

// The eased position of `frame` inside an original key list, expressed in key
// indices.  A track whose values are 0, 1, 2, ... evaluates to exactly that
// position, so the core's own Bezier time inversion and Step handling are
// reused instead of being re-derived here.
double EasedKeyPosition(const ac::Track& ramp, double frame) {
    return ac::Evaluate(ramp, frame).value;
}

// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------

// A display key owns two heap arrays.  A merged key has to end up holding the
// state in force at the merged time, but Prepare must never write through the
// live pointers: it records both the bytes the survivor holds now and the bytes
// it must hold afterwards, and Commit copies them.  The pointers are stored too,
// because they are the allocation the copy goes into - the survivor keeps its
// own, so nothing is shared and nothing is freed early.
struct PayloadPatch {
    std::uint8_t* ik = nullptr;
    std::uint8_t* selectors = nullptr;
    std::size_t ikBytes = 0;
    std::size_t selectorBytes = 0;
    std::vector<std::uint8_t> originalIk;
    std::vector<std::uint8_t> originalSelectors;
    std::vector<std::uint8_t> candidateIk;
    std::vector<std::uint8_t> candidateSelectors;
};

struct SlotPatch {
    std::vector<std::uint32_t> slots;  // original chain order
    std::vector<Bytes> original;       // original bytes, one per slot
    std::uint64_t digest = 0;          // slot index and bytes, in chain order
    std::vector<std::uint32_t> writeSlots;
    std::vector<Bytes> written;
    std::vector<std::uint32_t> clearSlots;  // zeroed once the commit is final
    std::vector<PayloadPatch> payloads;     // display key arrays, candidates only
    bool ownsPayloads = false;              // display keys: free the cleared payloads
    bool changed = false;
};

struct PoolPatch {
    std::uint8_t* pool = nullptr;
    std::size_t capacity = 0;
    std::size_t stride = 0;
    std::vector<SlotPatch> chains;
    bool changed = false;
};

struct ModelPatch {
    unsigned char* model = nullptr;
    int slot = 0;
    std::wstring tag;
    bool english = false;
    const void* morphs = nullptr;
    const void* bones = nullptr;
    std::uint32_t morphCount = 0;
    std::uint32_t boneCount = 0;
    std::uint32_t ikChainCount = 0;
    std::uint32_t boneOrderCount = 0;
    std::uint32_t searchCursor = 0;
    std::uint64_t identityDigest = 0;
    PoolPatch bone;
    PoolPatch morph;
    PoolPatch display;
    std::vector<PayloadPatch> displayPayloads;
    std::uint32_t maxFrame = 0;
    std::uint32_t oldMaxFrame = 0;
    bool changed = false;
    // `curves` and `layers` are the snapshots Commit verifies against; the
    // `retimed*` holders are the candidates Commit installs.  Keeping the two
    // apart is what lets verification compare the live table with what Prepare
    // saw, instead of with what Prepare is about to write.
    mutable bone_scale::CurveTableState curves;
    mutable bone_scale::CurveTableState retimedCurves;
    bool hasCurves = false;
    bool curvesMoved = false;
    mutable morph_curve::AuthoredLayers layers;
    mutable morph_curve::AuthoredLayers retimedLayers;
    bool hasLayers = false;
    bool layersMoved = false;
};

struct AppPatch {
    MMDApp* app = nullptr;
    std::array<unsigned char*, kModelSlotCount> modelIdentities{};
    std::array<mdl::AccessoryRecord*, kAccessorySlots> accessoryIdentities{};
    std::array<mdl::AccessoryKey*, kAccessorySlots> accessoryPools{};
    PoolPatch camera;
    PoolPatch light;
    PoolPatch shadow;
    PoolPatch gravity;
    std::vector<int> accessorySlots;
    std::vector<PoolPatch> accessories;
    std::uint32_t cameraCursor = 0;
    std::uint32_t lightCursor = 0;
    std::uint32_t shadowCursor = 0;
    std::uint32_t gravityCursor = 0;
    std::vector<std::uint32_t> accessoryCursors;
    std::uint32_t lastRegistered = 0;
    std::uint32_t oldLastRegistered = 0;
    std::uint8_t sceneModified = 0;
    bool changed = false;
    mutable curve_host::AuthoringTable authoring;
    mutable curve_host::AuthoringTable retimedAuthoringTable;
    bool hasAuthoring = false;
    bool authoringMoved = false;
    std::vector<curve_host::AuthoringEntry> retimedAuthoring;
};

}  // namespace

// The plan is opaque to every caller, so its layout lives here.
class Plan {
public:
    Plan() = default;
    Plan(const Plan&) = delete;
    Plan& operator=(const Plan&) = delete;

    fc::Rates rates;
    art::Options options;
    AppPatch app;
    std::vector<ModelPatch> models;
    mutable bool committed = false;
};

namespace {

// ---------------------------------------------------------------------------
// Record byte helpers
// ---------------------------------------------------------------------------

double ReadF32(const std::uint8_t* base, std::size_t offset) {
    float value = 0.0f;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}
void WriteF32(std::uint8_t* base, std::size_t offset, double value) {
    const float narrowed = static_cast<float>(value);
    std::memcpy(base + offset, &narrowed, sizeof(narrowed));
}
std::int32_t ReadI32(const std::uint8_t* base, std::size_t offset) {
    std::int32_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}
void WriteI32(std::uint8_t* base, std::size_t offset, std::int32_t value) {
    std::memcpy(base + offset, &value, sizeof(value));
}
std::uint32_t ReadU32(const std::uint8_t* base, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}
void WriteU32(std::uint8_t* base, std::size_t offset, std::uint32_t value) {
    std::memcpy(base + offset, &value, sizeof(value));
}

// ---------------------------------------------------------------------------
// Chain patch plumbing
// ---------------------------------------------------------------------------

// Copies every record of the chain and remembers its digest, so Commit can see
// that the pool has not moved since Prepare.
bool BeginChain(const std::uint8_t* pool, std::size_t capacity, std::size_t stride,
                const ChainPlan& chain, SlotPatch& patch, std::string& error) {
    patch = SlotPatch{};
    patch.changed = chain.changed;
    patch.slots = chain.slots;
    patch.original.reserve(chain.slots.size());
    for (std::uint32_t slot : chain.slots) {
        if (pool == nullptr || slot >= capacity) {
            error = "internal error: a chain slot is outside the pool";
            return false;
        }
        const std::uint8_t* record = pool + static_cast<std::size_t>(slot) * stride;
        patch.original.emplace_back(record, record + stride);
        patch.digest = HashValue(patch.digest, slot);
        patch.digest = HashBytes(patch.digest, record, stride);
    }
    if (!chain.changed) return true;
    std::vector<bool> survivor(patch.slots.size(), false);
    for (const art::Group& group : chain.groups) survivor[group.first] = true;
    for (std::size_t index = 0; index < patch.slots.size(); ++index) {
        if (survivor[index]) patch.writeSlots.push_back(patch.slots[index]);
        else patch.clearSlots.push_back(patch.slots[index]);
    }
    return true;
}

// Records a chain purely so Commit can verify it, without planning a write.
// An empty chain still has to be recorded: a key registered into a slot that
// was free at Prepare changes the pool, and the conversion must not go ahead
// and leave that key unconverted.
bool RecordOnly(const std::uint8_t* pool, std::size_t capacity, std::size_t stride,
                const ChainPlan& chain, std::vector<SlotPatch>& into, std::string& error) {
    SlotPatch patch;
    if (!BeginChain(pool, capacity, stride, chain, patch, error)) return false;
    into.push_back(std::move(patch));
    return true;
}

void LinkWritten(SlotPatch& patch) {
    for (std::size_t index = 0; index < patch.written.size(); ++index) {
        Bytes& record = patch.written[index];
        const std::uint32_t previous = index == 0 ? 0u : patch.writeSlots[index - 1];
        const std::uint32_t next =
            index + 1 < patch.writeSlots.size() ? patch.writeSlots[index + 1] : 0u;
        WriteU32(record.data(), 4, previous);
        WriteU32(record.data(), 8, next);
    }
}

// Applies one retime to one chain.  `writer` builds the new record bytes of
// output key `index` on top of the survivor's original bytes.
template <typename Writer>
bool ApplyChain(const std::uint8_t* pool, std::size_t capacity, std::size_t stride,
                const ChainPlan& chain, Writer&& writer, SlotPatch& patch, std::string& error) {
    if (!BeginChain(pool, capacity, stride, chain, patch, error)) return false;
    if (!patch.changed) return true;
    patch.written.reserve(chain.groups.size());
    for (std::size_t index = 0; index < chain.groups.size(); ++index) {
        const std::uint32_t slot = chain.outSlots[index];
        const std::uint8_t* original = pool + static_cast<std::size_t>(slot) * stride;
        Bytes out(original, original + stride);
        WriteU32(out.data(), 0, static_cast<std::uint32_t>(chain.groups[index].frame));
        writer(out, original, index);
        patch.written.push_back(std::move(out));
    }
    LinkWritten(patch);
    return true;
}

// Aggregates one chain's counters and its per-frame change entries.
void NoteChain(const ChainPlan& chain, const std::wstring& channel, Changes& changes,
               fc::Report& report) {
    report.inputKeys += chain.counters.inputKeys;
    report.outputKeys += chain.counters.outputKeys;
    report.mergeGroups += chain.counters.mergeGroups;
    for (const art::Group& group : chain.groups) {
        for (std::size_t member = group.first; member <= group.last; ++member) {
            changes.Add(channel, chain.frames[member], group.frame, member > group.first,
                        chain.frames[group.first], chain.frames[group.last]);
        }
    }
}

// The merged-value rule of one group, mirroring animation_retime::MergeValue.
enum class MergeRule { Baseline, LastState, Sample };

MergeRule RuleFor(const ChainPlan& chain, std::size_t groupIndex, bool discrete,
                  const std::vector<Controls>& controls) {
    const art::Group& group = chain.groups[groupIndex];
    if (chain.frames[group.first] == 0 && group.frame == 0) return MergeRule::Baseline;
    if (discrete) return MergeRule::LastState;
    for (std::size_t member = group.first; member < group.last; ++member) {
        if (!controls[member + 1].step) return MergeRule::Sample;
    }
    return MergeRule::LastState;
}

// Tracks the largest sampled error and the channel it belongs to.
struct ErrorWatch {
    void Note(const std::wstring& channel, const LanePlan& lane) {
        if (lane.shapePreserved) return;
        if (lane.sampledError <= maximum) return;
        maximum = lane.sampledError;
        channelName = channel;
    }
    double maximum = 0.0;
    std::wstring channelName;
};

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// Models: bones
// ---------------------------------------------------------------------------

bool PlanBones(const art::Rates& rates, const art::Options& options, const FrameMath& math,
               ModelPatch& mp, Changes& changes, fc::Report& report, ErrorWatch& watch,
               std::string& error) {
    const std::uint8_t* pool = mp.bone.pool;
    const std::size_t stride = sizeof(mdl::BoneKey);
    const std::size_t capacity = mp.bone.capacity;
    for (std::uint32_t bone = 0; bone < mp.boneCount; ++bone) {
        ChainPlan chain;
        if (!PlanChain(pool, mp.bone.capacity, stride, bone, rates, options, chain, error)) {
            return false;
        }
        if (chain.empty) {
            if (!RecordOnly(pool, mp.bone.capacity, stride, chain, mp.bone.chains, error)) return false;
            continue;
        }
        const auto name = mp.bones ? text_encoding::BoneLabel(mdl::Mdl(mp.model)->boneTable[bone], mp.english) : L"";
        const std::wstring channel = mp.tag + L" / " + name + L" [bone " + std::to_wstring(bone) + L"]";
        NoteChain(chain, channel, changes, report);
        mp.maxFrame = std::max(mp.maxFrame, static_cast<std::uint32_t>(chain.groups.back().frame));
        if (!chain.changed) {
            if (!RecordOnly(pool, capacity, stride, chain, mp.bone.chains, error)) return false;
            continue;
        }

        // Moving distinct keys changes only time and links. Preserve every
        // interpolation byte, including equal-value and native crossing handles.
        if (chain.counters.mergeGroups == 0) {
            SlotPatch patch;
            if (!ApplyChain(pool, capacity, stride, chain,
                            [](Bytes&, const std::uint8_t*, std::size_t) {}, patch, error)) return false;
            mp.bone.chains.push_back(std::move(patch));
            if (mp.hasCurves) mp.curvesMoved = true;
            continue;
        }

        std::array<std::vector<double>, 4> values;
        std::array<std::vector<Controls>, 4> controls;
        std::array<std::vector<double>, 4> keyPositions;
        for (int lane = 0; lane < 4; ++lane) {
            values[lane].reserve(chain.slots.size());
            controls[lane].assign(chain.slots.size(), Controls{});
            keyPositions[lane].reserve(chain.slots.size());
        }
        for (std::size_t index = 0; index < chain.slots.size(); ++index) {
            const std::uint8_t* record = pool + static_cast<std::size_t>(chain.slots[index]) * stride;
            for (int lane = 0; lane < 4; ++lane) {
                const std::size_t at = offsetof(mdl::BoneKey, interpolation) + static_cast<std::size_t>(lane);
                std::uint8_t tuple[4] = {record[at], record[at + 4], record[at + 8], record[at + 12]};
                std::memcpy(controls[lane][index].value.data(), tuple, 4);
                keyPositions[lane].push_back(static_cast<double>(index));
            }
            values[0].push_back(ReadF32(record, offsetof(mdl::BoneKey, position) + 0));
            values[1].push_back(ReadF32(record, offsetof(mdl::BoneKey, position) + 4));
            values[2].push_back(ReadF32(record, offsetof(mdl::BoneKey, position) + 8));
            values[3].push_back(ReadF32(record, offsetof(mdl::BoneKey, rotation) + 12));
        }

        std::array<LanePlan, 4> lanes;
        for (int lane = 0; lane < 4; ++lane) {
            ac::Track input;
            if (!BuildTrack(chain, lane == 3 ? keyPositions[3] : values[lane],
                            controls[lane], false, input, error)) return false;
            if (!PlanLane(input, chain, rates, options, lanes[lane], error, &controls[lane])) return false;
            watch.Note(channel, lanes[lane]);
        }

        // The rotation lane is a quaternion: the merged value is the native
        // slerp at the merged time, taken with the eased fraction the rotation
        // channel's own controls produce.
        ac::Track rampTrack;
        if (!BuildTrack(chain, keyPositions[3], controls[3], false, rampTrack, error)) return false;
        std::vector<Quaternion> rotations(chain.groups.size());
        for (std::size_t index = 0; index < chain.groups.size(); ++index) {
            const art::Group& group = chain.groups[index];
            const auto quaternionOf = [&](std::size_t member) {
                const std::uint8_t* record =
                    pool + static_cast<std::size_t>(chain.slots[member]) * stride;
                Quaternion q;
                for (int component = 0; component < 4; ++component) {
                    q.v[component] = ReadF32(record, offsetof(mdl::BoneKey, rotation) +
                                                         static_cast<std::size_t>(component) * 4);
                }
                return q;
            };
            if (!chain.outMerged[index]) {
                rotations[index] = quaternionOf(group.first);
                continue;
            }
            switch (RuleFor(chain, index, false, controls[3])) {
                case MergeRule::Baseline:
                    rotations[index] = quaternionOf(group.first);
                    break;
                case MergeRule::LastState:
                    rotations[index] = quaternionOf(group.last);
                    break;
                case MergeRule::Sample:
                default: {
                    // The same inverse mapping animation_retime samples with:
                    // the merged time is the target frame over the mapping
                    // ratio, not over the two floating point rates.
                    const long double mergedFrame =
                        static_cast<long double>(group.frame) / math.ratio;
                    const double position =
                        NativeLaneValue(rampTrack, controls[3], static_cast<double>(mergedFrame));
                    double lower = std::floor(position);
                    double fraction = position - lower;
                    if (!(fraction >= 0.0) || !(fraction <= 1.0)) {
                        error = "the eased rotation position is not finite";
                        return false;
                    }
                    const std::size_t size = chain.slots.size();
                    if (lower < 0.0) { lower = 0.0; fraction = 0.0; }
                    if (lower > static_cast<double>(size - 2)) {
                        lower = static_cast<double>(size - 2);
                        fraction = 1.0;
                    }
                    rotations[index] = Slerp(quaternionOf(static_cast<std::size_t>(lower)),
                                             quaternionOf(static_cast<std::size_t>(lower) + 1),
                                             fraction);
                    break;
                }
            }
        }

        SlotPatch patch;
        const bool applied = ApplyChain(pool, capacity, stride, chain,
            [&](Bytes& out, const std::uint8_t* original, std::size_t index) {
                for (int lane = 0; lane < 4; ++lane) {
                    if (index == 0 || (!chain.outMerged[index] && !chain.outMerged[index - 1])) continue;
                    if (lanes[lane].controls[index].step) continue;
                    const std::size_t at = offsetof(mdl::BoneKey, interpolation) +
                                           static_cast<std::size_t>(lane);
                    const auto& value = lanes[lane].controls[index].value;
                    out[at] = value[0];
                    out[at + 4] = value[1];
                    out[at + 8] = value[2];
                    out[at + 12] = value[3];
                }
                for (int axis = 0; axis < 3; ++axis) {
                    WriteF32(out.data(), offsetof(mdl::BoneKey, position) +
                                             static_cast<std::size_t>(axis) * 4,
                             lanes[axis].values[index]);
                }
                for (int component = 0; component < 4; ++component) {
                    WriteF32(out.data(), offsetof(mdl::BoneKey, rotation) +
                                             static_cast<std::size_t>(component) * 4,
                             rotations[index].v[component]);
                }
                // The export mark is a selection: a merge ORs it.  The physics
                // flag is a state, so the state in force at the merged time
                // wins, which is the group's last record.
                const art::Group& group = chain.groups[index];
                std::uint8_t allocated = 0;
                for (std::size_t member = group.first; member <= group.last; ++member) {
                    allocated |= patch.original[member][offsetof(mdl::BoneKey, allocated)];
                }
                out[offsetof(mdl::BoneKey, allocated)] = allocated;
                const auto stateMember = chain.frames[group.first] == 0 && group.frame == 0
                                             ? group.first : group.last;
                out[offsetof(mdl::BoneKey, physicsDisabled)] =
                    patch.original[stateMember][offsetof(mdl::BoneKey, physicsDisabled)];
                (void)original;
            }, patch, error);
        if (!applied) return false;
        // The scale channel is a per-slot side table, so its timing follows the
        // slot: a surviving slot keeps its own curve, a merged group's survivor
        // takes the retimed merged interval, and a slot that leaves the chain
        // loses its entry so a later key can never inherit it.
        if (mp.hasCurves) {
            std::vector<double> scaleValues;
            std::vector<Controls> scaleControls(chain.slots.size());
            for (std::size_t index = 0; index < chain.slots.size(); ++index) {
                const bone_scale::Curve curve =
                    bone_scale::KeyCurve(mp.model, static_cast<int>(chain.slots[index]));
                std::memcpy(scaleControls[index].value.data(), curve.data(), 4);
                scaleValues.push_back(static_cast<double>(index));
            }
            ac::Track scaleInput;
            if (!BuildTrack(chain, scaleValues, scaleControls, false, scaleInput, error)) return false;
            LanePlan scaleLane;
            if (!PlanLane(scaleInput, chain, rates, options, scaleLane, error, &scaleControls)) return false;
            watch.Note(channel + L".scale", scaleLane);
            for (std::size_t index = 0; index < chain.groups.size(); ++index) {
                const auto& group = chain.groups[index];
                if (chain.outMerged[index]) {
                    auto matrix = bone_scale::Key(mp.model, static_cast<int>(chain.slots[group.first]));
                    if (!(chain.frames[group.first] == 0 && group.frame == 0)) {
                        matrix = bone_scale::Sample(mp.model, static_cast<int>(bone),
                            static_cast<float>(static_cast<long double>(group.frame) / math.ratio));
                    }
                    if (!bone_scale::Valid(matrix)) { error = "the merged bone scale is invalid"; return false; }
                    const int slot = static_cast<int>(chain.outSlots[index]);
                    if (bone_scale::Identity(matrix)) mp.retimedCurves.keys.erase(slot);
                    else mp.retimedCurves.keys[slot] = matrix;
                    mp.curvesMoved = true;
                }
                if (index == 0 || (!chain.outMerged[index] && !chain.outMerged[index - 1]) ||
                    scaleLane.controls[index].step) continue;
                const auto& value = scaleLane.controls[index].value;
                if (IsLinearTuple(value.data())) {
                    mp.retimedCurves.curves.erase(static_cast<int>(chain.outSlots[index]));
                } else {
                    mp.retimedCurves.curves[static_cast<int>(chain.outSlots[index])] =
                        bone_scale::Curve{value[0], value[1], value[2], value[3]};
                }
                mp.curvesMoved = true;
            }
            for (std::uint32_t slot : patch.clearSlots) {
                if (mp.retimedCurves.curves.erase(static_cast<int>(slot)) != 0) mp.curvesMoved = true;
                if (mp.retimedCurves.keys.erase(static_cast<int>(slot)) != 0) mp.curvesMoved = true;
            }
        }
        mp.bone.chains.push_back(std::move(patch));
    }
    for (const SlotPatch& patch : mp.bone.chains) {
        if (patch.changed) { mp.bone.changed = true; mp.changed = true; }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Models: morphs and the display chain
// ---------------------------------------------------------------------------

bool PlanMorphs(const art::Rates& rates, const art::Options& options, ModelPatch& mp,
                Changes& changes, fc::Report& report, ErrorWatch& watch, std::string& error) {
    const std::uint8_t* pool = mp.morph.pool;
    const std::size_t stride = sizeof(mdl::MorphKey);
    const std::size_t capacity = mp.morph.capacity;
    for (std::uint32_t morph = 0; morph < mp.morphCount; ++morph) {
        ChainPlan chain;
        if (!PlanChain(pool, mp.morph.capacity, stride, morph, rates, options, chain, error)) {
            return false;
        }
        if (chain.empty) {
            if (!RecordOnly(pool, mp.morph.capacity, stride, chain, mp.morph.chains, error)) return false;
            continue;
        }
        const auto name = mp.morphs ? text_encoding::MorphLabel(mdl::Mdl(mp.model)->morphs[morph], mp.english) : L"";
        const std::wstring channel = mp.tag + L" / " + name + L" [morph " + std::to_wstring(morph) + L"]";
        NoteChain(chain, channel, changes, report);
        mp.maxFrame = std::max(mp.maxFrame, static_cast<std::uint32_t>(chain.groups.back().frame));
        if (!chain.changed) {
            if (!RecordOnly(pool, capacity, stride, chain, mp.morph.chains, error)) return false;
            continue;
        }
        std::vector<double> values;
        values.reserve(chain.slots.size());
        for (std::uint32_t slot : chain.slots) {
            values.push_back(ReadF32(pool + static_cast<std::size_t>(slot) * stride,
                                     offsetof(mdl::MorphKey, value)));
        }
        ac::Track input;
        if (!BuildTrack(chain, values, {}, false, input, error)) return false;
        LanePlan lane;
        if (!PlanLane(input, chain, rates, options, lane, error)) return false;
        watch.Note(channel, lane);

        SlotPatch patch;
        const bool applied = ApplyChain(pool, capacity, stride, chain,
            [&](Bytes& out, const std::uint8_t* original, std::size_t index) {
                WriteF32(out.data(), offsetof(mdl::MorphKey, value), lane.values[index]);
                const art::Group& group = chain.groups[index];
                std::uint8_t allocated = 0;
                for (std::size_t member = group.first; member <= group.last; ++member) {
                    allocated |= patch.original[member][offsetof(mdl::MorphKey, allocated)];
                }
                out[offsetof(mdl::MorphKey, allocated)] = allocated;
                (void)original;
            }, patch, error);
        if (!applied) return false;
        mp.morph.chains.push_back(std::move(patch));
    }
    for (const SlotPatch& patch : mp.morph.chains) {
        if (patch.changed) { mp.morph.changed = true; mp.changed = true; }
    }
    return true;
}

bool PlanDisplay(const art::Rates& rates, const art::Options& options, ModelPatch& mp,
                 Changes& changes, fc::Report& report, ErrorWatch& watch, std::string& error) {
    const std::uint8_t* pool = mp.display.pool;
    const std::size_t stride = sizeof(mdl::DisplayKey);
    const std::size_t capacity = mp.display.capacity;
    ChainPlan chain;
    if (!PlanChain(pool, mp.display.capacity, stride, 0, rates, options, chain, error)) return false;
    if (chain.empty) {
        return RecordOnly(pool, capacity, stride, chain, mp.display.chains, error);
    }
    const std::wstring channel = mp.tag + L".display";
    NoteChain(chain, channel, changes, report);
    mp.maxFrame = std::max(mp.maxFrame, static_cast<std::uint32_t>(chain.groups.back().frame));
    if (!chain.changed) return RecordOnly(pool, capacity, stride, chain, mp.display.chains, error);

    std::vector<double> visible;
    visible.reserve(chain.slots.size());
    for (std::uint32_t slot : chain.slots) {
        visible.push_back(static_cast<double>(
            pool[static_cast<std::size_t>(slot) * stride + offsetof(mdl::DisplayKey, visible)]));
    }
    ac::Track input;
    if (!BuildTrack(chain, visible, {}, true, input, error)) return false;
    LanePlan lane;
    if (!PlanLane(input, chain, rates, options, lane, error)) return false;
    watch.Note(channel, lane);

    const std::size_t ikBytes = mp.ikChainCount;
    const std::size_t selectorBytes = mp.boneOrderCount * sizeof(mdl::BoneReference);
    SlotPatch patch;
    const bool applied = ApplyChain(pool, capacity, stride, chain,
        [&](Bytes& out, const std::uint8_t* original, std::size_t index) {
            const art::Group& group = chain.groups[index];
            out[offsetof(mdl::DisplayKey, visible)] =
                static_cast<std::uint8_t>(std::llround(lane.values[index]));
            std::uint8_t allocated = 0;
            for (std::size_t member = group.first; member <= group.last; ++member) {
                allocated |= patch.original[member][offsetof(mdl::DisplayKey, allocated)];
            }
            out[offsetof(mdl::DisplayKey, allocated)] = allocated;
            // The state in force at the merged time is the group's last record -
            // except for a group that holds the original frame-zero key and
            // lands on frame zero, which keeps the frame-zero state.  Prepare
            // only RECORDS the candidate bytes here; writing them through the
            // live pointer would change the project before anything is verified
            // or committed.
            const bool baseline = chain.frames[group.first] == 0 && group.frame == 0;
            if (group.last > group.first && !baseline && ikBytes + selectorBytes > 0) {
                const std::uint8_t* last =
                    pool + static_cast<std::size_t>(chain.slots[group.last]) * stride;
                PayloadPatch payload;
                std::memcpy(&payload.ik, out.data() + offsetof(mdl::DisplayKey, ikStates),
                            sizeof(payload.ik));
                std::memcpy(&payload.selectors,
                            out.data() + offsetof(mdl::DisplayKey, selectorStates),
                            sizeof(payload.selectors));
                const std::uint8_t* sourceIk = nullptr;
                const std::uint8_t* sourceSelectors = nullptr;
                std::memcpy(&sourceIk, last + offsetof(mdl::DisplayKey, ikStates), sizeof(sourceIk));
                std::memcpy(&sourceSelectors, last + offsetof(mdl::DisplayKey, selectorStates),
                            sizeof(sourceSelectors));
                payload.ikBytes = ikBytes;
                payload.selectorBytes = selectorBytes;
                // The survivor keeps its own allocations, so the merged state is
                // copied into them rather than the two records sharing a buffer.
                if (payload.ik != nullptr && sourceIk != nullptr) {
                    payload.originalIk.assign(payload.ik, payload.ik + ikBytes);
                    payload.candidateIk.assign(sourceIk, sourceIk + ikBytes);
                }
                if (payload.selectors != nullptr && sourceSelectors != nullptr) {
                    payload.originalSelectors.assign(payload.selectors,
                                                     payload.selectors + selectorBytes);
                    payload.candidateSelectors.assign(sourceSelectors,
                                                      sourceSelectors + selectorBytes);
                }
                patch.payloads.push_back(std::move(payload));
            }
            (void)original;
        }, patch, error);
    if (!applied) return false;
    patch.ownsPayloads = true;
    mp.display.chains.push_back(std::move(patch));
    mp.display.changed = true;
    mp.changed = true;
    return true;
}

// ---------------------------------------------------------------------------
// Verification, application and rollback
// ---------------------------------------------------------------------------

std::wstring Wide(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

bool VerifyChain(const PoolPatch& poolPatch, const SlotPatch& patch, const char* what,
                 std::string& error) {
    if (patch.slots.empty()) return true;
    if (poolPatch.pool == nullptr) {
        error = std::string(what) + ": the key pool is gone";
        return false;
    }
    std::vector<std::uint32_t> live;
    std::string detail;
    if (!ReadChain(poolPatch.pool, poolPatch.capacity, poolPatch.stride, patch.slots.front(), live,
                   detail)) {
        error = std::string(what) + ": " + detail;
        return false;
    }
    if (live != patch.slots) {
        error = std::string(what) + ": the key chain changed since Prepare";
        return false;
    }
    std::uint64_t digest = 0;
    for (std::uint32_t slot : live) {
        digest = HashValue(digest, slot);
        digest = HashBytes(digest, poolPatch.pool + static_cast<std::size_t>(slot) * poolPatch.stride,
                           poolPatch.stride);
    }
    if (digest != patch.digest) {
        error = std::string(what) + ": a key changed since Prepare";
        return false;
    }
    return true;
}

// The display payloads of one chain.  Prepare recorded the bytes; these two
// copy them, so neither direction ever allocates or shares an allocation.
void ApplyPayloads(const SlotPatch& patch) {
    for (const PayloadPatch& payload : patch.payloads) {
        if (payload.ik != nullptr && payload.candidateIk.size() == payload.ikBytes) {
            std::memcpy(payload.ik, payload.candidateIk.data(), payload.ikBytes);
        }
        if (payload.selectors != nullptr &&
            payload.candidateSelectors.size() == payload.selectorBytes) {
            std::memcpy(payload.selectors, payload.candidateSelectors.data(), payload.selectorBytes);
        }
    }
}

void RevertPayloads(const SlotPatch& patch) {
    for (const PayloadPatch& payload : patch.payloads) {
        if (payload.ik != nullptr && payload.originalIk.size() == payload.ikBytes) {
            std::memcpy(payload.ik, payload.originalIk.data(), payload.ikBytes);
        }
        if (payload.selectors != nullptr &&
            payload.originalSelectors.size() == payload.selectorBytes) {
            std::memcpy(payload.selectors, payload.originalSelectors.data(), payload.selectorBytes);
        }
    }
}

void ApplyPatch(std::uint8_t* pool, std::size_t stride, const SlotPatch& patch) {
    if (pool == nullptr) return;
    for (std::size_t index = 0; index < patch.writeSlots.size(); ++index) {
        std::memcpy(pool + static_cast<std::size_t>(patch.writeSlots[index]) * stride,
                    patch.written[index].data(), stride);
    }
    ApplyPayloads(patch);
    for (std::uint32_t slot : patch.clearSlots) {
        std::memset(pool + static_cast<std::size_t>(slot) * stride, 0, stride);
    }
}

void RevertPatch(std::uint8_t* pool, std::size_t stride, const SlotPatch& patch) {
    if (pool == nullptr) return;
    // The payloads first: they live behind pointers the record bytes carry, and
    // they are never freed before the commit is final, so both orders are safe
    // and this one keeps the record as the last word.
    RevertPayloads(patch);
    for (std::size_t index = 0; index < patch.slots.size(); ++index) {
        std::memcpy(pool + static_cast<std::size_t>(patch.slots[index]) * stride,
                    patch.original[index].data(), stride);
    }
}

// Frees the display payloads of the slots a commit dropped.  Their records have
// already been zeroed, so the original record is the only place the pointers
// still exist; this runs after the commit is final, which is why a rollback can
// restore the records and keep the payloads alive.
void ReleaseClearedPayloads(const SlotPatch& patch, const PoolPatch& pool) {
    if (!patch.ownsPayloads || pool.pool == nullptr) return;
    for (std::size_t index = 0; index < patch.slots.size(); ++index) {
        const bool cleared = std::find(patch.clearSlots.begin(), patch.clearSlots.end(),
                                       patch.slots[index]) != patch.clearSlots.end();
        if (!cleared) continue;
        void* ik = nullptr;
        void* selectors = nullptr;
        std::memcpy(&ik, patch.original[index].data() + offsetof(mdl::DisplayKey, ikStates),
                    sizeof(ik));
        std::memcpy(&selectors, patch.original[index].data() + offsetof(mdl::DisplayKey, selectorStates),
                    sizeof(selectors));
        std::free(ik);
        std::free(selectors);
    }
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// Application-wide tables
// ---------------------------------------------------------------------------

struct ScalarChannel {
    bool discrete = false;
    std::function<double(const std::uint8_t*)> read;
    std::function<void(std::uint8_t*, double)> write;
};

// One chain whose records carry no interpolation tuple: every interval is
// linear, a discrete channel is a Step hold, and a merge samples the group's
// own linear curve (or keeps its last state when the channel is discrete).
bool PlanScalarChain(const art::Rates& rates, const art::Options& options, PoolPatch& target,
                     const std::wstring& channelName, const std::vector<ScalarChannel>& components,
                     std::size_t selectedOffset, std::uint32_t& maxFrame, Changes& changes,
                     fc::Report& report, ErrorWatch& watch, std::string& error) {
    ChainPlan chain;
    if (!PlanChain(target.pool, target.capacity, target.stride, 0, rates, options, chain, error)) {
        return false;
    }
    if (chain.empty) {
        return RecordOnly(target.pool, target.capacity, target.stride, chain, target.chains, error);
    }
    NoteChain(chain, channelName, changes, report);
    maxFrame = std::max(maxFrame, static_cast<std::uint32_t>(chain.groups.back().frame));
    if (!chain.changed) return RecordOnly(target.pool, target.capacity, target.stride, chain, target.chains, error);
    std::vector<LanePlan> lanes(components.size());
    for (std::size_t index = 0; index < components.size(); ++index) {
        std::vector<double> values;
        values.reserve(chain.slots.size());
        for (std::uint32_t slot : chain.slots) {
            values.push_back(components[index].read(
                target.pool + static_cast<std::size_t>(slot) * target.stride));
        }
        ac::Track input;
        if (!BuildTrack(chain, values, {}, components[index].discrete, input, error)) return false;
        if (!PlanLane(input, chain, rates, options, lanes[index], error)) return false;
        watch.Note(channelName, lanes[index]);
    }
    SlotPatch patch;
    const bool applied = ApplyChain(target.pool, target.capacity, target.stride, chain,
        [&](Bytes& out, const std::uint8_t* original, std::size_t index) {
            for (std::size_t component = 0; component < components.size(); ++component) {
                components[component].write(out.data(), lanes[component].values[index]);
            }
            if (selectedOffset != static_cast<std::size_t>(-1)) {
                // The selection mark is monotone across a merge.
                std::uint8_t selected = 0;
                const art::Group& group = chain.groups[index];
                for (std::size_t member = group.first; member <= group.last; ++member) {
                    selected |= patch.original[member][selectedOffset];
                }
                out[selectedOffset] = selected;
            }
            (void)original;
        }, patch, error);
    if (!applied) return false;
    target.chains.push_back(std::move(patch));
    target.changed = true;
    return true;
}

bool PlanCamera(const art::Rates& rates, const art::Options& options, AppPatch& ap,
                Changes& changes, fc::Report& report, ErrorWatch& watch, std::string& error) {
    const std::uint8_t* pool = ap.camera.pool;
    const std::size_t capacity = ap.camera.capacity;
    const std::size_t stride = sizeof(mdl::CameraKey);
    ChainPlan chain;
    if (!PlanChain(pool, capacity, stride, 0, rates, options, chain, error)) return false;
    if (chain.empty) {
        return RecordOnly(pool, capacity, stride, chain, ap.camera.chains, error);
    }
    NoteChain(chain, L"camera", changes, report);
    ap.lastRegistered = std::max(ap.lastRegistered,
                                 static_cast<std::uint32_t>(chain.groups.back().frame));
    if (!chain.changed) return RecordOnly(pool, capacity, stride, chain, ap.camera.chains, error);

    if (chain.counters.mergeGroups == 0) {
        SlotPatch patch;
        if (!ApplyChain(pool, capacity, stride, chain,
                        [](Bytes&, const std::uint8_t*, std::size_t) {}, patch, error)) return false;
        ap.camera.chains.push_back(std::move(patch));
        ap.camera.changed = true;
        return true;
    }

    constexpr int kComponentCount = 11;
    constexpr int kLaneCount = 6;
    // Component -> interpolation lane.  The three rotation components share
    // lane 3, exactly as the camera record stores one channel per axis and one
    // for the rotation triple.
    const int componentLane[kComponentCount] = {0, 1, 2, 3, 3, 3, 4, 5, -1, -1, -1};
    const bool componentDiscrete[kComponentCount] = {false, false, false, false, false, false,
                                                     false, false, true,  true,  true};
    const int laneOwner[kLaneCount] = {0, 1, 2, 3, 6, 7};

    std::vector<Controls> controls[kLaneCount];
    for (int lane = 0; lane < kLaneCount; ++lane) {
        controls[lane].assign(chain.slots.size(), Controls{});
    }
    for (std::size_t index = 0; index < chain.slots.size(); ++index) {
        const std::uint8_t* record = pool + static_cast<std::size_t>(chain.slots[index]) * stride;
        for (int lane = 0; lane < kLaneCount; ++lane) {
            const std::size_t at = offsetof(mdl::CameraKey, interpolation) + static_cast<std::size_t>(lane);
            std::uint8_t tuple[4] = {record[at], record[at + 6], record[at + 12], record[at + 18]};
            std::memcpy(controls[lane][index].value.data(), tuple, 4);
        }
    }

    std::vector<LanePlan> lanes(kComponentCount);
    for (int component = 0; component < kComponentCount; ++component) {
        std::vector<double> values;
        values.reserve(chain.slots.size());
        for (std::uint32_t slot : chain.slots) {
            const std::uint8_t* record = pool + static_cast<std::size_t>(slot) * stride;
            switch (component) {
                case 0: case 1: case 2:
                    values.push_back(ReadF32(record, offsetof(mdl::CameraKey, eye) +
                                                         static_cast<std::size_t>(component) * 4));
                    break;
                case 3: case 4: case 5:
                    values.push_back(ReadF32(record, offsetof(mdl::CameraKey, target) +
                                                         static_cast<std::size_t>(component - 3) * 4));
                    break;
                case 6:
                    values.push_back(ReadF32(record, offsetof(mdl::CameraKey, distance)));
                    break;
                case 7:
                    values.push_back(static_cast<double>(ReadI32(record, offsetof(mdl::CameraKey, fov))));
                    break;
                case 8:
                    values.push_back(static_cast<double>(record[offsetof(mdl::CameraKey, perspective)]));
                    break;
                case 9:
                    values.push_back(static_cast<double>(ReadI32(record, offsetof(mdl::CameraKey, parentModel))));
                    break;
                default:
                    values.push_back(static_cast<double>(ReadI32(record, offsetof(mdl::CameraKey, parentBone))));
                    break;
            }
        }
        ac::Track input;
        const std::vector<Controls>& intervalControls =
            componentLane[component] >= 0 ? controls[componentLane[component]] : controls[0];
        if (!BuildTrack(chain, values, intervalControls, componentDiscrete[component], input, error)) {
            return false;
        }
        if (!PlanLane(input, chain, rates, options, lanes[component], error,
                      componentLane[component] >= 0 ? &intervalControls : nullptr)) return false;
        watch.Note(L"camera", lanes[component]);
    }

    std::vector<double> rotationPositions;
    for (std::size_t i = 0; i < chain.slots.size(); ++i) rotationPositions.push_back(static_cast<double>(i));
    ac::Track rotationTiming;
    LanePlan rotationLane;
    if (!BuildTrack(chain, rotationPositions, controls[3], false, rotationTiming, error) ||
        !PlanLane(rotationTiming, chain, rates, options, rotationLane, error, &controls[3])) return false;
    watch.Note(L"camera.rotation", rotationLane);

    SlotPatch patch;
    const bool applied = ApplyChain(pool, capacity, stride, chain,
        [&](Bytes& out, const std::uint8_t* original, std::size_t index) {
            for (int lane = 0; lane < kLaneCount; ++lane) {
                if (index == 0 || (!chain.outMerged[index] && !chain.outMerged[index - 1])) continue;
                const LanePlan& owner = lane == 3 ? rotationLane : lanes[laneOwner[lane]];
                if (owner.controls[index].step) continue;
                const std::size_t at = offsetof(mdl::CameraKey, interpolation) +
                                       static_cast<std::size_t>(lane);
                const auto& value = owner.controls[index].value;
                out[at] = value[0];
                out[at + 6] = value[1];
                out[at + 12] = value[2];
                out[at + 18] = value[3];
            }
            for (int axis = 0; axis < 3; ++axis) {
                WriteF32(out.data(), offsetof(mdl::CameraKey, eye) +
                                         static_cast<std::size_t>(axis) * 4,
                         lanes[axis].values[index]);
                WriteF32(out.data(), offsetof(mdl::CameraKey, target) +
                                         static_cast<std::size_t>(axis) * 4,
                         lanes[3 + axis].values[index]);
            }
            WriteF32(out.data(), offsetof(mdl::CameraKey, distance), lanes[6].values[index]);
            WriteI32(out.data(), offsetof(mdl::CameraKey, fov),
                     static_cast<std::int32_t>(std::llround(lanes[7].values[index])));
            out[offsetof(mdl::CameraKey, perspective)] =
                static_cast<std::uint8_t>(std::llround(lanes[8].values[index]));
            WriteI32(out.data(), offsetof(mdl::CameraKey, parentModel),
                     static_cast<std::int32_t>(std::llround(lanes[9].values[index])));
            WriteI32(out.data(), offsetof(mdl::CameraKey, parentBone),
                     static_cast<std::int32_t>(std::llround(lanes[10].values[index])));
            std::uint8_t selected = 0;
            const art::Group& group = chain.groups[index];
            for (std::size_t member = group.first; member <= group.last; ++member) {
                selected |= patch.original[member][offsetof(mdl::CameraKey, selected)];
            }
            out[offsetof(mdl::CameraKey, selected)] = selected;
            (void)original;
        }, patch, error);
    if (!applied) return false;
    ap.camera.chains.push_back(std::move(patch));
    ap.camera.changed = true;
    return true;
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// Prepare
// ---------------------------------------------------------------------------

bool PlanModel(MMDApp& app, const art::Rates& rates, const art::Options& options,
               const FrameMath& math, Plan& plan, int slot, Changes& changes, fc::Report& report,
               ErrorWatch& watch, std::string& error) {
    unsigned char* const model = app.ModelSlot(slot);
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    ModelPatch mp;
    mp.model = model;
    mp.slot = slot;
    mp.english = UiUsesEnglishNames(app.EnglishUI());
    mp.tag = text_encoding::ModelLabel(record, mp.english) + L" [model " + std::to_wstring(slot) + L"]";
    mp.morphs = record.morphs;
    mp.bones = record.boneTable;
    mp.morphCount = record.morphCount;
    mp.boneCount = record.boneCount;
    mp.ikChainCount = record.ikChainCount;
    mp.boneOrderCount = record.boneOrderCount;
    mp.searchCursor = record.searchCursor;
    mp.oldMaxFrame = record.maxFrame;
    mp.identityDigest = HashValue(0, reinterpret_cast<std::uintptr_t>(model));
    mp.identityDigest = HashValue(mp.identityDigest, reinterpret_cast<std::uintptr_t>(mp.morphs));
    mp.identityDigest = HashValue(mp.identityDigest, reinterpret_cast<std::uintptr_t>(mp.bones));
    mp.identityDigest = HashValue(mp.identityDigest, mp.morphCount);
    mp.identityDigest = HashValue(mp.identityDigest, mp.boneCount);

    mp.bone.pool = reinterpret_cast<std::uint8_t*>(mdl::BoneKeys(model));
    mp.bone.capacity = mdl::BoneKeyCapacity(record);
    mp.bone.stride = sizeof(mdl::BoneKey);
    mp.morph.pool = reinterpret_cast<std::uint8_t*>(mdl::MorphKeys(model));
    mp.morph.capacity = mdl::MorphKeyCapacity(record);
    mp.morph.stride = sizeof(mdl::MorphKey);
    mp.display.pool = reinterpret_cast<std::uint8_t*>(mdl::DisplayKeys(model));
    mp.display.capacity = mdl::KeyPoolCapacity(mdl::DisplayKeys(model));
    mp.display.stride = sizeof(mdl::DisplayKey);

    // The scale side table is exchanged as a whole, so the original has to be
    // taken before anything is planned.
    mp.hasCurves = bone_scale::CaptureCurveTable(model, mp.curves);
    if (mp.hasCurves && !mp.bone.pool) mp.hasCurves = false;
    mp.retimedCurves = mp.curves;

    if (mp.bone.pool != nullptr) {
        if (!PlanBones(rates, options, math, mp, changes, report, watch, error)) return false;
        if (mp.curvesMoved) ++mp.retimedCurves.revision;
        mp.maxFrame = std::max(mp.maxFrame, 0u);
    }
    if (mp.morph.pool != nullptr) {
        if (!PlanMorphs(rates, options, mp, changes, report, watch, error)) return false;
    }
    if (mp.display.pool != nullptr) {
        if (!PlanDisplay(rates, options, mp, changes, report, watch, error)) return false;
        for (const auto& chain : mp.display.chains) for (const auto keySlot : chain.slots) {
            const auto& key = mdl::DisplayKeys(model)[keySlot];
            PayloadPatch payload;
            payload.ik = key.ikStates;
            payload.selectors = reinterpret_cast<std::uint8_t*>(key.selectorStates);
            payload.ikBytes = mp.ikChainCount;
            payload.selectorBytes = std::size_t(mp.boneOrderCount) * sizeof(mdl::BoneReference);
            if (payload.ik && payload.ikBytes)
                payload.originalIk.assign(payload.ik, payload.ik + payload.ikBytes);
            if (payload.selectors && payload.selectorBytes) {
                const auto bytes = reinterpret_cast<const std::uint8_t*>(payload.selectors);
                payload.originalSelectors.assign(bytes, bytes + payload.selectorBytes);
            }
            mp.displayPayloads.push_back(std::move(payload));
        }
    }
    // The authored expression layers follow the native morph keys.  The
    // candidate is built here, at Prepare time: RebuildLayers leaves the mirror
    // and the observed chain for the module's own derive to fill in from the
    // pool it sees after the write, so Commit has nothing to build.
    if (!morph_curve::CaptureLayers(model, mp.layers, error)) return false;
    mp.hasLayers = true;
    if (mp.morph.changed) {
        std::vector<ac::NamedTrack> authored;
        if (!morph_curve::LayersTracks(mp.layers, authored, error)) return false;
        if (!authored.empty()) {
            std::vector<ac::NamedTrack> retimed;
            retimed.reserve(authored.size());
            for (const ac::NamedTrack& track : authored) {
                ac::Track converted;
                art::Report laneReport;
                if (!art::Retime(track.track, rates, options, converted, laneReport, error)) {
                    return false;
                }
                LanePlan measured;
                measured.sampledError = laneReport.maxSampledValueError;
                measured.shapePreserved = laneReport.curveShapePreserved;
                std::wstring name = Wide(track.id);
                for (std::uint32_t morph = 0; morph < mp.morphCount; ++morph) {
                    if (morph_curve::StableId(morph, record.morphs[morph].name) == track.id) {
                        name = text_encoding::MorphLabel(record.morphs[morph], mp.english) +
                            L" [morph " + std::to_wstring(morph) + L"]";
                        break;
                    }
                }
                watch.Note(mp.tag + L" / " + name, measured);
                retimed.push_back(ac::NamedTrack{track.id, std::move(converted)});
            }
            if (!morph_curve::RebuildLayers(model, retimed, mp.retimedLayers, error)) return false;
            // The standard key records and their authored extension must agree
            // at the surviving frames, including a merged Bezier sample.
            for (auto& patch : mp.morph.chains) {
                if (!patch.changed || patch.slots.empty()) continue;
                const auto morph = patch.slots.front();
                const auto id = morph_curve::StableId(morph, record.morphs[morph].name);
                const auto named = std::find_if(retimed.begin(), retimed.end(),
                    [&](const ac::NamedTrack& item) { return item.id == id; });
                if (named == retimed.end()) continue;
                for (auto& bytes : patch.written) {
                    const auto frame = ReadU32(bytes.data(), 0);
                    WriteF32(bytes.data(), offsetof(mdl::MorphKey, value),
                             ac::Evaluate(named->track, frame).value);
                }
            }
            mp.hasLayers = true;
            mp.layersMoved = true;
        }
    }
    // The conversion is recorded even when nothing moved, so Commit can verify
    // the identity of a model it deliberately left alone.
    plan.models.push_back(std::move(mp));
    report.models += 1;
    return true;
}

}  // namespace

namespace {

bool PrepareImpl(MMDApp& app, const fc::Rates& rates, PlanPtr& plan, fc::Report& report,
                 std::wstring& error) {
    error.clear();
    if (!std::isfinite(rates.originalFPS) || !std::isfinite(rates.currentFPS) ||
        !(rates.originalFPS > 0.0) || !(rates.currentFPS > 0.0)) {
        error = Localized(app, L"フレームレートは有限の正の値でなければなりません。",
                          L"Both frame rates must be finite and positive.",
                          L"两个帧率都必须是有限的正数。");
        return false;
    }

    art::Rates applyRates;
    applyRates.originalFPS = rates.originalFPS;
    applyRates.currentFPS = rates.currentFPS;

    auto built = std::make_shared<Plan>();
    built->rates = rates;
    built->app.app = &app;
    built->options.maximumFrame = kMaximumFrame;
    const art::Options& options = built->options;

    Changes changes;
    const FrameMath frameMath = MakeFrameMath(rates.originalFPS, rates.currentFPS);
    changes.Reset(frameMath);
    ErrorWatch watch;
    fc::Report out;
    std::string detail;

    // ---- application-wide tables ------------------------------------------
    AppPatch& ap = built->app;
    for (int slot = 0; slot < kModelSlotCount; ++slot) ap.modelIdentities[slot] = app.ModelSlot(slot);
    for (int slot = 0; slot < static_cast<int>(kAccessorySlots); ++slot) {
        ap.accessoryIdentities[slot] = app.AccessorySlot(slot);
        ap.accessoryPools[slot] = app.AccessoryKeys(slot);
    }
    ap.cameraCursor = app.CameraTrackCursor();
    ap.lightCursor = app.LightTrackCursor();
    ap.shadowCursor = app.ShadowTrackCursor();
    ap.gravityCursor = app.GravityTrackCursor();
    ap.lastRegistered = app.LastRegisteredFrame();
    ap.oldLastRegistered = ap.lastRegistered;
    ap.sceneModified = app.SceneModified();

    ap.camera.pool = reinterpret_cast<std::uint8_t*>(app.CameraKeys());
    ap.camera.capacity = mdl::KeyPoolCapacity(app.CameraKeys());
    ap.camera.stride = sizeof(mdl::CameraKey);
    ap.light.pool = reinterpret_cast<std::uint8_t*>(app.LightKeys());
    ap.light.capacity = mdl::KeyPoolCapacity(app.LightKeys());
    ap.light.stride = sizeof(mdl::LightKey);
    ap.shadow.pool = reinterpret_cast<std::uint8_t*>(app.ShadowKeys());
    ap.shadow.capacity = mdl::KeyPoolCapacity(app.ShadowKeys());
    ap.shadow.stride = sizeof(mdl::SelfShadowKey);
    ap.gravity.pool = reinterpret_cast<std::uint8_t*>(app.GravityKeys());
    ap.gravity.capacity = mdl::KeyPoolCapacity(app.GravityKeys());
    ap.gravity.stride = sizeof(mdl::GravityKey);

    if (!PlanCamera(applyRates, options, ap, changes, out, watch, detail)) {
        error = Localized(app, L"カメラ トラックを変換できません。", L"The camera track cannot be converted.",
                          L"无法转换相机轨道。") + L" (" + Wide(detail) + L")";
        return false;
    }

    {
        std::vector<ScalarChannel> components;
        for (int axis = 0; axis < 3; ++axis) {
            const std::size_t offset = offsetof(mdl::LightKey, direction) + static_cast<std::size_t>(axis) * 4;
            components.push_back(ScalarChannel{false,
                [offset](const std::uint8_t* record) { return ReadF32(record, offset); },
                [offset](std::uint8_t* record, double value) { WriteF32(record, offset, value); }});
        }
        for (int axis = 0; axis < 3; ++axis) {
            const std::size_t offset = offsetof(mdl::LightKey, color) + static_cast<std::size_t>(axis) * 4;
            components.push_back(ScalarChannel{false,
                [offset](const std::uint8_t* record) { return ReadF32(record, offset); },
                [offset](std::uint8_t* record, double value) { WriteF32(record, offset, value); }});
        }
        if (!PlanScalarChain(applyRates, options, ap.light, L"light", components,
                             offsetof(mdl::LightKey, selected), ap.lastRegistered, changes, out, watch,
                             detail)) {
            error = Localized(app, L"照明トラックを変換できません。", L"The light track cannot be converted.",
                              L"无法转换灯光轨道。") + L" (" + Wide(detail) + L")";
            return false;
        }
    }
    {
        std::vector<ScalarChannel> components;
        components.push_back(ScalarChannel{true,
            [](const std::uint8_t* record) {
                return static_cast<double>(record[offsetof(mdl::SelfShadowKey, mode)]);
            },
            [](std::uint8_t* record, double value) {
                record[offsetof(mdl::SelfShadowKey, mode)] = static_cast<std::uint8_t>(std::llround(value));
            }});
        components.push_back(ScalarChannel{false,
            [](const std::uint8_t* record) {
                return ReadF32(record, offsetof(mdl::SelfShadowKey, distance));
            },
            [](std::uint8_t* record, double value) {
                WriteF32(record, offsetof(mdl::SelfShadowKey, distance), value);
            }});
        if (!PlanScalarChain(applyRates, options, ap.shadow, L"shadow", components,
                             offsetof(mdl::SelfShadowKey, selected), ap.lastRegistered, changes, out,
                             watch, detail)) {
            error = Localized(app, L"セルフシャドウ トラックを変換できません。",
                              L"The self-shadow track cannot be converted.",
                              L"无法转换自阴影轨道。") + L" (" + Wide(detail) + L")";
            return false;
        }
    }
    {
        std::vector<ScalarChannel> components;
        components.push_back(ScalarChannel{false,
            [](const std::uint8_t* record) {
                return ReadF32(record, offsetof(mdl::GravityKey, acceleration));
            },
            [](std::uint8_t* record, double value) {
                WriteF32(record, offsetof(mdl::GravityKey, acceleration), value);
            }});
        for (int axis = 0; axis < 3; ++axis) {
            const std::size_t offset = offsetof(mdl::GravityKey, direction) + static_cast<std::size_t>(axis) * 4;
            components.push_back(ScalarChannel{false,
                [offset](const std::uint8_t* record) { return ReadF32(record, offset); },
                [offset](std::uint8_t* record, double value) { WriteF32(record, offset, value); }});
        }
        components.push_back(ScalarChannel{true,
            [](const std::uint8_t* record) {
                return static_cast<double>(ReadI32(record, offsetof(mdl::GravityKey, noise)));
            },
            [](std::uint8_t* record, double value) {
                WriteI32(record, offsetof(mdl::GravityKey, noise),
                         static_cast<std::int32_t>(std::llround(value)));
            }});
        components.push_back(ScalarChannel{true,
            [](const std::uint8_t* record) {
                return static_cast<double>(record[offsetof(mdl::GravityKey, noiseEnabled)]);
            },
            [](std::uint8_t* record, double value) {
                record[offsetof(mdl::GravityKey, noiseEnabled)] =
                    static_cast<std::uint8_t>(std::llround(value));
            }});
        if (!PlanScalarChain(applyRates, options, ap.gravity, L"gravity", components,
                             offsetof(mdl::GravityKey, selected), ap.lastRegistered, changes, out,
                             watch, detail)) {
            error = Localized(app, L"重力トラックを変換できません。", L"The gravity track cannot be converted.",
                              L"无法转换重力轨道。") + L" (" + Wide(detail) + L")";
            return false;
        }
    }

    // ---- accessories -------------------------------------------------------
    for (int slot = 0; slot < static_cast<int>(kAccessorySlots); ++slot) {
        mdl::AccessoryRecord* object = app.AccessorySlot(slot);
        if (object == nullptr) continue;
        PoolPatch pool;
        pool.pool = reinterpret_cast<std::uint8_t*>(app.AccessoryKeys(slot));
        pool.capacity = mdl::KeyPoolCapacity(app.AccessoryKeys(slot));
        pool.stride = sizeof(mdl::AccessoryKey);
        if (pool.pool == nullptr) continue;
        std::vector<ScalarChannel> components;
        for (int axis = 0; axis < 3; ++axis) {
            const std::size_t offset = offsetof(mdl::AccessoryKey, position) + static_cast<std::size_t>(axis) * 4;
            components.push_back(ScalarChannel{false,
                [offset](const std::uint8_t* record) { return ReadF32(record, offset); },
                [offset](std::uint8_t* record, double value) { WriteF32(record, offset, value); }});
        }
        for (int axis = 0; axis < 3; ++axis) {
            const std::size_t offset = offsetof(mdl::AccessoryKey, rotation) + static_cast<std::size_t>(axis) * 4;
            components.push_back(ScalarChannel{false,
                [offset](const std::uint8_t* record) { return ReadF32(record, offset); },
                [offset](std::uint8_t* record, double value) { WriteF32(record, offset, value); }});
        }
        components.push_back(ScalarChannel{false,
            [](const std::uint8_t* record) { return ReadF32(record, offsetof(mdl::AccessoryKey, scale)); },
            [](std::uint8_t* record, double value) { WriteF32(record, offsetof(mdl::AccessoryKey, scale), value); }});
        components.push_back(ScalarChannel{false,
            [](const std::uint8_t* record) { return ReadF32(record, offsetof(mdl::AccessoryKey, opacity)); },
            [](std::uint8_t* record, double value) { WriteF32(record, offsetof(mdl::AccessoryKey, opacity), value); }});
        components.push_back(ScalarChannel{true,
            [](const std::uint8_t* record) {
                return static_cast<double>(record[offsetof(mdl::AccessoryKey, visible)]);
            },
            [](std::uint8_t* record, double value) {
                record[offsetof(mdl::AccessoryKey, visible)] = static_cast<std::uint8_t>(std::llround(value));
            }});
        components.push_back(ScalarChannel{true,
            [](const std::uint8_t* record) {
                return static_cast<double>(record[offsetof(mdl::AccessoryKey, shadowEnabled)]);
            },
            [](std::uint8_t* record, double value) {
                record[offsetof(mdl::AccessoryKey, shadowEnabled)] =
                    static_cast<std::uint8_t>(std::llround(value));
            }});
        components.push_back(ScalarChannel{true,
            [](const std::uint8_t* record) {
                return static_cast<double>(ReadI32(record, offsetof(mdl::AccessoryKey, parentModel)));
            },
            [](std::uint8_t* record, double value) {
                WriteI32(record, offsetof(mdl::AccessoryKey, parentModel),
                         static_cast<std::int32_t>(std::llround(value)));
            }});
        components.push_back(ScalarChannel{true,
            [](const std::uint8_t* record) {
                return static_cast<double>(ReadI32(record, offsetof(mdl::AccessoryKey, parentBone)));
            },
            [](std::uint8_t* record, double value) {
                WriteI32(record, offsetof(mdl::AccessoryKey, parentBone),
                         static_cast<std::int32_t>(std::llround(value)));
            }});
        const std::wstring channelName = text_encoding::Display(object->name) + L" [accessory " + std::to_wstring(slot) + L"]";
        if (!PlanScalarChain(applyRates, options, pool, channelName, components,
                             offsetof(mdl::AccessoryKey, selected), ap.lastRegistered, changes, out,
                             watch, detail)) {
            error = Localized(app, L"アクセサリ トラックを変換できません。",
                              L"An accessory track cannot be converted.",
                              L"无法转换配件轨道。") + L" (" + Wide(detail) + L")";
            return false;
        }
        ap.accessorySlots.push_back(slot);
        ap.accessoryCursors.push_back(app.AccessoryTrackCursor(slot));
        ap.accessories.push_back(std::move(pool));
        out.accessories += 1;
    }

    // ---- the host's authoring for those tracks ----------------------------
    if (!curve_host::CaptureAuthoring(app, ap.authoring, error)) return false;
    ap.hasAuthoring = true;
    std::vector<curve_host::AuthoringEntry> entries;
    if (!curve_host::AuthoringEntries(ap.authoring, entries)) {
        error = Localized(app, L"カーブ エディタの編集内容を読み出せません。",
                          L"The curve editor's authoring cannot be read.",
                          L"无法读取曲线编辑器的编辑内容。");
        return false;
    }
    if (!entries.empty()) {
        ap.hasAuthoring = true;
        for (curve_host::AuthoringEntry& entry : entries) {
            for (curve_editor::Channel& channel : entry.channels) {
                ac::Track converted;
                art::Report laneReport;
                if (!art::Retime(channel.track, applyRates, options, converted, laneReport, detail)) {
                    error = Localized(app, L"カーブ エディタの編集内容を変換できません。",
                                      L"The curve editor's authoring cannot be converted.",
                                      L"无法转换曲线编辑器的编辑内容。") + L" (" + Wide(detail) + L")";
                    return false;
                }
                channel.track = std::move(converted);
            }
            for (curve_editor::Channel& channel : entry.baseline) {
                ac::Track converted;
                art::Report laneReport;
                if (!art::Retime(channel.track, applyRates, options, converted, laneReport, detail)) {
                    error = Localized(app, L"カーブ エディタの編集内容を変換できません。",
                                      L"The curve editor's authoring cannot be converted.",
                                      L"无法转换曲线编辑器的编辑内容。") + L" (" + Wide(detail) + L")";
                    return false;
                }
                channel.track = std::move(converted);
            }
            PoolPatch* pool = nullptr;
            switch (entry.kind) {
            case curve_host::Kind::Camera: pool = &ap.camera; break;
            case curve_host::Kind::Light: pool = &ap.light; break;
            case curve_host::Kind::Shadow: pool = &ap.shadow; break;
            case curve_host::Kind::Gravity: pool = &ap.gravity; break;
            case curve_host::Kind::Accessory: {
                const auto slot = std::find(ap.accessorySlots.begin(), ap.accessorySlots.end(), entry.slot);
                if (slot != ap.accessorySlots.end()) pool = &ap.accessories[slot - ap.accessorySlots.begin()];
                break;
            }
            }
            if (pool) for (auto& chain : pool->chains) for (auto& bytes : chain.written) {
                if (!curve_host::SampleRetimedKey(entry, bytes.data(), bytes.size())) {
                    error = Localized(app, L"変換後のキーをサンプリングできません。",
                        L"Cannot sample a retimed native key.", L"无法采样修正后的原生关键帧。");
                    return false;
                }
            }
        }
        ap.retimedAuthoring = std::move(entries);
        ap.authoringMoved = true;
    }

    // ---- models ------------------------------------------------------------
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        if (app.ModelSlot(slot) == nullptr) continue;
        if (!PlanModel(app, applyRates, options, frameMath, *built, slot, changes, out, watch, detail)) {
            error = Localized(app, L"モデルのキーフレームを変換できません。",
                              L"A model's keyframes cannot be converted.",
                              L"无法转换某个模型的关键帧。") + L" (" + Wide(detail) + L")";
            return false;
        }
    }

    changes.Publish(out);
    out.maxSampledValueError = watch.maximum;
    out.worstChannel = watch.channelName;
    out.mmvTracks = 0;
    out.meshCaches = 0;

    plan = std::move(built);
    report = std::move(out);
    return true;
}

}  // namespace

// The public entry point.  Everything that can fail - including not being able
// to allocate the change list itself - becomes a message here.  A Report that
// silently lost detail entries would be worse than a refused conversion.
bool Prepare(MMDApp& app, const fc::Rates& rates, PlanPtr& plan, fc::Report& report,
             std::wstring& error) {
    error.clear();
    try {
        return PrepareImpl(app, rates, plan, report, error);
    } catch (const std::bad_alloc&) {
        error = Localized(app, L"メモリが不足しているため変換計画を作成できません。",
                          L"There is not enough memory to prepare the conversion.",
                          L"内存不足，无法准备转换计划。");
        return false;
    } catch (const std::exception& failure) {
        error = Localized(app, L"変換計画の作成中にエラーが発生しました。",
                          L"The conversion plan could not be prepared.",
                          L"准备转换计划时发生错误。") +
                L" (" + Wide(failure.what()) + L")";
        return false;
    }
}

// ---------------------------------------------------------------------------
// Commit
// ---------------------------------------------------------------------------

bool VerifyModel(MMDApp& app, const ModelPatch& mp, std::string& error) {
    if (app.ModelSlot(mp.slot) != mp.model) {
        error = "a model slot changed since Prepare";
        return false;
    }
    const mdl::ModelRecord& record = *mdl::Mdl(mp.model);
    if (mp.bone.pool != reinterpret_cast<std::uint8_t*>(mdl::BoneKeys(mp.model)) ||
        mp.morph.pool != reinterpret_cast<std::uint8_t*>(mdl::MorphKeys(mp.model)) ||
        mp.display.pool != reinterpret_cast<std::uint8_t*>(mdl::DisplayKeys(mp.model)) ||
        mp.bone.capacity != mdl::BoneKeyCapacity(record) ||
        mp.ikChainCount != record.ikChainCount || mp.boneOrderCount != record.boneOrderCount) {
        error = "a model key pool or display layout changed since Prepare";
        return false;
    }
    std::uint64_t digest = HashValue(0, reinterpret_cast<std::uintptr_t>(mp.model));
    digest = HashValue(digest, reinterpret_cast<std::uintptr_t>(record.morphs));
    digest = HashValue(digest, reinterpret_cast<std::uintptr_t>(record.boneTable));
    digest = HashValue(digest, record.morphCount);
    digest = HashValue(digest, record.boneCount);
    if (digest != mp.identityDigest || record.morphCount != mp.morphCount ||
        record.boneCount != mp.boneCount) {
        error = "a model changed since Prepare";
        return false;
    }
    const char* names[3] = {"model bone", "model morph", "model display"};
    const PoolPatch* pools[3] = {&mp.bone, &mp.morph, &mp.display};
    for (int index = 0; index < 3; ++index) {
        if (pools[index]->pool == nullptr) continue;
        for (const SlotPatch& patch : pools[index]->chains) {
            if (!VerifyChain(*pools[index], patch, names[index], error)) return false;
        }
    }
    {
        bone_scale::CurveTableState live;
        const bool present = bone_scale::CaptureCurveTable(mp.model, live);
        if (present != mp.hasCurves || (present && (live.revision != mp.curves.revision ||
            live.curves != mp.curves.curves || live.keys != mp.curves.keys))) {
            error = "the bone scale curves changed since Prepare";
            return false;
        }
    }
    if (mp.hasLayers && !morph_curve::LayersUnchanged(mp.model, mp.layers)) {
        error = "the authored expression curves changed since Prepare";
        return false;
    }
    for (const auto& payload : mp.displayPayloads) {
        if ((!payload.originalIk.empty() && std::memcmp(payload.ik, payload.originalIk.data(), payload.ikBytes)) ||
            (!payload.originalSelectors.empty() && std::memcmp(payload.selectors, payload.originalSelectors.data(), payload.selectorBytes))) {
            error = "a display/IK payload changed since Prepare";
            return false;
        }
    }
    return true;
}

bool VerifyApp(MMDApp& app, const AppPatch& ap, std::string& error) {
    if (ap.app != &app) {
        error = "the plan belongs to a different application";
        return false;
    }
    for (int slot = 0; slot < kModelSlotCount; ++slot) if (app.ModelSlot(slot) != ap.modelIdentities[slot]) {
        error = "the loaded model set changed since Prepare"; return false;
    }
    for (int slot = 0; slot < static_cast<int>(kAccessorySlots); ++slot)
        if (app.AccessorySlot(slot) != ap.accessoryIdentities[slot] || app.AccessoryKeys(slot) != ap.accessoryPools[slot]) {
            error = "the loaded accessory set changed since Prepare"; return false;
        }
    if (ap.camera.pool != reinterpret_cast<std::uint8_t*>(app.CameraKeys()) ||
        ap.light.pool != reinterpret_cast<std::uint8_t*>(app.LightKeys()) ||
        ap.shadow.pool != reinterpret_cast<std::uint8_t*>(app.ShadowKeys()) ||
        ap.gravity.pool != reinterpret_cast<std::uint8_t*>(app.GravityKeys())) {
        error = "a global key table changed since Prepare";
        return false;
    }
    const PoolPatch* pools[4] = {&ap.camera, &ap.light, &ap.shadow, &ap.gravity};
    const char* names[4] = {"camera", "light", "shadow", "gravity"};
    for (int index = 0; index < 4; ++index) {
        if (pools[index]->pool == nullptr) continue;
        for (const SlotPatch& patch : pools[index]->chains) {
            if (!VerifyChain(*pools[index], patch, names[index], error)) return false;
        }
    }
    for (std::size_t index = 0; index < ap.accessorySlots.size(); ++index) {
        const int slot = ap.accessorySlots[index];
        const PoolPatch& pool = ap.accessories[index];
        if (app.AccessorySlot(slot) == nullptr ||
            pool.pool != reinterpret_cast<std::uint8_t*>(app.AccessoryKeys(slot))) {
            error = "an accessory table changed since Prepare";
            return false;
        }
        for (const SlotPatch& patch : pool.chains) {
            if (!VerifyChain(pool, patch, "accessory", error)) return false;
        }
    }
    if (ap.hasAuthoring && !curve_host::AuthoringUnchanged(app, ap.authoring)) {
        error = "the curve editor's authoring changed since Prepare";
        return false;
    }
    return true;
}

void ApplyModel(const ModelPatch& mp) {
    for (const SlotPatch& patch : mp.bone.chains) ApplyPatch(mp.bone.pool, mp.bone.stride, patch);
    for (const SlotPatch& patch : mp.morph.chains) ApplyPatch(mp.morph.pool, mp.morph.stride, patch);
    for (const SlotPatch& patch : mp.display.chains) ApplyPatch(mp.display.pool, mp.display.stride, patch);
}

void RevertModel(const ModelPatch& mp) {
    for (const SlotPatch& patch : mp.display.chains) RevertPatch(mp.display.pool, mp.display.stride, patch);
    for (const SlotPatch& patch : mp.morph.chains) RevertPatch(mp.morph.pool, mp.morph.stride, patch);
    for (const SlotPatch& patch : mp.bone.chains) RevertPatch(mp.bone.pool, mp.bone.stride, patch);
}

void ApplyApp(const AppPatch& ap) {
    for (const SlotPatch& patch : ap.camera.chains) ApplyPatch(ap.camera.pool, ap.camera.stride, patch);
    for (const SlotPatch& patch : ap.light.chains) ApplyPatch(ap.light.pool, ap.light.stride, patch);
    for (const SlotPatch& patch : ap.shadow.chains) ApplyPatch(ap.shadow.pool, ap.shadow.stride, patch);
    for (const SlotPatch& patch : ap.gravity.chains) ApplyPatch(ap.gravity.pool, ap.gravity.stride, patch);
    for (const PoolPatch& pool : ap.accessories) {
        for (const SlotPatch& patch : pool.chains) ApplyPatch(pool.pool, pool.stride, patch);
    }
}

void RevertApp(const AppPatch& ap) {
    for (const PoolPatch& pool : ap.accessories) {
        for (const SlotPatch& patch : pool.chains) RevertPatch(pool.pool, pool.stride, patch);
    }
    for (const SlotPatch& patch : ap.gravity.chains) RevertPatch(ap.gravity.pool, ap.gravity.stride, patch);
    for (const SlotPatch& patch : ap.shadow.chains) RevertPatch(ap.shadow.pool, ap.shadow.stride, patch);
    for (const SlotPatch& patch : ap.light.chains) RevertPatch(ap.light.pool, ap.light.stride, patch);
    for (const SlotPatch& patch : ap.camera.chains) RevertPatch(ap.camera.pool, ap.camera.stride, patch);
}

// Recomputes the bookkeeping the registrars keep: the model timeline bound and
// the shared free-slot cursor.
void RefreshModelBookkeeping(MMDApp& app, const ModelPatch& mp) {
    mdl::ModelRecord& record = *mdl::Mdl(mp.model);
    std::uint32_t maximum = mp.maxFrame;
    for (const SlotPatch& patch : mp.bone.chains) {
        for (std::uint32_t slot : patch.writeSlots) {
            maximum = std::max(maximum, ReadU32(mp.bone.pool + static_cast<std::size_t>(slot) * mp.bone.stride, 0));
        }
    }
    for (const SlotPatch& patch : mp.morph.chains) {
        for (std::uint32_t slot : patch.writeSlots) {
            maximum = std::max(maximum, ReadU32(mp.morph.pool + static_cast<std::size_t>(slot) * mp.morph.stride, 0));
        }
    }
    for (const SlotPatch& patch : mp.display.chains) {
        for (std::uint32_t slot : patch.writeSlots) {
            maximum = std::max(maximum, ReadU32(mp.display.pool + static_cast<std::size_t>(slot) * mp.display.stride, 0));
        }
    }
    record.maxFrame = maximum;
    // The same rule ResetBoneKeyCursor uses: the first free continuation slot.
    int cursor = static_cast<int>(mp.boneCount);
    const std::size_t capacity = mp.bone.capacity;
    while (cursor < static_cast<int>(capacity) &&
           ReadU32(mp.bone.pool + static_cast<std::size_t>(cursor) * mp.bone.stride, 0) != 0) {
        ++cursor;
    }
    record.searchCursor = cursor;
    app.LastRegisteredFrame() = std::max(app.LastRegisteredFrame(), maximum);
    (void)app;
}

// One commit's undo record.
//
// It is created fresh for every Commit call, notes each installation in the
// order it happens, and undoes them in reverse order in its destructor unless
// the commit was released.  Two properties matter here:
//
//   * Everything it does on the way back is a byte copy or a swap of an object
//     that already exists, so it cannot allocate and cannot fail.
//   * The record lives in the guard, not in the plan.  A plan therefore cannot
//     carry a stale "installed" flag from a failed attempt into a retry, which
//     is what would make a second rollback swap the wrong table.
//
// Since the guard is a local of the commit body, an exception anywhere in the
// body - an allocation failure included - unwinds through this destructor
// before the caller's catch block builds its message, so the project is always
// restored before Commit returns false.
class CommitGuard {
public:
    CommitGuard(MMDApp& app, const Plan& plan) : app_(&app), plan_(&plan) {
        curves_.reserve(plan.models.size());
        layers_.reserve(plan.models.size());
    }
    CommitGuard(const CommitGuard&) = delete;
    CommitGuard& operator=(const CommitGuard&) = delete;

    void ArmPools() { pools_ = true; }
    void NoteCurves(std::size_t model) { curves_.push_back(model); }
    void NoteLayers(std::size_t model) { layers_.push_back(model); }
    void NoteAuthoring() { authoring_ = true; }
    void Release() { released_ = true; }

    ~CommitGuard() {
        if (released_) return;
        if (authoring_) {
            curve_host::SwapAuthoring(*app_, plan_->app.retimedAuthoringTable);
        }
        for (auto entry = layers_.rbegin(); entry != layers_.rend(); ++entry) {
            morph_curve::SwapLayers(plan_->models[*entry].model,
                                    plan_->models[*entry].retimedLayers);
        }
        for (auto entry = curves_.rbegin(); entry != curves_.rend(); ++entry) {
            bone_scale::SwapCurveTable(plan_->models[*entry].model,
                                       plan_->models[*entry].retimedCurves);
        }
        if (!pools_) return;
        for (const ModelPatch& mp : plan_->models) RevertModel(mp);
        RevertApp(plan_->app);
        for (const ModelPatch& mp : plan_->models) {
            mdl::Mdl(mp.model)->maxFrame = mp.oldMaxFrame;
            mdl::Mdl(mp.model)->searchCursor = mp.searchCursor;
        }
        app_->CameraTrackCursor() = plan_->app.cameraCursor;
        app_->LightTrackCursor() = plan_->app.lightCursor;
        app_->ShadowTrackCursor() = plan_->app.shadowCursor;
        app_->GravityTrackCursor() = plan_->app.gravityCursor;
        for (std::size_t index = 0; index < plan_->app.accessorySlots.size(); ++index) {
            app_->AccessoryTrackCursor(plan_->app.accessorySlots[index]) =
                plan_->app.accessoryCursors[index];
        }
        app_->LastRegisteredFrame() = plan_->app.oldLastRegistered;
        app_->SceneModified() = plan_->app.sceneModified;
    }

private:
    MMDApp* app_ = nullptr;
    const Plan* plan_ = nullptr;
    bool pools_ = false;
    bool authoring_ = false;
    bool released_ = false;
    // Allocation here happens before anything is installed for that entry, and
    // a throw while pushing is exactly the case the destructor covers.
    std::vector<std::size_t> curves_;
    std::vector<std::size_t> layers_;
};

bool CommitBody(MMDApp& app, const PlanPtr& plan, std::wstring& error,
                const std::function<bool(std::wstring&)>& beforeFinalize) {
    error.clear();
    if (!plan) {
        error = Localized(app, L"変換計画がありません。", L"There is no conversion plan.",
                          L"没有转换计划。");
        return false;
    }
    if (plan->committed) {
        error = Localized(app, L"この変換計画は既に適用されています。",
                          L"This conversion plan has already been applied.",
                          L"这个转换计划已经应用过了。");
        return false;
    }
    std::string detail;
    if (!VerifyApp(app, plan->app, detail)) {
        error = Localized(app, L"変換前にプロジェクトが変更されました。",
                          L"The project changed after the plan was prepared.",
                          L"准备计划后项目已被修改。") + L" (" + Wide(detail) + L")";
        return false;
    }
    for (const ModelPatch& mp : plan->models) {
        if (!VerifyModel(app, mp, detail)) {
            error = Localized(app, L"変換前にプロジェクトが変更されました。",
                              L"The project changed after the plan was prepared.",
                              L"准备计划后项目已被修改。") + L" (" + Wide(detail) + L")";
            return false;
        }
    }

    // Allocate the undo bookkeeping before touching any live bytes.
    CommitGuard guard(app, *plan);
    guard.ArmPools();
    // ---- writes, none of which can fail -----------------------------------
    ApplyApp(plan->app);
    for (const ModelPatch& mp : plan->models) ApplyModel(mp);

    // ---- the side tables: one swap each, recorded by the guard ------------
    // The pools were written just above, so the guard is armed immediately and
    // owns undoing them from here on.  Prepare built every candidate, so these
    // are exchanges of whole objects; the authoring table is the one call that
    // still allocates, and it sits inside the guarded region, so a failure
    // unwinds through the destructor instead of leaving a half-converted
    // project behind.
    for (std::size_t index = 0; index < plan->models.size(); ++index) {
        const ModelPatch& mp = plan->models[index];
        if (mp.hasCurves && mp.curvesMoved) {
            bone_scale::SwapCurveTable(mp.model, mp.retimedCurves);
            guard.NoteCurves(index);
        }
        if (mp.hasLayers && mp.layersMoved) {
            morph_curve::SwapLayers(mp.model, mp.retimedLayers);
            guard.NoteLayers(index);
        }
    }
    if (plan->app.hasAuthoring && plan->app.authoringMoved) {
        if (!curve_host::RebuildAuthoring(app, plan->app.retimedAuthoring,
                                          plan->app.retimedAuthoringTable, error)) {
            if (error.empty()) {
                error = Localized(app, L"カーブ エディタの編集内容を再構築できません。",
                                  L"The curve editor's authoring cannot be rebuilt.",
                                  L"无法重建曲线编辑器的编辑内容。");
            }
            return false;
        }
        curve_host::SwapAuthoring(app, plan->app.retimedAuthoringTable);
        guard.NoteAuthoring();
    }

    // ---- the caller's final say -------------------------------------------
    if (beforeFinalize) {
        std::wstring reason;
        bool accept = false;
        try {
            accept = beforeFinalize(reason);
        } catch (...) {
            accept = false;
        }
        if (!accept) {
            error = reason.empty()
                        ? Localized(app, L"変換は取り消されました。", L"The conversion was cancelled.",
                                    L"转换已取消。")
                        : reason;
            // The guard now restores, in reverse order: the side tables by
            // swapping the candidates back, then every touched pool byte, then
            // maxFrame, the search cursor, the track cursors, the last
            // registered frame and the scene flag.  None of that allocates.
            // Nothing about the plan records that an attempt happened, so the
            // same plan can be committed again.
            return false;
        }
    }

    // ---- finalize: noexcept from here on ----------------------------------
    guard.Release();
    for (const ModelPatch& mp : plan->models) {
        for (const SlotPatch& patch : mp.display.chains) ReleaseClearedPayloads(patch, mp.display);
    }
    for (const ModelPatch& mp : plan->models) RefreshModelBookkeeping(app, mp);
    app.SceneModified() = 1;
    plan->committed = true;
    return true;
}

// The public entry point.  Every failure, allocation included, becomes a
// message here; the guard inside the body has already restored the project by
// the time the message is built.
bool Commit(MMDApp& app, const PlanPtr& plan, std::wstring& error,
            const std::function<bool(std::wstring&)>& beforeFinalize) {
    error.clear();
    if (!plan) {
        error = Localized(app, L"変換計画がありません。", L"There is no conversion plan.",
                          L"没有转换计划。");
        return false;
    }
    if (plan->committed) {
        error = Localized(app, L"この変換計画は既に適用されています。",
                          L"This conversion plan has already been applied.",
                          L"这个转换计划已经应用过了。");
        return false;
    }
    try {
        return CommitBody(app, plan, error, beforeFinalize);
    } catch (const std::bad_alloc&) {
        error = Localized(app, L"メモリが不足しているため変換を適用できません。",
                          L"There is not enough memory to apply the conversion.",
                          L"内存不足，无法应用转换。");
        return false;
    } catch (const std::exception& failure) {
        error = Localized(app, L"変換の適用中にエラーが発生しました。",
                          L"The conversion could not be applied.",
                          L"应用转换时发生错误。") +
                L" (" + Wide(failure.what()) + L")";
        return false;
    }
}

}  // namespace mikumikustudio::native_frame_retime
