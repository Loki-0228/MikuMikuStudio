#pragma once

namespace mikumikustudio {
class MMDApp;

namespace legacy_playback_map {
void Attach(MMDApp* app) noexcept;
void Detach(MMDApp* app) noexcept;
}
}
