// MAINTENANCE: regression-keep; byte-exact bounded PMM identity matching.
// Standalone CPU checks of the private C++17 helper, with no application,
// plugin, model loader, locale conversion, file, UI or data-directory access.
// Windows guard pages belong solely to this test process and are released.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "../src/io/pmm_identity.hpp"
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
using mikumikustudio::pmm_io::IdentityLength;
using mikumikustudio::pmm_io::MatchesIdentity;
unsigned checks = 0, cases = 0;
unsigned first = 0, second = 0, third = 0;
const char* stage = "setup";
void Require(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL PMM identity [%s] check=%u indices=%u,%u,%u: %s\n", stage, checks, first, second, third, why); std::exit(1); }
}
struct Fields { char serialized[256]{}, native[20]{}, english[20]{}; };
template<std::size_t M> void Serialize(char (&out)[M], const char* bytes, std::size_t length) {
    Require(length < M, "test serialization includes its terminator within the real array");
    std::memset(out, 0xd5, M); if (length) std::memcpy(out, bytes, length); out[length] = 0;
}
template<std::size_t M, std::size_t N, std::size_t E>
void ExpectMatch(const char (&serialized)[M], const char (&native)[N], const char (&english)[E], bool expected, const char* why) {
    std::array<char, M> oldSerialized{}; std::array<char, N> oldNative{}; std::array<char, E> oldEnglish{};
    std::memcpy(oldSerialized.data(), serialized, M); std::memcpy(oldNative.data(), native, N); std::memcpy(oldEnglish.data(), english, E);
    Require(MatchesIdentity(serialized, native, english) == expected, why);
    Require(!std::memcmp(oldSerialized.data(), serialized, M) && !std::memcmp(oldNative.data(), native, N) && !std::memcmp(oldEnglish.data(), english, E),
        "identity inspection never repairs, terminates or otherwise edits any input field");
}
void BasicNamesAndNoEnglishAlias() {
    Fields f;
    ExpectMatch(f.serialized, f.native, f.english, true, "three empty fields have the same native identity");
    Require(IdentityLength(f.native) == 0 && IdentityLength(f.english) == 0 && IdentityLength(f.serialized) == 0, "empty fields have zero bounded length");
    std::memcpy(f.native, "JP", 3); std::memcpy(f.english, "EN", 3); Serialize(f.serialized, "JP", 2);
    ExpectMatch(f.serialized, f.native, f.english, true, "short exact native identity is accepted");
    Serialize(f.serialized, "EN", 2); ExpectMatch(f.serialized, f.native, f.english, false, "English field is not an alternate identity lookup");
    Serialize(f.serialized, "J", 1); ExpectMatch(f.serialized, f.native, f.english, false, "short prefix of a terminated native name is rejected");
    Serialize(f.serialized, "JPEN", 4); ExpectMatch(f.serialized, f.native, f.english, false, "terminated native field cannot use the old cross-field suffix exception");
    Serialize(f.serialized, "JPx", 3); ExpectMatch(f.serialized, f.native, f.english, false, "arbitrary extra suffix is rejected");
    f.native[0] = 0; Serialize(f.serialized, "EN", 2); ExpectMatch(f.serialized, f.native, f.english, false, "empty native name never falls back to a nonempty English name");
    Serialize(f.serialized, "", 0); ExpectMatch(f.serialized, f.native, f.english, true, "empty exact native name wins independently of English content");
    std::memset(f.native, 'N', 20); std::memset(f.english, 0xff, 20); Serialize(f.serialized, f.native, 20);
    ExpectMatch(f.serialized, f.native, f.english, true, "full twenty-byte native identity does not require a native NUL or a usable English suffix");
    Require(IdentityLength(f.native) == 20 && IdentityLength(f.english) == 20, "unclosed fields report their finite capacity rather than scanning the following field");
    Serialize(f.serialized, f.native, 19); ExpectMatch(f.serialized, f.native, f.english, false, "nineteen bytes are not a tolerated prefix of a full twenty-byte identity");
    std::memset(f.native, 'N', 19); f.native[19] = 0; Serialize(f.serialized, f.native, 19);
    ExpectMatch(f.serialized, f.native, f.english, true, "terminated nineteen-byte native identity remains exact");
    Serialize(f.serialized, "NNNNNNNNNNNNNNNNNNNN", 20);
    ExpectMatch(f.serialized, f.native, f.english, false, "twentieth serialized byte cannot extend a terminated nineteen-byte native name");
}
void DbcsAndSplitBytesAreIdentities() {
    Fields f; for (unsigned i = 0; i < 20; i += 2) { f.native[i] = char(0x82); f.native[i + 1] = char(0xa0); }
    Serialize(f.serialized, f.native, 20);
    ExpectMatch(f.serialized, f.native, f.english, true, "ten CP932 double-byte characters occupy all twenty native bytes and match exactly");
    Serialize(f.serialized, f.native, 19);
    ExpectMatch(f.serialized, f.native, f.english, false, "serialized DBCS lead-byte prefix does not match a complete full-capacity identity");
    f.native[19] = 0; Serialize(f.serialized, f.native, 19);
    ExpectMatch(f.serialized, f.native, f.english, true, "a genuine nineteen-byte mirror ending in a CP932 lead byte is compared as bytes without decoding or repair");
    f.serialized[18] = char(0x83); ExpectMatch(f.serialized, f.native, f.english, false, "changed final DBCS lead byte changes the exact identity");
    Serialize(f.serialized, f.native, 18); ExpectMatch(f.serialized, f.native, f.english, false, "dropping the split lead byte is not identity compatibility");
    f.native[0] = char(0xff); f.native[1] = char(0xfe); Serialize(f.serialized, f.native, 19);
    ExpectMatch(f.serialized, f.native, f.english, true, "identity matching does not introduce UTF8, ACP or text-validity interpretation");
}
void OnlyExactBoundedLegacyConcatenation() {
    Fields f; std::memset(f.native, 'J', 20); std::memcpy(f.english, "EN", 3);
    char combined[40]{}; std::memcpy(combined, f.native, 20); std::memcpy(combined + 20, f.english, 2);
    Serialize(f.serialized, combined, 22); ExpectMatch(f.serialized, f.native, f.english, true, "only the full native field followed by the complete terminated nonempty English name is legacy-compatible");
    for (unsigned i = 0; i < 22; ++i) {
        first = i; auto changed = f; changed.serialized[i] ^= 1;
        ExpectMatch(changed.serialized, changed.native, changed.english, false, "every serialized native-prefix and English-suffix byte remains significant");
    }
    for (unsigned i = 0; i < 20; ++i) {
        first = i; auto changed = f; changed.native[i] ^= 1;
        ExpectMatch(changed.serialized, changed.native, changed.english, false, "legacy compatibility never ignores a changed native byte");
    }
    for (unsigned i = 0; i < 2; ++i) {
        first = i; auto changed = f; changed.english[i] ^= 1;
        ExpectMatch(changed.serialized, changed.native, changed.english, false, "legacy compatibility never ignores a changed English byte");
    }
    Serialize(f.serialized, combined, 21); ExpectMatch(f.serialized, f.native, f.english, false, "a truncated English suffix is not accepted");
    combined[22] = '!'; Serialize(f.serialized, combined, 23); ExpectMatch(f.serialized, f.native, f.english, false, "a matching English suffix plus extra arbitrary bytes is not accepted");
    f.english[0] = 0; Serialize(f.serialized, combined, 22); ExpectMatch(f.serialized, f.native, f.english, false, "an empty English field cannot authorize any suffix");
    Serialize(f.serialized, f.native, 20); ExpectMatch(f.serialized, f.native, f.english, true, "full exact native identity is still valid when English is empty");
    std::memset(f.english, 'E', 20); std::memcpy(combined + 20, f.english, 20); Serialize(f.serialized, combined, 40);
    ExpectMatch(f.serialized, f.native, f.english, false, "English without a NUL in its own capacity cannot authorize old cross-field identity");
    Serialize(f.serialized, f.native, 20); ExpectMatch(f.serialized, f.native, f.english, true, "strict native equality does not depend on English termination");
    f.native[19] = 0; f.english[2] = 0; Serialize(f.serialized, combined, 22);
    ExpectMatch(f.serialized, f.native, f.english, false, "legacy exception is unavailable when native already terminates within its own field");
}
void TwentyNativePlusTwentySixEnglish() {
    Fields f; for (unsigned i = 0; i < 20; i += 2) { f.native[i] = char(0x82); f.native[i+1] = char(0xa0); }
    char english[27] = "abcdefghijklmnopqrstuvwxyz"; char concatenated[46]{};
    std::memcpy(concatenated, f.native, 20); std::memcpy(concatenated + 20, english, 26); Serialize(f.serialized, concatenated, 46);
    ExpectMatch(f.serialized, f.native, english, true, "full JP twenty bytes plus a genuinely bounded twenty-six-byte English field matches the one permitted old form");
    Require(IdentityLength(english) == 26, "larger English field length is derived from its own template capacity");
    for (unsigned i = 0; i < 46; ++i) { first = i; auto changed = f; changed.serialized[i] ^= 1;
        ExpectMatch(changed.serialized, changed.native, english, false, "all forty-six old-record identity bytes must match"); }
    Serialize(f.serialized, concatenated, 45); ExpectMatch(f.serialized, f.native, english, false, "twenty-five of twenty-six English bytes is not a compatible prefix");
    Serialize(f.serialized, concatenated, 46); std::memcpy(f.english, english, 20);
    ExpectMatch(f.serialized, f.native, f.english, false, "a fixed twenty-byte unterminated English field cannot borrow the extra six characters from adjacent storage");
    f.serialized[46] = 'x'; f.serialized[47] = 0; ExpectMatch(f.serialized, f.native, english, false, "an otherwise exact JP20 plus EN26 identity rejects one more suffix byte");
    Serialize(f.serialized, concatenated, 46); english[13] = 0;
    ExpectMatch(f.serialized, f.native, english, false, "earlier English NUL changes the bounded suffix length instead of permitting a partial legacy match");
}
void TerminatorsAndGarbage() {
    Fields f; std::memset(f.native, 0xa7, 20); std::memset(f.english, 0xb8, 20);
    std::memcpy(f.native, "ABC", 4); std::memcpy(f.english, "EN", 3); Serialize(f.serialized, f.native, 3);
    f.serialized[4] = 'x'; f.serialized[255] = char(0xff); f.native[4] = 'y'; f.english[3] = 'z';
    ExpectMatch(f.serialized, f.native, f.english, true, "garbage after each in-field NUL does not participate in the exact identity");
    std::memset(f.native, 'J', 20); char combined[22]{}; std::memcpy(combined, f.native, 20); std::memcpy(combined + 20, f.english, 2);
    Serialize(f.serialized, combined, 22); f.serialized[23] = char(0xff);
    ExpectMatch(f.serialized, f.native, f.english, true, "garbage after serialized or English terminators does not widen the allowed legacy suffix");
    for (unsigned terminator = 0; terminator < 256; ++terminator) {
        first = terminator; std::memset(f.serialized, 'J', 256); f.serialized[terminator] = 0; f.english[0] = 0;
        Require(IdentityLength(f.serialized) == terminator, "every serialized terminator location is scanned only within capacity");
        ExpectMatch(f.serialized, f.native, f.english, terminator == 20, "only exactly twenty bytes match this full native identity across every serialized termination position");
    }
    std::memset(f.serialized, 'J', 256); Require(IdentityLength(f.serialized) == 256, "unclosed serialized field reports its bounded capacity");
    ExpectMatch(f.serialized, f.native, f.english, false, "unclosed serialized field is rejected rather than treated as a string");
    char noTerminator[20]; std::memset(noTerminator, 'J', 20);
    ExpectMatch(noTerminator, f.native, f.english, false, "serialized field must contain NUL even when every byte and capacity equal the full native field");
    char tooLong[256]; std::memset(tooLong, 'J', 256);
    ExpectMatch(f.serialized, tooLong, f.english, false, "equal full-capacity unterminated arrays do not waive serialized termination");
}
void EveryByteAndNativeTerminator() {
    for (unsigned byte = 0; byte < 256; ++byte) for (unsigned terminator = 0; terminator <= 20; ++terminator) {
        first = byte; second = terminator; third = 0; Fields f;
        std::memset(f.native, int(byte), 20); std::memset(f.english, 'E', 20);
        if (terminator < 20) f.native[terminator] = 0;
        const auto expected = byte ? terminator : 0;
        Require(IdentityLength(f.native) == expected, "all byte values and native termination positions have the independently known bounded length");
        Serialize(f.serialized, f.native, expected);
        ExpectMatch(f.serialized, f.native, f.english, true, "every exact binary native identity is accepted without character interpretation");
        if (expected) {
            for (unsigned position = 0; position < expected; ++position) {
                third = position; auto changed = f; changed.serialized[position] ^= char(0x80);
                ExpectMatch(changed.serialized, changed.native, changed.english, false, "changing any binary identity byte cannot become a prefix or English alias match");
            }
        } else {
            f.serialized[0] = 'x'; f.serialized[1] = 0;
            ExpectMatch(f.serialized, f.native, f.english, false, "a nonempty serialized value never matches a binary field whose first byte is NUL");
        }
    }
}
void EveryEnglishTerminatorAndByte() {
    for (unsigned terminator = 0; terminator <= 20; ++terminator) {
        first = terminator; second = third = 0; Fields f; std::memset(f.native, 'J', 20); std::memset(f.english, 'E', 20);
        if (terminator < 20) f.english[terminator] = 0;
        Require(IdentityLength(f.english) == terminator, "all English terminator locations have exact bounded length");
        char combined[40]{}; std::memcpy(combined, f.native, 20); if (terminator) std::memcpy(combined + 20, f.english, terminator);
        Serialize(f.serialized, combined, 20 + terminator);
        ExpectMatch(f.serialized, f.native, f.english, terminator < 20,
            "only a fully terminated English suffix is allowed; zero length here is strict native equality");
        if (terminator == 0) { f.serialized[20] = 'E'; f.serialized[21] = 0;
            ExpectMatch(f.serialized, f.native, f.english, false, "empty English name cannot justify even one appended byte"); }
    }
    for (unsigned byte = 1; byte < 256; ++byte) {
        first = byte; second = 19; third = 0; Fields f; std::memset(f.native, 'J', 20); std::memset(f.english, int(byte), 19); f.english[19] = 0;
        char combined[39]{}; std::memcpy(combined, f.native, 20); std::memcpy(combined + 20, f.english, 19); Serialize(f.serialized, combined, 39);
        ExpectMatch(f.serialized, f.native, f.english, true, "all non-NUL suffix byte values remain exact raw English identity data");
        for (unsigned position = 0; position < 19; ++position) {
            third = position; auto changed = f; changed.english[position] ^= char(0x80);
            ExpectMatch(changed.serialized, changed.native, changed.english, false, "changing any English byte rejects the old concatenation, including a newly introduced earlier NUL");
        }
    }
}

struct GuardedPage {
    unsigned char* allocation = nullptr; std::size_t page = 0;
    GuardedPage() {
        SYSTEM_INFO info{}; GetSystemInfo(&info); page = info.dwPageSize;
        Require(page >= 256, "system page can hold every bounded fixture field");
        allocation = static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 3, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Require(allocation != nullptr, "allocate private three-page fixture region");
        DWORD old = 0;
        Require(VirtualProtect(allocation, page, PAGE_NOACCESS, &old) && VirtualProtect(allocation + page * 2, page, PAGE_NOACCESS, &old),
            "protect only this process's first and last fixture pages");
    }
    ~GuardedPage() { Require(VirtualFree(allocation, 0, MEM_RELEASE) != FALSE, "release this fixture's entire private region"); }
    template<std::size_t N> char (&End())[N] {
        static_assert(N > 0, "nonempty array capacity"); Require(N <= page, "guarded array fits the writable middle page");
        return *reinterpret_cast<char (*)[N]>(allocation + page * 2 - N);
    }
};
void GuardedReadsStayWithinFields() {
    GuardedPage nativePage, englishPage, serializedPage; Fields f;
    auto& native = nativePage.End<20>(); std::memset(native, 'J', 20);
    Require(IdentityLength(native) == 20, "native length does not touch a PAGE_NOACCESS byte immediately after a full twenty-byte field");
    Serialize(f.serialized, native, 20);
    ExpectMatch(f.serialized, native, f.english, true, "full native exact comparison stays inside its guarded allocation");
    std::memcpy(f.english, "EN", 3); char combined[40]{}; std::memcpy(combined, native, 20); std::memcpy(combined + 20, f.english, 2);
    Serialize(f.serialized, combined, 22); ExpectMatch(f.serialized, native, f.english, true, "legacy prefix comparison reads only the twenty-byte guarded native field, never the following English address");
    auto& english = englishPage.End<20>(); std::memset(english, 'E', 20);
    Require(IdentityLength(english) == 20, "English length is bounded even without an accessible following byte");
    std::memcpy(combined + 20, english, 20); Serialize(f.serialized, combined, 40);
    ExpectMatch(f.serialized, native, english, false, "guarded unterminated English data cannot authorize a suffix by scanning another page");
    auto& serialized = serializedPage.End<256>(); std::memset(serialized, 'J', 256);
    Require(IdentityLength(serialized) == 256, "serialized length scan stops at its last accessible capacity byte");
    ExpectMatch(serialized, native, english, false, "unterminated serialized record is rejected before reading the inaccessible following page");
    auto& shortSerialized = serializedPage.End<21>(); std::memcpy(shortSerialized, native, 20); shortSerialized[20] = 0;
    ExpectMatch(shortSerialized, native, english, true, "serialized terminator exactly at the last accessible byte is sufficient for strict equality");
    auto& lastNative = nativePage.End<20>(); std::memset(lastNative, 'J', 19); lastNative[19] = 0;
    Serialize(f.serialized, lastNative, 19); ExpectMatch(f.serialized, lastNative, english, true, "native NUL exactly at the last accessible byte preserves nineteen-byte identity");
    auto& lastEnglish = englishPage.End<20>(); std::memset(lastEnglish, 'E', 19); lastEnglish[19] = 0; std::memset(lastNative, 'J', 20);
    std::memcpy(combined, lastNative, 20); std::memcpy(combined + 20, lastEnglish, 19); Serialize(f.serialized, combined, 39);
    ExpectMatch(f.serialized, lastNative, lastEnglish, true, "legacy suffix NUL exactly at English capacity boundary is accepted without a following read");
    auto& emptySerialized = serializedPage.End<1>(); emptySerialized[0] = 0;
    f.native[0] = 0; ExpectMatch(emptySerialized, f.native, lastEnglish, true, "empty serialized field needs only its one accessible terminator byte");
    auto& tinyNative = nativePage.End<1>(); tinyNative[0] = 'J'; char tinySerialized[2]{'J', 0}, emptyEnglish[1]{};
    ExpectMatch(tinySerialized, tinyNative, emptyEnglish, true, "generic one-byte full native array follows the same bounded exact rule");
    tinySerialized[1] = 'J'; ExpectMatch(tinySerialized, tinyNative, emptyEnglish, false, "generic serialized array without NUL is still rejected");
}
}

int main() {
    const auto run = [](const char* label, void (*body)()) {
        stage = label; first = second = third = 0; ++cases; std::printf("RUN PMM identity: %s\n", label); std::fflush(stdout); body();
    };
    run("native exact identity and English non-alias", BasicNamesAndNoEnglishAlias);
    run("CP932 full and split mirrors remain bytes", DbcsAndSplitBytesAreIdentities);
    run("unique bounded legacy concatenation", OnlyExactBoundedLegacyConcatenation);
    run("JP20 and EN26 field-capacity distinction", TwentyNativePlusTwentySixEnglish);
    run("serialized termination and post-NUL garbage", TerminatorsAndGarbage);
    run("all byte values and native NUL positions", EveryByteAndNativeTerminator);
    run("English byte and NUL positions", EveryEnglishTerminatorAndByte);
    run("private PAGE_NOACCESS field boundaries", GuardedReadsStayWithinFields);
    std::printf("PASS PMM identity: %u cases, %u checks; private guard-page bounds exercised\n", cases, checks); return 0;
}
