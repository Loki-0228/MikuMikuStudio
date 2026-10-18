#include "mikudancestudio/frame_limits.hpp"
// MAINTENANCE: VMD transport for the animation curve editor.
//
// File layout this module writes
// ------------------------------
//   1. "Vocaloid Motion Data 0002" signature (30 bytes) + model name (20 bytes)
//   2. bone section        : u32 count, count * 111 bytes
//   3. morph section       : u32 count, count *  23 bytes
//   4. camera section      : u32 count, count *  61 bytes
//   5. light section       : u32 count, count *  28 bytes
//   6. self-shadow section : u32 count, count *   9 bytes
//   7. display/IK section  : u32 count, count * (9 bytes + 21 per IK entry)
//   8. optional curve payload followed by a 40-byte MMVCURV1 / MMVCURV2 footer
//      V1 holds the scalar codec; V2 wraps it in MDSN with exact camera timing.
//      Both preserve the standard VMD body and checksum the prefix/payload.
//
// Every standard section is written, in that order, before the tail, so a
// reader that knows nothing about the curve extension still sees a complete
// VMD whose per-frame proxy keys reproduce the motion at integer frames.
// Camera, light and self-shadow export into their native sections; accessory
// exports one "Accessory" bone record per frame (position plus quaternion)
// and morph proxies for its remaining channels; gravity, effect and morph
// export through morph proxies only.
//
// MMD interpolation byte layouts (pinned against this port's own loader,
// saver and camera ease evaluator in src/model/vmd_load.cpp,
// src/io/vmd_save.cpp and src/app/timeline_advance.cpp):
//   * bone: control g (0 x1, 1 y1, 2 x2, 3 y2) of channel ch (0 X, 1 Y, 2 Z,
//     3 rotation) sits at byte 16*ch + 4*g of the 64-byte block.  A default
//     MMD key is therefore eight 20s followed by eight 107s, twice.
//   * camera: each of the six lanes (X, Y, Z, rotation, distance, field of
//     view) stores its controls in the order X1, X2, Y1, Y2, so the default
//     (x1 = 20, y1 = 20, x2 = 107, y2 = 107) is written 20, 107, 20, 107.
// Reading either layout in the wrong order turns X2 into Y1 and silently
// changes the motion, which is why both are spelled out by name below.
//
// A standard VMD without the tail is imported as follows: accessory reads the
// four MMD interpolation channels of its single bone track, samples the pose
// at every integer frame (quaternion slerp driven by the rotation channel,
// Euler angles unwrapped across frames) and rejects a file that holds more
// than one bone track instead of guessing which one to use.  Camera keeps the
// native semantics of its six control lanes.  Morph and effect names that do
// not fit the 15-byte proxy field are stored as a checksum placeholder;
// without a tail that placeholder is kept verbatim rather than guessed back
// into an id.  Later standard sections may be absent (VMD 0001 files and
// writers that stop early); a curve tail is only accepted after the complete
// standard data it declares.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "mikudancestudio/animation_curve_motion.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace mikudancestudio::curve_motion {
namespace {

namespace ac = animation_curve;

constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kMaximumBytes = std::size_t((std::numeric_limits<std::ptrdiff_t>::max)());
constexpr std::size_t kMaximumRecords = (std::numeric_limits<std::uint32_t>::max)();
constexpr std::uint32_t kMaximumFrame = std::uint32_t(mikudancestudio::frame_limits::maximum);
constexpr std::size_t kFooterBytes = 40;
constexpr std::size_t kSignatureBytes = 30;
constexpr std::size_t kModelNameBytes = 20;
constexpr std::size_t kLegacyNameBytes = 10;
constexpr std::size_t kBoneRecordBytes = 111;
constexpr std::size_t kMorphRecordBytes = 23;
constexpr std::size_t kCameraRecordBytes = 61;
constexpr std::size_t kLightRecordBytes = 28;
constexpr std::size_t kShadowRecordBytes = 9;
constexpr std::size_t kIkMasterBytes = 9;
constexpr std::size_t kIkEntryBytes = 21;
constexpr char kFooterMagic[8] = {'M', 'M', 'V', 'C', 'U', 'R', 'V', '1'};
constexpr char kNativeFooterMagic[8] = {'M', 'M', 'V', 'C', 'U', 'R', 'V', '2'};
constexpr char kVmd2Signature[] = "Vocaloid Motion Data 0002";
constexpr char kVmd1Signature[] = "Vocaloid Motion Data file";
// "カメラ・照明" in Shift-JIS: the marker the original camera/light/shadow
// motion files carry in the VMD 0002 model-name field.
constexpr char kCameraLightName[] = "\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe";
constexpr char kCurveModelName[] = "MMV motion";

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = ~0u;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & std::uint32_t(-int(crc & 1)));
        }
    }
    return ~crc;
}

struct Writer {
    std::vector<std::uint8_t> bytes;

    void u8(std::uint8_t value) { bytes.push_back(value); }

    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) u8(std::uint8_t(value >> shift));
    }

    void u64(std::uint64_t value) {
        u32(std::uint32_t(value));
        u32(std::uint32_t(value >> 32));
    }

    void f64(double value) {
        std::uint64_t bits = 0; std::memcpy(&bits, &value, sizeof(bits)); u64(bits);
    }

    // The original stores every motion value as a 32-bit float.
    void f(double value) {
        if (!std::isfinite(value) || !std::isfinite(float(value))) {
            throw std::runtime_error("a motion value is not a finite VMD number");
        }
        const float narrowed = float(value);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &narrowed, sizeof(bits));
        u32(bits);
    }

    void raw(const void* data, std::size_t size) {
        const auto* first = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), first, first + size);
    }

    void text(const std::string& text, std::size_t size) {
        const std::size_t used = std::min(text.size(), size);
        raw(text.data(), used);
        bytes.resize(bytes.size() + (size - used), 0);
    }
};

struct Reader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;

    std::size_t remaining() const { return size - pos; }

    void need(std::size_t bytes) const {
        if (bytes > size - pos) throw std::runtime_error("the VMD is truncated");
    }

    std::uint8_t u8() {
        need(1);
        return data[pos++];
    }

    std::uint32_t u32() {
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8) value |= std::uint32_t(u8()) << shift;
        return value;
    }

    std::uint64_t u64() {
        const std::uint64_t low = u32();
        return low | (std::uint64_t(u32()) << 32);
    }

    double f() {
        const std::uint32_t bits = u32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value)) throw std::runtime_error("a VMD value is not finite");
        return double(value);
    }

    double f64() {
        const auto bits = u64(); double value = 0; std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value)) throw std::runtime_error("a native timing value is not finite");
        return value;
    }

    std::string text(std::size_t bytes) {
        need(bytes);
        std::string value(reinterpret_cast<const char*>(data + pos), bytes);
        pos += bytes;
        const std::size_t end = value.find('\0');
        if (end != std::string::npos) value.resize(end);
        return value;
    }

    void skip(std::size_t bytes) {
        need(bytes);
        pos += bytes;
    }

    // Reads a record count and rejects one whose records cannot fit; the
    // multiplication is done in 64 bits so no count can wrap the bound.
    std::uint32_t count(std::size_t recordBytes) {
        const std::uint32_t value = u32();
        if (value > kMaximumRecords || std::uint64_t(value) * recordBytes > remaining()) {
            throw std::runtime_error("the VMD declares an impossible record count");
        }
        return value;
    }
};

int CheckedFrame(std::uint32_t frame) {
    if (frame > kMaximumFrame) {
        throw std::runtime_error("frame " + std::to_string(frame) + " is outside the supported range");
    }
    return int(frame);
}

std::uint32_t ToRecordInteger(double value, double minimum, double maximum, const char* what) {
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        throw std::runtime_error(std::string("the ") + what + " value cannot be stored in a VMD record");
    }
    return std::uint32_t(std::llround(value));
}

// ---------------------------------------------------------------------------
// MMD interpolation layouts
// ---------------------------------------------------------------------------

std::array<double, 4> BoneChannel(const std::uint8_t block[64], int channel) {
    return {double(block[16 * channel]), double(block[16 * channel + 4]),
            double(block[16 * channel + 8]), double(block[16 * channel + 12])};
}

// Stored X1, X2, Y1, Y2; returned x1, y1, x2, y2.
std::array<double, 4> CameraChannel(const std::uint8_t lane[4]) {
    return {double(lane[0]), double(lane[2]), double(lane[1]), double(lane[3])};
}

void WriteDefaultBoneInterpolation(Writer& writer) {
    for (int index = 0; index < 64; ++index) writer.u8((index % 16) < 8 ? 20 : 107);
}

void WriteDefaultCameraLanes(Writer& writer) {
    for (int lane = 0; lane < 6; ++lane) {
        writer.u8(20);
        writer.u8(107);
        writer.u8(20);
        writer.u8(107);
    }
}

// ---------------------------------------------------------------------------
// Quaternions
// ---------------------------------------------------------------------------

std::array<double, 4> NormalizeQuaternion(std::array<double, 4> value) {
    double norm = 0.0;
    for (double component : value) norm += component * component;
    if (!(norm > 1e-15)) throw std::runtime_error("a bone track holds a zero quaternion");
    const double inverse = 1.0 / std::sqrt(norm);
    for (double& component : value) component *= inverse;
    return value;
}

// DirectX yaw (Y), pitch (X), roll (Z) from degrees, the convention the
// original uses for a bone's stored rotation.
std::array<double, 4> QuaternionDegrees(double x, double y, double z) {
    x *= kPi / 360.0;
    y *= kPi / 360.0;
    z *= kPi / 360.0;
    const double cx = std::cos(x), sx = std::sin(x);
    const double cy = std::cos(y), sy = std::sin(y);
    const double cz = std::cos(z), sz = std::sin(z);
    return {cy * sx * cz + sy * cx * sz, sy * cx * cz - cy * sx * sz, cy * cx * sz - sy * sx * cz,
            cy * cx * cz + sy * sx * sz};
}

std::array<double, 3> EulerDegrees(const std::array<double, 4>& input) {
    const std::array<double, 4> q = NormalizeQuaternion(input);
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double pitch = std::asin(std::clamp(2.0 * (w * x - y * z), -1.0, 1.0));
    double yaw = 0.0;
    double roll = 0.0;
    if (std::fabs(std::cos(pitch)) < 1e-7) {
        yaw = 2.0 * std::atan2(y, w);
    } else {
        yaw = std::atan2(2.0 * (w * y + x * z), 1.0 - 2.0 * (x * x + y * y));
        roll = std::atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (x * x + z * z));
    }
    return {pitch * 180.0 / kPi, yaw * 180.0 / kPi, roll * 180.0 / kPi};
}

std::array<double, 4> Slerp(std::array<double, 4> start, std::array<double, 4> end, double t) {
    double dot = start[0] * end[0] + start[1] * end[1] + start[2] * end[2] + start[3] * end[3];
    if (dot < 0.0) {
        for (double& component : end) component = -component;
        dot = -dot;
    }
    if (dot > 0.9995) {
        std::array<double, 4> blended{};
        for (std::size_t index = 0; index < blended.size(); ++index) {
            blended[index] = start[index] + (end[index] - start[index]) * t;
        }
        return NormalizeQuaternion(blended);
    }
    const double theta = std::acos(std::clamp(dot, -1.0, 1.0));
    const double sine = std::sin(theta);
    const double startWeight = std::sin((1.0 - t) * theta) / sine;
    const double endWeight = std::sin(t * theta) / sine;
    std::array<double, 4> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = start[index] * startWeight + end[index] * endWeight;
    }
    return NormalizeQuaternion(result);
}

// Keeps a sampled angle in the branch nearest the previous sample so the track
// stays continuous when the motion crosses a wrap point.  Closed form of
// "subtract / add 360 until within 180": a stepping loop never ends once the
// value is so large that 360 no longer changes it.
void Unwrap(double& value, double previous) {
    const double delta = value - previous;
    if (!std::isfinite(delta)) return;
    if (delta > 180.0) value -= 360.0 * std::ceil((delta - 180.0) / 360.0);
    else if (delta < -180.0) value += 360.0 * std::ceil((-180.0 - delta) / 360.0);
}

// ---------------------------------------------------------------------------
// Track building
// ---------------------------------------------------------------------------

using Tracks = std::map<std::string, ac::Track>;

ac::Key MakeLinearKey(std::int32_t frame, double value) {
    ac::Key key;
    key.frame = frame;
    key.value = value;
    key.tangent = ac::Tangent::Free;
    key.interpolation = ac::Interpolation::Linear;
    return key;
}

void AddKey(Tracks& tracks, const std::string& id, std::int32_t frame, double value, bool discrete) {
    ac::Track& track = tracks[id];
    if (discrete) track.discrete = true;
    ac::Key key;
    key.frame = frame;
    key.value = value;
    key.tangent = ac::Tangent::Free;
    key.interpolation = track.discrete ? ac::Interpolation::Step : ac::Interpolation::Linear;
    track.keys.push_back(key);
}

// Rejects repeated frames (the original keeps the last pose written for a
// frame) and puts every track into frame order.
void NormalizeTracks(Tracks& tracks) {
    for (auto& entry : tracks) {
        ac::Track& track = entry.second;
        std::stable_sort(track.keys.begin(), track.keys.end(),
                         [](const ac::Key& left, const ac::Key& right) { return left.frame < right.frame; });
        std::vector<ac::Key> unique;
        unique.reserve(track.keys.size());
        for (const ac::Key& key : track.keys) {
            if (!unique.empty() && unique.back().frame == key.frame) {
                unique.back() = key;
            } else {
                unique.push_back(key);
            }
        }
        track.keys.swap(unique);
        std::string reason;
        if (!ac::Validate(track, reason)) throw std::runtime_error(reason);
    }
}

bool IsAccessoryBoneChannel(const std::string& id) {
    return id == "x" || id == "y" || id == "z" || id == "rx" || id == "ry" || id == "rz";
}

// Proxy names: an id that fits the 15-byte field is stored verbatim, anything
// else becomes a checksum placeholder.  The placeholder is never decoded back
// into an id, so a file without a curve tail cannot invent one.
std::string ProxyName(const std::string& id) {
    const bool fits = !id.empty() && id.size() <= 15 &&
                      std::all_of(id.begin(), id.end(), [](char c) { return c >= 32 && c < 127; });
    if (fits) return id;
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "mmv_%08x",
                  Crc32(reinterpret_cast<const std::uint8_t*>(id.data()), id.size()));
    return buffer;
}

bool IsBooleanProxy(const std::string& id) {
    return id == "visible" || id == "shadow" || id == "parentModel" || id == "parentBone" ||
           id == "noiseEnabled" || id == "noise";
}

// ---------------------------------------------------------------------------
// Standard section records
// ---------------------------------------------------------------------------

struct BoneFrame {
    std::string name;
    std::uint32_t frame = 0;
    std::array<double, 3> position{};
    std::array<double, 4> quaternion{};
    std::array<std::array<double, 4>, 4> controls{};  // x, y, z, rotation as x1,y1,x2,y2
};

struct MorphFrame {
    std::string name;
    std::uint32_t frame = 0;
    double value = 0.0;
};

struct CameraFrame {
    std::uint32_t frame = 0;
    double distance = 0.0;
    std::array<double, 3> eye{};
    std::array<double, 3> target{};
    std::array<std::array<double, 4>, 6> controls{};  // X, Y, Z, rotation, distance, fov
    std::uint32_t fov = 0;
    std::uint8_t perspective = 0;
};

struct LightFrame {
    std::uint32_t frame = 0;
    std::array<double, 3> color{};
    std::array<double, 3> direction{};
};

struct ShadowFrame {
    std::uint32_t frame = 0;
    std::uint8_t mode = 0;
    double distance = 0.0;
};

template <class Record>
void SortByFrame(std::vector<Record>& records) {
    std::stable_sort(records.begin(), records.end(),
                     [](const Record& left, const Record& right) { return left.frame < right.frame; });
    std::vector<Record> unique;
    unique.reserve(records.size());
    for (const Record& record : records) {
        if (!unique.empty() && unique.back().frame == record.frame) {
            unique.back() = record;
        } else {
            unique.push_back(record);
        }
    }
    records.swap(unique);
}

void CheckSampleCount(std::uint32_t first, std::uint32_t last) {
    const std::uint64_t count = std::uint64_t(last) - std::uint64_t(first) + 1;
    if (count > kMaximumRecords) {
        throw std::runtime_error("sampling frames " + std::to_string(first) + ".." + std::to_string(last) +
                                 " would need " + std::to_string(count) + " keys, the limit is " +
                                 std::to_string(kMaximumRecords));
    }
}

// ---------------------------------------------------------------------------
// Standard body import
// ---------------------------------------------------------------------------

std::vector<ac::NamedTrack> BuildAccessory(std::vector<BoneFrame> bones) {
    if (bones.empty()) {
        throw std::runtime_error("the file has no bone track to read as an accessory motion");
    }
    std::set<std::string> names;
    for (const BoneFrame& bone : bones) names.insert(bone.name);
    if (names.size() > 1) {
        throw std::runtime_error("the file holds " + std::to_string(names.size()) +
                                 " bone tracks; an accessory motion needs exactly one and this import "
                                 "does not guess which one to use");
    }
    SortByFrame(bones);
    const std::uint32_t first = bones.front().frame;
    const std::uint32_t last = bones.back().frame;
    CheckSampleCount(first, last);

    std::vector<std::uint32_t> boneFrames;
    boneFrames.reserve(bones.size());
    for (BoneFrame& bone : bones) {
        boneFrames.push_back(bone.frame);
        bone.quaternion = NormalizeQuaternion(bone.quaternion);
    }

    std::vector<ac::NamedTrack> out;
    for (const char* id : {"x", "y", "z", "rx", "ry", "rz"}) out.push_back({id, ac::Track{}});
    if (bones.size() == 1) {
        const std::array<double, 3> euler = EulerDegrees(bones.front().quaternion);
        for (int axis = 0; axis < 3; ++axis) {
            out[std::size_t(axis)].track.keys.push_back(
                MakeLinearKey(CheckedFrame(first), bones.front().position[std::size_t(axis)]));
            out[std::size_t(3 + axis)].track.keys.push_back(
                MakeLinearKey(CheckedFrame(first), euler[std::size_t(axis)]));
        }
        return out;
    }

    // Accessory VMD import already emits every integer frame. Sample raw
    // MMD timing directly, so crossed/endpoint controls never pass through a
    // scalar-handle approximation. Preserve the legacy physics-marker fallback.
    const auto eased=[](const std::array<double,4>& controls,double time) {
        const bool valid=std::all_of(controls.begin(),controls.end(),[](double value){return value>=0&&value<=127;});
        return valid?ac::EvaluateMmdTiming(controls,time).value:std::clamp(time,0.0,1.0);
    };
    const std::uint64_t count = std::uint64_t(last) - std::uint64_t(first) + 1;
    std::size_t segment = 0;
    std::array<double, 3> previous{};
    bool havePrevious = false;
    for (std::uint64_t step = 0; step < count; ++step) {
        const std::uint32_t frame = std::uint32_t(std::uint64_t(first) + step);
        while (segment + 1 < bones.size() && bones[segment + 1].frame <= frame) ++segment;
        const std::size_t lower = std::min(segment, bones.size() - 2);
        const std::size_t upper = lower + 1;
        const double time=double(frame-bones[lower].frame)/double(bones[upper].frame-bones[lower].frame);
        const double fraction=std::clamp(eased(bones[upper].controls[3],time),0.0,1.0);
        const std::array<double, 4> quaternion =
            Slerp(bones[lower].quaternion, bones[upper].quaternion, fraction);
        std::array<double, 3> euler = EulerDegrees(quaternion);
        if (havePrevious) {
            for (int axis = 0; axis < 3; ++axis) {
                Unwrap(euler[std::size_t(axis)], previous[std::size_t(axis)]);
            }
        }
        previous = euler;
        havePrevious = true;
        const std::int32_t checked = CheckedFrame(frame);
        for (int axis = 0; axis < 3; ++axis) {
            out[std::size_t(axis)].track.keys.push_back(
                MakeLinearKey(checked, bones[lower].position[axis]+(bones[upper].position[axis]-bones[lower].position[axis])*eased(bones[upper].controls[axis],time)));
            out[std::size_t(3 + axis)].track.keys.push_back(MakeLinearKey(checked, euler[std::size_t(axis)]));
        }
    }
    return out;
}

std::vector<ac::NamedTrack> BuildCamera(std::vector<CameraFrame> cameras, std::vector<NativeTiming>& timing) {
    if (cameras.empty()) throw std::runtime_error("the file has no camera keys");
    SortByFrame(cameras);
    const std::string ids[9] = {"x", "y", "z", "rx", "ry", "rz", "distance", "fov", "perspective"};
    const int lanes[9] = {0, 1, 2, 3, 3, 3, 4, 5, -1};
    Tracks tracks;
    for (const CameraFrame& camera : cameras) {
        const std::int32_t frame = CheckedFrame(camera.frame);
        AddKey(tracks, "x", frame, camera.eye[0], false);
        AddKey(tracks, "y", frame, camera.eye[1], false);
        AddKey(tracks, "z", frame, camera.eye[2], false);
        AddKey(tracks, "rx", frame, camera.target[0] * 180.0 / kPi, false);
        AddKey(tracks, "ry", frame, camera.target[1] * 180.0 / kPi, false);
        AddKey(tracks, "rz", frame, camera.target[2] * 180.0 / kPi, false);
        AddKey(tracks, "distance", frame, camera.distance, false);
        AddKey(tracks, "fov", frame, double(camera.fov), false);
        AddKey(tracks, "perspective", frame, camera.perspective != 0 ? 1.0 : 0.0, true);
    }
    // A lane was recorded on the key that ends its interval, so the byte order
    // is only mapped back after the whole section has been read and sorted.
    for (int index = 0; index < 9; ++index) {
        if (lanes[index] < 0) continue;
        ac::Track& track = tracks[ids[index]];
        for (std::size_t key = 1; key < track.keys.size(); ++key) {
            const auto& controls = cameras[key].controls[std::size_t(lanes[index])];
            for (double value : controls) if (value < 0 || value > 127)
                throw std::runtime_error("a camera interpolation control is outside 0..127");
            timing.push_back({ids[index], track.keys[key].frame, controls, true});
            auto& a = track.keys[key-1]; auto& b = track.keys[key];
            std::string reason;
            if (!ac::SetMmdControls(track, key, controls, reason)) {
                const double x1=std::clamp(controls[0]/127.0,0.0002,0.9996);
                const double x2=std::clamp(controls[2]/127.0,x1+0.0002,0.9998);
                const double rate=(b.value-a.value)/double(b.frame-a.frame);
                a.outWeight=x1; b.inWeight=1-x2;
                a.outSlope=controls[1]/127.0*rate/x1;
                b.inSlope=(1-controls[3]/127.0)*rate/(1-x2);
                b.interpolation=ac::Interpolation::Bezier;
            }
            // Raw metadata also owns diagonal lanes (including camera cuts).
            b.interpolation=ac::Interpolation::Bezier;
        }
    }
    std::vector<ac::NamedTrack> out;
    for (const std::string& id : ids) out.push_back({id, std::move(tracks[id])});
    return out;
}

std::vector<ac::NamedTrack> BuildLight(const std::vector<LightFrame>& input) {
    if (input.empty()) throw std::runtime_error("the file has no light keys");
    std::vector<LightFrame> lights = input;
    SortByFrame(lights);
    Tracks tracks;
    for (const LightFrame& light : lights) {
        const std::int32_t frame = CheckedFrame(light.frame);
        AddKey(tracks, "r", frame, light.color[0], false);
        AddKey(tracks, "g", frame, light.color[1], false);
        AddKey(tracks, "b", frame, light.color[2], false);
        AddKey(tracks, "x", frame, light.direction[0], false);
        AddKey(tracks, "y", frame, light.direction[1], false);
        AddKey(tracks, "z", frame, light.direction[2], false);
    }
    std::vector<ac::NamedTrack> out;
    for (const char* id : {"x", "y", "z", "r", "g", "b"}) out.push_back({id, std::move(tracks[id])});
    return out;
}

std::vector<ac::NamedTrack> BuildShadow(const std::vector<ShadowFrame>& input) {
    if (input.empty()) throw std::runtime_error("the file has no self-shadow keys");
    std::vector<ShadowFrame> shadows = input;
    SortByFrame(shadows);
    Tracks tracks;
    for (const ShadowFrame& shadow : shadows) {
        const std::int32_t frame = CheckedFrame(shadow.frame);
        AddKey(tracks, "mode", frame, double(shadow.mode), true);
        AddKey(tracks, "distance", frame, shadow.distance, false);
    }
    std::vector<ac::NamedTrack> out;
    for (const char* id : {"mode", "distance"}) out.push_back({id, std::move(tracks[id])});
    return out;
}

std::vector<ac::NamedTrack> BuildParameterProxies(std::vector<MorphFrame> morphs) {
    if (morphs.empty()) {
        throw std::runtime_error("the file carries no parameter keys for the selected motion");
    }
    std::stable_sort(morphs.begin(), morphs.end(),
                     [](const MorphFrame& left, const MorphFrame& right) { return left.frame < right.frame; });
    Tracks tracks;
    for (const MorphFrame& morph : morphs) {
        AddKey(tracks, morph.name, CheckedFrame(morph.frame), morph.value, IsBooleanProxy(morph.name));
    }
    std::vector<ac::NamedTrack> out;
    out.reserve(tracks.size());
    for (auto& entry : tracks) out.push_back({entry.first, std::move(entry.second)});
    return out;
}

}  // namespace

namespace {
bool NativeGeometry(const curve_editor::Channel& channel, std::size_t end) {
    if (!end || end>=channel.track.keys.size() || end>=channel.nativeControls.size() ||
        end>=channel.controlGeometry.size()) return false;
    const auto& a=channel.track.keys[end-1]; const auto& b=channel.track.keys[end];
    return b.interpolation==ac::Interpolation::Bezier &&
        channel.controlGeometry[end]==std::array<double,4>{a.outWeight,a.outSlope,b.inWeight,b.inSlope};
}
float CameraCurveTime(const std::array<double,4>& control,float t) {
    // De Casteljau subdivision can introduce a few double-precision ulps
    // into an exactly diagonal native lane. Keep its original straight path.
    if(std::abs(control[0]-control[1])<=1e-12 && std::abs(control[2]-control[3])<=1e-12)return t;
    const float x1=float(control[0])/127.0f,x2=float(control[2])/127.0f;
    float u=0.5f,half=0.25f;
    for(int i=0;i<12;++i){
        const float v=1.0f-u,v3=v*3.0f;
        const float x=(v3*v*u)*x1+(v3*u*u)*x2+u*u*u;
        if(x==t)break;
        u=x>=t?u-half:u+half;half*=0.5f;
    }
    const float v=1.0f-u,v3=v*3.0f;
    return (v3*v*u)*float(control[1])/127.0f+(v3*u*u)*float(control[3])/127.0f+u*u*u;
}
}
ac::Sample SampleCameraChannel(const curve_editor::Channel& channel,double frame) {
    const auto& keys=channel.track.keys;
    if(keys.size()<2 || !std::isfinite(frame))return ac::Evaluate(channel.track,frame);
    const auto value=[&](double at){
        if(at<=keys.front().frame)return keys.front().value;
        if(at>=keys.back().frame)return keys.back().value;
        const auto bound=std::upper_bound(keys.begin(),keys.end(),at,[](double f,const ac::Key&k){return f<k.frame;});
        const auto end=std::size_t(bound-keys.begin());
        if(!NativeGeometry(channel,end))return ac::Evaluate(channel.track,at).value;
        const auto&a=keys[end-1];const auto&b=keys[end];
        if(at==a.frame)return a.value;
        const double span=double(b.frame)-a.frame;
        if(span==1.0&&(end>=channel.nativeOneFrameHolds.size()||channel.nativeOneFrameHolds[end]))return a.value;
        return a.value+(b.value-a.value)*CameraCurveTime(channel.nativeControls[end],float((at-a.frame)/span));
    };
    double speed=0;
    if(frame>=keys.front().frame && frame<=keys.back().frame){
        auto at=std::upper_bound(keys.begin(),keys.end(),frame,[](double f,const ac::Key&k){return f<k.frame;});
        const auto end=at==keys.end()?keys.size()-1:std::size_t(at-keys.begin());
        const auto&a=keys[end-1];const auto&b=keys[end];
        const double span=double(b.frame)-a.frame;
        if(NativeGeometry(channel,end)){
            if(span>1||(end<channel.nativeOneFrameHolds.size()&&!channel.nativeOneFrameHolds[end]))
                speed=(b.value-a.value)/span*ac::EvaluateMmdTiming(channel.nativeControls[end],(frame-a.frame)/span).derivative;
        }else speed=ac::Evaluate(channel.track,frame).derivative;
    }
    return {value(frame),speed};
}
Motion CaptureChannels(Kind kind,const std::vector<curve_editor::Channel>& channels) {
    Motion motion; motion.kind=kind;
    for (const auto& channel:channels) if (channel.selected) {
        motion.tracks.push_back({channel.id,channel.track});
        if (kind==Kind::Camera) for (std::size_t end=1;end<channel.track.keys.size();++end)
            if (NativeGeometry(channel,end)) motion.nativeTiming.push_back({channel.id,channel.track.keys[end].frame,
                channel.nativeControls[end],end>=channel.nativeOneFrameHolds.size()||channel.nativeOneFrameHolds[end]});
    }
    motion.sample=[channels](const std::string& id,double frame) {
        const auto found=std::find_if(channels.begin(),channels.end(),[&](const auto& channel){return channel.id==id;});
        return found==channels.end()?0.0:curve_editor::EvaluateChannel(*found,frame).value;
    };
    return motion;
}
bool RestoreChannels(const Motion& motion,std::vector<curve_editor::Channel>& channels,std::string& error) {
    error.clear();
    if (!motion.nativeTiming.empty() && motion.kind!=Kind::Camera) { error="Native timing requires a camera motion."; return false; }
    auto candidate=channels;
    std::set<std::pair<std::string,int>> seen;
    for (auto& channel:candidate) {
        const auto track=std::find_if(motion.tracks.begin(),motion.tracks.end(),[&](const auto& item){return item.id==channel.id;});
        if (track==motion.tracks.end()) continue;
        channel.track=track->track; channel.sample={}; channel.nativeControls.clear();
        channel.controlGeometry.clear(); channel.nativeOneFrameHolds.clear(); channel.interpolationGroup.clear();
        for (const auto& timing:motion.nativeTiming) if (timing.id==channel.id) {
            const auto at=std::lower_bound(channel.track.keys.begin(),channel.track.keys.end(),timing.endFrame,
                [](const auto& key,int frame){return key.frame<frame;});
            if (at==channel.track.keys.end() || at==channel.track.keys.begin() || at->frame!=timing.endFrame ||
                at->interpolation!=ac::Interpolation::Bezier || !seen.emplace(timing.id,timing.endFrame).second ||
                !std::all_of(timing.controls.begin(),timing.controls.end(),[](double value){return std::isfinite(value);})) {
                error="Native camera timing does not match a unique Bezier interval."; return false;
            }
            const auto end=std::size_t(at-channel.track.keys.begin());
            channel.nativeControls.resize(channel.track.keys.size());
            channel.controlGeometry.resize(channel.track.keys.size());
            channel.nativeOneFrameHolds.resize(channel.track.keys.size(),true);
            const auto& a=channel.track.keys[end-1];
            channel.nativeControls[end]=timing.controls;
            channel.controlGeometry[end]={a.outWeight,a.outSlope,at->inWeight,at->inSlope};
            channel.nativeOneFrameHolds[end]=timing.oneFrameHold;
            channel.sample=SampleCameraChannel;
        }
        if (motion.kind==Kind::Camera && (channel.id=="rx"||channel.id=="ry"||channel.id=="rz"))
            channel.interpolationGroup="camera/rotation";
    }
    // Metadata for channels omitted by this recipient is allowed, but every
    // metadata record must refer to a real source track and interval.
    for (const auto& timing:motion.nativeTiming) {
        const auto track=std::find_if(motion.tracks.begin(),motion.tracks.end(),[&](const auto& item){return item.id==timing.id;});
        if (track==motion.tracks.end()) { error="Native timing refers to a missing channel."; return false; }
        if (std::none_of(candidate.begin(),candidate.end(),[&](const auto& c){return c.id==timing.id;})) {
            curve_editor::Channel channel; channel.id=timing.id;
            std::vector<curve_editor::Channel> one{channel};
            Motion subset;subset.kind=motion.kind;subset.tracks.push_back(*track);
            for (const auto& value:motion.nativeTiming) if(value.id==timing.id) subset.nativeTiming.push_back(value);
            if (!RestoreChannels(subset,one,error)) return false;
        }
    }
    channels.swap(candidate);return true;
}
ac::Sample Evaluate(const Motion& motion,const std::string& id,double frame) {
    const auto track=std::find_if(motion.tracks.begin(),motion.tracks.end(),[&](const auto& item){return item.id==id;});
    if (track==motion.tracks.end()) return {};
    if (motion.sample) return {motion.sample(id,frame),0};
    curve_editor::Channel channel;channel.id=id;
    std::vector<curve_editor::Channel> channels{channel};std::string error;
    if (!RestoreChannels(motion,channels,error)) return {};
    return curve_editor::EvaluateChannel(channels.front(),frame);
}

bool Encode(const Motion& motion, std::vector<std::uint8_t>& out, std::string& error) {
    try {
        if (std::uint32_t(motion.kind) > std::uint32_t(Kind::Morph)) {
            throw std::runtime_error("unknown motion kind");
        }
        // The curve payload is the authoritative copy of every track id, so it
        // is built (and validated) before anything is written.
        std::vector<std::uint8_t> payload;
        if (!ac::Encode(motion.tracks, payload, error)) return false;
        if (!motion.nativeTiming.empty()) {
            if (motion.kind != Kind::Camera) throw std::runtime_error("native timing requires a camera motion");
            std::vector<curve_editor::Channel> checked;
            for (const auto& track : motion.tracks) { curve_editor::Channel channel; channel.id=track.id; checked.push_back(std::move(channel)); }
            if (!RestoreChannels(motion, checked, error)) return false;
            if (motion.nativeTiming.size() > kMaximumRecords) throw std::runtime_error("too many native timing records");
            Writer envelope;
            envelope.raw("MDSN",4); envelope.u64(payload.size()); envelope.raw(payload.data(),payload.size());
            envelope.u32(std::uint32_t(motion.nativeTiming.size()));
            for (const auto& timing : motion.nativeTiming) {
                envelope.u32(std::uint32_t(timing.id.size())); envelope.raw(timing.id.data(),timing.id.size());
                envelope.u32(std::uint32_t(timing.endFrame)); envelope.u32(timing.oneFrameHold?1:0);
                for (double value : timing.controls) envelope.f64(value);
            }
            payload.swap(envelope.bytes);
        }

        std::map<std::string, const ac::Track*> lookup;
        for (const ac::NamedTrack& named : motion.tracks) lookup[named.id] = &named.track;

        std::int64_t first = 0;
        std::int64_t last = -1;
        for (const ac::NamedTrack& named : motion.tracks) {
            if (named.track.keys.empty()) continue;
            const std::int64_t front = named.track.keys.front().frame;
            const std::int64_t back = named.track.keys.back().frame;
            if (last < 0) {
                first = front;
                last = back;
            } else {
                first = std::min(first, front);
                last = std::max(last, back);
            }
        }
        if (last < 0) throw std::runtime_error("the motion has no keys to export");
        if (last > std::int64_t(kMaximumFrame)) {
            throw std::runtime_error("frame " + std::to_string(last) + " is outside the supported range");
        }
        const std::uint64_t frames = std::uint64_t(last - first) + 1;

        const bool accessory = motion.kind == Kind::Accessory;
        const bool camera = motion.kind == Kind::Camera;
        const bool light = motion.kind == Kind::Light;
        const bool shadow = motion.kind == Kind::Shadow;
        const bool proxies = accessory || motion.kind == Kind::Gravity || motion.kind == Kind::Effect ||
                             motion.kind == Kind::Morph;

        std::vector<std::string> proxyIds;
        if (proxies) {
            std::set<std::string> proxyNames;
            for (const ac::NamedTrack& named : motion.tracks) {
                if (accessory && IsAccessoryBoneChannel(named.id)) continue;
                const std::string proxy = ProxyName(named.id);
                if (!proxyNames.insert(proxy).second) {
                    throw std::runtime_error("two channels would share the VMD proxy name \"" + proxy + "\"");
                }
                proxyIds.push_back(named.id);
            }
        }

        const std::uint64_t boneRecords = accessory ? frames : 0;
        const std::uint64_t morphRecords = proxies ? frames * proxyIds.size() : 0;
        const std::uint64_t cameraRecords = camera ? frames : 0;
        const std::uint64_t lightRecords = light ? frames : 0;
        const std::uint64_t shadowRecords = shadow ? frames : 0;
        const std::uint64_t records =
            boneRecords + morphRecords + cameraRecords + lightRecords + shadowRecords;
        if (records > kMaximumRecords) {
            throw std::runtime_error("the per-frame VMD proxy would need " + std::to_string(records) +
                                     " records, the limit is " + std::to_string(kMaximumRecords));
        }
        const std::uint64_t total = kSignatureBytes + kModelNameBytes + 4 +
                                    boneRecords * kBoneRecordBytes + 4 + morphRecords * kMorphRecordBytes + 4 +
                                    cameraRecords * kCameraRecordBytes + 4 + lightRecords * kLightRecordBytes +
                                    4 + shadowRecords * kShadowRecordBytes + 4 + kFooterBytes + payload.size();
        if (total > kMaximumBytes) {
            throw std::runtime_error("the VMD would need " + std::to_string(total) + " bytes, the limit is " +
                                     std::to_string(kMaximumBytes));
        }

        std::vector<curve_editor::Channel> nativeChannels;
        if (!motion.sample && !motion.nativeTiming.empty()) {
            for (const auto& named:motion.tracks) { curve_editor::Channel channel;channel.id=named.id;nativeChannels.push_back(std::move(channel)); }
            if (!RestoreChannels(motion,nativeChannels,error)) return false;
        }
        const auto value = [&lookup,&motion,&nativeChannels](const char* id, std::int32_t frame, double fallback) {
            const auto found = lookup.find(id);
            if (found == lookup.end() || found->second->keys.empty()) return fallback;
            if (motion.sample) return motion.sample(id,double(frame));
            const auto channel=std::find_if(nativeChannels.begin(),nativeChannels.end(),[&](const auto& c){return c.id==id;});
            return channel==nativeChannels.end()?ac::Evaluate(*found->second,double(frame)).value:
                curve_editor::EvaluateChannel(*channel,double(frame)).value;
        };
        const auto tracked = [&value](const std::string& id, std::int32_t frame) {
            return value(id.c_str(), frame, 0.0);
        };

        Writer writer;
        writer.bytes.reserve(std::size_t(total));
        writer.text(kVmd2Signature, kSignatureBytes);
        writer.text(camera || light || shadow ? std::string(kCameraLightName, 12) : std::string(kCurveModelName),
                    kModelNameBytes);

        // Bone section: the accessory proxy needs one record per frame so an
        // unaware reader interpolates nothing at all.
        writer.u32(std::uint32_t(boneRecords));
        if (accessory) {
            for (std::int64_t frame = first; frame <= last; ++frame) {
                const std::int32_t current = std::int32_t(frame);
                writer.text("Accessory", 15);
                writer.u32(std::uint32_t(frame));
                writer.f(value("x", current, 0.0));
                writer.f(value("y", current, 0.0));
                writer.f(value("z", current, 0.0));
                const std::array<double, 4> quaternion = QuaternionDegrees(
                    value("rx", current, 0.0), value("ry", current, 0.0), value("rz", current, 0.0));
                for (double component : quaternion) writer.f(component);
                WriteDefaultBoneInterpolation(writer);
            }
        }

        // Morph section: proxies for every channel the native sections cannot
        // carry, and for the whole motion of the proxy-only kinds.
        writer.u32(std::uint32_t(morphRecords));
        for (const std::string& id : proxyIds) {
            const std::string proxy = ProxyName(id);
            for (std::int64_t frame = first; frame <= last; ++frame) {
                writer.text(proxy, 15);
                writer.u32(std::uint32_t(frame));
                writer.f(tracked(id, std::int32_t(frame)));
            }
        }

        writer.u32(std::uint32_t(cameraRecords));
        if (camera) {
            for (std::int64_t frame = first; frame <= last; ++frame) {
                const std::int32_t current = std::int32_t(frame);
                writer.u32(std::uint32_t(frame));
                writer.f(value("distance", current, 0.0));
                writer.f(value("x", current, 0.0));
                writer.f(value("y", current, 0.0));
                writer.f(value("z", current, 0.0));
                writer.f(value("rx", current, 0.0) * kPi / 180.0);
                writer.f(value("ry", current, 0.0) * kPi / 180.0);
                writer.f(value("rz", current, 0.0) * kPi / 180.0);
                WriteDefaultCameraLanes(writer);
                writer.u32(ToRecordInteger(value("fov", current, 45.0), 0.0, 1000000.0, "field of view"));
                writer.u8(value("perspective", current, 0.0) != 0.0 ? std::uint8_t(1) : std::uint8_t(0));
            }
        }

        writer.u32(std::uint32_t(lightRecords));
        if (light) {
            for (std::int64_t frame = first; frame <= last; ++frame) {
                const std::int32_t current = std::int32_t(frame);
                writer.u32(std::uint32_t(frame));
                for (const char* id : {"r", "g", "b", "x", "y", "z"}) writer.f(value(id, current, 0.0));
            }
        }

        writer.u32(std::uint32_t(shadowRecords));
        if (shadow) {
            for (std::int64_t frame = first; frame <= last; ++frame) {
                const std::int32_t current = std::int32_t(frame);
                writer.u32(std::uint32_t(frame));
                writer.u8(std::uint8_t(ToRecordInteger(value("mode", current, 0.0), 0.0, 255.0, "shadow mode")));
                writer.f(value("distance", current, 0.0));
            }
        }

        // Complete but empty display/IK master section: the curve tail may only
        // follow the whole standard body.
        writer.u32(0);

        const std::size_t prefixBytes = writer.bytes.size();
        writer.raw(payload.data(), payload.size());
        writer.raw(motion.nativeTiming.empty()?kFooterMagic:kNativeFooterMagic, sizeof(kFooterMagic));
        writer.u64(std::uint64_t(prefixBytes));
        writer.u64(std::uint64_t(payload.size()));
        writer.u32(Crc32(writer.bytes.data(), prefixBytes));
        writer.u32(Crc32(payload.data(), payload.size()));
        writer.u32(std::uint32_t(motion.kind));
        writer.u32(0);
        out.swap(writer.bytes);
        return true;
    } catch (const std::exception& caught) {
        error = caught.what();
        return false;
    }
}

bool Decode(const std::uint8_t* data, std::size_t size, Kind requested, Motion& out, std::string& error) {
    try {
        if (data == nullptr) throw std::runtime_error("there is no VMD buffer to decode");
        if (size < kSignatureBytes + kLegacyNameBytes + 8) {
            throw std::runtime_error("the buffer is too small to be a VMD");
        }
        if (size > kMaximumBytes) throw std::runtime_error("the VMD exceeds the size limit");
        if (std::uint32_t(requested) > std::uint32_t(Kind::Morph)) {
            throw std::runtime_error("unknown motion kind");
        }

        Reader reader{data, size};
        const std::string signature = reader.text(kSignatureBytes);
        if (signature == kVmd2Signature) {
            reader.skip(kModelNameBytes);
        } else if (signature == kVmd1Signature) {
            reader.skip(kLegacyNameBytes);
        } else {
            throw std::runtime_error("the file is not a Vocaloid Motion Data file");
        }

        // A valid curve tail makes the standard proxy redundant, so the proxy
        // records are only materialised when they will actually be used.
        const bool nativeTail = size >= kFooterBytes && std::memcmp(data + size - kFooterBytes, kNativeFooterMagic, 8) == 0;
        const bool hasTail = nativeTail ||
            (size >= kFooterBytes && std::memcmp(data + size - kFooterBytes, kFooterMagic, 8) == 0);
        const bool store = !hasTail;
        const bool wantBones = store && requested == Kind::Accessory;
        const bool wantParameters = store && (requested == Kind::Accessory || requested == Kind::Effect ||
                                              requested == Kind::Morph || requested == Kind::Gravity);
        const bool wantCamera = store && requested == Kind::Camera;
        const bool wantLight = store && requested == Kind::Light;
        const bool wantShadow = store && requested == Kind::Shadow;

        std::vector<BoneFrame> bones;
        std::vector<MorphFrame> morphs;
        std::vector<CameraFrame> cameras;
        std::vector<LightFrame> lights;
        std::vector<ShadowFrame> shadows;

        const std::uint32_t boneCount = reader.count(kBoneRecordBytes);
        for (std::uint32_t index = 0; index < boneCount; ++index) {
            BoneFrame bone;
            bone.name = reader.text(15);
            bone.frame = reader.u32();
            CheckedFrame(bone.frame);
            for (double& component : bone.position) component = reader.f();
            for (double& component : bone.quaternion) component = reader.f();
            std::uint8_t block[64] = {};
            for (std::uint8_t& byte : block) byte = reader.u8();
            for (int channel = 0; channel < 4; ++channel) {
                bone.controls[std::size_t(channel)] = BoneChannel(block, channel);
            }
            if (wantBones) bones.push_back(std::move(bone));
        }

        const std::uint32_t morphCount = reader.count(kMorphRecordBytes);
        for (std::uint32_t index = 0; index < morphCount; ++index) {
            MorphFrame morph;
            morph.name = reader.text(15);
            morph.frame = reader.u32();
            CheckedFrame(morph.frame);
            morph.value = reader.f();
            if (wantParameters) morphs.push_back(std::move(morph));
        }

        // The remaining sections are optional: an old VMD 0001 file, or any
        // writer that stops early, simply ends here.
        if (reader.remaining() > 0) {
            const std::uint32_t cameraCount = reader.count(kCameraRecordBytes);
            for (std::uint32_t index = 0; index < cameraCount; ++index) {
                CameraFrame camera;
                camera.frame = reader.u32();
                CheckedFrame(camera.frame);
                camera.distance = reader.f();
                for (double& component : camera.eye) component = reader.f();
                for (double& component : camera.target) component = reader.f();
                for (std::array<double, 4>& lane : camera.controls) {
                    std::uint8_t bytes[4] = {};
                    for (std::uint8_t& byte : bytes) byte = reader.u8();
                    lane = CameraChannel(bytes);
                }
                camera.fov = reader.u32();
                camera.perspective = reader.u8();
                if (wantCamera) cameras.push_back(std::move(camera));
            }
        }
        if (reader.remaining() > 0) {
            const std::uint32_t lightCount = reader.count(kLightRecordBytes);
            for (std::uint32_t index = 0; index < lightCount; ++index) {
                LightFrame light;
                light.frame = reader.u32();
                CheckedFrame(light.frame);
                for (double& component : light.color) component = reader.f();
                for (double& component : light.direction) component = reader.f();
                if (wantLight) lights.push_back(std::move(light));
            }
        }
        if (reader.remaining() > 0) {
            const std::uint32_t shadowCount = reader.count(kShadowRecordBytes);
            for (std::uint32_t index = 0; index < shadowCount; ++index) {
                ShadowFrame shadow;
                shadow.frame = reader.u32();
                CheckedFrame(shadow.frame);
                shadow.mode = reader.u8();
                shadow.distance = reader.f();
                if (wantShadow) shadows.push_back(std::move(shadow));
            }
        }
        if (reader.remaining() > 0) {
            const std::uint32_t masterCount = reader.count(kIkMasterBytes);
            for (std::uint32_t index = 0; index < masterCount; ++index) {
                CheckedFrame(reader.u32());
                reader.u8();
                const std::uint32_t chains = reader.u32();
                if (chains > kMaximumRecords || std::uint64_t(chains) * kIkEntryBytes > reader.remaining()) {
                    throw std::runtime_error("the VMD declares an impossible IK chain count");
                }
                reader.skip(std::size_t(chains) * kIkEntryBytes);
            }
        }
        const std::size_t standardEnd = reader.pos;

        if (hasTail) {
            Reader tail{data + size - kFooterBytes, kFooterBytes};
            tail.skip(8);
            const std::uint64_t prefixBytes = tail.u64();
            const std::uint64_t curveBytes = tail.u64();
            const std::uint32_t prefixCrc = tail.u32();
            const std::uint32_t curveCrc = tail.u32();
            const std::uint32_t kind = tail.u32();
            const std::uint32_t reserved = tail.u32();
            if (prefixBytes != standardEnd) {
                throw std::runtime_error("the curve tail does not start at the end of the standard VMD data");
            }
            if (curveBytes != size - kFooterBytes - standardEnd) {
                throw std::runtime_error("the curve tail length does not match the payload it holds");
            }
            if (prefixCrc != Crc32(data, standardEnd)) {
                throw std::runtime_error("the standard VMD data does not match the curve tail checksum");
            }
            if (curveCrc != Crc32(data + standardEnd, std::size_t(curveBytes))) {
                throw std::runtime_error("the curve payload does not match its checksum");
            }
            if (reserved != 0) throw std::runtime_error("the curve tail reserved field must be zero");
            if (kind != std::uint32_t(requested)) {
                throw std::runtime_error("the file holds a different motion type than the one selected");
            }
            Motion candidate;
            candidate.kind = requested;
            const auto* curveData = data + standardEnd;
            std::size_t scalarBytes = std::size_t(curveBytes);
            Reader envelope{curveData,scalarBytes};
            if (nativeTail) {
                if (envelope.text(4)!="MDSN") throw std::runtime_error("the native timing envelope marker is invalid");
                const auto length = envelope.u64();
                if (length > envelope.remaining()) throw std::runtime_error("the native timing scalar length is invalid");
                curveData += envelope.pos; scalarBytes = std::size_t(length); envelope.skip(scalarBytes);
            }
            if (!ac::Decode(curveData, scalarBytes, candidate.tracks, error)) return false;
            if (nativeTail) {
                const auto count = envelope.u32();
                if (count > envelope.remaining()/45) throw std::runtime_error("the native timing count is invalid");
                for (std::uint32_t i=0;i<count;++i) {
                    const auto length = envelope.u32();
                    if (!length || length>1024) throw std::runtime_error("the native timing id is invalid");
                    NativeTiming timing; timing.id=envelope.text(length); timing.endFrame=CheckedFrame(envelope.u32());
                    const auto hold=envelope.u32();
                    if (hold>1) throw std::runtime_error("the native camera hold flag is invalid");
                    timing.oneFrameHold=hold!=0;
                    for (auto& value:timing.controls) value=envelope.f64();
                    candidate.nativeTiming.push_back(std::move(timing));
                }
                if (envelope.remaining()) throw std::runtime_error("the native timing payload has trailing bytes");
                std::vector<curve_editor::Channel> checked;
                for (const auto& track:candidate.tracks) { curve_editor::Channel channel;channel.id=track.id;checked.push_back(std::move(channel)); }
                if (!RestoreChannels(candidate,checked,error)) return false;
            }
            out = std::move(candidate);
            return true;
        }

        // A foreign tail (for example the MDSXVMDX block the bone scale
        // extension writes) is ignored, but this module's own markers must not
        // appear in the trailing region: a shifted footer, or a curve payload
        // whose footer was cut off, means the tail is damaged and the standard
        // proxy must not silently take its place.
        if (size - standardEnd >= 4 && (std::memcmp(data + standardEnd, "MDSC", 4) == 0 ||
            std::memcmp(data + standardEnd, "MDSN", 4) == 0)) {
            throw std::runtime_error("the curve payload is present but its footer is missing or damaged");
        }
        for (std::size_t at = standardEnd; at + sizeof(kFooterMagic) <= size; ++at) {
            if (std::memcmp(data + at, kFooterMagic, sizeof(kFooterMagic)) == 0 ||
                std::memcmp(data + at, kNativeFooterMagic, sizeof(kNativeFooterMagic)) == 0) {
                throw std::runtime_error("the curve tail is damaged or misplaced");
            }
        }

        Motion candidate;
        candidate.kind = requested;
        switch (requested) {
        case Kind::Accessory:
            candidate.tracks = BuildAccessory(std::move(bones));
            break;
        case Kind::Camera:
            candidate.tracks = BuildCamera(std::move(cameras), candidate.nativeTiming);
            break;
        case Kind::Light:
            candidate.tracks = BuildLight(std::move(lights));
            break;
        case Kind::Shadow:
            candidate.tracks = BuildShadow(std::move(shadows));
            break;
        default:
            candidate.tracks = BuildParameterProxies(std::move(morphs));
            break;
        }
        if (candidate.tracks.empty()) {
            throw std::runtime_error("the file carries no keys for the selected motion type");
        }
        out = std::move(candidate);
        return true;
    } catch (const std::exception& caught) {
        error = caught.what();
        return false;
    }
}

bool Save(const wchar_t* path, const Motion& motion, std::string& error) {
    if (path == nullptr) {
        error = "there is no path to save the VMD to";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    if (!Encode(motion, bytes, error)) return false;
    const std::wstring target(path);
    const std::wstring temporary = target + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                                   std::to_wstring(GetTickCount64());
    const HANDLE file =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot create the temporary VMD file";
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) != FALSE &&
              written == DWORD(bytes.size());
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (ok) {
        ok = MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!ok) {
        DeleteFileW(temporary.c_str());
        error = "cannot write the VMD file; any existing file was preserved";
    }
    return ok;
}

bool Load(const wchar_t* path, Kind requested, Motion& out, std::string& error) {
    if (path == nullptr) {
        error = "there is no VMD path to read";
        return false;
    }
    const HANDLE file =
        CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot open the VMD file";
        return false;
    }
    LARGE_INTEGER length{};
    const std::uint64_t minimum = kSignatureBytes + kLegacyNameBytes + 8;
    if (GetFileSizeEx(file, &length) == FALSE || length.QuadPart < 0 ||
        std::uint64_t(length.QuadPart) < minimum || std::uint64_t(length.QuadPart) > kMaximumBytes) {
        CloseHandle(file);
        error = "the VMD file size is not supported";
        return false;
    }
    std::vector<std::uint8_t> bytes(std::size_t(length.QuadPart));
    DWORD got = 0;
    const bool ok = ReadFile(file, bytes.data(), DWORD(bytes.size()), &got, nullptr) != FALSE &&
                    got == DWORD(bytes.size());
    CloseHandle(file);
    if (!ok) {
        error = "cannot read the VMD file";
        return false;
    }
    return Decode(bytes.data(), bytes.size(), requested, out, error);
}

}  // namespace mikudancestudio::curve_motion
