// ===========================================================================
// MikuMikuStudio - shared path-resolution workspace (app+0x9F338)
// ===========================================================================
#pragma once

#include <cstddef>

namespace mikumikustudio {

// wchar_t width of each wide path field below. The original reserves 256
// wchars per field on both ABIs, and the assertions at the end of this header
// fix those offsets. ExtractDirFromPath (VA 0x00408960) copies its source
// with a fixed 1000-wchar limit, yet most callers pass one of these
// fields, so the copy is wider than the buffer it receives and the bound must
// come from the caller. The fields cannot be enlarged because
// the offsets reproduce the original object layout.
// Defect HB-02.
constexpr std::size_t kPathFieldCapacity = 256;

// The original stores this scratch object inline in the application state.
// The calls at 0x408870, 0x4089F0, and 0x408E70 establish the three live paths.
struct PathResolutionWorkspace {
    wchar_t projectDirectory[kPathFieldCapacity];
    wchar_t executableDirectory[kPathFieldCapacity];
    unsigned char scratch[2000];
    wchar_t resolvedPath[kPathFieldCapacity];
};

static_assert(offsetof(PathResolutionWorkspace, executableDirectory) == 512);
static_assert(offsetof(PathResolutionWorkspace, resolvedPath) == 3024);
static_assert(sizeof(PathResolutionWorkspace) == 3536);

}  // namespace mikumikustudio
