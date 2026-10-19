#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/frame_rate_correction.hpp"
#include "mikumikustudio/native_frame_retime.hpp"
#include "mikumikustudio/physics_bake.hpp"

static_assert(MMD_ORIGINAL_PROFILE, "Original profile provider must not enter the full build");
namespace mikumikustudio::curve_host {
void Open(MMDApp*, bool) {}
void OpenInterpolation(MMDApp*, bool) {}
void OpenMorph(MMDApp*, int, bool) {}
void NotifyRegistered(MMDApp* app, Kind kind, int slot, int frame) {
    if(app)ReconcileRegisteredFrame(*app,kind,slot,frame);
}
void NotifyModelRegistered(MMDApp*, bool) {}
void CreateControls(MMDApp*, void*) {}
void LayoutControls(MMDApp*) {}
void LayoutMorphControls(MMDApp*) {}
void Localize(MMDApp*) {}
void ShowControls(MMDApp*, bool) {}
void DrawLabels(MMDApp*, void*) {}
bool Command(MMDApp*, unsigned id) {
    return id == curve_editor::kAccessoryCurve || id == curve_editor::kInterpolationCurve ||
        id == kMorphCurveLane[0] || id == kMorphCurveLane[1] || id == kMorphCurveLane[2] || id == kMorphCurveLane[3];
}
}
namespace mikumikustudio::curve_editor {
void* Open(void*, Session, bool) { return nullptr; }
void CloseOwned(void*) {}
bool OwnsWindow(void*, void*) { return false; }
void SetLanguage(void*, unsigned) {}
void NotifySourceChanged(void*, const std::string&) {}
void NotifyOwnedChanged(void*) {}
void DrawLauncher(void*, int, int, int, int, bool, bool, bool) {}
}
namespace mikumikustudio::frame_rate_correction {
bool Prepare(MMDApp&, const Rates&, PlanPtr&, Report&, std::wstring&) { return false; }
bool Apply(MMDApp&, const PlanPtr&, std::wstring&) { return false; }
void RequestSaveAs(MMDApp&) {}
bool Available(MMDApp*) { return false; }
void ShowDialog(MMDApp*, bool) {}
void RefreshMenu(MMDApp*) {}
bool HandleCommand(MMDApp*, unsigned id) { return id == kMenuCommand; }
}
namespace mikumikustudio::native_frame_retime {
bool Prepare(MMDApp&, const frame_rate_correction::Rates&, PlanPtr&, frame_rate_correction::Report&, std::wstring&) { return false; }
bool Commit(MMDApp&, const PlanPtr&, std::wstring&, const std::function<bool(std::wstring&)>&) { return false; }
}
namespace mikumikustudio::bake {
void RefreshMenu(MMDApp*) {}
bool HandleCommand(MMDApp*, unsigned id) {
    return id == kBakeMenuCommand || id == kExportMenuCommand || id == kClearMeshBakeMenuCommand || id == kBakeMeshMenuCommand;
}
void ShowDialog(MMDApp*) {}
bool CanBake(MMDApp*) { return false; }
bool HasExistingKeys(const unsigned char*, const Request&) { return false; }
Result Compute(MMDApp&, const Request&, const Progress&) { Result result; result.error = Error::InvalidRequest; return result; }
SceneResult ComputeScene(MMDApp&, const Request&, const Progress&) { SceneResult result; result.error = Error::InvalidRequest; return result; }
KeyPreImage CaptureKeys(unsigned char*, const Result&) { return {}; }
void SetLayerPhysicsSuppressed(bool) {}
bool LayerPhysicsSuppressed() { return false; }
}
