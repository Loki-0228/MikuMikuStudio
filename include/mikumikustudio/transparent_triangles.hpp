#pragma once

#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mikumikustudio {
// Cache only exact inputs to the ordering operation, not D3D/model lifetime.
// Geometry is read again on every call, including effect-modified vertices.
struct TransparentTriangleOrder {
    std::vector<std::uint32_t> source, keys, sorted;
};

// The render thread creates entries before dispatching material workers.
// Each worker then exclusively owns one material's entry for this call.
struct TransparentSortFrameCache {
    struct Entry {
        const void* vertices;
        const void* indices;
        UINT first;
        UINT count;
        TransparentTriangleOrder order;
    };
    std::vector<Entry> entries;
    std::size_t bytes = 0;
    static constexpr std::size_t missing = static_cast<std::size_t>(-1);
    TransparentSortFrameCache* previous;
    inline static thread_local TransparentSortFrameCache* active = nullptr;
    explicit TransparentSortFrameCache(bool enabled = true) : previous(active) {
        active = enabled ? this : nullptr;
    }
    ~TransparentSortFrameCache() { active = previous; }
    TransparentSortFrameCache(const TransparentSortFrameCache&) = delete;
    TransparentSortFrameCache& operator=(const TransparentSortFrameCache&) = delete;
    std::size_t EntryFor(const void* vertices, const void* indices, UINT first, UINT count) {
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].vertices == vertices && entries[i].indices == indices && entries[i].first == first)
                return entries[i].count == count ? i : missing;
        const std::size_t needed = (std::size_t(count) * 2 + count / 3) * sizeof(std::uint32_t);
        // Changed material ranges and scenes beyond this budget use the full
        // sorter. Nothing is retained across frames or scene replacement.
        if (needed > 32 * 1024 * 1024 - bytes) return missing;
        bytes += needed;
        entries.push_back({vertices, indices, first, count, {}});
        return entries.size() - 1;
    }
};

// Keep one indexed draw per material (MME may replace that draw). Ordering its
// triangles back to front lets alpha blend against the surface behind it while
// retaining the depth writes required by MMD's later outline pass.
template<class Index>
bool SortTransparentTriangles(const void* vertices, UINT stride, UINT vertexCount,
                              Index* indices, UINT indexCount,
                              const D3DMATRIX& worldView,
                              TransparentTriangleOrder* cache = nullptr) {
    if (vertices == nullptr || indices == nullptr || stride < 3 * sizeof(float) ||
        indexCount % 3 != 0)
        return false;
    struct Triangle { std::uint32_t source; std::uint32_t key; };
    // Scratch owns no model or device pointers. Reuse capacity across materials
    // and frames, and keep separate storage for each calling thread/index type.
    thread_local std::vector<Triangle> triangles;
    thread_local std::vector<Triangle> scratch;
    thread_local std::vector<Index> sortedIndices;
    triangles.clear();
    triangles.reserve(indexCount / 3);
    for (UINT i = 0; i < indexCount; i += 3) {
        Triangle triangle{i, 0};
        float triangleDepth = 0;
        for (UINT corner = 0; corner < 3; ++corner) {
            const auto index = indices[i + corner];
            if (static_cast<UINT>(index) >= vertexCount)
                return false;
            const auto* p = reinterpret_cast<const float*>(
                static_cast<const unsigned char*>(vertices) +
                static_cast<std::size_t>(index) * stride);
            const float depth = p[0] * worldView._13 + p[1] * worldView._23 +
                p[2] * worldView._33 + worldView._43;
            if (!std::isfinite(depth))
                return false;
            triangleDepth += depth / 3.0f;
        }
        // IEEE float order, descending; canonicalize signed zero so ties retain
        // the original triangle order, just as stable_sort(depth > depth) did.
        float depth = triangleDepth == 0.0f ? 0.0f : triangleDepth;
        std::uint32_t bits;
        std::memcpy(&bits, &depth, sizeof bits);
        triangle.key = (bits & 0x80000000u) ? bits : ~bits ^ 0x80000000u;
        triangles.push_back(triangle);
    }
    if (cache) {
        const bool same = cache->source.size() == indexCount &&
            cache->keys.size() == triangles.size() && cache->sorted.size() == indexCount &&
            std::equal(cache->source.begin(), cache->source.end(), indices) &&
            std::equal(cache->keys.begin(), cache->keys.end(), triangles.begin(),
                       [](std::uint32_t key, const Triangle& triangle) { return key == triangle.key; });
        if (same) {
            std::copy(cache->sorted.begin(), cache->sorted.end(), indices);
            return true;
        }
        cache->source.assign(indices, indices + indexCount);
        cache->keys.resize(triangles.size());
        for (std::size_t i = 0; i < triangles.size(); ++i) cache->keys[i] = triangles[i].key;
        cache->sorted.clear();
    }
    if (triangles.size() < 256) {
        std::stable_sort(triangles.begin(), triangles.end(),
            [](const Triangle& a, const Triangle& b) { return a.key < b.key; });
    } else {
        // Three stable radix passes over compact key/source pairs. Keep the
        // triangle payload out of the passes to reduce memory traffic.
        scratch.resize(triangles.size());
        for (unsigned shift = 0; shift < 32; shift += 11) {
            std::size_t offsets[2048]{};
            for (const auto& triangle : triangles)
                ++offsets[(triangle.key >> shift) & 2047];
            std::size_t sum = 0;
            for (auto& offset : offsets) {
                const auto count = offset; offset = sum; sum += count;
            }
            for (const auto& triangle : triangles)
                scratch[offsets[(triangle.key >> shift) & 2047]++] = triangle;
            triangles.swap(scratch);
        }
    }
    UINT i = 0;
    sortedIndices.resize(indexCount);
    for (const auto& triangle : triangles)
        for (UINT corner = 0; corner < 3; ++corner)
            sortedIndices[i++] = indices[triangle.source + corner];
    std::copy(sortedIndices.begin(), sortedIndices.end(), indices);
    if (cache) cache->sorted.assign(sortedIndices.begin(), sortedIndices.end());
    return true;
}
}
