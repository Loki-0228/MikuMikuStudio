#include "mikumikustudio/frame_rate.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include <chrono>

namespace mikumikustudio::frame_rate {
namespace {
double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Mirror(MMDApp* app) {
    app->FpsOverlayElapsedSeconds() = float(app->frameRate.elapsed);
    app->state.fpsOverlayFrameCount = app->frameRate.frames;
}
}
void Reset(MMDApp* app) {
    if (build_profile::original || !app) return;
    app->frameRate = {};
    app->frameRate.Tick(Now());
    app->FramesPerSecond() = 0;
    Mirror(app);
}
void Tick(MMDApp* app) {
    if (build_profile::original || !app) return;
    if (app->RecordingWindow()) {
        // Offline frames and their convergence samples are not editor FPS.
        Reset(app);
        return;
    }
    if (app->frameRate.Tick(Now())) app->FramesPerSecond() = app->frameRate.rate;
    Mirror(app);
}
void BeginScene(MMDApp* app) {
    if (build_profile::original || !app) return;
    Tick(app);
    app->frameRate.Begin();
}
void CompleteScene(MMDApp* app) {
    if (build_profile::original || !app || app->RecordingWindow()) return;
    app->frameRate.Complete();
}
void Presented(MMDApp* app, bool success) {
    if (build_profile::original || !app) return;
    Tick(app);
    if (!app->RecordingWindow()) app->frameRate.Present(success);
    Mirror(app);
}
}
