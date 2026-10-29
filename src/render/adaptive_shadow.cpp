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
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/shadow_filter.hpp"
#include "mikumikustudio/d3dx_mesh.hpp"
#include "mikumikustudio/d3d_object_guard.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

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
                       const Matrix* local, NativeShadowBounds& scene,
                       unsigned char* model = nullptr, NativeShadowBounds* objectBounds = nullptr) {
    MMD_PERF_SCOPE(ShadowBounds);
    if (!buffer || !PlausibleD3dObject(buffer) || stride < 12) return;
    NativeShadowBounds mesh;
    if (!model || !model_vertex_cache::ShadowBounds(model, buffer, stride, count, mesh)) {
        D3DVERTEXBUFFER_DESC desc{};
        if (FAILED(buffer->GetDesc(&desc))) return;
        const UINT requested = count;
        count = (std::min)(count, desc.Size / stride);
        void* data = nullptr;
        if (!count) return;
        { MMD_PERF_SCOPE(ShadowBoundsLock);
          if (FAILED(buffer->Lock(0, 0, &data, D3DLOCK_READONLY))) return; }
        // Coverage still includes every vertex on a miss. Cache only the exact
        // bounds of a body already verified unchanged during this render.
        mesh = NativeShadowVertexBounds(data, stride, count);
        const HRESULT unlocked = buffer->Unlock();
        if (FAILED(unlocked)) return;
        if (model) model_vertex_cache::RememberShadowBounds(model, buffer, stride, requested, mesh);
    }
    if (!mesh.Valid()) return;
    for (unsigned corner = 0; corner < 8; ++corner) {
        const auto point = mesh.Corner(corner);
        if (local) {
            float world[4]{};
            d3dx::Get().vec3Transform(world, point.data(), local);
            scene.Include(world);
            if(objectBounds)objectBounds->Include(world);
        } else {scene.Include(point.data());if(objectBounds)objectBounds->Include(point.data());}
    }
}
}

void LoadShadowResolutionPreference(MMDApp* app) {
    FILE* file = nullptr; int enabled = 0;
    // Without a stored preference every scene uses the same fixed precision.
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
    std::vector<NativeShadowBounds> objects;
    std::vector<std::pair<unsigned char*,NativeShadowBounds>> modelBounds;
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        auto* model = native_mesh::RenderModel(app->ModelSlot(slot));
        if (!model || app->ModelNonDisplayMode()) continue;
        const auto& record = *mdl::Mdl(model);
        if (!record.loadComplete || !record.indexCount) continue;
        const UINT stride = 32 + 16 * (std::min)(4u, UINT(record.pmxAdditionalUvCount));
        NativeShadowBounds bounds;
        IncludeMeshBounds(static_cast<IDirect3DVertexBuffer9*>(record.vertexBuffer), stride,
            record.vertexCount, nullptr, scene, model, &bounds);
        if(bounds.Valid()){objects.push_back(bounds);modelBounds.emplace_back(model,bounds);}
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
            NativeShadowBounds bounds;
            IncludeMeshBounds(buffer, 32, UINT_MAX, &local, scene, nullptr, &bounds);
            if(bounds.Valid())objects.push_back(bounds);
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
    // One coverage box for the whole visible picture, shared by every object.
    // Z remains scene-wide to retain casters outside the camera frustum.
    auto lightProjection=NativeShadowProjection(focus,app->LightDirection(),.1f,1,&scene);
    const auto rampProjection=NativeShadowComparisonProjection(app->LightDirection(),app->ShadowMode());
    Matrix cameraProjection{},cameraView{},worldView{},inverseView{},inverseProjection{};
    // Dynamic precision reads the final projection and rasterised viewport.
    float projectedPixels=0.f;
    bool orthographicCamera=false;
    if(device && SUCCEEDED(device->GetTransform(D3DTS_PROJECTION,
            reinterpret_cast<D3DMATRIX*>(&cameraProjection))) &&
        SUCCEEDED(device->GetTransform(D3DTS_VIEW,reinterpret_cast<D3DMATRIX*>(&cameraView)))) {
        api.multiply(&worldView,reinterpret_cast<const Matrix*>(frame),&cameraView);
        if(api.inverse(&inverseView,nullptr,&worldView))
            api.inverse(&inverseProjection,nullptr,&cameraProjection);
        orthographicCamera=cameraProjection.m[3][3]==1.f;
        D3DVIEWPORT9 viewport{};
        const bool viewportValid=SUCCEEDED(device->GetViewport(&viewport)) && viewport.Width && viewport.Height;
        projectedPixels=(std::max)(float(viewportValid?viewport.Width:app->RenderWidth())*std::fabs(cameraProjection.m[0][0]),
            float(viewportValid?viewport.Height:app->RenderHeight())*std::fabs(cameraProjection.m[1][1]));
    }
    Matrix worldClip{};
    api.multiply(&worldClip,&worldView,&cameraProjection);
    NativeShadowBounds visible;
    for(const auto& object:objects) {
        const auto clipped=VisibleShadowLightBounds(object,&worldClip.m[0][0],lightProjection.data());
        if(clipped.Valid()){visible.Include(clipped.minimum.data());visible.Include(clipped.maximum.data());}
    }
    lightProjection=FitVisibleShadowBox(lightProjection,visible);
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
    const unsigned referenceSize=app->ShadowResolutionLimit(); // original 2048 / 4096 map
    const float density=NativeShadowSamplingDensity(app->LightDirection(),app->ShadowDistance(),
        app->ShadowMode(),referenceSize);
    const float finestDensity=NativeShadowSamplingDensity(app->LightDirection(),.1f,
        app->ShadowMode(),referenceSize);
    // Neither object bounds nor the selected slider/mode change the depth
    // regions. On limited hardware reserve room for either original mode.
    const float finestBudget=(std::max)(NativeShadowSamplingDensity(app->LightDirection(),.1f,1,referenceSize),
        NativeShadowSamplingDensity(app->LightDirection(),.1f,2,referenceSize));
    const float nearRadius=(std::min)(5.f,(maximum>2u?maximum-2u:1u)/(2.f*finestBudget));
    const auto nearRegion=CameraNearShadowRegion(&inverseView.m[0][0],&inverseProjection.m[0][0],nearRadius);
    // Overlapping camera-depth regions independent of the orbit target. Moving
    // the target and distance together to preserve the view must preserve shadows.
    // A nearby wall must not inherit a 153-unit focus slab merely because the
    // camera target lies behind it. Give close buildings a fully detailed core
    // before the outer transition, then bridge to full-picture coverage.
    const auto middleRegion=CameraNearShadowRegion(&inverseView.m[0][0],&inverseProjection.m[0][0],
        nearRadius*5.f);
    const auto coverageBudgetRegion=CameraNearShadowRegion(&inverseView.m[0][0],&inverseProjection.m[0][0],
        nearRadius*16.f);
    // A close view of a large stage can still lie beyond the middle slab. Give
    // that picture a third screen-sampled layer instead of dropping straight
    // from the middle map to the far budget. Geometry is independent of object
    // size, slider and mode; fixed precision keeps its original bridge.
    const bool dynamic=app->AdaptiveShadowResolution();
    const auto bridgeRegion=dynamic?CameraNearShadowRegion(&inverseView.m[0][0],&inverseProjection.m[0][0],
        nearRadius*24.f):coverageBudgetRegion;
    const auto blendStart=[](const CameraShadowRegion& region,float fraction) {
        return region.farDepth-(region.farDepth-region.nearDepth)*fraction;
    };
    const float coverageWidth=2.f/MinimumShadowScale(lightProjection,scene);
    // Preserve the slider's native world-space density in the full-picture
    // map. A smaller screen-based budget exposed visibly coarse far shadows
    // before the first transition even began. The actual density may still be
    // lower when the coverage box exceeds the device's maximum texture size.
    const float coverageDensity=density;
    unsigned size=ShadowTextureSizeForDensity(coverageWidth,coverageDensity,maximum);
    const bool reuse=r->hdrTexture && r->shadowSurface && r->shadowDepthSurface &&
        unsigned(r->renderTargetWidth)==size && unsigned(r->renderTargetHeight)==size;
    if(!reuse) {
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
        if (texture && surface && depth) {
            if (r->hdrTexture) r->hdrTexture->Release();
            if (r->shadowSurface) r->shadowSurface->Release();
            if (r->shadowDepthSurface) r->shadowDepthSurface->Release();
            r->hdrTexture = texture; r->shadowSurface = surface; r->shadowDepthSurface = depth;
            r->renderTargetWidth = r->renderTargetHeight = size;
        }
    }
    // Use the actual allocation, also after a failed resize. Expanding the box
    // absorbs texture minimums without making characters sharper than stages.
    size=(std::max)(1,r->renderTargetWidth);
    app->DynamicShadowQuality()=size;
    lightProjection=FitShadowSamplingDensity(lightProjection,coverageDensity,size);
    if(api.inverse(&inverseShadow,nullptr,reinterpret_cast<const Matrix*>(lightProjection.data())))
        api.multiply(&toNative,&inverseShadow,reinterpret_cast<const Matrix*>(rampProjection.data()));
    FxSetMatrix(r->effect,"MdsShadowToNative",&toNative);
    // Differentiate camera-relative depth. A coarse shadow texel can span
    // thousands of screen pixels; gradients of offset depth lose significant
    // bits and then amplify that error during receiver-plane correction.
    float derivativeOffset=0.f;
    if(app->ShadowFilterEnhanced()) {
        derivativeOffset=.5f;
        const float w=inverseView.m[3][3];
        if(std::isfinite(w) && std::fabs(w)>1e-8f) {
            float z=lightProjection[14];
            for(int axis=0;axis<3;++axis)
                z+=inverseView.m[3][axis]/w*lightProjection[axis*4+2];
            if(std::isfinite(z))derivativeOffset=z;
        }
    }
    d3dx::FxSetFloatArray(r->effect,"MdsShadowDerivativeOffset",&derivativeOffset,1);
    // Fixed mode retains the full scene map and adds native-density receiver
    // boxes only where the global texture limit diluted a smaller model.
    // Whole-model bounds, not camera distance, determine this grid.
    if(!dynamic) {
        const float globalDensity=MinimumShadowScale(lightProjection,scene)*size*.5f;
        if(globalDensity < density*(1.f-1e-4f))for(const auto& entry:modelBounds) {
            if(!VisibleShadowLightBounds(entry.second,&worldClip.m[0][0],lightProjection.data()).Valid())continue;
            auto projection=FixedModelShadowProjection(lightProjection,entry.second);
            const float width=2.f/MinimumShadowScale(projection,entry.second);
            const unsigned modelSize=ShadowTextureSizeForDensity(width,density,maximum);
            projection=FitShadowSamplingDensity(projection,density,modelSize);
            const float modelDensity=MinimumShadowScale(projection,entry.second)*modelSize*.5f;
            if(modelDensity>globalDensity*(1.f+1e-4f))
                shadow_filter::PlanModel(app,entry.first,projection,lightProjection,modelSize,density);
        }
        return lightProjection;
    }
    const float middleStart=dynamic?(std::max)(nearRegion.farDepth,middleRegion.farDepth/1.5f):nearRegion.farDepth;
    const auto planLayer=[&](const CameraShadowRegion& region,unsigned layer,float demandDepth,float blendFraction) {
        if(!region.valid)return;
        auto projection=CameraDetailShadowProjection(region,app->LightDirection(),lightProjection,0);
        const float width=2.f/MinimumShadowScale(projection,scene);
        const float layerDensity=ShadowLayerDensity(density,finestDensity,projectedPixels,demandDepth,
            orthographicCamera,dynamic,layer!=2 || dynamic,(maximum-1.f)/width);
        const unsigned layerSize=ShadowTextureSizeForDensity(width,layerDensity,maximum);
        float nearDepth=region.nearDepth,farDepth=region.farDepth;
        if(dynamic && layer==2) {
            // Reuse the bridge's existing square and sample budget. Packing
            // only shifts XY; it neither enlarges the map nor changes shared Z.
            const auto packed=FitCameraShadowBox(&inverseView.m[0][0],&inverseProjection.m[0][0],
                projection,middleStart);
            if(packed.valid && packed.farDepth>region.farDepth) {
                projection=packed.projection;nearDepth=packed.nearDepth;farDepth=packed.farDepth;
                const float start=(std::max)(middleRegion.farDepth,farDepth/2.5f);
                blendFraction=ShadowTransitionFraction(nearDepth,start,farDepth);
            }
        }
        // Build the unsnapped box first. Snap only after its final footprint is
        // chosen, so every layer uses the same world grid at the same density.
        projection=FitShadowSamplingDensity(projection,layerDensity,layerSize);
        shadow_filter::PlanDetail(app,projection,lightProjection,&worldView.m[0][0],
            farDepth,layerSize,layer,layerDensity,nearDepth,blendFraction,
            0.f,orthographicCamera);
    };
    // Keep the old dynamic middle core explicitly. Recomputing it from the new
    // bridge density would make slider changes move the transition and could
    // make a finer setting locally coarser. All fades are quality independent.
    // The outer fade has a broad core:
    // bringing the middle map into view no longer exposes a magnified far grid.
    // Beyond this layer the coverage map retains its separate fixed-mode cap.
    planLayer(nearRegion,1,nearRegion.nearDepth,.35f);
    planLayer(middleRegion,0,blendStart(nearRegion,.35f),
        ShadowTransitionFraction(middleRegion.nearDepth,middleStart,middleRegion.farDepth));
    const float bridgeStart=dynamic?(std::max)(middleRegion.farDepth,bridgeRegion.farDepth/2.5f):middleRegion.farDepth;
    planLayer(bridgeRegion,2,middleRegion.farDepth,
        ShadowTransitionFraction(bridgeRegion.nearDepth,bridgeStart,bridgeRegion.farDepth));
    return lightProjection;
}
}

#endif
