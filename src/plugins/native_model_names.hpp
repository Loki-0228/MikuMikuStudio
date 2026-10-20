#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <string>
#include <vector>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
struct ModelNamesDraft {
    std::wstring model, comment;
    std::vector<std::wstring> bones, morphs, groups;
};
// Menu 259 uses private text until OK. Raw fixed-width mirrors, optional owned
// Unicode strings, group sidecars and frame-row names are one publication.
class NativeModelNamesPlan {
    struct State;
    std::shared_ptr<State> state_;
public:
    NativeModelNamesPlan() noexcept = default;
    NativeModelNamesPlan(const NativeModelNamesPlan&) = delete;
    NativeModelNamesPlan& operator=(const NativeModelNamesPlan&) = delete;
    MmsResult Begin(MMDApp&, unsigned slot) noexcept;
    MmsResult Read(ModelNamesDraft&) const noexcept;
    MmsResult Set(const ModelNamesDraft&) noexcept;
    MmsResult Take(std::shared_ptr<HistoryChange>&) noexcept;
};
bool TryEditBoundModelNames(MMDApp*, void* ownerWindow) noexcept;
// A UI-thread refresh hint only; unrelated to object or document identity.
std::uint64_t NativeModelNamesUiRevision() noexcept;
}
#endif
