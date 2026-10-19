// Transaction coordinator tests. Component behavior is exercised by the real
// native, cache and curve suites; these fakes force each cross-module veto.
#define NOMINMAX
#include "mikumikustudio/frame_rate_correction.hpp"
#include "mikumikustudio/native_frame_retime.hpp"
#include "mikumikustudio/mesh_bake_cache.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include <Windows.h>
#include <cstdio>
#include <memory>
#include <stdexcept>
using namespace mikumikustudio;
namespace {
int failures = 0, nativeValue = 0, cacheValue = 0, saves = 0;
bool busy = false, nativeVeto = false, cacheVeto = false;
bool nativePrepareFail = false, cachePrepareFail = false;
void Check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
}
namespace mikumikustudio {
void CmdSaveScene(MMDApp*) { ++saves; }
void RefreshAfterFrameApply(MMDApp*) {}
namespace curve_editor { void CloseOwned(void*) {} }
namespace alembic_export { bool IsExporting() { return busy; } }
namespace text_encoding {
bool Decode(const char* source, unsigned page, std::wstring& result) {
    int size = MultiByteToWideChar(page, 0, source, -1, nullptr, 0);
    if (!size) return false;
    result.resize(size); MultiByteToWideChar(page, 0, source, -1, result.data(), size); result.pop_back(); return true;
}
bool EncodeExact(const wchar_t* source, unsigned page, std::string& result) {
    int size = WideCharToMultiByte(page, 0, source, -1, nullptr, 0, nullptr, nullptr);
    if (!size) return false;
    result.resize(size); WideCharToMultiByte(page, 0, source, -1, result.data(), size, nullptr, nullptr); result.pop_back(); return true;
}
}
namespace native_frame_retime {
class Plan {};
bool Prepare(MMDApp&, const frame_rate_correction::Rates&, PlanPtr& plan,
             frame_rate_correction::Report& report, std::wstring& error) {
    if (nativePrepareFail) { error = L"native prepare"; return false; }
    plan = std::make_shared<Plan>(); report.inputKeys = 4; report.outputKeys = 3;
    report.mergedKeys = report.mergeGroups = 1; return true;
}
bool Commit(MMDApp&, const PlanPtr&, std::wstring& error, const std::function<bool(std::wstring&)>& next) {
    if (nativeVeto) { error = L"native veto"; return false; }
    ++nativeValue;
    if (next && !next(error)) { --nativeValue; return false; }
    return true;
}
}
namespace mesh_bake_cache {
struct TimeScalePlan {};
bool PrepareTimeScale(double, TimeScalePlanPtr& plan, std::size_t& count, std::string& error) {
    if (cachePrepareFail) { error = "cache prepare"; return false; }
    plan = std::make_shared<TimeScalePlan>(); count = 2; return true;
}
bool CommitTimeScale(const TimeScalePlanPtr&, std::string& error, const std::function<bool(std::string&)>& next) {
    if (cacheVeto) { error = "cache veto"; return false; }
    if (next && !next(error)) return false;
    ++cacheValue; return true;
}
}
}
int main() {
    auto app = std::make_unique<MMDApp>(); app->state = MMDAppState{};
    app->EnglishUI() = 2; app->CurrentFrame() = 12; app->TimelineStartFrame() = 3;
    app->PlaybackStartSeconds() = 1; app->PlaybackEndSeconds() = 5;
    namespace frc = frame_rate_correction;
    frc::PlanPtr plan; frc::Report report; std::wstring error;
    Check(frc::Available(app.get()), "camera-only project is available without a model");
    busy = true; Check(!frc::Available(app.get()), "active export blocks conversion"); busy = false;
    app->PlaybackActive() = 1; Check(!frc::Available(app.get()), "playback blocks conversion"); app->PlaybackActive() = 0;
    Check(!frc::Prepare(*app, {0, 30}, plan, report, error) && !plan, "invalid rates rejected before preparation");
    Check(frc::Prepare(*app, {60, 30}, plan, report, error), "prepares every module without mutation");
    Check(nativeValue == 0 && cacheValue == 0 && app->CurrentFrame() == 12, "preview leaves all project state unchanged");
    Check(report.inputKeys == 4 && report.outputKeys == 3 && report.mergedKeys == 1 && report.meshCaches == 2 && report.mmvTracks == 0, "retains native counts and cache report");
    auto previous = plan; report.inputKeys = 99;
    for (auto fail : {&nativePrepareFail, &cachePrepareFail}) {
        *fail = true;
        Check(!frc::Prepare(*app, {60, 30}, plan, report, error) && plan == previous && report.inputKeys == 99, "failed prepare preserves caller outputs");
        *fail = false;
    }
    for (auto veto : {&cacheVeto, &nativeVeto}) {
        *veto = true;
        Check(!frc::Apply(*app, plan, error), "cross-module veto reports failure");
        Check(nativeValue == 0 && cacheValue == 0 && app->CurrentFrame() == 12 && !app->SceneModified(), "cross-module veto leaves project unchanged");
        *veto = false;
    }
    app->CurrentFrame() = 13; Check(!frc::Apply(*app, plan, error), "changed playhead invalidates preview"); app->CurrentFrame() = 12;
    Check(frc::Apply(*app, plan, error), "retry applies complete transaction");
    Check(nativeValue == 1 && cacheValue == 1 && app->SceneModified(), "all components committed together");
    Check(app->CurrentFrame() == 6 && app->TimelineStartFrame() == 2 && app->PlaybackStartSeconds() == .5f && app->PlaybackEndSeconds() == 2.5f, "playhead and playback range follow the corrected timeline");
    Check(!frc::Apply(*app, plan, error) && nativeValue == 1, "one plan cannot apply twice");
    Check(saves == 0, "backend application itself never writes a file");
    frc::RequestSaveAs(*app); Check(saves == 1, "UI save request invokes native CmdSaveScene once");
    std::printf("frame-rate coordinator failures=%d\n", failures); return failures ? 1 : 0;
}
