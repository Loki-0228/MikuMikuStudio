#include "mikumikustudio/mesh_evaluation.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/affine_skinning_context.hpp"
#include "mikumikustudio/bone_evaluation.hpp"
#include "mikumikustudio/mesh_definition_snapshot.hpp"
#include "mikumikustudio/morph_evaluation.hpp"
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace mikumikustudio {
namespace {
constexpr float kZeroMatrix[16] = {};
// Identical to the production CPU fallback in model_skinning.cpp. In
// particular, this is not PrecomputeBone(kZeroMatrix): its auxiliary rotation
// and rigid classification are part of the native missing-bone behavior.
constexpr affine::BoneAux kMissingBoneAux = {
    {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f},
    {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {0.5f, 0.f, 0.f, 0.f}, 0u,
};
struct BoneWork {
    bone_scale::Transform matrix{};
    affine::BoneAux aux{};
};

void Charge(std::uint64_t& total, std::uint64_t count,
            std::uint64_t stride, std::uint64_t limit) {
    if (total > limit || (stride && count > (limit - total) / stride))
        throw std::length_error("mesh evaluation budget");
    total += count * stride;
}
void RequireFinite(const float* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        if (!std::isfinite(values[i]))
            throw std::overflow_error("mesh evaluation nonfinite result");
}
affine::BoneRef Bone(const std::vector<BoneWork>& work, std::int32_t index) {
    if (index >= 0 && static_cast<std::size_t>(index) < work.size()) {
        const auto& b = work[static_cast<std::size_t>(index)];
        return {b.matrix.data(), &b.aux};
    }
    return {kZeroMatrix, &kMissingBoneAux};
}
void ValidateMorphIndices(const MorphEvaluationFrame& morph,
                          std::uint32_t vertexCount) {
    for (std::uint32_t i = 0; i < morph.PositionCount(); ++i) {
        std::uint32_t vertex = 0;
        if (!morph.PositionIndexAt(i, vertex) || vertex >= vertexCount)
            throw std::invalid_argument("mesh evaluation position index");
    }
    for (std::uint32_t family = 0; family < 5; ++family) {
        const auto count = morph.UvCount(family);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t vertex = 0;
            if (!morph.UvIndexAt(family, i, vertex) || vertex >= vertexCount)
                throw std::invalid_argument("mesh evaluation UV index");
        }
    }
}
}

struct MeshEvaluationFrame::Impl {
    std::vector<MeshEvaluationVertex> vertices;
    std::uint64_t bytes = 0;
    std::uint8_t physicsMode = 0, additionalUvCount = 0;
};
MeshEvaluationFrame::MeshEvaluationFrame() = default;
MeshEvaluationFrame::~MeshEvaluationFrame() = default;

std::unique_ptr<MeshEvaluationFrame> MeshEvaluationFrame::Evaluate(
    const MeshDefinitionSnapshot& mesh, const MorphEvaluationFrame& morph,
    const BoneEvaluationFrame& bones, const BoneTransformContext& context,
    std::uint64_t retainedLimit, std::uint64_t workspaceLimit) {
    const auto vertexCount = mesh.VertexCount();
    const auto boneCount = mesh.BoneCount();
    const auto uvCount = mesh.AdditionalUvCount();
    const bool pmx = mesh.PhysicsMode() == 2;
    if (mesh.PhysicsMode() != morph.PhysicsMode() ||
        boneCount != bones.BoneCount() || uvCount > 4 || (!pmx && uvCount) ||
        (morph.BoneCount() && morph.BoneCount() != boneCount) ||
        (pmx && morph.MaterialCount() != mesh.MaterialCount()) ||
        (!pmx && morph.MaterialCount()))
        throw std::invalid_argument("mesh evaluation incompatible inputs");

    std::uint64_t retained = 0, workspace = 0;
    Charge(retained, 1, sizeof(MeshEvaluationFrame) + sizeof(Impl), retainedLimit);
    Charge(retained, vertexCount, sizeof(MeshEvaluationVertex), retainedLimit);
    Charge(workspace, 1, sizeof(std::vector<BoneWork>), workspaceLimit);
    Charge(workspace, boneCount, sizeof(BoneWork), workspaceLimit);
    ValidateMorphIndices(morph, vertexCount);

    std::vector<BoneWork> work(boneCount);
    Charge(workspace, work.capacity() - boneCount, sizeof(BoneWork), workspaceLimit);
    for (std::uint32_t i = 0; i < boneCount; ++i) {
        BoneEvaluationPose pose{};
        if (!bones.BoneAt(i, pose))
            throw std::invalid_argument("mesh evaluation bone index");
        work[i].matrix = pose.scaled;
        RequireFinite(work[i].matrix.data(), 16);
        float quaternion[4]{};
        if (pmx) {
            context.QuaternionFromMatrix(quaternion, work[i].matrix.data());
            RequireFinite(quaternion, 4);
        }
        affine::PrecomputeBone(work[i].matrix.data(), pmx ? quaternion : nullptr,
                               work[i].aux);
        RequireFinite(work[i].aux.stretch, 9);
        RequireFinite(work[i].aux.normalMatrix, 9);
        RequireFinite(work[i].aux.rotation, 4);
    }

    auto result = std::unique_ptr<MeshEvaluationFrame>(new MeshEvaluationFrame());
    result->impl_ = std::make_unique<Impl>();
    auto& impl = *result->impl_;
    impl.physicsMode = mesh.PhysicsMode();
    impl.additionalUvCount = uvCount;
    impl.vertices.resize(vertexCount);
    Charge(retained, impl.vertices.capacity() - vertexCount,
           sizeof(MeshEvaluationVertex), retainedLimit);

    for (std::uint32_t i = 0; i < vertexCount; ++i) {
        MeshDefinitionVertex source{};
        if (!mesh.VertexAt(i, source))
            throw std::invalid_argument("mesh evaluation vertex index");
        std::array<float, 3> morphedPosition{};
        if (morph.PositionAt(i, morphedPosition)) source.position = morphedPosition;
        MeshEvaluationVertex& output = impl.vertices[i];
        output.uv = source.uv;
        output.additionalUv = source.additionalUv;
        std::array<float, 4> uv{};
        if (morph.UvAt(0, i, uv)) {
            output.uv[0] = uv[0]; output.uv[1] = uv[1];
        }
        for (std::uint32_t family = 1; family <= uvCount; ++family)
            if (morph.UvAt(family, i, uv)) output.additionalUv[family - 1] = uv;

        const affine::BoneRef refs[4] = {Bone(work, source.bones[0]),
            Bone(work, source.bones[1]), Bone(work, source.bones[2]),
            Bone(work, source.bones[3])};
        affine::Skinned skinned{};
        switch (source.binding) {
        case MeshBindingType::Bdef1:
            affine::SkinBdef1(refs[0], source.position.data(), source.normal.data(), skinned);
            break;
        case MeshBindingType::Bdef2:
            affine::SkinBdef2(refs[0], refs[1], source.weights[0],
                              source.position.data(), source.normal.data(), skinned);
            break;
        case MeshBindingType::Bdef4:
            affine::SkinBdef4(refs, source.weights.data(), source.position.data(),
                              source.normal.data(), skinned);
            break;
        case MeshBindingType::Sdef:
            affine::SkinSdefPrivate(refs[0], refs[1], source.weights[0],
                source.position.data(), source.normal.data(), source.sdefCenter.data(),
                source.sdefR0Offset.data(), source.sdefR1Offset.data(), context, skinned);
            break;
        default: throw MeshDefinitionUnsupported();
        }
        RequireFinite(skinned.position, 3);
        RequireFinite(skinned.normal, 3);
        RequireFinite(output.uv.data(), 2);
        for (unsigned family = 0; family < uvCount; ++family)
            RequireFinite(output.additionalUv[family].data(), 4);
        std::memcpy(output.position.data(), skinned.position, sizeof(skinned.position));
        std::memcpy(output.normal.data(), skinned.normal, sizeof(skinned.normal));
    }
    impl.bytes = retained;
    return result;
}

std::uint8_t MeshEvaluationFrame::PhysicsMode() const noexcept { return impl_->physicsMode; }
std::uint8_t MeshEvaluationFrame::AdditionalUvCount() const noexcept { return impl_->additionalUvCount; }
std::uint32_t MeshEvaluationFrame::VertexCount() const noexcept { return static_cast<std::uint32_t>(impl_->vertices.size()); }
std::uint64_t MeshEvaluationFrame::RetainedBytes() const noexcept { return impl_->bytes; }
bool MeshEvaluationFrame::VertexAt(std::uint32_t index, MeshEvaluationVertex& out) const noexcept {
    if (index >= impl_->vertices.size()) return false;
    out = impl_->vertices[index];
    return true;
}
}
#endif
