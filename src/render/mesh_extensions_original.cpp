#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/alembic_export.hpp"
#include "mikumikustudio/deformed_mesh_capture.hpp"
#include "mikumikustudio/mesh_bake_cache.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/native_mesh.hpp"

static_assert(MMD_ORIGINAL_PROFILE, "The original mesh provider belongs to the mmd932 profile");

namespace mikumikustudio {
namespace {
constexpr char kUnavailable[] = "Mesh capture, Alembic and mesh baking are unavailable in the MMD 9.32 profile";

bool Unavailable(std::string& error) {
    error = kUnavailable;
    return false;
}
}

namespace native_mesh {
void Initialize(MMDApp*) {}
void Shutdown() {}
void Evaluate(MMDApp*, const float[16]) {}
void ClearScene() {}
void Forget(const void*) {}
void ProjectOpened(MMDApp*, const wchar_t*) {}
void DeviceLost() {}

unsigned char* RenderModel(unsigned char* source) { return source; }
mdl::AccessoryRecord* RenderAccessory(mdl::AccessoryRecord* source) { return source; }
const void* SourceObject(const void* object) { return object; }
const wchar_t* MaterialName(std::uint32_t, int, bool) { return nullptr; }
void* Texture(const wchar_t*) { return nullptr; }
bool TextureProperties(const wchar_t*, float*, bool*) { return false; }

// These are native data queries as well as proxy hooks. Keep their fallback
// semantics: a PMX uses 32-bit indices, a PMD does not, and an accessory owns
// its original mesh. Returning null for all three would break ordinary draws.
const std::int32_t* Indices32(unsigned char* model) {
    return model && mdl::Mdl(model)->physicsMode == 2 ? mdl::PmxIndices(model) : nullptr;
}
void* AccessoryMesh(void* accessory) {
    return accessory ? mdl::Accessory(accessory)->mesh : nullptr;
}

void EffectiveWeights(std::uint32_t weightType, const int bones[4], const float weights[4],
                      int outBones[4], float outWeights[4]) noexcept {
    for (int i = 0; i < 4; ++i) {
        outBones[i] = bones[i];
        outWeights[i] = weights[i];
    }
    if (weightType == 0) {
        outWeights[0] = 1.f;
        outWeights[1] = outWeights[2] = outWeights[3] = 0.f;
        outBones[1] = outBones[2] = outBones[3] = -1;
    } else if (weightType == 1 || weightType == 3) {
        outWeights[1] = 1.f - outWeights[0];
        outWeights[2] = outWeights[3] = 0.f;
        outBones[2] = outBones[3] = -1;
    }
}

bool DisableMeshEffects(MMDApp*, const std::vector<const void*>&, std::string& error) {
    return Unavailable(error);
}
void ClearMeshBake(MMDApp*, bool) {}
void ClearMeshBakeObjects(MMDApp*, const std::vector<const void*>&) {}
void SetBakeCapture(bool) {}
bool ReadBakeCapture(std::vector<deformed_mesh_capture::Mesh>& meshes, std::string& error) {
    meshes.clear();
    return Unavailable(error);
}
bool KeyBakedEffects(MMDApp*, const std::vector<const void*>&, int, int, std::string& error) {
    return Unavailable(error);
}

LayerBakeSupport LayerBakeState() { return LayerBakeSupport::NoPlugin; }
bool BakeLayerList(std::vector<std::uint32_t>& layers, std::string& error) {
    layers.clear();
    return Unavailable(error);
}
struct LayerSampling::Impl {};
LayerSampling::LayerSampling() = default;
LayerSampling::~LayerSampling() = default;
bool LayerSampling::Begin(std::uint32_t, std::string& error) { return Unavailable(error); }
void LayerSampling::End() noexcept {}
bool LayerSampling::Active() const noexcept { return false; }
std::uint32_t LayerSampling::Layer() const noexcept { return 0; }
std::uint64_t LayerSampling::Generation() const noexcept { return 0; }
bool LayerSampling::Query(LayerOutput& output, std::string& error, LayerOutput* source) const {
    output = {};
    if (source) *source = {};
    return Unavailable(error);
}

LayerBakeTransaction::LayerBakeTransaction() = default;
LayerBakeTransaction::~LayerBakeTransaction() = default;
bool LayerBakeTransaction::Begin(std::string& error) { return Unavailable(error); }
bool LayerBakeTransaction::StageRemoval(const std::vector<std::uint64_t>&, std::uint32_t,
                                      std::string& error) {
    return Unavailable(error);
}
bool LayerBakeTransaction::Commit(std::string& error) { return Unavailable(error); }
void LayerBakeTransaction::Cancel() noexcept {}
bool LayerBakeTransaction::Open() const noexcept { return false; }
std::uint64_t LayerBakeTransaction::Generation() const noexcept { return 0; }
}

namespace deformed_mesh_capture {
// A session owns no device, window, hook or capture state in this profile.
struct Session::Impl {};
Session::Session(MMDApp&, bool) {}
Session::~Session() = default;
void Session::BeginFrame() {}
const std::vector<Mesh>& Session::EndFrame() {
    static const std::vector<Mesh> empty;
    return empty;
}
const std::string& Session::Error() const {
    static const std::string error = kUnavailable;
    return error;
}
MaterialScope::MaterialScope(const void*, unsigned, bool, bool)
    : oldObject_(nullptr), oldMaterial_(0), oldOpaqueCoverage_(false) {}
MaterialScope::~MaterialScope() = default;
bool OpaqueCoverage() { return false; }
bool Active() { return false; }
void SetCamera(IDirect3DDevice9*, const float[16]) {}
}

namespace alembic_export {
bool CanExport(const MMDApp*) { return false; }
bool IsExporting() { return false; }
// Menu commands are excluded by the original-profile UI. These entry points
// remain inert if a stale command invokes them after a language/menu refresh.
void ShowDialog(MMDApp*) {}
void ShowBakeDialog(MMDApp*) {}
Result Export(MMDApp&, const std::filesystem::path&, const Options&, const Progress&) {
    Result result;
    result.error = kUnavailable;
    return result;
}
}

namespace mesh_bake_cache {
bool Install(MMDApp&, const std::filesystem::path&, std::string& error,
             const std::function<bool(std::string&)>&, std::uint32_t) {
    return Unavailable(error);
}
bool Info(const void*, EntryInfo& out) {
    out = {};
    return false;
}
bool BindGeneration(const void*, std::uint64_t) { return false; }
bool Has(const void*) { return false; }
bool ActiveAt(const void*, double) { return false; }
bool RangeLimited(const void*) { return false; }
bool Sample(const void*, double, std::vector<deformed_mesh_capture::Mesh>& meshes,
            std::string& error) {
    meshes.clear();
    return Unavailable(error);
}
void Forget(const void*) {}
void Clear() {}
bool Empty() { return true; }
bool Save(MMDApp&, const std::filesystem::path&, pmm_extension::Document&, std::string& error) {
    return Unavailable(error);
}
bool Load(MMDApp&, const std::filesystem::path&, const pmm_extension::Document&, std::string& error) {
    return Unavailable(error);
}
bool PrepareTimeScale(double, TimeScalePlanPtr& plan, std::size_t& count, std::string& error) {
    plan.reset();
    count = 0;
    return Unavailable(error);
}
bool CommitTimeScale(const TimeScalePlanPtr&, std::string& error,
                     const std::function<bool(std::string&)>&) {
    return Unavailable(error);
}
}
}
