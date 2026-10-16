#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
class DisplayKeyDeletion;
struct BoneRegistrationOptions {
    int frame = 0;
    int physicsMode = 2;
    bool automaticInterpolation = false;
    bool physicsEnabled = false;
};
// Builds both directions entirely in private storage. Null output on success
// means no authored change. Only owned bone pools and unchanged solver switches
// are supported. The caller budgets resident objects separately from the record.
// Planning and the first Prepare must run in one serialized editing scope,
// without intervening authoring changes or window-message dispatch. Subsequent
// undo/redo prepares validate the retained record against the current scene.
MmsResult PlanBoneRegistration(MMDApp&, unsigned modelSlot,
    const BoneRegistrationOptions&, std::shared_ptr<HistoryChange>&) noexcept;
// Bone deletion. Auxiliary marks reject unless a model-key aggregate explicitly
// owns the other families. An optional display record provides target parent
// windows; publish this bone participant before that display participant. The
// optional keyboard fallback selects current-frame keys privately only when
// none of the three families has an explicit mark; frame zero is not implied.
MmsResult PlanBoneKeyDeletion(MMDApp&, unsigned modelSlot, bool automaticInterpolation,
    std::shared_ptr<HistoryChange>&, bool selectedAtCurrentFrame = false,
    bool allowOtherMarks = false, std::shared_ptr<DisplayKeyDeletion> display = {}) noexcept;
struct BoneRangeScaleOptions {
    std::uint32_t start = 0, end = 0;
    double scale = 1;
    bool automaticInterpolation = false;
};
// Uses native slot-order reinsertion/collision rules. Successful edits include
// clearing the other key families' selection flags, without changing payloads.
// Requires start < end and a finite positive scale. Out-of-range frames reject
// the whole plan; an identity mapping after truncation preserves all selection.
// Fixed-axis correction needs the same optional D3DX math as the old registrar.
MmsResult PlanBoneRangeScale(MMDApp&, unsigned modelSlot, const BoneRangeScaleOptions&,
    std::shared_ptr<HistoryChange>&) noexcept;
// Production register-button route under the explicit migration binding only.
// True also covers rejection/no-op: never fall through to the old ring.
bool TryRegisterBoundBones(MMDApp*) noexcept;
}
#endif
