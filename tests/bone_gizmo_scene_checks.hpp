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
struct BoneGizmoObservation {
    bool enabled = false;
    unsigned triangles = 0;
    BoneGizmoScreenVertex last[2][3]{};
    RECT viewport{};
    float pixelScale = 1.0f;
} boneGizmoObservation;

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

void CheckBoneGizmoScene(MMDApp* app) {
#if defined(REQUIRE_MME)
    Check(GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll"),
        "gizmo MME regression requires the actual effect plugins");
    std::puts("MME loaded for bone gizmo regression");
#endif
    int slot = app->SelectedModelSlot();
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

    auto* device = app->Renderer()->device;
    const RECT originalRect = app->ViewportRect();
    D3DVIEWPORT9 originalViewport{};
    Check(SUCCEEDED(device->GetViewport(&originalViewport)), "read original gizmo viewport");
    Check(originalRect.right - originalRect.left > 160 &&
        originalRect.bottom - originalRect.top > 160, "gizmo fixture viewport has room for inset");
    const auto initialized = MH_Initialize();
    Check(initialized == MH_OK || initialized == MH_ERROR_ALREADY_INITIALIZED,
        "initialize gizmo draw observer");
    void* draw = (*reinterpret_cast<void***>(device))[83];
    Check(MH_CreateHook(draw, &ObserveBoneGizmoDraw,
        reinterpret_cast<void**>(&originalBoneGizmoDraw)) == MH_OK &&
        MH_EnableHook(draw) == MH_OK, "observe production gizmo triangles");

    unsigned cases = 0;
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
                        app->ViewportToolOperation() = static_cast<ViewportToolAction>(actions[kind]);
                        app->InteractionDragMode() = ViewportDragMode::None;
                        const D3DVIEWPORT9 viewport{DWORD(display.left), DWORD(display.top),
                            DWORD(display.right - display.left), DWORD(display.bottom - display.top), 0, 1};
                        Check(SUCCEEDED(device->SetViewport(&viewport)), "set inset gizmo viewport");
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
}
