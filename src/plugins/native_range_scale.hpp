#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <vector>
namespace mikudancestudio::plugins {
// Private family output for an atomic model range operation. Family records
// own only their keys and derived values; the aggregate publishes the shared
// cursor, maximum, last-registration frame and final selection marks once.
struct RangeScaleResult {
    std::shared_ptr<HistoryChange> change;
    std::uint32_t maxFrame = 0;
    int searchCursor = 0;
    bool hasKeys = false;
    bool moved = false;
    std::vector<unsigned> markedSlots;
};
}
#endif
