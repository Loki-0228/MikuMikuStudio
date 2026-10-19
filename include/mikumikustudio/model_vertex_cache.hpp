#pragma once
#include <cstddef>
#include <cstdint>

namespace mikumikustudio {
class MMDApp;
namespace model_vertex_cache {
// Reuse only completed model buffers. Effects and temporal history still run.
struct OutlineVertex {
    float position[3], normal[3], edgeScale;
    // High bit selects the fixed inward edge; remaining bits are material ID.
    std::uint32_t material;
};
using OutlineUpdate = bool (*)(MMDApp*, unsigned char*, float, const OutlineVertex*, std::size_t);
bool Reuse(MMDApp*, unsigned char* model, float outlineDistance, OutlineUpdate = nullptr) noexcept;
// Called while a successful CPU update still has its output mapped. Never
// read back GPU output or keep snapshots of continuously changing poses.
void RememberCpuBody(unsigned char* model, const void* vertices, std::size_t count, std::size_t stride) noexcept;
void Remember(MMDApp*, unsigned char* model, float outlineDistance) noexcept;
void Invalidate(unsigned char* model) noexcept;
void Forget(unsigned char* model) noexcept;
void Clear() noexcept;
// Loader-owned geometry/morph definitions change only through host writers.
// Those writers must Invalidate before changing them. Foreign models retain
// byte-for-byte snapshots; never infer ownership from their address or name.
void RegisterLoaded(unsigned char* model) noexcept;
struct Stats {
    std::uint64_t hits = 0, misses = 0, stores = 0;
    std::uint64_t outlineHits = 0;
    std::size_t bytes = 0, cpuBytes = 0;
};
Stats GetStats() noexcept;
// Diagnostic seam, never exposed as a user setting or saved in projects.
void SetEnabledForTests(bool enabled) noexcept;
void SetOutlineEnabledForTests(bool enabled) noexcept;
}
}
