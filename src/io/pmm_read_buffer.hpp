#pragma once

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <io.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>

namespace mikumikustudio::pmm_io {
// The window only borrows the descriptor. Input remains its sole owner.
inline constexpr unsigned int kReadBufferBytes = 64u * 1024u;
struct ReadStatistics {
    std::uint64_t requests = 0, systemReads = 0, bytes = 0, buffered = 0;
};
inline thread_local ReadStatistics readStatistics{};
// Internal A/B instrumentation; not a persisted application preference.
inline thread_local bool bufferedReads = true;

struct ReadBufferState {
    int descriptor = -1;
    bool buffered = false;
    std::int64_t logical = 0;
    unsigned int cursor = 0, filled = 0;
    unsigned char buffer[kReadBufferBytes];
    ReadStatistics statistics{};
    ReadBufferState* previousScope = nullptr;
};
inline thread_local ReadBufferState* readScopeTop = nullptr;

inline ReadBufferState* FindReadState(int fd) noexcept {
    if (fd < 0) return nullptr;
    for (auto* state = readScopeTop; state; state = state->previousScope)
        if (state->descriptor == fd) return state;
    return nullptr;
}
inline std::int64_t Position(int fd) noexcept {
    const auto* state = FindReadState(fd);
    return state && state->buffered ? state->logical : _telli64(fd);
}
inline long Tell(int fd) noexcept {
    const auto position = Position(fd);
    if (position > LONG_MAX || position < LONG_MIN) { errno = EINVAL; return -1; }
    return static_cast<long>(position);
}
inline void AccumulateReadStatistics(const ReadStatistics& value) noexcept {
    readStatistics.requests += value.requests;
    readStatistics.systemReads += value.systemReads;
    readStatistics.bytes += value.bytes;
    readStatistics.buffered += value.buffered;
}
inline ReadStatistics LiveReadStatistics(int fd) noexcept {
    const auto* state = FindReadState(fd);
    return state ? state->statistics : ReadStatistics{};
}

// Strictness and buffering are separate: V1 keeps its optional-tail EOF rule.
// Store live scopes, rather than saved fd integers, so a consumed number cannot
// be restored by a suspended borrower after a different file reuses it.
struct CheckedReads;
inline thread_local CheckedReads* checkedScopeTop = nullptr;
inline thread_local int checkedDescriptor = -1;
inline void RefreshCheckedDescriptor() noexcept;
struct CheckedReads {
    int descriptor;
    bool retired = false;
    CheckedReads* previous;
    explicit CheckedReads(int fd) noexcept : descriptor(fd), previous(checkedScopeTop) {
        checkedScopeTop = this; RefreshCheckedDescriptor();
    }
    ~CheckedReads() noexcept {
        auto** link = &checkedScopeTop;
        while (*link && *link != this) link = &(*link)->previous;
        if (*link) *link = previous;
        RefreshCheckedDescriptor();
    }
    CheckedReads(const CheckedReads&) = delete;
    CheckedReads& operator=(const CheckedReads&) = delete;
};
inline void RefreshCheckedDescriptor() noexcept {
    checkedDescriptor = -1;
    for (auto* scope = checkedScopeTop; scope; scope = scope->previous)
        if (!scope->retired) { checkedDescriptor = scope->descriptor; break; }
}

// Rewind unread bytes before a raw CRT reader takes over. Failure leaves the
// window intact; callers must refuse that handoff, never reopen the file.
inline bool SyncExternalPosition(int fd) noexcept {
    auto* state = FindReadState(fd);
    if (!state || !state->buffered || state->cursor == state->filled) return true;
    if (_lseeki64(fd, state->logical, SEEK_SET) < 0) return false;
    state->cursor = state->filled = 0;
    return true;
}
inline std::int64_t Seek(int fd, std::int64_t offset, int origin) noexcept {
    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) {
        errno = EINVAL; return -1;
    }
    auto* state = FindReadState(fd);
    if (state && state->buffered && origin == SEEK_CUR) {
        if ((offset > 0 && state->logical > (std::numeric_limits<std::int64_t>::max)() - offset) ||
            (offset < 0 && state->logical < (std::numeric_limits<std::int64_t>::min)() - offset)) {
            errno = EINVAL; return -1;
        }
        offset += state->logical; origin = SEEK_SET;
    }
    const auto position = _lseeki64(fd, offset, origin);
    if (position >= 0 && state) {
        state->logical = position; state->cursor = state->filled = 0;
    }
    return position;
}

inline void RetireDescriptorReadContexts(int fd) noexcept {
    if (fd < 0) return;
    for (auto* state = readScopeTop; state; state = state->previousScope) {
        if (state->descriptor != fd) continue;
        AccumulateReadStatistics(state->statistics);
        state->statistics = {};
        state->descriptor = -1; state->buffered = false;
        state->cursor = state->filled = 0;
    }
    for (auto* scope = checkedScopeTop; scope; scope = scope->previous)
        if (scope->descriptor == fd) scope->retired = true;
    RefreshCheckedDescriptor();
}

class ReadAhead final {
    ReadBufferState state_;
    bool ownsState_ = false;
public:
    explicit ReadAhead(int fd) noexcept {
        if (fd < 0 || FindReadState(fd)) return;
        const auto position = _telli64(fd);
        struct _stat64 information{};
        const bool regular = bufferedReads && _fstat64(fd, &information) == 0 &&
            (information.st_mode & _S_IFMT) == _S_IFREG;
        state_.descriptor = fd;
        state_.buffered = regular && position >= 0;
        state_.logical = position >= 0 ? position : 0;
        state_.previousScope = readScopeTop;
        readScopeTop = &state_; ownsState_ = true;
    }
    ~ReadAhead() noexcept {
        if (!ownsState_) return;
        if (state_.descriptor >= 0) (void)SyncExternalPosition(state_.descriptor);
        AccumulateReadStatistics(state_.statistics);
        auto** link = &readScopeTop;
        while (*link && *link != &state_) link = &(*link)->previousScope;
        if (*link) *link = state_.previousScope;
    }
    ReadAhead(const ReadAhead&) = delete;
    ReadAhead& operator=(const ReadAhead&) = delete;
};

inline int ReadBytes(int fd, void* destination, unsigned int count) {
    if (!count) return 0;
    if (!destination || count > static_cast<unsigned int>(INT_MAX)) { errno = EINVAL; return -1; }
    auto* state = FindReadState(fd);
    if (!state) return _read(fd, destination, count);
    if (!state->buffered) {
        ++state->statistics.systemReads;
        const int received = _read(fd, destination, count);
        if (received > 0) state->statistics.bytes += static_cast<unsigned int>(received);
        return received;
    }
    auto* out = static_cast<unsigned char*>(destination);
    unsigned int copied = 0;
    while (copied < count) {
        if (state->cursor < state->filled) {
            const auto available = state->filled - state->cursor;
            const auto take = available < count - copied ? available : count - copied;
            std::memcpy(out + copied, state->buffer + state->cursor, take);
            state->cursor += take; state->logical += take;
            state->statistics.buffered += take; copied += take;
            continue;
        }
        const auto remaining = count - copied;
        ++state->statistics.systemReads;
        if (remaining >= kReadBufferBytes) {
            const int received = _read(fd, out + copied, remaining);
            if (received <= 0) {
                state->statistics.bytes += copied;
                return copied ? static_cast<int>(copied) : received;
            }
            copied += static_cast<unsigned int>(received); state->logical += received;
            continue;
        }
        const int received = _read(fd, state->buffer, kReadBufferBytes);
        state->cursor = state->filled = 0;
        if (received <= 0) {
            state->statistics.bytes += copied;
            return copied ? static_cast<int>(copied) : received;
        }
        state->filled = static_cast<unsigned int>(received);
    }
    state->statistics.bytes += copied;
    return static_cast<int>(copied);
}

struct ShortRead : std::runtime_error {
    __int64 offset;
    unsigned int expected;
    int received;
    ShortRead(__int64 at, unsigned int wanted, int got)
        : std::runtime_error("Incomplete PMM record"), offset(at), expected(wanted), received(got) {}
};
inline int Rd(int fd, void* destination, unsigned int count) {
    const auto offset = checkedDescriptor == fd ? Position(fd) : 0;
    if (auto* state = FindReadState(fd)) ++state->statistics.requests;
    const int received = ReadBytes(fd, destination, count);
    if (checkedDescriptor == fd && (received < 0 || static_cast<unsigned int>(received) != count))
        throw ShortRead(offset, count, received);
    return received;
}
}
