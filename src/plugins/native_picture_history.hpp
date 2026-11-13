#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
class NativePicturePlan final {
    struct State;
    std::shared_ptr<State> state_;
public:
    MmsResult Begin(MMDApp&) noexcept;
    MmsResult Read(const wchar_t*) noexcept;
    MmsResult Clear() noexcept;
    MmsResult Visibility(bool) noexcept;
    MmsResult Take(std::shared_ptr<HistoryChange>&) noexcept;
};
MmsResult ImportBoundPicture(MMDApp&, const wchar_t*, bool& changed) noexcept;
MmsResult ClearBoundPicture(MMDApp&, bool& changed) noexcept;
MmsResult SetBoundPictureVisibility(MMDApp&, bool visible, bool& changed) noexcept;
// Bound requests are consumed even on failure; false means no history binding.
bool TryPickBoundPicture(MMDApp*) noexcept;
bool TryLoadBoundPicture(MMDApp*, const wchar_t*) noexcept;
bool TryToggleBoundPicture(MMDApp*) noexcept;
bool TryClearBoundPicture(MMDApp*) noexcept;
bool RefreshBoundPictureUi(MMDApp&) noexcept;
void ClearLegacyPicture(MMDApp*) noexcept;
}
#endif
