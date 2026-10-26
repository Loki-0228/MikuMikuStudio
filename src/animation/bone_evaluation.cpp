#include "mikumikustudio/bone_evaluation.hpp"
#if !MMD_ORIGINAL_PROFILE

#include "mikumikustudio/bone_definition_snapshot.hpp"
#include "mikumikustudio/bone_motion_snapshot.hpp"
#include "mikumikustudio/bone_transform_context.hpp"
#include "mikumikustudio/display_motion_snapshot.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/morph_definition_snapshot.hpp"
#include "mikumikustudio/morph_evaluation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mikumikustudio {
namespace {

// Retained-storage charge. Every addition and multiplication is checked
// against the remaining limit instead of wrapping; crossing it is length_error.
void Charge(std::uint64_t& bytes, std::uint64_t count, std::uint64_t stride,
            std::uint64_t limit) {
    if (bytes > limit || (stride != 0 && count > (limit - bytes) / stride))
        throw std::length_error("bone evaluation budget");
    bytes += count * stride;
}

bool Finite(const float* values, std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index)
        if (!std::isfinite(values[index])) return false;
    return true;
}

// Element-wise numeric equality; +0 and -0 compare equal, and nonfinite input
// has already been rejected by the caller.
bool SameValues(const float* left, const float* right,
                std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index)
        if (left[index] != right[index]) return false;
    return true;
}

void RequireFinite(const float* values, std::size_t count,
                   const char* message) {
    for (std::size_t index = 0; index < count; ++index)
        if (!std::isfinite(values[index])) throw std::overflow_error(message);
}

// Working-storage budget. The accounting deliberately charges owned capacity,
// not peak heap use: nested solver, scale and morph transients are bounded by
// their own modules. An allocation is only attempted after the matching charge
// has succeeded, so a too-small workspace fails as length_error before asking
// the allocator for another block.
class Workspace final {
public:
    explicit Workspace(std::uint64_t limit) : limit_(limit) {}

    void Charge(std::uint64_t count, std::uint64_t stride) {
        if (stride != 0 && count > (limit_ - used_) / stride)
            throw std::length_error("bone evaluation workspace budget");
        used_ += count * stride;
    }

    std::uint64_t Remaining() const noexcept { return limit_ - used_; }

private:
    std::uint64_t limit_ = 0;
    std::uint64_t used_ = 0;
};

// Charges the requested element count before growing, then supplements the
// capacity the allocator actually reserved. The vector starts empty for every
// call site, so capacity growth is the only allocation that has to be paid.
template <typename T>
void ResizeCharged(std::vector<T>& values, std::size_t count,
                   Workspace& workspace) {
    workspace.Charge(count, sizeof(T));
    values.resize(count);
    if (values.capacity() > count)
        workspace.Charge(values.capacity() - count, sizeof(T));
}

// Same contract for incremental growth: charge a slot before the push that
// needs it, then charge the extra capacity the growth reserved.
template <typename T>
void PushCharged(std::vector<T>& values, const T& value, Workspace& workspace) {
    if (values.size() == values.capacity()) workspace.Charge(1, sizeof(T));
    const std::size_t oldCapacity = values.capacity();
    values.push_back(value);
    if (values.capacity() > oldCapacity)
        workspace.Charge(values.capacity() - oldCapacity - 1, sizeof(T));
}

// One std::map node is charged with a conservative four-pointer tree
// allowance, the same rule the other snapshot modules use.
std::uint64_t ScaleNodeBytes() {
    return sizeof(bone_scale::Values::value_type) + 4 * sizeof(void*);
}

}  // namespace

struct BoneEvaluationFrame::Impl {
    std::vector<BoneEvaluationPose> poses;
    double frame = 0.0;
    std::uint8_t visible = 0;
    std::uint64_t bytes = 0;
};

BoneEvaluationFrame::BoneEvaluationFrame() = default;
BoneEvaluationFrame::~BoneEvaluationFrame() = default;

std::uint32_t BoneEvaluationFrame::BoneCount() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(impl_->poses.size()) : 0;
}

double BoneEvaluationFrame::Frame() const noexcept {
    return impl_ ? impl_->frame : 0.0;
}

std::uint8_t BoneEvaluationFrame::Visibility() const noexcept {
    return impl_ ? impl_->visible : 0;
}

std::uint64_t BoneEvaluationFrame::RetainedBytes() const noexcept {
    return impl_ ? impl_->bytes : sizeof(*this);
}

bool BoneEvaluationFrame::BoneAt(std::uint32_t bone,
                                 BoneEvaluationPose& out) const noexcept {
    if (!impl_ || bone >= impl_->poses.size()) return false;
    out = impl_->poses[bone];
    return true;
}

struct BoneEvaluationSource::Impl {
    std::unique_ptr<BoneDefinitionSnapshot> definitions;
    std::unique_ptr<BoneMotionSnapshot> motion;
    std::unique_ptr<DisplayMotionSnapshot> display;
    std::unique_ptr<MorphDefinitionSnapshot> morphDefinitions;
    std::unique_ptr<morph_curve::SamplingState> morphWeights;
    std::uint64_t bytes = 0;
};

BoneEvaluationSource::BoneEvaluationSource() = default;
BoneEvaluationSource::~BoneEvaluationSource() = default;

std::unique_ptr<BoneEvaluationSource> BoneEvaluationSource::Capture(
    const unsigned char* model, std::uint64_t limit) {
    if (model == nullptr)
        throw std::invalid_argument("bone evaluation source model");

    // The five frozen components are captured from the same source at one
    // caller-serialized UI safe point. The owning objects are paid for before
    // they are allocated; each component then gets the remaining budget and
    // its complete retained size is charged afterwards.
    std::uint64_t retained = 0;
    Charge(retained, 1, sizeof(BoneEvaluationSource) + sizeof(Impl), limit);

    auto result =
        std::unique_ptr<BoneEvaluationSource>(new BoneEvaluationSource);
    result->impl_ = std::make_unique<Impl>();
    Impl& value = *result->impl_;

    value.definitions =
        BoneDefinitionSnapshot::Capture(model, limit - retained);
    Charge(retained, 1, value.definitions->RetainedBytes(), limit);

    value.motion = BoneMotionSnapshot::Capture(model, limit - retained);
    Charge(retained, 1, value.motion->RetainedBytes(), limit);

    value.display = DisplayMotionSnapshot::Capture(model, limit - retained);
    Charge(retained, 1, value.display->RetainedBytes(), limit);

    value.morphDefinitions =
        MorphDefinitionSnapshot::Capture(model, limit - retained);
    Charge(retained, 1, value.morphDefinitions->RetainedBytes(), limit);

    value.morphWeights =
        morph_curve::SamplingState::Capture(model, limit - retained);
    Charge(retained, 1, value.morphWeights->RetainedBytes(), limit);

    // The components must describe the same skeleton, morph table, physics
    // dialect and native mapping tables. A mismatch cannot be repaired here.
    const BoneDefinitionSnapshot& definitions = *value.definitions;
    const BoneMotionSnapshot& motion = *value.motion;
    const DisplayMotionSnapshot& display = *value.display;
    const MorphDefinitionSnapshot& morphDefinitions = *value.morphDefinitions;
    const morph_curve::SamplingState& morphWeights = *value.morphWeights;

    const std::uint32_t boneCount = definitions.BoneCount();
    const std::uint32_t ikCount = definitions.IkCount();
    const std::uint32_t selectorCount = definitions.SelectorCount();
    if (motion.BoneCount() != boneCount ||
        morphDefinitions.BoneCount() != boneCount)
        throw std::invalid_argument("bone evaluation bone count mismatch");
    if (morphDefinitions.PhysicsMode() != definitions.PhysicsMode())
        throw std::invalid_argument("bone evaluation physics mode mismatch");
    if (display.IkCount() != ikCount ||
        display.SelectorCount() != selectorCount)
        throw std::invalid_argument("bone evaluation mapping count mismatch");
    if (morphWeights.MorphCount() != morphDefinitions.MorphCount())
        throw std::invalid_argument("bone evaluation morph count mismatch");

    for (std::uint32_t index = 0; index < ikCount; ++index) {
        BoneDefinitionIk chain{};
        std::int32_t mapped = 0;
        if (!definitions.IkAt(index, chain) ||
            !display.IkBone(index, mapped) || chain.bone != mapped)
            throw std::invalid_argument("bone evaluation IK mapping mismatch");
    }
    for (std::uint32_t index = 0; index < selectorCount; ++index) {
        std::int32_t definitionBone = 0, displayBone = 0;
        if (!definitions.SelectorBone(index, definitionBone) ||
            !display.SelectorBone(index, displayBone) ||
            definitionBone != displayBone)
            throw std::invalid_argument(
                "bone evaluation selector mapping mismatch");
    }

    value.bytes = retained;
    return result;
}

std::uint32_t BoneEvaluationSource::BoneCount() const noexcept {
    return impl_ && impl_->definitions ? impl_->definitions->BoneCount() : 0;
}

std::uint32_t BoneEvaluationSource::SelectorCount() const noexcept {
    return impl_ && impl_->definitions ? impl_->definitions->SelectorCount()
                                       : 0;
}

std::uint64_t BoneEvaluationSource::RetainedBytes() const noexcept {
    return impl_ ? impl_->bytes : sizeof(*this);
}

DisplayMotionStatus BoneEvaluationSource::SampleSelector(
    int selector, double frame, DisplaySelectorSample& out) const noexcept {
    if (!impl_ || !impl_->display)
        return DisplayMotionStatus::InvalidIndex;
    return impl_->display->SampleSelector(selector, frame, out);
}

std::unique_ptr<BoneEvaluationFrame> BoneEvaluationSource::Evaluate(
    double frame, const BoneTransformContext& context,
    const BoneEvaluationDependency* dependencies, std::uint32_t dependencyCount,
    std::uint64_t maxRetainedBytes,
    std::uint64_t maxWorkspaceBytes) const {
    if (!impl_ || !impl_->definitions || !impl_->motion || !impl_->display ||
        !impl_->morphDefinitions || !impl_->morphWeights)
        throw std::invalid_argument("bone evaluation source is empty");

    const BoneDefinitionSnapshot& definitions = *impl_->definitions;
    const BoneMotionSnapshot& motion = *impl_->motion;
    const DisplayMotionSnapshot& display = *impl_->display;
    const MorphDefinitionSnapshot& morphDefinitions = *impl_->morphDefinitions;
    const morph_curve::SamplingState& morphWeights = *impl_->morphWeights;

    if (!std::isfinite(frame) || frame < 0.0 ||
        frame > static_cast<double>(frame_limits::maximum))
        throw std::invalid_argument("bone evaluation frame");
    if (dependencyCount != 0 && dependencies == nullptr)
        throw std::invalid_argument("bone evaluation dependencies");
    const std::uint32_t selectorCount = definitions.SelectorCount();
    if (dependencyCount > selectorCount)
        throw std::invalid_argument("bone evaluation dependency count");

    const std::uint32_t boneCount = definitions.BoneCount();
    const std::uint32_t ikCount = definitions.IkCount();
    const std::uint8_t physicsMode = definitions.PhysicsMode();
    const std::int32_t maxBoneLayer = definitions.MaxBoneLayer();

    // Frame retained budget: the two owning objects plus the pose capacity the
    // allocator actually reserves. The count is checked before the vector is
    // allocated and the capacity difference is supplemented afterwards.
    std::uint64_t frameBytes = 0;
    Charge(frameBytes, 1,
           static_cast<std::uint64_t>(sizeof(BoneEvaluationFrame)) +
               static_cast<std::uint64_t>(sizeof(BoneEvaluationFrame::Impl)),
           maxRetainedBytes);
    Charge(frameBytes, boneCount, sizeof(BoneEvaluationPose), maxRetainedBytes);

    // The workspace budget owns the final frame too, plus every working
    // container, both model records and the conservative scale-map nodes.
    Workspace workspace(maxWorkspaceBytes);
    workspace.Charge(1, sizeof(BoneEvaluationFrame) +
                            sizeof(BoneEvaluationFrame::Impl));
    workspace.Charge(boneCount, sizeof(BoneEvaluationPose));

    auto result =
        std::unique_ptr<BoneEvaluationFrame>(new BoneEvaluationFrame);
    result->impl_ = std::make_unique<BoneEvaluationFrame::Impl>();
    BoneEvaluationFrame::Impl& out = *result->impl_;
    out.frame = frame;
    out.poses.resize(boneCount);
    if (out.poses.capacity() > boneCount) {
        const std::uint64_t excess = out.poses.capacity() - boneCount;
        Charge(frameBytes, excess, sizeof(BoneEvaluationPose),
               maxRetainedBytes);
        workspace.Charge(excess, sizeof(BoneEvaluationPose));
    }
    out.bytes = frameBytes;

    // ---- one private model record owned by this evaluation -----------------
    workspace.Charge(1, sizeof(mdl::ModelRecord));
    auto model = std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::BoneRecord> bones;
    ResizeCharged(bones, boneCount, workspace);
    std::vector<mdl::IkChain> chains;
    ResizeCharged(chains, ikCount, workspace);
    std::vector<std::vector<std::uint16_t>> chainLinks;
    ResizeCharged(chainLinks, ikCount, workspace);
    std::vector<mdl::BoneOrderEntry> order;
    ResizeCharged(order, selectorCount, workspace);

    model->boneCount = boneCount;
    model->ikChainCount = ikCount;
    model->boneOrderCount = selectorCount;
    model->morphCount = morphDefinitions.MorphCount();
    model->maxBoneLayer = maxBoneLayer;
    model->physicsMode = physicsMode;
    model->boneTable = boneCount != 0 ? bones.data() : nullptr;
    model->ikChains = ikCount != 0 ? chains.data() : nullptr;
    model->boneOrderTable =
        selectorCount != 0 ? static_cast<void*>(order.data()) : nullptr;

    // ---- frozen local channels, rest definitions and layer list ------------
    // The map header and its sentinel node are charged before the map exists;
    // every inserted element is charged again before its node is allocated.
    workspace.Charge(1, sizeof(bone_scale::Values));
    workspace.Charge(1, ScaleNodeBytes());
    bone_scale::Values scalePose;
    std::vector<int> layers;
    const bool pmx = physicsMode == 2;
    for (std::uint32_t index = 0; index < boneCount; ++index) {
        BoneDefinition definition{};
        if (!definitions.BoneAt(index, definition))
            throw std::invalid_argument("bone evaluation bone definition");
        mdl::BoneRecord& bone = bones[index];
        std::memcpy(bone.name, definition.name.data(), sizeof(bone.name));
        std::memcpy(bone.nameEn, definition.nameEn.data(),
                    sizeof(bone.nameEn));
        bone.parent = definition.parent;
        std::memcpy(bone.position, definition.position.data(),
                    sizeof(bone.position));
        bone.type = definition.type;
        bone.flags = definition.flags;
        bone.layer = definition.layer;
        bone.hasFlag = definition.hasFlag;
        bone.hasRigidBody = definition.hasRigidBody;
        bone.tailBone = definition.tailBone;
        bone.tailIdx = definition.tailIdx;
        std::memcpy(bone.tailOffset, definition.tailOffset.data(),
                    sizeof(bone.tailOffset));
        bone.inheritRatio = definition.inheritRatio;
        std::memcpy(bone.axis, definition.axis.data(), sizeof(bone.axis));
        std::memcpy(bone.localAxes, definition.localAxes.data(),
                    sizeof(bone.localAxes));
        bone.extParent = definition.extParent;
        bone.slotIndex = definition.slotIndex;
        bone.twistEnable = definition.twistEnable;
        std::memcpy(bone.ikLimitMin, definition.ikLimitMin.data(),
                    sizeof(bone.ikLimitMin));
        std::memcpy(bone.ikLimitMax, definition.ikLimitMax.data(),
                    sizeof(bone.ikLimitMax));
        bone.ikTarget = definition.ikTarget;
        bone.ikLoop = definition.ikLoop;
        bone.ikAngle = definition.ikAngle;
        // PMX file IK links stay owned by the definition snapshot; this kernel
        // resolves IK through the compact runtime chain records instead.
        bone.ikLinkCount = 0;
        bone.ikLinks = nullptr;
        bone.rotQuat[3] = 1.0f;
        bone.rotQuat2[3] = 1.0f;
        if (pmx) {
            for (std::size_t element = 0; element < 16; ++element)
                bone.matWorld[element] =
                    element % 5 == 0 ? 1.0f : 0.0f;
        } else {
            bone.matWorld[10] = bone.matWorld[15] = 1.0f;
        }

        BoneMotionSample sample{};
        const BoneMotionStatus motionStatus =
            motion.PrepareKinematicChannels(static_cast<int>(index), frame,
                                            display, sample);
        // With a validated capture, a legal frame and a valid bone index an
        // InvalidSample can only mean nonfinite interpolation arithmetic.
        if (motionStatus == BoneMotionStatus::InvalidSample)
            throw std::overflow_error("bone evaluation motion sample overflow");
        if (motionStatus != BoneMotionStatus::Ok)
            throw std::invalid_argument("bone evaluation motion sample");
        std::memcpy(bone.trans, sample.position.data(), sizeof(bone.trans));
        std::memcpy(bone.rotQuat, sample.rotation.data(),
                    sizeof(bone.rotQuat));
        bone.physicsDisabled = sample.physicsDisabled ? 1 : 0;
        if (!bone_scale::Identity(sample.scale)) {
            workspace.Charge(1, ScaleNodeBytes());
            scalePose.emplace(static_cast<int>(index), sample.scale);
        }
        if (definition.layer >= 0 && definition.layer <= maxBoneLayer)
            PushCharged(layers, static_cast<int>(definition.layer), workspace);
    }
    std::sort(layers.begin(), layers.end());
    layers.erase(std::unique(layers.begin(), layers.end()), layers.end());

    // ---- compact runtime IK chains in native order -------------------------
    for (std::uint32_t index = 0; index < ikCount; ++index) {
        BoneDefinitionIk definition{};
        if (!definitions.IkAt(index, definition))
            throw std::invalid_argument("bone evaluation IK definition");
        if (definition.linkCount > 0xFFu)
            throw std::invalid_argument("bone evaluation IK link count");
        mdl::IkChain& chain = chains[index];
        chain.boneIndex = definition.bone;
        chain.targetBone = definition.target;
        chain.linkCount = static_cast<std::uint8_t>(definition.linkCount);
        chain.iterations = definition.iterations;
        chain.maxAngle = definition.maxAngle;
        std::uint8_t enabled = 0;
        if (display.SampleIk(static_cast<int>(index), frame, enabled) !=
            DisplayMotionStatus::Ok)
            throw std::invalid_argument("bone evaluation IK enable sample");
        chain.enabled = enabled;
        std::vector<std::uint16_t>& links = chainLinks[index];
        ResizeCharged(links, definition.linkCount, workspace);
        chain.links = links.empty() ? nullptr : links.data();
        for (std::uint32_t link = 0; link < definition.linkCount; ++link) {
            std::uint32_t bone = 0;
            if (!definitions.IkLinkAt(index, link, bone) || bone > 0xFFFFu)
                throw std::invalid_argument("bone evaluation IK link");
            links[link] = static_cast<std::uint16_t>(bone);
        }
    }

    // ---- selector windows and the explicit dependency remap -----------------
    // dependencyForSelector is private working scratch; it keeps the mapping
    // from one captured selector to its caller-supplied dependency.
    std::vector<std::int32_t> dependencyForSelector;
    ResizeCharged(dependencyForSelector, selectorCount, workspace);
    std::fill(dependencyForSelector.begin(), dependencyForSelector.end(), -1);
    // Private index scratch for the same-raw-target agreement check. Sorting
    // these indices groups equal (modelIndex, boneIndex) pairs without
    // touching the caller's dependency order.
    std::vector<std::uint32_t> dependencyOrder;
    ResizeCharged(dependencyOrder, dependencyCount, workspace);
    for (std::uint32_t dependency = 0; dependency < dependencyCount;
         ++dependency)
        dependencyOrder[dependency] = dependency;
    for (std::uint32_t dependency = 0; dependency < dependencyCount;
         ++dependency) {
        const BoneEvaluationDependency& value = dependencies[dependency];
        if (!Finite(value.restPosition.data(), value.restPosition.size()) ||
            !Finite(value.attachment[0].data(), value.attachment[0].size()) ||
            !Finite(value.attachment[1].data(), value.attachment[1].size()))
            throw std::invalid_argument("bone evaluation dependency value");
        if (value.selector >= selectorCount)
            throw std::invalid_argument("bone evaluation dependency selector");
        if (dependencyForSelector[value.selector] != -1)
            throw std::invalid_argument(
                "bone evaluation duplicate dependency");
        dependencyForSelector[value.selector] =
            static_cast<std::int32_t>(dependency);
    }
    // Distinct selectors may name the same raw target, but a raw target has
    // exactly one native rest position and phase matrix. Two callers must
    // therefore agree element by element; the sort above makes that an
    // adjacent comparison instead of a quadratic scan.
    std::sort(dependencyOrder.begin(), dependencyOrder.end(),
              [dependencies](std::uint32_t left, std::uint32_t right) {
                  if (dependencies[left].modelIndex !=
                      dependencies[right].modelIndex)
                      return dependencies[left].modelIndex <
                             dependencies[right].modelIndex;
                  return dependencies[left].boneIndex <
                         dependencies[right].boneIndex;
              });
    for (std::size_t slot = 1; slot < dependencyOrder.size(); ++slot) {
        const BoneEvaluationDependency& left =
            dependencies[dependencyOrder[slot - 1]];
        const BoneEvaluationDependency& right =
            dependencies[dependencyOrder[slot]];
        if (left.modelIndex != right.modelIndex ||
            left.boneIndex != right.boneIndex)
            continue;
        if (!SameValues(left.restPosition.data(), right.restPosition.data(),
                        3) ||
            !SameValues(left.attachment[0].data(),
                        right.attachment[0].data(), 16) ||
            !SameValues(left.attachment[1].data(),
                        right.attachment[1].data(), 16))
            throw std::invalid_argument(
                "bone evaluation dependency target mismatch");
    }

    for (std::uint32_t index = 0; index < selectorCount; ++index) {
        DisplaySelectorSample sample{};
        if (display.SampleSelector(static_cast<int>(index), frame, sample) !=
            DisplayMotionStatus::Ok)
            throw std::invalid_argument("bone evaluation selector sample");
        std::int32_t definitionBone = 0;
        if (!definitions.SelectorBone(index, definitionBone))
            throw std::invalid_argument("bone evaluation selector definition");
        mdl::BoneOrderEntry& entry = order[index];
        entry.boneIndex = definitionBone;
        entry.linkedModel = sample.modelIndex;
        entry.linkedBone = sample.boneIndex;
        entry.windowStart = sample.windowStart;
        entry.windowEnd = sample.windowEnd;
        const std::int32_t dependency = dependencyForSelector[index];
        if (sample.modelIndex >= 0) {
            if (dependency < 0)
                throw std::invalid_argument(
                    "bone evaluation missing dependency");
            if (dependencies[dependency].modelIndex != sample.modelIndex ||
                dependencies[dependency].boneIndex != sample.boneIndex)
                throw std::invalid_argument(
                    "bone evaluation dependency mismatch");
            // The whole local model treats every active selector as one
            // compact proxy model, so the workspace never allocates a full
            // ModelRecord per selector. Raw pairs are still checked above.
            entry.linkedModel = 0;
            entry.linkedBone = dependency;
        } else if (dependency >= 0) {
            throw std::invalid_argument(
                "bone evaluation unexpected dependency");
        }
    }

    // ---- one heap proxy model carries every explicit external target ------
    std::unique_ptr<mdl::ModelRecord> proxy;
    std::vector<mdl::BoneRecord> proxyBones;
    std::array<unsigned char*, 1> slots{};
    workspace.Charge(1, sizeof(slots));
    if (dependencyCount != 0) {
        workspace.Charge(1, sizeof(mdl::ModelRecord));
        proxy = std::make_unique<mdl::ModelRecord>();
        ResizeCharged(proxyBones, dependencyCount, workspace);
        proxy->boneCount = dependencyCount;
        proxy->boneTable = proxyBones.data();
        for (std::uint32_t dependency = 0; dependency < dependencyCount;
             ++dependency) {
            mdl::BoneRecord& bone = proxyBones[dependency];
            std::memcpy(bone.position,
                        dependencies[dependency].restPosition.data(),
                        sizeof(bone.position));
        }
        slots[0] = reinterpret_cast<unsigned char*>(proxy.get());
    }

    // ---- frozen morph weights and native morph evaluation ------------------
    const std::uint32_t morphCount = morphDefinitions.MorphCount();
    std::vector<float> weights;
    ResizeCharged(weights, morphCount, workspace);
    for (std::uint32_t morph = 0; morph < morphCount; ++morph) {
        float weight = 0.0f;
        // Capture keeps one frame-zero head per track, and the index and frame
        // are already validated, so a false sample means a nonfinite
        // interpolation result rather than malformed input.
        if (!morphWeights.Sample(static_cast<int>(morph), frame, weight))
            throw std::overflow_error("bone evaluation morph weight overflow");
        weights[morph] = weight;
    }
    std::unique_ptr<MorphEvaluationFrame> morphFrame =
        MorphEvaluationFrame::Evaluate(
            morphDefinitions, morphCount != 0 ? weights.data() : nullptr,
            morphCount, workspace.Remaining());
    workspace.Charge(1, morphFrame->RetainedBytes());
    if (morphFrame->PhysicsMode() != physicsMode)
        throw std::invalid_argument("bone evaluation morph physics mode");
    const std::uint32_t morphBoneCount = morphFrame->BoneCount();
    if (morphBoneCount != 0 && morphBoneCount != boneCount)
        throw std::invalid_argument("bone evaluation morph bone count");
    std::vector<mdl::BoneMorphOffsetRecord> morphOffsets;
    ResizeCharged(morphOffsets, morphBoneCount, workspace);
    for (std::uint32_t bone = 0; bone < morphBoneCount; ++bone)
        if (!morphFrame->BoneAt(bone, morphOffsets[bone]))
            throw std::invalid_argument("bone evaluation morph bone");
    morphFrame.reset();
    model->boneMorphCount = static_cast<std::int32_t>(morphBoneCount);
    model->boneMorphTable =
        morphBoneCount != 0 ? morphOffsets.data() : nullptr;

    // ---- two native sweeps, one prepare per phase --------------------------
    for (int phase = 0; phase < 2; ++phase) {
        for (std::uint32_t dependency = 0; dependency < dependencyCount;
             ++dependency)
            std::memcpy(proxyBones[dependency].matExtra,
                        dependencies[dependency].attachment[phase].data(),
                        sizeof(proxyBones[dependency].matExtra));
        BonePreparePosePrivate(*model, 0, context);
        for (const int layer : layers)
            BoneFrameTransformPrivate(
                *model, static_cast<unsigned char>(phase), layer,
                slots.data(), 0, context);
        // The attachment of this complete phase is published immediately; the
        // next phase keeps the accumulated matrices instead of resetting them.
        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            std::memcpy(out.poses[bone].attachment[phase].data(),
                        bones[bone].matExtra,
                        sizeof(out.poses[bone].attachment[phase]));
    }

    // ---- rigid basis and complete scale -----------------------------------
    std::vector<bone_scale::Transform> rigid;
    ResizeCharged(rigid, boneCount, workspace);
    std::vector<bone_scale::Transform> scaled;
    ResizeCharged(scaled, boneCount, workspace);
    for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        std::memcpy(rigid[bone].data(), bones[bone].matInit,
                    sizeof(rigid[bone]));
    bone_scale::BuildTransformsPrivate(*model, scalePose, rigid, scaled);
    if (scaled.size() != boneCount)
        throw std::invalid_argument("bone evaluation scale result");
    for (std::uint32_t bone = 0; bone < boneCount; ++bone) {
        BoneEvaluationPose& pose = out.poses[bone];
        pose.rigid = rigid[bone];
        pose.scaled = scaled[bone];
        // Row-vector affine transform of the rest position. The accumulation
        // is double and the stored result is float, matching the native frame
        // oracle to the published test tolerance.
        for (std::size_t column = 0; column < 3; ++column) {
            double value = static_cast<double>(scaled[bone][12 + column]);
            for (std::size_t row = 0; row < 3; ++row)
                value += static_cast<double>(bones[bone].position[row]) *
                         static_cast<double>(scaled[bone][row * 4 + column]);
            pose.jointPosition[column] = static_cast<float>(value);
        }
    }

    std::uint8_t visible = 0;
    if (display.SampleVisibility(frame, visible) != DisplayMotionStatus::Ok)
        throw std::invalid_argument("bone evaluation visibility sample");
    out.visible = visible;

    // Only completed results are checked for nonfinite arithmetic.
    for (const BoneEvaluationPose& pose : out.poses) {
        RequireFinite(pose.rigid.data(), pose.rigid.size(),
                      "bone evaluation rigid overflow");
        RequireFinite(pose.scaled.data(), pose.scaled.size(),
                      "bone evaluation scaled overflow");
        RequireFinite(pose.attachment[0].data(), pose.attachment[0].size(),
                      "bone evaluation attachment overflow");
        RequireFinite(pose.attachment[1].data(), pose.attachment[1].size(),
                      "bone evaluation attachment overflow");
        RequireFinite(pose.jointPosition.data(), pose.jointPosition.size(),
                      "bone evaluation joint overflow");
    }
    return result;
}

}  // namespace mikumikustudio
#endif
