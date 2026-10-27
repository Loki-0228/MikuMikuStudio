#include "mikumikustudio/bone_scene_evaluation.hpp"
#if !MMD_ORIGINAL_PROFILE

#include "mikumikustudio/app_layout.hpp"
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

namespace detail {

// One independent model workspace: the captured model recast into the native
// private record, its working channels, selector table, scale pose, layer list,
// morph offsets and the candidate result frame. The same object drives the
// single-source wrapper and every model of a scene sweep, so the five native
// phases, pose preparation and scale finalization have exactly one copy.
struct BoneEvaluationWorkspace {
    std::unique_ptr<mdl::ModelRecord> model;
    std::vector<mdl::BoneRecord> bones;
    std::vector<mdl::IkChain> chains;
    std::vector<std::vector<std::uint16_t>> chainLinks;
    std::vector<mdl::BoneOrderEntry> selectors;
    bone_scale::Values scalePose;
    std::vector<int> layers;
    std::vector<mdl::BoneMorphOffsetRecord> morphOffsets;
    std::unique_ptr<BoneEvaluationFrame> frame;

    // Allocates the candidate frame and charges it to both budgets. The
    // returned count is the frame's retained size; maxRetainedBytes is the
    // budget that frame alone may consume.
    std::uint64_t CreateFrame(double frameValue, std::uint32_t boneCount,
                              std::uint64_t maxRetainedBytes,
                              Workspace& workspace);

    // Recasts the frozen definition/motion/display components into the private
    // model record and fills every workspace-owned channel. Selector bodies and
    // morph offsets are filled by the callers because their external targets
    // differ between the single-source and scene cases.
    void Prepare(const BoneEvaluationSource& source, const EvaluationTime& time,
                 Workspace& workspace);

    // Selector entry mapping for a scene: the captured raw model slot and raw
    // target bone stay in place, so the native world pass reads that model's
    // private matExtra directly (including self references and cycles).
    void PrepareSelectorsRaw(const BoneEvaluationSource& source,
                             std::uint32_t selectionFrame);

    // Selector entry mapping for the explicit-dependency wrapper: the caller's
    // target still becomes the compact proxy at slot zero.
    void PrepareSelectorsDependency(
        const BoneEvaluationSource& source, std::uint32_t selectionFrame,
        const BoneEvaluationDependency* dependencies,
        const std::int32_t* dependencyForSelector);

    // Frozen morph weights, native morph evaluation and the bone-offset table.
    void PrepareMorphs(const BoneEvaluationSource& source, double sampleFrame,
                       Workspace& workspace);

    // One complete phase for this model: one native pose preparation, every
    // actual layer in ascending order, then an immediate copy of the phase's
    // native attachment. Keeps accumulated matrices across phases.
    void RunPhase(int phase, unsigned char* const* slots,
                  const BoneTransformContext& context);

    // Rigid basis, complete scale, joint position, visibility and the finite
    // checks. Runs only after every phase of every model has completed.
    void Finalize(const BoneEvaluationSource& source, const EvaluationTime& time,
                  Workspace& workspace);
};

std::uint64_t BoneEvaluationWorkspace::CreateFrame(
    double frameValue, std::uint32_t boneCount,
    std::uint64_t maxRetainedBytes, Workspace& workspace) {
    // Frame retained budget: the two owning objects plus the pose capacity the
    // allocator actually reserves. The count is checked before the vector is
    // allocated and the capacity difference is supplemented afterwards.
    std::uint64_t frameBytes = 0;
    Charge(frameBytes, 1,
           static_cast<std::uint64_t>(sizeof(BoneEvaluationFrame)) +
               static_cast<std::uint64_t>(sizeof(BoneEvaluationFrame::Impl)),
           maxRetainedBytes);
    Charge(frameBytes, boneCount, sizeof(BoneEvaluationPose), maxRetainedBytes);

    // The workspace budget owns the final frame too.
    workspace.Charge(1, sizeof(BoneEvaluationFrame) +
                            sizeof(BoneEvaluationFrame::Impl));
    workspace.Charge(boneCount, sizeof(BoneEvaluationPose));

    frame = std::unique_ptr<BoneEvaluationFrame>(new BoneEvaluationFrame);
    frame->impl_ = std::make_unique<BoneEvaluationFrame::Impl>();
    BoneEvaluationFrame::Impl& out = *frame->impl_;
    out.frame = frameValue;
    out.poses.resize(boneCount);
    if (out.poses.capacity() > boneCount) {
        const std::uint64_t excess = out.poses.capacity() - boneCount;
        Charge(frameBytes, excess, sizeof(BoneEvaluationPose),
               maxRetainedBytes);
        workspace.Charge(excess, sizeof(BoneEvaluationPose));
    }
    out.bytes = frameBytes;
    return frameBytes;
}

void BoneEvaluationWorkspace::Prepare(
    const BoneEvaluationSource& source, const EvaluationTime& time,
    Workspace& workspace) {
    const BoneDefinitionSnapshot& definitions = *source.impl_->definitions;
    const BoneMotionSnapshot& motion = *source.impl_->motion;
    const DisplayMotionSnapshot& display = *source.impl_->display;
    const MorphDefinitionSnapshot& morphDefinitions =
        *source.impl_->morphDefinitions;

    const std::uint32_t boneCount = definitions.BoneCount();
    const std::uint32_t ikCount = definitions.IkCount();
    const std::uint32_t selectorCount = definitions.SelectorCount();
    const std::uint8_t physicsMode = definitions.PhysicsMode();
    const std::int32_t maxBoneLayer = definitions.MaxBoneLayer();

    // ---- one private model record owned by this workspace ------------------
    workspace.Charge(1, sizeof(mdl::ModelRecord));
    model = std::make_unique<mdl::ModelRecord>();
    ResizeCharged(bones, boneCount, workspace);
    ResizeCharged(chains, ikCount, workspace);
    ResizeCharged(chainLinks, ikCount, workspace);
    ResizeCharged(selectors, selectorCount, workspace);

    model->boneCount = boneCount;
    model->ikChainCount = ikCount;
    model->boneOrderCount = selectorCount;
    model->morphCount = morphDefinitions.MorphCount();
    model->maxBoneLayer = maxBoneLayer;
    model->physicsMode = physicsMode;
    model->boneTable = boneCount != 0 ? bones.data() : nullptr;
    model->ikChains = ikCount != 0 ? chains.data() : nullptr;
    model->boneOrderTable =
        selectorCount != 0 ? static_cast<void*>(selectors.data()) : nullptr;

    // ---- frozen local channels, rest definitions and layer list ------------
    // The workspace object's map header and sentinel were charged by the caller
    // before this workspace was constructed; every inserted element is charged
    // again before its node is allocated.
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
        // Local channels are continuous in Frame(), but native key selection,
        // physicsDisabled and the selector window use the exact discrete frame.
        const BoneMotionStatus motionStatus =
            motion.PrepareKinematicChannelsAt(static_cast<int>(index), time,
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
        // IK enable switches are discrete tracks: never sample them with the
        // continuous frame, which may round across the key boundary.
        if (display.SampleIk(static_cast<int>(index),
                             static_cast<double>(time.DiscreteFrame()),
                             enabled) != DisplayMotionStatus::Ok)
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
}

void BoneEvaluationWorkspace::PrepareSelectorsRaw(
    const BoneEvaluationSource& source, std::uint32_t selectionFrame) {
    const BoneDefinitionSnapshot& definitions = *source.impl_->definitions;
    const DisplayMotionSnapshot& display = *source.impl_->display;
    const std::uint32_t selectorCount = definitions.SelectorCount();

    // Raw captured slots are kept verbatim; scene evaluation wires the actual
    // private model workspaces into the slot table instead of a proxy.
    for (std::uint32_t index = 0; index < selectorCount; ++index) {
        DisplaySelectorSample sample{};
        // External-parent selection is discrete, so sample the exact frame.
        if (display.SampleSelector(static_cast<int>(index),
                                   static_cast<double>(selectionFrame),
                                   sample) != DisplayMotionStatus::Ok)
            throw std::invalid_argument("bone scene selector sample");
        std::int32_t definitionBone = 0;
        if (!definitions.SelectorBone(index, definitionBone))
            throw std::invalid_argument("bone evaluation selector definition");
        mdl::BoneOrderEntry& entry = selectors[index];
        entry.boneIndex = definitionBone;
        entry.linkedModel = sample.modelIndex;
        entry.linkedBone = sample.boneIndex;
        entry.windowStart = sample.windowStart;
        entry.windowEnd = sample.windowEnd;
    }
}

void BoneEvaluationWorkspace::PrepareSelectorsDependency(
    const BoneEvaluationSource& source, std::uint32_t selectionFrame,
    const BoneEvaluationDependency* dependencies,
    const std::int32_t* dependencyForSelector) {
    const BoneDefinitionSnapshot& definitions = *source.impl_->definitions;
    const DisplayMotionSnapshot& display = *source.impl_->display;
    const std::uint32_t selectorCount = definitions.SelectorCount();

    for (std::uint32_t index = 0; index < selectorCount; ++index) {
        DisplaySelectorSample sample{};
        // The dependency legality comparison below reads discrete selector
        // tracks, so it must use the exact frame and not the continuous one.
        if (display.SampleSelector(static_cast<int>(index),
                                   static_cast<double>(selectionFrame),
                                   sample) != DisplayMotionStatus::Ok)
            throw std::invalid_argument("bone evaluation selector sample");
        std::int32_t definitionBone = 0;
        if (!definitions.SelectorBone(index, definitionBone))
            throw std::invalid_argument("bone evaluation selector definition");
        mdl::BoneOrderEntry& entry = selectors[index];
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
}

void BoneEvaluationWorkspace::PrepareMorphs(
    const BoneEvaluationSource& source, double sampleFrame,
    Workspace& workspace) {
    const MorphDefinitionSnapshot& morphDefinitions =
        *source.impl_->morphDefinitions;
    const morph_curve::SamplingState& morphWeights = *source.impl_->morphWeights;
    const std::uint8_t physicsMode = source.impl_->definitions->PhysicsMode();

    // ---- frozen morph weights and native morph evaluation ------------------
    const std::uint32_t morphCount = morphDefinitions.MorphCount();
    std::vector<float> weights;
    ResizeCharged(weights, morphCount, workspace);
    for (std::uint32_t morph = 0; morph < morphCount; ++morph) {
        float weight = 0.0f;
        // Capture keeps one frame-zero head per track, and the index and frame
        // are already validated, so a false sample means a nonfinite
        // interpolation result rather than malformed input.
        if (!morphWeights.Sample(static_cast<int>(morph), sampleFrame, weight))
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
    if (morphBoneCount != 0 && morphBoneCount != model->boneCount)
        throw std::invalid_argument("bone evaluation morph bone count");
    ResizeCharged(morphOffsets, morphBoneCount, workspace);
    for (std::uint32_t bone = 0; bone < morphBoneCount; ++bone)
        if (!morphFrame->BoneAt(bone, morphOffsets[bone]))
            throw std::invalid_argument("bone evaluation morph bone");
    morphFrame.reset();
    model->boneMorphCount = static_cast<std::int32_t>(morphBoneCount);
    model->boneMorphTable =
        morphBoneCount != 0 ? morphOffsets.data() : nullptr;
}

void BoneEvaluationWorkspace::RunPhase(
    int phase, unsigned char* const* slots,
    const BoneTransformContext& context) {
    BonePreparePosePrivate(*model, 0, context);
    for (const int layer : layers)
        BoneFrameTransformPrivate(
            *model, static_cast<unsigned char>(phase), layer, slots, 0,
            context);
    // The attachment of this complete phase is published immediately; the
    // next phase keeps the accumulated matrices instead of resetting them.
    for (std::uint32_t bone = 0; bone < bones.size(); ++bone)
        std::memcpy(frame->impl_->poses[bone].attachment[phase].data(),
                    bones[bone].matExtra,
                    sizeof(frame->impl_->poses[bone].attachment[phase]));
}

void BoneEvaluationWorkspace::Finalize(const BoneEvaluationSource& source,
                                       const EvaluationTime& time,
                                       Workspace& workspace) {
    const DisplayMotionSnapshot& display = *source.impl_->display;
    const std::uint32_t boneCount = static_cast<std::uint32_t>(bones.size());

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
    BoneEvaluationFrame::Impl& out = *frame->impl_;
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
    // Visibility is a discrete display track; the frame metadata above stays
    // continuous, but the switch itself uses the exact discrete frame.
    if (display.SampleVisibility(static_cast<double>(time.DiscreteFrame()),
                                 visible) != DisplayMotionStatus::Ok)
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
}

}  // namespace detail

std::unique_ptr<BoneEvaluationFrame> BoneEvaluationSource::Evaluate(
    double frame, const BoneTransformContext& context,
    const BoneEvaluationDependency* dependencies, std::uint32_t dependencyCount,
    std::uint64_t maxRetainedBytes,
    std::uint64_t maxWorkspaceBytes) const {
    // The legacy double entry only validates and delegates; it shares the exact
    // path and does not duplicate solver, budget or failure logic.
    EvaluationTime time;
    if (!EvaluationTime::FromFrame(frame, time))
        throw std::invalid_argument("bone evaluation frame");
    return EvaluateAt(time, context, dependencies, dependencyCount,
                      maxRetainedBytes, maxWorkspaceBytes);
}

std::unique_ptr<BoneEvaluationFrame> BoneEvaluationSource::EvaluateAt(
    const EvaluationTime& time, const BoneTransformContext& context,
    const BoneEvaluationDependency* dependencies, std::uint32_t dependencyCount,
    std::uint64_t maxRetainedBytes,
    std::uint64_t maxWorkspaceBytes) const {
    if (!impl_ || !impl_->definitions || !impl_->motion || !impl_->display ||
        !impl_->morphDefinitions || !impl_->morphWeights)
        throw std::invalid_argument("bone evaluation source is empty");

    const BoneDefinitionSnapshot& definitions = *impl_->definitions;

    if (dependencyCount != 0 && dependencies == nullptr)
        throw std::invalid_argument("bone evaluation dependencies");
    const std::uint32_t selectorCount = definitions.SelectorCount();
    if (dependencyCount > selectorCount)
        throw std::invalid_argument("bone evaluation dependency count");

    // The wrapper keeps the legacy explicit-dependency contract: raw target
    // pairs are validated here and remapped onto the compact proxy below. The
    // shared workspace only owns the private model and the native phases.
    Workspace workspace(maxWorkspaceBytes);
    // The workspace owns its scale map object and the map's default-constructed
    // sentinel node; both are charged before the member is constructed.
    workspace.Charge(1, sizeof(detail::BoneEvaluationWorkspace) +
                            ScaleNodeBytes());
    detail::BoneEvaluationWorkspace evaluation;
    evaluation.CreateFrame(time.Frame(), definitions.BoneCount(),
                           maxRetainedBytes, workspace);
    evaluation.Prepare(*this, time, workspace);

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

    evaluation.PrepareSelectorsDependency(*this, time.DiscreteFrame(),
                                          dependencies,
                                          dependencyForSelector.data());

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

    evaluation.PrepareMorphs(*this, time.Frame(), workspace);

    // ---- two native sweeps, one prepare per phase --------------------------
    for (int phase = 0; phase < 2; ++phase) {
        for (std::uint32_t dependency = 0; dependency < dependencyCount;
             ++dependency)
            std::memcpy(proxyBones[dependency].matExtra,
                        dependencies[dependency].attachment[phase].data(),
                        sizeof(proxyBones[dependency].matExtra));
        evaluation.RunPhase(phase, slots.data(), context);
    }

    evaluation.Finalize(*this, time, workspace);
    return std::move(evaluation.frame);
}

// ---- scene frame ----------------------------------------------------------

struct BoneSceneFrame::Impl {
    struct Entry {
        BoneSceneModelInfo info{};
        std::unique_ptr<BoneEvaluationFrame> frame;
    };
    std::vector<Entry> models;
    double frame = 0.0;
    std::uint64_t bytes = 0;
};

BoneSceneFrame::BoneSceneFrame() = default;
BoneSceneFrame::~BoneSceneFrame() = default;

std::uint32_t BoneSceneFrame::ModelCount() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(impl_->models.size()) : 0;
}

double BoneSceneFrame::Frame() const noexcept {
    return impl_ ? impl_->frame : 0.0;
}

std::uint64_t BoneSceneFrame::RetainedBytes() const noexcept {
    return impl_ ? impl_->bytes : sizeof(*this);
}

bool BoneSceneFrame::ModelAt(std::uint32_t index,
                             BoneSceneModelInfo& out) const noexcept {
    if (!impl_ || index >= impl_->models.size()) return false;
    out = impl_->models[index].info;
    return true;
}

const BoneEvaluationFrame* BoneSceneFrame::ModelFrame(
    std::uint32_t index) const noexcept {
    if (!impl_ || index >= impl_->models.size()) return nullptr;
    return impl_->models[index].frame.get();
}

// ---- scene source ---------------------------------------------------------

struct BoneSceneSource::Impl {
    struct Entry {
        BoneSceneModelInfo info{};
        std::unique_ptr<BoneEvaluationSource> source;
    };
    std::vector<Entry> models;
    std::uint64_t bytes = 0;
};

BoneSceneSource::BoneSceneSource() = default;
BoneSceneSource::~BoneSceneSource() = default;

std::unique_ptr<BoneSceneSource> BoneSceneSource::Capture(
    const BoneSceneModelInput* models, std::uint32_t count,
    std::uint64_t limit) {
    if (count > static_cast<std::uint32_t>(kModelSlotCount))
        throw std::invalid_argument("bone scene model count");
    if (count != 0 && models == nullptr)
        throw std::invalid_argument("bone scene models");

    // Validate the complete requested set before capturing any component: the
    // host supplies one distinct live model per raw slot and object identity.
    std::array<int, kModelSlotCount> slotToInput{};
    slotToInput.fill(-1);
    for (std::uint32_t index = 0; index < count; ++index) {
        const BoneSceneModelInput& input = models[index];
        if (input.model == nullptr)
            throw std::invalid_argument("bone scene model");
        if (input.slot >= static_cast<std::uint32_t>(kModelSlotCount))
            throw std::invalid_argument("bone scene model slot");
        if (slotToInput[input.slot] != -1)
            throw std::invalid_argument("bone scene duplicate model slot");
        slotToInput[input.slot] = static_cast<int>(index);
        if (input.identity.lo == 0 && input.identity.hi == 0)
            throw std::invalid_argument("bone scene model identity");
        if (input.identity.generation == 0)
            throw std::invalid_argument("bone scene model generation");
        for (std::uint32_t previous = 0; previous < index; ++previous) {
            if (models[previous].model == input.model)
                throw std::invalid_argument("bone scene duplicate model");
            if (models[previous].identity.lo == input.identity.lo &&
                models[previous].identity.hi == input.identity.hi)
                throw std::invalid_argument("bone scene duplicate identity");
        }
    }

    // The scene object, its entry vector and every child component share one
    // retained budget. Child components are captured one by one with the
    // remaining amount, then their complete retained size is aggregated.
    std::uint64_t retained = 0;
    Charge(retained, 1, sizeof(BoneSceneSource) + sizeof(Impl), limit);
    auto result = std::unique_ptr<BoneSceneSource>(new BoneSceneSource);
    result->impl_ = std::make_unique<Impl>();
    Impl& value = *result->impl_;
    Charge(retained, count, sizeof(Impl::Entry), limit);
    value.models.resize(count);
    if (value.models.capacity() > count)
        Charge(retained, value.models.capacity() - count,
               sizeof(Impl::Entry), limit);

    for (std::uint32_t index = 0; index < count; ++index) {
        const BoneSceneModelInput& input = models[index];
        std::unique_ptr<BoneEvaluationSource> child =
            BoneEvaluationSource::Capture(input.model, limit - retained);
        Charge(retained, 1, child->RetainedBytes(), limit);
        Impl::Entry& entry = value.models[index];
        entry.info.slot = input.slot;
        entry.info.calculationOrder =
            static_cast<std::uint32_t>(mdl::Mdl(input.model)->comboSelIndex2);
        entry.info.boneCount = child->BoneCount();
        entry.info.identity = input.identity;
        entry.source = std::move(child);
    }

    // Every parent selection reachable from any display key, including future
    // keys, must name one of the captured raw slots and an existing target
    // bone. Cycles across models and self references are legal; only dangling
    // references are rejected.
    for (std::uint32_t index = 0; index < count; ++index) {
        const Impl::Entry& entry = value.models[index];
        const DisplayMotionSnapshot& display = *entry.source->impl_->display;
        for (std::uint32_t key = 0; key < display.KeyCount(); ++key) {
            std::uint32_t keyFrame = 0;
            if (!display.KeyFrame(key, keyFrame))
                throw std::invalid_argument("bone scene display key");
            for (std::uint32_t selector = 0;
                 selector < display.SelectorCount(); ++selector) {
                DisplaySelectorSample sample{};
                if (display.SampleSelector(static_cast<int>(selector),
                                           keyFrame,
                                           sample) != DisplayMotionStatus::Ok)
                    throw std::invalid_argument(
                        "bone scene selector sample");
                // -1 is an inactive reference; -2 is the native ground
                // external parent. Both need no captured target and keep
                // their raw value: ResolveBoneWorld takes different paths
                // for them, so they must not be normalized to each other.
                if (sample.modelIndex == -1 || sample.modelIndex == -2)
                    continue;
                if (sample.modelIndex < -2 ||
                    sample.modelIndex >= kModelSlotCount)
                    throw std::invalid_argument(
                        "bone scene selector model");
                const int target = slotToInput[sample.modelIndex];
                if (target < 0)
                    throw std::invalid_argument(
                        "bone scene selector slot");
                if (sample.boneIndex < 0 ||
                    static_cast<std::uint32_t>(sample.boneIndex) >=
                        value.models[target].info.boneCount)
                    throw std::invalid_argument(
                        "bone scene selector bone");
            }
        }
    }

    value.bytes = retained;
    return result;
}

std::uint32_t BoneSceneSource::ModelCount() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(impl_->models.size()) : 0;
}

std::uint64_t BoneSceneSource::RetainedBytes() const noexcept {
    return impl_ ? impl_->bytes : sizeof(*this);
}

bool BoneSceneSource::ModelAt(std::uint32_t index,
                              BoneSceneModelInfo& out) const noexcept {
    if (!impl_ || index >= impl_->models.size()) return false;
    out = impl_->models[index].info;
    return true;
}

std::unique_ptr<BoneSceneFrame> BoneSceneSource::Evaluate(
    double frame, const BoneTransformContext& context,
    std::uint64_t maxRetainedBytes,
    std::uint64_t maxWorkspaceBytes) const {
    // The legacy double entry only validates and delegates; the scene sweep,
    // budget accounting and failure paths remain in the exact-time overload.
    EvaluationTime time;
    if (!EvaluationTime::FromFrame(frame, time))
        throw std::invalid_argument("bone scene frame");
    return EvaluateAt(time, context, maxRetainedBytes, maxWorkspaceBytes);
}

std::unique_ptr<BoneSceneFrame> BoneSceneSource::EvaluateAt(
    const EvaluationTime& time, const BoneTransformContext& context,
    std::uint64_t maxRetainedBytes,
    std::uint64_t maxWorkspaceBytes) const {
    if (!impl_) throw std::invalid_argument("bone scene source is empty");

    const std::uint32_t count =
        static_cast<std::uint32_t>(impl_->models.size());

    // The shared working budget owns the result shell first; the retained
    // budget is charged before the shell is allocated, and the shell is only
    // counted once in each budget.
    Workspace workspace(maxWorkspaceBytes);
    workspace.Charge(1, sizeof(BoneSceneFrame) +
                            sizeof(BoneSceneFrame::Impl));
    std::uint64_t retained = 0;
    Charge(retained, 1,
           static_cast<std::uint64_t>(sizeof(BoneSceneFrame)) +
               static_cast<std::uint64_t>(sizeof(BoneSceneFrame::Impl)),
           maxRetainedBytes);
    auto result = std::unique_ptr<BoneSceneFrame>(new BoneSceneFrame);
    result->impl_ = std::make_unique<BoneSceneFrame::Impl>();
    BoneSceneFrame::Impl& out = *result->impl_;
    // Frame metadata stays continuous; only discrete tracks use DiscreteFrame.
    out.frame = time.Frame();

    Charge(retained, count, sizeof(BoneSceneFrame::Impl::Entry),
           maxRetainedBytes);
    workspace.Charge(count, sizeof(BoneSceneFrame::Impl::Entry));
    out.models.resize(count);
    if (out.models.capacity() > count) {
        const std::uint64_t excess = out.models.capacity() - count;
        Charge(retained, excess, sizeof(BoneSceneFrame::Impl::Entry),
               maxRetainedBytes);
        workspace.Charge(excess, sizeof(BoneSceneFrame::Impl::Entry));
    }

    // The workspace array, the all-slot pointer table and the sorted scene
    // order are part of the shared working budget as well.
    workspace.Charge(1, sizeof(std::unique_ptr<detail::BoneEvaluationWorkspace[]>));
    workspace.Charge(count, sizeof(detail::BoneEvaluationWorkspace));
    // Each workspace owns its scale map's default-constructed sentinel node.
    workspace.Charge(count, ScaleNodeBytes());
    std::unique_ptr<detail::BoneEvaluationWorkspace[]> workspaces;
    if (count != 0)
        workspaces.reset(new detail::BoneEvaluationWorkspace[count]);
    std::array<unsigned char*, kModelSlotCount> slots{};
    workspace.Charge(1, sizeof(slots));
    std::vector<std::uint32_t> order;
    ResizeCharged(order, count, workspace);

    // Every model is prepared and retained at the same time. The child result
    // frame consumes the remaining scene retained budget one by one.
    for (std::uint32_t index = 0; index < count; ++index) {
        const Impl::Entry& entry = impl_->models[index];
        const std::uint64_t frameBytes = workspaces[index].CreateFrame(
            time.Frame(), entry.info.boneCount, maxRetainedBytes - retained,
            workspace);
        Charge(retained, 1, frameBytes, maxRetainedBytes);
        workspaces[index].Prepare(*entry.source, time, workspace);
        workspaces[index].PrepareSelectorsRaw(*entry.source,
                                              time.DiscreteFrame());
        workspaces[index].PrepareMorphs(*entry.source, time.Frame(), workspace);
        slots[entry.info.slot] =
            reinterpret_cast<unsigned char*>(workspaces[index].model.get());
    }

    for (std::uint32_t index = 0; index < count; ++index) order[index] = index;
    std::sort(order.begin(), order.end(),
              [this](std::uint32_t left, std::uint32_t right) {
                  const BoneSceneModelInfo& a = impl_->models[left].info;
                  const BoneSceneModelInfo& b = impl_->models[right].info;
                  if (a.calculationOrder != b.calculationOrder)
                      return a.calculationOrder < b.calculationOrder;
                  return a.slot < b.slot;
              });

    // Every model's phase zero completes before any model's phase one, exactly
    // like the native host's two ordered scene sweeps. No topological order,
    // cycle rejection or per-model completion is attempted: self and cyclic
    // parent reads observe the target's current private matExtra.
    for (int phase = 0; phase < 2; ++phase) {
        for (const std::uint32_t index : order) {
            const BoneSceneModelInfo& info = impl_->models[index].info;
            if (info.calculationOrder >
                static_cast<std::uint32_t>(kModelSlotCount))
                continue;  // out of the native host's reachable order range
            workspaces[index].RunPhase(phase, slots.data(), context);
        }
    }

    // Scale, joint position and visibility are finalized only after both
    // complete scene sweeps.
    for (std::uint32_t index = 0; index < count; ++index)
        workspaces[index].Finalize(*impl_->models[index].source, time,
                                   workspace);

    // Publish only fully completed child frames.
    for (std::uint32_t index = 0; index < count; ++index) {
        out.models[index].info = impl_->models[index].info;
        out.models[index].frame = std::move(workspaces[index].frame);
    }
    out.bytes = retained;
    return result;
}

}  // namespace mikumikustudio
#endif
