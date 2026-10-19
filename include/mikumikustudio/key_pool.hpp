#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <type_traits>
#include <unordered_map>

namespace mikumikustudio::mdl {
struct DisplayKey;

// Keep the native record layout and stable slot numbers. Capacity belongs to
// the allocation, so copied scene records resolve the same bounds. Unregistered
// arrays are borrowed legacy-sized fixtures and are never freed by growth.
template<class Key> struct KeyPoolStorage {
    inline static std::mutex mutex;
    inline static std::unordered_map<const Key*, std::size_t> capacities;
    inline static constexpr std::size_t initial =
        std::is_same_v<Key, DisplayKey> ? 1000 : 10000;
};

template<class Key> constexpr std::size_t KeyPoolLimit() {
    return (std::min)(std::size_t((std::numeric_limits<std::int32_t>::max)()),
                     (std::numeric_limits<std::size_t>::max)() / sizeof(Key));
}

template<class Key> std::size_t KeyPoolCapacity(const Key* pool) {
    if (!pool) return 0;
    std::lock_guard<std::mutex> lock(KeyPoolStorage<Key>::mutex);
    const auto& map = KeyPoolStorage<Key>::capacities;
    const auto found = map.find(pool);
    return found == map.end() ? KeyPoolStorage<Key>::initial : found->second;
}

template<class Key> Key* KeyPoolCreate(std::size_t count = KeyPoolStorage<Key>::initial) {
    static_assert(std::is_trivially_copyable_v<Key>);
    if (!count) return nullptr;
    if (count > KeyPoolLimit<Key>()) throw std::bad_alloc();
    auto* pool = static_cast<Key*>(::operator new(sizeof(Key) * count));
    std::memset(pool, 0, sizeof(Key) * count);
    try {
        std::lock_guard<std::mutex> lock(KeyPoolStorage<Key>::mutex);
        KeyPoolStorage<Key>::capacities.emplace(pool, count);
    } catch (...) { ::operator delete(pool); throw; }
    return pool;
}

// Payloads owned by DisplayKey must be released by the caller before this call.
template<class Key> void KeyPoolDestroy(Key*& pool) {
    if (!pool) return;
    {
        std::lock_guard<std::mutex> lock(KeyPoolStorage<Key>::mutex);
        KeyPoolStorage<Key>::capacities.erase(pool);
    }
    ::operator delete(pool);
    pool = nullptr;
}

template<class Key> bool KeyPoolEnsure(Key*& pool, std::uint64_t required) noexcept {
    const auto capacity = KeyPoolCapacity(pool);
    if (required <= capacity) return true;
    if (required > KeyPoolLimit<Key>()) return false;
    const auto target = static_cast<std::size_t>((std::min)(
        std::uint64_t(KeyPoolLimit<Key>()),
        (std::max)(required, std::uint64_t(capacity) + capacity / 2 + 256)));
    Key* grown = nullptr;
    try { grown = KeyPoolCreate<Key>(target); }
    catch (...) { return false; }
    if (capacity) std::memcpy(grown, pool, sizeof(Key) * capacity);
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock(KeyPoolStorage<Key>::mutex);
        owned = KeyPoolStorage<Key>::capacities.erase(pool) != 0;
    }
    Key* previous = pool;
    pool = grown;
    if (owned) ::operator delete(previous);
    return true;
}

// Replacing a borrowed fixture must not release somebody else's storage.
template<class Key> void KeyPoolReleaseOwned(Key* pool) noexcept {
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock(KeyPoolStorage<Key>::mutex);
        owned = KeyPoolStorage<Key>::capacities.erase(pool) != 0;
    }
    if (owned) ::operator delete(pool);
}

// Locate a free non-head slot, growing only when all existing slots are used.
// Failure leaves both the allocation and all records unchanged.
template<class Key> bool KeyPoolFreeSlot(Key*& pool, std::uint32_t& slot) noexcept {
    const auto capacity = KeyPoolCapacity(pool);
    std::size_t index = 1;
    while (index < capacity && pool[index].frame != 0) ++index;
    if (index >= capacity && !KeyPoolEnsure(pool, std::uint64_t(index) + 1)) return false;
    slot = static_cast<std::uint32_t>(index);
    return true;
}
} // namespace mikumikustudio::mdl
