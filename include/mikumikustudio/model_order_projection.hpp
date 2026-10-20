#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace mikumikustudio::model_order {
// Rows contain slot + 1; zero is reserved for the camera/non-model row.
// This projection never repairs the stored bytes. The editor publishes a
// chosen permutation explicitly; readouts can display an undone damaged order.
template<std::size_t Capacity> struct Projection {
    std::array<std::int32_t, Capacity + 1> rows{};
    std::size_t count = 0;
    bool originalValid = true;
    int Row(unsigned slot) const noexcept {
        for (std::size_t row = 1; row <= count; ++row)
            if (rows[row] == static_cast<std::int32_t>(slot + 1)) return static_cast<int>(row);
        return -1;
    }
};

// rank(slot) is -1 for an empty slot, otherwise the original unsigned byte.
// Keep the first slot with a valid old rank in that row, then fill the holes
// with all unplaced objects in slot order. Healthy orders remain identical.
template<std::size_t Capacity, class Rank> Projection<Capacity> Project(Rank rank) noexcept {
    Projection<Capacity> result;
    std::array<bool, Capacity> placed{};
    for (std::size_t slot = 0; slot < Capacity; ++slot) if (rank(slot) >= 0) ++result.count;
    for (std::size_t slot = 0; slot < Capacity; ++slot) {
        const int value = rank(slot);
        if (value < 0) continue;
        if (value > 0 && static_cast<std::size_t>(value) <= result.count && !result.rows[value]) {
            result.rows[value] = static_cast<std::int32_t>(slot + 1); placed[slot] = true;
        } else result.originalValid = false;
    }
    std::size_t row = 1;
    for (std::size_t slot = 0; slot < Capacity; ++slot) if (rank(slot) >= 0 && !placed[slot]) {
        while (row <= result.count && result.rows[row]) ++row;
        result.rows[row] = static_cast<std::int32_t>(slot + 1);
    }
    return result;
}

// Managed model combos mark row zero with item data -1 and every model row
// with its slot. Validate the whole mapping before trusting even slot zero.
// Older healthy combos have default-zero item data and use their native ranks.
// An incomplete/stale managed combo, or an unmapped damaged order, is refused.
template<std::size_t Capacity, class Item> int Resolve(const Projection<Capacity>& order,
    int row, std::int64_t itemCount, Item item) noexcept {
    if (row <= 0 || static_cast<std::size_t>(row) > order.count) return -1;
    if (itemCount != static_cast<std::int64_t>(order.count + 1)) return -1;
    if (item(0) == -1) {
        std::array<bool, Capacity> used{};
        for (std::size_t i = 1; i <= order.count; ++i) {
            const auto slot = item(i);
            if (slot < 0 || static_cast<std::uint64_t>(slot) >= Capacity || used[slot] || order.Row(static_cast<unsigned>(slot)) < 1) return -1;
            used[slot] = true;
        }
        return static_cast<int>(item(row));
    } else if (!order.originalValid) return -1;
    return order.rows[row] - 1;
}

template<std::size_t Capacity, class Item> int FindRow(const Projection<Capacity>& order,
    unsigned slot, std::int64_t itemCount, Item item) noexcept {
    if (!order.count || Resolve(order, 1, itemCount, item) < 0) return -1;
    if (item(0) != -1) return order.Row(slot);
    for (std::size_t row = 1; row <= order.count; ++row)
        if (item(row) == slot) return static_cast<int>(row);
    return -1;
}
}
