#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <vector>
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Family output for one model clipboard aggregate. The participant owns only
// its key pool, authoring and derived values; the aggregate owns all marks,
// cursor, maximum frame and last-registration metadata.
struct ModelPasteFamilyResult {
    std::shared_ptr<HistoryChange> change;
    std::uint32_t maxFrame = 0;
    int searchCursor = 0;
    bool hasKeys = false;
    bool dataChanged = false;
    std::vector<unsigned> markedSlots;
};
MmsResult PlanModelKeyPaste(MMDApp&, unsigned modelSlot,
    std::shared_ptr<HistoryChange>&) noexcept;
}
#endif
