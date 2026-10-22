#pragma once

#include <cstddef>
#include <cstring>

namespace mikumikustudio::pmm_io {

// Bounded strlen for the fixed-width PMD identity fields. The full PMD loader
// keeps name[20] without a NUL when all twenty bytes are used, so the scan
// stops at the field capacity instead of walking into the adjacent field.
template <std::size_t N>
std::size_t IdentityLength(const char (&field)[N]) noexcept {
    const void* const terminator = std::memchr(field, 0, N);
    return terminator != nullptr
               ? static_cast<std::size_t>(
                     static_cast<const char*>(terminator) - field)
               : N;
}

// Bounded identity equality for the PMM v2 display/morph name tables.
// serialized is the length-prefixed name read from the project file and must
// carry a NUL within its own capacity. The exact form compares the same number
// of bytes as the native identity. The only other accepted form is the record
// an older full-profile writer produced when the native field had no NUL: the
// full native field followed by the complete, terminated, nonempty English
// field, both byte-identical. Prefixes, extra suffixes and English-only
// matches are rejected.
template <std::size_t M, std::size_t N, std::size_t E>
bool MatchesIdentity(const char (&serialized)[M],
                     const char (&native)[N],
                     const char (&english)[E]) noexcept {
    const std::size_t serializedLength = IdentityLength(serialized);
    if (serializedLength == M) return false;
    const std::size_t nativeLength = IdentityLength(native);
    if (serializedLength == nativeLength &&
        std::memcmp(serialized, native, nativeLength) == 0) {
        return true;
    }
    if (nativeLength != N) return false;
    const std::size_t englishLength = IdentityLength(english);
    if (englishLength == 0 || englishLength == E) return false;
    if (serializedLength < N) return false;
    if (serializedLength - N != englishLength) return false;
    return std::memcmp(serialized, native, N) == 0 &&
           std::memcmp(serialized + N, english, englishLength) == 0;
}

}  // namespace mikumikustudio::pmm_io
