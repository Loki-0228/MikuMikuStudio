#pragma once

#include "mikumikustudio/subrecord_layout.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace mikumikustudio::model_geometry_cache {

enum class Format : std::uint8_t { pmd, pmx };
struct Layout {
    Format format = Format::pmx;
    std::uint32_t vertexCount = 0;
    std::uint8_t additionalUv = 0;
    std::uint8_t boneIndexSize = 0;
    std::uint8_t vertexIndexSize = 0;
};
struct Statistics {
    std::uint64_t hits = 0, misses = 0, stores = 0, invalidations = 0;
    std::uint64_t evictions = 0, bypasses = 0;
    std::size_t bytes = 0, pendingBytes = 0, entries = 0;
};
Statistics GetProcessStatistics() noexcept;

class Session;
class Cache {
public:
    explicit Cache(std::size_t byteLimit = 256u * 1024 * 1024,
                   std::size_t entryByteLimit = 128u * 1024 * 1024,
                   std::size_t entryLimit = 32);
    ~Cache();
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
    Statistics GetStatistics() const noexcept;
    void Clear() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class Session;
};

// Cache snapshots own parsed CPU fields, never live model/GPU/physics pointers.
// The caller always supplies newly allocated mutable destination arrays.
// An explicitly supplied Cache must outlive its Sessions.
class Session {
public:
    Session() noexcept;
    ~Session();
    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    static Session Begin(int descriptor, Layout layout, std::int64_t vertexStart,
                         Cache* cache = nullptr, bool disabled = false) noexcept;
    bool Enabled() const noexcept;
    bool Hit() const noexcept;
    std::int64_t VertexEnd() const noexcept;
    std::int64_t IndexEnd() const noexcept;
    std::uint32_t IndexCount() const noexcept;
    bool CopyPmxVertices(mdl::PmxVertex* destination) const noexcept;
    bool CopyPmdVertices(mdl::PmdVertex* destination) const noexcept;
    bool CopyPmxIndices(std::int32_t* destination, std::uint32_t count) const noexcept;
    bool CopyPmdIndices(std::uint16_t* destination, std::uint32_t count) const noexcept;

    // Snapshot immediately after parsing geometry, before material/morph/pose
    // code mutates it. Publish only after the complete model load succeeds.
    void CapturePmx(const mdl::PmxVertex*, const std::int32_t*,
                    std::uint32_t indexCount, std::int64_t vertexEnd,
                    std::int64_t indexEnd) noexcept;
    void CapturePmd(const mdl::PmdVertex*, const std::uint16_t*,
                    std::uint32_t indexCount, std::int64_t vertexEnd,
                    std::int64_t indexEnd) noexcept;
    // Releases the source-file guard and any hit lease; copy before this call.
    void FinishGeometry() noexcept;
    void Commit() noexcept;
    void Invalidate() noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace mikumikustudio::model_geometry_cache
