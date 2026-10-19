#include "mikumikustudio/frame_limits.hpp"
#define NOMINMAX
#include "mikumikustudio/frame_rate_correction.hpp"
#include "mikumikustudio/alembic_export.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
#include "mikumikustudio/animation_retime.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mesh_bake_cache.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/native_frame_retime.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/text_encoding.hpp"

#include <Windows.h>
#include <cmath>
#include <exception>
#include <utility>

namespace mikumikustudio {
void RefreshAfterFrameApply(MMDApp*);
namespace frame_rate_correction {
namespace {
const wchar_t* Text(MMDApp& app, const wchar_t* zh, const wchar_t* en, const wchar_t* ja) {
    return app.EnglishUI() == 2 ? zh : app.EnglishUI() == 1 ? en : ja;
}
std::wstring Wide(const std::string& text) {
    std::wstring result;
    if (!text_encoding::Decode(text.c_str(), CP_UTF8, result))
        result.assign(text.begin(), text.end());
    return result;
}
std::string Utf8(const std::wstring& text) {
    std::string result;
    text_encoding::EncodeExact(text.c_str(), CP_UTF8, result);
    return result;
}
struct TimeState {
    int frame = 0, timeline = 0;
    double first = 0, last = 0;
};
TimeState Capture(MMDApp& app) {
    return {app.CurrentFrame(), app.TimelineStartFrame(),
            app.PlaybackStartSeconds(), app.PlaybackEndSeconds()};
}
bool Same(const TimeState& a, const TimeState& b) {
    return a.frame == b.frame && a.timeline == b.timeline &&
           a.first == b.first && a.last == b.last;
}
bool MapTime(const TimeState& input, const Rates& rates, TimeState& output, std::string& error) {
    const animation_retime::Rates r{rates.originalFPS, rates.currentFPS};
    constexpr int maximum = mikumikustudio::frame_limits::maximum;
    if (!animation_retime::MapFrame(input.frame, r, maximum, output.frame, error) ||
        !animation_retime::MapFrame(input.timeline, r, maximum, output.timeline, error)) return false;
    auto seconds = [&](double value, double& result) {
        if (!std::isfinite(value) || value < 0 || value * 30.0 > maximum + 0.5) {
            error = "Invalid playback range";
            return false;
        }
        int frame = 0;
        if (!animation_retime::MapFrame(std::llround(value * 30.0), r, maximum, frame, error)) return false;
        result = frame / 30.0;
        return true;
    };
    return seconds(input.first, output.first) && seconds(input.last, output.last);
}

}

class Plan {
public:
    MMDApp* owner = nullptr;
    native_frame_retime::PlanPtr native;
    mesh_bake_cache::TimeScalePlanPtr cache;
    TimeState before, after;
    bool consumed = false;
};

bool Available(MMDApp* app) {
    // A camera-only project is valid. CanExport additionally requires a model,
    // so use its busy state separately from the object-count condition.
    return app && !app->PlaybackActive() && !app->RecordingWindow() &&
           !bake::UiLocked() && !alembic_export::IsExporting();
}

bool Prepare(MMDApp& app, const Rates& rates, PlanPtr& plan, Report& report, std::wstring& error) {
    error.clear();
    if (!Available(&app)) {
        error = Text(app, L"请先停止播放、录制或烘焙。", L"Stop playback, recording or baking first.", L"再生・録画・ベイクを停止してください。");
        return false;
    }
    if (!std::isfinite(rates.originalFPS) || !std::isfinite(rates.currentFPS) ||
        rates.originalFPS <= 0 || rates.currentFPS <= 0 || rates.originalFPS > 1000 || rates.currentFPS > 1000) {
        error = Text(app, L"帧率须大于 0 且不超过 1000。", L"FPS must be above 0 and at most 1000.", L"FPS は 0 より大きく、1000 以下にしてください。");
        return false;
    }
    try {
        auto candidate = std::make_shared<Plan>();
        candidate->owner = &app;
        candidate->before = Capture(app);
        std::string reason;
        if (!MapTime(candidate->before, rates, candidate->after, reason)) { error = Wide(reason); return false; }
        Report result;
        if (!native_frame_retime::Prepare(app, rates, candidate->native, result, error)) return false;
        std::size_t count = 0;
        if (!mesh_bake_cache::PrepareTimeScale(rates.currentFPS / rates.originalFPS, candidate->cache, count, reason)) {
            error = Wide(reason); return false;
        }
        result.meshCaches = count;
        report = std::move(result);
        plan = std::move(candidate);
        return true;
    } catch (const std::exception& ex) {
        error = Text(app, L"无法准备帧率修正：", L"Cannot prepare frame-rate correction: ", L"フレームレート補正を準備できません：") + Wide(ex.what());
        return false;
    }
}

bool Apply(MMDApp& app, const PlanPtr& plan, std::wstring& error) {
    error.clear();
    if (!Available(&app) || !plan || plan->consumed || plan->owner != &app || !Same(plan->before, Capture(app))) {
        error = Text(app, L"工程已变化，请重新预览。", L"The project changed; preview again.", L"プロジェクトが変更されました。再度プレビューしてください。");
        return false;
    }
    std::string reason;
    // The cache lock is outermost. Native state can roll back without allocating;
    // Commit native keys while the cache transaction can still be cancelled.
    const bool committed = mesh_bake_cache::CommitTimeScale(plan->cache, reason, [&](std::string& cacheError) {
        std::wstring nativeError;
        const bool nativeCommitted = native_frame_retime::Commit(app, plan->native, nativeError);
        if (!nativeCommitted) cacheError = Utf8(nativeError);
        return nativeCommitted;
    });
    if (!committed) { error = Wide(reason); return false; }
    plan->consumed = true;
    app.CurrentFrame() = plan->after.frame;
    app.TimelineStartFrame() = plan->after.timeline;
    app.PlaybackStartSeconds() = plan->after.first;
    app.PlaybackEndSeconds() = plan->after.last;
    app.PlaybackCursorSeconds() = plan->after.frame / 30.0;
    app.PhysicsResetPending() = 1;
    app.SceneModified() = 1;
    app.state.messageSeen = 1;
    // Do not redraw while the cache transaction holds its mutex. Refreshing
    // native controls is best-effort after the irrevocable successful commit.
    if (app.MainWindow() && IsWindow(app.MainWindow())) {
        try {
            curve_editor::CloseOwned(app.MainWindow());
            SetDlgItemInt(app.MainWindow(), panel::kCurrentFrameEdit, app.CurrentFrame(), FALSE);
            RefreshAfterFrameApply(&app);
            InvalidateRect(app.MainWindow(), nullptr, FALSE);
        } catch (...) { /* The committed animation must not be reported as failed. */ }
    }
    return true;
}

void RequestSaveAs(MMDApp& app) {
    // The same function as File > Save As (command 0xD0). It preserves the
    // existing PMM/EMM/VMM bundled-save behavior and normal cancellation.
    CmdSaveScene(&app);
}
}
}
