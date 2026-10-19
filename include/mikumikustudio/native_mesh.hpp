#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "mikumikustudio/deformed_mesh_capture.hpp"
#include "mikumikustudio/native_mesh_types.hpp"

namespace mikumikustudio {
class MMDApp;
namespace mdl { struct AccessoryRecord; }
namespace native_mesh {
void Initialize(MMDApp*);
void Shutdown();
void Evaluate(MMDApp*, const float world[16]);



void ClearScene();
void Forget(const void* object);
void ProjectOpened(MMDApp*, const wchar_t*);
void DeviceLost();
unsigned char* RenderModel(unsigned char* source);
mdl::AccessoryRecord* RenderAccessory(mdl::AccessoryRecord* source);
const void* SourceObject(const void* renderObject);
const wchar_t* MaterialName(uint32_t objectId, int material, bool english);
bool DisableMeshEffects(MMDApp*, const std::vector<const void*>& objects, std::string& error);
void ClearMeshBake(MMDApp*, bool selectedOnly);
void ClearMeshBakeObjects(MMDApp*, const std::vector<const void*>&);

void EffectiveWeights(std::uint32_t weightType,const int bones[4],const float weights[4],
                      int outBones[4],float outWeights[4]) noexcept;
void SetBakeCapture(bool enabled);
bool ReadBakeCapture(std::vector<deformed_mesh_capture::Mesh>& meshes, std::string& error);
bool KeyBakedEffects(MMDApp*, const std::vector<const void*>& objects, int first, int last, std::string& error);
const std::int32_t* Indices32(unsigned char* renderModel);

enum class LayerBakeSupport { NoPlugin, MissingApi, Available };
LayerBakeSupport LayerBakeState();

bool BakeLayerList(std::vector<std::uint32_t>& layers, std::string& error);

struct LayerBone {
    std::string key, sourceKey, name;
    std::uint32_t generated = 0, valid = 0;
    std::int32_t parent = -1;
    float world[16]{};
    float origin[3]{};
    float skinMatrix[16]{};
};

struct LayerBindingVertex {
    float bindPosition[3]{}, bindNormal[3]{};
    float deformedPosition[3]{}, deformedNormal[3]{};
    bool deformedValid = false;

    std::uint32_t weightType = 1;
    std::int32_t bones[4]{-1, -1, -1, -1};
    float weights[4]{};
    float sdefCenter[3]{}, sdefR0[3]{}, sdefR1[3]{};
};
struct LayerMorph {
    std::string name;
    float value = 0;
    std::uint32_t type = 0;
};
struct LayerRigid {
    std::string key, sourceKey, name;
    std::uint32_t generated = 0, valid = 0;
    std::int32_t bone = -1;
    float world[16]{};
};
struct LayerObject {
    std::uint64_t objectId = 0;
    std::string key;
    std::uint32_t flags = 0;

    bool structureSynchronized = false;
    bool visible = true;

    const mesh_data::MeshView* mesh = nullptr;
    std::vector<LayerBone> bones;
    std::vector<LayerMorph> morphs;
    std::vector<LayerRigid> rigids;

    std::vector<LayerBindingVertex> bindings;
    std::vector<std::uint32_t> vertexBindings;
    std::uint64_t structureVersion = 0, meshVersion = 0;
};
struct LayerOutput {
    std::uint64_t evaluationId = 0;
    double timeSeconds = 0;
    std::vector<LayerObject> objects;
};

class LayerSampling {
public:
    LayerSampling();
    ~LayerSampling();
    LayerSampling(const LayerSampling&) = delete;
    LayerSampling& operator=(const LayerSampling&) = delete;

    bool Begin(std::uint32_t layer, std::string& error);
    void End() noexcept;
    bool Active() const noexcept;
    std::uint32_t Layer() const noexcept;
    std::uint64_t Generation() const noexcept;

    bool Query(LayerOutput& output, std::string& error, LayerOutput* source = nullptr) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class LayerBakeTransaction {
public:
    LayerBakeTransaction();
    ~LayerBakeTransaction();
    LayerBakeTransaction(const LayerBakeTransaction&) = delete;
    LayerBakeTransaction& operator=(const LayerBakeTransaction&) = delete;
    bool Begin(std::string& error);
    bool StageRemoval(const std::vector<std::uint64_t>& objectIds, std::uint32_t layer,
                      std::string& error);
    bool Commit(std::string& error);
    void Cancel() noexcept;
    bool Open() const noexcept;
    std::uint64_t Generation() const noexcept;
private:
    bool open_ = false;
    std::uint64_t generation_ = 0;
};

void* Texture(const wchar_t* path);
bool TextureProperties(const wchar_t* path, float* color, bool* usesAlpha);
void* AccessoryMesh(void* accessory);

}
}
