#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/bone_layout.hpp"
#include <array>
#include <cstdint>
#include <memory>

namespace mikumikustudio {
// Static loaded definitions, not pose matrices, motion or a solved scene frame.
// These internal records contain no pointers to the live model.
struct BoneDefinition {
    std::array<char, 20> name{}, nameEn{};
    std::int32_t parent = -1;
    std::array<float, 3> position{};
    mdl::BoneType type{};
    std::uint16_t flags = 0;
    std::int32_t layer = 0;
    std::uint8_t hasFlag = 0, hasRigidBody = 0;
    std::int32_t tailBone = -1, tailIdx = -1;
    std::array<float, 3> tailOffset{};
    float inheritRatio = 0;
    std::array<float, 3> axis{};
    std::array<float, 6> localAxes{};
    std::int32_t extParent = 0, slotIndex = -1;
    std::uint8_t twistEnable = 0;
    std::array<float, 3> ikLimitMin{}, ikLimitMax{};
    std::int32_t ikTarget = -1, ikLoop = 0;
    float ikAngle = 0;
    std::uint32_t pmxLinkCount = 0;
};
struct BoneDefinitionIkLink {
    std::int32_t bone = -1;
    std::uint8_t hasLimits = 0;
    std::array<float, 3> minimum{}, maximum{};
};
struct BoneDefinitionIk {
    std::int32_t bone = -1, target = -1;
    std::uint32_t linkCount = 0;
    std::uint16_t iterations = 0;
    float maxAngle = 0;
};
class BoneDefinitionSnapshot {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    BoneDefinitionSnapshot();
public:
    ~BoneDefinitionSnapshot();
    BoneDefinitionSnapshot(const BoneDefinitionSnapshot&) = delete;
    BoneDefinitionSnapshot& operator=(const BoneDefinitionSnapshot&) = delete;
    // Caller supplies valid loaded table extents and serializes Capture with
    // edits at a UI safe point. No source writes, side tables or pose evaluation.
    // Counts, active references, finite active constraints and parent cycles are
    // validated; IK feedback and duplicate links are not treated as DAG edges.
    // PMX source links are copied only for physicsMode==2 and the IK flag.
    // Runtime chains are independently copied, not required to equal PMX links.
    // Invalid data: invalid_argument. Retained budget: length_error. Allocation:
    // bad_alloc. The budget counts owned capacity, not peak temporary storage.
    static std::unique_ptr<BoneDefinitionSnapshot> Capture(const unsigned char* model,
        std::uint64_t maxRetainedBytes = 64ull * 1024 * 1024);
    std::uint32_t BoneCount() const noexcept;
    std::uint32_t IkCount() const noexcept;
    std::uint32_t SelectorCount() const noexcept;
    std::uint8_t PhysicsMode() const noexcept;
    std::int32_t MaxBoneLayer() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    // Concurrent, allocation-free reads remain valid after model destruction.
    // Original indices/order are retained. Invalid indices leave out untouched.
    bool BoneAt(std::uint32_t index, BoneDefinition& out) const noexcept;
    bool PmxLinkAt(std::uint32_t bone, std::uint32_t link, BoneDefinitionIkLink& out) const noexcept;
    bool IkAt(std::uint32_t index, BoneDefinitionIk& out) const noexcept;
    bool IkLinkAt(std::uint32_t chain, std::uint32_t link, std::uint32_t& out) const noexcept;
    bool SelectorBone(std::uint32_t index, std::int32_t& out) const noexcept;
};
}
#endif
