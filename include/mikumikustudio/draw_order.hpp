#pragma once

#include <array>
#include <cstddef>

namespace mikumikustudio {
template<class Pointer>
struct DrawOrderEntry {
    Pointer model = nullptr;
    int slot = -1;
};

// Build once per traversal, after the caller has resolved native/plugin objects.
// Preserve the old nested scan's first occupied slot for duplicate orders.
// Slot capacity and order capacity differ for models: Count slots use orders
// 1..Count, while order zero remains accepted for older scenes. Accessories
// retain Count zero-based orders by using the default OrderCount.
template<std::size_t Count, std::size_t OrderCount = Count, class ResolveSlot, class ReadOrder>
auto BuildDrawOrder(ResolveSlot resolve, ReadOrder readOrder) {
    std::array<DrawOrderEntry<decltype(resolve(0))>, OrderCount> entries{};
    for (std::size_t slot = 0; slot < Count; ++slot) {
        auto* object = resolve(static_cast<int>(slot));
        if (!object) continue;
        const int order = readOrder(object);
        if (order < 0 || static_cast<std::size_t>(order) >= OrderCount || entries[order].model)
            continue;
        entries[order] = {object, static_cast<int>(slot)};
    }
    return entries;
}
} // namespace mikumikustudio
