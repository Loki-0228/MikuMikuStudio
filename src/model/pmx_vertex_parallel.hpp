#pragma once
// The calling thread alone consumes the reader. Workers expand owned batches
// into disjoint vertex ranges; every exit stops and joins them before
// returning. Cancellation is polled while records are filled, so a cancelled
// result leaves the candidate destination contents unspecified.
#include "pmx_vertex_prepare.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <xmmintrin.h>

namespace mikumikustudio::pmx_prepare {
constexpr std::size_t kBatchVertices = 8192;
constexpr std::size_t kBatchCount = 3;
constexpr std::size_t kMinimumParallelVertices = 131072;
struct Statistics {
    std::size_t vertices = 0, batches = 0, workers = 0, temporaryBytes = 0;
};
enum class Result { complete, truncated, cancelled, unavailable };

// Allocations and thread startup finish before the first read. unavailable
// therefore guarantees that the original reader and destination are unchanged.
// A partial startup uses the workers already started. With no workers, the
// caller decodes the same prepared batches synchronously.
template<class Reader>
Result ReadVertices(Reader& reader, std::size_t count,
                    std::uint8_t additionalUv, std::uint8_t boneIndexSize,
                    mdl::PmxVertex* output, unsigned requestedWorkers,
                    Statistics* statistics = nullptr,
                    const std::atomic<bool>* cancelled = nullptr) {
    if (additionalUv > 4 || (count && !output))
        return Result::unavailable;
    if (statistics)
        *statistics = {};
    if (!count)
        return Result::complete;

    struct Batch {
        std::unique_ptr<unsigned char[]> bytes;
        std::array<std::uint32_t, kBatchVertices> offsets;
        mdl::PmxVertex* output = nullptr;
        std::size_t count = 0;
        bool busy = false;
    };
    // Keep the offset tables off the already deep GUI loading stack.
    std::unique_ptr<Batch[]> batches;
    try {
        batches.reset(new Batch[kBatchCount]);
        for (std::size_t i = 0; i < kBatchCount; ++i)
            batches[i].bytes.reset(new unsigned char[kBatchVertices * kMaxRecordBytes]);
    } catch (...) {
        return Result::unavailable;
    }
    const auto decode = [&](Batch& batch) noexcept {
        for (std::size_t i = 0; i < batch.count; ++i) {
            MemoryReader input(batch.bytes.get() + batch.offsets[i]);
            DecodeVertex(input, additionalUv, boneIndexSize, batch.output[i]);
        }
    };

    struct Queue {
        std::mutex mutex;
        std::condition_variable condition;
        std::array<std::size_t, kBatchCount> slots{};
        std::size_t first = 0;
        std::size_t count = 0;
        bool stopping = false;
    } queue;
    std::array<std::thread, 2> workers;
    unsigned workerCount = 0;
    // D3D9 and callers can change rounding/denormal handling on the loading
    // thread. SDEF arithmetic must use that same SSE state in each worker.
    const unsigned floatingPointControl = _mm_getcsr();
    const unsigned limit = count >= kMinimumParallelVertices
                               ? std::min(2u, requestedWorkers) : 0;
    try {
        for (; workerCount < limit; ++workerCount) {
            workers[workerCount] = std::thread([&]() {
                _mm_setcsr(floatingPointControl);
                for (;;) {
                    std::size_t slot;
                    {
                        std::unique_lock<std::mutex> lock(queue.mutex);
                        queue.condition.wait(lock, [&]() {
                            return queue.stopping || queue.count != 0;
                        });
                        if (!queue.count)
                            return;
                        slot = queue.slots[queue.first];
                        queue.first = (queue.first + 1) % kBatchCount;
                        --queue.count;
                    }
                    decode(batches[slot]);
                    {
                        std::lock_guard<std::mutex> lock(queue.mutex);
                        batches[slot].busy = false;
                    }
                    queue.condition.notify_all();
                }
            });
        }
    } catch (...) {
        // Any successfully started worker can still handle these batches.
        // Zero workers uses the same decode function synchronously.
    }
    struct Join {
        Queue& queue;
        std::array<std::thread, 2>& workers;
        bool finished = false;
        Join(Queue& queue_, std::array<std::thread, 2>& workers_)
            : queue(queue_), workers(workers_) {}
        // Stop and join once. On early returns or reader exceptions the
        // destructor performs this work; after explicit Finish it is a no-op.
        void Finish() {
            if (finished)
                return;
            finished = true;
            {
                std::lock_guard<std::mutex> lock(queue.mutex);
                queue.stopping = true;
            }
            queue.condition.notify_all();
            for (auto& worker : workers)
                if (worker.joinable())
                    worker.join();
        }
        ~Join() { Finish(); }
    } join(queue, workers);
    if (statistics) {
        statistics->workers = workerCount;
        statistics->temporaryBytes =
            kBatchCount * (kBatchVertices * kMaxRecordBytes
                           + sizeof(Batch::offsets));
    }

    const unsigned prefixBytes = 32 + 16 * additionalUv + 1;
    std::size_t completed = 0;
    std::size_t slot = 0;
    while (completed != count) {
        if (cancelled && cancelled->load(std::memory_order_relaxed))
            return Result::cancelled;
        auto& batch = batches[slot];
        if (workerCount) {
            std::unique_lock<std::mutex> lock(queue.mutex);
            queue.condition.wait(lock, [&]() { return !batch.busy; });
        }
        batch.output = output + completed;
        batch.count = std::min(kBatchVertices, count - completed);
        std::size_t offset = 0;
        for (std::size_t i = 0; i < batch.count; ++i) {
            // Poll before every record; a batch observed cancelled here is
            // never submitted, so statistics cannot count a partial batch.
            if (cancelled && cancelled->load(std::memory_order_relaxed))
                return Result::cancelled;
            batch.offsets[i] = static_cast<std::uint32_t>(offset);
            unsigned char* record = batch.bytes.get() + offset;
            if (reader.Read(record, prefixBytes) != static_cast<int>(prefixBytes))
                return Result::truncated;
            const unsigned tail = TailBytes(record[prefixBytes - 1], boneIndexSize);
            if (reader.Read(record + prefixBytes, tail) != static_cast<int>(tail))
                return Result::truncated;
            offset += prefixBytes + tail;
        }
        if (cancelled && cancelled->load(std::memory_order_relaxed))
            return Result::cancelled;
        if (workerCount) {
            {
                std::lock_guard<std::mutex> lock(queue.mutex);
                batch.busy = true;
                queue.slots[(queue.first + queue.count) % kBatchCount] = slot;
                ++queue.count;
            }
            queue.condition.notify_one();
        } else {
            decode(batch);
        }
        completed += batch.count;
        slot = (slot + 1) % kBatchCount;
        if (statistics) {
            // Only batches whose records were fully read and submitted count.
            statistics->vertices = completed;
            ++statistics->batches;
        }
    }
    // The cancellation token can change while the caller waits for workers.
    // Poll after joining so that observed cancellation cannot become complete.
    join.Finish();
    if (cancelled && cancelled->load(std::memory_order_relaxed))
        return Result::cancelled;
    return Result::complete;
}

}  // namespace mikumikustudio::pmx_prepare
