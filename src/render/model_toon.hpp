#pragma once
#include <d3d9.h>
namespace mikumikustudio {
class MMDApp;
class D3DRenderer;
namespace mdl { struct ModelRecord; struct ModelMaterialRecord; }
// Internal renderer query. Returned textures are borrowed from the app/cache.
IDirect3DTexture9* ResolveModelToonTexture(MMDApp*, D3DRenderer*,
    const mdl::ModelRecord&, const mdl::ModelMaterialRecord&);
// Base shader tint before applying per-material PMX morph multipliers.
void ResolveModelToonColor(MMDApp*, D3DRenderer*, const mdl::ModelRecord&,
    const mdl::ModelMaterialRecord&, float out[4]);
}
