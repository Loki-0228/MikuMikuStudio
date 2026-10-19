#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <climits>

#include "mikudancestudio/frame_presentation.hpp"
#include "mikudancestudio/frame_rate.hpp"
#include "mikudancestudio/build_profile.hpp"
#include "mikudancestudio/viewport_render_scale.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/window_dpi_scope.hpp"
#include "mikudancestudio/plugin_runtime.hpp"
#include "mikudancestudio/viewport_bounds.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"

namespace mikudancestudio {
namespace {
thread_local bool nativeBakePreview=false;
thread_local FrameSceneOutputScope* sceneOutput=nullptr;
thread_local FrameViewportPublicationWatch* viewportPublication=nullptr;
bool DeviceViewportRect(MMDApp* app,RECT& rect) {
    rect={};
    auto* renderer=app?app->Renderer():nullptr;
    if(!renderer||!renderer->device)return false;
    D3DVIEWPORT9 viewport{};
    IDirect3DSurface9* target=nullptr;
    D3DSURFACE_DESC desc{};
    const auto result=renderer->device->GetRenderTarget(0,&target);
    const bool described=SUCCEEDED(result)&&target&&SUCCEEDED(target->GetDesc(&desc));
    if(target)target->Release();
    if(!described||FAILED(renderer->device->GetViewport(&viewport))||!viewport.Width||!viewport.Height||
        viewport.X>=desc.Width||viewport.Y>=desc.Height||viewport.Width>desc.Width-viewport.X||
        viewport.Height>desc.Height-viewport.Y||viewport.X>LONG_MAX||viewport.Y>LONG_MAX||
        viewport.Width>DWORD(LONG_MAX)-viewport.X||viewport.Height>DWORD(LONG_MAX)-viewport.Y)return false;
    rect={LONG(viewport.X),LONG(viewport.Y),LONG(viewport.X+viewport.Width),LONG(viewport.Y+viewport.Height)};
    return true;
}
}
bool NativeBakePreviewActive(){return nativeBakePreview;}
bool BakePreview::Due(bool last){
    if(!app_||!app_->Renderer()||!app_->Renderer()->device)return false;
    const auto now=GetTickCount64();
    if(!last&&now<next_)return false;
    next_=now+66;return true;
}
void BakePreview::Present(){
    if(!app_||!app_->Renderer()||!app_->Renderer()->device)return;
    if(!FrameViewportRenderable(app_))return;
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

static bool ReadFrameGeometry(MMDApp* app,FrameViewportGeometry& geometry) {
    const bool fullscreen=app->FullscreenMode()!=0;
    const HWND window=fullscreen||!app->FloatingWindow()?app->MainWindow():app->FloatingWindow();
    const WindowDpiScope dpi(window);
    RECT client{};
    if(!window||IsIconic(window)||!GetClientRect(window,&client)){geometry={};return false;}
    return CalculateFrameViewport(client,app->SidebarWidth(),plugins::ToolbarHeight(app),
        fullscreen?FrameViewportMode::Fullscreen:app->FloatingWindow()?FrameViewportMode::Floating:FrameViewportMode::Docked,
        app->RenderWidth(),app->RenderHeight(),geometry);
}

static RECT BasePresentationRect(MMDApp* app) {
    RECT result = app->ViewportRect();
    if (app->RecordingWindow() != nullptr) return result;
    if(!build_profile::original){
        FrameViewportGeometry geometry;
        return ReadFrameGeometry(app,geometry)?geometry.available:RECT{};
    }
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
        result.top = std::min<LONG>(25 + plugins::ToolbarHeight(app), client.bottom);
        result.bottom = std::max(result.top, client.bottom - (floating ? 32 : 192));
    }
    return result;
}

ViewportPanelLayout FramePanelLayout(MMDApp* app) {
    if(!build_profile::original&&!app->RecordingWindow()){
        FrameViewportGeometry geometry;
        if(!ReadFrameGeometry(app,geometry)||!FrameRectWithin(app->ViewportRect(),geometry.available))return {};
        const bool adaptive=app->FullscreenMode()==0;
        auto panels=MakeViewportPanelLayout(geometry.available,app->ViewportRect(),adaptive,app->state.optflag[0]==0,
            adaptive&&app->state.optflag[0]==0?ViewportScaleCaptionWidth(app):36);
        if(!FrameRectWithin(panels.overlay,geometry.presentationBounds))return {};
        return panels;
    }
    const bool adaptive = !build_profile::original && !app->FullscreenMode() &&
        !app->RecordingWindow() && (app->MainWindow() || app->FloatingWindow());
    return MakeViewportPanelLayout(build_profile::original ? app->ViewportRect() : BasePresentationRect(app),
        app->ViewportRect(), adaptive, app->state.optflag[0] == 0,
        adaptive && app->state.optflag[0] == 0 ? ViewportScaleCaptionWidth(app) : 36);
}

RECT FramePresentationRect(MMDApp* app) {
    return build_profile::original ? BasePresentationRect(app) : FramePanelLayout(app).overlay;
}

bool FrameViewportRenderable(MMDApp* app) {
    if(!app)return false;
    if(build_profile::original||app->RecordingWindow())return true;
    for(auto* output=sceneOutput;output;output=output->previous_)
        if(output->app_==app&&output->borrowedViewport_)return false;
    const auto rect=FramePresentationRect(app);
    const auto* renderer=app->Renderer();
    if(!renderer||!renderer->device)return false;
    if(IsRectEmpty(&rect))return false;
    IDirect3DSurface9* back=nullptr;D3DSURFACE_DESC desc{};
    HRESULT result=renderer->device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);
    if(SUCCEEDED(result))result=back?back->GetDesc(&desc):E_FAIL;
    if(back)back->Release();
    if(FAILED(result)){
        // Requested presentation parameters can precede a failed Reset. Read
        // actual surface dimensions, but leave lost-device detection/recovery
        // reachable through the ordinary Present path.
        const auto cooperative=renderer->device->TestCooperativeLevel();
        return result==D3DERR_DEVICELOST||result==D3DERR_DEVICENOTRESET||
            cooperative==D3DERR_DEVICELOST||cooperative==D3DERR_DEVICENOTRESET;
    }
    if(!desc.Width||!desc.Height||desc.Width>LONG_MAX||desc.Height>LONG_MAX)return false;
    const RECT buffer{0,0,LONG(desc.Width),LONG(desc.Height)};
    return FrameRectWithin(rect,buffer)&&FrameRectWithin(app->ViewportRect(),buffer);
}

FrameSceneOutputScope::FrameSceneOutputScope(MMDApp* app):app_(app) {
    if(!app)return;
    if(!FrameViewportRenderable(app)){
        if(!DeviceViewportRect(app,borrowed_))return;
        saved_=app->ViewportRect();app->ViewportRect()=borrowed_;borrowedViewport_=restoreViewport_=true;
    }
    previous_=sceneOutput;sceneOutput=this;entered_=true;ready_=true;
}
FrameSceneOutputScope::~FrameSceneOutputScope(){
    if(!entered_)return;
    // A synchronous resize can publish newer geometry while an output callback
    // is running. Restore only the rectangle this scope still owns.
    if(restoreViewport_&&EqualRect(&app_->ViewportRect(),&borrowed_))app_->ViewportRect()=saved_;
    if(sceneOutput==this)sceneOutput=previous_;
}
void FrameViewportPublished(MMDApp* app) noexcept {
    FrameEditorViewportRefreshed(app);
}
void FrameEditorViewportRefreshed(MMDApp* app) noexcept {
    // Notify the whole chain: a nested same-app scope cannot pass the old
    // restore ownership back to an outer scope. Other apps remain untouched.
    for(auto* output=sceneOutput;output;output=output->previous_)
        if(output->app_==app)output->restoreViewport_=false;
    for(auto* watch=viewportPublication;watch;watch=watch->previous_)
        if(watch->app_==app)watch->observed_=true;
}
bool FrameEditorViewportRebuilding(MMDApp* app) noexcept {
    for(auto* watch=viewportPublication;watch;watch=watch->previous_)
        if(watch->app_==app&&watch->rebuilding_)return true;
    return false;
}
FrameViewportPublicationWatch::FrameViewportPublicationWatch(MMDApp* app) noexcept:app_(app) {
    previous_=viewportPublication;viewportPublication=this;
}
FrameViewportPublicationWatch::~FrameViewportPublicationWatch(){
    if(viewportPublication==this)viewportPublication=previous_;
}
void FrameViewportPublicationWatch::PreserveGeometryInto(MMDAppState& restored) const noexcept {
    if(!app_||!observed_||build_profile::original)return;
    const auto& live=app_->state;
    // Only the fixed-context UI geometry closure belongs to the refresh.
    // Output dimensions, HWND/menu ownership and sampled scene state do not.
    restored.hideRight=live.hideRight;restored.hideTop=live.hideTop;
    restored.hideLeft=live.hideLeft;restored.hideBottom=live.hideBottom;
    restored.sidebarWidth=live.sidebarWidth;restored.sidebarRatio=live.sidebarRatio;
    restored.separateWindowSidebarWidth=live.separateWindowSidebarWidth;
    restored.windowLayoutReady=live.windowLayoutReady;
}
void FrameViewportPublicationWatch::RestoreEditorUi() noexcept {
    if(!app_||!observed_||build_profile::original||!IsWindow(app_->MainWindow()))return;
    const bake::ScopedUiInputGate inputGate;
    const auto ready=app_->WindowLayoutReady();
    const bool relayout=app_->bottomPanelSizing.relayout;
    const auto selecting=app_->SelectionBoxDragging();
    const float rotation[]{app_->BoneRotationEditDegreesX(),app_->BoneRotationEditDegreesY(),app_->BoneRotationEditDegreesZ()};
    const bool rebuilding=rebuilding_;
    rebuilding_=true;app_->bottomPanelSizing.relayout=true;
    app_->SelectionBoxDragging()=0;app_->WindowLayoutReady()=0;
    bool restored=false;
    try{
        // Use the restored mode/frame, not the sampling callback's camera
        // mode. The relayout flag also avoids a lossy sidebar ratio round trip.
        // Refresh* observes rebuilding_ and leaves accepted/empty geometry and
        // the actual device viewport untouched throughout this UI-only pass.
        HandleWindowSize(app_);
        if(app_->Renderer()&&app_->PanelDC())PanelPaint(app_);
        restored=true;
    }catch(...){
        // Scene restoration must still complete during cancellation/unwind.
        // An incomplete UI pass cannot make a stale presentation usable.
        app_->ViewportRect()={};
    }
    app_->BoneRotationEditDegreesX()=rotation[0];app_->BoneRotationEditDegreesY()=rotation[1];
    app_->BoneRotationEditDegreesZ()=rotation[2];app_->SelectionBoxDragging()=selecting;
    app_->bottomPanelSizing.relayout=relayout;rebuilding_=rebuilding;
    app_->WindowLayoutReady()=restored?ready:0;
    RedrawWindow(app_->MainWindow(),nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
}
bool FrameSceneRenderable(MMDApp* app){
    if(FrameViewportRenderable(app))return true;
    if(!sceneOutput||!sceneOutput->ready_||sceneOutput->app_!=app)return false;
    RECT current{};
    return DeviceViewportRect(app,current)&&EqualRect(&current,&app->ViewportRect());
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
    if(!build_profile::original&&IsRectEmpty(&present))return;
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

}  // namespace mikudancestudio
