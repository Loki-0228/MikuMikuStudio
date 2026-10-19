#pragma once

#include <Windows.h>
#include "mikumikustudio/viewport_panel_layout.hpp"

struct IDirect3DDevice9;

namespace mikumikustudio {
class MMDApp;

// Present the side margins too, without changing the scene's aspect ratio.
RECT FramePresentationRect(MMDApp* app);
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
