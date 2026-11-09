#pragma once

namespace mikumikustudio {
class MMDApp;

namespace playback_timing_ui {
void RefreshMenu(MMDApp* app);
bool HandleCommand(MMDApp* app, unsigned command);
}
}
