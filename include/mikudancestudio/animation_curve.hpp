#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace mikudancestudio::animation_curve {
enum class Interpolation : std::uint32_t { Step, Linear, Bezier };
enum class Tangent : std::uint32_t { Free, Aligned, Flat, Linear, Automatic };
// Interpolation belongs to the interval ending at this key. Slopes are per frame.
struct Key {
    std::int32_t frame = 0;
    double value = 0, inSlope = 0, outSlope = 0;
    double inWeight = 1.0 / 3.0, outWeight = 1.0 / 3.0;
    Interpolation interpolation = Interpolation::Bezier;
    Tangent tangent = Tangent::Automatic;
};
struct Track { std::vector<Key> keys; bool discrete = false; };
struct Sample { double value = 0, derivative = 0; };
struct NamedTrack { std::string id; Track track; };
// Key counts follow frame/index representation and addressable storage.
constexpr std::size_t kMaximumKeys =
    ((std::numeric_limits<std::size_t>::max)() / sizeof(Key) <
     static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()))
        ? (std::numeric_limits<std::size_t>::max)() / sizeof(Key)
        : static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)());
bool Validate(const Track&, std::string& error);
// Clamps outside the first/last frame; Bezier time is inverted, not assumed linear.
Sample Evaluate(const Track&, double frame);
// Smooth display/subdivision of MMD timing controls (in 0..127 units).
// Subdivision may need full precision MMS-only controls outside that range.
// Derivative is with respect to normalized time. Native playback stays unchanged.
Sample EvaluateMmdTiming(const std::array<double,4>& controls, double time);
void UpdateAutomatic(Track&);
enum class InsertResult { Inserted, Existing, Rejected };
// Splits the time/value Bezier at an integer frame without changing its shape.
// Existing frames are no-ops; unrepresentable weights leave the track intact.
InsertResult InsertPreservingShape(Track&, std::int32_t frame, std::size_t& index, std::string& error);
bool SetTangent(Track&, std::size_t key, Tangent, std::string& error);
// Per-side Bezier editing.  A key's in side is the interval ending at it, its
// out side the interval starting at it.  A side is straight when that interval
// is not a Bezier (or the track is discrete, or the side does not exist) or
// when the key's handle on it lies on the chord (slope equals the secant).
bool SideIsStraight(const Track&, std::size_t key, bool outSide);
bool KeyIsLinear(const Track&, std::size_t key);  // both sides straight
// Puts the key's handle on that side onto the chord.  When the neighbour's
// facing handle is on the chord too, the interval becomes Linear.
void StraightenSide(Track&, std::size_t key, bool outSide);
void MakeKeyLinear(Track&, std::size_t key);  // straightens both sides
// Turns the key's linear sides into Bezier handles with one shared smooth
// slope (flat when the neighbours are collinear); the neighbours' facing
// handles stay on the chord.  Step intervals are left alone.
void MakeKeyBezier(Track&, std::size_t key);
// Ease: one side flat (curved), the other straight.  easeIn curves the in side.
void ApplyEase(Track&, std::size_t key, bool easeIn);
// Moves a handle of `key` to (frame, value).  The weight is clamped to the
// valid range; an Aligned key keeps both slopes equal, any other mode becomes
// Free.  Returns false when that side has no Bezier interval.
bool MoveHandle(Track&, std::size_t key, bool outSide, double frame, double value);
// Exact compatibility view. Rejects equal-valued endpoints, overshoot and step.
bool GetMmdControls(const Track&, std::size_t endKey, std::array<double,4>& controls);
bool SetMmdControls(Track&, std::size_t endKey, const std::array<double,4>& controls,
                    std::string& error);
bool Bake(const Track&, std::int32_t first, std::int32_t last, std::int32_t step,
          Track& output, std::string& error);
// Bounded, versioned little-endian codec, with transactional decode.
bool Encode(const std::vector<NamedTrack>&, std::vector<std::uint8_t>&, std::string& error);
bool Decode(const std::uint8_t*, std::size_t, std::vector<NamedTrack>&, std::string& error);
}
