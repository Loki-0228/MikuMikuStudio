#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "../model/texture_reload_preparation.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
struct TextureReloadLoaders {
    import_texture_cache::TextureLoader cache = nullptr;
    texture_reload::SharedToonLoader shared = nullptr;
};
// Internal host participant, not a public plugin edit service. The app,
// renderer and device identity must remain alive through plan and history.
// Source names/slot positions are immutable; later unrelated tail appends are
// preserved. Both resource directions keep real COM references, not file paths
// to reopen. Dropping a prepared plan/candidate publishes nothing.
class NativeTextureReloadPlan {
    struct State;
    std::shared_ptr<State> state_;
public:
    NativeTextureReloadPlan() noexcept = default;
    NativeTextureReloadPlan(const NativeTextureReloadPlan&) = delete;
    NativeTextureReloadPlan& operator=(const NativeTextureReloadPlan&) = delete;
    MmsResult Begin(MMDApp&, const TextureReloadLoaders& = {}) noexcept;
    MmsResult Take(std::shared_ptr<HistoryChange>&) noexcept;
};
// Full configuration owns menu 278 with or without a migration binding.
// A handled failure never falls through to destructive legacy reload.
bool TryReloadNativeTextures(MMDApp*) noexcept;
}
#endif
