#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mikudancestudio {
class MMDApp;
namespace mdl { struct BoneKey; }
namespace bone_scale { class AuthoringState; }
namespace bone_curve { class AuthoringState; }
}
namespace mikudancestudio::plugins {
// A direction-specific, read-only view of the complete target key tracks.
// Pointers returned by key remain valid throughout PrepareBoneEvaluation.
// The callback returns null for invalid slots. The prepared participant owns
// its results and does not retain this view or any callback/context pointer.
struct BoneEvaluationView {
    const void* context = nullptr;
    const mdl::BoneKey* (*key)(const void*, std::uint32_t) noexcept = nullptr;
    std::size_t keyCapacity = 0;
    const bone_scale::AuthoringState* scale = nullptr;
    const bone_curve::AuthoringState* curves = nullptr;
};
// Evaluate affected, clean bones at app.CurrentFrame() each time an edit, undo
// or redo is prepared. A nonzero bonePhysicsState byte preserves that bone's
// unregistered position, rotation and scale. Other bones, morphs, display/IK
// state, legacy rings and physics switches are never written.
//
// The enclosing record retains the scene/model and validates all native key
// and side-table inputs. Prepare the scale AuthoringRestore first: its storage
// lease must cover this participant if a target scale needs new pose storage.
// After validating ALL participants, publish native keys and side authoring
// before this participant. IsCurrent checks frame, pose and selector metadata,
// never the key pool or authoring revision that those earlier writes change.
// Publish is one-shot, allocation-free and must follow successful validation.
// Physics mode changes and mode-3 IK arming return MMS_E_NOT_READY; no solver
// setter or whole-model seek is called. Malformed tracks return MMS_E_ARGUMENT.
MmsResult PrepareBoneEvaluation(MMDApp&, unsigned modelSlot,
    const std::vector<unsigned>& affectedBones, const BoneEvaluationView&,
    std::unique_ptr<PreparedHistoryChange>&) noexcept;
}
#endif
