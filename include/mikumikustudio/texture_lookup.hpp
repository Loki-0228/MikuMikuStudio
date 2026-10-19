#pragma once

#include "mikumikustudio/d3d_wrapper.hpp"
#include <atomic>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mikumikustudio {
namespace texture_lookup_detail {
// Keep the ABI-pinned renderer unchanged. Keys own their strings so clearing a
// renderer never leaves a dangling string_view in another thread's index.
inline std::atomic<std::uint64_t> generation{1};
struct Index {
    D3DRenderer* renderer = nullptr;
    std::uint64_t generation = 0;
    std::deque<std::wstring> names;
    std::unordered_map<std::wstring_view, ResourcePoolEntry*> entries;
};
inline thread_local Index index;
}

// Call before reset/disposal/reload and after publishing a newly loaded entry.
// Resource access itself remains on the renderer's owning thread.
inline void InvalidateTextureLookup() {
    texture_lookup_detail::generation.fetch_add(1, std::memory_order_relaxed);
}

inline ResourcePoolEntry* FindTextureResource(D3DRenderer* renderer, const wchar_t* name) {
    if (!renderer || !name || !*name) return nullptr;
    auto& index = texture_lookup_detail::index;
    const auto generation = texture_lookup_detail::generation.load(std::memory_order_relaxed);
    if (index.renderer != renderer || index.generation != generation) {
        index.entries.clear();
        index.names.clear();
        for (auto& entry : renderer->resourcePool) {
            const auto* path = static_cast<const wchar_t*>(entry.heapBuffer);
            if (!path) break;
            index.names.emplace_back(path);
            // The original scan returns the first exact, case-sensitive match.
            index.entries.emplace(std::wstring_view(index.names.back()), &entry);
        }
        index.renderer = renderer;
        index.generation = generation;
    }
    const auto found = index.entries.find(std::wstring_view(name));
    return found == index.entries.end() ? nullptr : found->second;
}
} // namespace mikumikustudio
