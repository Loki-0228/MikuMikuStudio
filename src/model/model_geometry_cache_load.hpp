#pragma once

#include "model_geometry_cache.hpp"
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>

namespace mikumikustudio::model_geometry_cache {

struct GeometryDelete {
    template<class T> void operator()(T* value) const noexcept { ::operator delete(value); }
};

// Restore both CPU arrays before changing the parser's position or publishing
// either array to a model. Cache/allocation failures retain the ordinary path.
template<class Vertex> class HitCopy {
public:
    HitCopy(const Session& session, std::uint32_t vertexCount) noexcept {
        if (!session.Hit()) return;
        const std::uint32_t indices = session.IndexCount();
        if (vertexCount > (std::numeric_limits<std::size_t>::max)() / sizeof(Vertex) ||
            indices > (std::numeric_limits<std::uint32_t>::max)() / sizeof(std::int32_t)) return;
        try {
            if (vertexCount) {
                vertices_.reset(static_cast<Vertex*>(::operator new(sizeof(Vertex) * vertexCount)));
                // Match the respective loader's initialization, including the
                // PMX reference's single leading 40*count-byte clear.
                std::memset(vertices_.get(), 0,
                    (std::is_same_v<Vertex, mdl::PmdVertex> ? sizeof(Vertex) : 40) * vertexCount);
            }
            if (indices) indices_.reset(static_cast<std::int32_t*>(
                ::operator new(sizeof(std::int32_t) * indices)));
            if constexpr (std::is_same_v<Vertex, mdl::PmdVertex>) {
                ready_ = session.CopyPmdVertices(vertices_.get()) &&
                    session.CopyPmdIndices(reinterpret_cast<std::uint16_t*>(indices_.get()), indices);
            } else {
                static_assert(std::is_same_v<Vertex, mdl::PmxVertex>);
                ready_ = session.CopyPmxVertices(vertices_.get()) &&
                    session.CopyPmxIndices(indices_.get(), indices);
            }
        } catch (...) { ready_ = false; }
        if (!ready_) Reset();
    }
    bool Ready() const noexcept { return ready_; }
    Vertex* ReleaseVertices() noexcept { return vertices_.release(); }
    std::int32_t* ReleaseIndices() noexcept { return indices_.release(); }
    void Reset() noexcept { ready_ = false; vertices_.reset(); indices_.reset(); }
private:
    bool ready_ = false;
    std::unique_ptr<Vertex, GeometryDelete> vertices_;
    std::unique_ptr<std::int32_t, GeometryDelete> indices_;
};

}  // namespace mikumikustudio::model_geometry_cache
