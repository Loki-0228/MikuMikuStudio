#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mikumikustudio {
class MMDApp;
namespace mdl { struct BoneKey; struct BoneOrderEntry; struct BoneRecord; }
namespace bone_scale { class AuthoringState; }
namespace bone_curve { class AuthoringState; }
}
namespace mikumikustudio::plugins {
// A direction-specific, read-only view of the complete target key tracks.
// Pointers returned by key remain valid throughout sampling/preparation.
// The callback returns null for invalid slots. The prepared participant owns
// its results and does not retain this view or any callback/context pointer.
struct BoneEvaluationView {
    const void* context = nullptr;
    const mdl::BoneKey* (*key)(const void*, std::uint32_t) noexcept = nullptr;
    std::size_t keyCapacity = 0;
    const bone_scale::AuthoringState* scale = nullptr;
    const bone_curve::AuthoringState* curves = nullptr;
    // Optional target external-parent windows for a combined display edit.
    // Evaluation guards the live source selectors, but samples these targets.
    const mdl::BoneOrderEntry* selectors = nullptr;
    std::size_t selectorCount = 0;
};
struct BoneEvaluationSample {
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    bone_scale::Matrix scale;
    bool writePose = false;
};
// Read one target track at an explicit nonnegative frame, independently of the
// live playhead and dirty flag. No pose, cursor, side storage or physics writes.
// bone supplies type/physics/selector metadata and the fallback pose for types
// not sampled natively (free curves may still provide their pose). All keys,
// scale, curves and required selector windows come exclusively from view.
// Unlike PrepareBoneEvaluation, a required selector has no live fallback here.
// writePose controls position/rotation publication; scale is a separate result.
// The caller owns source lifetime/serialization and dirty-draft policy. No view
// pointers are retained; failure leaves out unchanged, including physics refusal.
MmsResult SampleBoneEvaluation(const mdl::BoneRecord& bone, unsigned boneIndex,
    unsigned boneCount, int physicsMode, int sampleFrame, const BoneEvaluationView&,
    BoneEvaluationSample& out) noexcept;
// Evaluate affected, clean bones at app.CurrentFrame() each time an edit, undo
// or redo is prepared. A nonzero bonePhysicsState byte preserves that bone's
// unregistered position, rotation and scale. Other bones, morphs, display/IK
// state, legacy rings and physics switches are never written.
//
// The enclosing record retains the scene/model and validates all native key
// and side-table inputs. Prepare the scale AuthoringRestore first: its storage
// lease must cover this participant if a target scale needs new pose storage.
// After validating ALL participants, publish native keys and side authoring
// before this participant. Publish this participant BEFORE target selectors.
// IsCurrent checks frame, pose and source selector metadata,
// never the key pool or authoring revision that those earlier writes change.
// Publish is one-shot, allocation-free and must follow successful validation.
// Physics mode changes and mode-3 IK arming return MMS_E_NOT_READY; no solver
// setter or whole-model seek is called. Malformed tracks return MMS_E_ARGUMENT.
MmsResult PrepareBoneEvaluation(MMDApp&, unsigned modelSlot,
    const std::vector<unsigned>& affectedBones, const BoneEvaluationView&,
    std::unique_ptr<PreparedHistoryChange>&) noexcept;
}
#endif
