#pragma once
#include "mikumikustudio/animation_curve.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
namespace mikumikustudio::curve_motion {
enum class Kind : uint32_t { Camera, Light, Shadow, Gravity, Accessory, Effect, Morph };
// One motion source.  Track ids follow the curve editor's channel ids:
//   Camera    x, y, z, rx, ry, rz (degrees), distance, fov, perspective
//   Light     x, y, z (direction), r, g, b
//   Shadow    mode, distance
//   Gravity   strength, x, y, z, noise, noiseEnabled
//   Accessory x, y, z, rx, ry, rz (degrees), scale, opacity, visible, shadow,
//             parentModel, parentBone
//   Effect / Morph  arbitrary parameter ids, stored verbatim in the tail
struct NativeTiming {
    std::string id;
    std::int32_t endFrame = 0;
    std::array<double, 4> controls{};
    bool oneFrameHold = true;
};
struct Motion {
    Kind kind=Kind::Accessory;
    std::vector<animation_curve::NamedTrack> tracks;
    // Raw camera timing is authoritative; tracks retain the editable canvas
    // proxy, which cannot express crossed X handles or vertical endpoints.
    std::vector<NativeTiming> nativeTiming;
    std::function<double(const std::string&, double)> sample;
};
// Shared with the native host: fixed-step playback values and smooth graph
// derivatives, with exact key values and native one-frame camera cuts.
animation_curve::Sample SampleCameraChannel(const curve_editor::Channel&, double frame);
// Copy only valid native handle geometry, preserving the actual sampler for
// standard VMD proxies. Restore replaces stale channel metadata on import.
Motion CaptureChannels(Kind, const std::vector<curve_editor::Channel>&);
bool RestoreChannels(const Motion&, std::vector<curve_editor::Channel>&, std::string& error);
// Evaluate the authoritative motion; raw scalar tracks are canvas proxies.
animation_curve::Sample Evaluate(const Motion&, const std::string& id, double frame);
// All six standard VMD sections precede the optional, checksummed tail.
// Exports bake a per-frame proxy so readers unaware of the curve extension
// still reproduce the motion at integer frames; the tail holds the exact curve
// data and the real track ids.  Decode falls back to reading the standard
// proxy when no tail is present: accessory samples its single bone track
// through the MMD interpolation channels, camera, light and self-shadow keep
// their native sections, and proxy names that do not fit the 15-byte field are
// kept as their checksum placeholder instead of being guessed back.
bool Encode(const Motion&,std::vector<uint8_t>&,std::string&error);
bool Decode(const uint8_t*,size_t,Kind requested,Motion&,std::string&error);
bool Save(const wchar_t*path,const Motion&,std::string&error);
bool Load(const wchar_t*path,Kind requested,Motion&,std::string&error);
}
