#pragma once
#include "mikudancestudio/build_profile.hpp"
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#if !MMD_ORIGINAL_PROFILE
#include <map>
#include <mutex>
#endif

namespace mikudancestudio::undo_storage {
// UndoRecord keeps its native raw-pointer ABI. Capacity is the allocation's
// requested byte count, including unused snapshot slots, never its dirty count.
struct Allocation {
    std::size_t bytes = 0;
    const void* registryIdentity = nullptr;
    std::size_t registryBytes = 0;
};
namespace detail {
enum class Family { New, Malloc };
#if !MMD_ORIGINAL_PROFILE
struct Entry { std::size_t bytes; Family family; };
inline std::mutex mutex;
inline std::map<const void*, Entry> capacities;
inline void Remember(void* pointer, std::size_t bytes, Family family) {
    if (!pointer) return;
    std::lock_guard<std::mutex> lock(mutex);
    capacities.insert_or_assign(pointer, Entry{bytes, family});
}
inline Family Take(void* pointer, Family fallback) noexcept {
    decltype(capacities)::node_type removed;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = capacities.find(pointer);
        if (found == capacities.end()) return fallback;
        fallback = found->second.family;
        removed = capacities.extract(found);
    }
    return fallback; // Registry node is released after unlocking.
}
#else
inline void Remember(void*, std::size_t, Family) {}
inline Family Take(void*, Family fallback) noexcept { return fallback; }
#endif
inline void Release(void* pointer, Family fallback) noexcept {
    if (!pointer) return;
    if (Take(pointer, fallback) == Family::New) ::operator delete(pointer);
    else std::free(pointer);
}
}
inline bool Find(const void* pointer, Allocation& out) noexcept {
#if !MMD_ORIGINAL_PROFILE
    if (!pointer) return false;
    std::lock_guard<std::mutex> lock(detail::mutex);
    const auto found = detail::capacities.find(pointer);
    if (found == detail::capacities.end()) return false;
    // Conservative tree-node overhead, following other native registries.
    out = {found->second.bytes, &*found, sizeof(*found) + 4 * sizeof(void*)};
    return true;
#else
    (void)pointer; (void)out; return false;
#endif
}
inline std::size_t Count() noexcept {
#if !MMD_ORIGINAL_PROFILE
    std::lock_guard<std::mutex> lock(detail::mutex);
    return detail::capacities.size();
#else
    return 0;
#endif
}
inline void* Allocate(std::size_t bytes) {
    void* pointer = ::operator new(bytes);
    try { detail::Remember(pointer, bytes, detail::Family::New); }
    catch (...) { ::operator delete(pointer); throw; }
    return pointer;
}
inline void* Malloc(std::size_t bytes) noexcept {
    void* pointer = std::malloc(bytes);
    try { detail::Remember(pointer, bytes, detail::Family::Malloc); }
    catch (...) { std::free(pointer); return nullptr; }
    return pointer;
}
inline void* Calloc(std::size_t count, std::size_t stride) noexcept {
    if (stride && count > (std::numeric_limits<std::size_t>::max)() / stride) return nullptr;
    void* pointer = std::calloc(count, stride);
    try { detail::Remember(pointer, count * stride, detail::Family::Malloc); }
    catch (...) { std::free(pointer); return nullptr; }
    return pointer;
}
// These older writers have no nullable-allocation path. Registration failure
// must follow their throwing allocation path instead of a null memcpy.
inline void* MallocOrThrow(std::size_t bytes) {
    void* pointer = Malloc(bytes);
#if !MMD_ORIGINAL_PROFILE
    if (!pointer) throw std::bad_alloc();
#endif
    return pointer;
}
inline void Free(void* pointer) noexcept { detail::Release(pointer, detail::Family::Malloc); }
inline void Delete(void* pointer) noexcept { detail::Release(pointer, detail::Family::New); }
}
