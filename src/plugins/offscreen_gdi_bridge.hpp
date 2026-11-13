#pragma once

#include "mikumikustudio/offscreen_gdi_runtime.hpp"

namespace mikumikustudio {
class MMDApp;
namespace plugins {
// Internal UI-thread-only access. No surface or state is published to plugins.
// A null result means the presenter must not attempt off-screen measurement.
OffscreenGdiRuntimeCore<16>* OffscreenRuntimeForUi(MMDApp*) noexcept;
// Call on the same UI thread when a surface release fails. Cleanup is
// coalesced and retried at a safe point; the runtime retains all handles.
void RequestOffscreenCleanup(MMDApp*) noexcept;
}
}
