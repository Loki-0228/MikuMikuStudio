#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

namespace mikumikustudio::plugins {
// Detached slot sets are strictly increasing. A nonnull empty set selects
// nothing; it never falls back to live timeline marks or current-frame keys.
inline bool ValidKeySelection(const std::vector<unsigned>& slots, std::size_t capacity) noexcept {
    for (std::size_t i = 0; i < slots.size(); ++i)
        if (slots[i] >= capacity || (i && slots[i - 1] >= slots[i])) return false;
    return true;
}
inline bool SelectedKeySlot(const std::vector<unsigned>& slots, unsigned slot) noexcept {
    return std::binary_search(slots.begin(), slots.end(), slot);
}
}
