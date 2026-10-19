#pragma once
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_alpha_pass.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/texture_lookup.hpp"
#include "mmv/mms.h"
#include <cmath>
#include <vector>

namespace mikumikustudio::mmv4mms {
struct MaterialPassContext {
    MMDApp* app;
    const std::vector<MmvObject>* objects;
};

// Read the same cached alpha tags as the native model renderer. Evaluated
// inline textures have no host resource entry, so inspect their CPU alpha
// directly. An unknown plugin texture stays translucent without a prepass.
inline bool MaterialTextureUsesAlpha(D3DRenderer* renderer,
                                     const MmvTexture& texture, bool& known) {
    if (texture.rgba && texture.width && texture.height) {
        if (texture.width > 16384 || texture.height > 16384) {
            known = false;
            return true;
        }
        const auto count = std::size_t(texture.width) * texture.height;
        for (std::size_t i = 0; i < count; ++i)
            if (!std::isfinite(texture.rgba[i * 4 + 3]) || texture.rgba[i * 4 + 3] < 1.f)
                return true;
        return false;
    }
    if (!texture.path || !*texture.path) return false;
    bool alpha = false;
    if (native_mesh::TextureProperties(texture.path, nullptr, &alpha)) return alpha;
    if (const auto* resource = FindTextureResource(renderer, texture.path)) {
        const auto tag = reinterpret_cast<const unsigned char*>(&resource->tag)[3];
        known &= (tag & kTextureAlphaKnown) != 0;
        return (tag & kTextureAlphaKnown) == 0 || (tag & kTextureUsesAlpha) != 0;
    }
    known = false;
    return true;
}

inline int MMV_CALL ReadMaterialPass(void* pointer, uint32_t objectIndex,
    const MmvMaterial* material, uint32_t effectFlags, uint32_t* output) {
    const auto* context = static_cast<const MaterialPassContext*>(pointer);
    if (!context || !context->app || !context->objects || !material || !output ||
        objectIndex >= context->objects->size() ||
        (effectFlags & ~MMV_MMS_MATERIAL_CUSTOM_EFFECT) ||
        !std::isfinite(material->diffuse[3])) return MMV_INVALID_ARGUMENT;
    const auto& object = (*context->objects)[objectIndex];
    if (object.kind != MMV_MODEL || !object.id) return MMV_INVALID_ARGUMENT;
    const auto& model = *reinterpret_cast<const mdl::ModelRecord*>(uintptr_t(object.id));
    if (model.displayState) {
        *output = MMV_MMS_MATERIAL_ORIGINAL;
        return MMV_OK;
    }
    const bool custom = (effectFlags & MMV_MMS_MATERIAL_CUSTOM_EFFECT) != 0;
    const float alpha = material->diffuse[3];
    if (model.postLoadFlag2 || (alpha < 1.f && (custom || MaterialAlphaVisible(alpha)))) {
        *output = MMV_MMS_MATERIAL_TRANSLUCENT;
        return MMV_OK;
    }
    if (!custom && !MaterialAlphaVisible(alpha)) {
        *output = MMV_MMS_MATERIAL_OPAQUE;
        return MMV_OK;
    }
    bool known = true;
    bool usesAlpha = MaterialTextureUsesAlpha(context->app->Renderer(), material->texture, known);
    if (model.physicsMode != 2 || material->sphereMode != 0)
        usesAlpha |= MaterialTextureUsesAlpha(context->app->Renderer(), material->sphere, known);
    *output = !usesAlpha ? MMV_MMS_MATERIAL_OPAQUE :
        (!custom && known ? MMV_MMS_MATERIAL_COVERAGE : MMV_MMS_MATERIAL_TRANSLUCENT);
    return MMV_OK;
}
} // namespace mikumikustudio::mmv4mms
