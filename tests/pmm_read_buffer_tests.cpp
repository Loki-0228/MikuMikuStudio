// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// Focused regressions for the PMM standard-body read-ahead window
// (src/io/pmm_read_buffer.hpp): byte-exact reads across the 64 KiB boundary,
// requests larger than the window, ShortRead / OptionalRd end-of-stream
// semantics, ReadCount capacity checks, the external-reader position handoff,
// scope exit positions, an early Close, descriptor reuse and nested scopes.
// Every temporary file is removed by its TempFile guard on every path.
//
// Pure CPU: no window, no GPU, no MMDApp, no PMM parser.  The test uses only
// the reader helpers, so reader regressions fail here independently of the
// loader bodies.
#include "io/pmm_read_buffer.hpp"

#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pmm_io = mikumikustudio::pmm_io;

namespace {

int g_checks = 0;
int g_failures = 0;
int g_tempSerial = 0;

void Check(bool condition, const std::string& description) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    }
}

// Some regressions deliberately hand a closed descriptor or an unseekable
// pipe to the CRT to prove the old error return is kept.  The UCRT invalid
// parameter handler would otherwise terminate the process with 0xC0000409
// (__fastfail) before the assertions run.  A no-op thread-local handler lets
// the CRT return its documented -1/errno result instead.  It only changes how
// the CRT reports invalid arguments; Check() still counts and prints every
// failed assertion, and the exit code is still driven by g_failures.
void __cdecl IgnoreInvalidParameter(const wchar_t*, const wchar_t*,
                                    const wchar_t*, unsigned int, uintptr_t) {}

struct InvalidParameterHandlerScope {
    _invalid_parameter_handler previous;
    InvalidParameterHandlerScope()
        : previous(_set_thread_local_invalid_parameter_handler(
              &IgnoreInvalidParameter)) {}
    ~InvalidParameterHandlerScope() {
        (void)_set_thread_local_invalid_parameter_handler(previous);
    }
    InvalidParameterHandlerScope(const InvalidParameterHandlerScope&) = delete;
    InvalidParameterHandlerScope& operator=(
        const InvalidParameterHandlerScope&) = delete;
};

// Prints a flushed marker before each group so a crash or a fastfail names the
// group that was running; the failure delta is reported per group without
// hiding any Check() output.
void RunTest(const char* name, void (*test)()) {
    const int failuresBefore = g_failures;
    std::printf("[ RUN  ] %s\n", name);
    std::fflush(stdout);
    test();
    const int newFailures = g_failures - failuresBefore;
    std::printf("[ %s ] %s", newFailures == 0 ? " OK " : "FAIL", name);
    if (newFailures != 0) std::printf(" (%d new failures)", newFailures);
    std::printf("\n");
    std::fflush(stdout);
}

std::vector<unsigned char> Pattern(std::size_t size, unsigned seed = 0) {
    std::vector<unsigned char> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<unsigned char>(
            (i * 37u + i / 251u + 11u + static_cast<std::size_t>(seed) * 29u) &
            0xFFu);
    }
    return bytes;
}

struct TempFile {
    char path[160]{};
    int fd = -1;
    explicit TempFile(const char* tag) {
        std::snprintf(path, sizeof path, "pmm-read-buffer-%d-%d-%s.tmp",
                      _getpid(), ++g_tempSerial, tag);
        std::remove(path);
        fd = _open(path, _O_CREAT | _O_TRUNC | _O_RDWR | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
        Check(fd >= 0, "temporary file opens");
    }
    ~TempFile() {
        if (fd >= 0) _close(fd);
        std::remove(path);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    void Write(const std::vector<unsigned char>& bytes) {
        const int want = static_cast<int>(bytes.size());
        const int wrote =
            bytes.empty()
                ? 0
                : _write(fd, bytes.data(),
                         static_cast<unsigned int>(bytes.size()));
        Check(wrote == want, "fixture bytes written");
        Check(_lseeki64(fd, 0, SEEK_SET) == 0, "fixture rewinds");
    }
    // The code under test closed the descriptor (early-Close scenarios).
    void Release() { fd = -1; }
};

std::vector<unsigned char> CountFixture(std::int32_t count,
                                        std::size_t payload) {
    std::vector<unsigned char> bytes(4 + payload);
    std::memcpy(bytes.data(), &count, 4);
    for (std::size_t i = 4; i < bytes.size(); ++i)
        bytes[i] = static_cast<unsigned char>(0xA0 + (i & 0x0F));
    return bytes;
}

bool ReadCountAccepted(const std::vector<unsigned char>& fixture,
                       unsigned int minimumBytes,
                       std::int32_t* out = nullptr) {
    TempFile file("read-count");
    file.Write(fixture);
    pmm_io::CheckedReads scope(file.fd);
    try {
        std::int32_t count = 0;
        pmm_io::ReadCount(file.fd, count, minimumBytes);
        if (out != nullptr) *out = count;
        return true;
    } catch (const std::runtime_error&) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// 64 KiB boundary crossing, a request larger than the window, and the same
// sequence through the disabled window.
// ---------------------------------------------------------------------------
constexpr unsigned int kBoundarySizes[] = {65536u, 4u,    4u,    4u,
                                           100000u, 65535u, 68917u};
constexpr std::size_t kBoundaryTotal =
    65536u + 4u + 4u + 4u + 100000u + 65535u + 68917u;

void TestBoundaryCrossing() {
    const std::vector<unsigned char> data = Pattern(kBoundaryTotal, 1);

    struct Trace {
        std::vector<std::vector<unsigned char>> chunks;
        std::vector<std::int64_t> positions;
        pmm_io::ReadStatistics live{};
        pmm_io::ReadStatistics accumulated{};
        std::int64_t endPosition = -1;
    };

    auto run = [](const std::vector<unsigned char>& fixture,
                  bool buffered) {
        TempFile file("boundary");
        file.Write(fixture);
        pmm_io::bufferedReads = buffered;
        pmm_io::readStatistics = {};
        Trace trace;
        std::size_t offset = 0;
        {
            pmm_io::CheckedReads scope(file.fd);
            for (const unsigned int size : kBoundarySizes) {
                std::vector<unsigned char> chunk(size, 0xCD);
                const int received = pmm_io::Rd(file.fd, chunk.data(), size);
                Check(received == static_cast<int>(size),
                      "boundary read returns the whole field");
                Check(std::memcmp(chunk.data(), fixture.data() + offset, size) ==
                          0,
                      "boundary read bytes");
                offset += size;
                Check(pmm_io::Position(file.fd) ==
                          static_cast<std::int64_t>(offset),
                      "boundary read logical position");
                trace.positions.push_back(pmm_io::Position(file.fd));
                trace.chunks.push_back(std::move(chunk));
            }
            trace.live = pmm_io::LiveReadStatistics(file.fd);
        }
        Check(offset == fixture.size(), "boundary fixture fully consumed");
        trace.accumulated = pmm_io::readStatistics;
        trace.endPosition = _telli64(file.fd);
        return trace;
    };

    const Trace buffered = run(data, true);
    const Trace direct = run(data, false);
    const std::size_t count = sizeof kBoundarySizes / sizeof kBoundarySizes[0];
    Check(buffered.chunks.size() == count && direct.chunks.size() == count,
          "all boundary fields read in both modes");
    std::size_t cumulative = 0;
    for (std::size_t i = 0; i < count && i < buffered.chunks.size(); ++i) {
        cumulative += kBoundarySizes[i];
        Check(buffered.chunks[i] == direct.chunks[i],
              "buffered and per-field reads return the same bytes");
        Check(buffered.positions[i] ==
                  static_cast<std::int64_t>(cumulative),
              "buffered logical position advances by field size");
        Check(direct.positions[i] == buffered.positions[i],
              "buffered and per-field logical positions agree");
    }
    Check(buffered.live.requests == count, "one request per field, buffered");
    Check(direct.live.requests == count, "one request per field, direct");
    Check(buffered.live.bytes == kBoundaryTotal, "buffered byte accounting");
    Check(direct.live.bytes == kBoundaryTotal, "direct byte accounting");
    Check(buffered.live.buffered > 0, "some bytes come from the window");
    Check(buffered.live.systemReads < buffered.live.requests,
          "window coalesces field reads into fewer _read calls");
    Check(direct.live.systemReads == direct.live.requests,
          "disabled window issues one _read per field");
    Check(direct.live.buffered == 0, "disabled window serves no buffered bytes");
    Check(buffered.accumulated.requests == buffered.live.requests &&
              buffered.accumulated.systemReads == buffered.live.systemReads &&
              buffered.accumulated.bytes == buffered.live.bytes,
          "scope statistics reach the thread-local totals");
    Check(buffered.endPosition == static_cast<std::int64_t>(kBoundaryTotal),
          "scope exit leaves the descriptor at the consumed position");
    Check(direct.endPosition == static_cast<std::int64_t>(kBoundaryTotal),
          "direct mode scope exit position");
}

// ---------------------------------------------------------------------------
// ShortRead offsets/counts, full-EOF OptionalRd = 0, partial optional field.
// ---------------------------------------------------------------------------
void TestShortReadAndOptional() {
    for (const bool buffered : {true, false}) {
        pmm_io::bufferedReads = buffered;
        pmm_io::readStatistics = {};
        {
            TempFile file("short-read");
            file.Write(Pattern(10, 2));
            pmm_io::CheckedReads scope(file.fd);
            Check(pmm_io::Rd(file.fd, nullptr, 0) == 0,
                  "zero-length Rd returns 0");
            Check(pmm_io::Position(file.fd) == 0,
                  "zero-length Rd does not move the stream");
            unsigned char head[8]{};
            Check(pmm_io::Rd(file.fd, head, 8) == 8,
                  "short-read fixture head");
            bool threw = false;
            try {
                unsigned char tail[4]{};
                pmm_io::Rd(file.fd, tail, 4);
            } catch (const pmm_io::ShortRead& error) {
                threw = true;
                Check(error.offset == 8, "ShortRead reports the logical offset");
                Check(error.expected == 4, "ShortRead reports the expected count");
                Check(error.received == 2, "ShortRead reports the received count");
            }
            Check(threw, "a partial field throws ShortRead");
            Check(pmm_io::Position(file.fd) == 10,
                  "a partial field consumes the available bytes");
            threw = false;
            try {
                unsigned char byte = 0;
                pmm_io::Rd(file.fd, &byte, 1);
            } catch (const pmm_io::ShortRead& error) {
                threw = true;
                Check(error.offset == 10 && error.expected == 1 &&
                          error.received == 0,
                      "repeated EOF reports a clean zero-byte short read");
            }
            Check(threw, "a field at full EOF throws ShortRead");
        }
        {
            TempFile file("optional-eof");
            file.Write(Pattern(6, 3));
            pmm_io::CheckedReads scope(file.fd);
            unsigned char field[6]{};
            Check(pmm_io::OptionalRd(file.fd, field, 6) == 6,
                  "OptionalRd reads a present field");
            unsigned char probe[4]{};
            Check(pmm_io::OptionalRd(file.fd, probe, 4) == 0,
                  "OptionalRd returns 0 at full EOF");
            Check(pmm_io::OptionalRd(file.fd, probe, 1) == 0,
                  "repeated OptionalRd EOF stays 0");
            Check(pmm_io::OptionalRd(file.fd, nullptr, 0) == 0,
                  "zero-length OptionalRd returns 0");
            Check(pmm_io::Position(file.fd) == 6,
                  "EOF probes do not move the logical position");
        }
        {
            TempFile file("optional-partial");
            file.Write(Pattern(6, 4));
            pmm_io::CheckedReads scope(file.fd);
            unsigned char head[4]{};
            Check(pmm_io::Rd(file.fd, head, 4) == 4,
                  "optional-partial fixture head");
            bool threw = false;
            try {
                unsigned char tail[4]{};
                pmm_io::OptionalRd(file.fd, tail, 4);
            } catch (const pmm_io::ShortRead& error) {
                threw = true;
                Check(error.offset == 4 && error.expected == 4 &&
                          error.received == 2,
                      "OptionalRd reports a partially present field");
            }
            Check(threw, "a partially present optional field is still corrupt");
        }
    }
}

// ---------------------------------------------------------------------------
// ReadCount: negative, overflowing, past-remaining and legal counts, with the
// minimum-bytes capacity check evaluated against the logical position.
// ---------------------------------------------------------------------------
void TestReadCount() {
    for (const bool buffered : {true, false}) {
        pmm_io::bufferedReads = buffered;
        std::int32_t count = -7;
        Check(ReadCountAccepted(CountFixture(2, 8), 4, &count),
              "ReadCount accepts a count that fits");
        Check(count == 2, "ReadCount returns the count");
        Check(!ReadCountAccepted(CountFixture(3, 8), 4),
              "ReadCount rejects a count past the remaining bytes");
        Check(!ReadCountAccepted(CountFixture(-1, 8), 4),
              "ReadCount rejects a negative count");
        Check(!ReadCountAccepted(CountFixture(INT32_MAX, 8), 4),
              "ReadCount rejects an overflowing count");
        Check(ReadCountAccepted(CountFixture(5, 5), 1),
              "ReadCount accepts minimumBytes=1 at the boundary");
        Check(!ReadCountAccepted(CountFixture(6, 5), 1),
              "ReadCount rejects minimumBytes=1 past the boundary");
        Check(ReadCountAccepted(CountFixture(0, 0), 4),
              "ReadCount accepts a zero count");
    }
}

// ---------------------------------------------------------------------------
// SyncExternalPosition: a raw CRT reader (the extension trailer reader) must
// start at the consumed byte, not at the read-ahead position.
// ---------------------------------------------------------------------------
void TestExternalHandoff() {
    const std::vector<unsigned char> data = Pattern(200, 9);
    TempFile file("external-handoff");
    file.Write(data);
    pmm_io::bufferedReads = true;
    pmm_io::readStatistics = {};
    {
        pmm_io::CheckedReads scope(file.fd);
        unsigned char head[7]{};
        Check(pmm_io::Rd(file.fd, head, 7) == 7, "handoff fixture head");
        Check(std::memcmp(head, data.data(), 7) == 0, "handoff fixture bytes");
        Check(pmm_io::Position(file.fd) == 7, "handoff logical position");
        Check(_telli64(file.fd) == 200,
              "read-ahead advanced the CRT descriptor");
        Check(pmm_io::SyncExternalPosition(file.fd),
              "position handoff reports success");
        Check(_telli64(file.fd) == 7,
              "sync leaves the CRT descriptor at the consumed position");
        unsigned char next = 0;
        Check(_read(file.fd, &next, 1) == 1 && next == data[7],
              "raw reader sees the next record byte");
        Check(_lseeki64(file.fd, 7, SEEK_SET) == 7,
              "raw reader restores the saved position");
        unsigned char more[4]{};
        Check(pmm_io::Rd(file.fd, more, 4) == 4,
              "buffered reads resume after the handoff");
        Check(std::memcmp(more, data.data() + 7, 4) == 0,
              "resumed bytes after the handoff");
        Check(pmm_io::Position(file.fd) == 11,
              "resumed logical position after the handoff");
        unsigned char tail[2]{};
        Check(pmm_io::Rd(file.fd, tail, 2) == 2, "post-handoff tail");
        Check(std::memcmp(tail, data.data() + 11, 2) == 0,
              "post-handoff tail bytes");
    }
    Check(_telli64(file.fd) == 13,
          "scope exit leaves the descriptor at the consumed position");
    Check(pmm_io::readStatistics.requests == 3,
          "handoff scope request accounting");
}

// ---------------------------------------------------------------------------
// Close during an open scope, then a new descriptor (the CRT reuses the
// number): the new scope must not inherit the closed scope's window.
// ---------------------------------------------------------------------------
void TestEarlyCloseAndReuse() {
    const std::vector<unsigned char> data = Pattern(100, 10);
    TempFile first("early-close");
    first.Write(data);
    const int closedFd = first.fd;
    {
        pmm_io::CheckedReads scope(first.fd);
        unsigned char head[3]{};
        Check(pmm_io::Rd(first.fd, head, 3) == 3, "early-close fixture head");
        // The loader closes the descriptor on its abort paths while the
        // CheckedReads scope is still alive.
        pmm_io::Close(first.fd);
        first.Release();
    }

    const std::vector<unsigned char> other = Pattern(50, 11);
    TempFile second("descriptor-reuse");
    second.Write(other);
    Check(second.fd == closedFd,
          "closed descriptor number is reused by the next open");
    pmm_io::readStatistics = {};
    {
        pmm_io::CheckedReads scope(second.fd);
        std::vector<unsigned char> actual(other.size(), 0xCD);
        Check(pmm_io::Rd(second.fd, actual.data(),
                         static_cast<unsigned int>(actual.size())) ==
                  static_cast<int>(actual.size()),
              "descriptor after an early Close reads cleanly");
        Check(actual == other, "reused descriptor bytes");
        Check(pmm_io::Position(second.fd) ==
                  static_cast<std::int64_t>(other.size()),
              "reused descriptor logical position");
    }
    Check(_telli64(second.fd) == static_cast<std::int64_t>(other.size()),
          "reused descriptor exit position");
}

// ---------------------------------------------------------------------------
// Nested scopes: a different descriptor restores the outer scope, the same
// descriptor shares the window instead of resetting it.
// ---------------------------------------------------------------------------
void TestNestedScopes() {
    const std::vector<unsigned char> dataA = Pattern(1000, 5);
    const std::vector<unsigned char> dataB = Pattern(700, 6);
    TempFile a("nested-a");
    TempFile b("nested-b");
    a.Write(dataA);
    b.Write(dataB);
    pmm_io::bufferedReads = true;
    pmm_io::readStatistics = {};
    {
        pmm_io::CheckedReads outer(a.fd);
        unsigned char head[5]{};
        Check(pmm_io::Rd(a.fd, head, 5) == 5, "outer scope first field");
        {
            pmm_io::CheckedReads inner(b.fd);
            Check(pmm_io::LiveReadStatistics(b.fd).requests == 0,
                  "inner scope starts with fresh statistics");
            std::vector<unsigned char> all = dataB;
            Check(pmm_io::Rd(b.fd, all.data(),
                             static_cast<unsigned int>(all.size())) ==
                      static_cast<int>(all.size()),
                  "inner scope reads its descriptor");
            Check(all == dataB, "inner scope bytes");
            Check(pmm_io::Position(b.fd) == 700,
                  "inner scope logical position");
        }
        Check(pmm_io::Position(a.fd) == 5,
              "outer logical position restored after the inner scope");
        unsigned char more[5]{};
        Check(pmm_io::Rd(a.fd, more, 5) == 5,
              "outer scope continues after the inner scope");
        Check(std::memcmp(head, dataA.data(), 5) == 0, "outer head bytes");
        Check(std::memcmp(more, dataA.data() + 5, 5) == 0,
              "outer resumed bytes");
        Check(pmm_io::Position(a.fd) == 10,
              "outer logical position after the resume");
    }
    Check(_telli64(a.fd) == 10, "outer descriptor synced on scope exit");
    Check(_telli64(b.fd) == 700, "inner descriptor synced on scope exit");

    pmm_io::readStatistics = {};
    {
        pmm_io::CheckedReads outer(a.fd);
        unsigned char first[5]{};
        Check(pmm_io::Rd(a.fd, first, 5) == 5 &&
                  std::memcmp(first, dataA.data() + 10, 5) == 0,
              "shared-window outer first field");
        {
            pmm_io::CheckedReads inner(a.fd);
            unsigned char second[5]{};
            Check(pmm_io::Rd(a.fd, second, 5) == 5 &&
                      std::memcmp(second, dataA.data() + 15, 5) == 0,
                  "shared-window nested read");
            Check(pmm_io::Position(a.fd) == 20,
                  "shared-window nested logical position");
        }
        unsigned char third[5]{};
        Check(pmm_io::Rd(a.fd, third, 5) == 5 &&
                  std::memcmp(third, dataA.data() + 20, 5) == 0,
              "shared-window outer resumes");
        Check(pmm_io::Position(a.fd) == 25,
              "shared-window outer logical position");
    }
    Check(_telli64(a.fd) == 25, "shared-window scope exit position");
}

// ---------------------------------------------------------------------------
// The headline claim: many 1/4-byte field reads must not become that many
// system reads.
// ---------------------------------------------------------------------------
void TestReadAheadReducesSystemReads() {
    const std::size_t kBytes = 200004;  // 6 bytes per 3-field cycle
    const std::vector<unsigned char> data = Pattern(kBytes, 7);
    for (const bool buffered : {true, false}) {
        TempFile file("read-counts");
        file.Write(data);
        pmm_io::bufferedReads = buffered;
        pmm_io::readStatistics = {};
        std::size_t offset = 0;
        unsigned int step = 0;
        std::uint64_t fieldReads = 0;
        pmm_io::ReadStatistics live{};
        {
            pmm_io::CheckedReads scope(file.fd);
            while (offset < data.size()) {
                const unsigned int size = step % 3 == 0 ? 4u : 1u;
                unsigned char field[4]{};
                Check(pmm_io::Rd(file.fd, field, size) ==
                          static_cast<int>(size),
                      "small field read");
                Check(std::memcmp(field, data.data() + offset, size) == 0,
                      "small field bytes");
                offset += size;
                ++step;
                ++fieldReads;
            }
            live = pmm_io::LiveReadStatistics(file.fd);
        }
        Check(fieldReads == 100002, "expected number of field reads");
        Check(live.requests == fieldReads,
              "requests count every field read");
        Check(live.bytes == kBytes, "read-ahead byte accounting");
        if (buffered) {
            Check(live.systemReads * 1000 < live.requests,
                  "read-ahead keeps _read calls far below field reads");
            Check(live.systemReads <= 8,
                  "roughly one _read per 64 KiB window");
            Check(live.buffered > 0, "field bytes come from the window");
        } else {
            Check(live.systemReads == live.requests,
                  "disabled window keeps one _read per field");
            Check(live.buffered == 0,
                  "disabled window serves no buffered bytes");
        }
        Check(pmm_io::readStatistics.requests == live.requests,
              "scope totals accumulated on exit");
        Check(_telli64(file.fd) == static_cast<std::int64_t>(kBytes),
              "descriptor position equals consumed bytes after the scope");
    }
}

// ---------------------------------------------------------------------------
// A CRT read error is still a ShortRead with the old offset/count fields.
// ---------------------------------------------------------------------------
void TestReadErrorPropagation() {
    pmm_io::bufferedReads = true;
    TempFile file("read-error");
    file.Write(Pattern(8, 8));
    const int fd = file.fd;
    file.Release();
    {
        pmm_io::CheckedReads scope(fd);
        _close(fd);  // the descriptor dies under the reader; no window was filled
        bool threw = false;
        try {
            unsigned char byte = 0;
            pmm_io::Rd(fd, &byte, 1);
        } catch (const pmm_io::ShortRead& error) {
            threw = true;
            Check(error.offset == 0, "CRT error keeps the logical offset");
            Check(error.expected == 1, "CRT error keeps the expected count");
            Check(error.received == -1,
                  "CRT error surfaces as a negative received count");
        }
        Check(threw, "a CRT read error throws ShortRead");
    }
}

// ---------------------------------------------------------------------------
// NULL destinations and requests above INT_MAX are refused with the CRT's
// negative result before anything is copied, in both reader modes.
// ---------------------------------------------------------------------------
void TestReadArgumentChecks() {
    for (const bool buffered : {true, false}) {
        pmm_io::bufferedReads = buffered;
        pmm_io::readStatistics = {};
        TempFile file("argument-checks");
        file.Write(Pattern(16, 15));
        {
            pmm_io::CheckedReads scope(file.fd);
            const std::int64_t start = pmm_io::Position(file.fd);
            bool threw = false;
            try {
                pmm_io::Rd(file.fd, nullptr, 1);
            } catch (const pmm_io::ShortRead& error) {
                threw = true;
                Check(error.offset == start && error.expected == 1 &&
                          error.received == -1,
                      "NULL destination reports a negative CRT-style read");
            }
            Check(threw, "NULL destination throws ShortRead");
            Check(pmm_io::Position(file.fd) == start,
                  "a rejected NULL destination does not move the stream");
            threw = false;
            try {
                unsigned char byte = 0;
                pmm_io::Rd(file.fd, &byte, 0x80000000u);
            } catch (const pmm_io::ShortRead& error) {
                threw = true;
                Check(error.expected == 0x80000000u && error.received == -1,
                      "a request above INT_MAX is rejected before the CRT");
            }
            Check(threw, "a request above INT_MAX throws ShortRead");
            Check(pmm_io::Position(file.fd) == start,
                  "an oversized request does not move the stream");
            unsigned char ok[4]{};
            Check(pmm_io::Rd(file.fd, ok, 4) == 4,
                  "reads continue after rejected arguments");
        }
    }
}

// ---------------------------------------------------------------------------
// A descriptor whose initial position query fails must keep the old per-field
// read path: the CheckedReads constructor disables buffering, Position falls
// back to the failing CRT query (-1), and a field read reports offset -1 /
// received -1 instead of accumulating -1 as a logical position.  A closed,
// not-yet-reused descriptor makes _telli64 fail deterministically on Windows;
// the local no-op invalid-parameter handler turns the CRT's invalid-fd report
// into the documented negative result.
// ---------------------------------------------------------------------------
void TestFailedInitialPosition() {
    TempFile file("failed-initial-position");
    file.Write(Pattern(8, 26));
    const int fd = file.fd;
    file.Release();
    Check(_close(fd) == 0, "fixture descriptor closes");
    // Preconditions: the number is closed and not reused, and the CRT reports
    // the failed position query as a negative result.
    Check(_telli64(fd) < 0,
          "closed descriptor reports a failed position query");
    pmm_io::bufferedReads = true;
    pmm_io::readStatistics = {};
    {
        pmm_io::CheckedReads scope(fd);
        Check(pmm_io::LiveReadStatistics(fd).requests == 0,
              "constructor does not read from the failed descriptor");
        // Position falls back to the CRT query only when the constructor
        // disabled buffering on the failed initial position; a buffered state
        // would report logical 0 here.
        Check(pmm_io::Position(fd) == -1,
              "failed initial position keeps the per-field position source");
        bool threw = false;
        try {
            unsigned char byte = 0;
            pmm_io::Rd(fd, &byte, 1);
        } catch (const pmm_io::ShortRead& error) {
            threw = true;
            Check(error.offset == -1,
                  "failed initial position reports offset -1");
            Check(error.expected == 1,
                  "failed initial position keeps the expected count");
            Check(error.received == -1,
                  "failed initial position reports received -1");
        }
        Check(threw, "a read on the failed descriptor throws ShortRead");
        const pmm_io::ReadStatistics live =
            pmm_io::LiveReadStatistics(fd);
        Check(live.requests == 1 && live.systemReads == 1,
              "failed initial position falls back to one CRT read per field");
        Check(live.buffered == 0,
              "failed initial position never serves buffered bytes");
    }
    Check(pmm_io::readStatistics.requests == 1 &&
              pmm_io::readStatistics.systemReads == 1,
          "failed-initial-position scope statistics are folded on exit");
    Check(_telli64(fd) < 0,
          "scope exit leaves the closed descriptor untouched");
}

// ---------------------------------------------------------------------------
// Close a suspended outer descriptor from an inner scope, let the CRT reuse
// the number, then exit both scopes: neither exit may seek the new file or
// resurrect the closed window.
// ---------------------------------------------------------------------------
void TestCloseSuspendedOuterDescriptor() {
    const std::vector<unsigned char> outerData = Pattern(400, 16);
    const std::vector<unsigned char> innerData = Pattern(300, 17);
    const std::vector<unsigned char> reusedData = Pattern(64, 18);
    TempFile outerFile("suspended-close-outer");
    TempFile innerFile("suspended-close-inner");
    outerFile.Write(outerData);
    innerFile.Write(innerData);
    const int outerFd = outerFile.fd;
    std::unique_ptr<TempFile> reusedFile;
    pmm_io::bufferedReads = true;
    pmm_io::readStatistics = {};
    {
        pmm_io::CheckedReads outer(outerFile.fd);
        unsigned char outerHead[5]{};
        Check(pmm_io::Rd(outerFile.fd, outerHead, 5) == 5,
              "suspended-close outer head");
        {
            pmm_io::CheckedReads inner(innerFile.fd);
            unsigned char innerHead[5]{};
            Check(pmm_io::Rd(innerFile.fd, innerHead, 5) == 5,
                  "suspended-close inner head");
            pmm_io::Close(outerFile.fd);
            outerFile.Release();
            reusedFile = std::make_unique<TempFile>("suspended-close-reused");
            reusedFile->Write(reusedData);
            Check(reusedFile->fd == outerFd,
                  "the closed outer descriptor number is reused");
            {
                pmm_io::CheckedReads reused(reusedFile->fd);
                std::vector<unsigned char> all(reusedData.size(), 0xCD);
                Check(pmm_io::Rd(reusedFile->fd, all.data(),
                                 static_cast<unsigned int>(all.size())) ==
                          static_cast<int>(all.size()),
                      "reused descriptor reads through its own scope");
                Check(all == reusedData, "reused descriptor bytes");
            }
            Check(_telli64(reusedFile->fd) ==
                      static_cast<std::int64_t>(reusedData.size()),
                  "reused descriptor synced by its own scope");
        }
        Check(_telli64(reusedFile->fd) ==
                  static_cast<std::int64_t>(reusedData.size()),
              "inner scope exit leaves the reused descriptor alone");
    }
    Check(_telli64(reusedFile->fd) ==
              static_cast<std::int64_t>(reusedData.size()),
          "outer scope exit does not seek the reused descriptor");
    Check(reusedFile->fd >= 0,
          "reused descriptor is still open after the outer scope exits");
    unsigned char probe = 0;
    Check(_lseeki64(reusedFile->fd, 0, SEEK_SET) == 0 &&
              _read(reusedFile->fd, &probe, 1) == 1 &&
              probe == reusedData[0],
          "reused descriptor is usable after the outer scope exits");
}

// ---------------------------------------------------------------------------
// Close must clear a suspended ScopedDescriptor owner too, otherwise the
// outer owner would close the reused descriptor number a second time.
// ---------------------------------------------------------------------------
void TestSuspendedDescriptorOwners() {
    const std::vector<unsigned char> firstData = Pattern(32, 19);
    const std::vector<unsigned char> secondData = Pattern(32, 20);
    const std::vector<unsigned char> replacementData = Pattern(48, 21);
    TempFile first("owner-a");
    TempFile second("owner-b");
    first.Write(firstData);
    second.Write(secondData);
    int fdA = first.fd;
    int fdB = second.fd;
    first.Release();
    second.Release();
    const int fdAValue = fdA;
    std::unique_ptr<TempFile> replacement;
    {
        pmm_io::ScopedDescriptor outerOwner(fdA);
        {
            pmm_io::ScopedDescriptor innerOwner(fdB);
            pmm_io::Close(fdA);
            Check(fdA == -1,
                  "Close invalidates a suspended ScopedDescriptor owner");
            Check(fdB >= 0,
                  "Close leaves unrelated descriptor owners untouched");
            replacement = std::make_unique<TempFile>("owner-replacement");
            replacement->Write(replacementData);
            Check(replacement->fd == fdAValue,
                  "the closed owner's descriptor number is reused");
        }
        Check(fdB == -1,
              "the inner ScopedDescriptor still closes its own descriptor");
    }
    Check(replacement->fd >= 0,
          "outer owner exit does not close the reused descriptor");
    Check(_telli64(replacement->fd) == 0,
          "outer owner exit does not seek the reused descriptor");
    unsigned char probe = 0;
    Check(_read(replacement->fd, &probe, 1) == 1 &&
              probe == replacementData[0],
          "reused owner descriptor stays usable");
}

// ---------------------------------------------------------------------------
// A read on a descriptor whose scope is suspended below a different-descriptor
// scope must continue through that descriptor's own window, not the physical
// position left by its read-ahead.
// ---------------------------------------------------------------------------
void TestSuspendedDescriptorReads() {
    const std::vector<unsigned char> dataA = Pattern(200000, 22);
    const std::vector<unsigned char> dataB = Pattern(64, 23);
    TempFile a("suspended-read-a");
    TempFile b("suspended-read-b");
    a.Write(dataA);
    b.Write(dataB);
    pmm_io::bufferedReads = true;
    pmm_io::readStatistics = {};
    {
        pmm_io::CheckedReads outer(a.fd);
        unsigned char first[5]{};
        Check(pmm_io::Rd(a.fd, first, 5) == 5 &&
                  std::memcmp(first, dataA.data(), 5) == 0,
              "suspended-read outer first field");
        Check(_telli64(a.fd) == 65536,
              "suspended-read window pre-read past the consumed byte");
        {
            pmm_io::CheckedReads inner(b.fd);
            std::vector<unsigned char> allB(dataB.size(), 0xCD);
            Check(pmm_io::Rd(b.fd, allB.data(),
                             static_cast<unsigned int>(allB.size())) ==
                      static_cast<int>(allB.size()) &&
                      allB == dataB,
                  "suspended-read inner descriptor bytes");
            std::vector<unsigned char> acrossA(70000, 0xCD);
            Check(pmm_io::Rd(a.fd, acrossA.data(),
                             static_cast<unsigned int>(acrossA.size())) ==
                      static_cast<int>(acrossA.size()),
                  "a suspended descriptor stays readable");
            Check(std::memcmp(acrossA.data(), dataA.data() + 5,
                              acrossA.size()) == 0,
                  "suspended descriptor bytes");
            Check(pmm_io::Position(a.fd) == 70005,
                  "suspended descriptor logical position advances");
            Check(pmm_io::Position(b.fd) == 64,
                  "the inner descriptor keeps its own logical position");
        }
        unsigned char after[10]{};
        Check(pmm_io::Rd(a.fd, after, 10) == 10 &&
                  std::memcmp(after, dataA.data() + 70005, 10) == 0,
              "outer descriptor resumes after the inner scope");
        Check(pmm_io::Position(a.fd) == 70015,
              "outer logical position after the suspended reads");
    }
    Check(_telli64(a.fd) == 70015,
          "suspended reads leave the outer descriptor at the consumed byte");
}

// ---------------------------------------------------------------------------
// A -> B -> A nesting: the second A scope shares the suspended A window
// instead of starting a second buffer at the read-ahead position.
// ---------------------------------------------------------------------------
void TestNestedSameDescriptorAfterInnerScope() {
    const std::vector<unsigned char> dataA = Pattern(300, 24);
    const std::vector<unsigned char> dataB = Pattern(80, 25);
    TempFile a("nested-again-a");
    TempFile b("nested-again-b");
    a.Write(dataA);
    b.Write(dataB);
    pmm_io::bufferedReads = true;
    pmm_io::readStatistics = {};
    {
        pmm_io::CheckedReads outerA(a.fd);
        unsigned char first[5]{};
        Check(pmm_io::Rd(a.fd, first, 5) == 5 &&
                  std::memcmp(first, dataA.data(), 5) == 0,
              "nested-again outer A field");
        Check(pmm_io::Position(a.fd) == 5, "nested-again outer A position");
        {
            pmm_io::CheckedReads innerB(b.fd);
            std::vector<unsigned char> allB(dataB.size(), 0xCD);
            Check(pmm_io::Rd(b.fd, allB.data(),
                             static_cast<unsigned int>(allB.size())) ==
                      static_cast<int>(allB.size()) &&
                      allB == dataB,
                  "nested-again middle B scope bytes");
            {
                pmm_io::CheckedReads innerA(a.fd);
                unsigned char second[5]{};
                Check(pmm_io::Rd(a.fd, second, 5) == 5,
                      "nested-again second A scope reads");
                Check(std::memcmp(second, dataA.data() + 5, 5) == 0,
                      "second A scope shares the suspended window");
                Check(pmm_io::Position(a.fd) == 10,
                      "second A scope continues the shared logical position");
            }
            Check(pmm_io::Position(a.fd) == 10,
                  "second A scope exit leaves the shared state in place");
        }
        unsigned char third[5]{};
        Check(pmm_io::Rd(a.fd, third, 5) == 5 &&
                  std::memcmp(third, dataA.data() + 10, 5) == 0,
              "outer A resumes after the A -> B -> A nesting");
        Check(pmm_io::Position(a.fd) == 15,
              "outer A logical position after the nesting");
    }
    Check(_telli64(a.fd) == 15,
          "one A window is synchronised on the outer exit");
}

}  // namespace

int main() {
    // Installed for the whole run: only the CRT's invalid-argument reporting
    // changes (closed-descriptor and unseekable-pipe probes); assertions are
    // still counted normally.  The previous handler is restored on exit.
    InvalidParameterHandlerScope invalidParameterHandler;
    RunTest("boundary-crossing", &TestBoundaryCrossing);
    RunTest("short-read-and-optional", &TestShortReadAndOptional);
    RunTest("read-count", &TestReadCount);
    RunTest("external-handoff", &TestExternalHandoff);
    RunTest("early-close-and-reuse", &TestEarlyCloseAndReuse);
    RunTest("nested-scopes", &TestNestedScopes);
    RunTest("read-ahead-system-reads", &TestReadAheadReducesSystemReads);
    RunTest("read-error-propagation", &TestReadErrorPropagation);
    RunTest("read-argument-checks", &TestReadArgumentChecks);
    RunTest("failed-initial-position", &TestFailedInitialPosition);
    RunTest("close-suspended-outer-descriptor",
            &TestCloseSuspendedOuterDescriptor);
    RunTest("suspended-descriptor-owners", &TestSuspendedDescriptorOwners);
    RunTest("suspended-descriptor-reads", &TestSuspendedDescriptorReads);
    RunTest("nested-same-descriptor-after-inner",
            &TestNestedSameDescriptorAfterInnerScope);
    if (g_failures != 0) {
        std::fprintf(stderr, "FAILED %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS %d PMM read-buffer checks\n", g_checks);
    return 0;
}
