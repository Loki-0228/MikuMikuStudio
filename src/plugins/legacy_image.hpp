#pragma once

// In-memory adaptation of legacy MMEffect/MMHack DLL images.
//
// This module only parses bytes and returns an independent rewritten copy. It
// never loads or executes the image, calls an export, or touches the
// filesystem, so a caller can keep the result in a machine-local adapter cache.
//
// The rewrite targets exactly one value: an import module name that equals
// "MikuMikuDance.exe" ASCII case-insensitively. A matching name is replaced by
// "MMSAdapter.dll" inside its existing NUL-terminated name slot; the rest of
// that slot is zero-filled. No section, RVA, thunk, or other import is moved or
// changed. Exports, ordinals, resources, and unrelated imports stay byte for
// byte identical.
//
// The input must be a Windows AMD64 PE32+ DLL with an ordinary import
// directory. Bound imports, delay imports, descriptor binder metadata, and
// malformed, truncated, overlapping, or ambiguous images are rejected with a
// specific error. They are never reported as adapted.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mikumikustudio::plugins {

inline constexpr char LegacyHostImportName[] = "MikuMikuDance.exe";
inline constexpr char LegacyAdapterModuleName[] = "MMSAdapter.dll";

enum class LegacyImageError {
    None = 0,
    EmptyInput,
    Truncated,
    BadDosHeader,
    BadPeSignature,
    UnsupportedOptionalHeader,
    NotAmd64,
    NotADll,
    InvalidSectionTable,
    SectionOverlap,
    AmbiguousRvaMapping,
    UnsupportedBoundImports,
    UnsupportedDelayImports,
    InvalidImportDirectory,
    ImportTableUnterminated,
    InvalidImportDescriptor,
    RvaOutOfRange,
    UnterminatedImportName,
    ImportNameSlotTooSmall,
    NoMatchingImport,
    Overflow,
    AllocationFailed,
};

const char* LegacyImageErrorName(LegacyImageError error) noexcept;

struct LegacyImagePatch {
    // Independent image copy. Empty exactly when error != None.
    std::vector<std::uint8_t> image;
    // Number of matching import descriptors rewritten. Zero on failure.
    std::size_t rewritten = 0;
    LegacyImageError error = LegacyImageError::None;
    // Failure description; may be empty on allocator exhaustion.
    std::string message;

    bool ok() const noexcept { return error == LegacyImageError::None; }
};

// Rewrites every normal import module name that addresses the legacy host.
// On success image holds the full copy, rewritten is at least one, and message
// is empty. On any error image is empty, rewritten is zero, and message names
// the failure (message may be empty on allocator exhaustion). The input vector is never modified.
LegacyImagePatch RedirectLegacyHostImport(const std::vector<std::uint8_t>& image) noexcept;

} // namespace mikumikustudio::plugins
