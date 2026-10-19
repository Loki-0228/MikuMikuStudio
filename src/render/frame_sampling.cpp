#include "mikumikustudio/frame_sampling.hpp"
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"

namespace mikumikustudio {
void RenderFrameSceneForOutput(MMDApp* app) {
    RenderFrameScene(app);
    if (!app->RecordingWindow()) return;

    struct RestoreDelta {
        MMDApp* app;
        float value;
        ~RestoreDelta() { app->DeltaTime() = value; }
    } restoreDelta{app, app->DeltaTime()};
    // AA warm-up samples share one output timestamp and do not advance effects.
    app->DeltaTime() = 0;

    // A sequence retains its history between output frames. Its first frame (or
    // a reset) needs warm-up, including a project containing only frame zero.
    // Keep this next to the sole render entry, not in readback or file encoding.
    const unsigned required = antialias::RequiredSamples(app);
    for (unsigned pass = 1; pass < required; ++pass) {
        const unsigned before = antialias::AccumulatedSamples(app);
        if (!before || before >= required) break;
        app->state.messageSeen = 1;
        RenderFrameScene(app);
        // Unsupported motion or a failed frame cannot converge. Keep the
        // spatial fallback and do not render it repeatedly for each image.
        if (antialias::AccumulatedSamples(app) <= before) break;
    }
}
}
