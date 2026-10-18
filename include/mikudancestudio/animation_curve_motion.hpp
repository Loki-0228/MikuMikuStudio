#pragma once
#include "mikudancestudio/animation_curve.hpp"
#include "mikudancestudio/animation_curve_editor.hpp"
#include <memory>
namespace mikudancestudio::curve_motion {
enum class Kind : uint32_t { Camera, Light, Shadow, Gravity, Accessory, Effect, Morph };
// One motion source.  Track ids follow the curve editor's channel ids:
//   Camera    x, y, z, rx, ry, rz (degrees), distance, fov, perspective
//   Light     x, y, z (direction), r, g, b
//   Shadow    mode, distance
//   Gravity   strength, x, y, z, noise, noiseEnabled
//   Accessory x, y, z, rx, ry, rz (degrees), scale, opacity, visible, shadow,
//             parentModel, parentBone
//   Effect / Morph  arbitrary parameter ids, stored verbatim in the tail
struct NativeCameraData;
struct Motion {
    Kind kind=Kind::Accessory;
    std::vector<animation_curve::NamedTrack> tracks;
    // In-memory source data only. Ordinary native camera files retain their
    // raw timing and version without adding or changing a wire extension.
    std::shared_ptr<const NativeCameraData> nativeCamera;
};
// A complete, synchronized native camera can be written as sparse standard
// records. The caller must also establish that its samplers are native, not
// arbitrary callbacks. This query neither allocates nor changes channels.
bool CanCaptureNativeCamera(const std::vector<curve_editor::Channel>&, bool selectedOnly=true) noexcept;
Motion CaptureChannels(Kind, const std::vector<curve_editor::Channel>&);
// Replaces matching tracks and their old metadata transactionally. The host
// installs its canonical camera sampler after restoring raw timing.
bool RestoreChannels(const Motion&, std::vector<curve_editor::Channel>&, std::string& error);
// Authoritative native timing when present; scalar tracks are editable canvas
// proxies for raw crossed/endpoint-X controls the scalar core cannot encode.
animation_curve::Sample Evaluate(const Motion&, const std::string& id, double frame);
// All six standard VMD sections precede the optional, checksummed tail.
// Unmodified native camera motions keep sparse standard records, raw controls
// and their original version. Nonstandard bindings may use the existing tail.
// Other exports bake a per-frame proxy so readers unaware of the extension
// still reproduce the motion at integer frames; the existing tail holds Free
// curve data and real track ids. Decode falls back to reading the standard
// proxy when no tail is present: accessory samples its single bone track
// through the MMD interpolation channels, camera, light and self-shadow keep
// their native sections, and proxy names that do not fit the 15-byte field are
// kept as their checksum placeholder instead of being guessed back.
bool Encode(const Motion&,std::vector<uint8_t>&,std::string&error);
bool Decode(const uint8_t*,size_t,Kind requested,Motion&,std::string&error);
bool Save(const wchar_t*path,const Motion&,std::string&error);
bool Load(const wchar_t*path,Kind requested,Motion&,std::string&error);
}
