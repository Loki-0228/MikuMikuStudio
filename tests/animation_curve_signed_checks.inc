// Include at file scope after the other native_edit_history_tests inc files.
// Uses the parent's Check(bool, const char*); no separate runner is required.
#include "mikumikustudio/animation_curve.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {
namespace animation_curve_signed_checks {
namespace curve = mikumikustudio::animation_curve;
constexpr std::int32_t minimum = mikumikustudio::frame_limits::minimum;
constexpr std::int32_t maximum = mikumikustudio::frame_limits::maximum;
static_assert(std::int64_t(maximum) - std::int64_t(minimum) == 4294967292LL,
              "exercise the entire signed authoring span");

void Near(double actual, double expected, double tolerance, const char* what) {
    Check(std::isfinite(actual) && std::fabs(actual - expected) <= tolerance, what);
}

curve::Track Interval(std::int32_t first, std::int32_t last,
                      curve::Interpolation interpolation) {
    curve::Key a, b;
    a.frame = first;
    b.frame = last;
    a.value = -2;
    b.value = 3;
    a.tangent = b.tangent = curve::Tangent::Free;
    a.interpolation = b.interpolation = interpolation;
    const double span = static_cast<double>(std::int64_t(last) - std::int64_t(first));
    a.outWeight = 1.0 / 6.0;
    b.inWeight = 0.5;
    a.outSlope = 12.0 / span;
    b.inSlope = 8.0 / span;
    curve::Track result;
    result.keys = {a, b};
    return result;
}

bool Same(const curve::Track& a, const curve::Track& b) {
    if (a.discrete != b.discrete || a.keys.size() != b.keys.size()) return false;
    for (std::size_t i = 0; i < a.keys.size(); ++i) {
        const auto& x = a.keys[i];
        const auto& y = b.keys[i];
        if (x.frame != y.frame || x.value != y.value || x.inSlope != y.inSlope ||
            x.outSlope != y.outSlope || x.inWeight != y.inWeight ||
            x.outWeight != y.outWeight || x.interpolation != y.interpolation ||
            x.tangent != y.tangent) return false;
    }
    return true;
}

curve::Sample Oracle(curve::Interpolation interpolation, bool discrete, double u, double span) {
    if (discrete || interpolation == curve::Interpolation::Step)
        return {u == 1 ? 3.0 : -2.0, 0};
    if (interpolation == curve::Interpolation::Linear) return {-2 + 5 * u, 5 / span};
    // Independently specified parametric curve:
    // x(s)=(s+s^2)/2, y(s)=-2+6s-9s^2+8s^3. Solve the quadratic exactly,
    // then apply dy/dframe=(dy/ds)/(span*dx/ds). This neither copies the
    // production cubic root finder nor computes expected values via Evaluate.
    const double s = (std::sqrt(1 + 8 * u) - 1) / 2;
    return {-2 + 6 * s - 9 * s * s + 8 * s * s * s,
            (6 - 18 * s + 24 * s * s) / (span * (0.5 + s))};
}

void EvaluationAndTranslation() {
    struct Range { std::int32_t first, last; std::int64_t shift; };
    const Range ranges[] = {
        {minimum, maximum, 0}, // No nonzero translation fits the full legal span.
        {minimum, maximum - 64, 32},
        {minimum, 0, maximum},
        {-2, maximum, -1}, // One more than INT32_MAX frames.
        {minimum, -1, maximum},
        {minimum, minimum + 1, maximum},
        {maximum - 1, maximum, -std::int64_t(maximum)},
        {-1, 0, maximum},
        {-19, 34, 19},
        {minimum, minimum + 33, maximum},
    };
    for (const auto& range : ranges) {
        const double span = static_cast<double>(std::int64_t(range.last) - range.first);
        for (auto mode : {curve::Interpolation::Step, curve::Interpolation::Linear,
                          curve::Interpolation::Bezier}) {
            for (bool discrete : {false, true}) {
                auto track = Interval(range.first, range.last, mode);
                track.discrete = discrete;
                std::string error = "stale";
                Check(curve::ValidateSigned(track, error) && error.empty(),
                      "signed interval validates and clears the diagnostic");
                auto translated = track;
                for (auto& key : translated.keys)
                    key.frame = static_cast<std::int32_t>(std::int64_t(key.frame) + range.shift);
                Check(curve::ValidateSigned(translated, error), "translated interval stays legal");
                if (translated.keys.front().frame >= 0)
                    Check(curve::Validate(translated, error), "positive translation uses legacy validation");
                for (double u : {0.0, 0.0625, 0.125, 0.25, 0.5, 0.75, 0.875, 0.9375, 1.0}) {
                    const double frame = double(range.first) + span * u;
                    const auto expected = Oracle(mode, discrete, u, span);
                    const auto sample = curve::Evaluate(track, frame);
                    Near(sample.value, expected.value, 2e-11, "signed value matches analytic oracle");
                    // Absolute 1e-9 would let a zero derivative pass on a huge span.
                    Near(sample.derivative, expected.derivative, 2e-10 / span,
                         "signed derivative matches analytic oracle, including tiny slopes");
                    const auto shifted = curve::Evaluate(translated, frame + double(range.shift));
                    Near(shifted.value, expected.value, 2e-11, "translated value matches independent oracle");
                    Near(shifted.derivative, expected.derivative, 2e-10 / span,
                         "translated derivative matches independent oracle");
                    Near(shifted.value, sample.value, 2e-11, "value is invariant under frame translation");
                    Near(shifted.derivative, sample.derivative, 2e-10 / span,
                         "derivative is invariant under frame translation");
                }
                const auto before = curve::Evaluate(track, double(range.first) - 0.5);
                const auto after = curve::Evaluate(track, double(range.last) + 0.5);
                Check(before.value == -2 && before.derivative == 0 &&
                      after.value == 3 && after.derivative == 0,
                      "signed intervals clamp outside endpoints with zero derivative");
            }
        }
    }
}

void UnequalIntervalsAndSharedMath() {
    // Moving the middle key tests a one-frame interval next to a >INT32_MAX
    // interval, unequal wide intervals, and selection of the right interval
    // at an exact interior key. All expected values are closed-form.
    for (auto middle : {minimum + 1, -123, maximum - 1}) {
        const double left = double(std::int64_t(middle) - minimum);
        const double right = double(std::int64_t(maximum) - middle);
        for (auto mode : {curve::Interpolation::Step, curve::Interpolation::Linear,
                          curve::Interpolation::Bezier}) {
            auto track = Interval(minimum, middle, mode);
            track.keys[0].value = 1;
            track.keys[1].value = 7;
            auto last = track.keys[1];
            last.frame = maximum;
            last.value = -5;
            track.keys.push_back(last);
            // Uniform-time Bezier with horizontal handles is smoothstep.
            for (auto& key : track.keys) {
                key.inWeight = key.outWeight = 1.0 / 3.0;
                key.inSlope = key.outSlope = 0;
            }
            std::string error;
            Check(curve::ValidateSigned(track, error), "unequal signed intervals validate");
            for (std::size_t i = 0; i < 2; ++i) {
                const double span = i == 0 ? left : right;
                const double base = i == 0 ? 1 : 7;
                const double delta = i == 0 ? 6 : -12;
                for (double u : {0.0, 0.25, 0.5, 0.75}) {
                    const auto sample = curve::Evaluate(track, double(track.keys[i].frame) + u * span);
                    const double f = mode == curve::Interpolation::Step ? 0 :
                        mode == curve::Interpolation::Linear ? u : u * u * (3 - 2 * u);
                    const double df = mode == curve::Interpolation::Step ? 0 :
                        mode == curve::Interpolation::Linear ? 1 : 6 * u * (1 - u);
                    Near(sample.value, base + delta * f, 2e-11, "unequal intervals match polynomial values");
                    Near(sample.derivative, delta * df / span, 2e-10 / span,
                         "unequal intervals match polynomial derivatives");
                }
            }
            const auto final = curve::Evaluate(track, maximum);
            Near(final.value, -5, 0, "last signed key is exact");
            Near(final.derivative, mode == curve::Interpolation::Linear ? -12 / right : 0,
                 2e-10 / right, "last key uses the incoming interval derivative");
        }
        auto automatic = Interval(minimum, middle, curve::Interpolation::Bezier);
        automatic.keys.push_back(automatic.keys.back());
        automatic.keys.back().frame = maximum;
        for (auto& key : automatic.keys) {
            key.value = 2.0 * key.frame + 5;
            key.tangent = curve::Tangent::Automatic;
        }
        curve::UpdateAutomatic(automatic);
        for (const auto& key : automatic.keys) {
            Near(key.inSlope, 2, 1e-14, "automatic incoming secant survives signed spans");
            Near(key.outSlope, 2, 1e-14, "automatic outgoing secant survives signed spans");
        }
        Check(curve::SideIsStraight(automatic, 1, false) && curve::SideIsStraight(automatic, 1, true),
              "shared automatic and secant helpers agree on a signed straight line");
    }
}

void ValidationBoundaries() {
    std::string error = "stale";
    Check(curve::ValidateSigned({}, error) && error.empty(), "empty signed track clears errors");
    const auto empty = curve::Evaluate({}, -7);
    Check(empty.value == 0 && empty.derivative == 0, "empty signed evaluation is zero");
    for (auto frame : {minimum, minimum + 1, -1, 0, 1, maximum - 1, maximum}) {
        curve::Track track;
        track.keys.resize(1);
        track.keys[0].frame = frame;
        track.keys[0].value = 13;
        error = "stale";
        Check(curve::ValidateSigned(track, error) && error.empty(), "signed boundary singleton is valid");
        for (double probe : {double(frame) - 1, double(frame), double(frame) + 1}) {
            const auto sample = curve::Evaluate(track, probe);
            Check(sample.value == 13 && sample.derivative == 0, "signed singleton is constant");
        }
        const bool legacy = curve::Validate(track, error);
        Check(legacy == (frame >= 0), "legacy validator still admits only nonnegative frames");
        if (frame < 0)
            Check(error == "key 0: frame " + std::to_string(frame) + " is negative",
                  "legacy negative frame diagnostic is unchanged");
    }
    for (auto frame : {(std::numeric_limits<std::int32_t>::min)(), minimum - 1,
                      (std::numeric_limits<std::int32_t>::max)()}) {
        for (std::size_t index : {std::size_t(0), std::size_t(1)}) {
            auto track = Interval(minimum, maximum, curve::Interpolation::Linear);
            track.keys[index].frame = frame;
            Check(!curve::ValidateSigned(track, error) && error == "key " + std::to_string(index) +
                  ": frame " + std::to_string(frame) + " is outside [-2147483646, 2147483646]",
                  "reserved signed frame is rejected with indexed range diagnostic");
        }
    }
    curve::Track legacyMaximum;
    legacyMaximum.keys.resize(1);
    legacyMaximum.keys[0].frame = (std::numeric_limits<std::int32_t>::max)();
    Check(curve::Validate(legacyMaximum, error), "legacy positive upper-bound behavior is unchanged");
}

void ValidationFields() {
    const double badNumbers[] = {(std::numeric_limits<double>::quiet_NaN)(),
                                (std::numeric_limits<double>::infinity)(),
                                -(std::numeric_limits<double>::infinity)()};
    for (auto mode : {curve::Interpolation::Step, curve::Interpolation::Linear,
                      curve::Interpolation::Bezier}) {
        for (bool discrete : {false, true}) {
            auto valid = Interval(-9, 7, mode);
            valid.discrete = discrete;
            const auto reject = [&](const curve::Track& bad, const std::string& expected) {
                std::string error = "stale";
                Check(!curve::ValidateSigned(bad, error) && error == expected,
                      "signed validation preserves field diagnostics even on step/discrete tracks");
                auto positive = bad;
                for (auto& key : positive.keys) key.frame += 10;
                Check(!curve::Validate(positive, error) && error == expected,
                      "same invalid positive fields retain the legacy diagnostic");
            };
            for (std::size_t index : {std::size_t(0), std::size_t(1)}) {
                const std::string label = "key " + std::to_string(index) + ": ";
                struct Field { double curve::Key::*member; const char* diagnostic; };
                const Field fields[] = {{&curve::Key::value, "value"},
                    {&curve::Key::inSlope, "slope"}, {&curve::Key::outSlope, "slope"},
                    {&curve::Key::inWeight, "handle weight"}, {&curve::Key::outWeight, "handle weight"}};
                for (const auto& field : fields) for (double number : badNumbers) {
                    auto bad = valid;
                    bad.keys[index].*field.member = number;
                    reject(bad, label + field.diagnostic + " is not a finite number");
                }
                for (bool incoming : {false, true}) for (double weight : {-0.5, 0.0, 0.000099, 0.999901, 1.0}) {
                    auto bad = valid;
                    (incoming ? bad.keys[index].inWeight : bad.keys[index].outWeight) = weight;
                    reject(bad, label + (incoming ? "inWeight " : "outWeight ") + std::to_string(weight) +
                                " is outside [0.0001, 0.9999]");
                }
                for (auto raw : {std::uint32_t(99), (std::numeric_limits<std::uint32_t>::max)()}) {
                    auto bad = valid;
                    bad.keys[index].interpolation = static_cast<curve::Interpolation>(raw);
                    reject(bad, label + "interpolation value " + std::to_string(raw) + " is not a known interpolation");
                    bad = valid;
                    bad.keys[index].tangent = static_cast<curve::Tangent>(raw);
                    reject(bad, label + "tangent value " + std::to_string(raw) + " is not a known tangent mode");
                }
            }
            for (auto tangent : {curve::Tangent::Free, curve::Tangent::Aligned, curve::Tangent::Flat,
                                 curve::Tangent::Linear, curve::Tangent::Automatic}) {
                auto track = valid;
                for (auto& key : track.keys) key.tangent = tangent;
                std::string error;
                Check(curve::ValidateSigned(track, error), "all known tangent tags remain valid");
            }
            for (auto second : {-9, -10}) {
                auto bad = valid;
                bad.keys[1].frame = second;
                std::string error;
                Check(!curve::ValidateSigned(bad, error) && error == "frame " + std::to_string(second) +
                      " at key 1 does not follow frame -9 at key 0; key frames must increase strictly",
                      "duplicate and reversed negative frames retain strict ordering diagnostic");
                bad.keys[0].frame += 10;
                bad.keys[1].frame += 10;
                Check(!curve::Validate(bad, error) && error == "frame " + std::to_string(second + 10) +
                      " at key 1 does not follow frame 1 at key 0; key frames must increase strictly",
                      "positive ordering diagnostic remains unchanged");
            }
            auto weights = valid;
            weights.keys[0].outWeight = 0.4999;
            weights.keys[1].inWeight = 0.5;
            std::string error;
            Check(curve::ValidateSigned(weights, error), "maximum legal paired weight is accepted");
            for (double outgoing : {0.49995, 0.5, 0.6}) {
                weights.keys[0].outWeight = outgoing;
                reject(weights, "key 1: outWeight " + std::to_string(outgoing) +
                    " of the previous key plus inWeight 0.500000 exceeds 0.9999, which would make the interval ambiguous in time");
            }
            // Individual endpoints may use both inclusive weight limits.
            weights = valid;
            weights.keys[0].inWeight = weights.keys[1].outWeight = 0.9999;
            weights.keys[0].outWeight = weights.keys[1].inWeight = 0.0001;
            Check(curve::ValidateSigned(weights, error), "individual inclusive weight bounds are retained");
        }
    }
}

void LegacyAdmission() {
    const auto signedTrack = Interval(-9, 7, curve::Interpolation::Bezier);
    std::string error;
    auto edited = signedTrack;
    Check(!curve::SetTangent(edited, 0, curve::Tangent::Flat, error) && Same(edited, signedTrack),
          "SetTangent retains negative-frame rejection without mutation");
    Check(!curve::SetMmdControls(edited, 1, {20, 20, 100, 100}, error) && Same(edited, signedTrack),
          "SetMmdControls retains negative-frame rejection without mutation");
    std::size_t index = 0;
    Check(curve::InsertPreservingShape(edited, 1, index, error) == curve::InsertResult::Rejected &&
          Same(edited, signedTrack), "insertion does not admit a signed track");
    const auto sentinel = Interval(0, 8, curve::Interpolation::Linear);
    auto output = sentinel;
    Check(!curve::Bake(signedTrack, 0, 8, 1, output, error) && Same(output, sentinel),
          "Bake rejects signed input without replacing output");
    Check(!curve::Bake(sentinel, -1, 8, 1, output, error) && Same(output, sentinel),
          "Bake still rejects a negative requested range");
    std::vector<std::uint8_t> bytes = {0x12, 0x34};
    Check(!curve::Encode({{"s", signedTrack}}, bytes, error) && bytes == std::vector<std::uint8_t>({0x12, 0x34}),
          "codec encode does not gain signed admission or replace output on failure");
    Check(curve::Encode({{"s", sentinel}}, bytes, error), "prepare positive codec fixture");
    // Version-1 wire layout: 12-byte header, id length, one id byte, discrete,
    // key count, then the first little-endian i32 frame.
    constexpr std::size_t frameOffset = 12 + 4 + 1 + 1 + 4;
    Check(bytes.size() >= frameOffset + 4, "codec fixture contains a complete frame");
    for (auto frame : {-1, minimum, (std::numeric_limits<std::int32_t>::min)()}) {
        auto malformed = bytes;
        const auto bits = static_cast<std::uint32_t>(frame);
        for (unsigned i = 0; i < 4; ++i)
            malformed[frameOffset + i] = static_cast<std::uint8_t>(bits >> (8 * i));
        std::vector<curve::NamedTrack> decoded = {{"keep", sentinel}};
        Check(!curve::Decode(malformed.data(), malformed.size(), decoded, error) &&
              error == "track 0: (\"s\") key 0: frame " + std::to_string(frame) + " is negative",
              "codec decode retains negative frame diagnostics");
        Check(decoded.size() == 1 && decoded[0].id == "keep" && Same(decoded[0].track, sentinel),
              "negative decode leaves prior output intact");
    }
}
} // namespace animation_curve_signed_checks
} // namespace

void AnimationCurveSignedChecks() {
    animation_curve_signed_checks::EvaluationAndTranslation();
    animation_curve_signed_checks::UnequalIntervalsAndSharedMath();
    animation_curve_signed_checks::ValidationBoundaries();
    animation_curve_signed_checks::ValidationFields();
    animation_curve_signed_checks::LegacyAdmission();
}
