// MAINTENANCE: regression-keep; multi-joint rotation HUD and per-joint sectors.
// A hidden real D3D9 device renders production DrawBoneOperationAxis. The draw
// observer copies submitted geometry after the driver accepts each call.
#pragma once
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <string>

namespace bone_multiselect_hud_tests {
using namespace mikumikustudio;
struct ScreenVertex { float x, y, z, rhw; DWORD color; };
struct Triangle { ScreenVertex v[3]; };
static_assert(sizeof(ScreenVertex) == 20 && sizeof(Triangle) == 60);
struct Observation {
    unsigned calls = 0;
    std::vector<Triangle> triangles;
    std::vector<DWORD> pixels;
};
using DrawFunction = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
DrawFunction originalDraw = nullptr;
Observation* observing = nullptr;

void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL multi-bone HUD: %s\n", message); std::exit(1); }
}
HRESULT WINAPI ObserveDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                           UINT primitives, const void* vertices, UINT stride) {
    const HRESULT result = originalDraw(device, type, primitives, vertices, stride);
    if (!observing || FAILED(result) || type != D3DPT_TRIANGLELIST ||
        stride != sizeof(ScreenVertex) || !vertices) return result;
    DWORD fvf = 0;
    device->GetFVF(&fvf);
    if (fvf != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)) return result;
    ++observing->calls;
    const auto* triangles = static_cast<const Triangle*>(vertices);
    observing->triangles.insert(observing->triangles.end(), triangles, triangles + primitives);
    return result;
}
D3DMATRIX Identity() {
    D3DMATRIX result{}; result._11 = result._22 = result._33 = result._44 = 1;
    return result;
}
struct Fixture {
    static constexpr int width = 960, height = 600, count = 5;
    HWND window = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    IDirect3DSurface9* read = nullptr;
    void* drawAddress = nullptr;
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<D3DRenderer> renderer = std::make_unique<D3DRenderer>();
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    mdl::BoneRecord bones[count]{};
    unsigned char selected[count] = {1, 1, 1, 0, 1};
    D3DMATRIX frame = Identity(), view = Identity(), projection = Identity();
    std::array<std::array<float, 2>, count> origins{};
    bool compact = false;
    unsigned char* Raw() { return reinterpret_cast<unsigned char*>(model.get()); }
    Fixture() {
        Require(d3dx::Get().Load(), "load transform and image dependencies");
        window = CreateWindowW(L"STATIC", L"Multi-bone HUD regression", WS_POPUP,
            0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(window != nullptr, "create hidden window");
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
        Require(d3d != nullptr, "create D3D9");
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferWidth = width; pp.BackBufferHeight = height; pp.BackBufferFormat = D3DFMT_A8R8G8B8;
        Require(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device)), "create actual GPU device");
        Require(SUCCEEDED(device->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &read, nullptr)), "allocate GPU readback");
        const auto state = std::make_unique<MMDAppState>(); app->state = *state;
        std::memset(renderer.get(), 0, sizeof(*renderer));
        app->Renderer() = renderer.get(); renderer->device = device; renderer->viewScale = 1;
        app->ViewportRect() = {0, 0, width, height};
        app->ModelSlot(0) = Raw(); app->SetSelectedModelSlot(0);
        app->CameraFov() = 30; app->CameraDistance() = -10;
        app->CameraPitch() = .45f; app->CameraYaw() = -.35f; app->CameraRoll() = .2f;
        model->boneTable = bones; model->boneCount = count; model->selectedBone = 2;
        model->boneSelection = selected; model->physicsMode = 2;
        d3dx::D3DXMATRIXF rx{}, ry{}, rz{}, xy{}, v{};
        d3dx::Get().rotX(&rx, .45f); d3dx::Get().rotY(&ry, -.35f); d3dx::Get().rotZ(&rz, .2f);
        d3dx::Get().multiply(&xy, &rx, &ry); d3dx::Get().multiply(&v, &xy, &rz);
        std::memcpy(&view, &v, sizeof(view));
        projection._11 = .18f; projection._22 = .28f; projection._33 = .05f;
        projection._43 = .5f;
        device->SetTransform(D3DTS_WORLD, &frame); device->SetTransform(D3DTS_VIEW, &view);
        device->SetTransform(D3DTS_PROJECTION, &projection);
        device->SetVertexShader(nullptr); device->SetPixelShader(nullptr);
        device->SetRenderState(D3DRS_LIGHTING, FALSE); device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE); device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        const auto initialized = MH_Initialize();
        Require(initialized == MH_OK || initialized == MH_ERROR_ALREADY_INITIALIZED, "initialize draw observer");
        drawAddress = (*reinterpret_cast<void***>(device))[83];
        Require(MH_CreateHook(drawAddress, &ObserveDraw, reinterpret_cast<void**>(&originalDraw)) == MH_OK &&
                MH_EnableHook(drawAddress) == MH_OK, "observe actual HUD geometry submissions");
        ResetPose(); Hover(0, 0);
    }
    ~Fixture() {
        app->ViewportToolOperation() = ViewportToolAction::None;
        app->InteractionDragMode() = ViewportDragMode::None;
        DrawBoneOperationAxis(app.get(), reinterpret_cast<const float*>(&frame));
        bone_scale::gizmo::Publish(app.get(), {});
        observing = nullptr;
        MH_DisableHook(drawAddress); MH_RemoveHook(drawAddress);
        read->Release(); device->Release(); d3d->Release(); DestroyWindow(window);
        app->ModelSlot(0) = nullptr;
    }
    void Hover(int kind, int axis) {
        const int starts[] = {9, 12, 23};
        app->ViewportToolOperation() = static_cast<ViewportToolAction>(starts[kind] + axis);
        app->InteractionDragMode() = ViewportDragMode::None;
    }
    void ResetPose() {
        for (int i = 0; i < count; ++i) {
            bones[i] = {};
            bones[i].parent = i > 0 && i < 3 ? i - 1 : -1;
            bones[i].tailBone = (i + 1) % count;
            bones[i].type = i == 4 ? mdl::BoneType::Move : mdl::BoneType::RotateMove;
            bones[i].flags = mdl::kBoneFlagVisible;
            const float x = .25f + i * .35f;
            bones[i].localAxes[0] = std::cos(x); bones[i].localAxes[1] = std::sin(x);
            bones[i].localAxes[5] = 1;
            bones[i].axis[0] = .3f; bones[i].axis[1] = .7f; bones[i].axis[2] = .4f;
        }
        Pose(0, 0, 0);
    }
    void Pose(float a0, float a1, float a2) {
        const float angles[count] = {a0, a1, a2, 0, 0};
        const float x[count] = {-3.6f, -1.2f, 1.2f, 3.6f, 0};
        float inherited = 0;
        for (int i = 0; i < count; ++i) {
            auto& b = bones[i];
            b.rotQuat[0] = b.rotQuat[1] = 0;
            b.rotQuat[2] = std::sin(angles[i] / 2); b.rotQuat[3] = std::cos(angles[i] / 2);
            d3dx::D3DXMATRIXF parent{}, world{};
            d3dx::Get().rotZ(&parent, i < 3 ? inherited : 0);
            d3dx::Get().rotZ(&world, i < 3 ? inherited + angles[i] : 0);
            std::memcpy(b.matWorld, &parent, sizeof(parent));
            std::memcpy(b.matInit, &world, sizeof(world));
            b.matInit[12] = compact && i < 3 ? (i - 1) * .45f : x[i];
            b.matInit[13] = i == 4 ? -2.1f : compact && i < 3 ? (i - 1) * .55f : .4f;
            if (i < 3) inherited += angles[i];
        }
    }
    Observation Draw(bool skeleton = false) {
        for (int i = 0; i < count; ++i) {
            int x = 0, y = 0; float w = 0;
            Require(ProjectBonePoint(bones[i], BoneOverlayPoint::Origin, frame, view, projection,
                app->ViewportRect(), &x, &y, &w), "project fixture bone origin");
            origins[i] = {float(x), float(y)};
        }
        Require(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF303943u, 1, 0)), "clear HUD frame");
        Require(SUCCEEDED(device->BeginScene()), "begin HUD frame");
        if (skeleton) {
            ScreenVertex lines[4];
            for (int i = 0; i < 4; ++i) {
                const auto& p = origins[(i + 1) / 2];
                lines[i] = {p[0], p[1], 0, 1, 0xFF87939Fu};
            }
            device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            device->DrawPrimitiveUP(D3DPT_LINELIST, 2, lines, sizeof(ScreenVertex));
        }
        Observation result;
        observing = &result;
        DrawBoneOperationAxis(app.get(), reinterpret_cast<const float*>(&frame));
        observing = nullptr;
        Require(SUCCEEDED(device->EndScene()), "finish HUD frame");
        IDirect3DSurface9* back = nullptr;
        Require(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)), "get HUD render target");
        const HRESULT copied = device->GetRenderTargetData(back, read); back->Release();
        Require(SUCCEEDED(copied), "read actual HUD pixels");
        D3DLOCKED_RECT locked{};
        Require(SUCCEEDED(read->LockRect(&locked, nullptr, D3DLOCK_READONLY)), "lock HUD readback");
        result.pixels.resize(width * height);
        for (int y = 0; y < height; ++y)
            std::memcpy(result.pixels.data() + y * width, static_cast<const char*>(locked.pBits) + y * locked.Pitch, width * 4);
        read->UnlockRect();
        return result;
    }
};

bool Near(float a, float b) { return std::fabs(a - b) < .02f; }
bool SamePoint(const ScreenVertex& a, const ScreenVertex& b) { return Near(a.x, b.x) && Near(a.y, b.y); }
bool OriginTriangle(const Triangle& triangle, const std::array<float, 2>& p) {
    for (const auto& v : triangle.v)
        if (v.color != 0xFFFFE000u || !Near(std::fabs(v.x - p[0]), 2) || !Near(std::fabs(v.y - p[1]), 2)) return false;
    return true;
}
unsigned Origins(const Observation& o, const std::array<float, 2>& p) {
    return static_cast<unsigned>(std::count_if(o.triangles.begin(), o.triangles.end(),
        [&](const Triangle& t) { return OriginTriangle(t, p); })) / 2;
}
void CheckOrigins(Fixture& f, const Observation& o, const std::array<bool, Fixture::count>& expected) {
    unsigned n = 0;
    for (int i = 0; i < Fixture::count; ++i) {
        Require(Origins(o, f.origins[i]) == (expected[i] ? 1u : 0u), "only intended bone pivots submit yellow origin squares");
        if (expected[i]) {
            ++n;
            const auto& p = f.origins[i];
            Require((o.pixels[int(p[1]) * Fixture::width + int(p[0])] & 0xFFFFFFu) == 0xFFE000u,
                "every displayed pivot has its yellow origin in actual GPU pixels");
        }
    }
    Require(o.calls >= n * 3 && o.calls <= n * 4 + 1, "rotation previews batch geometry in about three submissions per joint");
    if (n) Require(o.triangles.size() >= 2 && OriginTriangle(o.triangles.back(), f.origins[f.model->selectedBone]),
        "active bone origin is drawn last");
}
std::array<double, 3> RingShape(const Observation& o, const std::array<float, 2>& origin) {
    // Each ring stroke is a pair of black triangles forming a rectangle.
    // Their diagonal midpoint is the actual ring centreline, unaffected by
    // different outline widths/radii on active and secondary previews.
    double xx = 0, xy = 0, yy = 0;
    unsigned n = 0;
    for (std::size_t i = 0; i + 1 < o.triangles.size(); ++i) {
        const auto& a = o.triangles[i]; const auto& b = o.triangles[i + 1];
        if ((a.v[0].color & 0xFFFFFFu) || a.v[0].color != b.v[0].color ||
            !SamePoint(a.v[0], b.v[0]) || !SamePoint(a.v[2], b.v[1])) continue;
        const double x = (a.v[0].x + a.v[2].x) * .5 - origin[0];
        const double y = (a.v[0].y + a.v[2].y) * .5 - origin[1];
        const double distance = std::hypot(x, y);
        const double length = std::hypot(a.v[1].x - a.v[0].x, a.v[1].y - a.v[0].y);
        const double width = std::hypot(a.v[2].x - a.v[1].x, a.v[2].y - a.v[1].y);
        if (distance < 5 || distance > 70 || length > 20 || width > 10) continue;
        xx += x * x; xy += x * y; yy += y * y; ++n; ++i;
    }
    Require(n >= 48 && xx + yy > 1, "observe a complete projected rotation ring");
    return {xx / (xx + yy), xy / (xx + yy), yy / (xx + yy)};
}
unsigned SectorTriangles(const Observation& o, const std::array<float, 2>& origin) {
    unsigned count = 0;
    for (const auto& t : o.triangles) {
        if ((t.v[0].color & 0xFFFFFFu) != 0xFFE000u || (t.v[0].color >> 24) >= 255 ||
            !Near(t.v[0].x, origin[0]) || !Near(t.v[0].y, origin[1])) continue;
        if (std::hypot(t.v[1].x - origin[0], t.v[1].y - origin[1]) > 5 &&
            std::hypot(t.v[2].x - origin[0], t.v[2].y - origin[1]) > 5) ++count;
    }
    return count;
}
double SectorArea(const Observation& o, const std::array<float, 2>& origin) {
    double area = 0;
    for (const auto& t : o.triangles) {
        if ((t.v[0].color & 0xFFFFFFu) != 0xFFE000u || (t.v[0].color >> 24) >= 255 ||
            !Near(t.v[0].x, origin[0]) || !Near(t.v[0].y, origin[1])) continue;
        area += (t.v[1].x - origin[0]) * (t.v[2].y - origin[1]) -
                (t.v[1].y - origin[1]) * (t.v[2].x - origin[0]);
    }
    return area;
}
void NoSectors(const Fixture& f, const Observation& o, const char* message) {
    for (const auto& p : f.origins) Require(SectorTriangles(o, p) == 0, message);
}
void Capture(Fixture& f) {
    wchar_t directory[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"MMS_MULTIBONE_HUD_CAPTURE_DIR", directory, 32768);
    if (!length || length >= 32768) return;
    std::filesystem::create_directories(directory);
    for (bool compact : {false, true}) {
        f.compact = compact; f.ResetPose(); f.Pose(.12f, .19f, -.08f); f.Hover(0, 0);
        f.Draw(true);
        const auto path = std::filesystem::path(directory) / (compact ? L"multibone-compact.png" : L"multibone-spaced.png");
        Require(d3dx::Get().saveSurfaceToFileW && SUCCEEDED(d3dx::Get().saveSurfaceToFileW(path.c_str(), 3, f.read, nullptr, nullptr)),
                "save optional HUD fixture PNG");
        std::wprintf(L"HUD_CAPTURE %ls\n", path.c_str());
    }
    f.compact = false; f.ResetPose();
}
void Run() {
    Fixture f;
    Capture(f);
    wchar_t only[2]{};
    if (GetEnvironmentVariableW(L"MMS_MULTIBONE_HUD_CAPTURE_ONLY", only, 2) && only[0] == L'1') return;
    unsigned cases = 0;
    for (int coordinate = 0; coordinate < 3; ++coordinate)
    for (int axis = 0; axis < 3; ++axis)
    for (bool fixed : {false, true}) {
        f.ResetPose(); f.Pose(.12f, .19f, -.08f);
        f.bones[1].type = fixed ? mdl::BoneType::FixedAxis : mdl::BoneType::RotateMove;
        if (fixed) f.bones[1].flags |= mdl::kBoneFlagFixedAxis;
        f.app->state.coordinateSystem = coordinate; f.Hover(0, axis);
        f.model->selectedBone = 2;
        const unsigned char selection[] = {1, 1, 1, 0, 1};
        std::memcpy(f.selected, selection, sizeof(selection));
        const auto group = f.Draw();
        CheckOrigins(f, group, {true, true, true, false, false});
        for (int bone = 0; bone < 3; ++bone) {
            const auto shape = RingShape(group, f.origins[bone]);
            std::fill_n(f.selected, Fixture::count, 0); f.selected[bone] = 1; f.model->selectedBone = bone;
            const auto single = f.Draw();
            const auto reference = RingShape(single, f.origins[bone]);
            for (int i = 0; i < 3; ++i) Require(std::fabs(shape[i] - reference[i]) < .025,
                "each secondary ring has the same projected axis plane as that joint selected alone");
            std::array<bool, Fixture::count> expected{}; expected[bone] = true;
            CheckOrigins(f, single, expected);
        }
        ++cases;
    }
    f.ResetPose(); std::fill_n(f.selected, 3, 1); f.selected[3] = 0; f.selected[4] = 1;
    f.model->selectedBone = 2;
    for (int kind = 1; kind < (build_profile::original ? 2 : 3); ++kind) {
        f.Hover(kind, 0);
        CheckOrigins(f, f.Draw(), {false, false, true, false, false});
    }
    f.app->state.coordinateSystem = 1; f.Hover(0, 2); f.Draw();
    f.app->InteractionDragMode() = static_cast<ViewportDragMode>(6);
    NoSectors(f, f.Draw(), "new multi-joint drag starts without stale sectors");
    f.Pose(.6f, 0, 0);
    auto frame = f.Draw();
    Require(SectorTriangles(frame, f.origins[0]) == 6, "parent joint records its own drag sector");
    Require(SectorTriangles(frame, f.origins[1]) == 0 && SectorTriangles(frame, f.origins[2]) == 0,
        "inherited parent motion does not count as child rotation");
    f.Pose(.6f, .27f, -.39f);
    frame = f.Draw();
    const unsigned sectors[] = {6, 3, 4};
    for (int i = 0; i < 3; ++i) Require(SectorTriangles(frame, f.origins[i]) == sectors[i],
        "each joint retains an independent signed rotation sweep across frames");
    Require(SectorArea(frame, f.origins[0]) * SectorArea(frame, f.origins[1]) > 0 &&
            SectorArea(frame, f.origins[1]) * SectorArea(frame, f.origins[2]) < 0,
            "opposite local rotations sweep their sectors in opposite directions");
    frame = f.Draw();
    for (int i = 0; i < 3; ++i) Require(SectorTriangles(frame, f.origins[i]) == sectors[i],
        "redrawing an unchanged pose does not accumulate extra rotation");
    f.selected[1] = 0; NoSectors(f, f.Draw(), "selection removal resets all sector histories");
    f.Pose(.8f, .27f, -.6f); frame = f.Draw();
    Require(SectorTriangles(frame, f.origins[2]) > 0, "remaining joints continue recording after a selection change");
    f.selected[1] = 1; NoSectors(f, f.Draw(), "selection addition starts fresh sector histories");
    f.Pose(1.f, .4f, -.8f); frame = f.Draw();
    Require(SectorTriangles(frame, f.origins[2]) > 0, "reselected group starts a new nonzero sweep");
    f.app->InteractionDragMode() = static_cast<ViewportDragMode>(5);
    NoSectors(f, f.Draw(), "changing the rotation axis clears old sector histories");
    f.app->InteractionDragMode() = static_cast<ViewportDragMode>(6); f.Draw();
    f.Pose(1.2f, .6f, -1.f); frame = f.Draw();
    Require(SectorTriangles(frame, f.origins[2]) > 0, "model lifecycle check begins with a nonzero sweep");
    f.app->ModelSlot(0) = nullptr; Require(f.Draw().calls == 0, "missing model draws no HUD");
    f.app->ModelSlot(0) = f.Raw(); NoSectors(f, f.Draw(), "model disappearance clears sector histories");
    f.Pose(1.4f, .8f, -1.2f); frame = f.Draw();
    Require(SectorTriangles(frame, f.origins[2]) > 0, "drag completion check begins with a nonzero sweep");
    f.Hover(0, 2); NoSectors(f, f.Draw(), "ending a drag clears sector histories");
    f.app->InteractionDragMode() = static_cast<ViewportDragMode>(6);
    NoSectors(f, f.Draw(), "a subsequent drag starts without a previous sweep");
    Require(!IsWindowVisible(f.window), "HUD regression remains hidden");
    std::printf("PASS multi-bone HUD: %u axis/coordinate/fixed-axis cases; GPU pivots, draw batches, independent sectors and lifecycle\n", cases);
}
} // namespace bone_multiselect_hud_tests
