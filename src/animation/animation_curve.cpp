#include <cstdint>
// MAINTENANCE: pure-CPU curve core shared by the host attachment/expression
// pipeline and the MMV parameter pipeline.  Keep it free of Win32, DirectX and
// GUI dependencies so both callers can link it unchanged.
//
// Curve model
// -----------
// * Keys are stored in increasing frame order; every key's frame is a whole,
//   non-negative frame number.
// * Key::interpolation describes the interval that ENDS at the key, so the
//   first key's interpolation value is unused.  The pair (key i, key i+1) is
//   therefore interpolated with keys[i + 1].interpolation.
// * Key::inSlope / outSlope are values per frame: the left / right derivative
//   of the curve at the key.
// * Key::inWeight / outWeight are the Bezier handle lengths as a fraction of
//   the interval, in (0, 1).  On the interval [t0, t1] with span dt the first
//   handle sits at (t0 + outWeight * dt, v0 + outSlope * outWeight * dt) and
//   the second at (t1 - inWeight * dt, v1 - inSlope * inWeight * dt).  A handle
//   whose slope equals the secant slope lies on the chord, so the segment is
//   exactly straight even though it is still evaluated as a cubic.
// * Handle weights are limited to [0.0001, 0.9999] and the neighbouring pair
//   outWeight(i) + inWeight(i+1) must stay at or below 0.9999.  That keeps the
//   two control abscissae strictly inside the interval, which makes the cubic
//   strictly increasing in time and therefore invertible.
// * Time is recovered by inverting the cubic; it is never assumed linear.

#include "mikumikustudio/animation_curve.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace mikumikustudio::animation_curve {
namespace {

constexpr double kMinimumWeight = 0.0001;
constexpr double kMaximumWeight = 0.9999;
// Adjacent out/in weights must leave a gap: at a sum of exactly 1 the two
// handles would share an abscissa and the time inversion would be ambiguous.
constexpr double kMaximumWeightSum = 0.9999;
constexpr std::size_t kMaximumIdBytes = 1024;
constexpr std::size_t kMaximumEncodedBytes = (std::min)(std::size_t(UINT32_MAX), std::size_t(PTRDIFF_MAX));

// On-disk size of one key record in format version 1.
constexpr std::size_t kKeyRecordBytes = 4 + 5 * 8 + 4 + 4;
// Header plus the smallest possible track prefix.
constexpr std::size_t kHeaderBytes = 4 + 2 + 2 + 4;
constexpr std::size_t kMinimumTrackBytes = 4 + 1 + 1 + 4;

bool IsValidInterpolation(Interpolation value) {
    switch (value) {
    case Interpolation::Step:
    case Interpolation::Linear:
    case Interpolation::Bezier:
        return true;
    }
    return false;
}

bool IsValidTangent(Tangent value) {
    switch (value) {
    case Tangent::Free:
    case Tangent::Aligned:
    case Tangent::Flat:
    case Tangent::Linear:
    case Tangent::Automatic:
        return true;
    }
    return false;
}

std::string KeyLabel(std::size_t index) { return "key " + std::to_string(index) + ": "; }

double FrameSpan(const Key &start, const Key &end) {
    return static_cast<double>(end.frame) - static_cast<double>(start.frame);
}

double SecantSlope(const Key &start, const Key &end) {
    return (end.value - start.value) / FrameSpan(start, end);
}

// Root of the normalised-time cubic with control abscissae 0, x1, x2, 1.
// The time polynomial must be monotone. Scalar handles are ordered, while
// native MMD handles and their subdivisions may cross. Newton steps fall back to
// bisection whenever they would leave the bracket.
double SolveBezierTime(double x1, double x2, double u) {
    const double cubic = 1.0 - 3.0 * x2 + 3.0 * x1;
    const double quadratic = 3.0 * x2 - 6.0 * x1;
    const double linear = 3.0 * x1;
    const auto evaluate = [&](double s) { return ((cubic * s + quadratic) * s + linear) * s; };
    const auto derivative = [&](double s) { return (3.0 * cubic * s + 2.0 * quadratic) * s + linear; };
    double low = 0.0;
    double high = 1.0;
    double s = u;
    for (int iteration = 0; iteration < 96; ++iteration) {
        const double residual = evaluate(s) - u;
        if (residual > 0.0)
            high = s;
        else
            low = s;
        if (high - low <= 1e-16) break;
        const double slope = derivative(s);
        double next = slope > 1e-12 ? s - residual / slope : 0.5 * (low + high);
        if (!(next > low && next < high)) next = 0.5 * (low + high);
        s = next;
    }
    return s;
}

// Samples the interval [start, end] at normalised time u, which the caller
// derived from the frame.  Endpoints are answered exactly so that keys always
// evaluate to their stored value.
Sample SampleInterval(bool discrete, const Key &start, const Key &end, double u) {
    const Interpolation interpolation = discrete ? Interpolation::Step : end.interpolation;
    if (interpolation == Interpolation::Step) {
        return u >= 1.0 ? Sample{end.value, 0.0} : Sample{start.value, 0.0};
    }
    const double span = FrameSpan(start, end);
    if (interpolation == Interpolation::Linear) {
        const double slope = SecantSlope(start, end);
        if (u <= 0.0) return Sample{start.value, slope};
        if (u >= 1.0) return Sample{end.value, slope};
        return Sample{start.value + (end.value - start.value) * u, slope};
    }
    if (u <= 0.0) return Sample{start.value, start.outSlope};
    if (u >= 1.0) return Sample{end.value, end.inSlope};
    const double x1 = start.outWeight;
    const double x2 = 1.0 - end.inWeight;
    const double y0 = start.value;
    const double y1 = start.value + start.outSlope * start.outWeight * span;
    const double y2 = end.value - end.inSlope * end.inWeight * span;
    const double y3 = end.value;
    const double s = SolveBezierTime(x1, x2, u);
    const double inv = 1.0 - s;
    const double value = inv * inv * inv * y0 + 3.0 * inv * inv * s * y1 + 3.0 * inv * s * s * y2 +
                         s * s * s * y3;
    const double dx = 3.0 * inv * inv * x1 + 6.0 * inv * s * (x2 - x1) + 3.0 * s * s * (1.0 - x2);
    const double dy = 3.0 * inv * inv * (y1 - y0) + 6.0 * inv * s * (y2 - y1) + 3.0 * s * s * (y3 - y2);
    if (!(std::fabs(dx) > 1e-12)) return Sample{value, 0.0};
    return Sample{value, dy / (span * dx)};
}

// Slope an automatic key gets from its neighbours.  Local extrema and plateaus
// become horizontal, and interior keys use the Fritsch-Carlson weighted
// harmonic mean, which stays inside the monotone region of both neighbouring
// intervals.  Together those rules are what keeps automatic curves from
// overshooting the keys they pass through.
double AutomaticSlope(const Track &track, std::size_t index) {
    const std::vector<Key> &keys = track.keys;
    if (keys.size() < 2) return 0.0;
    const Key &key = keys[index];
    if (index == 0) {
        return FrameSpan(key, keys[1]) > 0.0 ? SecantSlope(key, keys[1]) : 0.0;
    }
    if (index + 1 == keys.size()) {
        return FrameSpan(keys[index - 1], key) > 0.0 ? SecantSlope(keys[index - 1], key) : 0.0;
    }
    const double previousSpan = FrameSpan(keys[index - 1], key);
    const double nextSpan = FrameSpan(key, keys[index + 1]);
    if (!(previousSpan > 0.0) || !(nextSpan > 0.0)) return 0.0;
    const double previousSlope = SecantSlope(keys[index - 1], key);
    const double nextSlope = SecantSlope(key, keys[index + 1]);
    if (previousSlope * nextSlope <= 0.0) return 0.0;
    const double previousWeight = 2.0 * nextSpan + previousSpan;
    const double nextWeight = nextSpan + 2.0 * previousSpan;
    return (previousWeight + nextWeight) / (previousWeight / previousSlope + nextWeight / nextSlope);
}

// A key that carries an explicitly assigned slope cannot stay automatic, or the
// next UpdateAutomatic would silently undo the assignment.
void FreezeIfAutomatic(Key &key) {
    if (key.tangent == Tangent::Automatic) key.tangent = Tangent::Free;
}

bool IsValidUtf8(const std::string &text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t continuation = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead < 0x80) {
            ++index;
            continue;
        }
        if ((lead & 0xE0) == 0xC0) {
            if (lead < 0xC2) return false;  // overlong
            continuation = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            continuation = 2;
            if (lead == 0xE0) low = 0xA0;  // overlong
            if (lead == 0xED) high = 0x9F;  // UTF-16 surrogates
        } else if ((lead & 0xF8) == 0xF0) {
            if (lead > 0xF4) return false;
            continuation = 3;
            if (lead == 0xF0) low = 0x90;  // overlong
            if (lead == 0xF4) high = 0x8F;  // above U+10FFFF
        } else {
            return false;
        }
        if (index + continuation >= text.size()) return false;
        for (std::size_t step = 1; step <= continuation; ++step) {
            const unsigned char byte = static_cast<unsigned char>(text[index + step]);
            if (byte < low || byte > high) return false;
            low = 0x80;
            high = 0xBF;
        }
        index += continuation + 1;
    }
    return true;
}

class Reader {
  public:
    Reader(const std::uint8_t *data, std::size_t size) : data_(data), size_(size) {}

    bool Read(void *destination, std::size_t count) {
        if (count > size_ - offset_) return false;
        std::memcpy(destination, data_ + offset_, count);
        offset_ += count;
        return true;
    }

    bool ReadU8(std::uint8_t &out) { return Read(&out, 1); }

    bool ReadU16(std::uint16_t &out) {
        std::uint8_t bytes[2];
        if (!Read(bytes, 2)) return false;
        out = static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
        return true;
    }

    bool ReadU32(std::uint32_t &out) {
        std::uint8_t bytes[4];
        if (!Read(bytes, 4)) return false;
        out = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
              (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
        return true;
    }

    bool ReadI32(std::int32_t &out) {
        std::uint32_t raw = 0;
        if (!ReadU32(raw)) return false;
        out = static_cast<std::int32_t>(raw);
        return true;
    }

    bool ReadU64(std::uint64_t &out) {
        std::uint8_t bytes[8];
        if (!Read(bytes, 8)) return false;
        out = 0;
        for (int index = 7; index >= 0; --index) out = (out << 8) | bytes[index];
        return true;
    }

    bool ReadF64(double &out) {
        std::uint64_t bits = 0;
        if (!ReadU64(bits)) return false;
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

    bool ReadText(std::size_t length, std::string &out) {
        if (length > size_ - offset_) return false;
        out.assign(reinterpret_cast<const char *>(data_ + offset_), length);
        offset_ += length;
        return true;
    }

    std::size_t Remaining() const { return size_ - offset_; }

  private:
    const std::uint8_t *data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
};

void WriteU8(std::vector<std::uint8_t> &out, std::uint8_t value) { out.push_back(value); }

void WriteU16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void WriteU32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void WriteI32(std::vector<std::uint8_t> &out, std::int32_t value) {
    WriteU32(out, static_cast<std::uint32_t>(value));
}

void WriteF64(std::vector<std::uint8_t> &out, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((bits >> shift) & 0xFFu));
    }
}

}  // namespace

bool Validate(const Track &track, std::string &error) {
    error.clear();
    const std::vector<Key> &keys = track.keys;
    if (keys.size() > kMaximumKeys) {
        error = "the track holds " + std::to_string(keys.size()) + " keys, the limit is " +
                std::to_string(kMaximumKeys);
        return false;
    }
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const Key &key = keys[index];
        if (key.frame < 0) {
            error = KeyLabel(index) + "frame " + std::to_string(key.frame) + " is negative";
            return false;
        }
        if (!std::isfinite(key.value)) {
            error = KeyLabel(index) + "value is not a finite number";
            return false;
        }
        if (!std::isfinite(key.inSlope) || !std::isfinite(key.outSlope)) {
            error = KeyLabel(index) + "slope is not a finite number";
            return false;
        }
        if (!std::isfinite(key.inWeight) || !std::isfinite(key.outWeight)) {
            error = KeyLabel(index) + "handle weight is not a finite number";
            return false;
        }
        if (key.inWeight < kMinimumWeight || key.inWeight > kMaximumWeight) {
            error = KeyLabel(index) + "inWeight " + std::to_string(key.inWeight) +
                    " is outside [0.0001, 0.9999]";
            return false;
        }
        if (key.outWeight < kMinimumWeight || key.outWeight > kMaximumWeight) {
            error = KeyLabel(index) + "outWeight " + std::to_string(key.outWeight) +
                    " is outside [0.0001, 0.9999]";
            return false;
        }
        if (!IsValidInterpolation(key.interpolation)) {
            error = KeyLabel(index) + "interpolation value " +
                    std::to_string(static_cast<std::uint32_t>(key.interpolation)) +
                    " is not a known interpolation";
            return false;
        }
        if (!IsValidTangent(key.tangent)) {
            error = KeyLabel(index) + "tangent value " +
                    std::to_string(static_cast<std::uint32_t>(key.tangent)) +
                    " is not a known tangent mode";
            return false;
        }
        if (index > 0) {
            const Key &previous = keys[index - 1];
            if (previous.frame >= key.frame) {
                error = "frame " + std::to_string(key.frame) + " at key " + std::to_string(index) +
                        " does not follow frame " + std::to_string(previous.frame) +
                        " at key " + std::to_string(index - 1) +
                        "; key frames must increase strictly";
                return false;
            }
            if (previous.outWeight + key.inWeight > kMaximumWeightSum) {
                error = KeyLabel(index) + "outWeight " + std::to_string(previous.outWeight) +
                        " of the previous key plus inWeight " + std::to_string(key.inWeight) +
                        " exceeds 0.9999, which would make the interval ambiguous in time";
                return false;
            }
        }
    }
    return true;
}

Sample EvaluateMmdTiming(const std::array<double,4>& controls, double time) {
    const double x=std::clamp(time,0.0,1.0);
    if(controls[0]==controls[1] && controls[2]==controls[3])return {x,1};
    const double x1=controls[0]/127.0,y1=controls[1]/127.0;
    const double x2=controls[2]/127.0,y2=controls[3]/127.0;
    const double t=x<=0?0:x>=1?1:SolveBezierTime(x1,x2,x);
    const double v=1-t;
    const double value=v*v*3*t*y1+v*3*t*t*y2+t*t*t;
    const auto slope=[&](double u){
        const double a=1-u;
        const double dx=a*a*x1+2*a*u*(x2-x1)+u*u*(1-x2);
        const double dy=a*a*y1+2*a*u*(y2-y1)+u*u*(1-y2);
        return std::abs(dx)>1e-15?dy/dx:0.0;
    };
    // A vertical endpoint handle has an unbounded mathematical slope. Use
    // the one-sided display limit rather than producing non-finite pixels.
    return {value,slope(std::clamp(t,1e-7,1-1e-7))};
}

Sample Evaluate(const Track &track, double frame) {
    const std::vector<Key> &keys = track.keys;
    if (keys.empty()) return Sample{};
    if (!std::isfinite(frame)) return Sample{keys.front().value, 0.0};
    if (frame < static_cast<double>(keys.front().frame)) return Sample{keys.front().value, 0.0};
    if (frame > static_cast<double>(keys.back().frame)) return Sample{keys.back().value, 0.0};
    if (keys.size() == 1) return Sample{keys.front().value, 0.0};
    const auto bound = std::upper_bound(
        keys.begin(), keys.end(), frame,
        [](double value, const Key &key) { return value < static_cast<double>(key.frame); });
    std::size_t index = static_cast<std::size_t>(bound - keys.begin());
    if (index == keys.size()) index = keys.size() - 1;  // frame sits on the last key
    if (index == 0) index = 1;                          // defensive: frame sits on the first key
    const Key &start = keys[index - 1];
    const Key &end = keys[index];
    const double span = FrameSpan(start, end);
    if (!(span > 0.0)) return Sample{end.value, 0.0};
    const double u = (frame - static_cast<double>(start.frame)) / span;
    return SampleInterval(track.discrete, start, end, u);
}

void UpdateAutomatic(Track &track) {
    std::vector<Key> &keys = track.keys;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        Key &key = keys[index];
        if (key.tangent != Tangent::Automatic) continue;
        const double slope = AutomaticSlope(track, index);
        key.inSlope = slope;
        key.outSlope = slope;
    }
}

bool SetTangent(Track &track, std::size_t key, Tangent tangent, std::string &error) {
    error.clear();
    if (!IsValidTangent(tangent)) {
        error = "tangent value " + std::to_string(static_cast<std::uint32_t>(tangent)) +
                " is not a known tangent mode";
        return false;
    }
    if (key >= track.keys.size()) {
        error = "key index " + std::to_string(key) + " is outside a track with " +
                std::to_string(track.keys.size()) + " keys";
        return false;
    }
    if (!Validate(track, error)) return false;
    std::vector<Key> &keys = track.keys;
    if (keys[key].tangent == Tangent::Automatic && tangent != Tangent::Automatic) {
        // Leaving the automatic mode freezes the slope that mode currently
        // produces instead of keeping whatever value was stored last.
        const double frozen = AutomaticSlope(track, key);
        keys[key].inSlope = frozen;
        keys[key].outSlope = frozen;
    }
    switch (tangent) {
    case Tangent::Free:
        break;
    case Tangent::Aligned: {
        const double aligned = 0.5 * (keys[key].inSlope + keys[key].outSlope);
        keys[key].inSlope = aligned;
        keys[key].outSlope = aligned;
        break;
    }
    case Tangent::Flat:
        keys[key].inSlope = 0.0;
        keys[key].outSlope = 0.0;
        break;
    case Tangent::Linear: {
        // Both neighbouring intervals only become exactly straight when the
        // facing slope of the neighbour key is the same secant slope.
        if (key + 1 < keys.size()) {
            const double slope = SecantSlope(keys[key], keys[key + 1]);
            keys[key].outSlope = slope;
            keys[key + 1].inSlope = slope;
            FreezeIfAutomatic(keys[key + 1]);
        }
        if (key > 0) {
            const double slope = SecantSlope(keys[key - 1], keys[key]);
            keys[key].inSlope = slope;
            keys[key - 1].outSlope = slope;
            FreezeIfAutomatic(keys[key - 1]);
        }
        break;
    }
    case Tangent::Automatic:
        keys[key].tangent = Tangent::Automatic;
        UpdateAutomatic(track);
        return true;
    }
    keys[key].tangent = tangent;
    return true;
}

// ---- per-side Bezier editing ------------------------------------------------
namespace {

bool NearlyEqual(double a, double b) {
    return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

// The key's effective slope on one side: an automatic key follows its
// neighbours, whatever was stored last.
double SideSlope(const Track &track, std::size_t key, bool outSide) {
    const Key &k = track.keys[key];
    if (k.tangent == Tangent::Automatic) return AutomaticSlope(track, key);
    return outSide ? k.outSlope : k.inSlope;
}

// A side edit makes the key's two slopes independent: freeze the automatic
// slope and leave the coupled modes for Free.
void Decouple(Track &track, std::size_t key) {
    Key &k = track.keys[key];
    if (k.tangent == Tangent::Automatic) {
        const double frozen = AutomaticSlope(track, key);
        k.inSlope = frozen;
        k.outSlope = frozen;
    }
    k.tangent = Tangent::Free;
}

// Interval index (its end key) of a side, or 0 when the side does not exist.
std::size_t SideInterval(const Track &track, std::size_t key, bool outSide) {
    if (key >= track.keys.size()) return 0;
    if (outSide) return key + 1 < track.keys.size() ? key + 1 : 0;
    return key;  // key 0 has no in side
}

}  // namespace

bool SideIsStraight(const Track &track, std::size_t key, bool outSide) {
    const std::size_t end = SideInterval(track, key, outSide);
    if (end == 0 || track.discrete) return true;
    const Key &start = track.keys[end - 1];
    const Key &stop = track.keys[end];
    if (stop.interpolation != Interpolation::Bezier) return true;
    if (!(FrameSpan(start, stop) > 0.0)) return true;
    return NearlyEqual(SideSlope(track, key, outSide), SecantSlope(start, stop));
}

bool KeyIsLinear(const Track &track, std::size_t key) {
    return SideIsStraight(track, key, false) && SideIsStraight(track, key, true);
}

void StraightenSide(Track &track, std::size_t key, bool outSide) {
    const std::size_t end = SideInterval(track, key, outSide);
    if (end == 0 || track.discrete) return;
    if (track.keys[end].interpolation != Interpolation::Bezier) return;
    if (!(FrameSpan(track.keys[end - 1], track.keys[end]) > 0.0)) return;
    Decouple(track, key);
    const double secant = SecantSlope(track.keys[end - 1], track.keys[end]);
    (outSide ? track.keys[key].outSlope : track.keys[key].inSlope) = secant;
    const std::size_t neighbour = outSide ? key + 1 : key - 1;
    if (SideIsStraight(track, neighbour, !outSide)) {
        track.keys[end].interpolation = Interpolation::Linear;
        // Store chord slopes on both ends so a later switch back to Bezier
        // starts from the same straight segment.
        Decouple(track, neighbour);
        (outSide ? track.keys[neighbour].inSlope : track.keys[neighbour].outSlope) = secant;
    }
}

void MakeKeyLinear(Track &track, std::size_t key) {
    StraightenSide(track, key, false);
    StraightenSide(track, key, true);
}

void MakeKeyBezier(Track &track, std::size_t key) {
    if (key >= track.keys.size() || track.discrete) return;
    double slope = AutomaticSlope(track, key);
    // Neighbours on one line give the chord slope, which would leave the key
    // looking linear; a flat slope gives the visible two-sided ease instead.
    bool onChord = true;
    for (bool outSide : {false, true}) {
        const std::size_t end = SideInterval(track, key, outSide);
        if (end == 0 || track.keys[end].interpolation == Interpolation::Step) continue;
        if (!(FrameSpan(track.keys[end - 1], track.keys[end]) > 0.0)) continue;
        if (!NearlyEqual(slope, SecantSlope(track.keys[end - 1], track.keys[end]))) onChord = false;
    }
    if (onChord) slope = 0.0;
    Decouple(track, key);
    for (bool outSide : {false, true}) {
        const std::size_t end = SideInterval(track, key, outSide);
        if (end == 0 || track.keys[end].interpolation == Interpolation::Step) continue;
        if (!(FrameSpan(track.keys[end - 1], track.keys[end]) > 0.0)) continue;
        if (track.keys[end].interpolation == Interpolation::Linear) {
            track.keys[end].interpolation = Interpolation::Bezier;
            const std::size_t neighbour = outSide ? key + 1 : key - 1;
            const double secant = SecantSlope(track.keys[end - 1], track.keys[end]);
            Decouple(track, neighbour);
            (outSide ? track.keys[neighbour].inSlope : track.keys[neighbour].outSlope) = secant;
        }
        (outSide ? track.keys[key].outSlope : track.keys[key].inSlope) = slope;
    }
}

void ApplyEase(Track &track, std::size_t key, bool easeIn) {
    if (key >= track.keys.size() || track.discrete) return;
    const bool curvedSide = !easeIn;  // false = in side
    const std::size_t end = SideInterval(track, key, curvedSide);
    if (end != 0 && track.keys[end].interpolation != Interpolation::Step &&
        FrameSpan(track.keys[end - 1], track.keys[end]) > 0.0) {
        Decouple(track, key);
        if (track.keys[end].interpolation == Interpolation::Linear) {
            track.keys[end].interpolation = Interpolation::Bezier;
            // The neighbour's facing handle stays on the chord.
            const std::size_t neighbour = curvedSide ? key + 1 : key - 1;
            Decouple(track, neighbour);
            const double secant = SecantSlope(track.keys[end - 1], track.keys[end]);
            (curvedSide ? track.keys[neighbour].inSlope : track.keys[neighbour].outSlope) = secant;
        }
        (curvedSide ? track.keys[key].outSlope : track.keys[key].inSlope) = 0.0;
    }
    StraightenSide(track, key, !curvedSide);
}

bool MoveHandle(Track &track, std::size_t key, bool outSide, double frame, double value) {
    const std::size_t end = SideInterval(track, key, outSide);
    if (end == 0 || track.discrete) return false;
    if (track.keys[end].interpolation != Interpolation::Bezier) return false;
    const double span = FrameSpan(track.keys[end - 1], track.keys[end]);
    if (!(span > 0.0) || !std::isfinite(frame) || !std::isfinite(value)) return false;
    Key &k = track.keys[key];
    const bool aligned = k.tangent == Tangent::Aligned;
    Decouple(track, key);
    // The facing handle of the neighbour limits how long this one may get.
    const double facing = outSide ? track.keys[key + 1].inWeight : track.keys[key - 1].outWeight;
    const double maxWeight = std::max(kMinimumWeight, std::min(kMaximumWeight, kMaximumWeightSum - facing));
    const double reach = outSide ? frame - k.frame : k.frame - frame;
    const double weight = std::clamp(reach / span, kMinimumWeight, maxWeight);
    const double dt = weight * span;
    const double slope = outSide ? (value - k.value) / dt : (k.value - value) / dt;
    if (!std::isfinite(slope)) return false;
    if (outSide) { k.outWeight = weight; k.outSlope = slope; }
    else { k.inWeight = weight; k.inSlope = slope; }
    if (aligned) {
        (outSide ? k.inSlope : k.outSlope) = slope;
        k.tangent = Tangent::Aligned;
    }
    return true;
}

// MMD stores the four Bezier controls as bytes, so this bridge works in the
// original 0..127 domain and quantises to that grid.  It refuses everything the
// grid cannot express instead of truncating: step intervals, equal-valued
// endpoints (the value axis is normalised by the key difference), value handles
// outside [0, 1] (overshoot) and handles that would leave the supported weight
// range after rounding.
bool GetMmdControls(const Track &track, std::size_t endKey, std::array<double, 4> &controls) {
    if (track.discrete) return false;
    if (endKey == 0 || endKey >= track.keys.size()) return false;
    const Key &start = track.keys[endKey - 1];
    const Key &end = track.keys[endKey];
    if (!IsValidInterpolation(end.interpolation)) return false;
    if (end.interpolation == Interpolation::Step) return false;
    const double span = FrameSpan(start, end);
    if (!(span > 0.0)) return false;
    const double delta = end.value - start.value;
    if (delta == 0.0) return false;
    const double x1 = start.outWeight;
    const double x2 = 1.0 - end.inWeight;
    if (!(x1 > 0.0) || !(x1 < x2) || !(x2 < 1.0)) return false;
    const double normalized[4] = {
        x1, start.outWeight * start.outSlope * span / delta, x2,
        1.0 - end.inWeight * end.inSlope * span / delta};
    std::array<double, 4> quantised{};
    for (std::size_t index = 0; index < quantised.size(); ++index) {
        const double value = normalized[index];
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) return false;
        quantised[index] = std::floor(value * 127.0 + 0.5);
    }
    if (quantised[0] < 1.0 || quantised[0] > 126.0) return false;
    if (quantised[2] < 1.0 || quantised[2] > 126.0) return false;
    if (!(quantised[0] < quantised[2])) return false;
    controls = quantised;
    return true;
}

bool SetMmdControls(Track &track, std::size_t endKey, const std::array<double, 4> &controls,
                    std::string &error) {
    error.clear();
    if (track.discrete) {
        error = "the track is marked discrete; a discrete track has no Bezier handles";
        return false;
    }
    if (endKey == 0 || endKey >= track.keys.size()) {
        error = "key index " + std::to_string(endKey) + " has no interval ending at it (the track holds " +
                std::to_string(track.keys.size()) + " keys)";
        return false;
    }
    for (std::size_t index = 0; index < controls.size(); ++index) {
        if (!std::isfinite(controls[index]) || controls[index] < 0.0 || controls[index] > 127.0) {
            error = "MMD control " + std::to_string(index) + " must be a finite value in [0, 127]";
            return false;
        }
    }
    const double x1 = controls[0];
    const double x2 = controls[2];
    if (!(x1 < x2)) {
        error = "MMD controls need x1 < x2 so the interval stays invertible in time (got x1 = " +
                std::to_string(x1) + ", x2 = " + std::to_string(x2) + ")";
        return false;
    }
    const double outWeight = x1 / 127.0;
    const double inWeight = 1.0 - x2 / 127.0;
    if (outWeight < kMinimumWeight || outWeight > kMaximumWeight || inWeight < kMinimumWeight ||
        inWeight > kMaximumWeight) {
        error = "MMD controls map to handle weights outside [0.0001, 0.9999]";
        return false;
    }
    if (outWeight + inWeight > kMaximumWeightSum) {
        error = "MMD controls map to handle weights that sum above 0.9999";
        return false;
    }
    if (!Validate(track, error)) return false;
    Key &start = track.keys[endKey - 1];
    Key &end = track.keys[endKey];
    const double span = FrameSpan(start, end);
    if (!(span > 0.0)) {
        error = "the interval ending at key " + std::to_string(endKey) + " has no duration";
        return false;
    }
    const double delta = end.value - start.value;
    if (delta == 0.0) {
        error = "the interval ending at key " + std::to_string(endKey) +
                " has equal-valued endpoints; MMD normalises the value axis by the difference";
        return false;
    }
    const double outSlope = (controls[1] / 127.0) * delta / (span * outWeight);
    const double inSlope = (1.0 - controls[3] / 127.0) * delta / (span * inWeight);
    if (!std::isfinite(outSlope) || !std::isfinite(inSlope)) {
        error = "MMD controls produce a non-finite slope";
        return false;
    }
    start.outWeight = outWeight;
    start.outSlope = outSlope;
    end.inWeight = inWeight;
    end.inSlope = inSlope;
    // Controls that sit on the diagonal describe a straight segment; anything
    // else is a genuine cubic.
    end.interpolation = (controls[0] == controls[1] && controls[2] == controls[3])
                            ? Interpolation::Linear
                            : Interpolation::Bezier;
    FreezeIfAutomatic(start);
    FreezeIfAutomatic(end);
    return true;
}

bool Bake(const Track &track, std::int32_t first, std::int32_t last, std::int32_t step,
          Track &output, std::string &error) {
    error.clear();
    if (!Validate(track, error)) return false;
    if (track.keys.empty()) {
        error = "cannot bake a track without keys";
        return false;
    }
    if (step < 1) {
        error = "the bake step must be at least one frame, got " + std::to_string(step);
        return false;
    }
    if (last < first) {
        error = "the bake range is empty: first frame " + std::to_string(first) +
                " is after last frame " + std::to_string(last);
        return false;
    }
    if (first < 0) {
        error = "the bake range must start at frame 0 or later, got " + std::to_string(first);
        return false;
    }
    const std::int32_t firstKey = track.keys.front().frame;
    const std::int32_t lastKey = track.keys.back().frame;
    if (first > firstKey || last < lastKey) {
        // Rewriting only part of a track would have to cut the curve at the
        // range edges and keep the outside exact; this version refuses that
        // rather than quietly changing the animation outside the range.
        error = "the bake range " + std::to_string(first) + ".." + std::to_string(last) +
                " must cover the whole track " + std::to_string(firstKey) + ".." +
                std::to_string(lastKey) + "; partial-range baking is not supported";
        return false;
    }
    const std::int64_t span = static_cast<std::int64_t>(last) - static_cast<std::int64_t>(first);
    const std::int64_t stride = step;
    const std::int64_t count = span / stride + 1 + (span % stride != 0 ? 1 : 0);
    if (count > static_cast<std::int64_t>(kMaximumKeys)) {
        error = "baking " + std::to_string(first) + ".." + std::to_string(last) + " every " +
                std::to_string(step) + " frames would need " + std::to_string(count) +
                " keys, the limit is " + std::to_string(kMaximumKeys);
        return false;
    }
    std::vector<std::int64_t> frames;
    frames.reserve(static_cast<std::size_t>(count));
    for (std::int64_t frame = first; frame <= last; frame += stride) frames.push_back(frame);
    if (frames.empty() || frames.back() != last) frames.push_back(last);

    Track baked;
    baked.discrete = track.discrete;
    baked.keys.reserve(frames.size());
    for (const std::int64_t frame : frames) {
        Key key;
        key.frame = static_cast<std::int32_t>(frame);
        key.value = Evaluate(track, static_cast<double>(frame)).value;
        key.interpolation = track.discrete ? Interpolation::Step : Interpolation::Linear;
        key.tangent = track.discrete ? Tangent::Flat : Tangent::Linear;
        baked.keys.push_back(key);
    }
    // Give every key the chord slope of the intervals it touches.  A Bezier
    // using those slopes with the default weights reproduces the straight
    // segments exactly, so the bake survives a later interpolation change.
    for (std::size_t index = 1; index < baked.keys.size(); ++index) {
        const double slope = SecantSlope(baked.keys[index - 1], baked.keys[index]);
        baked.keys[index - 1].outSlope = slope;
        baked.keys[index].inSlope = slope;
    }
    output = std::move(baked);
    return true;
}

bool Encode(const std::vector<NamedTrack> &tracks, std::vector<std::uint8_t> &output,
            std::string &error) {
    error.clear();
    std::size_t totalKeys = 0;
    std::size_t totalBytes = kHeaderBytes;
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        const NamedTrack &named = tracks[index];
        const std::string label = "track " + std::to_string(index) + ": ";
        if (named.id.empty()) {
            error = label + "the id is empty";
            return false;
        }
        if (named.id.size() > kMaximumIdBytes) {
            error = label + "the id is longer than " + std::to_string(kMaximumIdBytes) + " bytes";
            return false;
        }
        if (!IsValidUtf8(named.id)) {
            error = label + "the id is not valid UTF-8";
            return false;
        }
        std::string reason;
        if (!Validate(named.track, reason)) {
            error = label + "(\"" + named.id + "\") " + reason;
            return false;
        }
        totalKeys += named.track.keys.size();
        totalBytes += kMinimumTrackBytes + named.id.size() + named.track.keys.size() * kKeyRecordBytes;
    }
    if (totalKeys > kMaximumKeys) {
        error = "the payload would hold " + std::to_string(totalKeys) + " keys in total, the limit is " +
                std::to_string(kMaximumKeys);
        return false;
    }
    if (totalBytes > kMaximumEncodedBytes) {
        error = "the payload would need " + std::to_string(totalBytes) + " bytes, the limit is " +
                std::to_string(kMaximumEncodedBytes);
        return false;
    }
    std::vector<const std::string *> ids;
    ids.reserve(tracks.size());
    for (const NamedTrack &named : tracks) ids.push_back(&named.id);
    std::sort(ids.begin(), ids.end(),
              [](const std::string *left, const std::string *right) { return *left < *right; });
    for (std::size_t index = 1; index < ids.size(); ++index) {
        if (*ids[index - 1] == *ids[index]) {
            error = "duplicate track id \"" + *ids[index] + "\"";
            return false;
        }
    }

    std::vector<std::uint8_t> buffer;
    buffer.reserve(totalBytes);
    buffer.push_back('M');
    buffer.push_back('M');
    buffer.push_back('S');
    buffer.push_back('C');
    WriteU16(buffer, 1);
    WriteU16(buffer, 0);
    WriteU32(buffer, static_cast<std::uint32_t>(tracks.size()));
    for (const NamedTrack &named : tracks) {
        WriteU32(buffer, static_cast<std::uint32_t>(named.id.size()));
        buffer.insert(buffer.end(), named.id.begin(), named.id.end());
        WriteU8(buffer, named.track.discrete ? 1 : 0);
        WriteU32(buffer, static_cast<std::uint32_t>(named.track.keys.size()));
        for (const Key &key : named.track.keys) {
            WriteI32(buffer, key.frame);
            WriteF64(buffer, key.value);
            WriteF64(buffer, key.inSlope);
            WriteF64(buffer, key.outSlope);
            WriteF64(buffer, key.inWeight);
            WriteF64(buffer, key.outWeight);
            WriteU32(buffer, static_cast<std::uint32_t>(key.interpolation));
            WriteU32(buffer, static_cast<std::uint32_t>(key.tangent));
        }
    }
    output.swap(buffer);
    return true;
}

bool Decode(const std::uint8_t *data, std::size_t size, std::vector<NamedTrack> &output,
            std::string &error) {
    error.clear();
    if (data == nullptr) {
        error = "there is no payload to decode";
        return false;
    }
    if (size > kMaximumEncodedBytes) {
        error = "the payload is " + std::to_string(size) + " bytes, the limit is " +
                std::to_string(kMaximumEncodedBytes);
        return false;
    }
    Reader reader(data, size);
    std::uint8_t magic[4] = {};
    if (!reader.Read(magic, sizeof(magic))) {
        error = "the payload is shorter than the " + std::to_string(kHeaderBytes) + "-byte header";
        return false;
    }
    if (!((magic[0] == 'M' && magic[1] == 'M' && magic[2] == 'S' && magic[3] == 'C') ||
          (magic[0] == 'M' && magic[1] == 'D' && magic[2] == 'S' && magic[3] == 'C'))) {
        error = "the payload does not start with the MMSC magic";
        return false;
    }
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint32_t trackCount = 0;
    if (!reader.ReadU16(version) || !reader.ReadU16(flags) || !reader.ReadU32(trackCount)) {
        error = "the payload is shorter than the " + std::to_string(kHeaderBytes) + "-byte header";
        return false;
    }
    if (version != 1) {
        error = "unsupported format version " + std::to_string(version) + ", this build reads version 1";
        return false;
    }
    if (flags != 0) {
        error = "reserved header flags must be zero, got " + std::to_string(flags);
        return false;
    }
    if (trackCount > reader.Remaining() / kMinimumTrackBytes) {
        error = "the header claims " + std::to_string(trackCount) + " tracks, which does not fit in the " +
                std::to_string(reader.Remaining()) + " remaining bytes";
        return false;
    }

    std::vector<NamedTrack> decoded;
    // The count is only bounded by the payload size at this point, so reserve a
    // modest amount and let the vector grow while the tracks are validated.
    decoded.reserve(std::min<std::size_t>(trackCount, 1024));
    std::size_t totalKeys = 0;
    for (std::uint32_t index = 0; index < trackCount; ++index) {
        const std::string label = "track " + std::to_string(index) + ": ";
        std::uint32_t idLength = 0;
        if (!reader.ReadU32(idLength)) {
            error = label + "the id length is missing (truncated payload)";
            return false;
        }
        if (idLength == 0) {
            error = label + "the id is empty";
            return false;
        }
        if (idLength > kMaximumIdBytes) {
            error = label + "the id is longer than " + std::to_string(kMaximumIdBytes) + " bytes";
            return false;
        }
        std::string id;
        if (!reader.ReadText(idLength, id)) {
            error = label + "the id is truncated";
            return false;
        }
        if (!IsValidUtf8(id)) {
            error = label + "the id is not valid UTF-8";
            return false;
        }
        std::uint8_t discrete = 0;
        if (!reader.ReadU8(discrete)) {
            error = label + "the discrete flag is missing (truncated payload)";
            return false;
        }
        if (discrete > 1) {
            error = label + "the discrete flag must be 0 or 1, got " + std::to_string(discrete);
            return false;
        }
        std::uint32_t keyCount = 0;
        if (!reader.ReadU32(keyCount)) {
            error = label + "the key count is missing (truncated payload)";
            return false;
        }
        if (keyCount > kMaximumKeys) {
            error = label + "the key count " + std::to_string(keyCount) + " exceeds the limit of " +
                    std::to_string(kMaximumKeys);
            return false;
        }
        if (keyCount > reader.Remaining() / kKeyRecordBytes) {
            error = label + "the key count " + std::to_string(keyCount) + " needs " +
                    std::to_string(static_cast<std::size_t>(keyCount) * kKeyRecordBytes) +
                    " bytes but only " + std::to_string(reader.Remaining()) + " remain";
            return false;
        }
        totalKeys += keyCount;
        if (totalKeys > kMaximumKeys) {
            error = "the payload declares more than " + std::to_string(kMaximumKeys) + " keys in total";
            return false;
        }
        NamedTrack named;
        named.id = std::move(id);
        named.track.discrete = discrete == 1;
        named.track.keys.resize(keyCount);
        for (std::uint32_t keyIndex = 0; keyIndex < keyCount; ++keyIndex) {
            Key &key = named.track.keys[keyIndex];
            std::uint32_t interpolation = 0;
            std::uint32_t tangent = 0;
            if (!reader.ReadI32(key.frame) || !reader.ReadF64(key.value) || !reader.ReadF64(key.inSlope) ||
                !reader.ReadF64(key.outSlope) || !reader.ReadF64(key.inWeight) ||
                !reader.ReadF64(key.outWeight) || !reader.ReadU32(interpolation) ||
                !reader.ReadU32(tangent)) {
                error = label + "key " + std::to_string(keyIndex) + " is truncated";
                return false;
            }
            key.interpolation = static_cast<Interpolation>(interpolation);
            key.tangent = static_cast<Tangent>(tangent);
        }
        std::string reason;
        if (!Validate(named.track, reason)) {
            error = label + "(\"" + named.id + "\") " + reason;
            return false;
        }
        decoded.push_back(std::move(named));
    }
    if (reader.Remaining() != 0) {
        error = "there are " + std::to_string(reader.Remaining()) +
                " bytes of trailing data after the last track";
        return false;
    }
    std::vector<const std::string *> ids;
    ids.reserve(decoded.size());
    for (const NamedTrack &named : decoded) ids.push_back(&named.id);
    std::sort(ids.begin(), ids.end(),
              [](const std::string *left, const std::string *right) { return *left < *right; });
    for (std::size_t index = 1; index < ids.size(); ++index) {
        if (*ids[index - 1] == *ids[index]) {
            error = "duplicate track id \"" + *ids[index] + "\"";
            return false;
        }
    }
    output = std::move(decoded);
    return true;
}

}  // namespace mikumikustudio::animation_curve
