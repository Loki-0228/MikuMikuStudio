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
template<std::size_t Count, class ResolveSlot, class ReadOrder>
auto BuildDrawOrder(ResolveSlot resolve, ReadOrder readOrder) {
    std::array<DrawOrderEntry<decltype(resolve(0))>, Count> entries{};
    for (std::size_t slot = 0; slot < Count; ++slot) {
        auto* object = resolve(static_cast<int>(slot));
        if (!object) continue;
        const int order = readOrder(object);
        if (order < 0 || static_cast<std::size_t>(order) >= Count || entries[order].model)
            continue;
        entries[order] = {object, static_cast<int>(slot)};
    }
    return entries;
}
} // namespace mikumikustudio
