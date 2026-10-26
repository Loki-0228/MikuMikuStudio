#pragma once

#include <memory>

namespace mikumikustudio {
namespace mdl { struct ModelRecord; }

// Internal CPU math lease for independent bone workspaces. Create at a safe
// host boundary, then share the immutable context with evaluation tasks.
// The DLL reference and all eight math exports outlive every retained lease.
class BoneTransformContext final {
public:
    // Returns null when the D3DX CPU backend is unavailable or incomplete.
    // Allocation failures propagate. No graphics device is required, and no
    // global D3DX API table or host import hook is read or updated.
    static std::shared_ptr<const BoneTransformContext> Create();
    ~BoneTransformContext();
    BoneTransformContext(const BoneTransformContext&) = delete;
    BoneTransformContext& operator=(const BoneTransformContext&) = delete;

private:
    struct Impl;
    explicit BoneTransformContext(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend void BoneFrameTransformPrivate(
        mdl::ModelRecord&, unsigned char, int, unsigned char* const*, int,
        const BoneTransformContext&);
};

// Executes the native five passes for ONE layer and physics channel.
// Caller preconditions: all referenced tables, indices, IK links and external
// targets are validated and alive; selector zero exists; mutable model/IK
// storage belongs to this evaluation task. External matrices must be stable
// during the call, never borrowed from concurrently edited live models.
// Hold a context lease through the call. Different workspaces may share it.
// This does not sample tracks, apply scale, run physics, schedule layers or
// freeze scene dependencies. It does not touch scale registries or probes.
void BoneFrameTransformPrivate(
    mdl::ModelRecord& ownedModel, unsigned char afterPhysics, int layer,
    unsigned char* const* ownedModelSlots, int physicsMode,
    const BoneTransformContext& context);

}  // namespace mikumikustudio
