#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Parsed values for the seven numeric controls committed by command 487.
// Rotation is in radians. The UI reads at most seven text bytes, as the native
// control path does, and converts degrees with 3.141592025756836 / 180.
// Reading controls must not call CommitEditControl or refresh live pose first.
struct AccessoryRegistrationInput {
    float position[3]{};
    float rotation[3]{};
    float scale = 1;
};
// Plans input pose, one accessory key, global key selection and last frame
// together. Null success is a no-op. The first publication preserves the typed
// pose; history traversal samples only this accessory at its current playhead.
// Pristine curves and world/bone attachment are retained. No UI or old ring is
// called. The caller publishes UI feedback only after a successful commit.
// The app outlives the record; resident resource budgets belong to the scene.
MmsResult PlanAccessoryRegistration(MMDApp&, unsigned slot,
    const AccessoryRegistrationInput&, std::shared_ptr<HistoryChange>&) noexcept;
}
#endif
