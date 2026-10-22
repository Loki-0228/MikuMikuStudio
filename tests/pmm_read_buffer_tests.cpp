// CPU-only tests of the production read-ahead scopes, strict-read scopes and
// sole descriptor owner. The CRT observers forward every ordinary operation.
// Faults target valid owned descriptors only; no invalid-parameter handler is
// installed and no invalid close is hidden.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

int __cdecl ObserveBufferRead(int, void*, unsigned int);
__int64 __cdecl ObserveBufferSeek(int, __int64, int);
int __cdecl ObserveBufferInputClose(int) noexcept;

// Only the production helpers' CRT calls are instrumented. Fixture setup,
// teardown and the observers' forwarding calls use the real CRT names.
#define _read ::ObserveBufferRead
#define _lseeki64 ::ObserveBufferSeek
#include "../src/io/pmm_read_buffer.hpp"
#undef _lseeki64
#undef _read
#include "../src/io/pmm_io_common.hpp"
#define _close ::ObserveBufferInputClose
#include "../src/io/pmm_input.hpp"
#undef _close

namespace pmm = mikumikustudio::pmm_io;
namespace fs = std::filesystem;
namespace {
constexpr unsigned B = 65536;
static_assert(pmm::kReadBufferBytes == B);
std::atomic<unsigned> checks{0}, serial{0};
thread_local long allocationBudget = -1;
thread_local const char* phase = "setup";
fs::path root;
void Check(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL [%s] %s\n", phase, why); std::exit(1); }
}
struct Hooks {
    std::uint64_t reads = 0, seeks = 0;
    int readFd = -1, seekFd = -1, limitedFd = -1;
    unsigned readOrdinal = 0, seekOrdinal = 0, failReadAt = 0, failSeekAt = 0;
    unsigned readFailures = 0, seekFailures = 0, maximumRead = 0;
};
thread_local Hooks hooks;
struct CloseProbe {
    const pmm::Input* owner = nullptr;
    int fd = -1, expectedChecked = -1, sentinel = -1;
    unsigned calls = 0;
    bool reuse = false;
    fs::path path;
};
thread_local CloseProbe closeProbe;

std::vector<unsigned char> Pattern(std::size_t count, unsigned seed = 0) {
    std::vector<unsigned char> bytes(count);
    for (std::size_t i = 0; i < count; ++i) bytes[i] = static_cast<unsigned char>((i * 37 + i / 251 + seed * 19 + 11) & 255);
    return bytes;
}
struct File {
    fs::path path;
    int fd = -1;
    explicit File(const std::vector<unsigned char>& bytes) {
        path = root / (L"read-" + std::to_wstring(++serial) + L".bin");
        Check(_wsopen_s(&fd, path.c_str(), _O_BINARY | _O_RDWR | _O_CREAT | _O_EXCL,
            _SH_DENYNO, _S_IREAD | _S_IWRITE) == 0 && fd >= 0, "create an exclusively owned real CRT fixture");
        Check(bytes.size() <= INT_MAX && (bytes.empty() || _write(fd, bytes.data(), static_cast<unsigned>(bytes.size())) == int(bytes.size())) &&
            _lseeki64(fd, 0, SEEK_SET) == 0, "write and rewind a complete real binary fixture");
    }
    ~File() {
        if (fd >= 0) Check(_close(fd) == 0, "close a fixture only through its current owner");
        if (!path.empty()) Check(DeleteFileW(path.c_str()) != FALSE, "delete only this fixture's own temporary file");
    }
    int Transfer() { return std::exchange(fd, -1); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};
struct Configuration {
    bool prior = pmm::bufferedReads;
    Configuration() { hooks = {}; pmm::bufferedReads = true; pmm::readStatistics = {}; }
    ~Configuration() { pmm::bufferedReads = prior; }
};
void BytesAt(int fd, const std::vector<unsigned char>& data, std::size_t at, unsigned count) {
    Check(at <= data.size() && count <= data.size() - at, "expected read stays inside its real source fixture");
    std::vector<unsigned char> actual(count, 0xcd);
    Check(pmm::Rd(fd, actual.data(), count) == int(count) &&
        std::memcmp(actual.data(), data.data() + at, count) == 0, "production read returns the exact expected file interval");
    Check(pmm::Position(fd) == static_cast<std::int64_t>(at + count), "logical position advances only by returned bytes");
}
void SameStatistics(const decltype(pmm::readStatistics)& first, const decltype(pmm::readStatistics)& second) {
    Check(first.requests == second.requests && first.systemReads == second.systemReads &&
        first.bytes == second.bytes && first.buffered == second.buffered, "read statistics are folded exactly once");
}
void ArmReadFailure(int fd, unsigned ordinal = 1) {
    hooks.readFd = fd; hooks.failReadAt = ordinal; hooks.readOrdinal = 0;
}
void ArmSeekFailure(int fd) {
    hooks.seekFd = fd; hooks.failSeekAt = 1; hooks.seekOrdinal = 0;
}
void ExpectShortRead(int fd, unsigned count, std::int64_t at, int received,
                     std::vector<unsigned char>& output) {
    bool rejected = false;
    try { (void)pmm::Rd(fd, output.data(), count); }
    catch (const pmm::ShortRead& error) {
        rejected = error.offset == at && error.expected == count && error.received == received;
        if (!rejected) std::fprintf(stderr, "DIAG [%s] short read offset=%lld expected=%u received=%d\n",
            phase, static_cast<long long>(error.offset), error.expected, error.received);
    }
    Check(rejected, "strict reads preserve the field's logical offset, expected count and actual result");
}

void BoundaryAndDirectReads() {
    Configuration config; const auto data = Pattern(B * 5 + 29, 1);
    const std::array<unsigned, 8> sizes{1, B - 2, 4, B + 17, 3, B - 1, B, B - 2};
    std::array<std::vector<unsigned char>, 2> traces;
    for (unsigned mode = 0; mode < 2; ++mode) {
        File file(data); pmm::bufferedReads = mode == 0; hooks = {}; pmm::readStatistics = {};
        std::size_t at = 0;
        {
            pmm::ReadAhead buffer(file.fd); pmm::CheckedReads strict(file.fd);
            for (auto count : sizes) {
                Check(count <= data.size() - at, "the boundary plan fits its complete source");
                std::vector<unsigned char> bytes(count, 0xcd);
                Check(pmm::Rd(file.fd, bytes.data(), count) == int(count) &&
                    !std::memcmp(bytes.data(), data.data() + at, count), "small, cross-window and direct large fields stay byte-exact");
                at += count; traces[mode].insert(traces[mode].end(), bytes.begin(), bytes.end());
                Check(pmm::Position(file.fd) == std::int64_t(at) && pmm::Tell(file.fd) == long(at),
                    "logical tell follows the consumed interval in both A/B modes");
            }
        }
        Check(_telli64(file.fd) == std::int64_t(at), "scope exit puts the real descriptor at the consumed byte");
    }
    Check(traces[0] == traces[1], "buffered and old per-field paths return identical complete traces");
    pmm::bufferedReads = true;
    {
        File directLarge(data); pmm::ReadAhead buffer(directLarge.fd); pmm::CheckedReads strict(directLarge.fd);
        BytesAt(directLarge.fd, data, 0, B * 2 + 7);
    }
    File positioned(data); Check(_lseeki64(positioned.fd, 17, SEEK_SET) == 17, "set a nonzero actual input start");
    pmm::bufferedReads = true;
    {
        pmm::ReadAhead buffer(positioned.fd);
        Check(pmm::checkedDescriptor == -1, "a read-ahead scope does not silently enable strict reads");
        BytesAt(positioned.fd, data, 17, 7);
        Check(_telli64(positioned.fd) > pmm::Position(positioned.fd), "a real fill moves the CRT beyond its logical consumer");
    }
    Check(_telli64(positioned.fd) == 24, "nonzero-start scope exit restores its own consumed position");
}
void SeekAndExternalHandoff() {
    Configuration config; const auto data = Pattern(B * 3 + 31, 2); File file(data);
    {
        pmm::ReadAhead buffer(file.fd); pmm::CheckedReads strict(file.fd);
        BytesAt(file.fd, data, 0, 7);
        Check(pmm::Seek(file.fd, 3, SEEK_CUR) == 10, "SEEK_CUR uses the logical cursor rather than read-ahead position");
        BytesAt(file.fd, data, 10, 5);
        Check(pmm::Seek(file.fd, B - 1, SEEK_SET) == B - 1, "absolute seek discards the former window");
        BytesAt(file.fd, data, B - 1, 8);
        Check(pmm::Seek(file.fd, -9, SEEK_END) == std::int64_t(data.size() - 9), "SEEK_END uses the real file extent");
        BytesAt(file.fd, data, data.size() - 9, 4);
        Check(pmm::Seek(file.fd, 19, SEEK_SET) == 19, "position a buffered external-reader handoff");
        BytesAt(file.fd, data, 19, 3);
        const auto consumed = pmm::Position(file.fd);
        Check(_telli64(file.fd) > consumed && pmm::SyncExternalPosition(file.fd) && _telli64(file.fd) == consumed,
            "successful handoff rewinds the same descriptor to its logical boundary");
        const auto* state = pmm::FindReadState(file.fd);
        Check(state && state->cursor == state->filled, "handoff invalidates every unread cached byte");
        std::array<unsigned char, 5> raw{};
        Check(_read(file.fd, raw.data(), unsigned(raw.size())) == int(raw.size()) &&
            !std::memcmp(raw.data(), data.data() + consumed, raw.size()), "a raw CRT consumer sees the exact next field on the same fd");
        Check(_lseeki64(file.fd, consumed, SEEK_SET) == consumed, "the raw reader restores its saved entry position");
        BytesAt(file.fd, data, std::size_t(consumed), 5);
        Check(pmm::SyncExternalPosition(file.fd) && pmm::SyncExternalPosition(file.fd), "repeated handoff is idempotent");
    }
    Check(pmm::Position(file.fd) == _telli64(file.fd), "without a read scope Position reports the actual CRT position");
}
void SeekFailuresPreserveUnreadBytes() {
    Configuration config; const auto data = Pattern(B * 2 + 31, 3); File file(data);
    {
        pmm::ReadAhead buffer(file.fd); pmm::CheckedReads strict(file.fd);
        BytesAt(file.fd, data, 0, 7);
        ArmSeekFailure(file.fd);
        Check(!pmm::SyncExternalPosition(file.fd) && hooks.seekFailures == 1 && pmm::Position(file.fd) == 7,
            "failed external sync reports failure without changing the logical boundary");
        BytesAt(file.fd, data, 7, 4);
        Check(pmm::SyncExternalPosition(file.fd) && _telli64(file.fd) == 11, "a real retry can synchronise after a one-shot valid-fd seek error");
        BytesAt(file.fd, data, 11, 3);
        ArmSeekFailure(file.fd);
        Check(pmm::Seek(file.fd, B + 10, SEEK_SET) == -1 && hooks.seekFailures == 2 && pmm::Position(file.fd) == 14,
            "failed logical seek never claims the requested new position");
        BytesAt(file.fd, data, 14, 5);
        Check(pmm::Seek(file.fd, 33, SEEK_SET) == 33, "a later real seek remains usable after refusal");
        BytesAt(file.fd, data, 33, 4);
    }
}
void EofAndPartialFields() {
    Configuration config; const auto data = Pattern(B + 3, 4);
    for (bool strictMode : {false, true}) {
        File file(data);
        {
            pmm::ReadAhead buffer(file.fd);
            if (!strictMode) {
                BytesAt(file.fd, data, 0, B - 1);
                std::vector<unsigned char> bytes(8, 0xcd);
                Check(pmm::Rd(file.fd, bytes.data(), 8) == 4 &&
                    !std::memcmp(bytes.data(), data.data() + B - 1, 4) &&
                    std::all_of(bytes.begin() + 4, bytes.end(), [](unsigned char byte) { return byte == 0xcd; }),
                    "unchecked partial fields return their real count and never write beyond it");
                const auto prior = bytes;
                Check(pmm::Rd(file.fd, bytes.data(), 8) == 0 && bytes == prior && pmm::checkedDescriptor == -1,
                    "read-ahead alone preserves V1 optional-tail EOF without enabling strict checks");
            } else {
                pmm::CheckedReads strict(file.fd); BytesAt(file.fd, data, 0, B - 1);
                std::vector<unsigned char> bytes(8, 0xcd); ExpectShortRead(file.fd, 8, B - 1, 4, bytes);
                Check(!std::memcmp(bytes.data(), data.data() + B - 1, 4) &&
                    std::all_of(bytes.begin() + 4, bytes.end(), [](unsigned char byte) { return byte == 0xcd; }),
                    "strict partial diagnostics do not imply rollback of the delivered prefix");
                const auto prior = bytes; ExpectShortRead(file.fd, 8, data.size(), 0, bytes);
                Check(bytes == prior, "a full EOF never changes the field destination");
            }
            Check(pmm::Position(file.fd) == std::int64_t(data.size()), "EOF logical position stops at the actual consumed extent");
        }
        Check(_telli64(file.fd) == std::int64_t(data.size()), "EOF scope exit leaves the descriptor at true EOF");
    }
}
void ReadFailuresAndShortBackingReads() {
    Configuration config; const auto data = Pattern(B * 2 + 23, 5);
    {
        File file(data); pmm::ReadAhead buffer(file.fd); ArmReadFailure(file.fd);
        std::array<unsigned char, 7> bytes{}; bytes.fill(0xcd);
        Check(pmm::Rd(file.fd, bytes.data(), unsigned(bytes.size())) == -1 && pmm::checkedDescriptor == -1 &&
            pmm::Position(file.fd) == 0 && std::all_of(bytes.begin(), bytes.end(), [](unsigned char byte) { return byte == 0xcd; }),
            "read-ahead without strictness returns the real backend error without writing or inventing a short-read exception");
    }
    hooks = {};
    {
        File file(data); pmm::ReadAhead buffer(file.fd); pmm::CheckedReads strict(file.fd);
        ArmReadFailure(file.fd); std::vector<unsigned char> bytes(7, 0xcd);
        ExpectShortRead(file.fd, 7, 0, -1, bytes);
        Check(hooks.readFailures == 1 && pmm::Position(file.fd) == 0 &&
            std::all_of(bytes.begin(), bytes.end(), [](unsigned char byte) { return byte == 0xcd; }),
            "a valid-fd backend failure before delivery preserves destination and logical position");
        BytesAt(file.fd, data, 0, B - 2); ArmReadFailure(file.fd);
        std::vector<unsigned char> suffix(6, 0xcd); ExpectShortRead(file.fd, 6, B - 2, 2, suffix);
        Check(hooks.readFailures == 2 && !std::memcmp(suffix.data(), data.data() + B - 2, 2) &&
            std::all_of(suffix.begin() + 2, suffix.end(), [](unsigned char byte) { return byte == 0xcd; }),
            "a backend failure after cached delivery reports the delivered prefix rather than a false negative count");
        BytesAt(file.fd, data, B, 6);
    }
    hooks = {};
    {
        File file(data); pmm::ReadAhead buffer(file.fd); pmm::CheckedReads strict(file.fd);
        hooks.limitedFd = file.fd; hooks.maximumRead = 997;
        BytesAt(file.fd, data, 0, B * 2 + 7);
        Check(hooks.reads > 2 && pmm::Position(file.fd) == B * 2 + 7,
            "positive short backing reads are assembled without losing or duplicating bytes");
    }
}
void InvalidArgumentsAndNoAllocation() {
    Configuration config; const auto data = Pattern(97, 6); File file(data);
    {
        allocationBudget = 0; pmm::ReadAhead buffer(file.fd); pmm::CheckedReads strict(file.fd); allocationBudget = -1;
        const auto reads = hooks.reads; const auto at = pmm::Position(file.fd); unsigned char byte = 0x5a;
        errno = 0;
        Check(pmm::ReadBytes(file.fd, nullptr, 0) == 0 && pmm::Position(file.fd) == at && hooks.reads == reads,
            "a zero-length null read does not call the CRT or advance the stream");
        Check(pmm::Rd(file.fd, nullptr, 0) == 0 && pmm::Position(file.fd) == at && hooks.reads == reads,
            "the strict wrapper also accepts an empty field without touching the CRT");
        errno = 0;
        Check(pmm::ReadBytes(file.fd, nullptr, 1) == -1 && errno == EINVAL && pmm::Position(file.fd) == at && hooks.reads == reads,
            "a nonempty null read fails before any CRT call or stream mutation");
        errno = 0;
        Check(pmm::ReadBytes(file.fd, &byte, unsigned(INT_MAX) + 1u) == -1 && errno == EINVAL && byte == 0x5a &&
            pmm::Position(file.fd) == at && hooks.reads == reads,
            "an oversized count cannot overwrite a short destination or invoke CRT invalid-parameter handling");
        bool rejected = false;
        try { (void)pmm::Rd(file.fd, nullptr, 5); }
        catch (const pmm::ShortRead& error) { rejected = error.offset == at && error.expected == 5 && error.received == -1; }
        Check(rejected && pmm::Position(file.fd) == at && hooks.reads == reads, "strict argument refusal keeps the original field offset");
        rejected = false; errno = 0;
        try { (void)pmm::Rd(file.fd, &byte, UINT_MAX); }
        catch (const pmm::ShortRead& error) { rejected = error.offset == at && error.expected == UINT_MAX && error.received == -1; }
        Check(rejected && errno == EINVAL && byte == 0x5a && pmm::Position(file.fd) == at && hooks.reads == reads,
            "UINT_MAX cannot alias the negative error result and bypass strict short-read refusal");
        BytesAt(file.fd, data, 0, 5);
        const auto seeks = hooks.seeks; errno = 0;
        Check(pmm::Seek(file.fd, 0, INT_MIN) == -1 && errno == EINVAL && hooks.seeks == seeks && pmm::Position(file.fd) == 5,
            "an invalid seek origin is refused without invoking CRT invalid-parameter handling");
        errno = 0;
        Check(pmm::Seek(file.fd, (std::numeric_limits<std::int64_t>::max)(), SEEK_CUR) == -1 && errno == EINVAL &&
            hooks.seeks == seeks && pmm::Position(file.fd) == 5,
            "logical relative-seek overflow is refused before any backend seek");
        BytesAt(file.fd, data, 5, 4);
    }
    {
        pmm::Input empty; allocationBudget = 0;
        const bool ahead = empty.EnableReadAhead(), checked = empty.EnableCheckedReads(); empty.Close(); allocationBudget = -1;
        Check(!ahead && !checked && empty.Descriptor() == -1, "empty owner activation and close remain allocation-free no-ops");
    }
}
void NestedAndSuspendedReads() {
    Configuration config; const auto a = Pattern(B + 41, 7), b = Pattern(B + 43, 8); File first(a), second(b);
    std::uint64_t total = 0;
    {
        pmm::ReadAhead outer(first.fd); pmm::CheckedReads strictA(first.fd);
        const auto* shared = pmm::FindReadState(first.fd); Check(shared != nullptr, "the outer fd owns a real read state");
        BytesAt(first.fd, a, 0, 5); total += 5;
        {
            pmm::ReadAhead other(second.fd); pmm::CheckedReads strictB(second.fd);
            BytesAt(second.fd, b, 0, 3); total += 3;
            BytesAt(first.fd, a, 5, 4); total += 4;
            Check(pmm::checkedDescriptor == second.fd, "reading a suspended fd does not replace the active strict context");
            {
                pmm::ReadAhead again(first.fd); pmm::CheckedReads strictAgain(first.fd);
                Check(pmm::FindReadState(first.fd) == shared, "A-to-B-to-A borrows A's original cached window");
                BytesAt(first.fd, a, 9, 9); total += 9;
            }
            Check(pmm::checkedDescriptor == second.fd && pmm::FindReadState(first.fd) == shared,
                "ending the borrowed A scope restores B and leaves A's original state alive");
            BytesAt(second.fd, b, 3, 5); total += 5;
        }
        Check(pmm::checkedDescriptor == first.fd, "ending B restores the original A strict context");
        BytesAt(first.fd, a, 18, 7); total += 7;
    }
    Check(pmm::checkedDescriptor == -1 && _telli64(first.fd) == 25 && _telli64(second.fd) == 8 && pmm::readStatistics.bytes == total,
        "nested scopes restore both physical positions and fold each stream's bytes once");
}
void RemoveMiddleScopesWhileAnotherFdIsActive() {
    Configuration config;
    const auto a = Pattern(B + 41, 14), b = Pattern(B + 43, 15), c = Pattern(B + 47, 16);
    File first(a), middle(b), last(c);
    {
        pmm::ReadAhead outer(first.fd); pmm::CheckedReads strictA(first.fd);
        const auto* stateA = pmm::FindReadState(first.fd);
        BytesAt(first.fd, a, 0, 5);
        std::optional<pmm::ReadAhead> bufferB; bufferB.emplace(middle.fd);
        std::optional<pmm::CheckedReads> strictB; strictB.emplace(middle.fd);
        BytesAt(middle.fd, b, 0, 3);
        {
            pmm::ReadAhead bufferC(last.fd); pmm::CheckedReads strictC(last.fd);
            const auto* stateC = pmm::FindReadState(last.fd);
            BytesAt(last.fd, c, 0, 7);
            Check(stateA && stateC && pmm::FindReadState(middle.fd) && pmm::checkedDescriptor == last.fd,
                "three distinct live descriptors form real buffer and strict scope chains");
            strictB.reset();
            Check(pmm::checkedDescriptor == last.fd, "removing the middle strict scope preserves active C");
            bufferB.reset();
            Check(!pmm::FindReadState(middle.fd) && pmm::FindReadState(first.fd) == stateA &&
                pmm::FindReadState(last.fd) == stateC && _telli64(middle.fd) == 3,
                "removing the middle buffer synchronises only B and preserves both surrounding states");
            BytesAt(last.fd, c, 7, 4);
            BytesAt(first.fd, a, 5, 6);
            Check(pmm::checkedDescriptor == last.fd && pmm::readStatistics.bytes == 3,
                "remaining A and C consume their own windows while only removed B is folded");
        }
        Check(pmm::checkedDescriptor == first.fd && !pmm::FindReadState(last.fd) &&
            pmm::FindReadState(first.fd) == stateA && _telli64(last.fd) == 11,
            "C unwinds directly to A after its middle predecessor was removed");
    }
    Check(pmm::checkedDescriptor == -1 && !pmm::FindReadState(first.fd) && !pmm::FindReadState(middle.fd) &&
        !pmm::FindReadState(last.fd) && _telli64(first.fd) == 11 && _telli64(middle.fd) == 3 &&
        _telli64(last.fd) == 11 && pmm::readStatistics.bytes == 25,
        "non-LIFO removal leaves exact physical cursors and folds all three streams once");
}
void StandaloneDestructorSyncIsBestEffort() {
    Configuration config; const auto data = Pattern(B + 31, 17); File file(data);
    std::int64_t physical = -1; std::uint64_t seeks = 0;
    {
        pmm::ReadAhead buffer(file.fd);
        BytesAt(file.fd, data, 0, 7);
        physical = _telli64(file.fd); seeks = hooks.seeks;
        Check(physical > 7 && pmm::checkedDescriptor == -1, "standalone scope has real unread bytes without strictness");
        ArmSeekFailure(file.fd);
    }
    // Unlike Input::Close, a standalone nonowner scope must try to synchronise.
    // Its noexcept teardown cannot promise a rewound fd after an actual failure.
    Check(hooks.seekFailures == 1 && hooks.seeks == seeks + 1 && !pmm::FindReadState(file.fd) &&
        pmm::checkedDescriptor == -1 && _telli64(file.fd) == physical && pmm::Position(file.fd) == physical,
        "standalone destructor attempts sync once, unlinks safely and leaves a failed physical rewind observable");
    Check(pmm::readStatistics.requests == 1 && pmm::readStatistics.bytes == 7 &&
        _lseeki64(file.fd, 0, SEEK_CUR) == physical,
        "best-effort destructor folds only delivered bytes and does not close the independently owned descriptor");
}
void SentinelRemaining() {
    constexpr char text[] = "fresh-sentinel";
    Check(closeProbe.sentinel >= 0 && _telli64(closeProbe.sentinel) == 3, "close-hook reuse preserves the new file's deliberately distinct cursor");
    std::array<char, 40> actual{}; bool strict = false; int received = -1;
    try { received = pmm::Rd(closeProbe.sentinel, actual.data(), unsigned(actual.size())); }
    catch (const pmm::ShortRead&) { strict = true; }
    Check(!strict && received == int(sizeof(text) - 1 - 3) && !std::memcmp(actual.data(), text + 3, received),
        "the reused fd inherits neither old cached bytes nor a retired strict-read context");
    Check(_lseeki64(closeProbe.sentinel, 5, SEEK_SET) == 5, "mark the sentinel cursor before retired outer scopes unwind");
}
void ReleaseSentinel() {
    Check(closeProbe.sentinel >= 0 && _telli64(closeProbe.sentinel) == 5 && closeProbe.calls == 1,
        "retired outer destructors never seek or close the reused file");
    Check(_close(closeProbe.sentinel) == 0, "the sentinel is closed only by its actual test owner"); closeProbe.sentinel = -1;
    Check(DeleteFileW(closeProbe.path.c_str()) != FALSE, "remove only the owned close-hook sentinel file"); closeProbe = {};
}
void CloseInsideSameFdBorrowers() {
    Configuration config; const auto data = Pattern(B + 53, 9); File file(data); const int fd = file.Transfer();
    closeProbe = {}; closeProbe.fd = fd; closeProbe.path = root / (L"sentinel-" + std::to_wstring(++serial) + L".bin");
    closeProbe.reuse = true; closeProbe.expectedChecked = -1;
    {
        pmm::ReadAhead outer(fd); pmm::CheckedReads borrowedStrict(fd);
        pmm::ReadAhead innerBorrow(fd);
        {
            pmm::Input owner(fd); closeProbe.owner = &owner;
            allocationBudget = 0;
            const bool ahead = owner.EnableReadAhead(), strict = owner.EnableCheckedReads();
            const bool repeat = owner.EnableReadAhead() && owner.EnableCheckedReads(); allocationBudget = -1;
            Check(ahead && strict && repeat, "the sole owner activates and reuses read contexts without allocating");
            BytesAt(fd, data, 0, 7);
            allocationBudget = 0; owner.Close(); owner.Close(); allocationBudget = -1;
            Check(owner.Descriptor() == -1 && !owner.EnableReadAhead() && !owner.EnableCheckedReads() &&
                pmm::checkedDescriptor == -1 && !pmm::FindReadState(fd), "close retires all same-fd borrowers before its CRT callback reuses the number");
            SentinelRemaining();
        }
        Check(pmm::checkedDescriptor == -1 && !pmm::FindReadState(fd), "the consumed owner's destructor cannot resurrect a borrowed fd");
    }
    closeProbe.owner = nullptr;
    Check(pmm::checkedDescriptor == -1 && !pmm::FindReadState(fd), "retired checked and read-ahead scopes never restore their former descriptor");
    Check(pmm::readStatistics.bytes == 10, "retired borrowers fold the old seven bytes and new three bytes exactly once");
    ReleaseSentinel();
}
void CloseSuspendedOwner() {
    Configuration config; const auto a = Pattern(B + 59, 10), b = Pattern(B + 61, 11); File first(a), second(b);
    const int fd = first.Transfer(); closeProbe = {}; closeProbe.fd = fd;
    closeProbe.path = root / (L"sentinel-" + std::to_wstring(++serial) + L".bin"); closeProbe.reuse = true;
    {
        pmm::Input outer(fd); closeProbe.owner = &outer;
        Check(outer.EnableReadAhead() && outer.EnableCheckedReads(), "enable the outer sole input owner"); BytesAt(fd, a, 0, 7);
        {
            pmm::ReadAhead inner(second.fd); pmm::CheckedReads strictB(second.fd); BytesAt(second.fd, b, 0, 3);
            closeProbe.expectedChecked = second.fd;
            allocationBudget = 0; outer.Close(); allocationBudget = -1;
            Check(pmm::checkedDescriptor == second.fd && pmm::FindReadState(second.fd) && !pmm::FindReadState(fd),
                "closing suspended A preserves the still-active B state and strict context");
            SentinelRemaining(); BytesAt(second.fd, b, 3, 6);
        }
        Check(pmm::checkedDescriptor == -1 && !pmm::FindReadState(fd), "B exit cannot restore its retired previous A descriptor");
    }
    closeProbe.owner = nullptr;
    Check(pmm::readStatistics.bytes == 19, "suspended retirement preserves each file's counters without counting an old state twice");
    ReleaseSentinel();
}
void UnseekableFallback() {
    Configuration config; int descriptors[2]{-1, -1};
    Check(_pipe(descriptors, 256, _O_BINARY) == 0, "create a real valid unseekable binary pipe");
    constexpr char text[] = "pipe-data";
    Check(_write(descriptors[1], text, sizeof(text) - 1) == sizeof(text) - 1 && _close(descriptors[1]) == 0,
        "seed and close the pipe writer so EOF cannot block");
    {
        pmm::ReadAhead buffer(descriptors[0]);
        const auto* state = pmm::FindReadState(descriptors[0]);
        Check(state && !state->buffered, "failed initial pipe positioning selects the real unbuffered path");
        std::array<char, 32> actual{};
        Check(pmm::Rd(descriptors[0], actual.data(), unsigned(actual.size())) == sizeof(text) - 1 &&
            !std::memcmp(actual.data(), text, sizeof(text) - 1) && pmm::Rd(descriptors[0], actual.data(), 1) == 0,
            "a valid unseekable descriptor keeps ordinary partial-read and EOF behavior");
        Check(pmm::Position(descriptors[0]) == _telli64(descriptors[0]), "an unseekable fallback reports the CRT result rather than inventing a logical cursor");
    }
    Check(_close(descriptors[0]) == 0, "release the independently owned pipe reader");
}
struct Trace { decltype(pmm::readStatistics) live{}, folded{}; std::uint64_t calls = 0; };
Trace ScalarTrace(bool buffered) {
    const auto data = Pattern(B * 2, 12); File file(data); hooks = {}; pmm::bufferedReads = buffered; pmm::readStatistics = {};
    Trace trace;
    {
        pmm::ReadAhead buffer(file.fd); pmm::CheckedReads strict(file.fd);
        for (std::size_t at = 0; at < data.size(); at += 4) {
            std::array<unsigned char, 4> actual{};
            Check(pmm::Rd(file.fd, actual.data(), 4) == 4 && !std::memcmp(actual.data(), data.data() + at, 4),
                "many scalar fields remain byte-exact in the measured path");
        }
        trace.live = pmm::LiveReadStatistics(file.fd); trace.calls = hooks.reads;
    }
    trace.folded = pmm::readStatistics; SameStatistics(trace.live, trace.folded);
    Check(_telli64(file.fd) == data.size(), "scalar measurement leaves the real stream fully consumed"); return trace;
}
void SystemReadStatistics() {
    Configuration config; const auto buffered = ScalarTrace(true), direct = ScalarTrace(false);
    Check(buffered.calls > 0 && buffered.calls < direct.calls && direct.calls == (B * 2) / 4,
        "the real read-ahead path reduces actual CRT calls for the same scalar trace");
    for (const auto& trace : {buffered, direct})
        Check(trace.live.requests == (B * 2) / 4 && trace.live.bytes == B * 2 && trace.live.systemReads == trace.calls,
            "live counters agree with independently observed real read calls and delivered bytes");
    Check(buffered.live.buffered == B * 2 && direct.live.buffered == 0, "the A/B switch reports only bytes actually served through the window");
    std::printf("DIAG scalar fields=%u real CRT calls buffered=%llu direct=%llu\n", (B * 2) / 4,
        static_cast<unsigned long long>(buffered.calls), static_cast<unsigned long long>(direct.calls));
}
void ThreadIsolation() {
    Configuration config; std::atomic<unsigned> ready{0}; std::atomic<bool> go{false}; std::array<Trace, 2> result{};
    pmm::bufferedReads = true; pmm::readStatistics.requests = 123;
    const auto mainStats = pmm::readStatistics; const auto mainHooks = hooks;
    const auto worker = [&](unsigned index) {
        phase = index ? "thread-direct" : "thread-buffered";
        Check(pmm::bufferedReads && pmm::checkedDescriptor == -1, "each worker starts with independent default TLS configuration");
        pmm::bufferedReads = index == 0; ++ready;
        const auto deadline = GetTickCount64() + 5000;
        while (!go.load()) { Check(GetTickCount64() < deadline, "worker barrier completes without a hidden deadlock"); Sleep(1); }
        result[index] = ScalarTrace(index == 0);
        Check(pmm::checkedDescriptor == -1 && pmm::bufferedReads == (index == 0), "worker exit restores only its own strict context and switch");
    };
    std::thread first(worker, 0), second(worker, 1);
    const auto deadline = GetTickCount64() + 5000;
    while (ready.load() != 2) { Check(GetTickCount64() < deadline, "both real worker threads reach the barrier"); Sleep(1); }
    go = true; first.join(); second.join();
    Check(pmm::bufferedReads && pmm::checkedDescriptor == -1 && hooks.reads == mainHooks.reads && hooks.seeks == mainHooks.seeks,
        "worker reads and scopes never mutate the UI thread's hooks, switch or strict descriptor"); SameStatistics(pmm::readStatistics, mainStats);
    Check(result[0].calls < result[1].calls && result[0].live.buffered == B * 2 && result[1].live.buffered == 0,
        "concurrent opposite switches retain their own real read counts and windows");
}
void Run(const char* name, void (*test)()) {
    phase = name; std::printf("STAGE %s\n", name);
    Check(pmm::checkedDescriptor == -1, "a new group starts without a leaked strict scope");
    test();
    Check(pmm::checkedDescriptor == -1 && allocationBudget == -1, "a completed group leaves no strict scope or armed allocation failure");
    std::printf("PASS %s\n", name);
}
} // namespace

int __cdecl ObserveBufferRead(int fd, void* output, unsigned count) {
    ++hooks.reads;
    if (fd == hooks.readFd && ++hooks.readOrdinal == hooks.failReadAt) {
        ++hooks.readFailures; errno = EIO; return -1;
    }
    if (fd == hooks.limitedFd && hooks.maximumRead) count = (std::min)(count, hooks.maximumRead);
    return _read(fd, output, count);
}
__int64 __cdecl ObserveBufferSeek(int fd, __int64 offset, int origin) {
    ++hooks.seeks;
    if (fd == hooks.seekFd && ++hooks.seekOrdinal == hooks.failSeekAt) {
        ++hooks.seekFailures; errno = EIO; return -1;
    }
    return _lseeki64(fd, offset, origin);
}
int __cdecl ObserveBufferInputClose(int fd) noexcept {
    if (fd == closeProbe.fd) {
        ++closeProbe.calls;
        Check(closeProbe.owner && closeProbe.owner->Descriptor() == -1 && !pmm::FindReadState(fd) &&
            pmm::checkedDescriptor == closeProbe.expectedChecked, "the owner and every fd context retire before entering the actual CRT close");
    }
    const int status = _close(fd); // Always forward; never hide an invalid close.
    if (fd == closeProbe.fd && closeProbe.reuse) {
        closeProbe.reuse = false; Check(status == 0, "the real former input closes successfully exactly once");
        int sentinel = -1;
        Check(_wsopen_s(&sentinel, closeProbe.path.c_str(), _O_BINARY | _O_RDWR | _O_CREAT | _O_EXCL,
            _SH_DENYNO, _S_IREAD | _S_IWRITE) == 0 && sentinel >= 0, "the close callback opens an independent real sentinel");
        if (sentinel != fd) {
            Check(_dup2(sentinel, fd) == 0 && _close(sentinel) == 0, "the close callback deterministically reuses only the just-freed descriptor slot"); sentinel = fd;
        }
        closeProbe.sentinel = sentinel;
        constexpr char text[] = "fresh-sentinel";
        Check(_write(sentinel, text, sizeof(text) - 1) == sizeof(text) - 1 && _lseeki64(sentinel, 0, SEEK_SET) == 0,
            "seed the genuinely new file before returning from the close callback");
        {
            pmm::ReadAhead fresh(sentinel); pmm::CheckedReads strict(sentinel); std::array<char, 3> bytes{};
            Check(pmm::Rd(sentinel, bytes.data(), unsigned(bytes.size())) == int(bytes.size()) &&
                !std::memcmp(bytes.data(), text, bytes.size()), "a new scope inside the CRT close callback reads the new file's bytes");
        }
        Check(pmm::checkedDescriptor == closeProbe.expectedChecked && !pmm::FindReadState(sentinel) && _telli64(sentinel) == 3,
            "the new scope unwinds without reviving the retired old descriptor context");
    }
    return status;
}
void* operator new(std::size_t bytes) {
    if (allocationBudget == 0) throw std::bad_alloc();
    if (allocationBudget > 0) --allocationBudget;
    if (auto* value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

int main() {
    static_assert(!std::is_copy_constructible_v<pmm::Input> && !std::is_move_constructible_v<pmm::Input>);
    static_assert(noexcept(std::declval<pmm::Input&>().EnableReadAhead()) && noexcept(std::declval<pmm::Input&>().Close()));
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t executable[32768]{}; const auto count = GetModuleFileNameW(nullptr, executable, 32768);
    Check(count > 0 && count < 32768, "locate the actual CPU test executable");
    root = fs::path(executable).parent_path() / L"data" / L"pmm-read-buffer-tests" /
        (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Check(root.native().size() + 48 < MAX_PATH && fs::create_directories(root), "create a fresh bounded exe/data fixture directory");
    Run("boundary-and-direct", BoundaryAndDirectReads);
    Run("logical-seek-and-external-handoff", SeekAndExternalHandoff);
    Run("seek-and-sync-failures", SeekFailuresPreserveUnreadBytes);
    Run("eof-and-partial-fields", EofAndPartialFields);
    Run("backend-failure-and-short-reads", ReadFailuresAndShortBackingReads);
    Run("argument-refusal-and-no-allocation", InvalidArgumentsAndNoAllocation);
    Run("nested-and-suspended-reads", NestedAndSuspendedReads);
    Run("remove-middle-scopes-with-active-c", RemoveMiddleScopesWhileAnotherFdIsActive);
    Run("standalone-destructor-sync-best-effort", StandaloneDestructorSyncIsBestEffort);
    Run("close-inside-same-fd-borrowers", CloseInsideSameFdBorrowers);
    Run("close-suspended-owner", CloseSuspendedOwner);
    Run("real-unseekable-pipe-fallback", UnseekableFallback);
    Run("real-system-read-statistics", SystemReadStatistics);
    Run("thread-isolation", ThreadIsolation);
    std::printf("PASS PMM read-ahead CPU contracts: %u checks\n", checks.load());
    return 0;
}
