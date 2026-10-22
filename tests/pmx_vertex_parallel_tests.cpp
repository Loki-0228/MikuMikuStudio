// MAINTENANCE: regression-keep; PMX CPU preparation contracts. No files,
// windows, UI input, GPU device, or production scene state are used here.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "../src/model/pmx_vertex_parallel.hpp"
#include "model_geometry_reference.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <limits>
#include <new>
#include <utility>
#include <vector>

using namespace mikumikustudio;
namespace prep = pmx_prepare;
namespace ref = geometry_test_reference;
namespace {
unsigned checks = 0;
const char* stage = "setup";
thread_local long failAfter = -1;
thread_local bool failedAllocation = false;
std::atomic<bool> observeWorkers{false};
std::atomic<DWORD> readerThread{0};
std::array<std::atomic<DWORD>, 8> observedIds{};
std::array<std::atomic<HANDLE>, 8> observedHandles{};
std::atomic<bool> observerOverflow{false}, observerFailure{false};
std::atomic<std::atomic<bool>*> cancelOnWorkerDelete{nullptr};
bool sawRealWorker = false;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL [%s] %s\n", stage, message);
        std::exit(1);
    }
}

// MSVC's std::thread destroys its C++ startup tuple on the started thread.
// Observe that ordinary delete, without wrapping or changing the launcher.
// A real kernel thread handle remaining signalled after ReadVertices returns
// proves termination; a delay or a second immediate memcpy would not do so.
void ObserveDeleteThread() noexcept {
    if (!observeWorkers.load(std::memory_order_acquire)) return;
    const DWORD id = GetCurrentThreadId();
    if (id == readerThread.load(std::memory_order_relaxed)) return;
    if (auto* cancelled = cancelOnWorkerDelete.load(std::memory_order_acquire))
        cancelled->store(true, std::memory_order_release);
    for (std::size_t i = 0; i < observedIds.size(); ++i) {
        DWORD found = observedIds[i].load(std::memory_order_acquire);
        if (found == id) return;
        if (found == 0 && observedIds[i].compare_exchange_strong(found, id)) {
            HANDLE handle = OpenThread(SYNCHRONIZE, FALSE, id);
            if (!handle) observerFailure.store(true);
            observedHandles[i].store(handle, std::memory_order_release);
            return;
        }
        if (found == id) return;
    }
    observerOverflow.store(true);
}

struct WorkerObservation {
    bool finished = false;
    WorkerObservation() {
        Check(!observeWorkers.load(), "thread observation scopes do not overlap");
        for (std::size_t i = 0; i < observedIds.size(); ++i) {
            observedIds[i].store(0); observedHandles[i].store(nullptr);
        }
        observerOverflow.store(false); observerFailure.store(false);
        readerThread.store(GetCurrentThreadId());
        observeWorkers.store(true, std::memory_order_release);
    }
    void Finish(std::size_t expected) {
        observeWorkers.store(false, std::memory_order_release);
        Check(!observerOverflow.load() && !observerFailure.load(),
              "started worker thread observation succeeded within fixed storage");
        std::size_t observed = 0;
        for (std::size_t i = 0; i < observedIds.size(); ++i) {
            if (!observedIds[i].load()) continue;
            ++observed;
            const auto handle = observedHandles[i].load(std::memory_order_acquire);
            Check(handle && WaitForSingleObject(handle, 0) == WAIT_OBJECT_0,
                  "every actually observed worker has terminated before return or exception propagation");
        }
        Check(observed == expected,
              "actual distinct started threads agree with reported worker count");
        sawRealWorker = sawRealWorker || observed != 0;
        Close();
        finished = true;
    }
    void Close() noexcept {
        observeWorkers.store(false, std::memory_order_release);
        cancelOnWorkerDelete.store(nullptr, std::memory_order_release);
        for (auto& cell : observedHandles) {
            if (HANDLE handle = cell.exchange(nullptr)) CloseHandle(handle);
        }
    }
    ~WorkerObservation() { if (!finished) Close(); }
};

struct ReadFailure { std::size_t call; };
struct Reader {
    const ref::Bytes& bytes;
    std::size_t limit, at = 0, calls = 0;
    const DWORD owner = GetCurrentThreadId();
    std::atomic<bool> wrongThread{false};
    std::atomic<bool>* cancel = nullptr;
    std::size_t cancelAtCall = 0, throwAtCall = 0;
    explicit Reader(const ref::Bytes& source, std::size_t length)
        : bytes(source), limit(std::min(length, source.size())) {}
    int Read(void* output, unsigned count) {
        if (GetCurrentThreadId() != owner) {
            wrongThread.store(true); return 0;
        }
        ++calls;
        if (throwAtCall && calls == throwAtCall) throw ReadFailure{calls};
        const auto n = std::min<std::size_t>(count, limit - at);
        if (n) std::memcpy(output, bytes.data() + at, n);
        at += n;
        if (cancel && cancelAtCall && calls == cancelAtCall)
            cancel->store(true, std::memory_order_release);
        return static_cast<int>(n);
    }
};

struct Wire {
    ref::Bytes bytes;
    std::size_t count = 0, geometryBytes = 0;
    unsigned uv = 0, boneWidth = 0;
};
constexpr unsigned char kSentinel = 0xa5;
constexpr std::size_t kTemporaryBound = 3 * 8192 * (149 + sizeof(std::uint32_t));
constexpr std::array<unsigned char, 8> kFollowingSection{0xe1,0x92,0x83,0x74,0x65,0x56,0x47,0x38};

Wire MakeWire(std::size_t count, unsigned uv, unsigned boneWidth) {
    auto f = ref::Pmx(static_cast<std::uint32_t>(count), uv, boneWidth);
    Wire result; result.count = count; result.uv = uv; result.boneWidth = boneWidth;
    result.geometryBytes = static_cast<std::size_t>(f.vertexEnd - f.vertexStart);
    f.bytes.erase(f.bytes.begin(), f.bytes.begin() + static_cast<std::size_t>(f.vertexStart));
    f.bytes.resize(result.geometryBytes);
    result.bytes = std::move(f.bytes);
    result.bytes.insert(result.bytes.end(), kFollowingSection.begin(), kFollowingSection.end());
    return result;
}

std::vector<mdl::PmxVertex> Fresh(std::size_t count) {
    std::vector<mdl::PmxVertex> result(count + 2);
    std::memset(result.data(), kSentinel, result.size() * sizeof(result[0]));
    return result;
}

std::vector<mdl::PmxVertex> Expected(const Wire& wire) {
    auto output = Fresh(wire.count);
    Reader reader(wire.bytes, wire.bytes.size());
    for (std::size_t i = 0; i < wire.count; ++i)
        Check(ref::OriginalVertex(reader, wire.uv, wire.boneWidth, output[i + 1]),
              "independent original field decoder accepts the owned complete record");
    Check(reader.at == wire.geometryBytes && !reader.wrongThread.load(),
          "independent decoder stops exactly before the following section");
    return output;
}

void Equal(const std::vector<mdl::PmxVertex>& expected,
           const std::vector<mdl::PmxVertex>& actual) {
    Check(expected.size() == actual.size(), "comparison includes both guard records");
    if (std::memcmp(expected.data(), actual.data(), actual.size() * sizeof(actual[0]))) {
        for (std::size_t i = 0; i < actual.size(); ++i)
            if (std::memcmp(&expected[i], &actual[i], sizeof(actual[i]))) {
                std::fprintf(stderr, "first unequal full PMX record (including unused fields): %zu\n", i);
                break;
            }
        Check(false, "all 188 bytes per vertex and both guards match independent old decoding");
    }
    Check(true, "all full records, untouched slots, padding and bounds match independent old decoding");
}

void BoundedStatistics(const prep::Statistics& statistics, std::size_t count,
                       unsigned requestedWorkers, bool complete) {
    const auto limit = count >= 131072 ? std::min(2u, requestedWorkers) : 0u;
    Check(statistics.workers <= limit, "reported worker count respects threshold and two-worker cap");
    Check(statistics.temporaryBytes <= kTemporaryBound,
          "reported packed batch and offset storage remains within the fixed three-batch bound");
    Check(statistics.vertices <= count && statistics.batches <= (count + 8191) / 8192,
          "statistics never publish more vertices or batches than requested");
    if (complete) {
        Check(statistics.vertices == count && statistics.batches == (count + 8191) / 8192,
              "successful statistics count exactly the fully submitted records and final batch");
        Check(count ? statistics.temporaryBytes != 0 : statistics.temporaryBytes == 0,
              "only nonempty preparation owns temporary batch storage");
    }
}

void CompleteCase(const Wire& wire, const std::vector<mdl::PmxVertex>& expected,
                  unsigned workers) {
    auto actual = Fresh(wire.count);
    Reader reader(wire.bytes, wire.bytes.size());
    prep::Statistics statistics{};
    WorkerObservation observation;
    const auto result = prep::ReadVertices(reader, wire.count,
        static_cast<std::uint8_t>(wire.uv), static_cast<std::uint8_t>(wire.boneWidth),
        actual.data() + 1, workers, &statistics);
    observation.Finish(statistics.workers);
    Check(result == prep::Result::complete, "complete wire returns complete");
    Check(!reader.wrongThread.load() && reader.at == wire.geometryBytes,
          "only the calling thread reads and following-section bytes are not consumed");
    Equal(expected, actual);
    BoundedStatistics(statistics, wire.count, workers, true);
    Check(!wire.count || reader.calls == wire.count * 2,
          "complete packed input performs only one prefix and one tail read per record");
}

void LayoutsAndBoundaries() {
    for (unsigned uv = 0; uv <= 4; ++uv) for (unsigned width : {1u,2u,4u}) {
        const auto wire = MakeWire(7, uv, width);
        const auto expected = Expected(wire);
        for (unsigned workers : {0u,1u,2u,99u}) CompleteCase(wire, expected, workers);
        Reader reader(wire.bytes, wire.bytes.size());
        auto packed = Fresh(7);
        for (std::size_t i = 0; i < 7; ++i)
            Check(prep::ReadPackedVertex(reader, static_cast<std::uint8_t>(uv),
                      static_cast<std::uint8_t>(width), packed[i + 1]),
                  "single packed helper accepts each original weight branch");
        Equal(expected, packed);
        Check(reader.at == wire.geometryBytes, "single packed helper preserves next-section offset");
        for (unsigned type = 0; type < 7; ++type)
            Check(static_cast<unsigned>(expected[type + 1].weightType) == type,
                  "fixture actually includes BDEF1/BDEF2/BDEF4/SDEF and old QDEF/unknown fallbacks");
    }
    // These nonstandard width bytes follow the existing decoder's int32
    // fallback; preparation must not invent a different interpretation.
    for (unsigned width : {0u,3u,255u}) {
        const auto wire = MakeWire(7, 4, width);
        CompleteCase(wire, Expected(wire), 2);
    }
    const std::array<std::size_t, 9> counts{0,1,8191,8192,8193,131071,131072,131073,155665};
    for (std::size_t index = 0; index < counts.size(); ++index) {
        const auto wire = MakeWire(counts[index], static_cast<unsigned>(index % 5),
                                   std::array<unsigned,3>{1,2,4}[index % 3]);
        const auto expected = Expected(wire);
        for (unsigned workers : {0u,1u,2u,99u}) CompleteCase(wire, expected, workers);
    }
    Check(sawRealWorker, "at least one large case actually executed and joined real worker threads");
}

Wire SingleRecord(const Wire& seven, unsigned type) {
    Reader reader(seven.bytes, seven.geometryBytes);
    std::size_t begin = 0, end = 0;
    for (unsigned i = 0; i <= type; ++i) {
        begin = reader.at;
        mdl::PmxVertex scratch{};
        Check(ref::OriginalVertex(reader, seven.uv, seven.boneWidth, scratch),
              "independent decoder locates each variable-size record boundary");
        end = reader.at;
    }
    Wire result; result.count = 1; result.uv = seven.uv; result.boneWidth = seven.boneWidth;
    result.bytes.assign(seven.bytes.begin() + begin, seven.bytes.begin() + end);
    result.geometryBytes = result.bytes.size();
    return result;
}

void EveryShortReadAndEarlyRejection() {
    for (unsigned uv = 0; uv <= 4; ++uv) for (unsigned width : {1u,2u,4u}) {
        const auto seven = MakeWire(7, uv, width);
        for (unsigned type = 0; type < 7; ++type) {
            const auto wire = SingleRecord(seven, type);
            for (std::size_t cut = 0; cut < wire.geometryBytes; ++cut) {
                auto actual = Fresh(1), unchanged = actual;
                Reader reader(wire.bytes, cut);
                prep::Statistics statistics{};
                Check(prep::ReadVertices(reader, 1, static_cast<std::uint8_t>(uv),
                          static_cast<std::uint8_t>(width), actual.data() + 1, 2, &statistics)
                          == prep::Result::truncated,
                      "every possible prefix or tail byte truncation returns truncated");
                Check(reader.at == cut && !reader.wrongThread.load(),
                      "short read never accesses bytes outside the reader's declared input");
                Check(statistics.vertices == 0 && statistics.batches == 0 && statistics.workers == 0,
                      "partial first record never publishes a partial batch or worker statistic");
                Equal(unchanged, actual);
                Reader packedReader(wire.bytes, cut);
                Check(!prep::ReadPackedVertex(packedReader, static_cast<std::uint8_t>(uv),
                          static_cast<std::uint8_t>(width), actual[1]),
                      "single packed helper rejects every truncated prefix and tail");
                Equal(unchanged, actual);
            }
        }
    }
    const auto wire = MakeWire(7, 4, 4);
    auto output = Fresh(7), unchanged = output;
    for (unsigned invalidUv : {5u,255u}) {
        Reader reader(wire.bytes, wire.bytes.size()); prep::Statistics statistics{91,92,93,94};
        Check(prep::ReadVertices(reader, 7, static_cast<std::uint8_t>(invalidUv), 4,
                  output.data() + 1, 2, &statistics) == prep::Result::unavailable,
              "unsupported extra UV count is rejected before any reading or writing");
        Check(!reader.calls && !reader.at, "invalid UV count does not consume the reader");
        Check(!prep::ReadPackedVertex(reader, static_cast<std::uint8_t>(invalidUv), 4, output[1])
                  && !reader.calls, "single packed helper also rejects extra UVs without reading");
        Equal(unchanged, output);
    }
    Reader nullReader(wire.bytes, wire.bytes.size());
    Check(prep::ReadVertices(nullReader, 7, 4, 4, nullptr, 2) == prep::Result::unavailable
              && !nullReader.calls && !nullReader.at,
          "nonempty null output is rejected before all reader access");
    Reader emptyReader(wire.bytes, wire.bytes.size()); prep::Statistics emptyStatistics{91,92,93,94};
    Check(prep::ReadVertices(emptyReader, 0, 4, 4, nullptr, 99, &emptyStatistics)
              == prep::Result::complete && !emptyReader.calls,
          "empty valid input accepts null output without allocating, launching or reading");
    BoundedStatistics(emptyStatistics, 0, 99, true);
}

void StableAfterReturn(const std::vector<mdl::PmxVertex>& actual) {
    const auto snapshot = actual;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    Equal(snapshot, actual);
}

void CancellationAndExceptions() {
    const auto wire = MakeWire(131079, 4, 4);
    for (const std::size_t trigger : {std::size_t{0},std::size_t{1},
                                    std::size_t{2 * (8192 + 17) - 1},wire.count * 2}) {
        auto output = Fresh(wire.count), unchanged = output;
        Reader reader(wire.bytes, wire.bytes.size());
        std::atomic<bool> cancelled{trigger == 0};
        reader.cancel = &cancelled; reader.cancelAtCall = trigger;
        prep::Statistics statistics{};
        WorkerObservation observation;
        const auto result = prep::ReadVertices(reader, wire.count, 4, 4,
            output.data() + 1, 2, &statistics, &cancelled);
        observation.Finish(statistics.workers);
        Check(result == prep::Result::cancelled && cancelled.load(),
              "initial, mid-record, mid-batch and final-record cancellation never returns success");
        Check(!reader.wrongThread.load(), "cancellable input still has one reader owner");
        Check(statistics.vertices % 8192 == 0 && statistics.batches == statistics.vertices / 8192,
              "cancelled partial batch is absent from committed batch statistics");
        BoundedStatistics(statistics, wire.count, 2, false);
        const std::size_t expectedPublished = trigger == 0 || trigger == 1 ? 0
            : trigger == wire.count * 2 ? 131072 : 8192;
        Check(statistics.vertices == expectedPublished,
              "cancellation publishes exactly the earlier complete batches");
        Check(!std::memcmp(output.data() + 1 + expectedPublished,
                          unchanged.data() + 1 + expectedPublished,
                          (wire.count + 1 - expectedPublished) * sizeof(output[0]))
                  && !std::memcmp(output.data(), unchanged.data(), sizeof(output[0])),
              "cancelled in-progress and unread vertices plus bounds retain their original bytes");
        if (trigger == 0) Check(!reader.calls && !reader.at,
                               "an already cancelled request consumes no input");
        StableAfterReturn(output);
    }
    {
        // The worker's startup tuple is destroyed only after its decode loop
        // exits. Cancellation at that ordinary deletion therefore occurs
        // after the last input/submission poll, while the caller joins. This
        // makes the final post-join cancellation check observable without a
        // sleep, scheduling guess, or injected thread-launch implementation.
        auto output = Fresh(wire.count);
        Reader reader(wire.bytes, wire.bytes.size());
        std::atomic<bool> cancelled{false}; prep::Statistics statistics{};
        WorkerObservation observation;
        cancelOnWorkerDelete.store(&cancelled, std::memory_order_release);
        const auto result = prep::ReadVertices(reader, wire.count, 4, 4,
            output.data() + 1, 2, &statistics, &cancelled);
        cancelOnWorkerDelete.store(nullptr, std::memory_order_release);
        observation.Finish(statistics.workers);
        Check(result == prep::Result::cancelled && cancelled.load()
                  && statistics.workers != 0 && reader.at == wire.geometryBytes,
              "cancellation during actual worker exit is rechecked after join before returning success");
        BoundedStatistics(statistics, wire.count, 2, true);
        Equal(Expected(wire), output);
        StableAfterReturn(output);
    }
    // Throw only after an entire real batch has been queued. Statistics and
    // actual signalled worker handles prove this is not a serial-only unwind.
    auto output = Fresh(wire.count), unchanged = output;
    Reader reader(wire.bytes, wire.bytes.size()); reader.throwAtCall = 8192 * 2 + 3;
    prep::Statistics statistics{}; bool caught = false;
    WorkerObservation observation;
    try { prep::ReadVertices(reader, wire.count, 4, 4, output.data() + 1, 2, &statistics); }
    catch (const ReadFailure& failure) { caught = failure.call == reader.throwAtCall; }
    observation.Finish(statistics.workers);
    Check(caught && statistics.workers != 0 && statistics.vertices == 8192 && statistics.batches == 1,
          "the original reader exception propagates after work starts and only one complete batch is counted");
    Check(!reader.wrongThread.load() && !std::memcmp(output.data() + 8193,
              unchanged.data() + 8193, (wire.count - 8192 + 1) * sizeof(output[0])),
          "exception does not publish the interrupted second batch or change unread records");
    StableAfterReturn(output);

    // A late short read exercises the same join guard with work in flight.
    output = Fresh(wire.count); unchanged = output;
    Reader truncated(wire.bytes, wire.geometryBytes - 1); statistics = {};
    WorkerObservation truncationObservation;
    const auto result = prep::ReadVertices(truncated, wire.count, 4, 4,
        output.data() + 1, 99, &statistics);
    truncationObservation.Finish(statistics.workers);
    Check(result == prep::Result::truncated && statistics.workers != 0
              && statistics.vertices == 131072 && statistics.batches == 16,
          "late truncation joins real workers and excludes the final incomplete batch");
    Check(!std::memcmp(output.data() + 131073, unchanged.data() + 131073,
              (wire.count - 131072 + 1) * sizeof(output[0])),
          "late truncation leaves every final partial-batch record and the trailing guard untouched");
    StableAfterReturn(output);
}

void PreparationAllocationFailures() {
    const auto wire = MakeWire(131072, 4, 4);
    const auto expected = Expected(wire);
    auto output = Fresh(wire.count), unchanged = output;
    unsigned failures = 0; bool reachedComplete = false;
    long preparationBudget = -1;
    for (long budget = 0; budget < 16; ++budget) {
        output = unchanged;
        Reader reader(wire.bytes, wire.bytes.size()); prep::Statistics statistics{};
        prep::Result result = prep::Result::unavailable;
        failedAllocation = false; failAfter = budget;
        try { result = prep::ReadVertices(reader, wire.count, 4, 4, output.data() + 1, 0, &statistics); }
        catch (...) { failAfter = -1; Check(false, "preparation allocation refusal must return unavailable instead of escaping"); }
        failAfter = -1;
        if (!failedAllocation) {
            reachedComplete = result == prep::Result::complete;
            preparationBudget = budget;
            Equal(expected, output);
            break;
        }
        ++failures;
        Check(result == prep::Result::unavailable && !reader.calls && !reader.at,
              "every injected preparation allocation failure returns unavailable before reader access");
        Equal(unchanged, output);
        Check(!statistics.vertices && !statistics.batches && !statistics.workers,
              "refused preparation cannot publish completed work or started workers");
    }
    Check(reachedComplete && failures >= 4,
          "caller-only allocation sweep exercises batch ownership and all three packed buffers before completing");
    // Each MSVC thread constructor allocates its owning callable tuple on the
    // caller before _beginthreadex. Refuse that ordinary allocation at the
    // measured boundary, without changing the production thread launcher or
    // trying to exhaust operating-system resources. Worker-local allocations
    // are never affected by this thread_local failure budget.
    for (long started : {0L,1L}) {
        output = unchanged;
        Reader reader(wire.bytes, wire.bytes.size()); prep::Statistics statistics{};
        WorkerObservation observation;
        failedAllocation = false; failAfter = preparationBudget + started;
        prep::Result result = prep::Result::unavailable;
        try { result = prep::ReadVertices(reader, wire.count, 4, 4, output.data() + 1, 2, &statistics); }
        catch (...) { failAfter = -1; Check(false, "ordinary worker-start allocation refusal must use the prepared fallback"); }
        failAfter = -1;
        observation.Finish(statistics.workers);
        Check(failedAllocation && statistics.workers == static_cast<std::size_t>(started),
              "failure at measured thread startup boundary exercises actual zero- and one-worker paths");
        Check(result == prep::Result::complete && reader.at == wire.geometryBytes
                  && !reader.wrongThread.load(),
              "startup allocation refusal completes through caller or surviving worker with the same sole reader");
        BoundedStatistics(statistics, wire.count, 2, true);
        Equal(expected, output);
        StableAfterReturn(output);
    }
}

float FloatBits(std::uint32_t bits) {
    float value; std::memcpy(&value, &bits, sizeof value); return value;
}
Wire SdefArithmeticWire() {
    Wire wire; wire.count = 131075; wire.uv = 4; wire.boneWidth = 4;
    wire.bytes.reserve(wire.count * 149 + kFollowingSection.size());
    for (std::size_t i = 0; i < wire.count; ++i) {
        for (unsigned c = 0; c < 8; ++c) ref::Put(wire.bytes, FloatBits(0x3f000001u + c));
        for (unsigned uv = 0; uv < 4; ++uv) for (unsigned c = 0; c < 4; ++c)
            ref::Put(wire.bytes, FloatBits(0x3f000001u + uv * 4 + c));
        ref::Put(wire.bytes, static_cast<std::uint8_t>(3));
        ref::SignedIndex(wire.bytes, -12345 + static_cast<int>(i % 23), 4);
        ref::SignedIndex(wire.bytes, 12345 - static_cast<int>(i % 17), 4);
        ref::Put(wire.bytes, FloatBits(std::array<std::uint32_t,4>{
            0x3eaaaaab,0x3f000001,0x3f7fffff,0x3dcccccd}[i % 4]));
        for (std::uint32_t bits : {0x3eaaaaabu,0x00800001u,0x80000003u,
                                  0x3f800001u,0x00800001u,0x80000003u,
                                  0xbeaaaaabu,0x00000003u,0x00800007u})
            ref::Put(wire.bytes, FloatBits(bits + static_cast<std::uint32_t>(i % 2)));
        ref::Put(wire.bytes, .75f);
    }
    wire.geometryBytes = wire.bytes.size();
    wire.bytes.insert(wire.bytes.end(), kFollowingSection.begin(), kFollowingSection.end());
    return wire;
}

struct MxcsrGuard {
    const unsigned saved = _mm_getcsr();
    ~MxcsrGuard() { _mm_setcsr(saved); }
};
void FloatingPointControls() {
    const auto wire = SdefArithmeticWire();
    MxcsrGuard restore;
    alignas(16) std::array<unsigned char,512> image{};
    _fxsave(image.data());
    unsigned supported = 0;
    std::memcpy(&supported, image.data() + 28, sizeof supported);
    if (!supported) supported = 0xffbfu;
    const unsigned denormalMask = supported & 0x8040u;
    std::array<mdl::PmxSdefData,8> nearest{};
    bool sawRoundingDifference = false, sawDenormalDifference = false;
    for (unsigned rounding : {0u,0x2000u,0x4000u,0x6000u})
        for (unsigned denormals : {0u,0x40u,0x8000u,0x8040u}) {
            if (denormals & ~denormalMask) continue;
            const unsigned control = (restore.saved & ~(0x6000u | 0x8040u | 0x3fu))
                | rounding | denormals;
            _mm_setcsr(control);
            const auto expected = Expected(wire);
            if (!rounding && !denormals)
                for (std::size_t i = 0; i < nearest.size(); ++i) nearest[i] = expected[i + 1].sdef;
            else for (std::size_t i = 0; i < nearest.size(); ++i) {
                const bool changed = std::memcmp(&nearest[i], &expected[i + 1].sdef, sizeof nearest[i]) != 0;
                if (rounding && !denormals) sawRoundingDifference = sawRoundingDifference || changed;
                if (!rounding && denormals) sawDenormalDifference = sawDenormalDifference || changed;
            }
            // Reset status flags as well as controls so original decoding and
            // each actual worker start from the same caller SSE state.
            _mm_setcsr(control);
            CompleteCase(wire, expected, 2);
            Check((_mm_getcsr() & (0x6000u | 0x8040u)) == (control & (0x6000u | 0x8040u)),
                  "preparation preserves the caller's rounding and denormal controls");
        }
    Check(sawRoundingDifference, "SDEF fixture is actually sensitive to the tested rounding controls");
    Check(!denormalMask || sawDenormalDifference,
          "SDEF fixture is actually sensitive to supported flush/denormals-are-zero controls");
    if ((denormalMask & 0x40u) == 0)
        std::printf("LIMIT processor MXCSR mask does not support DAZ; unsupported control was not set\n");
}

void Run(const char* name, void(*test)()) {
    stage = name; std::printf("STAGE %s\n", name); test();
    Check(failAfter == -1 && !observeWorkers.load(), "test leaves no allocation or worker observer scope active");
    std::printf("PASS %s\n", name);
}
} // namespace

void* operator new(std::size_t bytes) {
    if (failAfter == 0) { failedAllocation = true; throw std::bad_alloc(); }
    if (failAfter > 0) --failAfter;
    if (auto* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { ObserveDeleteThread(); std::free(memory); }
void operator delete[](void* memory) noexcept { ObserveDeleteThread(); std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { ObserveDeleteThread(); std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ObserveDeleteThread(); std::free(memory); }

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    static_assert(prep::kBatchVertices == 8192 && prep::kBatchCount == 3
        && prep::kMinimumParallelVertices == 131072 && prep::kMaxRecordBytes == 149);
    Run("independent-layouts-and-batch-thresholds", LayoutsAndBoundaries);
    Run("all-short-read-offsets-and-early-rejection", EveryShortReadAndEarlyRejection);
    Run("cancel-exception-and-worker-lifetime", CancellationAndExceptions);
    Run("preparation-allocation-failure", PreparationAllocationFailures);
    Run("SDEF-bitwise-MXCSR-controls", FloatingPointControls);
    std::printf("PASS PMX vertex preparation contracts: %u checks\n", checks);
    return 0;
}
