// MAINTENANCE: regression-keep; issue #37, production scene gizmo anchoring.
// Included inside the hidden scene lifecycle harness's anonymous namespace.
struct BoneGizmoScreenVertex {
    float x, y, z, rhw;
    D3DCOLOR color;
};
static_assert(sizeof(BoneGizmoScreenVertex) == 20);

using BoneGizmoDraw = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE,
    UINT, const void*, UINT);
BoneGizmoDraw originalBoneGizmoDraw = nullptr;
using RayEffectCreate = HRESULT(WINAPI*)(IDirect3DDevice9*, LPCWSTR, const void*,
    const void*, DWORD, void*, void**, void**);
RayEffectCreate originalRayEffectCreate = nullptr;
bool rayEffectCreated = false;

HRESULT WINAPI ObserveRayEffectCreate(IDirect3DDevice9* device, LPCWSTR filename,
    const void* macros, const void* include, DWORD flags, void* pool,
    void** effect, void** errors) {
    const HRESULT result = originalRayEffectCreate(device, filename, macros, include,
        flags, pool, effect, errors);
    if (SUCCEEDED(result) && filename && effect && *effect) {
        const wchar_t* base = std::wcsrchr(filename, L'\\');
        if (_wcsicmp(base ? base + 1 : filename, L"ray.fx") == 0)
            rayEffectCreated = true;
    }
    return result;
}

void* ObserveRayEffectCompilation() {
    Check(GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll"),
        "Ray gizmo regression requires the real MME plugins");
    Check(SetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", L"1") != FALSE,
        "route Ray effect creation through the observed compiler");
    const auto initialized = MH_Initialize();
    Check(initialized == MH_OK || initialized == MH_ERROR_ALREADY_INITIALIZED,
        "initialize Ray effect observer");
    auto* compile = reinterpret_cast<void*>(GetProcAddress(d3dx::Get().module,
        "D3DXCreateEffectFromFileW"));
    Check(compile && MH_CreateHook(compile, &ObserveRayEffectCreate,
        reinterpret_cast<void**>(&originalRayEffectCreate)) == MH_OK &&
        MH_EnableHook(compile) == MH_OK, "observe real Ray effect creation");
    return compile;
}

struct BoneGizmoObservation {
    bool enabled = false;
    unsigned triangles = 0;
    BoneGizmoScreenVertex last[2][3]{};
    RECT viewport{};
    float pixelScale = 1.0f;
} boneGizmoObservation;

std::vector<DWORD> ReadGizmoPixels(IDirect3DDevice9* device, const RECT& area) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDirect3DSurface9> back, resolved, cpu;
    Check(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)),
        "get completed Ray preview backbuffer");
    D3DSURFACE_DESC desc{};
    Check(SUCCEEDED(back->GetDesc(&desc)) &&
        (desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8) &&
        area.left >= 0 && area.top >= 0 && area.right <= LONG(desc.Width) &&
        area.bottom <= LONG(desc.Height) && area.left < area.right && area.top < area.bottom,
        "Ray gizmo sample lies within a BGRA backbuffer");
    auto* source = back.Get();
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        Check(SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
            D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) &&
            SUCCEEDED(device->StretchRect(source, nullptr, resolved.Get(), nullptr, D3DTEXF_NONE)),
            "resolve completed Ray preview");
        source = resolved.Get();
    }
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
        D3DPOOL_SYSTEMMEM, &cpu, nullptr)) && SUCCEEDED(device->GetRenderTargetData(source, cpu.Get())),
        "read completed Ray preview pixels");
    D3DLOCKED_RECT lock{};
    Check(SUCCEEDED(cpu->LockRect(&lock, &area, D3DLOCK_READONLY)), "lock local gizmo pixels");
    const auto width = area.right - area.left;
    const auto height = area.bottom - area.top;
    std::vector<DWORD> pixels(std::size_t(width) * height);
    for (LONG y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const DWORD*>(
            static_cast<const char*>(lock.pBits) + y * lock.Pitch);
        std::copy_n(row, width, pixels.data() + std::size_t(y) * width);
    }
    cpu->UnlockRect();
    return pixels;
}

HRESULT WINAPI ObserveBoneGizmoDraw(IDirect3DDevice9* device,
    D3DPRIMITIVETYPE type, UINT primitives, const void* vertices, UINT stride) {
    const HRESULT result = originalBoneGizmoDraw(device, type, primitives, vertices, stride);
    if (!boneGizmoObservation.enabled || FAILED(result) ||
        type != D3DPT_TRIANGLELIST || primitives != 1 ||
        stride != sizeof(BoneGizmoScreenVertex) || !vertices)
        return result;
    DWORD fvf = 0;
    device->GetFVF(&fvf);
    if (fvf != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)) return result;
    const auto* triangle = static_cast<const BoneGizmoScreenVertex*>(vertices);
    for (int i = 0; i < 3; ++i)
        if (triangle[i].color != 0xFFFFE000u) return result;
    // The preview ends with its yellow origin square, two triangles. Other
    // yellow preview strokes precede it; normal sprite/text batches use VB draws.
    std::memcpy(boneGizmoObservation.last[0], boneGizmoObservation.last[1],
        sizeof(boneGizmoObservation.last[0]));
    std::memcpy(boneGizmoObservation.last[1], triangle,
        sizeof(boneGizmoObservation.last[1]));
    ++boneGizmoObservation.triangles;
    boneGizmoObservation.viewport = g_Block->ViewportRect();
    boneGizmoObservation.pixelScale = g_Block->Renderer()->viewScale;
    return result;
}

void CheckBoneGizmoScene(MMDApp* app, void* rayCompiler) {
#if defined(REQUIRE_MME)
    Check(GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll"),
        "gizmo MME regression requires the actual effect plugins");
    std::puts("MME loaded for bone gizmo regression");
#endif
    int slot = app->SelectedModelSlot();
    if (rayCompiler) {
        // The stage also contains helpers, lights, and a skybox. Use the
        // character with the most bones to exercise the model editing HUD.
        unsigned mostBones = 0;
        for (int candidate = 0; candidate < kModelSlotCount; ++candidate) {
            const auto* model = mdl::Mdl(app->ModelSlot(candidate));
            if (!model || model->boneCount <= mostBones) continue;
            mostBones = model->boneCount;
            slot = candidate;
        }
        app->SetSelectedModelSlot(slot);
    }
    if (!app->SelectedModel()) {
        slot = 0;
        while (slot < kModelSlotCount && !app->ModelSlot(slot)) ++slot;
        Check(slot < kModelSlotCount, "gizmo fixture contains a model");
        app->SetSelectedModelSlot(slot);
    }
    auto* bytes = app->SelectedModel();
    auto* model = mdl::Mdl(bytes);
    auto* bones = mdl::Bones(bytes);
    Check(model && bones && model->boneCount && model->boneSelection,
        "gizmo fixture has selectable bones");
    int selected = -1;
    float closest = 1.0e30f;
    for (unsigned i = 0; i < model->boneCount; ++i) {
        const auto& bone = bones[i];
        if (bone.type == mdl::BoneType::FixedAxis ||
            (bone.type == mdl::BoneType::UnderIk &&
             (bone.flags & mdl::kBoneFlagFixedAxis) == mdl::kBoneFlagFixedAxis))
            continue;
        const float distance = std::fabs(bone.position[0]) +
            std::fabs(bone.position[1] - 10.0f) + std::fabs(bone.position[2]);
        if (distance < closest) { closest = distance; selected = int(i); }
    }
    Check(selected >= 0, "gizmo fixture has an ordinary bone");
    model->selectedBone = selected;
    std::fill(model->boneSelection, model->boneSelection + model->boneCount, 0);
    model->boneSelection[selected] = 1;
    app->CameraMode() = 0;
    app->state.optflag[0] = 0;
    app->PlaybackActive() = 0;
    app->PlaybackPhysicsMode() = 0;
    app->state.followCameraEnabled = 0;
    app->CameraParentModel() = -1;
    app->CameraReferenceMode() = CameraAttachmentReference::None;
    app->ViewOffsetX() = 0;
    app->ViewOffsetY() = 10;
    app->CameraDistance() = -60;
    app->CameraFov() = 45;
    app->CameraRoll() = 0;
    app->EditMode() = ViewportEditMode::Bone;
    app->InteractionDragMode() = ViewportDragMode::None;
    app->antialiasing.enabled = false;
    EnableD3D12Native(false);
    ReloadModels(app);
    // Loading a model does not evaluate the first complete bone pose. In
    // particular, matInit can still be all zero, including its homogeneous
    // component, until FrameDriver runs the normal model/physics update.
    app->DeltaTime() = 1.0f / 30.0f;
    app->state.messageSeen = 10;
    FrameDriver(app);
    Check(std::fabs(bones[selected].matInit[15] - 1.0f) < .001f,
        "production warmup evaluates a valid bone pose");
    if (rayCompiler) {
        for (int i = 0; i < 5; ++i) {
            app->state.messageSeen = 10;
            FrameDriver(app);
        }
        Check(rayEffectCreated, "real MME successfully creates ray.fx");
        MH_DisableHook(rayCompiler);
        MH_RemoveHook(rayCompiler);
    }

    auto* device = app->Renderer()->device;
    const RECT originalRect = app->ViewportRect();
    D3DVIEWPORT9 originalViewport{};
    Check(SUCCEEDED(device->GetViewport(&originalViewport)), "read original gizmo viewport");
    Check(originalRect.right - originalRect.left > 160 &&
        originalRect.bottom - originalRect.top > 160, "gizmo fixture viewport has room for inset");
    const auto initialized = MH_Initialize();
    Check(initialized == MH_OK || initialized == MH_ERROR_ALREADY_INITIALIZED,
        "initialize gizmo draw observer");
    Microsoft::WRL::ComPtr<IDirect3DDevice9> presentationDevice;
    if (MMD_ORIGINAL_PROFILE && !GetModuleHandleW(L"MMEffect.dll")) {
        presentationDevice = device;
    } else {
        Microsoft::WRL::ComPtr<IDirect3DSurface9> back;
        Check(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) &&
            SUCCEEDED(back->GetDevice(&presentationDevice)), "get native presentation device");
    }
    void* draw = (*reinterpret_cast<void***>(presentationDevice.Get()))[83];
    Check(MH_CreateHook(draw, &ObserveBoneGizmoDraw,
        reinterpret_cast<void**>(&originalBoneGizmoDraw)) == MH_OK &&
        MH_EnableHook(draw) == MH_OK, "observe production gizmo triangles");

    unsigned cases = 0;
    unsigned pixelCases = 0;
    for (int inset : {0, 1}) {
        const RECT display{originalRect.left + inset * 37,
            originalRect.top + inset * 23, originalRect.right - inset * 19,
            originalRect.bottom - inset * 17};
        for (double percent : {100., 50., 150.}) {
            if (MMD_ORIGINAL_PROFILE && percent != 100.) continue;
            app->ViewportRect() = display;
            std::wstring error;
            Check(viewport_render_scale::Configure(app, percent == 100., percent, error),
                "configure gizmo preview scale");
            for (int angle : {0, 1}) {
                app->CameraYaw() = angle ? .55f : 0.0f;
                app->CameraPitch() = angle ? .3f : 0.0f;
                for (int coordinate = 0; coordinate < 3; ++coordinate) {
                    app->state.coordinateSystem = coordinate;
                    const int actions[] = {9 + coordinate, 12 + coordinate, 23 + coordinate};
                    for (int kind = 0; kind < (MMD_ORIGINAL_PROFILE ? 2 : 3); ++kind) {
                        app->InteractionDragMode() = ViewportDragMode::None;
                        const D3DVIEWPORT9 viewport{DWORD(display.left), DWORD(display.top),
                            DWORD(display.right - display.left), DWORD(display.bottom - display.top), 0, 1};
                        Check(SUCCEEDED(device->SetViewport(&viewport)), "set inset gizmo viewport");
                        // Global Y is side-on in this view and its yellow
                        // stroke extends beyond the selected bone sprite.
                        const bool checkPixels = rayCompiler && inset == 0 && angle == 0 &&
                            coordinate == 1 && kind == 1;
                        RECT sample{};
                        std::vector<DWORD> before;
                        if (checkPixels) {
                            app->ViewportToolOperation() = ViewportToolAction::None;
                            app->state.messageSeen = 10;
                            RenderFrameScene(app);
                            const auto& bone = bones[selected];
                            const LONG radius = LONG(std::ceil(72 * app->Renderer()->viewScale));
                            sample = {(std::max)(display.left, LONG(bone.selState) - radius),
                                (std::max)(display.top, LONG(bone.selState2) - radius),
                                (std::min)(display.right, LONG(bone.selState) + radius),
                                (std::min)(display.bottom, LONG(bone.selState2) + radius)};
                            before = ReadGizmoPixels(presentationDevice.Get(), sample);
                        }
                        app->ViewportToolOperation() = static_cast<ViewportToolAction>(actions[kind]);
                        boneGizmoObservation = {};
                        boneGizmoObservation.enabled = true;
                        app->state.messageSeen = 10;
                        // FrameDriver would replace the deliberately selected HUD hover.
                        RenderFrameScene(app);
                        boneGizmoObservation.enabled = false;
                        Check(boneGizmoObservation.triangles >= 2, "production HUD preview submits an origin square");
                        float left = boneGizmoObservation.last[0][0].x, right = left;
                        float top = boneGizmoObservation.last[0][0].y, bottom = top;
                        for (const auto& triangle : boneGizmoObservation.last)
                            for (const auto& vertex : triangle) {
                                left = (std::min)(left, vertex.x); right = (std::max)(right, vertex.x);
                                top = (std::min)(top, vertex.y); bottom = (std::max)(bottom, vertex.y);
                            }
                        const float expectedSide = 4.0f * boneGizmoObservation.pixelScale;
                        Check(std::fabs(right - left - expectedSide) < .01f &&
                            std::fabs(bottom - top - expectedSide) < .01f,
                            "last yellow triangles are the bone-origin square");
                        const auto& render = boneGizmoObservation.viewport;
                        const float scaleX = float(render.right - render.left) / (display.right - display.left);
                        const float scaleY = float(render.bottom - render.top) / (display.bottom - display.top);
                        const float x = ((left + right) * .5f - render.left + .5f) / scaleX - .5f + display.left;
                        const float y = ((top + bottom) * .5f - render.top + .5f) / scaleY - .5f + display.top;
                        const auto& bone = bones[selected];
                        const float tolerance = percent == 100. ? .01f : 2.1f;
                        const bool anchored = std::fabs(x - bone.selState) <= tolerance &&
                            std::fabs(y - bone.selState2) <= tolerance;
                        if (!anchored) {
                            std::fprintf(stderr,
                                "GIZMO inset=%d scale=%g angle=%d coordinate=%d action=%d origin=(%.3f,%.3f) bone=(%d,%d)\n",
                                inset, percent, angle, coordinate, actions[kind], x, y, bone.selState, bone.selState2);
                        }
                        Check(anchored, "production HUD preview remains centered on the selected bone");
                        if (checkPixels) {
                            const auto after = ReadGizmoPixels(presentationDevice.Get(), sample);
                            std::size_t revealed = 0, changed = 0;
                            DWORD nearestYellow = 0;
                            int nearestDistance = 1000;
                            for (std::size_t i = 0; i < after.size(); ++i) {
                                const DWORD rgb = after[i] & 0xFFFFFF;
                                if (rgb == (before[i] & 0xFFFFFF)) continue;
                                ++changed;
                                if (rgb == 0xFFE000) ++revealed;
                                const int distance = 255 - int(rgb >> 16) +
                                    std::abs(int((rgb >> 8) & 255) - 224) + int(rgb & 255);
                                if (distance < nearestDistance) {
                                    nearestDistance = distance;
                                    nearestYellow = rgb;
                                }
                            }
                            std::printf("RAY GIZMO scale=%g newly yellow=%zu changed=%zu nearest=%06lX\n",
                                percent, revealed, changed, static_cast<unsigned long>(nearestYellow));
                            Check(revealed > 0, "yellow axis reaches the completed Ray preview");
                            ++pixelCases;
                        }
                        ++cases;
                    }
                }
            }
        }
    }
    MH_DisableHook(draw);
    MH_RemoveHook(draw);
    std::wstring error;
    Check(viewport_render_scale::Configure(app, true, 100., error), "restore preview resolution");
    app->ViewportRect() = originalRect;
    device->SetViewport(&originalViewport);
    app->ViewportToolOperation() = ViewportToolAction::None;
    Check(!IsWindowVisible(app->MainWindow()), "gizmo scene test stays hidden");
    std::printf("PASS production bone gizmo origin: %u cases across HUD rows, coordinate modes, camera angles, inset viewports and render scales\n", cases);
    if (rayCompiler)
        std::printf("PASS production Ray gizmo pixels: %u render scales\n", pixelCases);
}
