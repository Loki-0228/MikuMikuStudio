#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
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
// Production register-button route under the explicit migration binding only.
// True also covers rejection/no-op: never fall through to the old ring.
bool TryRegisterBoundBones(MMDApp*) noexcept;
}
#endif
