#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace mikumikustudio {
// Compact only pools whose live indices exceed the original format. Existing
// files keep their slot numbers; an expanded but mostly empty pool can still
// be saved as a legacy PMM. This mapping never modifies the running scene.
template<class Key> class PmmKeyMap {
public:
    PmmKeyMap(const Key* keys, std::size_t dense, std::size_t capacity,
              std::size_t original) {
        std::size_t used = dense;
        bool high = dense > original;
        for (std::size_t i = dense; keys && i < capacity; ++i) {
            if (!keys[i].frame) continue;
            ++used; high |= i >= original;
        }
        if (!high || used > original) return;
        map_.resize(capacity, UINT32_MAX);
        for (std::size_t i = 0; i < original && i < capacity; ++i)
            if (i < dense || keys[i].frame) map_[i] = static_cast<std::uint32_t>(i);
        std::size_t free = dense;
        for (std::size_t i = original; i < capacity; ++i) {
            if (!keys[i].frame) continue;
            while (free < original && map_[free] != UINT32_MAX) ++free;
            if (free == original) throw std::runtime_error("PMM key mapping is full");
            map_[i] = static_cast<std::uint32_t>(free++);
        }
    }
    std::uint32_t Index(std::uint32_t index) const {
        if (map_.empty()) return index;
        if (index >= map_.size() || map_[index] == UINT32_MAX)
            throw std::runtime_error("PMM key references an empty slot");
        return map_[index];
    }
    Key Remap(const Key& source) const {
        Key result = source;
        result.previous = Index(source.previous);
        result.next = Index(source.next);
        return result;
    }
private:
    std::vector<std::uint32_t> map_;
};
}
