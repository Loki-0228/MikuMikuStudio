#include "mikumikustudio/media_process_retirement.hpp"
#include <cassert>
#include <new>
#include <utility>

namespace mikumikustudio::media {
namespace detail {
struct ProcessRetirementNode {
    std::shared_ptr<const void> source;
    HANDLE process = nullptr;
    ProcessRetirementNode* next = nullptr;
    bool terminationAttempted = false;
};
struct ProcessRetirementState {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE wake = nullptr, thread = nullptr;
    ProcessRetirementNode* pending = nullptr;
    std::size_t reserved = 0, pendingCount = 0;
    DWORD lastError = ERROR_SUCCESS;
    bool closing = false;
    ~ProcessRetirementState() noexcept {
        assert(!pending && reserved == 0);
        if (thread) CloseHandle(thread);
        if (wake) CloseHandle(wake);
    }
};
struct Guard {
    SRWLOCK& lock;
    explicit Guard(SRWLOCK& value) noexcept : lock(value) { AcquireSRWLockExclusive(&lock); }
    ~Guard() { ReleaseSRWLockExclusive(&lock); }
};
struct Operation {
    bool& flag;
    explicit Operation(bool& value) noexcept : flag(value) { flag = true; }
    ~Operation() { flag = false; }
};
void Error(ProcessRetirementState& state, DWORD error) noexcept {
    Guard guard(state.lock); state.lastError = error;
}
DWORD WINAPI Reap(void* opaque) noexcept {
    std::unique_ptr<std::shared_ptr<ProcessRetirementState>> startup(
        static_cast<std::shared_ptr<ProcessRetirementState>*>(opaque));
    auto state = std::move(*startup);
    startup.reset();
    for (;;) {
        ProcessRetirementNode* batch = nullptr;
        {
            Guard guard(state->lock);
            if (state->closing && state->reserved == 0) return 0;
            batch = state->pending; state->pending = nullptr;
        }
        bool stillRunning = false;
        while (batch) {
            auto* node = batch; batch = node->next; node->next = nullptr;
            DWORD result = WaitForSingleObject(node->process, 0);
            if (result == WAIT_FAILED) Error(*state, GetLastError());
            if (result != WAIT_OBJECT_0 && !node->terminationAttempted) {
                node->terminationAttempted = true;
                if (!TerminateProcess(node->process, 0)) Error(*state, GetLastError());
                result = WaitForSingleObject(node->process, 0);
                if (result == WAIT_FAILED) Error(*state, GetLastError());
            }
            if (result == WAIT_OBJECT_0) {
                CloseHandle(node->process);
                // Destruction may execute arbitrary source cleanup. No queue
                // lock is held, and capacity remains reserved until it ends.
                delete node;
                Guard guard(state->lock);
                --state->reserved; --state->pendingCount;
            } else {
                stillRunning = true;
                Guard guard(state->lock);
                node->next = state->pending; state->pending = node;
            }
        }
        {
            Guard guard(state->lock);
            if (state->closing && state->reserved == 0) return 0;
            stillRunning = stillRunning || state->pending != nullptr;
        }
        const DWORD wait = WaitForSingleObject(state->wake, stillRunning ? 20 : INFINITE);
        if (wait == WAIT_FAILED) { Error(*state, GetLastError()); Sleep(20); }
    }
}
}

PreparedProcess::PreparedProcess() noexcept = default;
PreparedProcess::~PreparedProcess() noexcept { Retire(); }
PreparedProcess::PreparedProcess(PreparedProcess&& other) noexcept {
    if (other.operating_) return;
    detail::Operation own(operating_), source(other.operating_);
    state_ = std::move(other.state_); node_ = std::exchange(other.node_, nullptr);
}
PreparedProcess& PreparedProcess::operator=(PreparedProcess&& other) noexcept {
    if (this == &other || operating_ || other.operating_) return *this;
    detail::Operation own(operating_), source(other.operating_);
    RetireOwned();
    state_ = std::move(other.state_); node_ = std::exchange(other.node_, nullptr);
    return *this;
}
bool PreparedProcess::IsValid() const noexcept { return node_ != nullptr; }
HANDLE PreparedProcess::Handle() const noexcept { return node_ ? node_->process : nullptr; }
void PreparedProcess::AdoptStarted(HANDLE process) noexcept {
    assert(!operating_ && node_ && !node_->process && process && process != INVALID_HANDLE_VALUE);
    node_->process = process;
}
PreparedProcess::Retirement PreparedProcess::Retire() noexcept {
    if (operating_) return Retirement::Busy;
    detail::Operation operation(operating_);
    return RetireOwned();
}
PreparedProcess::Retirement PreparedProcess::RetireOwned() noexcept {
    if (!node_) return Retirement::Empty;
    auto state = std::move(state_);
    auto* node = std::exchange(node_, nullptr);
    if (!node->process) {
        delete node;
        { detail::Guard guard(state->lock); --state->reserved; }
        SetEvent(state->wake);
        return Retirement::Empty;
    }
    {
        detail::Guard guard(state->lock);
        node->next = state->pending; state->pending = node; ++state->pendingCount;
    }
    SetEvent(state->wake);
    return Retirement::Pending;
}

ProcessRetirementQueue::ProcessRetirementQueue(std::size_t capacity) noexcept : capacity_(capacity) {}
ProcessRetirementQueue::~ProcessRetirementQueue() noexcept {
    std::shared_ptr<detail::ProcessRetirementState> state;
    { detail::Guard guard(lock_); state.swap(state_); }
    if (state) {
        { detail::Guard guard(state->lock); state->closing = true; }
        SetEvent(state->wake);
    }
}
ProcessRetirementQueue::Result ProcessRetirementQueue::Prepare(
    std::shared_ptr<const void> source, PreparedProcess& out) noexcept {
    if (out.operating_) return Result::Busy;
    detail::Operation operation(out.operating_);
    // Function parameters are destroyed after local variables. Move the pin
    // into a local declared after the operation guard so even early failure
    // cleanup cannot reenter an unguarded output token.
    auto ownedSource = std::move(source);
    if (out.IsValid()) return Result::Busy;
    if (!capacity_) return Result::Invalid;
    try {
        std::shared_ptr<detail::ProcessRetirementState> state;
        {
            detail::Guard guard(lock_);
            if (!state_) {
                auto candidate = std::make_shared<detail::ProcessRetirementState>();
                candidate->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!candidate->wake) return Result::Unavailable;
                auto startup = std::make_unique<std::shared_ptr<detail::ProcessRetirementState>>(candidate);
                candidate->thread = CreateThread(nullptr, 0, detail::Reap, startup.get(), 0, nullptr);
                if (!candidate->thread) return Result::Unavailable;
                startup.release();
                state_.swap(candidate);
            }
            state = state_;
        }
        auto node = std::make_unique<detail::ProcessRetirementNode>();
        node->source = std::move(ownedSource);
        {
            detail::Guard guard(state->lock);
            if (state->closing) return Result::Unavailable;
            if (state->reserved >= capacity_) return Result::Busy;
            ++state->reserved;
        }
        out.state_ = std::move(state); out.node_ = node.release();
        return Result::Ok;
    } catch (const std::bad_alloc&) { return Result::NoMemory; }
      catch (...) { return Result::Unavailable; }
}
ProcessRetirementQueue::Status ProcessRetirementQueue::Snapshot() const noexcept {
    std::shared_ptr<detail::ProcessRetirementState> state;
    { detail::Guard guard(lock_); state = state_; }
    if (!state) return {};
    detail::Guard guard(state->lock);
    return {state->reserved, state->pendingCount, state->lastError};
}
}
