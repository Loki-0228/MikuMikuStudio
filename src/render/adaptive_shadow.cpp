#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"

#if MMD_ORIGINAL_PROFILE
namespace mikumikustudio {
void LoadShadowResolutionPreference(MMDApp* app) {
    // The original build never consumes the full build's adaptive preference.
    app->AdaptiveShadowResolution() = false;
    app->ShadowFilterEnhanced() = false;
}
void SaveShadowResolutionPreference(MMDApp*) {}
std::array<float, 16> UpdateAdaptiveShadowMap(MMDApp*, const float[16]) { return {}; }
int ShadowSliderPosition(MMDApp* app) {
    // Preserve the original truncation followed by a positive half-step test.
    const double raw = 10000.0 - double(app->ShadowDistance()) * 100000.0;
    int position = static_cast<int>(raw);
    if (raw - double(position) >= 0.5) ++position;
    return position;
}
void SetShadowSliderPosition(MMDApp* app, int position) {
    app->ShadowDistance() = float((10000.0 - position) / 100000.0);
    app->SceneModified() = 1;
}
}
#else
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/d3dx_mesh.hpp"
#include "mikumikustudio/d3d_object_guard.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

extern "C" float* ExpGetAcsWorldMat(float*, int);
namespace mikumikustudio {
namespace {
std::wstring PreferencePath(MMDApp* app) {
    return std::wstring(app->ExeDir()) + L"\\Data\\adaptive_shadow.ini";
}
using Matrix = d3dx::D3DXMATRIXF;
std::array<float, 16> ReferenceShadowProjection(MMDApp* app, const float frame[16]) {
    auto& api = d3dx::Get();
    float target[3] = {app->ViewOffsetX(), app->ViewOffsetY(),
                       app->CameraDistance()};
    const D3DVECTOR& lightDirection = app->SceneLight().Direction;
    float eye[3] = {
        target[0] - lightDirection.x * 50.0f,
        target[1] - lightDirection.y * 50.0f,
        target[2] - lightDirection.z * 50.0f};
    const float up[3] = {0.0f, 0.0f, 1.0f};
    Matrix lightView;
    api.lookAtLH(&lightView, eye, target, up);

    float direction[3] = {lightDirection.x, lightDirection.y,
                          lightDirection.z};
    api.vec3Normalize(direction, direction);
    const float slope = -direction[2];
    const float edge = 1.0f - std::fabs(slope);
    const float base = app->ShadowDistance();
    const bool modeOne = app->ShadowMode() == 1;
    float scale;
    int part;
    if (modeOne) {
        part = 0;
        if (slope < -0.9f || slope > 0.9f)
            scale = base;
        else if (slope < -0.8f)
            scale = ((slope - 0.1f) * 10.0f + 10.0f) * base + base;
        else if (slope > 0.8f)
            scale = (10.0f - (slope + 0.1f) * 10.0f) * base + base;
        else
            scale = base + base;
    } else {
        part = 1;
        if (slope < -0.9f || slope > 0.9f)
            scale = base;
        else if (slope < -0.8f)
            scale = (slope * 20.0f + 19.0f) * base;
        else if (slope > 0.8f)
            scale = (19.0f - slope * 20.0f) * base;
        else
            scale = base * 3.0f;
    }

    Matrix shadowProjection{};
    shadowProjection.m[0][0] = scale;
    shadowProjection.m[1][1] = scale;
    shadowProjection.m[2][2] = base * 0.1500000059604645f;
    shadowProjection.m[3][3] = 1.0f;
    Matrix shadowMatrix;
    Matrix temp;
    api.multiply(&temp, reinterpret_cast<const Matrix*>(frame), &lightView);
    api.multiply(&shadowMatrix, &temp, &shadowProjection);
    Matrix transform;
    api.scaling(&transform, 1.0f, 1.0f, 2.0f);
    api.multiply(&shadowMatrix, &shadowMatrix, &transform);
    api.translation(&transform, 0.0f, 0.0f, -1.0f);
    api.multiply(&shadowMatrix, &shadowMatrix, &transform);

    Matrix orientation;
    std::memset(&orientation, 0, sizeof(orientation));
    for (int axis = 0; axis < 4; ++axis) orientation.m[axis][axis] = 1.f;
    float positive;
    float negative;
    if (slope < -0.9f || slope > 0.9f) {
        positive = edge;
        negative = -edge;
    } else if (slope < -0.8f) {
        positive = modeOne ? 4.0f * slope + 3.7f
                           : 9.0f * slope + 8.2f;
        negative = -9.0f * slope - 8.2f;
    } else if (slope > 0.8f) {
        positive = modeOne ? 3.7f - 4.0f * slope
                           : 8.2f - 9.0f * slope;
        negative = 9.0f * slope - 8.2f;
    } else {
        positive = modeOne ? 0.5f : 1.0f;
        negative = -1.0f;
    }
    orientation.m[1][3] = positive;
    orientation.m[3][1] = negative;
    api.multiply(&shadowMatrix, &shadowMatrix, &orientation);
    api.translation(&transform, 0.0f, 0.0f, 1.0f);
    api.multiply(&shadowMatrix, &shadowMatrix, &transform);
    api.scaling(&transform, 1.0f, 1.0f, 0.5f);
    api.multiply(&shadowMatrix, &shadowMatrix, &transform);
    std::array<float, 16> result{};
    std::memcpy(result.data(), &shadowMatrix, sizeof(shadowMatrix));
    return result;
}

void IncludeMeshBounds(IDirect3DVertexBuffer9* buffer, UINT stride, UINT count,
                       const Matrix* local, NativeShadowBounds& scene) {
    MMD_PERF_SCOPE(ShadowBounds);
    if (!buffer || !PlausibleD3dObject(buffer) || stride < 12) return;
    D3DVERTEXBUFFER_DESC desc{};
    if (FAILED(buffer->GetDesc(&desc))) return;
    count = (std::min)(count, desc.Size / stride);
    void* data = nullptr;
    if (!count) return;
    { MMD_PERF_SCOPE(ShadowBoundsLock);
      if (FAILED(buffer->Lock(0, 0, &data, D3DLOCK_READONLY))) return; }
    // Coverage must include every vertex, not a sparse density sample.
    // Accumulate the local AABB first; transform only its eight corners.
    const NativeShadowBounds mesh = NativeShadowVertexBounds(data, stride, count);
    buffer->Unlock();
    if (!mesh.Valid()) return;
    for (unsigned corner = 0; corner < 8; ++corner) {
        const auto point = mesh.Corner(corner);
        if (local) {
            float world[4]{};
            d3dx::Get().vec3Transform(world, point.data(), local);
            scene.Include(world);
        } else scene.Include(point.data());
    }
}
}

void LoadShadowResolutionPreference(MMDApp* app) {
    FILE* file = nullptr; int enabled = 0;
    app->ShadowFilterEnhanced() = false;
    if (_wfopen_s(&file, PreferencePath(app).c_str(), L"rt") == 0) {
        if (fscanf_s(file, "%d", &enabled) != 1) enabled = 0;
        // A numeric second field in old files was a separate precision.
        // Only the named key opts into filtering; old settings stay disabled.
        char line[128]{};
        while (fgets(line, sizeof(line), file)) {
            int enhanced = 0;
            if (sscanf_s(line, "filter_enhanced=%d", &enhanced) == 1)
                app->ShadowFilterEnhanced() = enhanced == 1;
        }
        fclose(file);
    }
    app->AdaptiveShadowResolution() = enabled == 1;
}
void SaveShadowResolutionPreference(MMDApp* app) {
    CreateDirectoryW((std::wstring(app->ExeDir()) + L"\\Data").c_str(), nullptr);
    FILE* file = nullptr;
    if (_wfopen_s(&file, PreferencePath(app).c_str(), L"wt") == 0) {
        fprintf(file, "%d\nfilter_enhanced=%d\n", app->AdaptiveShadowResolution() ? 1 : 0,
            app->ShadowFilterEnhanced() ? 1 : 0);
        fclose(file);
    }
}

int ShadowSliderPosition(MMDApp* app) {
    return int(std::lround(10000.0 - double(app->ShadowDistance()) * 100000.0));
}
void SetShadowSliderPosition(MMDApp* app, int position) {
    const float range = float((10000.0 - (std::clamp)(position, 0, 9999)) / 100000.0);
    if (app->ShadowDistance() != range) {
        app->ShadowDistance() = range;
        app->SceneModified() = 1;
    }
}

std::array<float, 16> UpdateAdaptiveShadowMap(MMDApp* app, const float frame[16]) {
    auto* r = app->Renderer();
    auto* device = r->device;
    auto& api = d3dx::Get();
    NativeShadowBounds scene;
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        auto* model = native_mesh::RenderModel(app->ModelSlot(slot));
        if (!model || app->ModelNonDisplayMode()) continue;
        const auto& record = *mdl::Mdl(model);
        if (!record.loadComplete || !record.indexCount) continue;
        const UINT stride = 32 + 16 * (std::min)(4u, UINT(record.pmxAdditionalUvCount));
        IncludeMeshBounds(static_cast<IDirect3DVertexBuffer9*>(record.vertexBuffer), stride,
            record.vertexCount, nullptr, scene);
    }
    int ordinal = 0;
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        auto* accessory = native_mesh::RenderAccessory(app->AccessorySlot(slot));
        if (!accessory) continue;
        const int index = ordinal++;
        if (!accessory->visible || accessory->opacity <= 0 || !accessory->mesh ||
            !PlausibleD3dObject(accessory->mesh) || d3dx::MeshFvf(accessory->mesh) != 274) continue;
        IDirect3DVertexBuffer9* buffer = nullptr;
        if (SUCCEEDED(d3dx::MeshVertexBuffer(accessory->mesh, &buffer)) && buffer) {
            Matrix local{};
            ExpGetAcsWorldMat(&local.m[0][0], index);
            IncludeMeshBounds(buffer, 32, UINT_MAX, &local, scene);
            buffer->Release();
        }
    }
    float focus[3]{};
    if (ResolveFrameFocus(app, static_cast<int>(app->CameraReferenceMode()), focus) == FrameFocusSource::FreeView) {
        focus[0] = app->ViewOffsetX(); focus[1] = app->ViewOffsetY();
    }
    const auto safeProjection = NativeShadowProjection(focus, app->LightDirection(),
        app->ShadowDistance(), app->ShadowMode(), &scene);
    auto referenceProjection = ReferenceShadowProjection(app, frame);
    const auto& direction = app->SceneLight().Direction;
    const float directionLength = direction.x*direction.x + direction.y*direction.y + direction.z*direction.z;
    const bool degenerate = directionLength < 1e-12f ||
        direction.x*direction.x + direction.y*direction.y < directionLength*1e-8f;
    if (degenerate || !std::all_of(referenceProjection.begin(), referenceProjection.end(),
            [](float value) { return std::isfinite(value); })) referenceProjection = safeProjection;
    const auto lightProjection = FitReferenceShadowProjection(referenceProjection, scene);
    const float referenceScale = MinimumShadowScale(referenceProjection, scene);
    D3DCAPS9 caps{};
    if (!device || FAILED(device->GetDeviceCaps(&caps))) return lightProjection;
    const float lateralScale = MinimumShadowScale(lightProjection, scene);
    const unsigned hardwareLimit = (std::min)(caps.MaxTextureWidth, caps.MaxTextureHeight);
    // The screen target alone is not the frame's pixel budget: fixed shadows
    // otherwise keep rasterizing up to 8192 squared even in a 25% preview.
    // Dynamic demand already uses the scaled viewport; only cap it here.
    const unsigned limit = viewport_render_scale::ShadowSize(app, (std::max)(
        FittedShadowResolutionLimit(app->ShadowResolutionLimit(), referenceScale, lateralScale, hardwareLimit),
        FittedShadowResolutionLimit(app->ShadowResolutionLimit(),
            NativeShadowLateralScale(app->ShadowDistance(), app->ShadowMode()),
            MinimumShadowScale(safeProjection, scene), hardwareLimit)));
    if (limit < 512) return lightProjection;
    unsigned size = limit;
    if (app->AdaptiveShadowResolution()) {
        Matrix view{}, projection{}, worldView{};
        D3DVIEWPORT9 viewport{};
        device->GetTransform(D3DTS_VIEW, reinterpret_cast<D3DMATRIX*>(&view));
        device->GetTransform(D3DTS_PROJECTION, reinterpret_cast<D3DMATRIX*>(&projection));
        device->GetViewport(&viewport);
        api.multiply(&worldView, reinterpret_cast<const Matrix*>(frame), &view);
        float nearest = std::numeric_limits<float>::infinity();
        float farthest = -std::numeric_limits<float>::infinity();
        NativeShadowBounds viewBounds;
        if (scene.Valid()) {
            for (unsigned corner = 0; corner < 8; ++corner) {
                const auto point = scene.Corner(corner);
                float transformed[4]{};
                api.vec3Transform(transformed, point.data(), &worldView);
                viewBounds.Include(transformed);
                nearest = (std::min)(nearest, transformed[2]);
                farthest = (std::max)(farthest, transformed[2]);
            }
            nearest = farthest > 0 ? (std::max)(.0001f, nearest) : std::numeric_limits<float>::infinity();
        }
        const float projectedPixels = (std::max)(viewport.Width * std::fabs(projection.m[0][0]),
                                                viewport.Height * std::fabs(projection.m[1][1]));
        const float required = RequiredShadowMapSizeForScale(lateralScale,
            projectedPixels, nearest, projection.m[3][3] == 1.f) * ShadowSamplingFactor(app->ShadowDistance());
        // The native projective grid compresses at large camera distances.
        // Its worst Jacobian alone can request a huge map for a model only
        // one screen pixel tall. Bound that demand by visible scene extent,
        // with 32 map texels per screen pixel and the existing 512 minimum.
        // Near/large scenes retain the full fitted-coverage resolution cap.
        unsigned footprintLimit = limit;
        if (viewBounds.Valid() && viewBounds.minimum[2] > .0001f) {
            const float depth = projection.m[3][3] == 1.f ? 1.f : viewBounds.minimum[2];
            const float extent = (std::max)(viewBounds.maximum[0]-viewBounds.minimum[0],
                                             viewBounds.maximum[1]-viewBounds.minimum[1]);
            const float footprint = extent*projectedPixels/(2.f*depth);
            footprintLimit = 512;
            while (footprintLimit < limit && footprintLimit < footprint*32.f) footprintLimit *= 2;
            footprintLimit = (std::min)(footprintLimit, limit);
        }
        size = AdaptiveShadowSize(required, unsigned(r->renderTargetWidth), footprintLimit);
    }
    if (r->hdrTexture && r->shadowSurface && r->shadowDepthSurface && unsigned(r->renderTargetWidth) == size &&
        unsigned(r->renderTargetHeight) == size) return lightProjection;
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* surface = nullptr;
    IDirect3DSurface9* depth = nullptr;
    while (size >= 512) {
        if (SUCCEEDED(d3dx::Get().createTexture(device, size, size, 1, D3DUSAGE_RENDERTARGET,
                D3DFMT_R32F, D3DPOOL_DEFAULT, &texture)) &&
            SUCCEEDED(texture->GetSurfaceLevel(0, &surface)) &&
            SUCCEEDED(device->CreateDepthStencilSurface(size, size, D3DFMT_D24X8,
                D3DMULTISAMPLE_NONE, 0, FALSE, &depth, nullptr))) break;
        if (depth) { depth->Release(); depth = nullptr; }
        if (surface) { surface->Release(); surface = nullptr; }
        if (texture) { texture->Release(); texture = nullptr; }
        size /= 2;
    }
    if (!texture || !surface || !depth) return lightProjection;
    if (r->hdrTexture) r->hdrTexture->Release();
    if (r->shadowSurface) r->shadowSurface->Release();
    if (r->shadowDepthSurface) r->shadowDepthSurface->Release();
    r->hdrTexture = texture; r->shadowSurface = surface; r->shadowDepthSurface = depth;
    r->renderTargetWidth = r->renderTargetHeight = size;
    return lightProjection;
}
}

#endif
