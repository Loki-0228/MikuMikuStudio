#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include "mikumikustudio/import_texture_cache.hpp"
#include <array>
#include <string>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
using ModelToonDraft = std::array<std::wstring, 10>;
// The modal editor owns only private names/resources until one explicit OK.
// Both history directions retain their textures; neither reopens model files.
class NativeModelToonPlan {
    struct State;
    std::shared_ptr<State> state_;
public:
    NativeModelToonPlan() noexcept = default;
    NativeModelToonPlan(const NativeModelToonPlan&) = delete;
    NativeModelToonPlan& operator=(const NativeModelToonPlan&) = delete;
    MmsResult Begin(MMDApp&, unsigned slot, import_texture_cache::TextureLoader = nullptr) noexcept;
    MmsResult Read(ModelToonDraft&) const noexcept;
    MmsResult Set(const ModelToonDraft&) noexcept;
    MmsResult Take(std::shared_ptr<HistoryChange>&) noexcept;
};
bool TryEditBoundModelToon(MMDApp*, void* ownerWindow) noexcept;
}
#endif
