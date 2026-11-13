#pragma once

#include <Windows.h>
#include "mikumikustudio/viewport_panel_layout.hpp"

struct IDirect3DDevice9;

namespace mikumikustudio {
class MMDApp;
struct MMDAppState;

// Present the side margins too, without changing the scene's aspect ratio.
RECT FramePresentationRect(MMDApp* app);
// Insufficient editor space suspends rendering/presentation. Recording keeps
// its independent output target, and hidden windows with real clients work.
// When requested, return the same presentation rectangle used for validation.
bool FrameViewportRenderable(MMDApp* app, RECT* presentation = nullptr);
// Render the current pose for an explicitly requested modal settings preview.
bool PresentViewportPreview(MMDApp* app);
// A successful native viewport publication supersedes borrowed geometry, even
// when its coordinates match the temporary rectangle byte for byte.
void FrameViewportPublished(MMDApp* app) noexcept;
// An editor refresh owns its final geometry even when it deliberately clears
// an unusable viewport. Temporary output borrowing does not send this signal.
void FrameEditorViewportRefreshed(MMDApp* app) noexcept;
// During restoration of sampled UI, layout must not retry a device viewport
// that the callback explicitly invalidated. This is scoped to the same app.
bool FrameEditorViewportRebuilding(MMDApp* app) noexcept;
// Private sampling restores scene state, but must retain real editor resize /
// sidebar publications made by a synchronous callback. This allocation-free
// watch observes explicit refreshes, including same-rectangle retries and
// invalidation, rather than comparing rectangles. Window lifetime/mode changes
// are outside this fixed-window-context restore policy.
class FrameViewportPublicationWatch {
    MMDApp* app_=nullptr;
    FrameViewportPublicationWatch* previous_=nullptr;
    bool observed_=false,rebuilding_=false;
    friend void FrameEditorViewportRefreshed(MMDApp* app) noexcept;
    friend bool FrameEditorViewportRebuilding(MMDApp* app) noexcept;
public:
    explicit FrameViewportPublicationWatch(MMDApp* app) noexcept;
    ~FrameViewportPublicationWatch();
    FrameViewportPublicationWatch(const FrameViewportPublicationWatch&)=delete;
    FrameViewportPublicationWatch& operator=(const FrameViewportPublicationWatch&)=delete;
    bool Observed() const noexcept {return observed_;}
    void PreserveGeometryInto(MMDAppState& restored) const noexcept;
    void RestoreEditorUi() noexcept;
};
// Output sampling opts in explicitly. A private render uses the last valid
// device viewport if editor space is unavailable, while presentation remains
// subject to FrameViewportRenderable.
class FrameSceneOutputScope {
    MMDApp* app_=nullptr;
    FrameSceneOutputScope* previous_=nullptr;
    RECT saved_{},borrowed_{};
    bool ready_=false,entered_=false,borrowedViewport_=false,restoreViewport_=false;
    friend bool FrameViewportRenderable(MMDApp* app, RECT* presentation);
    friend void FrameViewportPublished(MMDApp* app) noexcept;
    friend void FrameEditorViewportRefreshed(MMDApp* app) noexcept;
    friend bool FrameSceneRenderable(MMDApp* app);
public:
    explicit FrameSceneOutputScope(MMDApp* app);
    ~FrameSceneOutputScope();
    FrameSceneOutputScope(const FrameSceneOutputScope&)=delete;
    FrameSceneOutputScope& operator=(const FrameSceneOutputScope&)=delete;
    bool Ready() const noexcept {return ready_;}
};
bool FrameSceneRenderable(MMDApp* app);
ViewportPanelLayout FramePanelLayout(MMDApp* app);
int ViewportScaleCaptionWidth(MMDApp* app);
// Editor controls use the whole presentation area, including its black margins.
// The original profile retains its scene-bound overlay layout.
RECT FrameOverlayRect(MMDApp* app);
float FrameOverlayScale(MMDApp* app);
void FillFramePresentationMargins(MMDApp* app, IDirect3DDevice9* device);
// Bake previews present completed samples without driving playback or physics.
bool NativeBakePreviewActive();
class BakePreview {
    MMDApp* app_;
    ULONGLONG next_=0;
public:
    explicit BakePreview(MMDApp* app):app_(app){}
    bool Due(bool last=false);
    void Present();
    void RenderNative();
};

}  // namespace mikumikustudio
