#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <algorithm>

#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/frame_rate.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/window_dpi_scope.hpp"

namespace mikumikustudio {
namespace { thread_local bool nativeBakePreview=false; }
bool NativeBakePreviewActive(){return nativeBakePreview;}
bool BakePreview::Due(bool last){
    if(!app_||!app_->Renderer()||!app_->Renderer()->device)return false;
    const auto now=GetTickCount64();
    if(!last&&now<next_)return false;
    next_=now+66;return true;
}
void BakePreview::Present(){
    if(!app_||!app_->Renderer()||!app_->Renderer()->device)return;
    auto* device=app_->Renderer()->device;
    if (build_profile::original) FillFramePresentationMargins(app_,device);
    const RECT rect=FramePresentationRect(app_);
    const HWND window=app_->FloatingWindow()?app_->FloatingWindow():app_->MainWindow();
    const HRESULT presented=device->Present(&rect,&rect,window,nullptr);
    frame_rate::Presented(app_,SUCCEEDED(presented));
}
void BakePreview::RenderNative(){
    struct Guard{
        MMDApp* app;bool old=nativeBakePreview;
        int pass,count;void* object;
        explicit Guard(MMDApp* a):app(a),pass(a->state.activeRenderPass),count(a->state.renderPassCount),object(a->state.activeRenderObject){}
        ~Guard(){nativeBakePreview=old;app->state.activeRenderPass=pass;app->state.renderPassCount=count;app->state.activeRenderObject=object;}
    } guard(app_);
    nativeBakePreview=true;RenderFrameScene(app_);Present();
}

static RECT BasePresentationRect(MMDApp* app) {
    RECT result = app->ViewportRect();
    if (app->RecordingWindow() != nullptr) return result;
    const HWND window = app->FullscreenMode() != 0 || !app->FloatingWindow()
        ? app->MainWindow() : app->FloatingWindow();
    const WindowDpiScope dpi(window);
    if (app->FullscreenMode() != 0) {
        GetClientRect(app->MainWindow(), &result);
        return result;
    }

    const bool floating = app->FloatingWindow() != nullptr;
    RECT client{};
    if (!GetClientRect(window, &client))
        return result;

    // Keep the GDI controls above/below the scene outside the present region.
    result.left = std::max<LONG>(0, floating ? 0 : app->SidebarWidth() + 9);
    result.right = std::max(result.left, client.right);
    if (!build_profile::original) {
        result.top = std::min<LONG>(25, client.bottom);
        result.bottom = std::max(result.top, client.bottom - (floating ? 32 : 192));
    }
    return result;
}

ViewportPanelLayout FramePanelLayout(MMDApp* app) {
    const bool adaptive = !build_profile::original && !app->FullscreenMode() &&
        !app->RecordingWindow() && (app->MainWindow() || app->FloatingWindow());
    return MakeViewportPanelLayout(build_profile::original ? app->ViewportRect() : BasePresentationRect(app),
        app->ViewportRect(), adaptive, app->state.optflag[0] == 0,
        adaptive && app->state.optflag[0] == 0 ? ViewportScaleCaptionWidth(app) : 36);
}

RECT FramePresentationRect(MMDApp* app) {
    return build_profile::original ? BasePresentationRect(app) : FramePanelLayout(app).overlay;
}

RECT FrameOverlayRect(MMDApp* app) {
    return build_profile::original ? app->ViewportRect() : FramePresentationRect(app);
}

float FrameOverlayScale(MMDApp* app) {
    const float scale = app->Renderer() ? app->Renderer()->viewScale : 1.0f;
    const RECT frame = FrameOverlayRect(app);
    if (build_profile::original) return scale;
    const float frameWidth = float(frame.right - frame.left), frameHeight = float(frame.bottom - frame.top);
    // One HUD unit is one window pixel. Keep that size until the FPS and
    // operation block cannot sit side by side, or the block is too tall.
    return frameWidth > 0 && frameHeight > 0
        ? std::min({1.0f, frameWidth / 360.0f, frameHeight / 177.0f}) : 1.0f;
}

void FillFramePresentationMargins(MMDApp* app, IDirect3DDevice9* device) {
    if (viewport_render_scale::Rendering(app)) return;
    if (app->RecordingWindow() != nullptr) return;
    const RECT scene = app->ViewportRect();
    const RECT present = FramePresentationRect(app);
    if (EqualRect(&present, &scene))
        return;

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO,
                                    &backBuffer)))
        return;
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(backBuffer->GetDesc(&desc))) {
        const RECT bounds{0, 0, static_cast<LONG>(desc.Width),
                         static_cast<LONG>(desc.Height)};
        const RECT margins[] = {
            {present.left, present.top, present.right, scene.top},
            {present.left, scene.bottom, present.right, present.bottom},
            {present.left, present.top, scene.left, present.bottom},
            {scene.right, present.top, present.right, present.bottom},
        };
        for (const RECT& margin : margins) {
            RECT clipped{};
            if (IntersectRect(&clipped, &margin, &bounds))
                device->ColorFill(backBuffer, &clipped, D3DCOLOR_XRGB(0, 0, 0));
        }
    }
    backBuffer->Release();
    // Do not use Clear or change the viewport here: MME owns the active
    // render target, and a zero-rectangle Clear clears the entire scene.
}

}  // namespace mikumikustudio
