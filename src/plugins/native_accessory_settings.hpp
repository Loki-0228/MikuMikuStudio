#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Private metadata only. Capture before opening the modal, read its scratch
// list before closing, then consume the immutable change after it has closed.
class AccessorySettingsPlan {
    struct State;
    std::shared_ptr<State> state_;
public:
    MmsResult Begin(MMDApp&) noexcept;
    MmsResult ReadDialog(HWND) noexcept;
    MmsResult Take(std::shared_ptr<HistoryChange>&) noexcept;
};
}
#endif
