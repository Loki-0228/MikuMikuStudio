// CPU-only tests of the real CRT descriptor owner. No PMM parser, application,
// renderer or modal UI is substituted for these focused lifetime checks.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>

// Include the CRT and read helpers before instrumenting only Input's close.
// The observer always forwards to the real CRT; it never hides an invalid fd.
#include "../src/io/pmm_io_common.hpp"
int ObservePmmInputClose(int descriptor) noexcept;
#define _close ::ObservePmmInputClose
#include "../src/io/pmm_input.hpp"
#undef _close

namespace {
thread_local long failAfter = -1;
using mikudancestudio::pmm_io::Input;
namespace pmm = mikudancestudio::pmm_io;
unsigned checks = 0;
struct CloseEvent { int descriptor, checked; bool consumed; };
std::array<CloseEvent, 128> closeEvents{};
std::size_t closeCount = 0;
const Input* observedOwner = nullptr;
std::filesystem::path root;
unsigned fileSerial = 0;
void Check(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
}
int File(const char* contents = "PMM-input") {
    const auto path = root / (L"input-" + std::to_wstring(++fileSerial) + L".bin");
    int descriptor = -1;
    Check(_wsopen_s(&descriptor, path.c_str(), _O_BINARY | _O_RDWR | _O_CREAT | _O_TRUNC,
        _SH_DENYNO, _S_IREAD | _S_IWRITE) == 0 && descriptor >= 0, "open an isolated real CRT file");
    const auto count = static_cast<unsigned>(std::strlen(contents));
    Check(_write(descriptor, contents, count) == static_cast<int>(count) && _lseek(descriptor, 0, SEEK_SET) == 0,
        "seed and rewind an isolated CRT file");
    return descriptor;
}
int Reuse(int former) {
    int sentinel = File("");
    if (sentinel != former) {
        // The former number was just consumed by this single-threaded fixture.
        // Force a valid duplicate into that known free slot; do not infer file
        // ownership from whether a descriptor currently happens to be open.
        Check(_dup2(sentinel, former) == 0, "force a deterministic real CRT descriptor-number reuse");
        Check(_close(sentinel) == 0, "release the original duplicate sentinel descriptor");
        sentinel = former;
    }
    Check(sentinel == former, "sentinel has exactly the previously closed PMM descriptor number");
    return sentinel;
}
void SentinelIo(int descriptor) {
    constexpr char content[] = "still-owned";
    constexpr int length = sizeof(content) - 1;
    Check(_write(descriptor, content, length) == length && _lseek(descriptor, 0, SEEK_SET) == 0,
        "the independently owned reused descriptor remains writable");
    char actual[sizeof(content) + 5]{};
    bool threw = false; int received = -1;
    try { received = pmm::Rd(descriptor, actual, sizeof(actual)); }
    catch (const pmm::ShortRead&) { threw = true; }
    Check(!threw && received == length && std::memcmp(actual, content, length) == 0,
        "a reused descriptor is readable and never inherits the closed PMM short-read context");
}
void CloseWasConsumed(std::size_t event, int descriptor, int previousCheck) {
    Check(event < closeCount && closeEvents[event].descriptor == descriptor && closeEvents[event].consumed &&
        closeEvents[event].checked == previousCheck,
        "the fd slot is consumed and the prior read context restored before the real CRT close");
}
void EmptyAndExplicitClose() {
    const auto start = closeCount; const auto previous = pmm::checkedDescriptor;
    {
        Input empty; observedOwner = &empty;
        failAfter = 0;
        const bool enabled = empty.EnableCheckedReads(); empty.Close(); empty.Close();
        failAfter = -1;
        Check(empty.Descriptor() == -1 && !enabled && closeCount == start && pmm::checkedDescriptor == previous,
            "an empty owner does not activate checks allocate or close an unrelated descriptor");
    }
    observedOwner = nullptr;
    const int descriptor = File();
    {
        failAfter = 0;
        Input input(descriptor); observedOwner = &input;
        const bool enabled = input.EnableCheckedReads();
        const bool repeated = input.EnableCheckedReads();
        const bool active = pmm::checkedDescriptor == descriptor;
        input.Close(); input.Close();
        const bool disabled = !input.EnableCheckedReads();
        failAfter = -1;
        Check(enabled && repeated && active && disabled && input.Descriptor() == -1 && closeCount == start + 1,
            "activation and repeated explicit close need no allocation and consume a real descriptor once");
        CloseWasConsumed(start, descriptor, previous);
        const int sentinel = Reuse(descriptor);
        input.Close();
        Check(closeCount == start + 1, "a repeated close cannot close a later file with the same number");
        SentinelIo(sentinel); Check(_close(sentinel) == 0, "close the sentinel only through its own owner");
    }
    observedOwner = nullptr;
    Check(closeCount == start + 1 && pmm::checkedDescriptor == previous, "an explicitly closed owner has an inert destructor");
}
void DestructorAndUncheckedReads() {
    const int previous = pmm::checkedDescriptor;
    const auto start = closeCount; const int descriptor = File("v1");
    {
        Input input(descriptor); observedOwner = &input;
        char bytes[8]{};
        Check(pmm::Rd(input.Descriptor(), bytes, sizeof(bytes)) == 2 && pmm::Rd(input.Descriptor(), bytes, 1) == 0,
            "a default input preserves unchecked V1 optional-tail EOF behavior");
        Check(pmm::checkedDescriptor == previous, "a default input does not silently enable V2 checked reads");
        failAfter = 0;
    }
    failAfter = -1; observedOwner = nullptr;
    Check(closeCount == start + 1, "a still-open owner closes exactly once during allocation-free destruction");
    CloseWasConsumed(start, descriptor, previous);
    const int sentinel = Reuse(descriptor); SentinelIo(sentinel); Check(_close(sentinel) == 0, "release the destructor-reuse sentinel");
}
void ShortReadAndNestedRestoration() {
    const int previous = pmm::checkedDescriptor;
    const int outerFd = File("abc"), innerFd = File("z");
    const auto start = closeCount;
    {
        Input outer(outerFd); Check(outer.EnableCheckedReads(), "enable a real outer input read context");
        {
            Input inner(innerFd); Check(inner.EnableCheckedReads(), "enable a distinct nested input read context");
            observedOwner = &inner;
            char bytes[4]{}; bool rejected = false;
            try { pmm::Rd(inner.Descriptor(), bytes, 3); }
            catch (const pmm::ShortRead& error) { rejected = error.offset == 0 && error.expected == 3 && error.received == 1; }
            Check(rejected && pmm::checkedDescriptor == innerFd, "V2 short reads retain the exact offset and byte-count diagnostics");
            failAfter = 0; inner.Close(); failAfter = -1;
            Check(pmm::checkedDescriptor == outerFd && inner.Descriptor() == -1,
                "closing the nested stream restores the still-live outer read context");
            CloseWasConsumed(start, innerFd, outerFd);
            const int sentinel = Reuse(innerFd); SentinelIo(sentinel); Check(_close(sentinel) == 0, "release the nested sentinel");
        }
        Check(pmm::checkedDescriptor == outerFd, "the consumed nested input destructor does not restore a stale TLS number");
        char bytes[4]{}; bool rejected = false;
        try { pmm::Rd(outer.Descriptor(), bytes, sizeof(bytes)); }
        catch (const pmm::ShortRead& error) { rejected = error.offset == 0 && error.expected == 4 && error.received == 3; }
        Check(rejected, "the restored outer stream still enforces its own V2 short-read rule");
        observedOwner = &outer; failAfter = 0; outer.Close(); failAfter = -1;
        CloseWasConsumed(start + 1, outerFd, previous);
    }
    observedOwner = nullptr;
    Check(closeCount == start + 2 && pmm::checkedDescriptor == previous, "nested closed owners leave no resource or TLS context behind");

    const int borrowed = File("borrowed"), owned = File("owned");
    {
        pmm::CheckedReads external(borrowed);
        {
            Input input(owned); observedOwner = &input;
            Check(input.EnableCheckedReads() && pmm::checkedDescriptor == owned, "an input can nest inside an existing borrowed checked-read scope");
            failAfter = 0; input.Close(); failAfter = -1;
            Check(pmm::checkedDescriptor == borrowed, "closing the input restores an existing external check context");
        }
        observedOwner = nullptr;
        Check(pmm::checkedDescriptor == borrowed, "a consumed input destructor preserves the external scope");
    }
    Check(pmm::checkedDescriptor == previous && _close(borrowed) == 0, "the borrowed descriptor remains independently owned and its TLS scope restores normally");
}
enum class Fault { Memory, Runtime };
void Raise(Fault fault) {
    if (fault == Fault::Memory) throw std::bad_alloc();
    throw std::runtime_error("test failure after stream read stage");
}
void ExceptionCleanup(Fault fault, bool closeBeforeThrow, bool reuse) {
    const int previous = pmm::checkedDescriptor;
    const auto start = closeCount; const int descriptor = File();
    int sentinel = -1; bool caught = false;
    try {
        Input input(descriptor); observedOwner = &input;
        Check(input.EnableCheckedReads(), "enable checked reads before a controlled C++ failure");
        if (closeBeforeThrow) {
            input.Close();
            Check(input.Descriptor() == -1 && pmm::checkedDescriptor == previous,
                "a successful read-stage close has already released ownership and TLS before the tail");
            if (reuse) sentinel = Reuse(descriptor);
        }
        Raise(fault);
    } catch (const std::bad_alloc&) { caught = fault == Fault::Memory; }
    catch (const std::runtime_error&) { caught = fault == Fault::Runtime; }
    observedOwner = nullptr;
    Check(caught && closeCount == start + 1 && pmm::checkedDescriptor == previous,
        "memory and runtime failures before or after explicit close clean up exactly once");
    CloseWasConsumed(start, descriptor, previous);
    if (reuse) { SentinelIo(sentinel); Check(_close(sentinel) == 0, "tail failure leaves the reused descriptor for its real owner to close"); }
    else {
        sentinel = Reuse(descriptor); SentinelIo(sentinel); Check(_close(sentinel) == 0, "the closed failed input descriptor can be independently reused");
    }
}
} // namespace

int ObservePmmInputClose(int descriptor) noexcept {
    if (closeCount < closeEvents.size()) closeEvents[closeCount] =
        {descriptor, pmm::checkedDescriptor, observedOwner && observedOwner->Descriptor() == -1};
    ++closeCount;
    return _close(descriptor);
}
void* operator new(std::size_t bytes) {
    if (failAfter == 0) throw std::bad_alloc();
    if (failAfter > 0) --failAfter;
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main() {
    static_assert(!std::is_copy_constructible_v<Input> && !std::is_copy_assignable_v<Input> &&
        !std::is_move_constructible_v<Input> && !std::is_move_assignable_v<Input>);
    static_assert(std::is_nothrow_constructible_v<Input, int> && std::is_nothrow_destructible_v<Input>);
    wchar_t executable[32768]{}; const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    Check(length > 0 && length < 32768, "resolve the test executable's own storage root");
    root = std::filesystem::path(executable).parent_path() / L"data" / L"pmm-input-tests" /
        (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Check(std::filesystem::create_directories(root), "create a dedicated exe/data fixture directory");
    EmptyAndExplicitClose(); DestructorAndUncheckedReads(); ShortReadAndNestedRestoration();
    for (const auto fault : {Fault::Memory, Fault::Runtime}) {
        ExceptionCleanup(fault, false, false);
        ExceptionCleanup(fault, true, false);
        ExceptionCleanup(fault, true, true);
    }
    Check(closeCount < closeEvents.size() && pmm::checkedDescriptor == -1, "all close observations fit and no checked input remains active");
    std::printf("PASS PMM input ownership: %u checks; %zu real owned closes\n", checks, closeCount);
    return 0;
}
