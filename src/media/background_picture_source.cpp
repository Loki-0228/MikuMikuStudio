#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/background_picture_source.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace mikumikustudio::media {
namespace detail {
struct PictureSourceBudgetState final {
    SRWLOCK lock = SRWLOCK_INIT;
    std::uint64_t limit, used = 0;
    explicit PictureSourceBudgetState(std::uint64_t maximum) noexcept : limit(maximum) {}
};
}
namespace {
struct BudgetLock final {
    SRWLOCK& lock;
    explicit BudgetLock(SRWLOCK& value) noexcept : lock(value) { AcquireSRWLockExclusive(&lock); }
    ~BudgetLock() { ReleaseSRWLockExclusive(&lock); }
};
void ReturnBudget(detail::PictureSourceBudgetState& state, std::uint64_t amount) noexcept {
    BudgetLock lock(state.lock);
    state.used -= amount;
}
struct Reservation final {
    std::shared_ptr<detail::PictureSourceBudgetState> state;
    std::uint64_t amount = 0;
    explicit Reservation(std::shared_ptr<detail::PictureSourceBudgetState> value) noexcept
        : state(std::move(value)) {}
    bool Acquire(std::uint64_t requested) noexcept {
        BudgetLock lock(state->lock);
        if (state->used > state->limit || requested > state->limit - state->used) return false;
        state->used += requested;
        amount = requested;
        return true;
    }
    ~Reservation() { if (amount) ReturnBudget(*state, amount); }
};
}

PictureSourceBudget::PictureSourceBudget(std::uint64_t limit)
    : state_(std::make_shared<detail::PictureSourceBudgetState>(limit)) {}
void PictureSourceBudget::Configure(std::uint64_t limit) noexcept {
    BudgetLock lock(state_->lock);
    state_->limit = limit;
}
PictureSourceBudgetUsage PictureSourceBudget::Usage() const noexcept {
    BudgetLock lock(state_->lock);
    return {state_->limit, state_->used};
}
PictureSourceBudget& GlobalPictureSourceBudget() {
    static PictureSourceBudget budget;
    return budget;
}

BackgroundPictureSource::BackgroundPictureSource(std::unique_ptr<unsigned char[]> bytes,
    std::uint32_t size, std::shared_ptr<detail::PictureSourceBudgetState> budget) noexcept
    : bytes_(std::move(bytes)), size_(size), budget_(std::move(budget)) {}

BackgroundPictureSource::~BackgroundPictureSource() noexcept {
    // No new reservation can reuse this capacity while its payload is live.
    bytes_.reset();
    ReturnBudget(*budget_, RetainedBytes());
}

std::uint64_t BackgroundPictureSource::OverheadBytes() noexcept {
    // Separate shared_ptr allocation: conservatively cover the MSVC control
    // block (vptr, strong/weak counters and managed pointer) on x86 and x64.
    return sizeof(BackgroundPictureSource) + 4 * sizeof(void*);
}

PictureSourceResult CaptureBackgroundPictureSource(const wchar_t* path,
    std::uint64_t maximumRetainedBytes, BackgroundPictureSourcePtr& out) noexcept {
    if (!path || !path[0]) return PictureSourceResult::Invalid;
    try {
        return CaptureBackgroundPictureSource(path, maximumRetainedBytes, GlobalPictureSourceBudget(), out);
    } catch (const std::bad_alloc&) {
        return PictureSourceResult::NoMemory;
    } catch (...) {
        return PictureSourceResult::ReadFailed;
    }
}

PictureSourceResult CaptureBackgroundPictureSource(const wchar_t* path,
    std::uint64_t maximumRetainedBytes, PictureSourceBudget& budget,
    BackgroundPictureSourcePtr& out) noexcept {
    if (!path || !path[0]) return PictureSourceResult::Invalid;
    struct File {
        HANDLE value;
        ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        bool Close() noexcept {
            if (!CloseHandle(value)) return false;
            value = INVALID_HANDLE_VALUE;
            return true;
        }
    } file{CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return PictureSourceResult::OpenFailed;
    if (GetFileType(file.value) != FILE_TYPE_DISK) return PictureSourceResult::Invalid;
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file.value, &information)) return PictureSourceResult::ReadFailed;
    if (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return PictureSourceResult::Invalid;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.value, &length)) return PictureSourceResult::ReadFailed;
    if (length.QuadPart <= 0) return PictureSourceResult::Invalid;
    const auto size = static_cast<std::uint64_t>(length.QuadPart);
    const auto overhead = BackgroundPictureSource::OverheadBytes();
    if (size > (std::numeric_limits<std::uint32_t>::max)() ||
        maximumRetainedBytes < overhead || size > maximumRetainedBytes - overhead)
        return PictureSourceResult::TooLarge;
    Reservation reservation(budget.state_);
    if (!reservation.Acquire(overhead + size)) return PictureSourceResult::TooLarge;
    try {
        auto bytes = std::make_unique<unsigned char[]>(static_cast<std::size_t>(size));
        std::uint32_t offset = 0;
        while (offset < size) {
            constexpr DWORD chunk = 1024 * 1024;
            const auto requested = static_cast<DWORD>((std::min)(size - offset, std::uint64_t(chunk)));
            DWORD received = 0;
            if (!ReadFile(file.value, bytes.get() + offset, requested, &received, nullptr) ||
                received == 0 || received > requested) return PictureSourceResult::ReadFailed;
            offset += received;
        }
        if (!file.Close()) return PictureSourceResult::ReadFailed;
        // No file handle escapes; the lease owns only immutable encoded bytes.
        auto* captured = new BackgroundPictureSource(std::move(bytes), static_cast<std::uint32_t>(size), reservation.state);
        // The object now owns the reservation. If shared_ptr's control block
        // allocation fails, its constructor deletes captured and returns it.
        reservation.amount = 0;
        BackgroundPictureSourcePtr source(captured);
        out.swap(source);
        return PictureSourceResult::Ok;
    } catch (const std::bad_alloc&) {
        return PictureSourceResult::NoMemory;
    } catch (...) {
        return PictureSourceResult::ReadFailed;
    }
}
}
