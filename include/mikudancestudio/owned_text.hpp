#pragma once
#include "mikudancestudio/build_profile.hpp"
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#if !MMD_ORIGINAL_PROFILE
#include <map>
#include <mutex>
#endif

namespace mikudancestudio::owned_text {
// The native records keep their raw wchar_t* ABI. Capacity belongs to the
// allocation, not its visible length (PMX strings may contain early NULs).
// Callers must unregister before freeing or replacing an owned pointer.
struct Allocation {
    std::size_t bytes = 0;
    const void* registryIdentity = nullptr;
    std::size_t registryBytes = 0;
};
#if !MMD_ORIGINAL_PROFILE
namespace detail {
inline std::mutex mutex;
inline std::map<const wchar_t*, std::size_t> capacities;
}
inline void Remember(const wchar_t* pointer, std::size_t bytes) {
    if (!pointer) return;
    std::lock_guard<std::mutex> lock(detail::mutex);
    detail::capacities.insert_or_assign(pointer, bytes);
}
inline void Forget(const wchar_t* pointer) noexcept {
    if (!pointer) return;
    std::lock_guard<std::mutex> lock(detail::mutex);
    detail::capacities.erase(pointer);
}
// Main-thread scene ownership keeps the allocation alive through measurement.
// The returned registry identity is only a deduplication key, never dereferenced.
inline bool Find(const wchar_t* pointer, Allocation& out) noexcept {
    if (!pointer) return false;
    std::lock_guard<std::mutex> lock(detail::mutex);
    const auto found = detail::capacities.find(pointer);
    if (found == detail::capacities.end()) return false;
    out = {found->second, &*found, sizeof(*found) + 4 * sizeof(void*)};
    return true;
}
inline std::size_t Count() noexcept {
    std::lock_guard<std::mutex> lock(detail::mutex);
    return detail::capacities.size();
}
#else
inline void Remember(const wchar_t*, std::size_t) {}
inline void Forget(const wchar_t*) noexcept {}
inline bool Find(const wchar_t*, Allocation&) noexcept { return false; }
inline std::size_t Count() noexcept { return 0; }
#endif

// Preserve the loader's allocation/deallocation families. A registry failure
// cannot orphan a newly allocated buffer; caller-owned fields are untouched.
inline wchar_t* AllocateBytes(std::size_t bytes) {
    auto* pointer = static_cast<wchar_t*>(::operator new(bytes));
    try { Remember(pointer, bytes); }
    catch (...) { ::operator delete(pointer); throw; }
    return pointer;
}
inline void Delete(wchar_t* pointer) noexcept {
    Forget(pointer); ::operator delete(pointer);
}
inline void Free(wchar_t* pointer) noexcept {
    Forget(pointer); std::free(pointer);
}
struct Deleter { void operator()(wchar_t* pointer) const noexcept { Delete(pointer); } };
using Buffer = std::unique_ptr<wchar_t, Deleter>;
}
