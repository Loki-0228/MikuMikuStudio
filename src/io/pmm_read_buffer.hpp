// ===========================================================================
// pmm_read_buffer.hpp - bounded read-ahead for the PMM standard body
// ===========================================================================
// LoadSceneV1 / LoadSceneV2 read one scalar per _read call (usually 1 or 4
// bytes) and Rd probes the descriptor position once per field to record the
// ShortRead offset.  A real project's standard body is a few megabytes, so
// those per-field descriptor calls dominate the read path.  This header puts
// one fixed 64 KiB read-ahead window under the existing record readers; their
// parsing, checks and byte order are untouched.
//
// Contract kept from the old per-field helpers:
//   * A scope is opened with CheckedReads(fd).  A field that does not receive
//     all requested bytes still throws ShortRead with the logical offset the
//     field started at, the expected count and the received count.  A full
//     end of file is reported by OptionalRd as 0; a partial field is still an
//     error.  Zero-length reads do not move the stream and a NULL destination
//     or a request above INT_MAX is rejected with the same negative result the
//     CRT would report, before any byte is copied.
//   * The CRT descriptor is still owned by ScopedDescriptor / Close.  Active
//     scopes live on an intrusive thread-local chain, and owning descriptors
//     live on a second chain, so Close(fd) retires every suspended scope and
//     every registered owner of fd - not just the innermost one.  A therefore
//     closed descriptor number can be reused without an outer scope seeking or
//     closing the new file, and no exit path can resurrect a closed window.
//   * SyncExternalPosition(fd) puts the CRT descriptor at the logical
//     (consumed) position and drops the window before a raw CRT reader takes
//     over - the PMM extension trailer reader seeks to the file end and back.
//     It reports a seek failure so the caller can refuse the handoff.  Leaving
//     a CheckedReads scope synchronises the same way but is noexcept and never
//     touches a descriptor Close already retired.
//   * Nested scopes restore the previous scope.  A scope on a descriptor that
//     already has a (possibly suspended) state shares that state instead of
//     resetting it, and reads of a suspended descriptor keep using its own
//     window, so A -> B -> A nests without a second wrong-start buffer.
//   * If the position query in the CheckedReads constructor fails (for example
//     an unseekable descriptor), the scope falls back to the old per-field
//     _read path instead of treating -1 as a valid logical position.
//
// bufferedReads / readStatistics are the A/B switch and counters the focused
// test uses to show that field reads do not map one-to-one onto _read calls.
// They are thread-local; there is no product UI for them.
#pragma once

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <io.h>
#include <stdexcept>

namespace mikumikustudio::pmm_io {

inline constexpr unsigned int kReadBufferBytes = 64u * 1024u;

struct ReadStatistics {
    std::uint64_t requests = 0;     // Rd / OptionalRd calls on the buffered fd
    std::uint64_t systemReads = 0;  // backing _read calls
    std::uint64_t bytes = 0;        // bytes handed to the record readers
    std::uint64_t buffered = 0;     // bytes served from the read-ahead window
};
inline thread_local ReadStatistics readStatistics{};

// Test-only A/B switch: false runs the same record readers with the old
// per-field _read path.
inline thread_local bool bufferedReads = true;

struct ReadBufferState {
    int descriptor = -1;
    bool buffered = true;
    __int64 logical = 0;      // bytes consumed by the record readers
    unsigned int cursor = 0;  // consumed prefix of buffer
    unsigned int filled = 0;  // valid suffix of buffer
    unsigned char buffer[kReadBufferBytes];
    ReadStatistics statistics{};
    ReadBufferState* previousScope = nullptr;  // link in the active-scope chain
};

// Innermost active CheckedReads scope, threaded through the scope objects
// themselves so Close(fd) can walk every suspended scope.
inline thread_local ReadBufferState* readScopeTop = nullptr;

// Registered ScopedDescriptor owners, threaded through the scope objects for
// the same reason: Close(fd) must clear every owner, not only the innermost.
struct DescriptorOwner {
    int* descriptor;
    DescriptorOwner* previous;
};
inline thread_local DescriptorOwner* descriptorOwners = nullptr;

inline thread_local int checkedDescriptor = -1;

struct ShortRead : std::runtime_error {
    __int64 offset;
    unsigned int expected;
    int received;
    ShortRead(__int64 at,unsigned int wanted,int got)
        : std::runtime_error("Incomplete PMM record"),offset(at),expected(wanted),received(got) {}
};

inline ReadBufferState* FindReadState(int fd) {
    if (fd < 0) return nullptr;
    for (ReadBufferState* state = readScopeTop; state != nullptr;
         state = state->previousScope) {
        if (state->descriptor == fd) return state;
    }
    return nullptr;
}

// Logical (consumed) position of fd.  Falls back to the CRT position when fd
// is not buffered (no scope, the disabled test path, or a scope whose initial
// position query failed).
inline std::int64_t Position(int fd) {
    ReadBufferState* const state = FindReadState(fd);
    if (state != nullptr && state->buffered) return state->logical;
    return _telli64(fd);
}

// _tell-compatible view for the diagnostic log helpers.
inline long Tell(int fd) {
    return static_cast<long>(Position(fd));
}

inline void AccumulateReadStatistics(const ReadStatistics& statistics) {
    readStatistics.requests += statistics.requests;
    readStatistics.systemReads += statistics.systemReads;
    readStatistics.bytes += statistics.bytes;
    readStatistics.buffered += statistics.buffered;
}

// Drops one state when Close releases its descriptor: statistics are folded
// into the thread-local totals and the state can never seek the number again.
inline void RetireReadState(ReadBufferState& state) {
    AccumulateReadStatistics(state.statistics);
    state.statistics = ReadStatistics{};
    state.descriptor = -1;
    state.buffered = false;
    state.cursor = 0;
    state.filled = 0;
}

// Puts the CRT descriptor at the logical position and drops the window, so a
// raw CRT reader (the PMM extension trailer reader) starts exactly where the
// record readers stopped.  Returns false when the seek fails; the caller must
// then refuse the handoff.  Returns true when nothing is buffered (the
// descriptor is already level).
inline bool SyncExternalPosition(int fd) {
    ReadBufferState* const state = FindReadState(fd);
    if (state == nullptr || !state->buffered) return true;
    if (state->cursor >= state->filled) return true;
    if (_lseeki64(fd, state->logical, SEEK_SET) < 0) return false;
    state->cursor = 0;
    state->filled = 0;
    return true;
}

// One windowed read.  Returns what the single _read used to return: count on
// success, a short count at end of file, a negative CRT error when no byte
// was delivered.
inline int ReadBytes(int fd, void* destination, unsigned int count) {
    if (count == 0) return 0;
    // Match the CRT's own argument handling without invoking its invalid
    // parameter handler (which can terminate the process): a NULL destination
    // and a request above INT_MAX are refused before anything is copied.
    if (destination == nullptr || count > static_cast<unsigned int>(INT_MAX)) {
        errno = EINVAL;
        return -1;
    }
    // A descriptor can be read while a later, different-descriptor scope is on
    // top of the chain: its own (suspended) state still owns the correct
    // logical position and window, so pick it out of the whole chain rather
    // than only the top.
    ReadBufferState* const state = FindReadState(fd);
    if (state == nullptr) return _read(fd, destination, count);
    if (!state->buffered) {
        ++state->statistics.systemReads;
        const int received = _read(fd, destination, count);
        if (received > 0)
            state->statistics.bytes += static_cast<unsigned int>(received);
        return received;
    }
    unsigned char* const out = static_cast<unsigned char*>(destination);
    unsigned int copied = 0;
    while (copied < count) {
        if (state->cursor < state->filled) {
            const unsigned int available = state->filled - state->cursor;
            const unsigned int take =
                available < count - copied ? available : count - copied;
            std::memcpy(out + copied, state->buffer + state->cursor, take);
            state->cursor += take;
            state->logical += take;
            state->statistics.buffered += take;
            copied += take;
            continue;
        }
        const unsigned int remaining = count - copied;
        ++state->statistics.systemReads;
        if (remaining >= kReadBufferBytes) {
            // Requests at least as large as the window go straight to the
            // destination.  The descriptor and the logical position are level
            // here because a fill never leaves unconsumed bytes behind.
            const int received = _read(fd, out + copied, remaining);
            if (received <= 0) {
                state->statistics.bytes += copied;
                return copied > 0 ? static_cast<int>(copied) : received;
            }
            copied += static_cast<unsigned int>(received);
            state->logical += received;
            continue;
        }
        const int received = _read(fd, state->buffer, kReadBufferBytes);
        state->cursor = 0;
        state->filled = 0;
        if (received <= 0) {
            state->statistics.bytes += copied;
            return copied > 0 ? static_cast<int>(copied) : received;
        }
        state->filled = static_cast<unsigned int>(received);
    }
    state->statistics.bytes += copied;
    return static_cast<int>(copied);
}

// Thread-local read scope over one checked descriptor.  Construct it after the
// descriptor is positioned at the first record byte; its destructor restores
// the previous scope and leaves the descriptor at the consumed position.  The
// state is a member, linked into readScopeTop, so a Close of this or any
// suspended descriptor can invalidate it in place.
struct CheckedReads {
    ReadBufferState state{};
    ReadBufferState* parent;
    int descriptor;
    int previousCheckedDescriptor;
    bool ownsState;

    explicit CheckedReads(int fd)
        : parent(readScopeTop),
          descriptor(fd),
          previousCheckedDescriptor(checkedDescriptor),
          ownsState(FindReadState(fd) == nullptr) {
        if (ownsState) {
            const __int64 at = _telli64(fd);
            state.descriptor = fd;
            // A failed initial position query falls back to the per-field
            // path; -1 must never be accumulated as a logical position.
            state.buffered = bufferedReads && at >= 0;
            state.logical = at >= 0 ? at : 0;
            state.previousScope = parent;
            readScopeTop = &state;
        }
        checkedDescriptor = fd;
    }
    ~CheckedReads() noexcept {
        if (ownsState) {
            if (state.descriptor == descriptor)
                (void)SyncExternalPosition(descriptor);
            AccumulateReadStatistics(state.statistics);
            if (readScopeTop == &state) {
                readScopeTop = state.previousScope;
            } else {
                // Defensive unlink; normal nesting always pops the top.
                for (ReadBufferState* scope = readScopeTop; scope != nullptr;
                     scope = scope->previousScope) {
                    if (scope->previousScope == &state) {
                        scope->previousScope = state.previousScope;
                        break;
                    }
                }
            }
        }
        checkedDescriptor = previousCheckedDescriptor;
    }
    CheckedReads(const CheckedReads&) = delete;
    CheckedReads& operator=(const CheckedReads&) = delete;
};

inline void Close(int fd) {
    if (fd < 0) return;
    // Invalidate every suspended scope and every registered owner of fd, so a
    // descriptor number reused by the next open inherits neither a read-ahead
    // window nor a stale owner that would close it a second time.
    for (ReadBufferState* state = readScopeTop; state != nullptr;
         state = state->previousScope) {
        if (state->descriptor == fd) RetireReadState(*state);
    }
    for (DescriptorOwner* owner = descriptorOwners; owner != nullptr;
         owner = owner->previous) {
        if (*owner->descriptor == fd) *owner->descriptor = -1;
    }
    _close(fd);
}

struct ScopedDescriptor {
    int& descriptor;
    DescriptorOwner owner;
    explicit ScopedDescriptor(int& fd)
        : descriptor(fd) {
        owner.descriptor = &fd;
        owner.previous = descriptorOwners;
        descriptorOwners = &owner;
    }
    ~ScopedDescriptor() noexcept {
        Close(descriptor);
        if (descriptorOwners == &owner) {
            descriptorOwners = owner.previous;
        } else {
            for (DescriptorOwner* entry = descriptorOwners; entry != nullptr;
                 entry = entry->previous) {
                if (entry->previous == &owner) {
                    entry->previous = owner.previous;
                    break;
                }
            }
        }
    }
    ScopedDescriptor(const ScopedDescriptor&) = delete;
    ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;
};

inline int Rd(int fd, void* buf, unsigned int count) {
    const __int64 offset = checkedDescriptor == fd ? Position(fd) : 0;
    if (ReadBufferState* state = FindReadState(fd))
        ++state->statistics.requests;
    const int received = ReadBytes(fd, buf, count);
    if(checkedDescriptor == fd && received != static_cast<int>(count))
        throw ShortRead(offset,count,received);
    return received;
}

// V1 extensions may be absent at EOF, but a partially present field is corrupt.
inline int OptionalRd(int fd, void* buf, unsigned int count) {
    const __int64 offset = Position(fd);
    if (ReadBufferState* state = FindReadState(fd))
        ++state->statistics.requests;
    const int received = ReadBytes(fd, buf, count);
    if (received != 0 && received != static_cast<int>(count))
        throw ShortRead(offset, count, received);
    return received;
}

inline void ReadCount(int fd, std::int32_t& count, unsigned minimumBytes = 1) {
    Rd(fd, &count, 4);
    const auto position = Position(fd), size = _filelengthi64(fd);
    if (count < 0 || position < 0 || size < position ||
        std::uint64_t(count) > std::uint64_t(size - position) / minimumBytes)
        throw std::runtime_error("Invalid PMM record count");
}

// Live counters of the scope that is buffering fd (empty when fd is not the
// buffered descriptor).  Scope totals are folded into readStatistics by the
// CheckedReads destructor and by Close.
inline ReadStatistics LiveReadStatistics(int fd) {
    const ReadBufferState* const state = FindReadState(fd);
    return state != nullptr ? state->statistics : ReadStatistics{};
}

}  // namespace mikumikustudio::pmm_io
