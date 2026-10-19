#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/bone_curve.hpp"

static_assert(MMD_ORIGINAL_PROFILE, "Original profile provider must not enter the full build");
namespace mikumikustudio::bone_curve {
void Synchronize(const unsigned char*) {}
void ReconcileRegisteredFrame(const unsigned char*,int,int) {}
}
namespace mikumikustudio::curve_host {
bool Read(MMDApp&, Kind, int, std::vector<curve_editor::Channel>&, std::wstring&) { return false; }
bool Write(MMDApp&, Kind, int, const std::vector<curve_editor::Channel>&, std::wstring&) { return false; }
void Apply(MMDApp*, Kind, double, int) {}
void ReconcileRegisteredFrame(MMDApp&, Kind, int, int) {}
bool Has(MMDApp*, Kind, int) { return false; }
void Reset(MMDApp*) {}
void ForgetAccessory(MMDApp*, int) {}
void Capture(MMDApp&, pmm_extension::Document&) {}
void Load(MMDApp&, const pmm_extension::Document&) {}
struct AuthoringTable::Impl {};
AuthoringTable::AuthoringTable() = default;
AuthoringTable::~AuthoringTable() = default;
AuthoringTable::AuthoringTable(AuthoringTable&&) noexcept = default;
AuthoringTable& AuthoringTable::operator=(AuthoringTable&&) noexcept = default;
bool CaptureAuthoring(MMDApp&, AuthoringTable&, std::wstring&) { return true; }
bool AuthoringEntries(const AuthoringTable&, std::vector<AuthoringEntry>& out) { out.clear(); return true; }
bool SampleRetimedKey(const AuthoringEntry&, void*, std::size_t) { return false; }
bool RebuildAuthoring(MMDApp&, const std::vector<AuthoringEntry>& entries, AuthoringTable&, std::wstring&) { return entries.empty(); }
void SwapAuthoring(MMDApp&, AuthoringTable&) noexcept {}
bool AuthoringUnchanged(MMDApp&, const AuthoringTable&) { return true; }
}

namespace mikumikustudio::morph_curve {
// Native morph interpolation has already run at each call site. Do not replace
// its weights or create authored layers in the original profile.
void Evaluate(unsigned char*, double) {}
bool HasCurve(const unsigned char*, int) { return false; }
bool HasCurve(const MMDApp&, int) { return false; }
bool AnyCurve(const unsigned char*) { return false; }
std::size_t CurveCount(const unsigned char*) { return 0; }
void Forget(unsigned char*, int) {}
void Forget(unsigned char*) {}
void Reset(MMDApp*) {}
void CopyState(const unsigned char*, unsigned char*) {}
void CapturePmm(const MMDApp&, pmm_extension::Document&) {}
void LoadPmm(const MMDApp&, const pmm_extension::Document&) {}
bool CaptureVmd(const unsigned char*, std::uint32_t, std::vector<std::uint8_t>& out, std::string&) { out.clear(); return true; }
std::size_t VmdBlockSize(const std::uint8_t*, std::size_t) { return 0; }
std::size_t DecodeVmdBlock(const std::uint8_t*, std::size_t, std::vector<std::uint8_t>&) { return 0; }
std::size_t ApplyVmd(unsigned char*, const std::uint8_t*, std::size_t, int) { return 0; }
bool Write(unsigned char*, const std::vector<animation_curve::NamedTrack>&, std::string&) { return false; }
bool Write(MMDApp&, const std::vector<animation_curve::NamedTrack>&, std::wstring&) { return false; }
struct AuthoredLayers::Impl {};
AuthoredLayers::AuthoredLayers() = default;
AuthoredLayers::~AuthoredLayers() = default;
AuthoredLayers::AuthoredLayers(AuthoredLayers&&) noexcept = default;
AuthoredLayers& AuthoredLayers::operator=(AuthoredLayers&&) noexcept = default;
bool CaptureLayers(const unsigned char*, AuthoredLayers&, std::string&) { return true; }
bool LayersTracks(const AuthoredLayers&, std::vector<animation_curve::NamedTrack>& out, std::string&) { out.clear(); return true; }
bool RebuildLayers(const unsigned char*, const std::vector<animation_curve::NamedTrack>& tracks, AuthoredLayers&, std::string&) { return tracks.empty(); }
void SwapLayers(unsigned char*, AuthoredLayers&) noexcept {}
bool LayersUnchanged(const unsigned char*, const AuthoredLayers&) { return true; }
}
