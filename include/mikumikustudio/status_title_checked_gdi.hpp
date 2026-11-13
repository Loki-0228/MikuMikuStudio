#pragma once

#include "mikumikustudio/offscreen_gdi_owner.hpp"
#include "mikumikustudio/offscreen_gdi_runtime.hpp"
#include "mikumikustudio/ui_translation.hpp"

namespace mikumikustudio {
// The owner must outlive any failed cleanup and be retried on its UI thread.
// Checked functions operate only within the owner's synchronous draw session.
// Failure discards partial pixels; no view or native handle may escape it.
using StatusTitleGdiOwner = OffscreenGdiOwner<16>;
using StatusTitleGdiSession = StatusTitleGdiOwner::DrawSession;
bool DrawStatusGlyphChecked(StatusTitleGdiSession&, MMDApp*, const char*,
    int size, int x, int y, unsigned char r, unsigned char g, unsigned char b, int bold, int& advance) noexcept;
bool DrawStatusWideGlyphChecked(StatusTitleGdiSession&, const wchar_t*,
    int size, int x, int y, unsigned char r, unsigned char g, unsigned char b, int bold, int& advance,
    const wchar_t* face = L"Microsoft YaHei UI") noexcept;
bool DrawStatusUiGlyphChecked(StatusTitleGdiSession&, MMDApp*, const char*,
    int size, int x, int y, unsigned char r, unsigned char g, unsigned char b, int bold, int& advance,
    UiGlyphFace face = UiGlyphFace::Label) noexcept;
// Experimental off-screen path; saving messages are deliberately rejected.
bool PaintStatusTextChecked(StatusTitleGdiOwner&, OffscreenSurfaceHandle, MMDApp*) noexcept;
// Experimental: same owned DIB holds the checked native gradient and title.
// `changed` is the RGB difference bounding box in the title clip; an empty
// rectangle means no changed pixel. Output is unchanged on failure. The
// caller must discard the surface after failure and must validate title state
// and layout again before any later use. This does not publish UI geometry.
bool CaptureStatusTitleDifference(OffscreenGdiRuntimeCore<16>&, OffscreenRuntimeHandle,
    MMDApp*, RECT& changed) noexcept;
}
