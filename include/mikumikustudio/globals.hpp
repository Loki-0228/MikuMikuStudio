// ===========================================================================
// MikuMikuStudio - standalone .data/.rdata globals recovered from the original
// ===========================================================================
#pragma once

namespace mikumikustudio {

// VA 0x005294C8 - numerator of the quaternion->matrix scale factor
// (factor = g_QuatScaleFactor / (qx^2+qy^2+qz^2+qw^2)).  x64 empirical
// (2026-09-07): the x64 image has NO writable global for this - the
// quaternion->matrix code reads 2.0f directly from the constant pool at
// 0x140132B08 (mulss x N at 0x140007040..0x14000705E, function start
// 0x14000701F; also 0x140001079/0x1400073F2).  The runtime-init global is
// an x86-side artifact; the x86 write site at 0x5294C8 remains unverified
// No x86 binary is available. Defect HB-11.
extern float g_QuatScaleFactor;

// VA 0x00529688 - L"%s%s"; used by WinMain's no-command-line branch as the
// swprintf_s format for the env file name buffer.  The original calls it
// with zero varargs (verified in disassembly at 0x004C44CA..0x004C44DA) -
// behaviour intentionally preserved.
extern const wchar_t g_SourceFormat[];

// VA 0x00529679 - "" (empty ANSI string); format source for the three
// recent-file buffers in InitDefaults (sprintf_s(..., Locale)).
extern const char g_Locale[];

// VA 0x0052B9F0 - 4294967296.0f (2^32): negative-frame wrap fix in the
// animation-frame section of the frame driver (0x0046B090).
extern const float g_Wrap32;

// VA 0x0052BA68 - timeline frame scale.  x64 empirical (2026-09-07): the
// x64 image has NO writable global for this - 30.0f lives in the constant
// pool at 0x140132A64 and is consumed by the frame/timeline code
// (0x1400286FF/0x140028A4B/0x140028B06/0x1400295F7/0x140029777/
// 0x14002989B/0x140029983) and by the InitDefaults path at 0x14000A606
// (movss xmm3,[rip+...]; function start 0x14000A3FD).  The x86 0x52BA68
// Write site is unverified (no x86 binary available); the port keeps
// The writable global used for x86 parity. Defect HB-11.
extern float g_FrameScale;

// The FrameDriver's var_14C8 dt budget (0x46EFDE/0x46F00D): initialised to
// the real elapsed time (or 1/fps in frame-step recording mode), decremented
// by 1/60 per catch-up substep (0x46F226 in BLOCK 1 / the BLOCK 2 twin) and
// consumed by the settle section's main step (0x46FDBB) as its timeStep -
// i.e. the settle steps only the REMAINDER of the frame's dt, not the full
// dt again. Shared between PlaybackCatchup and PhysicsFrame (the original
// keeps it in one stack slot of sub_46B090). This port keeps it in this global
// because the two consumers live in different translation units. It starts at
// 0.0f, and every frame's first assignment is made in
// src/app/playback_catchup.cpp before PhysicsFrame reads it, so the settle
// step never sees the zero initialiser in the current frame order. Defect
// AB-07: the comment here once claimed the prefill preserved the full real dt,
// which 0.0f does not.
extern float g_CatchupDtBudget;

// VA 0x0052B760 / 0x0052B768 - exact double registry-echo conversion pair
// used by the frame-driver edit boxes: value / PI * 180.
extern double g_ConvA52B760;
extern double g_ConvB52B768;

// VA 0x0052B8F0 - mode-3 view-plane rotation scale (atan2 result multiplier).
// Initialized at runtime in the original; the port sets it to 0.5 in
// src/app/frame_modes.cpp, derived from the sibling drag scales. The original's
// runtime value is unverified (no write site found).  Defect HB-10.
extern double g_MouseScaleA;

}  // namespace mikumikustudio
