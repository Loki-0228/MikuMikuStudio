#include "mikumikustudio/mmv4mms.hpp"
#include "mikumikustudio/shadow_filter.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/transparent_capture.hpp"
#include "mikumikustudio/transparent_sort_cache.hpp"
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/transparent_peeling.hpp"
#endif
// ===========================================================================
// VA 0x00440DB0 - PostDeviceReset (app method, thiscall this = Block)
//
// Releases every D3D resource that lives across a device reset (the overlay
// objects at app+0x9EB80/0x9EB88/0x9EB8C, the background texture at 0x9E3F0
// and its surface at 0x9E3F4, and the render-sub slots 0x1D534/0x1D538/
// 0x1D53C/0x1D548/0x1D550/0x1D554), drops the D3DX object at 0x1D560 into
// OnLostDevice, calls IDirect3DDevice9::Reset with the present parameters
// embedded at render-sub+0x1D4FC (the width/height pair the picture renderer
// and the canvas-size dialog grow), re-runs InitRenderStates (0x406E90),
// reapplies the 0x115-shadow-menu-keyed SetRenderState(0xA1, 0/1), brings the
// 0x1D560 object back through OnResetDevice and finally refreshes the
// viewport (0x4290F0 when app+0xA0274 fullscreen flag is set, else 0x42C810).
//
// Reached from the render-to-picture command (0x114), the canvas-size dialog
// (0xD4), the AVI record starters (0x45E820/0x464760) and the frame driver's
// device-lost recovery.  The former PostDeviceReset stub twin in
// src/unported/stubs.cpp was removed; this is the single definition now.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <cstdint>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "../app/device_lost_wait.hpp"

namespace mikumikustudio {
namespace {

template <typename ComPtr>
void ReleaseSubSlot(ComPtr& slot) {
    if (slot != nullptr) {
        slot->Release();
        slot = nullptr;
    }
}

}  // namespace

bool PostDeviceReset(MMDApp* app) {
    transparent_sort_cache::Release(app);
    model_vertex_cache::Clear();
    device_lost_wait::ResetScope recovery(app);
    viewport_render_scale::Release(app);
    antialias::Release(app);
    ReleaseTransparentCapture(app);
    mmv4mms::DeviceLost();
    native_mesh::DeviceLost();
    ReleaseSubSlot(app->CaptureTexture());                        // 0x440DC8
    ReleaseSubSlot(app->AviBackgroundTexture());                  // 0x440DE0
    ReleaseSubSlot(app->AviBackgroundSurface());                  // 0x440DF8
    ReleaseSubSlot(app->CaptureRenderTarget());                   // 0x440E10
    ReleaseSubSlot(app->CaptureSystemSurface());                  // 0x440E28

    D3DRenderer* r = app->Renderer();
    shadow_filter::ReleaseRenderer(r);
    if (r == nullptr)
        return false;
    // The capture render target is not rebuilt here; recording consumers
    // recreate it on demand via EnsureCaptureRenderTarget. Before that
    // path existed, nothing rebuilt it; a device loss during a recording then left
    // every consumer holding a released surface.
    // Defect RB-04.
    ReleaseSubSlot(r->captureSurface);                            // 0x440E4A
    ReleaseSubSlot(r->depthStencilSurface);                       // 0x440E72
    ReleaseSubSlot(r->backbufferSurface);                         // 0x440E9A
    ReleaseSubSlot(r->shadowSurface);                             // 0x440EC2
    ReleaseSubSlot(r->hdrTexture);                                // 0x440EEA
    ReleaseSubSlot(r->shadowDepthSurface);                        // 0x440F12

    // A vtable entry is the function address, not a pointer to that address.
    // Use the shared, architecture-independent COM dispatch helpers.
#if !MMD_ORIGINAL_PROFILE
    ClearTransparentPeelingCache();
#endif
    if (r->effect != nullptr)
        d3dx::FxOnLostDevice(r->effect);

    IDirect3DDevice9* device = r->device;
    if (device == nullptr)
        return false;
    const HRESULT reset = device->Reset(&r->presentParameters);    // 0x440F57
    runtime_log::Write("DEVICE_RESET size=%ux%u hr=0x%08lX",
        r->presentParameters.BackBufferWidth, r->presentParameters.BackBufferHeight,
        static_cast<unsigned long>(reset));
    runtime_log::Trace("DEVICE_RESET size=%ux%u hr=0x%08lX",
        r->presentParameters.BackBufferWidth,
        r->presentParameters.BackBufferHeight,
        static_cast<unsigned long>(reset));
    // A failed Reset leaves the device lost. Do not ask effects to recreate
    // resources or issue rendering calls until a later reset succeeds.
    if (FAILED(reset))
        return false;

    InitRenderStates(app);                                        // 0x406E90

    const HWND hwnd = static_cast<HWND>(app->state.hwnd);
    const HMENU menu = app->FullscreenMode() != 0 ? app->state.savedMenu : GetMenu(hwnd);
    const UINT menuState = GetMenuState(menu, 0x115, 0);// 0x440F7C
    device->SetRenderState(                                       // 0x440FAD
        static_cast<D3DRENDERSTATETYPE>(0xA1),
        (menuState & 8) != 0 ? 1 : 0);
    antialias::ApplyRenderState(app);

    if (r->effect != nullptr) {
        const HRESULT effectReset = d3dx::FxOnResetDevice(r->effect);
        if (FAILED(effectReset)) {
            runtime_log::Write("EFFECT_RESET hr=0x%08lX", static_cast<unsigned long>(effectReset));
            return false;
        }
    }

    if (app->FullscreenMode() != 0) {
        RefreshSeparateWindowViewport(app);                                           // 0x4290F0
    } else {
        RefreshMainWindowViewport(app);                                           // 0x42C810
    }

    app->state.recentFile0[0] = 0;                                 // 0x440FE2
    app->state.recentFile1[0] = 0;
    app->state.recentFile2[0] = 0;
    recovery.Successful();
    return true;
}

}  // namespace mikumikustudio
