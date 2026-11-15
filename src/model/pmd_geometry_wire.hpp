#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mikumikustudio::model_geometry_cache {
inline constexpr unsigned kPmdVertexWireBytes = 38;
inline constexpr unsigned kPmdVertexHostBytes = 40;
inline constexpr unsigned kPmdIndexWireBytes = 2;
inline constexpr unsigned kPmdIndexHostBytes = 4;

// PMD reads directly into native little-endian fields. Visit their original
// byte representation before any model transformation; never visit host
// padding or perform floating-point arithmetic. Batch the scattered fields
// so feeding a digest doesn't incur one call per vertex or index.
template<class Consume>
bool VisitPmdGeometryWire(std::uint32_t vertexCount, const unsigned char* vertices,
                         std::uint32_t indexCount, const unsigned char* indices,
                         Consume consume) noexcept {
    unsigned char batch[4096];
    unsigned used = 0;
    const auto append = [&](const void* bytes, unsigned count) noexcept {
        if (count > sizeof(batch) - used) {
            if (!consume(batch, used)) return false;
            used = 0;
        }
        std::memcpy(batch + used, bytes, count);
        used += count;
        return true;
    };
    const auto appendCount = [&](std::uint32_t count) noexcept {
        const unsigned char littleEndian[] = {
            static_cast<unsigned char>(count), static_cast<unsigned char>(count >> 8),
            static_cast<unsigned char>(count >> 16), static_cast<unsigned char>(count >> 24)
        };
        return append(littleEndian, sizeof(littleEndian));
    };
    if (!appendCount(vertexCount)) return false;
    for (std::uint32_t i = 0; i < vertexCount; ++i)
        if (!append(vertices + std::size_t(i) * kPmdVertexHostBytes, kPmdVertexWireBytes)) return false;
    if (!appendCount(indexCount)) return false;
    for (std::uint32_t i = 0; i < indexCount; ++i)
        if (!append(indices + std::size_t(i) * kPmdIndexHostBytes, kPmdIndexWireBytes)) return false;
    return consume(batch, used);
}
}
