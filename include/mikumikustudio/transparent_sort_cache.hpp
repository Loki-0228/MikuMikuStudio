#pragma once
#include <cstddef>
#include <cstdint>
namespace mikumikustudio {
class MMDApp;
namespace transparent_sort_cache {
struct Statistics {
    std::uint64_t sourceHits=0, sourceMisses=0, planHits=0, planMisses=0;
    std::uint64_t objectHits=0, objectMisses=0;
    std::uint64_t gpuHits=0, uploads=0, allocations=0;
    std::size_t sourceBytes=0, gpuBytes=0;
};
Statistics GetStatistics() noexcept;
// Releases default-pool buffers before Reset and when the owning app closes.
void Release(MMDApp* app) noexcept;
void SetEnabledForTests(bool enabled) noexcept;
}
}
