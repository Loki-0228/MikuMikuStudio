#include "mikumikustudio/mesh_scene_evaluation.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/mesh_definition_snapshot.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/morph_definition_snapshot.hpp"
#include "mikumikustudio/morph_evaluation.hpp"
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
}
struct MeshSceneSource::Impl {
    struct Entry {
        std::unique_ptr<MeshDefinitionSnapshot> mesh;
        std::unique_ptr<MorphDefinitionSnapshot> morph;
        std::unique_ptr<morph_curve::SamplingState> weights;
    };
    std::unique_ptr<BoneSceneSource> bones;
    std::vector<Entry> models;
    std::uint64_t bytes = 0;
};
struct MeshSceneFrame::Impl {
    std::unique_ptr<BoneSceneFrame> bones;
    std::vector<std::unique_ptr<MeshEvaluationFrame>> models;
    EvaluationTime time;
    std::uint64_t bytes = 0;
};
MeshSceneSource::MeshSceneSource() = default;
MeshSceneSource::~MeshSceneSource() = default;
MeshSceneFrame::MeshSceneFrame() = default;
MeshSceneFrame::~MeshSceneFrame() = default;

std::unique_ptr<MeshSceneSource> MeshSceneSource::Capture(
    const BoneSceneModelInput* models, std::uint32_t count, std::uint64_t limit) {
    std::uint64_t bytes = 0;
    Charge(bytes, 1, sizeof(MeshSceneSource) + sizeof(Impl), limit);
    Charge(bytes, count, sizeof(Impl::Entry), limit);
    auto result = std::unique_ptr<MeshSceneSource>(new MeshSceneSource);
    result->impl_ = std::make_unique<Impl>();
    auto& value = *result->impl_;
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
const MeshDefinitionSnapshot* MeshSceneSource::MeshAt(std::uint32_t index) const noexcept {
    return index < impl_->models.size() ? impl_->models[index].mesh.get() : nullptr;
}

std::unique_ptr<MeshSceneFrame> MeshSceneSource::EvaluateAt(
    const EvaluationTime& time, const BoneTransformContext& context,
    std::uint64_t retainedLimit, std::uint64_t workspaceLimit) const {
    std::uint64_t retained = 0;
    const auto count = ModelCount();
    Charge(retained, 1, sizeof(MeshSceneFrame) + sizeof(MeshSceneFrame::Impl), retainedLimit);
    Charge(retained, count, sizeof(std::unique_ptr<MeshEvaluationFrame>), retainedLimit);
    auto result = std::unique_ptr<MeshSceneFrame>(new MeshSceneFrame);
    result->impl_ = std::make_unique<MeshSceneFrame::Impl>();
    auto& out = *result->impl_;
    out.time = time;
    out.models.resize(count);
    Charge(retained, out.models.capacity() - count,
           sizeof(std::unique_ptr<MeshEvaluationFrame>), retainedLimit);
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
EvaluationTime MeshSceneFrame::Time() const noexcept { return impl_->time; }
const BoneSceneFrame& MeshSceneFrame::Bones() const noexcept { return *impl_->bones; }
const MeshEvaluationFrame* MeshSceneFrame::ModelFrame(std::uint32_t i) const noexcept {
    return i < impl_->models.size() ? impl_->models[i].get() : nullptr;
}
}
#endif
