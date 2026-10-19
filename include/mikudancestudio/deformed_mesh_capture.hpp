#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d9.h>
struct IDirect3DDevice9;
namespace mikudancestudio {
class MMDApp;
namespace deformed_mesh_capture {
struct Mesh {
    std::string name;
    // Optional source-index patch. UINT32_MAX means this vertex is not selected.
    // Patch playback changes only positions/normals of addressed source vertices.
    bool vertexPatch = false;
    std::vector<std::uint32_t> sourceVertices;
    std::vector<std::array<float,3>> positions;
    std::vector<std::array<float,3>> normals;
    std::vector<std::array<float,2>> uvs;
    std::vector<std::int32_t> indices;
};
class Session {
public:
    explicit Session(MMDApp& app, bool selectedOnly);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    void BeginFrame();
    const std::vector<Mesh>& EndFrame();
    const std::string& Error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Scope only the main material draw. Shadow maps, outlines and screen passes
// are excluded. The system-device hook sees the shader selected inside MME.
class MaterialScope {
public:
    MaterialScope(const void* object, unsigned material, bool enabled, bool opaqueCoverage=false);
    ~MaterialScope();
private:
    const void* oldObject_;
    unsigned oldMaterial_;
    bool oldOpaqueCoverage_;
};
// Only the native renderer may assert this after inspecting material/textures.
// Arbitrary MME shaders must retain their real alpha/discard coverage pass.
bool OpaqueCoverage();
bool Active();
void SetCamera(IDirect3DDevice9* device, const float world[16]);
// Shared native draw observer. Callbacks run after MME has selected its final
// shaders. Draws made by a callback are excluded from both observers.
class DrawObserver {
public:
    virtual ~DrawObserver() = default;
    virtual void Indexed(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT,
                         UINT, UINT, const void*, unsigned) noexcept = 0;
    virtual void Unsupported(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT) noexcept = 0;
};
bool Observe(MMDApp& app, DrawObserver* observer, std::string& error);
void StopObserving(DrawObserver* observer);
// Internal screen composition is not source geometry. Suppress both native
// draw observers and geometry export only for the scoped synthetic draw.
class ObserverSilenceScope {
public:
    ObserverSilenceScope();
    ~ObserverSilenceScope();
    ObserverSilenceScope(const ObserverSilenceScope&) = delete;
    ObserverSilenceScope& operator=(const ObserverSilenceScope&) = delete;
private:
    unsigned previous_;
};
}
}
