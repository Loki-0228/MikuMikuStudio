// Included after image_sequence_continuity_checks.hpp inside its fixture
// namespace. Uses the existing real host, encoders, decoder and hidden windows.
namespace image_export_render_state_checks {
using Microsoft::WRL::ComPtr;
struct Probe {
    unsigned scenes = 0, shadows = 0;
    unsigned captureCreated = 0, captureBound = 0;
    bool cold = false, recreated = false;
};
Probe* probe = nullptr;
decltype(&RenderFrameScene) originalScene = nullptr;
decltype(&RenderShadowMap) originalShadow = nullptr;

void ObserveScene(MMDApp* app) {
    if (probe) {
        ++probe->scenes;
        if (probe->cold && probe->scenes == 1) {
            auto* model = app->ModelSlot(0);
            Check(model && mdl::Mdl(model)->boneCount && mdl::Bones(model),
                "cold image scene still owns its loaded model");
            bool nonzero = false;
            for (float value : mdl::Bones(model)[0].matInit) nonzero |= value != 0;
            Check(nonzero, "first image initializes the bone matrix before GPU submission");
        }
    }
    originalScene(app);
}

void ObserveShadow(MMDApp* app, const float* frame) {
    if (!probe) { originalShadow(app, frame); return; }
    auto* device = app->Renderer()->device;
    ComPtr<IDirect3DSurface9> before, depthBefore, after, depthAfter;
    D3DVIEWPORT9 viewportBefore{}, viewportAfter{};
    Check(SUCCEEDED(device->GetRenderTarget(0, &before)) &&
        SUCCEEDED(device->GetViewport(&viewportBefore)), "read incoming image shadow target");
    device->GetDepthStencilSurface(&depthBefore);
    if (probe->recreated) {
        Check(app->Renderer()->captureSurface &&
            before.Get() == app->Renderer()->captureSurface,
            "recording reset recreates and binds capture target before shadow entry");
        D3DSURFACE_DESC colour{}, depth{};
        const HRESULT colourResult = before->GetDesc(&colour);
        const HRESULT depthResult = depthBefore ? depthBefore->GetDesc(&depth) : D3DERR_NOTFOUND;
        const auto* renderer = app->Renderer();
        std::printf("TRACE capture-reset created=%u bound=%u colour_hr=%08lX depth_hr=%08lX "
            "rt=%ux%u fmt=%u msaa=%u/%lu depth=%ux%u fmt=%u msaa=%u/%lu "
            "screen=%dx%d pp=%ux%u msaa=%u/%lu viewport=%u,%u,%u,%u\n",
            probe->captureCreated, probe->captureBound, colourResult, depthResult,
            colour.Width, colour.Height, unsigned(colour.Format), unsigned(colour.MultiSampleType), colour.MultiSampleQuality,
            depth.Width, depth.Height, unsigned(depth.Format), unsigned(depth.MultiSampleType), depth.MultiSampleQuality,
            renderer->screenWidth, renderer->screenHeight,
            renderer->presentParameters.BackBufferWidth, renderer->presentParameters.BackBufferHeight,
            unsigned(renderer->presentParameters.MultiSampleType), renderer->presentParameters.MultiSampleQuality,
            viewportBefore.X, viewportBefore.Y, viewportBefore.Width, viewportBefore.Height);
        Check(probe->captureCreated && probe->captureBound && depthBefore &&
            SUCCEEDED(colourResult) && SUCCEEDED(depthResult) &&
            colour.MultiSampleType == D3DMULTISAMPLE_NONE && colour.MultiSampleQuality == 0 &&
            depth.MultiSampleType == colour.MultiSampleType && depth.MultiSampleQuality == colour.MultiSampleQuality &&
            depth.Width >= colour.Width && depth.Height >= colour.Height,
            "reset recording uses a successfully created and bound compatible non-MSAA colour/depth pair");
    }
    if (TransparentExportActive(app))
        Check(before.Get() == TransparentCaptureSurface(app),
            "native-AA PNG shadow starts on the transparent scene target");
    originalShadow(app, frame);
    ++probe->shadows;
    Check(SUCCEEDED(device->GetRenderTarget(0, &after)) &&
        SUCCEEDED(device->GetViewport(&viewportAfter)), "read restored image shadow target");
    device->GetDepthStencilSurface(&depthAfter);
    Check(before.Get() == after.Get() && depthBefore.Get() == depthAfter.Get() &&
        !std::memcmp(&viewportBefore, &viewportAfter, sizeof(viewportBefore)),
        "native shadow returns to the exact image RT depth and viewport");
}

using SetTarget = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetDepth = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*);
using Clear = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetViewport = HRESULT(WINAPI*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using CreateTarget = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT,
    D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
SetTarget originalTarget = nullptr;
SetDepth originalDepth = nullptr;
Clear originalClear = nullptr;
SetViewport originalViewport = nullptr;
CreateTarget originalCreateTarget = nullptr;
enum class Failure { None, Target, Depth, Clear, Viewport };
Failure failure = Failure::None;
IDirect3DSurface9* failingTarget = nullptr;
IDirect3DSurface9* failingDepth = nullptr;
unsigned injected = 0;
bool Reject(Failure value) {
    if (failure != value || injected) return false;
    ++injected; return true;
}
HRESULT WINAPI SetTargetProbe(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface) {
    if (index == 0 && surface == failingTarget && Reject(Failure::Target)) return E_FAIL;
    const HRESULT result = originalTarget(device, index, surface);
    if (probe && probe->recreated && index == 0 && surface &&
        surface == g_Block->Renderer()->captureSurface) {
        Check(SUCCEEDED(result), "actual device accepts the recreated capture render-target binding");
        ++probe->captureBound;
    }
    return result;
}
HRESULT WINAPI CreateTargetProbe(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format,
    D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL lockable, IDirect3DSurface9** surface, HANDLE* shared) {
    const HRESULT result = originalCreateTarget(device, width, height, format, samples, quality, lockable, surface, shared);
    if (probe && probe->recreated && lockable &&
        width == static_cast<UINT>(g_Block->Renderer()->screenWidth) &&
        height == static_cast<UINT>(g_Block->Renderer()->screenHeight)) {
        Check(SUCCEEDED(result) && surface && *surface && samples == D3DMULTISAMPLE_NONE && quality == 0,
            "actual device creates the requested lockable non-MSAA capture target");
        ++probe->captureCreated;
    }
    return result;
}
HRESULT WINAPI SetDepthProbe(IDirect3DDevice9* device, IDirect3DSurface9* surface) {
    if (surface == failingDepth && Reject(Failure::Depth)) return E_FAIL;
    return originalDepth(device, surface);
}
HRESULT WINAPI ClearProbe(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects,
                         DWORD flags, D3DCOLOR colour, float z, DWORD stencil) {
    ComPtr<IDirect3DSurface9> current;
    if (failure == Failure::Clear && SUCCEEDED(device->GetRenderTarget(0, &current)) &&
        current.Get() == failingTarget && Reject(Failure::Clear)) return E_FAIL;
    return originalClear(device, count, rects, flags, colour, z, stencil);
}
HRESULT WINAPI SetViewportProbe(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport) {
    ComPtr<IDirect3DSurface9> current;
    if (failure == Failure::Viewport && SUCCEEDED(device->GetRenderTarget(0, &current)) &&
        current.Get() == failingTarget && Reject(Failure::Viewport)) return E_FAIL;
    return originalViewport(device, viewport);
}

// Ordinary and checked entry points must restore exact objects on failures,
// including a valid scene that intentionally has no depth surface.
void ShadowTargets(MMDApp* app) {
    auto* renderer = app->Renderer();
    auto* device = renderer->device;
    Check(renderer->effect != nullptr && renderer->shadowSurface && renderer->shadowDepthSurface,
        "real image export established the native shadow resources");
    ComPtr<IDirect3DSurface9> previous, previousDepth;
    ComPtr<IDirect3DStateBlock9> state;
    D3DVIEWPORT9 previousViewport{};
    Check(SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &state)) &&
        SUCCEEDED(device->GetRenderTarget(0, &previous)) &&
        SUCCEEDED(device->GetViewport(&previousViewport)), "save caller GPU state for shadow cases");
    device->GetDepthStencilSurface(&previousDepth);
    const auto objectBefore = app->ActiveRenderObject();
    const auto passBefore = app->ActiveRenderPass();
    auto** table = *reinterpret_cast<void***>(device);
    void* entries[] = {table[37], table[39], table[43], table[47]};
    void* replacements[] = {reinterpret_cast<void*>(&SetTargetProbe), reinterpret_cast<void*>(&SetDepthProbe),
        reinterpret_cast<void*>(&ClearProbe), reinterpret_cast<void*>(&SetViewportProbe)};
    void** originals[] = {reinterpret_cast<void**>(&originalTarget), reinterpret_cast<void**>(&originalDepth),
        reinterpret_cast<void**>(&originalClear), reinterpret_cast<void**>(&originalViewport)};
    for (unsigned i = 0; i < std::size(entries); ++i)
        Check(MH_CreateHook(entries[i], replacements[i], originals[i]) == MH_OK &&
            MH_EnableHook(entries[i]) == MH_OK, "observe real shadow D3D boundary");
    failingTarget = renderer->shadowSurface; failingDepth = renderer->shadowDepthSurface;
    D3DMATRIX frame{};
    Check(SUCCEEDED(device->GetTransform(D3DTS_WORLD, &frame)), "read real shadow frame transform");
    D3DCAPS9 caps{}; device->GetDeviceCaps(&caps);
    ComPtr<IDirect3D9> d3d;
    Check(SUCCEEDED(device->GetDirect3D(&d3d)), "read real device capabilities");
    D3DDEVICE_CREATION_PARAMETERS creation{};
    Check(SUCCEEDED(device->GetCreationParameters(&creation)), "read device creation parameters");
    for (auto samples : {D3DMULTISAMPLE_NONE, D3DMULTISAMPLE_2_SAMPLES}) {
        DWORD quality = 0;
        if (samples != D3DMULTISAMPLE_NONE &&
            (FAILED(d3d->CheckDeviceMultiSampleType(creation.AdapterOrdinal, creation.DeviceType,
                D3DFMT_A8R8G8B8, TRUE, samples, &quality)) ||
             FAILED(d3d->CheckDeviceMultiSampleType(creation.AdapterOrdinal, creation.DeviceType,
                D3DFMT_D24S8, TRUE, samples, &quality)))) {
            std::puts("SKIP shadow incoming MSAA2: device does not support both colour and depth");
            continue;
        }
        ComPtr<IDirect3DSurface9> target, depth;
        Check(SUCCEEDED(device->CreateRenderTarget(96, 80, D3DFMT_A8R8G8B8,
            samples, 0, FALSE, &target, nullptr)), "create independent scene target");
        Check(SUCCEEDED(device->CreateDepthStencilSurface(96, 80, D3DFMT_D24S8,
            samples, 0, FALSE, &depth, nullptr)), "create independent scene depth");
        for (bool withDepth : {false, true}) for (bool checked : {false, true})
            for (auto fail : {Failure::None, Failure::Target, Failure::Depth, Failure::Clear, Failure::Viewport}) {
                failure = Failure::None;
                Check(SUCCEEDED(device->SetDepthStencilSurface(nullptr)) &&
                    SUCCEEDED(device->SetRenderTarget(0, target.Get())) &&
                    SUCCEEDED(device->SetDepthStencilSurface(withDepth ? depth.Get() : nullptr)),
                    "bind an independent incoming scene");
                const D3DVIEWPORT9 viewport{3, 5, 64, 48, .125f, .875f};
                D3DMATRIX projection{};
                projection._11 = 1.25f; projection._22 = .75f;
                projection._33 = .5f; projection._44 = 1;
                Check(SUCCEEDED(device->SetViewport(&viewport)) &&
                    SUCCEEDED(device->SetTransform(D3DTS_PROJECTION, &projection)),
                    "bind a nondefault scene viewport and projection");
                injected = 0; failure = fail;
                Check(SUCCEEDED(device->BeginScene()), "begin actual shadow test scene");
                bool result = true;
                if (checked) result = RenderShadowMapChecked(app, reinterpret_cast<const float*>(&frame));
                else RenderShadowMap(app, reinterpret_cast<const float*>(&frame));
                Check(SUCCEEDED(device->EndScene()), "end actual shadow test scene");
                failure = Failure::None;
                Check(injected == (fail == Failure::None ? 0u : 1u), "failure injection reaches its requested shadow boundary");
                if (checked) Check(result == (fail == Failure::None), "checked shadow reports the actual failed boundary");
                ComPtr<IDirect3DSurface9> actual, actualDepth;
                D3DVIEWPORT9 actualViewport{}; D3DMATRIX actualProjection{};
                Check(SUCCEEDED(device->GetRenderTarget(0, &actual)) &&
                    SUCCEEDED(device->GetViewport(&actualViewport)) &&
                    SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &actualProjection)),
                    "read actual state after shadow entry or early exit");
                const HRESULT gotDepth = device->GetDepthStencilSurface(&actualDepth);
                Check((withDepth ? SUCCEEDED(gotDepth) : gotDepth == D3DERR_NOTFOUND) &&
                    actual.Get() == target.Get() && actualDepth.Get() == (withDepth ? depth.Get() : nullptr) &&
                    !std::memcmp(&actualViewport, &viewport, sizeof viewport) &&
                    !std::memcmp(&actualProjection, &projection, sizeof projection),
                    "shadow success and failure restore exact offscreen RT depth viewport projection");
            }
    }
    failure = Failure::None; failingTarget = failingDepth = nullptr;
    for (auto* entry : entries) { MH_DisableHook(entry); MH_RemoveHook(entry); }
    device->SetDepthStencilSurface(nullptr);
    device->SetRenderTarget(0, previous.Get());
    device->SetDepthStencilSurface(previousDepth.Get());
    state->Apply(); device->SetViewport(&previousViewport);
    app->ActiveRenderObject() = objectBefore; app->ActiveRenderPass() = passBefore;
    std::puts("PASS real native shadow targets, no-depth/MSAA input, checked and ordinary failure restoration");
}
} // namespace image_export_render_state_checks

void CheckImageExportRenderState(MMDApp* app) {
    using namespace image_export_render_state_checks;
    wchar_t modelPath[32768]{};
    const auto length = GetEnvironmentVariableW(L"MMS_MENU_SEQUENCE_MODEL", modelPath, std::size(modelPath));
    if (!length) {
        std::puts("SKIP image render state checks: set MMS_MENU_SEQUENCE_MODEL to a real PMX/PMD fixture");
        return;
    }
    Check(length < std::size(modelPath) && fs::is_regular_file(modelPath), "image render-state model fixture exists");
    ResetAppState(app); LoadModelFile(app, modelPath, false);
    Check(app->ModelSlot(0) && mdl::Mdl(app->ModelSlot(0))->boneCount,
        "load a real cold image model without driving an editing frame");
    const auto savedSettings = app->antialiasing;
    std::wstring error;
    Check(antialias::Select(app, antialias::Algorithm::MmdOriginal, error), "select native AA for exact image target probes");
    app->GroundGridEnabled() = false; app->GroundShadowEnabled() = false;
    app->ShadowFilterEnhanced() = false; app->AdaptiveShadowResolution() = false;
    app->state.selfShadowEnabled = 1; app->SelfShadowMode() = 1;
    app->ShadowKeys()[0].mode = 1;
    app->CameraMode() = 1;
    auto& camera = app->CameraKeys()[0];
    camera.eye[0] = camera.eye[2] = 0; camera.eye[1] = 10;
    camera.distance = -45; camera.fov = 30; camera.perspective = 0; camera.parentModel = -1;
    std::memset(camera.target, 0, sizeof camera.target);
    Check(MH_CreateHook(reinterpret_cast<void*>(&RenderFrameScene), &ObserveScene,
        reinterpret_cast<void**>(&originalScene)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(&RenderFrameScene)) == MH_OK &&
        MH_CreateHook(reinterpret_cast<void*>(&RenderShadowMap), &ObserveShadow,
        reinterpret_cast<void**>(&originalShadow)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(&RenderShadowMap)) == MH_OK,
        "observe actual first image GPU and shadow submission");
    auto** deviceTable = *reinterpret_cast<void***>(app->Renderer()->device);
    void* captureCreateEntry = deviceTable[28];
    void* captureBindEntry = deviceTable[37];
    Check(MH_CreateHook(captureCreateEntry, &CreateTargetProbe,
        reinterpret_cast<void**>(&originalCreateTarget)) == MH_OK &&
        MH_EnableHook(captureCreateEntry) == MH_OK &&
        MH_CreateHook(captureBindEntry, &SetTargetProbe,
        reinterpret_cast<void**>(&originalTarget)) == MH_OK &&
        MH_EnableHook(captureBindEntry) == MH_OK,
        "observe actual capture creation and binding HRESULTs");
    for (bool reset : {false, true}) {
        image_export_render_state_checks::Probe current;
        current.cold = true; current.recreated = reset; probe = &current;
        seq::Options options;
        options.directory = (output / (reset ? L"render-state-reset" : L"render-state-cold")).wstring();
        fs::create_directories(options.directory);
        options.width = 64; options.height = 48; options.start = options.end = 0;
        options.fps = 30; options.format = reset ? 0 : 2;
        app->CurrentFrame() = 0; app->PlaybackCursorSeconds() = 0;
        auto* renderer = app->Renderer();
        const auto savedSamples = renderer->presentParameters.MultiSampleType;
        const auto savedQuality = renderer->presentParameters.MultiSampleQuality;
        const auto savedAvailability = renderer->multisampleAvailable;
        const auto savedOriginalSamples = app->antialiasing.originalSamples;
        const auto savedOriginalQuality = app->antialiasing.originalQuality;
        Start(app, options);
        if (reset) {
            // Reset the real swap chain and its automatic depth surface too.
            // Merely clearing multisampleAvailable would leave an illegal
            // non-MSAA capture target paired with an MSAA depth surface.
            renderer->presentParameters.MultiSampleType = D3DMULTISAMPLE_NONE;
            renderer->presentParameters.MultiSampleQuality = 0;
            renderer->multisampleAvailable = 0;
            // Native-AA preferences describe the device's native mode. Keep
            // them consistent with this deliberately simulated no-MSAA
            // device, so PrepareFrame cannot restore 8x on the first image.
            app->antialiasing.originalSamples = unsigned(D3DMULTISAMPLE_NONE);
            app->antialiasing.originalQuality = 0;
            Check(PostDeviceReset(app), "reset the actual device during opaque image recording");
            Check(!app->Renderer()->captureSurface, "actual reset released the capture target");
            {
                ComPtr<IDirect3DSurface9> back, depth;
                D3DSURFACE_DESC colourDesc{}, depthDesc{};
                auto* device = renderer->device;
                Check(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) &&
                    SUCCEEDED(device->GetDepthStencilSurface(&depth)) &&
                    SUCCEEDED(back->GetDesc(&colourDesc)) && SUCCEEDED(depth->GetDesc(&depthDesc)) &&
                    colourDesc.MultiSampleType == D3DMULTISAMPLE_NONE && colourDesc.MultiSampleQuality == 0 &&
                    depthDesc.MultiSampleType == D3DMULTISAMPLE_NONE && depthDesc.MultiSampleQuality == 0,
                    "real reset establishes a non-MSAA swap chain and automatic depth surface");
            }
            current.captureCreated = current.captureBound = 0;
        }
        auto* model = app->ModelSlot(0);
        for (unsigned i = 0; i < mdl::Mdl(model)->boneCount; ++i)
            std::memset(mdl::Bones(model)[i].matInit, 0, sizeof mdl::Bones(model)[i].matInit);
        for (unsigned step = 0; seq::Active(app) && step < 20; ++step) {
            app->DeltaTime() = 1.f / 30; app->state.messageSeen = 1; coordinator_fixture::Drive(app);
        }
        Check(!seq::Active(app) && !app->RecordingWindow() && current.scenes && current.shadows,
            "cold or reset image writes through the real scene and native shadow pipeline");
        const auto pixels = ImagePixels(seq::FramePath(options, 1));
        unsigned clear = 0, visible = 0, coloured = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            clear += pixels[i + 3] == 0;
            visible += pixels[i + 3] != 0;
            coloured += pixels[i + 3] != 0 && (pixels[i] != pixels[i + 1] || pixels[i + 1] != pixels[i + 2]);
        }
        Check(visible > 4 && coloured > 4, "first encoded image contains coloured fixture geometry");
        Check(reset ? clear == 0 : clear > 4, "actual encoded image has the requested opaque or transparent background");
        probe = nullptr;
        if (reset) {
            renderer->presentParameters.MultiSampleType = savedSamples;
            renderer->presentParameters.MultiSampleQuality = savedQuality;
            renderer->multisampleAvailable = savedAvailability;
            app->antialiasing.originalSamples = savedOriginalSamples;
            app->antialiasing.originalQuality = savedOriginalQuality;
            Check(PostDeviceReset(app), "restore the actual original swap-chain multisampling mode");
            ComPtr<IDirect3DSurface9> back, depth;
            D3DSURFACE_DESC colourDesc{}, depthDesc{};
            Check(SUCCEEDED(renderer->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) &&
                SUCCEEDED(renderer->device->GetDepthStencilSurface(&depth)) &&
                SUCCEEDED(back->GetDesc(&colourDesc)) && SUCCEEDED(depth->GetDesc(&depthDesc)) &&
                colourDesc.MultiSampleType == savedSamples && colourDesc.MultiSampleQuality == savedQuality &&
                depthDesc.MultiSampleType == savedSamples && depthDesc.MultiSampleQuality == savedQuality,
                "restored colour and depth surfaces match the original real MSAA mode");
        }
    }
    for (auto* entry : {captureCreateEntry, captureBindEntry}) { MH_DisableHook(entry); MH_RemoveHook(entry); }
    for (auto* target : {reinterpret_cast<void*>(&RenderFrameScene), reinterpret_cast<void*>(&RenderShadowMap)}) {
        MH_DisableHook(target); MH_RemoveHook(target);
    }
    // A normal recording completion can release its shadow map during reset;
    // the stopped bone cursor legitimately uses the fixed renderer. Warm a
    // real camera-tool preview so the shadow failure cases own live resources.
    const auto warmupMode = app->EditMode();
    app->EditMode() = ViewportEditMode::Camera;
    std::printf("TRACE shadow-warmup edit_mode=%d previous_mode=%d playback=%u shadow_mode=%d enabled=%u\n",
        int(app->EditMode()), int(warmupMode), unsigned(app->PlaybackActive()),
        app->SelfShadowMode(), unsigned(app->state.selfShadowEnabled));
    app->state.messageSeen = 1; RenderFrameScene(app);
    app->EditMode() = warmupMode;
    ShadowTargets(app);
    ResetAppState(app);
    Check(antialias::Select(app, savedSettings.algorithm, error), "restore prior AA after image render-state probes");
}
