#include "mikumikustudio/mesh_scene_evaluation.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/mesh_definition_snapshot.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/morph_definition_snapshot.hpp"
#include "mikumikustudio/morph_evaluation.hpp"
#include "mikumikustudio/model.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace mikumikustudio {
namespace {
void Charge(std::uint64_t& total, std::uint64_t count, std::uint64_t size,
            std::uint64_t limit) {
    if (total > limit || (size && count > (limit - total) / size))
        throw std::length_error("mesh scene budget");
    total += count * size;
}
template<std::size_t N> void Copy(std::array<float,N>& target, const float* source) {
    std::copy_n(source,N,target.begin());
}
template<std::size_t N> bool Finite(const std::array<float,N>& values) noexcept {
    for (float value:values) if (!std::isfinite(value)) return false;
    return true;
}
bool FiniteMaterial(const MeshSceneMaterial& value) noexcept {
    return Finite(value.nativeDiffuse) && Finite(value.diffuseMirrorAndAlpha) &&
        Finite(value.specular) && Finite(value.ambient) && Finite(value.edgeColor) &&
        std::isfinite(value.specularPower) && std::isfinite(value.edgeSize) &&
        Finite(value.textureAdd) && Finite(value.textureMultiply) &&
        Finite(value.sphereAdd) && Finite(value.sphereMultiply) &&
        Finite(value.toonAdd) && Finite(value.toonMultiply);
}
MeshSceneMaterial CaptureMaterial(const mdl::ModelMaterialRecord& native, std::uint8_t mode) {
    MeshSceneMaterial result{};
    result.physicsMode=mode;
    Copy(result.nativeDiffuse, native.diffuse);
    if (mode != 2) {
        for (std::size_t i=0;i<3;++i) {
            result.diffuseMirrorAndAlpha[i]=native.diffuseMirror[i];
        }
        result.diffuseMirrorAndAlpha[3]=native.diffuse[3];
        Copy(result.specular,native.specular);Copy(result.ambient,native.ambient);
        Copy(result.edgeColor,native.edgeColor);
        result.specularPower=native.specularPower;result.edgeSize=native.edgeSize;
        result.textureMultiply.fill(1.f);result.sphereMultiply.fill(1.f);result.toonMultiply.fill(1.f);
    }
    return result;
}
void ApplyMaterial(MeshSceneMaterial& result, const MorphMaterialEvaluation& value) {
    result.diffuseMirrorAndAlpha=value.properties.diffuse;
    result.specular=value.properties.specular;result.ambient=value.properties.ambient;
    result.specularPower=value.properties.specularPower;
    result.edgeColor=value.properties.edgeColor;result.edgeSize=value.properties.edgeSize;
    std::copy_n(value.additive.textureTint,4,result.textureAdd.begin());
    std::copy_n(value.multiplicative.textureTint,4,result.textureMultiply.begin());
    std::copy_n(value.additive.sphereTint,4,result.sphereAdd.begin());
    std::copy_n(value.multiplicative.sphereTint,4,result.sphereMultiply.begin());
    std::copy_n(value.additive.toonTint,4,result.toonAdd.begin());
    std::copy_n(value.multiplicative.toonTint,4,result.toonMultiply.begin());
}
}
struct MeshSceneSource::Impl {
    struct Entry {
        std::unique_ptr<MeshDefinitionSnapshot> mesh;
        std::unique_ptr<MorphDefinitionSnapshot> morph;
        std::unique_ptr<morph_curve::SamplingState> weights;
        std::vector<MeshSceneMaterial> materials;
    };
    std::unique_ptr<BoneSceneSource> bones;
    std::vector<Entry> models;
    std::uint64_t bytes = 0;
    bool materialsEnabled = false;
};
struct MeshSceneFrame::Impl {
    std::unique_ptr<BoneSceneFrame> bones;
    std::vector<std::unique_ptr<MeshEvaluationFrame>> models;
    std::vector<std::vector<MeshSceneMaterial>> materials;
    EvaluationTime time;
    std::uint64_t bytes = 0;
    bool materialsEnabled = false;
};
MeshSceneSource::MeshSceneSource() = default;
MeshSceneSource::~MeshSceneSource() = default;
MeshSceneFrame::MeshSceneFrame() = default;
MeshSceneFrame::~MeshSceneFrame() = default;

std::unique_ptr<MeshSceneSource> MeshSceneSource::Capture(
    const BoneSceneModelInput* models, std::uint32_t count, std::uint64_t limit) {
    return CaptureImpl(models,count,limit,false);
}
std::unique_ptr<MeshSceneSource> MeshSceneSource::CaptureWithMaterials(
    const BoneSceneModelInput* models, std::uint32_t count, std::uint64_t limit) {
    return CaptureImpl(models,count,limit,true);
}
std::unique_ptr<MeshSceneSource> MeshSceneSource::CaptureImpl(
    const BoneSceneModelInput* models, std::uint32_t count, std::uint64_t limit, bool materialsEnabled) {
    std::uint64_t bytes = 0;
    Charge(bytes, 1, sizeof(MeshSceneSource) + sizeof(Impl), limit);
    Charge(bytes, count, sizeof(Impl::Entry), limit);
    auto result = std::unique_ptr<MeshSceneSource>(new MeshSceneSource);
    result->impl_ = std::make_unique<Impl>();
    auto& value = *result->impl_;
    value.materialsEnabled = materialsEnabled;
    value.bones = BoneSceneSource::Capture(models, count, limit - bytes);
    Charge(bytes, 1, value.bones->RetainedBytes(), limit);
    value.models.resize(count);
    Charge(bytes, value.models.capacity() - count, sizeof(Impl::Entry), limit);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto& entry = value.models[i];
        entry.mesh = MeshDefinitionSnapshot::Capture(models[i].model, limit - bytes);
        Charge(bytes, 1, entry.mesh->RetainedBytes(), limit);
        entry.morph = MorphDefinitionSnapshot::Capture(models[i].model, limit - bytes);
        Charge(bytes, 1, entry.morph->RetainedBytes(), limit);
        entry.weights = morph_curve::SamplingState::Capture(models[i].model, limit - bytes);
        Charge(bytes, 1, entry.weights->RetainedBytes(), limit);
        if (materialsEnabled) {
            const auto& model = *mdl::Mdl(models[i].model);
            if (model.materialCount && !model.materials)
                throw std::invalid_argument("mesh scene material table");
            const auto* native=static_cast<const mdl::ModelMaterialRecord*>(model.materials);
            Charge(bytes, model.materialCount, sizeof(MeshSceneMaterial), limit);
            entry.materials.resize(model.materialCount);
            Charge(bytes, entry.materials.capacity() - model.materialCount, sizeof(MeshSceneMaterial), limit);
            for (std::uint32_t j=0;j<model.materialCount;++j)
                entry.materials[j]=CaptureMaterial(native[j],model.physicsMode);
            if (entry.mesh->MaterialCount() != entry.materials.size())
                throw std::invalid_argument("mesh scene material count");
        }
        if (entry.mesh->VertexCount() != entry.morph->VertexCount() ||
            entry.mesh->BoneCount() != entry.morph->BoneCount() ||
            entry.mesh->PhysicsMode() != entry.morph->PhysicsMode() ||
            entry.morph->MorphCount() != entry.weights->MorphCount())
            throw std::invalid_argument("mesh scene incompatible capture");
    }
    value.bytes = bytes;
    return result;
}
std::uint32_t MeshSceneSource::ModelCount() const noexcept {
    return static_cast<std::uint32_t>(impl_->models.size());
}
std::uint64_t MeshSceneSource::RetainedBytes() const noexcept { return impl_->bytes; }
bool MeshSceneSource::HasMaterials() const noexcept { return impl_->materialsEnabled; }
const MeshDefinitionSnapshot* MeshSceneSource::MeshAt(std::uint32_t index) const noexcept {
    return index < impl_->models.size() ? impl_->models[index].mesh.get() : nullptr;
}
bool MeshSceneSource::ModelAt(std::uint32_t index, BoneSceneModelInfo& out) const noexcept {
    return impl_->bones->ModelAt(index, out);
}

std::unique_ptr<MeshSceneFrame> MeshSceneSource::EvaluateAt(
    const EvaluationTime& time, const BoneTransformContext& context,
    std::uint64_t retainedLimit, std::uint64_t workspaceLimit) const {
    std::uint64_t retained = 0;
    const auto count = ModelCount();
    Charge(retained, 1, sizeof(MeshSceneFrame) + sizeof(MeshSceneFrame::Impl), retainedLimit);
    Charge(retained, count, sizeof(std::unique_ptr<MeshEvaluationFrame>), retainedLimit);
    if (impl_->materialsEnabled)
        Charge(retained, count, sizeof(std::vector<MeshSceneMaterial>), retainedLimit);
    auto result = std::unique_ptr<MeshSceneFrame>(new MeshSceneFrame);
    result->impl_ = std::make_unique<MeshSceneFrame::Impl>();
    auto& out = *result->impl_;
    out.time = time;
    out.materialsEnabled = impl_->materialsEnabled;
    out.models.resize(count);
    if (out.materialsEnabled) out.materials.resize(count);
    Charge(retained, out.models.capacity() - count,
           sizeof(std::unique_ptr<MeshEvaluationFrame>), retainedLimit);
    if (out.materialsEnabled)
        Charge(retained, out.materials.capacity() - count,
               sizeof(std::vector<MeshSceneMaterial>), retainedLimit);
    out.bones = impl_->bones->EvaluateAt(time, context, retainedLimit - retained, workspaceLimit);
    Charge(retained, 1, out.bones->RetainedBytes(), retainedLimit);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& entry = impl_->models[i];
        const auto weightCount = entry.weights->MorphCount();
        std::uint64_t workspace = 0;
        Charge(workspace, 1, sizeof(std::vector<float>), workspaceLimit);
        Charge(workspace, weightCount, sizeof(float), workspaceLimit);
        std::vector<float> weights(weightCount);
        Charge(workspace, weights.capacity() - weightCount, sizeof(float), workspaceLimit);
        for (std::uint32_t j = 0; j < weightCount; ++j)
            if (!entry.weights->Sample(static_cast<int>(j), time.Frame(), weights[j]))
                throw std::overflow_error("mesh scene morph weight");
        auto morph = MorphEvaluationFrame::Evaluate(*entry.morph,
            weights.empty() ? nullptr : weights.data(), weightCount, workspaceLimit - workspace);
        Charge(workspace, 1, morph->RetainedBytes(), workspaceLimit);
        if (out.materialsEnabled) {
            const auto materialCount=entry.materials.size();
            if (entry.mesh->PhysicsMode()==2 ? morph->MaterialCount()!=materialCount : morph->MaterialCount()!=0)
                throw std::invalid_argument("mesh scene material count");
            Charge(retained, materialCount, sizeof(MeshSceneMaterial), retainedLimit);
            out.materials[i]=entry.materials;
            Charge(retained, out.materials[i].capacity()-materialCount, sizeof(MeshSceneMaterial), retainedLimit);
            if (entry.mesh->PhysicsMode()==2) {
                for (std::uint32_t j=0;j<materialCount;++j) {
                    MorphMaterialEvaluation material{};
                    if (!morph->MaterialAt(j,material)) throw std::invalid_argument("mesh scene material result");
                    ApplyMaterial(out.materials[i][j],material);
                }
            }
        }
        const auto* bones = out.bones->ModelFrame(i);
        if (!bones) throw std::invalid_argument("mesh scene missing bone frame");
        out.models[i] = MeshEvaluationFrame::Evaluate(*entry.mesh, *morph, *bones,
            context, retainedLimit - retained, workspaceLimit - workspace);
        Charge(retained, 1, out.models[i]->RetainedBytes(), retainedLimit);
    }
    out.bytes = retained;
    return result;
}
std::uint32_t MeshSceneFrame::ModelCount() const noexcept {
    return static_cast<std::uint32_t>(impl_->models.size());
}
std::uint64_t MeshSceneFrame::RetainedBytes() const noexcept { return impl_->bytes; }
bool MeshSceneFrame::HasMaterials() const noexcept { return impl_->materialsEnabled; }
EvaluationTime MeshSceneFrame::Time() const noexcept { return impl_->time; }
const BoneSceneFrame& MeshSceneFrame::Bones() const noexcept { return *impl_->bones; }
const MeshEvaluationFrame* MeshSceneFrame::ModelFrame(std::uint32_t i) const noexcept {
    return i < impl_->models.size() ? impl_->models[i].get() : nullptr;
}
std::uint32_t MeshSceneFrame::MaterialCount(std::uint32_t model) const noexcept {
    return model < impl_->materials.size() ? static_cast<std::uint32_t>(impl_->materials[model].size()) : 0;
}
bool MeshSceneFrame::MaterialAt(std::uint32_t model, std::uint32_t material, MeshSceneMaterial& out) const noexcept {
    if (model >= impl_->materials.size() || material >= impl_->materials[model].size()) return false;
    const auto& value=impl_->materials[model][material];
    if (!FiniteMaterial(value)) return false;
    out=value;return true;
}
}
#endif
