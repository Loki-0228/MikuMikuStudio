#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio {class MMDApp;}
namespace mikumikustudio::plugins {
class NativeAudioPlan {
    struct State;std::shared_ptr<State> state_;
public:
    MmsResult Begin(MMDApp&) noexcept;
    MmsResult Read(const wchar_t* source) noexcept;
    MmsResult Clear() noexcept;
    MmsResult Take(std::shared_ptr<HistoryChange>&) noexcept;
};
MmsResult ImportBoundAudio(MMDApp&,const wchar_t*,bool& changed) noexcept;
bool TryPickBoundAudio(MMDApp*) noexcept;
bool TryLoadBoundAudio(MMDApp*,const wchar_t*,bool rememberDirectory=false) noexcept;
// Consumes bound requests, including refusals. Returns false for legacy mode.
bool TryClearBoundAudio(MMDApp*) noexcept;
// Real fallback for a separately registered remove-audio menu command.
void ClearLegacyAudio(MMDApp*) noexcept;
// Called after common-history publication, outside the no-allocation phase.
// False keeps its pending redraw obligation for the next UI refresh.
bool RefreshBoundAudioUi(MMDApp&) noexcept;
}
#endif
