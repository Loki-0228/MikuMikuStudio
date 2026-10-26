#include "mikumikustudio/bone_definition_snapshot.hpp"
#if !MMD_ORIGINAL_PROFILE

#include "mikumikustudio/model.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace mikumikustudio {
namespace {

using PmxLinkList = std::vector<BoneDefinitionIkLink>;
using ChainLinkList = std::vector<std::uint32_t>;

// The budget covers the two owning objects, every vector allocation and every
// nested vector object. All additions and multiplications are checked against
// the limit instead of wrapping.
void Charge(std::uint64_t& bytes, std::uint64_t count, std::uint64_t stride,
            std::uint64_t limit) {
    if (bytes > limit || (stride != 0 && count > (limit - bytes) / stride))
        throw std::length_error("bone definition snapshot budget");
    bytes += count * stride;
}

// Counts are bounded by INT32_MAX at the top level and by the native table
// field widths below it, but retain an explicit overflow guard for the sums.
std::uint64_t AddCount(std::uint64_t total, std::uint64_t count) {
    if (count > (std::numeric_limits<std::uint64_t>::max)() - total)
        throw std::length_error("bone definition snapshot count");
    return total + count;
}

bool Finite(const float* values, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i)
        if (!std::isfinite(values[i])) return false;
    return true;
}

bool LimitsOrdered(const float* minimum, const float* maximum) noexcept {
    for (std::size_t i = 0; i < 3; ++i)
        if (minimum[i] > maximum[i]) return false;
    return true;
}

bool ValidBoneIndex(std::int64_t index, std::uint64_t boneCount) noexcept {
    return index >= 0 && static_cast<std::uint64_t>(index) < boneCount;
}

}  // namespace

struct BoneDefinitionSnapshot::Impl {
    std::vector<BoneDefinition> bones;
    std::vector<PmxLinkList> pmxLinks;
    std::vector<BoneDefinitionIk> chains;
    std::vector<ChainLinkList> chainLinks;
    std::vector<std::int32_t> selectors;
    std::uint8_t physicsMode = 0;
    std::int32_t maxBoneLayer = 0;
    std::uint64_t bytes = 0;
};

BoneDefinitionSnapshot::BoneDefinitionSnapshot() = default;
BoneDefinitionSnapshot::~BoneDefinitionSnapshot() = default;

std::unique_ptr<BoneDefinitionSnapshot> BoneDefinitionSnapshot::Capture(
    const unsigned char* source, std::uint64_t limit) {
    if (!source) throw std::invalid_argument("bone definition snapshot source");
    const auto& model = *mdl::Mdl(source);

    const std::uint64_t boneCount = model.boneCount;
    const std::uint64_t ikCount = model.ikChainCount;
    const std::uint64_t selectorCount = model.boneOrderCount;
    const std::uint64_t int32Max = static_cast<std::uint64_t>(
        (std::numeric_limits<std::int32_t>::max)());
    if (boneCount > int32Max || ikCount > int32Max ||
        selectorCount > int32Max)
        throw std::invalid_argument("bone definition snapshot counts");
    if ((boneCount != 0 && !model.boneTable) ||
        (ikCount != 0 && !model.ikChains) ||
        (selectorCount != 0 && !model.boneOrderTable))
        throw std::invalid_argument("bone definition snapshot tables");
    if (boneCount != 0 && selectorCount == 0)
        throw std::invalid_argument("bone definition snapshot global selector");

    const mdl::BoneRecord* const bones = model.boneTable;
    const mdl::IkChain* const chains = model.ikChains;
    const auto* const selectors =
        static_cast<const mdl::BoneOrderEntry*>(model.boneOrderTable);
    const bool copyPmxLinks = model.physicsMode == 2;
    const std::uint64_t base = sizeof(BoneDefinitionSnapshot) + sizeof(Impl);

    // Reject impossible top-level counts or budgets before walking nested
    // tables or allocating any owned storage.
    {
        std::uint64_t required = 0;
        Charge(required, 1, base, limit);
        Charge(required, boneCount, sizeof(BoneDefinition), limit);
        Charge(required, boneCount, sizeof(PmxLinkList), limit);
        Charge(required, ikCount, sizeof(BoneDefinitionIk), limit);
        Charge(required, ikCount, sizeof(ChainLinkList), limit);
        Charge(required, selectorCount, sizeof(std::int32_t), limit);
    }

    if (selectorCount != 0) {
        // Slot zero is the whole-model external parent and carries no bone.
        if (selectors[0].boneIndex != -1)
            throw std::invalid_argument("bone definition snapshot selector root");
        for (std::uint64_t i = 1; i < selectorCount; ++i)
            if (!ValidBoneIndex(selectors[i].boneIndex, boneCount))
                throw std::invalid_argument(
                    "bone definition snapshot selector bone");
    }

    std::uint64_t pmxLinkTotal = 0;
    for (std::uint64_t index = 0; index < boneCount; ++index) {
        const mdl::BoneRecord& bone = bones[static_cast<std::size_t>(index)];

        if (bone.parent != -1 &&
            !ValidBoneIndex(bone.parent, boneCount))
            throw std::invalid_argument("bone definition snapshot parent");
        if (!Finite(bone.position, 3))
            throw std::invalid_argument("bone definition snapshot position");
        if (static_cast<unsigned>(bone.type) > 9)
            throw std::invalid_argument("bone definition snapshot type");

        const bool tailIsBone =
            (bone.flags & mdl::kBoneFlagTailIsBone) != 0;
        if (bone.type == mdl::BoneType::CoRotate) {
            // CoRotate reads a source bone regardless of the tail flag.
            if (!ValidBoneIndex(bone.tailBone, boneCount))
                throw std::invalid_argument(
                    "bone definition snapshot co-rotate tail");
        } else if (tailIsBone) {
            if (bone.tailBone != -1 &&
                !ValidBoneIndex(bone.tailBone, boneCount))
                throw std::invalid_argument(
                    "bone definition snapshot tail bone");
        } else if (copyPmxLinks && !Finite(bone.tailOffset, 3)) {
            throw std::invalid_argument(
                "bone definition snapshot tail offset");
        }

        if ((bone.type == mdl::BoneType::RotateGrant ||
             (bone.flags & mdl::kBoneFlagInheritMask) != 0) &&
            !ValidBoneIndex(bone.tailIdx, boneCount))
            throw std::invalid_argument(
                "bone definition snapshot inheritance source");
        if ((bone.flags & mdl::kBoneFlagInheritMask) != 0 &&
            !std::isfinite(bone.inheritRatio))
            throw std::invalid_argument(
                "bone definition snapshot inheritance ratio");
        if ((bone.type == mdl::BoneType::FixedAxis ||
             (bone.flags & mdl::kBoneFlagFixedAxis) != 0) &&
            !Finite(bone.axis, 3))
            throw std::invalid_argument("bone definition snapshot fixed axis");
        if ((bone.flags & mdl::kBoneFlagLocalAxes) != 0 &&
            !Finite(bone.localAxes, 6))
            throw std::invalid_argument("bone definition snapshot local axes");
        if (bone.twistEnable != 0 &&
            (!Finite(bone.ikLimitMin, 3) ||
             !Finite(bone.ikLimitMax, 3) ||
             !LimitsOrdered(bone.ikLimitMin, bone.ikLimitMax)))
            throw std::invalid_argument(
                "bone definition snapshot twist limits");

        if (bone.slotIndex < -1)
            throw std::invalid_argument(
                "bone definition snapshot selector slot");
        if (bone.slotIndex >= 0) {
            const std::uint64_t slot =
                static_cast<std::uint64_t>(bone.slotIndex);
            if (slot >= selectorCount ||
                selectors[slot].boneIndex !=
                    static_cast<std::int32_t>(index))
                throw std::invalid_argument(
                    "bone definition snapshot selector back reference");
        }

        if (copyPmxLinks && (bone.flags & mdl::kBoneFlagIk) != 0) {
            if (bone.ikLinkCount < 0)
                throw std::invalid_argument(
                    "bone definition snapshot PMX link count");
            const std::uint64_t linkCount =
                static_cast<std::uint64_t>(bone.ikLinkCount);
            if (linkCount != 0 && !bone.ikLinks)
                throw std::invalid_argument(
                    "bone definition snapshot PMX link table");
            if (!ValidBoneIndex(bone.ikTarget, boneCount))
                throw std::invalid_argument(
                    "bone definition snapshot PMX target");
            if (bone.ikLoop < 0)
                throw std::invalid_argument(
                    "bone definition snapshot PMX loop");
            if (!std::isfinite(bone.ikAngle))
                throw std::invalid_argument(
                    "bone definition snapshot PMX angle");
            pmxLinkTotal = AddCount(pmxLinkTotal, linkCount);
        }
    }

    // Parent-only cycle detection. Each node is marked at most once, and the
    // iterative walk avoids recursion and any parent<self ordering assumption.
    if (boneCount != 0) {
        std::vector<std::uint8_t> state(
            static_cast<std::size_t>(boneCount), 0);
        std::vector<std::uint32_t> path;
        for (std::uint64_t index = 0; index < boneCount; ++index) {
            if (state[static_cast<std::size_t>(index)] != 0) continue;
            path.clear();
            std::int32_t cursor = static_cast<std::int32_t>(index);
            while (cursor != -1) {
                const auto node = static_cast<std::size_t>(cursor);
                if (state[node] == 2) break;
                if (state[node] == 1)
                    throw std::invalid_argument(
                        "bone definition snapshot parent cycle");
                state[node] = 1;
                path.push_back(static_cast<std::uint32_t>(cursor));
                cursor = bones[node].parent;
            }
            for (const std::uint32_t node : path) state[node] = 2;
        }
    }

    std::uint64_t chainLinkTotal = 0;
    for (std::uint64_t index = 0; index < ikCount; ++index) {
        const mdl::IkChain& chain = chains[static_cast<std::size_t>(index)];
        if (!ValidBoneIndex(chain.boneIndex, boneCount) ||
            !ValidBoneIndex(chain.targetBone, boneCount))
            throw std::invalid_argument("bone definition snapshot IK bone");
        if (!std::isfinite(chain.maxAngle))
            throw std::invalid_argument("bone definition snapshot IK angle");
        const std::uint64_t linkCount = chain.linkCount;
        if (linkCount != 0 && !chain.links)
            throw std::invalid_argument("bone definition snapshot IK links");
        chainLinkTotal = AddCount(chainLinkTotal, linkCount);
    }

    // Refuse an impossible retained budget before any owned sequence is
    // allocated, including the nested link tables counted above.
    {
        std::uint64_t required = 0;
        Charge(required, 1, base, limit);
        Charge(required, boneCount, sizeof(BoneDefinition), limit);
        Charge(required, boneCount, sizeof(PmxLinkList), limit);
        Charge(required, pmxLinkTotal, sizeof(BoneDefinitionIkLink), limit);
        Charge(required, ikCount, sizeof(BoneDefinitionIk), limit);
        Charge(required, ikCount, sizeof(ChainLinkList), limit);
        Charge(required, chainLinkTotal, sizeof(std::uint32_t), limit);
        Charge(required, selectorCount, sizeof(std::int32_t), limit);
    }

    // Validate the nested index and limit fields after the budget guard so a
    // huge count cannot force a large allocation before it is rejected.
    if (copyPmxLinks) {
        for (std::uint64_t index = 0; index < boneCount; ++index) {
            const mdl::BoneRecord& bone =
                bones[static_cast<std::size_t>(index)];
            if ((bone.flags & mdl::kBoneFlagIk) == 0) continue;
            const std::uint32_t linkCount =
                static_cast<std::uint32_t>(bone.ikLinkCount);
            for (std::uint32_t link = 0; link < linkCount; ++link) {
                const mdl::PmxIkLinkRecord& sourceLink = bone.ikLinks[link];
                if (!ValidBoneIndex(sourceLink.boneIndex, boneCount))
                    throw std::invalid_argument(
                        "bone definition snapshot PMX link bone");
                if (sourceLink.hasLimits == 1 &&
                    (!Finite(sourceLink.minimum, 3) ||
                     !Finite(sourceLink.maximum, 3) ||
                     !LimitsOrdered(sourceLink.minimum,
                                    sourceLink.maximum)))
                    throw std::invalid_argument(
                        "bone definition snapshot PMX link limits");
            }
        }
    }
    for (std::uint64_t index = 0; index < ikCount; ++index) {
        const mdl::IkChain& chain = chains[static_cast<std::size_t>(index)];
        for (std::uint32_t link = 0; link < chain.linkCount; ++link)
            if (!ValidBoneIndex(chain.links[link], boneCount))
                throw std::invalid_argument(
                    "bone definition snapshot IK link bone");
    }

    auto result = std::unique_ptr<BoneDefinitionSnapshot>(
        new BoneDefinitionSnapshot);
    result->impl_ = std::make_unique<Impl>();
    auto& value = *result->impl_;
    value.physicsMode = model.physicsMode;
    value.maxBoneLayer = model.maxBoneLayer;

    std::uint64_t retained = 0;
    Charge(retained, 1, base, limit);
    value.bones.resize(static_cast<std::size_t>(boneCount));
    Charge(retained, value.bones.capacity(), sizeof(BoneDefinition), limit);
    value.pmxLinks.resize(static_cast<std::size_t>(boneCount));
    Charge(retained, value.pmxLinks.capacity(), sizeof(PmxLinkList), limit);
    value.chains.resize(static_cast<std::size_t>(ikCount));
    Charge(retained, value.chains.capacity(), sizeof(BoneDefinitionIk), limit);
    value.chainLinks.resize(static_cast<std::size_t>(ikCount));
    Charge(retained, value.chainLinks.capacity(), sizeof(ChainLinkList), limit);
    value.selectors.resize(static_cast<std::size_t>(selectorCount));
    Charge(retained, value.selectors.capacity(), sizeof(std::int32_t), limit);

    for (std::uint64_t index = 0; index < boneCount; ++index) {
        const mdl::BoneRecord& sourceBone =
            bones[static_cast<std::size_t>(index)];
        BoneDefinition& bone =
            value.bones[static_cast<std::size_t>(index)];
        std::memcpy(bone.name.data(), sourceBone.name, sizeof(bone.name));
        std::memcpy(bone.nameEn.data(), sourceBone.nameEn,
                    sizeof(bone.nameEn));
        bone.parent = sourceBone.parent;
        std::memcpy(bone.position.data(), sourceBone.position,
                    sizeof(bone.position));
        bone.type = sourceBone.type;
        bone.flags = sourceBone.flags;
        bone.layer = sourceBone.layer;
        bone.hasFlag = sourceBone.hasFlag;
        bone.hasRigidBody = sourceBone.hasRigidBody;
        bone.tailBone = sourceBone.tailBone;
        bone.tailIdx = sourceBone.tailIdx;
        std::memcpy(bone.tailOffset.data(), sourceBone.tailOffset,
                    sizeof(bone.tailOffset));
        bone.inheritRatio = sourceBone.inheritRatio;
        std::memcpy(bone.axis.data(), sourceBone.axis,
                    sizeof(bone.axis));
        std::memcpy(bone.localAxes.data(), sourceBone.localAxes,
                    sizeof(bone.localAxes));
        bone.extParent = sourceBone.extParent;
        bone.slotIndex = sourceBone.slotIndex;
        bone.twistEnable = sourceBone.twistEnable;
        std::memcpy(bone.ikLimitMin.data(), sourceBone.ikLimitMin,
                    sizeof(bone.ikLimitMin));
        std::memcpy(bone.ikLimitMax.data(), sourceBone.ikLimitMax,
                    sizeof(bone.ikLimitMax));
        bone.ikTarget = sourceBone.ikTarget;
        bone.ikLoop = sourceBone.ikLoop;
        bone.ikAngle = sourceBone.ikAngle;
        bone.pmxLinkCount = 0;

        if (copyPmxLinks && (sourceBone.flags & mdl::kBoneFlagIk) != 0) {
            const std::uint64_t linkCount =
                static_cast<std::uint64_t>(sourceBone.ikLinkCount);
            bone.pmxLinkCount = static_cast<std::uint32_t>(linkCount);
            PmxLinkList& links =
                value.pmxLinks[static_cast<std::size_t>(index)];
            if (linkCount != 0) {
                if (linkCount >
                    (limit - retained) / sizeof(BoneDefinitionIkLink))
                    throw std::length_error(
                        "bone definition snapshot PMX links budget");
                links.resize(static_cast<std::size_t>(linkCount));
            }
            Charge(retained, links.capacity(),
                   sizeof(BoneDefinitionIkLink), limit);
            for (std::uint32_t link = 0; link < linkCount; ++link) {
                const mdl::PmxIkLinkRecord& sourceLink =
                    sourceBone.ikLinks[link];
                BoneDefinitionIkLink& target = links[link];
                target.bone = sourceLink.boneIndex;
                target.hasLimits = sourceLink.hasLimits;
                std::memcpy(target.minimum.data(), sourceLink.minimum,
                            sizeof(target.minimum));
                std::memcpy(target.maximum.data(), sourceLink.maximum,
                            sizeof(target.maximum));
            }
        }
    }

    for (std::uint64_t index = 0; index < ikCount; ++index) {
        const mdl::IkChain& sourceChain =
            chains[static_cast<std::size_t>(index)];
        BoneDefinitionIk& chain =
            value.chains[static_cast<std::size_t>(index)];
        chain.bone = sourceChain.boneIndex;
        chain.target = sourceChain.targetBone;
        chain.linkCount = sourceChain.linkCount;
        chain.iterations = sourceChain.iterations;
        chain.maxAngle = sourceChain.maxAngle;

        ChainLinkList& links =
            value.chainLinks[static_cast<std::size_t>(index)];
        if (sourceChain.linkCount != 0) {
            if (sourceChain.linkCount >
                (limit - retained) / sizeof(std::uint32_t))
                throw std::length_error(
                    "bone definition snapshot IK links budget");
            links.resize(sourceChain.linkCount);
        }
        Charge(retained, links.capacity(), sizeof(std::uint32_t), limit);
        for (std::uint32_t link = 0; link < sourceChain.linkCount; ++link)
            links[link] = sourceChain.links[link];
    }

    for (std::uint64_t index = 0; index < selectorCount; ++index)
        value.selectors[static_cast<std::size_t>(index)] =
            selectors[index].boneIndex;

    value.bytes = retained;
    return result;
}

std::uint32_t BoneDefinitionSnapshot::BoneCount() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(impl_->bones.size()) : 0;
}

std::uint32_t BoneDefinitionSnapshot::IkCount() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(impl_->chains.size()) : 0;
}

std::uint32_t BoneDefinitionSnapshot::SelectorCount() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(impl_->selectors.size()) : 0;
}

std::uint8_t BoneDefinitionSnapshot::PhysicsMode() const noexcept {
    return impl_ ? impl_->physicsMode : 0;
}

std::int32_t BoneDefinitionSnapshot::MaxBoneLayer() const noexcept {
    return impl_ ? impl_->maxBoneLayer : 0;
}

std::uint64_t BoneDefinitionSnapshot::RetainedBytes() const noexcept {
    return impl_ ? impl_->bytes : sizeof(*this);
}

bool BoneDefinitionSnapshot::BoneAt(std::uint32_t index,
                                    BoneDefinition& out) const noexcept {
    if (!impl_ || index >= impl_->bones.size()) return false;
    out = impl_->bones[index];
    return true;
}

bool BoneDefinitionSnapshot::PmxLinkAt(
    std::uint32_t bone, std::uint32_t link,
    BoneDefinitionIkLink& out) const noexcept {
    if (!impl_ || bone >= impl_->pmxLinks.size()) return false;
    const PmxLinkList& links = impl_->pmxLinks[bone];
    if (link >= links.size()) return false;
    out = links[link];
    return true;
}

bool BoneDefinitionSnapshot::IkAt(std::uint32_t index,
                                  BoneDefinitionIk& out) const noexcept {
    if (!impl_ || index >= impl_->chains.size()) return false;
    out = impl_->chains[index];
    return true;
}

bool BoneDefinitionSnapshot::IkLinkAt(
    std::uint32_t chain, std::uint32_t link,
    std::uint32_t& out) const noexcept {
    if (!impl_ || chain >= impl_->chainLinks.size()) return false;
    const ChainLinkList& links = impl_->chainLinks[chain];
    if (link >= links.size()) return false;
    out = links[link];
    return true;
}

bool BoneDefinitionSnapshot::SelectorBone(std::uint32_t index,
                                          std::int32_t& out) const noexcept {
    if (!impl_ || index >= impl_->selectors.size()) return false;
    out = impl_->selectors[index];
    return true;
}

}  // namespace mikumikustudio
#endif
