#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/d3dx_dyn.hpp"

namespace mikumikustudio {
class MMDApp;
namespace mdl { struct AccessoryRecord; struct AccessoryKey; }
namespace import_texture_cache { class Preparation; }
namespace accessory_import {
enum class Result { Ready, InvalidPath, NotFound, InvalidVac, InvalidMesh, NotReady, Memory, TextureFailure };

// Owns an unpublished accessory and its registered key pool. Cancellation only
// releases these private resources; it never touches scene slots or UI. The
// renderer/device must outlive this object and the caller's texture preparation.
class Prepared final {
    mdl::AccessoryRecord* object_ = nullptr;
    mdl::AccessoryKey* keys_ = nullptr;
    void Reset() noexcept;
    friend Result Prepare(MMDApp&, const wchar_t*, import_texture_cache::Preparation&, Prepared&) noexcept;
public:
    Prepared() noexcept = default;
    ~Prepared() noexcept;
    Prepared(Prepared&&) noexcept;
    Prepared& operator=(Prepared&&) noexcept;
    Prepared(const Prepared&) = delete;
    Prepared& operator=(const Prepared&) = delete;
    mdl::AccessoryRecord* Object() const noexcept { return object_; }
    mdl::AccessoryKey* Keys() const noexcept { return keys_; }
    // Both destinations must be empty. The caller assumes ownership, including
    // KeyPoolDestroy for keys. No scene membership or texture cache is published.
    void Release(mdl::AccessoryRecord*& object, mdl::AccessoryKey*& keys) noexcept;
};

// UI/renderer thread only, under the caller's scene-operation guard. The input
// is an absolute X/VAC path, at most 255 wchar_t. out must be empty. Failure
// leaves out empty; discard the caller's texture preparation on any failure.
// A VAC keeps its sourcePath, resolves its mesh relative to the VAC directory,
// and resolves textures relative to that mesh. No dialog, message dispatch,
// working-directory/path-workspace write, slot, selection, dirty flag or effect
// sidecar import occurs here. D3DX/device calls remain external dependencies.
Result Prepare(MMDApp&, const wchar_t* absolutePath,
               import_texture_cache::Preparation&, Prepared& out) noexcept;

namespace testing {
// Internal CPU-test seam; normal calls use the production D3DX entry point and
// NormalizeAccessoryMeshFvf. Scopes are thread-local and must unwind in LIFO
// order. The loader owns every mesh/material buffer returned even on failure.
struct MeshApi {
    d3dx::FnLoadMeshFromXW load = nullptr;
    bool (*normalize)(void*&, IDirect3DDevice9*) = nullptr;
};
class ScopedMeshApi final {
    MeshApi api_;
    const MeshApi* previous_;
public:
    explicit ScopedMeshApi(MeshApi) noexcept;
    ~ScopedMeshApi() noexcept;
    ScopedMeshApi(const ScopedMeshApi&) = delete;
    ScopedMeshApi& operator=(const ScopedMeshApi&) = delete;
};
}
}
}
#endif
