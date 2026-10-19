#pragma once
namespace mikumikustudio {
class MMDApp;
// Renders one output frame, completing only a newly started temporal history.
// Extra samples keep the same pose/time and never run the playback/physics pump.
void RenderFrameSceneForOutput(MMDApp* app);
}
