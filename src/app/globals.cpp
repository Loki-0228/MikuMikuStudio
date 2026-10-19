// ===========================================================================
// Standalone globals (definitions)
// ===========================================================================
#include "mikumikustudio/globals.hpp"

namespace mikumikustudio {

// VA 0x005294C8; .rdata image 0.0f, runtime-set to 2.0f (initializer port
// tracked in docs/PORTING_STATUS.md under "globals init").
float g_QuatScaleFactor = 2.0f;

// VA 0x00529688 - L"%s%s"
const wchar_t g_SourceFormat[] = L"%s%s";

// VA 0x00529679 - ""
const char g_Locale[] = "";

// VA 0x0052B9F0 - 2^32
const float g_Wrap32 = 4294967296.0f;

// VA 0x0052BA68 - runtime-initialized; 30.0 = MMD default timeline FPS
float g_FrameScale = 30.0f;

// var_14C8 of sub_46B090 - see globals.hpp. Statically zero-initialised:
// PlaybackCatchup sets the frame's real dt (or 1/fps in frame-step
// recording mode) before PhysicsFrame uses it as the settle step's
// timeStep, so this initial value is never observable in the current frame order.
// Defect AB-07: the earlier comment claimed the prefill kept the full real dt,
// which 0.0f does not.
float g_CatchupDtBudget = 0.0f;

}  // namespace mikumikustudio
