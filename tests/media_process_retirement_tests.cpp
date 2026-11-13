#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <MinHook.h>
#include "mikumikustudio/media_process_retirement.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace { thread_local int failAllocation = -1; }
void* operator new(std::size_t size) {
    if (failAllocation >= 0 && failAllocation-- == 0) throw std::bad_alloc();
    if (auto* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

using namespace mikumikustudio::media;
namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s error=%lu\n", message, GetLastError()); throw std::runtime_error(message); }
}
using WaitFunction = DWORD(WINAPI*)(HANDLE, DWORD);
using TerminateFunction = BOOL(WINAPI*)(HANDLE, UINT);
WaitFunction realWait = nullptr;
TerminateFunction realTerminate = nullptr;
std::atomic<HANDLE> intercepted{nullptr};
std::atomic<unsigned> waitMode{0}, waitCalls{0}, terminateCalls{0};
DWORD WINAPI FaultWait(HANDLE handle, DWORD milliseconds) {
    if (handle == intercepted.load() && waitMode.load()) {
        ++waitCalls;
        if (waitMode.load() == 2) { SetLastError(ERROR_ACCESS_DENIED); return WAIT_FAILED; }
        return WAIT_TIMEOUT;
    }
    return realWait(handle, milliseconds);
}
BOOL WINAPI FaultTerminate(HANDLE handle, UINT code) {
    if (handle == intercepted.load()) {
        ++terminateCalls; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
    }
    return realTerminate(handle, code);
}
struct Hooks {
    Hooks() {
        Check(MH_Initialize() == MH_OK, "initialize process-only fault hooks");
        Check(MH_CreateHook(reinterpret_cast<void*>(&WaitForSingleObject), reinterpret_cast<void*>(&FaultWait),
            reinterpret_cast<void**>(&realWait)) == MH_OK, "create wait hook");
        Check(MH_CreateHook(reinterpret_cast<void*>(&TerminateProcess), reinterpret_cast<void*>(&FaultTerminate),
            reinterpret_cast<void**>(&realTerminate)) == MH_OK, "create terminate hook");
        Check(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "enable process-only hooks");
    }
    ~Hooks() { intercepted = nullptr; waitMode = 0; MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize(); }
};
template<class Predicate> bool Eventually(Predicate predicate) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    do { if (predicate()) return true; Sleep(5); } while (GetTickCount64() < deadline);
    return predicate();
}
struct Child {
    HANDLE event = nullptr, observer = nullptr;
    explicit Child(PreparedProcess& token) {
        static std::atomic<unsigned> sequence{0};
        const auto name = L"Local\\MMS-Retirement-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
        event = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        Check(event != nullptr && GetLastError() != ERROR_ALREADY_EXISTS, "create private child exit event");
        wchar_t executable[32768]{};
        const auto length = GetModuleFileNameW(nullptr, executable, 32768);
        Check(length && length < 32768, "resolve own test executable");
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --child \"" + name + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process) != FALSE, "start only this test's child");
        // The handoff itself is proven to allocate nothing.
        failAllocation = 0; token.AdoptStarted(process.hProcess); failAllocation = -1;
        CloseHandle(process.hThread);
        Check(DuplicateHandle(GetCurrentProcess(), token.Handle(), GetCurrentProcess(), &observer,
            SYNCHRONIZE, FALSE, 0) != FALSE, "create independent exit observer");
    }
    ~Child() {
        if (event) SetEvent(event);
        if (observer) { WaitForSingleObject(observer, 5000); CloseHandle(observer); }
        if (event) CloseHandle(event);
    }
    void Exit() { Check(SetEvent(event) != FALSE, "signal child exit"); Check(WaitForSingleObject(observer, 5000) == WAIT_OBJECT_0, "observe real child exit"); }
};
struct Source {
    ProcessRetirementQueue* queue = nullptr;
    std::atomic<unsigned>* destroyed = nullptr;
    ~Source() {
        // If source destruction holds the queue lock, this reentry deadlocks.
        if (queue) (void)queue->Snapshot();
        if (destroyed) ++*destroyed;
    }
};
struct ReentrantSource {
    ProcessRetirementQueue* queue = nullptr;
    PreparedProcess* target = nullptr;
    PreparedProcess* incoming = nullptr;
    PreparedProcess* spare = nullptr;
    unsigned* calls = nullptr;
    bool* correct = nullptr;
    ~ReentrantSource() {
        ++*calls;
        *correct = *correct && queue->Prepare({}, *target) == ProcessRetirementQueue::Result::Busy;
        *correct = *correct && target->Retire() == PreparedProcess::Retirement::Busy;
        *target = std::move(*spare);
        *correct = *correct && spare->IsValid();
        *spare = std::move(*target);
        *correct = *correct && spare->IsValid();
        PreparedProcess rejected(std::move(*target));
        *correct = *correct && !rejected.IsValid();
        if (incoming) {
            *correct = *correct && incoming->Retire() == PreparedProcess::Retirement::Busy;
            *spare = std::move(*incoming);
            *correct = *correct && incoming->IsValid() && spare->IsValid();
            PreparedProcess alsoRejected(std::move(*incoming));
            *correct = *correct && !alsoRejected.IsValid() && incoming->IsValid();
        }
        // This successful operation proves callbacks remain outside locks and
        // only the operands of the active operation are protected.
        PreparedProcess independent;
        *correct = *correct && queue->Prepare({}, independent) == ProcessRetirementQueue::Result::Ok;
    }
};
void SameTokenReentry() {
    for (unsigned action = 0; action != 3; ++action) {
        ProcessRetirementQueue queue(8);
        auto target = std::make_unique<PreparedProcess>();
        PreparedProcess incoming, spare;
        Check(queue.Prepare({}, spare) == ProcessRetirementQueue::Result::Ok, "prepare reentry spare");
        if (action == 1) Check(queue.Prepare({}, incoming) == ProcessRetirementQueue::Result::Ok, "prepare incoming move reservation");
        unsigned calls = 0; bool correct = true;
        auto source = std::make_shared<ReentrantSource>();
        source->queue = &queue; source->target = target.get(); source->spare = &spare;
        source->incoming = action == 1 ? &incoming : nullptr;
        source->calls = &calls; source->correct = &correct;
        Check(queue.Prepare(source, *target) == ProcessRetirementQueue::Result::Ok, "prepare source with same-token cleanup callback");
        source.reset();
        if (action == 0) target->Retire();
        else if (action == 1) *target = std::move(incoming);
        else target.reset();
        Check(calls == 1 && correct, "cleanup rejects same-output Prepare, recursive retire and moves involving busy operands");
        Check(spare.IsValid() && !incoming.IsValid(), "rejected reentrant moves preserve responsibility");
        Check(queue.Snapshot().reserved == (action == 1 ? 2u : 1u), "reentrant cleanup leaves no leaked reservations");
        if (target) target->Retire();
        spare.Retire();
        Check(queue.Snapshot().reserved == 0, "all reservation capacity reclaimed after reentry");
    }
}
void FailedPrepareSourceReentry() {
    for (unsigned action = 0; action != 3; ++action) {
        ProcessRetirementQueue healthy(8), failing(action == 0 ? 0 : 8);
        PreparedProcess target, spare;
        Check(healthy.Prepare({}, spare) == ProcessRetirementQueue::Result::Ok, "prepare failed-prepare reentry spare");
        const bool initiallyOccupied = action == 2;
        if (initiallyOccupied) Check(healthy.Prepare({}, target) == ProcessRetirementQueue::Result::Ok, "prepare occupied output before Busy failure");
        unsigned calls = 0; bool correct = true;
        auto source = std::make_shared<ReentrantSource>();
        source->queue = &healthy; source->target = &target; source->spare = &spare;
        source->calls = &calls; source->correct = &correct;
        if (action == 1) failAllocation = 0;
        const auto result = failing.Prepare(std::move(source), target);
        failAllocation = -1;
        const auto expected = action == 0 ? ProcessRetirementQueue::Result::Invalid :
            action == 1 ? ProcessRetirementQueue::Result::NoMemory : ProcessRetirementQueue::Result::Busy;
        Check(result == expected && calls == 1 && correct, "early Prepare failure destroys source while output remains protected");
        Check(target.IsValid() == initiallyOccupied && spare.IsValid(), "failure-time source reentry cannot alter original output or spare");
        Check(healthy.Snapshot().reserved == (initiallyOccupied ? 2u : 1u) && failing.Snapshot().reserved == 0,
            "Invalid OOM and occupied Busy preserve reservation accounting");
        target.Retire(); spare.Retire();
        Check(healthy.Snapshot().reserved == 0, "failure reentry leaves no reservation behind");
    }
}
void PreparedAndFailures() {
    PreparedProcess empty;
    Check(!empty.IsValid() && !empty.Handle() && empty.Retire() == PreparedProcess::Retirement::Empty, "empty retirement is idempotent");
    ProcessRetirementQueue zero(0);
    Check(zero.Prepare({}, empty) == ProcessRetirementQueue::Result::Invalid && !empty.IsValid(), "zero capacity rejects before launch");
    for (int failure = 0; failure != 4; ++failure) {
        ProcessRetirementQueue queue;
        PreparedProcess token;
        failAllocation = failure;
        const auto result = queue.Prepare({}, token);
        failAllocation = -1;
        Check(result == ProcessRetirementQueue::Result::NoMemory || result == ProcessRetirementQueue::Result::Ok, "startup allocation failure translated");
        Check((result == ProcessRetirementQueue::Result::Ok) == token.IsValid(), "allocation failure preserves empty output");
        token.Retire();
        Check(queue.Snapshot().reserved == 0, "unstarted reservation releases capacity");
    }
    ProcessRetirementQueue queue(1);
    PreparedProcess token, second;
    auto source = std::make_shared<Source>(); std::weak_ptr<Source> weak = source;
    Check(queue.Prepare(source, token) == ProcessRetirementQueue::Result::Ok, "reserve unstarted source"); source.reset();
    Check(queue.Prepare({}, token) == ProcessRetirementQueue::Result::Busy && token.IsValid(), "nonempty output retained");
    Check(queue.Prepare({}, second) == ProcessRetirementQueue::Result::Busy && !second.IsValid(), "capacity includes unstarted reservations");
    Check(!weak.expired(), "unstarted reservation owns source");
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    // No child is launched here. The reserved token must remain safely
    // destructible when the actual launch operation fails.
    Check(!CreateProcessW(L"\\\\?\\MMS-process-retirement-no-such-device\\missing.exe", nullptr,
        nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process),
        "real child startup failure creates no owned process");
    Check(!token.Handle(), "failed launch never adopts an invalid process handle");
    token.Retire(); Check(weak.expired(), "failed CreateProcess equivalent releases unstarted source");
    Check(queue.Prepare({}, second) == ProcessRetirementQueue::Result::Ok, "capacity reusable after failed start");
}
void FailedWaitAndTermination(unsigned mode) {
    ProcessRetirementQueue queue(1);
    std::atomic<unsigned> destroyed{0};
    auto source = std::make_shared<Source>(); source->queue = &queue; source->destroyed = &destroyed;
    std::weak_ptr<Source> weak = source;
    PreparedProcess token, blocked;
    Check(queue.Prepare(source, token) == ProcessRetirementQueue::Result::Ok, "reserve child source"); source.reset();
    Child child(token);
    intercepted = token.Handle(); waitMode = mode; waitCalls = 0; terminateCalls = 0;
    const auto start = GetTickCount64();
    failAllocation = 0; const auto retired = token.Retire(); failAllocation = -1;
    Check(retired == PreparedProcess::Retirement::Pending && !token.IsValid() && GetTickCount64() - start < 1000, "retirement nonblocking and allocation free");
    Check(Eventually([] { return terminateCalls.load() != 0 && waitCalls.load() >= 2; }), "exercise failed terminate and uncertain wait");
    Check(!weak.expired() && destroyed == 0 && queue.Snapshot().pending == 1, "uncertain process keeps source and process ownership");
    Check(queue.Prepare({}, blocked) == ProcessRetirementQueue::Result::Busy, "unconfirmed exit consumes bounded capacity");
    Check(queue.Snapshot().lastError == ERROR_ACCESS_DENIED, "failed process operation retained in diagnostics");
    child.Exit();
    // Even a genuinely exited process may not be released until its own wait
    // confirms that state; the faulted handle still reports uncertainty.
    Sleep(50); Check(!weak.expired(), "actual exit alone does not bypass uncertain wait");
    waitMode = 0;
    Check(Eventually([&] { return weak.expired() && destroyed == 1 && queue.Snapshot().reserved == 0; }), "confirmed exit reclaims outside locks");
    intercepted = nullptr;
    Check(token.Retire() == PreparedProcess::Retirement::Empty, "repeat retire cannot release twice");
}
void QueueBeforeTokens() {
    PreparedProcess token;
    auto source = std::make_shared<Source>(); std::weak_ptr<Source> weak = source;
    auto queue = std::make_unique<ProcessRetirementQueue>(1);
    Check(queue->Prepare(source, token) == ProcessRetirementQueue::Result::Ok, "prepare before queue shutdown"); source.reset();
    Child child(token);
    intercepted = token.Handle(); waitMode = 1; waitCalls = 0; terminateCalls = 0;
    queue.reset();
    Check(!weak.expired() && token.Handle(), "destroy queue before live token preserves child and source");
    token.Retire();
    Check(Eventually([] { return terminateCalls.load() != 0; }) && !weak.expired(), "orphaned queue continues retirement");
    child.Exit(); waitMode = 0;
    Check(Eventually([&] { return weak.expired(); }), "self-owned reaper ends after delayed token exits");
    intercepted = nullptr;
    PreparedProcess unstarted;
    { ProcessRetirementQueue shortLived; Check(shortLived.Prepare({}, unstarted) == ProcessRetirementQueue::Result::Ok, "prepare token outliving queue without launch"); }
    unstarted.Retire();
}
void MultipleProcessesAndThreads() {
    ProcessRetirementQueue queue(4);
    PreparedProcess tokens[4];
    std::unique_ptr<Child> children[4];
    for (unsigned i = 0; i < 4; ++i) {
        Check(queue.Prepare({}, tokens[i]) == ProcessRetirementQueue::Result::Ok, "reserve simultaneous child");
        children[i] = std::make_unique<Child>(tokens[i]);
    }
    std::thread threads[4];
    for (unsigned i = 0; i < 4; ++i) threads[i] = std::thread([&, i] { tokens[i].Retire(); });
    for (auto& thread : threads) thread.join();
    Check(Eventually([&] { return queue.Snapshot().reserved == 0; }), "simultaneous real process termination observed");
    for (auto& child : children) Check(WaitForSingleObject(child->observer, 0) == WAIT_OBJECT_0, "all test children really exited");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wcscmp(argv[1], L"--child") == 0) {
        HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
        if (!event) return 7;
        const auto wait = WaitForSingleObject(event, 15000); CloseHandle(event);
        return wait == WAIT_OBJECT_0 ? 0 : 8;
    }
    try {
        Hooks hooks;
        PreparedAndFailures(); SameTokenReentry(); FailedPrepareSourceReentry();
        FailedWaitAndTermination(1); FailedWaitAndTermination(2);
        QueueBeforeTokens(); MultipleProcessesAndThreads();
        std::printf("PASS media process retirement %u checks; real isolated children, uncertain waits and termination failures retain sources\n", checks);
        return 0;
    } catch (const std::exception& error) {
        failAllocation = -1; std::fprintf(stderr, "FAIL media process retirement: %s\n", error.what()); return 1;
    }
}
