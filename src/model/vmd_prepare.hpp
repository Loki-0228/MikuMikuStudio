#pragma once

#include "mikumikustudio/parallel_prepare_policy.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <thread>
#include <type_traits>
#include <xmmintrin.h>

namespace mikumikustudio::vmd_read {

// A validated fixed-width section can be decoded into disjoint output slots.
// Keep workers short-lived and avoid startup overhead for small motions.
template<class Decode>
bool PrepareRecords(std::size_t count, Decode decode) {
    static_assert(std::is_nothrow_invocable_r_v<bool, Decode, std::size_t>);
    constexpr std::size_t minimumRecordsPerThread = 8192;
    constexpr std::size_t maximumThreads = 3;
    auto run = [&](std::size_t first, std::size_t end) noexcept {
        for (std::size_t i = first; i < end; ++i)
            if (!decode(i)) return false;
        return true;
    };

    if (count < 2 * minimumRecordsPerThread || parallel_prepare_policy::Disabled())
        return run(0, count);
    const auto threads = (std::min)({count / minimumRecordsPerThread, maximumThreads,
                                     std::size_t(std::thread::hardware_concurrency())});
    if (threads < 2) return run(0, count);

    std::array<std::thread, maximumThreads - 1> workers;
    std::array<bool, maximumThreads - 1> succeeded{};
    const auto recordsPerThread = count / threads;
    const auto floatingPointControl = _mm_getcsr();
    std::size_t started = 0;
    try {
        for (; started + 1 < threads; ++started) {
            workers[started] = std::thread([&, slot = started] {
                _mm_setcsr(floatingPointControl);
                succeeded[slot] = run(slot * recordsPerThread, (slot + 1) * recordsPerThread);
            });
        }
    } catch (...) {
        // The caller takes every range that could not start on a worker.
    }
    bool valid = run(started * recordsPerThread, count);
    for (std::size_t i = 0; i < started; ++i) {
        workers[i].join();
        valid = succeeded[i] && valid;
    }
    return valid;
}

} // namespace mikumikustudio::vmd_read
