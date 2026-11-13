#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstddef>
#include <memory>

namespace mikumikustudio::media {
namespace detail { struct ProcessRetirementState; struct ProcessRetirementNode; }

// One reservation, made before CreateProcess. Not concurrently callable.
class PreparedProcess final {
public:
    enum class Retirement { Empty, Pending, Busy };
    PreparedProcess() noexcept;
    ~PreparedProcess() noexcept;
    PreparedProcess(PreparedProcess&&) noexcept;
    PreparedProcess& operator=(PreparedProcess&&) noexcept;
    PreparedProcess(const PreparedProcess&) = delete;
    PreparedProcess& operator=(const PreparedProcess&) = delete;
    bool IsValid() const noexcept;
    HANDLE Handle() const noexcept; // borrowed; never close this handle
    // Requires a valid unused reservation and an owned, real process handle.
    // Ownership transfers without allocation immediately after CreateProcess.
    void AdoptStarted(HANDLE ownedProcess) noexcept;
    Retirement Retire() noexcept;
private:
    // Synchronous reentry may inspect a token, but cannot mutate it while an
    // operation owns it. Moving from/to a busy token preserves both operands;
    // move construction from a busy token produces an empty destination.
    Retirement RetireOwned() noexcept;
    std::shared_ptr<detail::ProcessRetirementState> state_;
    detail::ProcessRetirementNode* node_ = nullptr;
    bool operating_ = false;
    friend class ProcessRetirementQueue;
};

class ProcessRetirementQueue final {
public:
    enum class Result { Ok, Busy, NoMemory, Unavailable, Invalid };
    struct Status { std::size_t reserved = 0, pending = 0; DWORD lastError = ERROR_SUCCESS; };
    explicit ProcessRetirementQueue(std::size_t capacity = 64) noexcept;
    ~ProcessRetirementQueue() noexcept;
    ProcessRetirementQueue(const ProcessRetirementQueue&) = delete;
    ProcessRetirementQueue& operator=(const ProcessRetirementQueue&) = delete;
    // Null source is allowed. Failure leaves out unchanged. Slots include
    // reservations, running children and retired children, not just queue size.
    Result Prepare(std::shared_ptr<const void> source, PreparedProcess& out) noexcept;
    Status Snapshot() const noexcept;
private:
    const std::size_t capacity_;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::shared_ptr<detail::ProcessRetirementState> state_;
};
}
