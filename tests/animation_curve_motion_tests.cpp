// MAINTENANCE: unit coverage for the VMD curve transport
// (src/animation/animation_curve_motion.cpp).  Pure CPU: no GUI, no GPU and no
// window; the file test only creates a temporary directory.
#include "mikumikustudio/animation_curve_motion.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cm = mikumikustudio::curve_motion;
namespace ac = mikumikustudio::animation_curve;

namespace {

int g_failures = 0;

void Check(bool pass, const std::string& text) {
    if (pass) return;
    ++g_failures;
    std::printf("FAIL %s\n", text.c_str());
}

void CheckNear(double actual, double expected, double tolerance, const std::string& text) {
    if (std::isfinite(actual) && std::fabs(actual - expected) <= tolerance) return;
    ++g_failures;
    std::printf("FAIL %s: expected %.17g, got %.17g (tolerance %.3g)\n", text.c_str(), expected, actual,
                tolerance);
}

// ---------------------------------------------------------------------------
// Small independent math used as the expected result
// ---------------------------------------------------------------------------

double Cubic(double p0, double p1, double p2, double p3, double s) {
    const double t = 1.0 - s;
    return t * t * t * p0 + 3.0 * t * t * s * p1 + 3.0 * t * s * s * p2 + s * s * s * p3;
}

// MMD easing: control bytes are 0..127, the x cubic is inverted by bisection and
// the y cubic gives the eased fraction of the value range.
double EaseFraction(double x1, double y1, double x2, double y2, double u) {
    if (x1 == y1 && x2 == y2) return u;
    const double a = x1 / 127.0;
    const double b = x2 / 127.0;
    double low = 0.0;
    double high = 1.0;
    for (int step = 0; step < 200; ++step) {
        const double middle = 0.5 * (low + high);
        if (Cubic(0.0, a, b, 1.0, middle) < u) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return Cubic(0.0, y1 / 127.0, y2 / 127.0, 1.0, 0.5 * (low + high));
}

std::array<double, 4> QuaternionDegrees(double xDeg, double yDeg, double zDeg) {
    const double r = 3.14159265358979323846 / 360.0;
    const double x = xDeg * r, y = yDeg * r, z = zDeg * r;
    const double cx = std::cos(x), sx = std::sin(x);
    const double cy = std::cos(y), sy = std::sin(y);
    const double cz = std::cos(z), sz = std::sin(z);
    return {cy * sx * cz + sy * cx * sz, sy * cx * cz - cy * sx * sz, cy * cx * sz - sy * sx * cz,
            cy * cx * cz + sy * sx * sz};
}

std::array<double, 3> EulerDegrees(std::array<double, 4> q) {
    double norm = 0.0;
    for (double v : q) norm += v * v;
    for (double& v : q) v /= std::sqrt(norm);
    const double pi = 3.14159265358979323846;
    const double pitch = std::asin(std::clamp(2.0 * (q[3] * q[0] - q[1] * q[2]), -1.0, 1.0));
    const double yaw = std::atan2(2.0 * (q[3] * q[1] + q[0] * q[2]), 1.0 - 2.0 * (q[0] * q[0] + q[1] * q[1]));
    const double roll = std::atan2(2.0 * (q[3] * q[2] + q[0] * q[1]), 1.0 - 2.0 * (q[0] * q[0] + q[2] * q[2]));
    return {pitch * 180.0 / pi, yaw * 180.0 / pi, roll * 180.0 / pi};
}

std::array<double, 4> Slerp(std::array<double, 4> a, std::array<double, 4> b, double t) {
    double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.0) {
        for (double& v : b) v = -v;
        dot = -dot;
    }
    const double theta = std::acos(std::clamp(dot, -1.0, 1.0));
    std::array<double, 4> out{};
    if (std::sin(theta) < 1e-9) {
        for (int i = 0; i < 4; ++i) out[std::size_t(i)] = a[std::size_t(i)];
    } else {
        const double wa = std::sin((1.0 - t) * theta) / std::sin(theta);
        const double wb = std::sin(t * theta) / std::sin(theta);
        for (int i = 0; i < 4; ++i) out[std::size_t(i)] = a[std::size_t(i)] * wa + b[std::size_t(i)] * wb;
    }
    double norm = 0.0;
    for (double v : out) norm += v * v;
    for (double& v : out) v /= std::sqrt(norm);
    return out;
}

// ---------------------------------------------------------------------------
// Track helpers
// ---------------------------------------------------------------------------

ac::Track MakeTrack(std::initializer_list<std::pair<std::int32_t, double>> keys, bool discrete = false) {
    ac::Track track;
    track.discrete = discrete;
    for (const auto& entry : keys) {
        ac::Key key;
        key.frame = entry.first;
        key.value = entry.second;
        key.tangent = ac::Tangent::Free;
        key.interpolation = discrete ? ac::Interpolation::Step : ac::Interpolation::Linear;
        track.keys.push_back(key);
    }
    return track;
}

const ac::Track* Find(const cm::Motion& motion, const std::string& id) {
    for (const ac::NamedTrack& named : motion.tracks) {
        if (named.id == id) return &named.track;
    }
    return nullptr;
}

bool SameTracks(const cm::Motion& left, const cm::Motion& right) {
    if (left.kind != right.kind) return false;
    if (left.tracks.size() != right.tracks.size()) return false;
    for (const ac::NamedTrack& named : left.tracks) {
        const ac::Track* other = Find(right, named.id);
        if (other == nullptr) return false;
        if (other->discrete != named.track.discrete) return false;
        if (other->keys.size() != named.track.keys.size()) return false;
        for (std::size_t index = 0; index < named.track.keys.size(); ++index) {
            const ac::Key& a = named.track.keys[index];
            const ac::Key& b = other->keys[index];
            if (a.frame != b.frame || a.value != b.value || a.inSlope != b.inSlope ||
                a.outSlope != b.outSlope || a.inWeight != b.inWeight || a.outWeight != b.outWeight ||
                a.interpolation != b.interpolation || a.tangent != b.tangent) {
                return false;
            }
        }
    }
    return true;
}

cm::Motion MotionFor(cm::Kind kind) {
    cm::Motion motion;
    motion.kind = kind;
    auto add = [&motion](const char* id, std::initializer_list<std::pair<std::int32_t, double>> keys,
                         bool discrete = false) {
        motion.tracks.push_back({id, MakeTrack(keys, discrete)});
    };
    switch (kind) {
    case cm::Kind::Camera:
        add("x", {{0, 1.0}, {12, -3.5}});
        add("y", {{0, 0.0}, {12, 2.0}});
        add("z", {{0, 30.0}, {12, 30.0}});
        add("rx", {{0, 0.0}, {12, 15.0}});
        add("ry", {{0, -20.0}, {12, 20.0}});
        add("rz", {{0, 5.0}, {12, 5.0}});
        add("distance", {{0, -45.0}, {12, -20.0}});
        add("fov", {{0, 30.0}, {12, 45.0}});
        add("perspective", {{0, 0.0}, {12, 1.0}}, true);
        break;
    case cm::Kind::Light:
        add("x", {{0, -1.0}, {12, 0.5}});
        add("y", {{0, -1.0}, {12, 0.5}});
        add("z", {{0, 0.0}, {12, 0.0}});
        add("r", {{0, 1.0}, {12, 0.25}});
        add("g", {{0, 1.0}, {12, 0.5}});
        add("b", {{0, 1.0}, {12, 0.75}});
        break;
    case cm::Kind::Shadow:
        add("mode", {{0, 1.0}, {12, 2.0}}, true);
        add("distance", {{0, 0.0}, {12, 0.5}});
        break;
    case cm::Kind::Gravity:
        add("strength", {{0, 9.8}, {12, 1.6}});
        add("x", {{0, 0.0}, {12, 1.0}});
        add("y", {{0, -1.0}, {12, -1.0}});
        add("z", {{0, 0.0}, {12, 0.0}});
        add("noise", {{0, 0.0}, {12, 0.5}});
        add("noiseEnabled", {{0, 0.0}, {12, 1.0}}, true);
        break;
    case cm::Kind::Accessory:
        add("x", {{0, 0.0}, {12, 4.0}});
        add("y", {{0, 0.0}, {12, -2.0}});
        add("z", {{0, 0.0}, {12, 1.5}});
        add("rx", {{0, 0.0}, {12, 25.0}});
        add("ry", {{0, 0.0}, {12, -35.0}});
        add("rz", {{0, 0.0}, {12, 10.0}});
        add("scale", {{0, 1.0}, {12, 2.0}});
        add("opacity", {{0, 1.0}, {12, 0.5}});
        add("visible", {{0, 1.0}, {12, 0.0}}, true);
        break;
    case cm::Kind::Effect:
        add("bloom.strength", {{0, 0.0}, {12, 1.0}});
        add("glow", {{7, 0.5}});
        break;
    default:
        add("\u307e\u3070\u305f\u304d", {{0, 0.0}, {12, 1.0}});  // まばたき
        add("smile", {{0, 0.0}, {12, 0.75}});
        break;
    }
    return motion;
}

// ---------------------------------------------------------------------------
// Hand-built standard VMD files (an independent encoder for the fixed layout)
// ---------------------------------------------------------------------------

struct Cursor {
    std::vector<std::uint8_t> bytes;

    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) u8(std::uint8_t(value >> shift));
    }
    void i32(std::int32_t value) { u32(std::uint32_t(value)); }
    void f32(double value) {
        const float narrowed = float(value);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &narrowed, sizeof(bits));
        u32(bits);
    }
    void text(const std::string& text, std::size_t size) {
        const std::size_t used = std::min(text.size(), size);
        bytes.insert(bytes.end(), text.begin(), text.begin() + std::ptrdiff_t(used));
        bytes.resize(bytes.size() + (size - used), 0);
    }
    void raw(const std::uint8_t* data, std::size_t size) { bytes.insert(bytes.end(), data, data + size); }
};

// MMD bone block: control g (0 x1, 1 y1, 2 x2, 3 y2) of channel ch at byte
// 16*ch + 4*g (four 16-byte channel blocks, as vmd_save.cpp writes).
std::array<std::uint8_t, 64> BoneBlock(const double lanes[4][4]) {
    std::array<std::uint8_t, 64> block{};
    for (int channel = 0; channel < 4; ++channel) {
        for (int control = 0; control < 4; ++control) {
            block[std::size_t(16 * channel + 4 * control)] = std::uint8_t(lanes[channel][control]);
        }
    }
    return block;
}

// MMD camera lane: stored X1, X2, Y1, Y2.
std::array<std::uint8_t, 24> CameraBlock(const double lanes[6][4]) {
    std::array<std::uint8_t, 24> bytes{};
    for (int lane = 0; lane < 6; ++lane) {
        bytes[std::size_t(4 * lane + 0)] = std::uint8_t(lanes[lane][0]);  // X1
        bytes[std::size_t(4 * lane + 1)] = std::uint8_t(lanes[lane][2]);  // X2
        bytes[std::size_t(4 * lane + 2)] = std::uint8_t(lanes[lane][1]);  // Y1
        bytes[std::size_t(4 * lane + 3)] = std::uint8_t(lanes[lane][3]);  // Y2
    }
    return bytes;
}

struct BoneInput {
    std::string name;
    std::uint32_t frame = 0;
    std::array<double, 3> position{};
    std::array<double, 4> quaternion{0.0, 0.0, 0.0, 1.0};
    std::array<std::uint8_t, 64> block{};
};

struct MorphInput {
    std::string name;
    std::uint32_t frame = 0;
    double value = 0.0;
};

struct CameraInput {
    std::uint32_t frame = 0;
    double distance = 0.0;
    std::array<double, 3> eye{};
    std::array<double, 3> target{};
    std::array<std::uint8_t, 24> block{};
    std::uint32_t fov = 45;
    std::uint8_t perspective = 0;
};

struct VmdFile {
    bool legacy = false;
    std::string modelName = "MMV motion";
    std::vector<BoneInput> bones;
    std::vector<MorphInput> morphs;
    std::vector<CameraInput> cameras;
    bool sectionsAfterMorphs = true;  // false: the file ends after the morphs
    std::vector<std::uint8_t> trailing;  // appended foreign tail
};

std::vector<std::uint8_t> Build(const VmdFile& file) {
    Cursor cursor;
    cursor.text(file.legacy ? "Vocaloid Motion Data file" : "Vocaloid Motion Data 0002", 30);
    cursor.text(file.modelName, file.legacy ? 10 : 20);
    cursor.u32(std::uint32_t(file.bones.size()));
    for (const BoneInput& bone : file.bones) {
        cursor.text(bone.name, 15);
        cursor.u32(bone.frame);
        for (double value : bone.position) cursor.f32(value);
        for (double value : bone.quaternion) cursor.f32(value);
        cursor.raw(bone.block.data(), bone.block.size());
    }
    cursor.u32(std::uint32_t(file.morphs.size()));
    for (const MorphInput& morph : file.morphs) {
        cursor.text(morph.name, 15);
        cursor.u32(morph.frame);
        cursor.f32(morph.value);
    }
    if (file.sectionsAfterMorphs) {
        cursor.u32(std::uint32_t(file.cameras.size()));
        for (const CameraInput& camera : file.cameras) {
            cursor.u32(camera.frame);
            cursor.f32(camera.distance);
            for (double value : camera.eye) cursor.f32(value);
            for (double value : camera.target) cursor.f32(value);
            cursor.raw(camera.block.data(), camera.block.size());
            cursor.u32(camera.fov);
            cursor.u8(camera.perspective);
        }
        cursor.u32(0);  // light
        cursor.u32(0);  // self shadow
        cursor.u32(0);  // display/IK master
    }
    cursor.raw(file.trailing.data(), file.trailing.size());
    return cursor.bytes;
}

struct BodyCounts {
    bool parsed = false;
    std::string modelName;
    std::uint32_t bones = 0;
    std::uint32_t morphs = 0;
    std::uint32_t cameras = 0;
    std::uint32_t lights = 0;
    std::uint32_t shadows = 0;
    std::uint32_t ikMasters = 0;
    bool hasIkSection = false;
    std::size_t end = 0;
    std::size_t boneStart = 0;
    std::size_t morphStart = 0;
    std::size_t cameraStart = 0;
    bool legacy = false;
};

// Independent walk over the standard sections: the offsets it lands on prove
// every record length and field order.
bool ParseBody(const std::vector<std::uint8_t>& bytes, BodyCounts& counts, std::string& error) {
    std::size_t pos = 0;
    const auto need = [&](std::size_t count) {
        if (pos + count > bytes.size()) throw std::runtime_error("truncated");
    };
    const auto u32 = [&]() {
        need(4);
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8) value |= std::uint32_t(bytes[pos++]) << shift;
        return value;
    };
    try {
        need(30);
        const std::string signature(reinterpret_cast<const char*>(bytes.data()), 25);
        if (signature == "Vocaloid Motion Data 0002") {
            counts.legacy = false;
            pos = 30;
        } else if (signature == "Vocaloid Motion Data file") {
            counts.legacy = true;
            pos = 30;
        } else {
            error = "bad signature";
            return false;
        }
        need(counts.legacy ? 10 : 20);
        counts.modelName.assign(reinterpret_cast<const char*>(bytes.data() + pos), counts.legacy ? 10 : 20);
        const std::size_t nameEnd = counts.modelName.find('\0');
        if (nameEnd != std::string::npos) counts.modelName.resize(nameEnd);
        pos += counts.legacy ? 10 : 20;

        counts.bones = u32();
        counts.boneStart = pos;
        pos += std::size_t(counts.bones) * 111;
        need(0);
        counts.morphStart = pos;
        counts.morphs = u32();
        pos += std::size_t(counts.morphs) * 23;
        need(0);
        counts.cameraStart = pos;
        counts.cameras = u32();
        pos += std::size_t(counts.cameras) * 61;
        need(0);
        counts.lights = u32();
        pos += std::size_t(counts.lights) * 28;
        need(0);
        counts.shadows = u32();
        pos += std::size_t(counts.shadows) * 9;
        need(0);
        counts.hasIkSection = pos < bytes.size();
        if (counts.hasIkSection) {
            counts.ikMasters = u32();
            for (std::uint32_t index = 0; index < counts.ikMasters; ++index) {
                need(9);
                pos += 5;
                const std::uint32_t chains = u32();
                pos += std::size_t(chains) * 21;
                need(0);
            }
        }
        counts.end = pos;
        counts.parsed = true;
        return true;
    } catch (const std::exception&) {
        error = "the body does not parse";
        return false;
    }
}

constexpr char kFooterMagic[8] = {'M', 'M', 'V', 'C', 'U', 'R', 'V', '1'};

std::vector<std::uint8_t> WithoutTail(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 40 || std::memcmp(bytes.data() + bytes.size() - 40, kFooterMagic, 8) != 0) return bytes;
    std::uint64_t prefix = 0;
    std::memcpy(&prefix, bytes.data() + bytes.size() - 32, 8);
    return std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + std::ptrdiff_t(prefix));
}

// Offset where the standard body ends and the curve tail begins.
std::size_t TailPrefix(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t prefix = 0;
    std::memcpy(&prefix, bytes.data() + bytes.size() - 32, 8);
    return std::size_t(prefix);
}

std::string DecodeError(const std::vector<std::uint8_t>& bytes, cm::Kind kind, cm::Motion& out) {
    std::string error;
    cm::Decode(bytes.data(), bytes.size(), kind, out, error);
    return error;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void TestRoundTripEveryKind() {
    const cm::Kind kinds[] = {cm::Kind::Camera, cm::Kind::Light, cm::Kind::Shadow, cm::Kind::Gravity,
                              cm::Kind::Accessory, cm::Kind::Effect, cm::Kind::Morph};
    for (cm::Kind kind : kinds) {
        const cm::Motion motion = MotionFor(kind);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(cm::Encode(motion, bytes, error), "encode succeeds for kind: " + error);
        Check(bytes.size() > 40 && std::memcmp(bytes.data() + bytes.size() - 40, kFooterMagic, 8) == 0,
              "the encoded file ends with the curve footer");
        BodyCounts counts;
        std::string parseError;
        Check(ParseBody(WithoutTail(bytes), counts, parseError),
              "the standard body parses on its own: " + parseError);
        Check(counts.end == WithoutTail(bytes).size(),
              "the standard body ends exactly where the tail starts");
        Check(counts.ikMasters == 0 && counts.hasIkSection,
              "the display/IK section is present and complete with count 0");

        cm::Motion decoded;
        Check(cm::Decode(bytes.data(), bytes.size(), kind, decoded, error),
              "decode succeeds for kind: " + error);
        Check(SameTracks(decoded, motion), "the tail restores every track exactly");

        for (cm::Kind other : kinds) {
            if (other == kind) continue;
            cm::Motion wrong;
            wrong.kind = cm::Kind::Camera;
            const bool ok = cm::Decode(bytes.data(), bytes.size(), other, wrong, error);
            Check(!ok && wrong.kind == cm::Kind::Camera,
                  "selecting a different motion type is refused and leaves the result untouched");
        }
    }
}

void TestStandardMarkers() {
    const cm::Kind kinds[] = {cm::Kind::Camera, cm::Kind::Light, cm::Kind::Shadow, cm::Kind::Accessory,
                              cm::Kind::Morph};
    for (cm::Kind kind : kinds) {
        const cm::Motion motion = MotionFor(kind);
        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(cm::Encode(motion, bytes, error), "encode succeeds: " + error);
        BodyCounts counts;
        std::string parseError;
        Check(ParseBody(WithoutTail(bytes), counts, parseError), "the body parses: " + parseError);
        const bool cameraFamily =
            kind == cm::Kind::Camera || kind == cm::Kind::Light || kind == cm::Kind::Shadow;
        const std::string expected =
            cameraFamily ? std::string("\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe") : std::string("MMV motion");
        Check(counts.modelName == expected,
              std::string("the model-name marker matches the source: ") +
                  (cameraFamily ? "camera family" : "curve family"));
    }
}

void TestSectionRouting() {
    std::string error;
    {
        const cm::Motion motion = MotionFor(cm::Kind::Camera);
        std::vector<std::uint8_t> bytes;
        Check(cm::Encode(motion, bytes, error), "camera encodes: " + error);
        BodyCounts counts;
        std::string parseError;
        Check(ParseBody(WithoutTail(bytes), counts, parseError), "camera body parses: " + parseError);
        Check(counts.cameras == 13 && counts.bones == 0 && counts.morphs == 0,
              "camera writes only the native camera section, one record per frame");
    }
    {
        const cm::Motion motion = MotionFor(cm::Kind::Light);
        std::vector<std::uint8_t> bytes;
        Check(cm::Encode(motion, bytes, error), "light encodes: " + error);
        BodyCounts counts;
        std::string parseError;
        Check(ParseBody(WithoutTail(bytes), counts, parseError), "light body parses: " + parseError);
        Check(counts.lights == 13 && counts.cameras == 0, "light writes its native section");
    }
    {
        const cm::Motion motion = MotionFor(cm::Kind::Shadow);
        std::vector<std::uint8_t> bytes;
        Check(cm::Encode(motion, bytes, error), "shadow encodes: " + error);
        BodyCounts counts;
        std::string parseError;
        Check(ParseBody(WithoutTail(bytes), counts, parseError), "shadow body parses: " + parseError);
        Check(counts.shadows == 13 && counts.lights == 0, "self shadow writes its native section");
    }
    {
        const cm::Motion motion = MotionFor(cm::Kind::Accessory);
        std::vector<std::uint8_t> bytes;
        Check(cm::Encode(motion, bytes, error), "accessory encodes: " + error);
        BodyCounts counts;
        std::string parseError;
        Check(ParseBody(WithoutTail(bytes), counts, parseError), "accessory body parses: " + parseError);
        Check(counts.bones == 13, "accessory writes one bone proxy per frame");
        Check(counts.morphs == 13 * 3, "accessory writes morph proxies for its remaining channels");
        Check(counts.cameras == 0 && counts.lights == 0 && counts.shadows == 0,
              "accessory writes no camera, light or shadow data");
    }
    {
        const cm::Motion motion = MotionFor(cm::Kind::Effect);
        std::vector<std::uint8_t> bytes;
        Check(cm::Encode(motion, bytes, error), "effect encodes: " + error);
        BodyCounts counts;
        std::string parseError;
        Check(ParseBody(WithoutTail(bytes), counts, parseError), "effect body parses: " + parseError);
        Check(counts.bones == 0 && counts.morphs == 13 * 2, "effect writes morph proxies only");
    }
}

void TestAccessoryProxyMatchesCurve() {
    // The per-frame bone proxy must reproduce the accessory curve at integer
    // frames for a reader that ignores the tail.
    const cm::Motion motion = MotionFor(cm::Kind::Accessory);
    std::vector<std::uint8_t> bytes;
    std::string error;
    Check(cm::Encode(motion, bytes, error), "accessory encodes: " + error);
    const std::vector<std::uint8_t> body = WithoutTail(bytes);
    cm::Motion decoded;
    Check(cm::Decode(body.data(), body.size(), cm::Kind::Accessory, decoded, error),
          "the standard accessory proxy decodes: " + error);
    for (int frame = 0; frame <= 12; ++frame) {
        const ac::Track* x = Find(motion, "x");
        const ac::Track* dx = Find(decoded, "x");
        CheckNear(dx->keys[std::size_t(frame)].value, ac::Evaluate(*x, double(frame)).value, 1e-4,
                  "the bone proxy keeps the X value at frame " + std::to_string(frame));
        const ac::Track* rx = Find(motion, "rx");
        const ac::Track* ry = Find(motion, "ry");
        const ac::Track* rz = Find(motion, "rz");
        const ac::Track* drx = Find(decoded, "rx");
        const ac::Track* dry = Find(decoded, "ry");
        const ac::Track* drz = Find(decoded, "rz");
        const std::array<double, 4> source =
            QuaternionDegrees(ac::Evaluate(*rx, double(frame)).value, ac::Evaluate(*ry, double(frame)).value,
                              ac::Evaluate(*rz, double(frame)).value);
        const std::array<double, 4> restored =
            QuaternionDegrees(drx->keys[std::size_t(frame)].value, dry->keys[std::size_t(frame)].value,
                              drz->keys[std::size_t(frame)].value);
        double dot = 0.0;
        for (int index = 0; index < 4; ++index) dot += source[std::size_t(index)] * restored[std::size_t(index)];
        CheckNear(std::fabs(dot), 1.0, 1e-4,
                  "the bone proxy keeps the rotation at frame " + std::to_string(frame));
    }
}

void TestAccessoryBezierImportVersusExpected() {
    // A two-key bone track with a non-linear lane on every channel.  The
    // expected samples are computed here with an independent bisection solver.
    const double lanes[4][4] = {
        {30.0, 90.0, 100.0, 20.0},   // X
        {10.0, 120.0, 110.0, 5.0},   // Y
        {60.0, 60.0, 70.0, 70.0},    // Z, diagonal on purpose
        {5.0, 110.0, 120.0, 30.0}};  // rotation
    const std::array<double, 3> p0{0.0, 0.0, 0.0};
    const std::array<double, 3> p1{10.0, -4.0, 2.0};
    const std::array<double, 4> q0{0.0, 0.0, 0.0, 1.0};
    const std::array<double, 4> q1 = QuaternionDegrees(20.0, 30.0, 10.0);

    VmdFile file;
    BoneInput first;
    first.name = "Accessory";
    first.frame = 0;
    first.position = p0;
    first.quaternion = q0;
    first.block = BoneBlock(lanes);
    BoneInput second;
    second.name = "Accessory";
    second.frame = 10;
    second.position = p1;
    second.quaternion = q1;
    // Literal file-order bytes, independent of BoneBlock and the decoder's
    // indexing formula. Each row is one channel's four 4-byte control groups.
    second.block = {{
        30,0,0,0, 90,0,0,0, 100,0,0,0, 20,0,0,0,
        10,0,0,0, 120,0,0,0, 110,0,0,0, 5,0,0,0,
        60,0,0,0, 60,0,0,0, 70,0,0,0, 70,0,0,0,
        5,0,0,0, 110,0,0,0, 120,0,0,0, 30,0,0,0}};
    file.bones = {first, second};
    const std::vector<std::uint8_t> bytes = Build(file);

    cm::Motion decoded;
    std::string error;
    Check(cm::Decode(bytes.data(), bytes.size(), cm::Kind::Accessory, decoded, error),
          "a standard accessory file decodes through its bone interpolation: " + error);
    const ac::Track* x = Find(decoded, "x");
    const ac::Track* y = Find(decoded, "y");
    const ac::Track* z = Find(decoded, "z");
    const ac::Track* rx = Find(decoded, "rx");
    const ac::Track* ry = Find(decoded, "ry");
    const ac::Track* rz = Find(decoded, "rz");
    Check(x && y && z && rx && ry && rz, "the accessory import produces all six pose tracks");

    for (int frame = 0; frame <= 10; ++frame) {
        const double u = frame / 10.0;
        const double tx = EaseFraction(lanes[0][0], lanes[0][1], lanes[0][2], lanes[0][3], u);
        const double ty = EaseFraction(lanes[1][0], lanes[1][1], lanes[1][2], lanes[1][3], u);
        const double tz = EaseFraction(lanes[2][0], lanes[2][1], lanes[2][2], lanes[2][3], u);
        const double tr = EaseFraction(lanes[3][0], lanes[3][1], lanes[3][2], lanes[3][3], u);
        CheckNear(x->keys[std::size_t(frame)].value, p0[0] + (p1[0] - p0[0]) * tx, 1e-4,
                  "the X channel follows its own lane at frame " + std::to_string(frame));
        CheckNear(y->keys[std::size_t(frame)].value, p0[1] + (p1[1] - p0[1]) * ty, 1e-4,
                  "the Y channel follows its own lane at frame " + std::to_string(frame));
        CheckNear(z->keys[std::size_t(frame)].value, p0[2] + (p1[2] - p0[2]) * tz, 1e-4,
                  "the Z channel follows its own lane at frame " + std::to_string(frame));
        const std::array<double, 3> expected = EulerDegrees(Slerp(q0, q1, tr));
        CheckNear(rx->keys[std::size_t(frame)].value, expected[0], 1e-3,
                  "the rotation channel slerps with its lane at frame " + std::to_string(frame));
        CheckNear(ry->keys[std::size_t(frame)].value, expected[1], 1e-3,
                  "the rotation channel slerps with its lane at frame " + std::to_string(frame));
        CheckNear(rz->keys[std::size_t(frame)].value, expected[2], 1e-3,
                  "the rotation channel slerps with its lane at frame " + std::to_string(frame));
    }
    // The lane is genuinely non-linear, so a linear import would be wrong.
    Check(std::fabs(x->keys[5].value - (p0[0] + p1[0] * 0.5)) > 0.2,
          "the bone interpolation channel is not ignored");
}

void TestAccessoryUnwrap() {
    // Two keys whose yaw crosses the wrap point: the sampled track must stay
    // continuous instead of jumping by 360 degrees.
    VmdFile file;
    BoneInput first;
    first.name = "Accessory";
    first.frame = 0;
    first.quaternion = QuaternionDegrees(0.0, 170.0, 0.0);
    BoneInput second;
    second.name = "Accessory";
    second.frame = 4;
    second.quaternion = QuaternionDegrees(0.0, 190.0, 0.0);
    file.bones = {first, second};
    const std::vector<std::uint8_t> bytes = Build(file);
    cm::Motion decoded;
    std::string error;
    Check(cm::Decode(bytes.data(), bytes.size(), cm::Kind::Accessory, decoded, error),
          "the wrap-around accessory decodes: " + error);
    const ac::Track* ry = Find(decoded, "ry");
    Check(ry != nullptr && ry->keys.size() == 5, "the wrap-around import samples every frame");
    double biggest = 0.0;
    for (std::size_t index = 1; index < ry->keys.size(); ++index) {
        biggest = std::max(biggest, std::fabs(ry->keys[index].value - ry->keys[index - 1].value));
    }
    Check(biggest < 90.0, "the unwrapped yaw stays continuous, largest step " + std::to_string(biggest));
}

void TestCameraLaneOrder() {
    // One lane is written in the file order X1, X2, Y1, Y2 with a deliberately
    // asymmetric curve; reading it as X1, Y1, X2, Y2 would cross the handles
    // and fall back to linear, so the two readings are far apart.
    double lanes[6][4] = {};
    for (int lane = 0; lane < 6; ++lane) {
        lanes[lane][0] = 20.0;
        lanes[lane][1] = 20.0;
        lanes[lane][2] = 107.0;
        lanes[lane][3] = 107.0;
    }
    lanes[0][0] = 20.0;   // x1
    lanes[0][1] = 5.0;    // y1
    lanes[0][2] = 110.0;  // x2
    lanes[0][3] = 120.0;  // y2

    VmdFile file;
    file.modelName = "\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe";
    CameraInput first;
    first.frame = 0;
    first.eye = {0.0, 5.0, 9.0};
    first.block = CameraBlock(lanes);
    CameraInput second;
    second.frame = 10;
    second.eye = {10.0, 5.0, 9.0};
    second.block = CameraBlock(lanes);
    file.cameras = {first, second};
    const std::vector<std::uint8_t> bytes = Build(file);

    cm::Motion decoded;
    std::string error;
    Check(cm::Decode(bytes.data(), bytes.size(), cm::Kind::Camera, decoded, error),
          "the camera file decodes: " + error);
    const ac::Track* x = Find(decoded, "x");
    Check(x != nullptr && x->keys.size() == 2, "the camera import keeps both keys");
    const double expected = 0.0 + 10.0 * EaseFraction(20.0, 5.0, 110.0, 120.0, 0.5);
    CheckNear(ac::Evaluate(*x, 5.0).value, expected, 1e-6,
              "the camera lane keeps its native X1, X2, Y1, Y2 order");
    Check(std::fabs(ac::Evaluate(*x, 5.0).value - 5.0) > 0.05,
          "a swapped control order would have produced the linear midpoint");
    const ac::Track* y = Find(decoded, "y");
    CheckNear(y->keys[1].value, 5.0, 1e-9, "an unchanged camera channel keeps its value");
}

void TestNativeCameraTimingRoundTrip() {
    for (const std::array<double,4> controls : {std::array<double,4>{100,10,20,115}, {0,127,127,0}}) {
        double lanes[6][4]{};
        for(int lane=0;lane<6;++lane)for(int c=0;c<4;++c)lanes[lane][c]=controls[c];
        VmdFile file;CameraInput first,last;
        first.frame=0;first.fov=45;first.block=CameraBlock(lanes);
        last=first;last.frame=100;last.eye={10,0,0};file.cameras={first,last};
        auto bytes=Build(file);cm::Motion imported;std::string error;
        Check(cm::Decode(bytes.data(),bytes.size(),cm::Kind::Camera,imported,error),"native camera imports: "+error);
        Check(imported.nativeTiming.size()==8,"all native camera lanes retain original timing");
        std::vector<mikumikustudio::curve_editor::Channel> channels;
        for(const auto& named:imported.tracks){mikumikustudio::curve_editor::Channel channel;channel.id=named.id;channels.push_back(std::move(channel));}
        Check(cm::RestoreChannels(imported,channels,error),"restore native camera editor channels: "+error);
        auto captured=cm::CaptureChannels(cm::Kind::Camera,channels);
        Check(cm::Encode(captured,bytes,error),"native timing VMD encodes: "+error);
        Check(std::memcmp(bytes.data()+bytes.size()-40,"MMVCURV2",8)==0,"raw native timing uses the versioned optional tail");
        cm::Motion roundtrip;
        Check(cm::Decode(bytes.data(),bytes.size(),cm::Kind::Camera,roundtrip,error),"native timing tail decodes: "+error);
        Check(roundtrip.nativeTiming.size()==imported.nativeTiming.size(),"native timing count survives transport");
        for(std::size_t i=0;i<roundtrip.nativeTiming.size();++i)
            Check(roundtrip.nativeTiming[i].controls==imported.nativeTiming[i].controls,"crossed or endpoint controls round-trip exactly");
        for(double frame=0;frame<=100;frame+=0.25)
            CheckNear(cm::Evaluate(roundtrip,"x",frame).value,cm::Evaluate(imported,"x",frame).value,0,
                "fractional native camera motion survives the author tail");
        const auto prefix=TailPrefix(bytes);
        std::vector<std::uint8_t> body(bytes.begin(),bytes.begin()+std::ptrdiff_t(prefix));
        cm::Motion compatible;
        Check(cm::Decode(body.data(),body.size(),cm::Kind::Camera,compatible,error),"standard camera proxy remains a complete VMD");
        for(int frame=0;frame<=100;++frame)
            CheckNear(cm::Evaluate(compatible,"x",frame).value,cm::Evaluate(imported,"x",frame).value,1e-6,
                "standard VMD proxy samples actual native camera values");
        auto damaged=bytes;damaged.resize(damaged.size()-40);
        Check(!cm::Decode(damaged.data(),damaged.size(),cm::Kind::Camera,roundtrip,error),"a missing native timing footer is refused");
        damaged=bytes;damaged[prefix+15]^=0x40;
        Check(!cm::Decode(damaged.data(),damaged.size(),cm::Kind::Camera,roundtrip,error),"a corrupt native timing envelope is refused");
        double boneLanes[4][4]{};
        for(int lane=0;lane<4;++lane)for(int c=0;c<4;++c)boneLanes[lane][c]=controls[c];
        VmdFile accessoryFile;BoneInput begin,end;begin.name=end.name="Accessory";
        begin.block=end.block=BoneBlock(boneLanes);end.frame=100;end.position[0]=10;
        accessoryFile.bones={begin,end};auto accessoryBytes=Build(accessoryFile);cm::Motion accessory;
        Check(cm::Decode(accessoryBytes.data(),accessoryBytes.size(),cm::Kind::Accessory,accessory,error),
            "accessory VMD imports crossed and endpoint native controls");
        const auto* x=Find(accessory,"x");
        for(int frame=0;frame<=100;++frame)
            CheckNear(ac::Evaluate(*x,frame).value,frame==0?0:frame==100?10:10*EaseFraction(controls[0],controls[1],controls[2],controls[3],frame/100.0),
                1e-10,"accessory import samples raw native timing rather than dropping it");
    }
    // A native one-frame camera cut must not turn into interpolated movement.
    cm::Motion cut;cut.kind=cm::Kind::Camera;
    ac::Track track;ac::Key a,b;a.frame=0;a.value=0;b.frame=1;b.value=10;
    a.tangent=b.tangent=ac::Tangent::Free;b.interpolation=ac::Interpolation::Bezier;
    track.keys={a,b};cut.tracks.push_back({"x",track});
    cut.nativeTiming.push_back({"x",1,{20,20,107,107},true});
    CheckNear(cm::Evaluate(cut,"x",0.5).value,0,0,"one-frame native camera cut holds the source value");
    cut.nativeTiming[0].oneFrameHold=false;
    CheckNear(cm::Evaluate(cut,"x",0.5).value,5,1e-6,"subdivided one-frame camera interval keeps continuous motion");
}

void TestStandardBodyOptionalSections() {
    // After the morph section the rest may be missing: old VMD 0001 files and
    // writers that stop early must still load.
    double lanes[4][4] = {};
    for (int channel = 0; channel < 4; ++channel) {
        lanes[channel][0] = 20.0;
        lanes[channel][1] = 20.0;
        lanes[channel][2] = 107.0;
        lanes[channel][3] = 107.0;
    }
    VmdFile file;
    file.sectionsAfterMorphs = false;
    BoneInput bone;
    bone.name = "Accessory";
    bone.frame = 3;
    bone.block = BoneBlock(lanes);
    file.bones = {bone};
    const std::vector<std::uint8_t> bytes = Build(file);
    cm::Motion decoded;
    std::string error;
    Check(cm::Decode(bytes.data(), bytes.size(), cm::Kind::Accessory, decoded, error),
          "a file that stops after the morph section loads: " + error);
    Check(decoded.tracks.size() == 6, "the truncated standard file still yields the six pose tracks");

    VmdFile legacy = file;
    legacy.legacy = true;
    const std::vector<std::uint8_t> legacyBytes = Build(legacy);
    cm::Motion legacyDecoded;
    Check(cm::Decode(legacyBytes.data(), legacyBytes.size(), cm::Kind::Accessory, legacyDecoded, error),
          "a VMD 0001 file loads: " + error);
    Check(legacyDecoded.tracks.size() == 6, "the VMD 0001 file yields the pose tracks");
}

void TestForeignTailIgnored() {
    double lanes[4][4] = {};
    for (int channel = 0; channel < 4; ++channel) {
        lanes[channel][0] = 20.0;
        lanes[channel][1] = 20.0;
        lanes[channel][2] = 107.0;
        lanes[channel][3] = 107.0;
    }
    VmdFile file;
    BoneInput bone;
    bone.name = "Accessory";
    bone.frame = 0;
    bone.position = {1.0, 2.0, 3.0};
    bone.block = BoneBlock(lanes);
    file.bones = {bone};
    const std::vector<std::uint8_t> plain = Build(file);
    cm::Motion expected;
    std::string error;
    Check(cm::Decode(plain.data(), plain.size(), cm::Kind::Accessory, expected, error),
          "the plain file decodes: " + error);

    VmdFile withTail = file;
    withTail.trailing.assign({'M', 'D', 'S', 'X', 'V', 'M', 'D', 'X'});
    withTail.trailing.resize(80, 0x5a);
    const std::vector<std::uint8_t> bytes = Build(withTail);
    cm::Motion decoded;
    Check(cm::Decode(bytes.data(), bytes.size(), cm::Kind::Accessory, decoded, error),
          "an unrelated MDSXVMDX tail is ignored: " + error);
    Check(SameTracks(decoded, expected), "the unrelated tail does not change the standard data");
}

void TestMultipleBoneTracksRejected() {
    double lanes[4][4] = {};
    for (int channel = 0; channel < 4; ++channel) {
        lanes[channel][0] = 20.0;
        lanes[channel][1] = 20.0;
        lanes[channel][2] = 107.0;
        lanes[channel][3] = 107.0;
    }
    VmdFile file;
    BoneInput first;
    first.name = "Accessory";
    first.frame = 0;
    first.block = BoneBlock(lanes);
    BoneInput second;
    second.name = "Accessory";
    second.frame = 5;
    second.block = BoneBlock(lanes);
    BoneInput other;
    other.name = "center";
    other.frame = 0;
    other.block = BoneBlock(lanes);
    file.bones = {first, second, other};
    const std::vector<std::uint8_t> bytes = Build(file);

    cm::Motion untouched;
    untouched.kind = cm::Kind::Morph;
    untouched.tracks.push_back({"sentinel", MakeTrack({{0, 1.0}})});
    std::string error;
    const bool ok = cm::Decode(bytes.data(), bytes.size(), cm::Kind::Accessory, untouched, error);
    Check(!ok, "a file with two bone tracks is refused");
    Check(error.find("bone tracks") != std::string::npos,
          "the refusal explains that a bone track must be chosen, got \"" + error + "\"");
    Check(untouched.kind == cm::Kind::Morph && untouched.tracks.size() == 1 &&
              untouched.tracks[0].id == "sentinel",
          "the refused import leaves the result untouched");

    // Repeating one bone name is a single track and stays supported.
    VmdFile single;
    single.bones = {first, second};
    const std::vector<std::uint8_t> singleBytes = Build(single);
    cm::Motion decoded;
    Check(cm::Decode(singleBytes.data(), singleBytes.size(), cm::Kind::Accessory, decoded, error),
          "one bone track split over several keys still loads: " + error);
}

void TestCapacityLimits() {
    double lanes[4][4] = {};
    for (int channel = 0; channel < 4; ++channel) {
        lanes[channel][0] = 20.0;
        lanes[channel][1] = 20.0;
        lanes[channel][2] = 107.0;
        lanes[channel][3] = 107.0;
    }
    VmdFile file;
    BoneInput first;
    first.name = "Accessory";
    first.frame = 0;
    first.block = BoneBlock(lanes);
    BoneInput second;
    second.name = "Accessory";
    second.frame = 2147483647;
    second.block = BoneBlock(lanes);
    file.bones = {first, second};
    const std::vector<std::uint8_t> bytes = Build(file);
    cm::Motion decoded;
    std::string error;
    const bool ok = cm::Decode(bytes.data(), bytes.size(), cm::Kind::Accessory, decoded, error);
    Check(!ok, "sampling beyond the supported signed frame range is refused");
    Check(decoded.tracks.empty(), "the refused sampling leaves the result untouched");

    cm::Motion wide;
    wide.kind = cm::Kind::Accessory;
    wide.tracks.push_back({"x", MakeTrack({{0, 0.0}, {2147483647, 1.0}})});
    std::vector<std::uint8_t> out;
    Check(!cm::Encode(wide, out, error), "a proxy beyond the supported frame range is refused");
    Check(out.empty(), "the refused encode leaves the buffer untouched");
}

void TestCorruptTailIsRejected() {
    const cm::Motion motion = MotionFor(cm::Kind::Accessory);
    std::vector<std::uint8_t> bytes;
    std::string error;
    Check(cm::Encode(motion, bytes, error), "the corruption fixture encodes: " + error);

    const auto refused = [&](const std::vector<std::uint8_t>& data, const std::string& text) {
        cm::Motion untouched;
        untouched.kind = cm::Kind::Morph;
        untouched.tracks.push_back({"sentinel", MakeTrack({{0, 1.0}})});
        std::string reason;
        const bool ok = cm::Decode(data.data(), data.size(), cm::Kind::Accessory, untouched, reason);
        Check(!ok, text + ": the damaged file must be refused");
        Check(untouched.kind == cm::Kind::Morph && untouched.tracks.size() == 1,
              text + ": the refused decode leaves the result untouched");
    };

    const std::size_t footer = bytes.size() - 40;
    const std::size_t bodyEnd = TailPrefix(bytes);
    refused({}, "an empty buffer");
    refused(std::vector<std::uint8_t>(20, 0), "a buffer below the header size");

    std::vector<std::uint8_t> truncated(bytes.begin(), bytes.end() - 1);
    refused(truncated, "a file truncated by one byte");
    std::vector<std::uint8_t> shortBody(bytes.begin(), bytes.begin() + std::ptrdiff_t(bodyEnd - 3));
    refused(shortBody, "a file truncated inside the standard body");
    std::vector<std::uint8_t> shortPayload(
        bytes.begin(), bytes.begin() + std::ptrdiff_t(bodyEnd + (bytes.size() - 40 - bodyEnd) / 2));
    refused(shortPayload, "a file truncated inside the curve payload");

    std::vector<std::uint8_t> trailing = bytes;
    trailing.push_back(0);
    refused(trailing, "one appended byte after the footer");

    std::vector<std::uint8_t> badPrefixCrc = bytes;
    badPrefixCrc[footer + 24] ^= 0x01;
    refused(badPrefixCrc, "a damaged standard-body checksum");

    std::vector<std::uint8_t> badCurveCrc = bytes;
    badCurveCrc[footer + 28] ^= 0x01;
    refused(badCurveCrc, "a damaged curve checksum");

    std::vector<std::uint8_t> badPrefix = bytes;
    badPrefix[footer + 8] ^= 0x01;
    refused(badPrefix, "a tail that does not start at the end of the standard data");

    std::vector<std::uint8_t> badLength = bytes;
    badLength[footer + 16] ^= 0x01;
    refused(badLength, "a tail whose payload length is wrong");

    std::vector<std::uint8_t> badReserved = bytes;
    badReserved[bytes.size() - 1] = 0x01;
    refused(badReserved, "a non-zero reserved footer field");

    std::vector<std::uint8_t> wrongKind = bytes;
    wrongKind[footer + 32] = 1;  // the fixture is an accessory motion
    refused(wrongKind, "a footer that claims another motion type");

    std::vector<std::uint8_t> damagedBody = bytes;
    damagedBody[70] ^= 0x40;  // inside the first bone record, after the header
    refused(damagedBody, "a standard body that no longer matches its checksum");

    std::vector<std::uint8_t> badSignature = bytes;
    badSignature[0] = 'X';
    refused(badSignature, "an unknown file signature");

    std::vector<std::uint8_t> badCount;
    badCount = WithoutTail(bytes);
    badCount[30 + 20] = 0xff;
    badCount[30 + 21] = 0xff;
    badCount[30 + 22] = 0xff;
    badCount[30 + 23] = 0xff;
    refused(badCount, "a bone count far beyond the file size");
}

void TestProxyNamesWithoutTail() {
    cm::Motion motion;
    motion.kind = cm::Kind::Effect;
    motion.tracks.push_back({"bloom", MakeTrack({{0, 0.0}, {5, 1.0}})});
    motion.tracks.push_back({"\u975e\u5e38\u306b\u9577\u3044\u540d\u524d\u306e\u30a8\u30d5\u30a7\u30af\u30c8",
                             MakeTrack({{0, 0.0}, {5, 1.0}})});
    std::vector<std::uint8_t> bytes;
    std::string error;
    Check(cm::Encode(motion, bytes, error), "the proxy fixture encodes: " + error);

    cm::Motion withTail;
    Check(cm::Decode(bytes.data(), bytes.size(), cm::Kind::Effect, withTail, error),
          "the proxy fixture decodes with its tail: " + error);
    Check(SameTracks(withTail, motion), "the tail restores the real parameter ids");

    const std::vector<std::uint8_t> body = WithoutTail(bytes);
    cm::Motion proxy;
    Check(cm::Decode(body.data(), body.size(), cm::Kind::Effect, proxy, error),
          "the standard body decodes without the tail: " + error);
    bool foundAscii = false;
    bool foundPlaceholder = false;
    for (const ac::NamedTrack& named : proxy.tracks) {
        if (named.id == "bloom") foundAscii = true;
        if (named.id.rfind("mmv_", 0) == 0) foundPlaceholder = true;
        Check(named.id != "\u975e\u5e38\u306b\u9577\u3044\u540d\u524d\u306e\u30a8\u30d5\u30a7\u30af\u30c8",
              "a name that does not fit the proxy field is never guessed back into an id");
    }
    Check(foundAscii, "a short ASCII parameter id survives the proxy verbatim");
    Check(foundPlaceholder, "a long parameter id becomes a checksum placeholder");
}

void TestFileRoundTripWithUnicodePath() {
    wchar_t temporary[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temporary) == 0) {
        Check(false, "the temporary directory is available");
        return;
    }
    const std::wstring directory = std::wstring(temporary) + L"\u66f2\u7ebf\u6d4b\u8bd5";
    CreateDirectoryW(directory.c_str(), nullptr);
    const std::wstring path = directory + L"\\\u52a8\u4f5c.vmd";

    const cm::Motion motion = MotionFor(cm::Kind::Camera);
    std::string error;
    Check(cm::Save(path.c_str(), motion, error), "the motion saves to a unicode path: " + error);
    Check(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES, "the saved file exists");

    cm::Motion loaded;
    Check(cm::Load(path.c_str(), cm::Kind::Camera, loaded, error), "the motion loads back: " + error);
    Check(SameTracks(loaded, motion), "the file round trip preserves every track");

    // A refused export must not touch the file that is already there.
    cm::Motion broken;
    broken.kind = cm::Kind::Camera;
    broken.tracks.push_back({"", MakeTrack({{0, 0.0}})});
    Check(!cm::Save(path.c_str(), broken, error), "an invalid motion is refused before writing");
    cm::Motion again;
    Check(cm::Load(path.c_str(), cm::Kind::Camera, again, error), "the existing file is still readable");
    Check(SameTracks(again, motion), "the refused export preserved the existing file");

    DeleteFileW(path.c_str());
    RemoveDirectoryW(directory.c_str());
}

void TestDefaultInterpolationBytes() {
    // The accessory proxy must write the byte pattern the original writes for a
    // default bone key, and that pattern must read back as a diagonal (linear)
    // lane on every channel.
    const cm::Motion motion = MotionFor(cm::Kind::Accessory);
    std::vector<std::uint8_t> bytes;
    std::string error;
    Check(cm::Encode(motion, bytes, error), "the default-pattern fixture encodes: " + error);
    const std::vector<std::uint8_t> body = WithoutTail(bytes);
    const std::size_t blockOffset = 50 + 4 + 15 + 4 + 12 + 16;  // header, bone count, name, frame, pose
    bool pattern = true;
    for (int index = 0; index < 64; ++index) {
        const std::uint8_t expected = (index % 16) < 8 ? 20 : 107;
        if (blockOffset + std::size_t(index) >= body.size() ||
            body[blockOffset + std::size_t(index)] != expected) {
            pattern = false;
        }
    }
    Check(pattern, "the accessory proxy writes the original default bone interpolation bytes");

    std::array<std::uint8_t, 64> classic{};
    for (int index = 0; index < 64; ++index) classic[std::size_t(index)] = (index % 16) < 8 ? 20 : 107;
    VmdFile file;
    BoneInput first;
    first.name = "Accessory";
    first.frame = 0;
    first.block = classic;
    BoneInput second;
    second.name = "Accessory";
    second.frame = 10;
    second.position = {10.0, 0.0, 0.0};
    second.block = classic;
    file.bones = {first, second};
    const std::vector<std::uint8_t> classicFile = Build(file);
    cm::Motion decoded;
    Check(cm::Decode(classicFile.data(), classicFile.size(), cm::Kind::Accessory, decoded, error),
          "the classic default bone pattern decodes: " + error);
    const ac::Track* x = Find(decoded, "x");
    Check(x != nullptr, "the classic default bone pattern yields the X track");
    if (x != nullptr) {
        CheckNear(x->keys[5].value, 5.0, 1e-6,
                  "the original default bone pattern is the linear diagonal curve");
    }

    // The camera default store is 20, 107, 20, 107 per lane, the same diagonal
    // curve expressed in the camera control order.
    double lanes[6][4];
    for (int lane = 0; lane < 6; ++lane) {
        lanes[lane][0] = 20.0;
        lanes[lane][1] = 20.0;
        lanes[lane][2] = 107.0;
        lanes[lane][3] = 107.0;
    }
    VmdFile cameraFile;
    cameraFile.modelName = "\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe";
    CameraInput camera0;
    camera0.frame = 0;
    camera0.block = CameraBlock(lanes);
    CameraInput camera1;
    camera1.frame = 10;
    camera1.eye = {10.0, 0.0, 0.0};
    camera1.block = CameraBlock(lanes);
    cameraFile.cameras = {camera0, camera1};
    const std::vector<std::uint8_t> cameraBytes = Build(cameraFile);
    cm::Motion cameraDecoded;
    Check(cm::Decode(cameraBytes.data(), cameraBytes.size(), cm::Kind::Camera, cameraDecoded, error),
          "the camera default file decodes: " + error);
    const ac::Track* cameraX = Find(cameraDecoded, "x");
    Check(cameraX != nullptr, "the camera default file yields the X track");
    if (cameraX != nullptr) {
        CheckNear(ac::Evaluate(*cameraX, 5.0).value, 5.0, 1e-6,
                  "the original default camera lane is the linear diagonal curve");
    }

    const cm::Motion cameraSource = MotionFor(cm::Kind::Camera);
    std::vector<std::uint8_t> encoded;
    Check(cm::Encode(cameraSource, encoded, error), "the camera proxy encodes: " + error);
    const std::vector<std::uint8_t> cameraBody = WithoutTail(encoded);
    const std::size_t laneOffset = 50 + 4 + 4 + 4 + 4 + 4 + 12 + 12;  // header through the first record's target
    bool cameraPattern = true;
    for (int lane = 0; lane < 6; ++lane) {
        const std::size_t base = laneOffset + std::size_t(lane) * 4;
        if (base + 3 >= cameraBody.size() || cameraBody[base] != 20 || cameraBody[base + 1] != 107 ||
            cameraBody[base + 2] != 20 || cameraBody[base + 3] != 107) {
            cameraPattern = false;
        }
    }
    Check(cameraPattern, "the camera proxy writes the original default lane bytes");
}

}  // namespace

int main() {
    TestRoundTripEveryKind();
    TestStandardMarkers();
    TestSectionRouting();
    TestAccessoryProxyMatchesCurve();
    TestAccessoryBezierImportVersusExpected();
    TestAccessoryUnwrap();
    TestCameraLaneOrder();
    TestNativeCameraTimingRoundTrip();
    TestStandardBodyOptionalSections();
    TestForeignTailIgnored();
    TestMultipleBoneTracksRejected();
    TestCapacityLimits();
    TestCorruptTailIsRejected();
    TestDefaultInterpolationBytes();
    TestProxyNamesWithoutTail();
    TestFileRoundTripWithUnicodePath();
    if (g_failures == 0) {
        std::printf("animation_curve_motion_tests: all checks passed\n");
        return 0;
    }
    std::printf("animation_curve_motion_tests: %d check(s) failed\n", g_failures);
    return 1;
}
