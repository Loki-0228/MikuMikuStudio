#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/system_settings.hpp"
#include "../app/settings_json.hpp"

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
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/shadow_filter.hpp"
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
    // The ABI branch stores general preferences in data/settings.json. The
    // shadow section carries both the dynamic precision switch and the enhanced
    // filter flag; neither changes the project's keyed shadow range.
    std::string body;
    const auto result = system_settings::ReadSection("shadow", body);
    if (result != system_settings::Result::Missing) {
        app->AdaptiveShadowResolution() = false;
        app->ShadowFilterEnhanced() = false;
        if (result == system_settings::Result::Ok) try {
            const auto object = settings_json::Object(body);
            app->AdaptiveShadowResolution() = settings_json::Bool(object, "adaptive", false);
            app->ShadowFilterEnhanced() = settings_json::Bool(object, "filterEnhanced", false);
        } catch (...) {}
        return;
    }
    // Without a stored preference every scene uses the same fixed precision.
    FILE* file = nullptr; int enabled = 0;
    app->AdaptiveShadowResolution() = false;
    app->ShadowFilterEnhanced() = false;
    if (_wfopen_s(&file, PreferencePath(app).c_str(), L"rt") == 0) {
        if (fscanf_s(file, "%d", &enabled) == 1)
            app->AdaptiveShadowResolution() = enabled == 1;
        // A numeric second field in old files was a separate precision.
        // Only the named key opts into filtering.
        char line[128]{};
        while (fgets(line, sizeof(line), file)) {
            int enhanced = 0;
            if (sscanf_s(line, "filter_enhanced=%d", &enhanced) == 1)
                app->ShadowFilterEnhanced() = enhanced == 1;
        }
        fclose(file);
    }
    system_settings::CompareExchangeSection("shadow", nullptr, std::string("{\"adaptive\":") +
        settings_json::Bool(app->AdaptiveShadowResolution()) + ",\"filterEnhanced\":" +
        settings_json::Bool(app->ShadowFilterEnhanced()) + "}");
}
void SaveShadowResolutionPreference(MMDApp* app) {
    system_settings::WriteSection("shadow", std::string("{\"adaptive\":") +
        settings_json::Bool(app->AdaptiveShadowResolution()) + ",\"filterEnhanced\":" +
        settings_json::Bool(app->ShadowFilterEnhanced()) + "}");
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
    // The frame translates every model by the focus point. Keep that point
    // separately: the light projection extends its centre by the view offsets,
    // but the depth cut must use the point the picture is centred on.
    float frameFocus[3]{};
    float focus[3]{};
    const auto focusSource = ResolveFrameFocus(app, static_cast<int>(app->CameraReferenceMode()), frameFocus);
    for (int axis = 0; axis < 3; ++axis) focus[axis] = frameFocus[axis];
    if (focusSource == FrameFocusSource::FreeView) {
        focus[0] = app->ViewOffsetX(); focus[1] = app->ViewOffsetY();
    }
    // Coverage, depth units and the directional-light basis must not change
    // when the user changes precision. A separate detail layer covers the view.
    // Default mode keeps the original 9.32 range box clipped to the scene: one
    // cube for the whole picture, sized by the shadow range slider, so a large
    // object cannot scale the map that carries a character-sized self shadow.
    // Dynamic mode removes that range limit and fits the scene, then tunes the
    // map size so the near end keeps at least the slider precision.
    const float halfWidthLimit=app->AdaptiveShadowResolution()?INFINITY:
        1.f/NativeShadowLateralScale(app->ShadowDistance(),1);
    const auto lightProjection = NativeShadowProjection(focus,app->LightDirection(),.1f,1,
        &scene,halfWidthLimit);
    auto rampProjection = NativeShadowProjection(focus,app->LightDirection(),.1f,1);
    // Keep the mode-2 ramp's multiplier fixed too: camera-relative Y previously
    // changed both shadow bias and darkness when orbiting or changing precision.
    for(int row=0;row<4;++row)rampProjection[row*4+1]=0;
    Matrix cameraProjection{},cameraView{},worldView{},inverseView{},inverseProjection{};
    CameraShadowRegion cameraRegion;
    // Dynamic precision reads the same rendered picture: focus depth, final
    // projection and the render size the picture is rasterised at.
    float focusDepth=std::numeric_limits<float>::quiet_NaN();
    float projectedPixels=0.f;
    bool orthographicCamera=false;
    if(device && SUCCEEDED(device->GetTransform(D3DTS_PROJECTION,
            reinterpret_cast<D3DMATRIX*>(&cameraProjection))) &&
        SUCCEEDED(device->GetTransform(D3DTS_VIEW,reinterpret_cast<D3DMATRIX*>(&cameraView)))) {
        api.multiply(&worldView,reinterpret_cast<const Matrix*>(frame),&cameraView);
        if(api.inverse(&inverseView,nullptr,&worldView) &&
            api.inverse(&inverseProjection,nullptr,&cameraProjection)) {
            // Read the focus depth from the rendered transform. The camera panel
            // xyz values do not include the reference, attachment or preview
            // transforms that move the picture.
            float viewFocus[4]{};
            api.vec3Transform(viewFocus,frameFocus,&worldView);
            focusDepth=viewFocus[2];
            cameraRegion=CameraDepthShadowRegion(&inverseView.m[0][0],
                &inverseProjection.m[0][0],focusDepth);
        }
        orthographicCamera=cameraProjection.m[3][3]==1.f;
        projectedPixels=(std::max)(float(app->RenderWidth())*std::fabs(cameraProjection.m[0][0]),
            float(app->RenderHeight())*std::fabs(cameraProjection.m[1][1]));
    }
    Matrix inverseShadow{},toNative{};
    for(int axis=0;axis<4;++axis)toNative.m[axis][axis]=1;
    if(api.inverse(&inverseShadow,nullptr,reinterpret_cast<const Matrix*>(lightProjection.data())))
        api.multiply(&toNative,&inverseShadow,reinterpret_cast<const Matrix*>(rampProjection.data()));
    if(FAILED(FxSetMatrix(r->effect,"MdsShadowToNative",&toNative)))
        return ReferenceShadowProjection(app,frame); // unchanged SM2 fallback
    d3dx::FxSetInt(r->effect,"MdsStableShadowGrid",1);
    D3DCAPS9 caps{};
    if(!device || FAILED(device->GetDeviceCaps(&caps)))return lightProjection;
    const unsigned hardwareLimit=(std::min)(caps.MaxTextureWidth,caps.MaxTextureHeight);
    const unsigned maximum=(std::min)({8192u,hardwareLimit,app->ShadowResolutionLimit()*2u});
    const unsigned selectedQuality=ShadowQualityMapSize(app->ShadowDistance(),maximum);
    unsigned qualitySize=selectedQuality;
    if(app->AdaptiveShadowResolution()) {
        // Dynamic mode removed the range limit, so the demand follows the
        // scene-fitted coverage at the picture's focus depth. The chosen size
        // never falls below the selected tier, which keeps the same slider at
        // least as precise as the range-limited default.
        const float coverageWidth=scene.Valid()?
            2.f/MinimumShadowScale(lightProjection,scene):0.f;
        const float required=DynamicShadowMapDemand(projectedPixels,coverageWidth,
            focusDepth,orthographicCamera);
        qualitySize=DynamicShadowQualitySize(required,selectedQuality,
            app->DynamicShadowQuality(),maximum);
    }
    app->DynamicShadowQuality()=qualitySize;
    const unsigned detailSize=viewport_render_scale::ShadowSize(app,qualitySize);
    if(cameraRegion.valid) {
        const auto detailProjection=CameraDetailShadowProjection(cameraRegion,app->LightDirection(),lightProjection);
        // Every tier shares the final camera frustum and the per-pixel view-Z
        // transition. Only texture resolution changes with the precision slider.
        if(MinimumShadowScale(detailProjection,scene)>MinimumShadowScale(lightProjection,scene)*1.5f)
            shadow_filter::PlanDetail(app,detailProjection,lightProjection,&worldView.m[0][0],
                cameraRegion.farDepth,detailSize);
    }
    // The range-limited default box only spans a character-sized world area, so
    // it uses the selected tier directly instead of the half-size coverage map
    // that the scene-fitted dynamic mode needs for its wider box.
    const unsigned coverageSize=app->AdaptiveShadowResolution()?
        (std::max)(512u,qualitySize/2u):qualitySize;
    unsigned size=viewport_render_scale::ShadowSize(app,coverageSize);
    if(size<512)return lightProjection;
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
