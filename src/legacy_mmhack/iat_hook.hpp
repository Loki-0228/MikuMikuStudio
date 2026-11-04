#pragma once
#include <windows.h>
#include <cstddef>
namespace mme_re::mmhack {
struct IatPatchResult { std::size_t replaced = 0; DWORD error = ERROR_SUCCESS; bool ok() const noexcept { return replaced != 0 && error == ERROR_SUCCESS; } };
// Bounded PE parser. The caller supplies a mapped-image span, not a disk PE.
IatPatchResult replace_import_address(void* mapped_image, std::size_t image_bytes, FARPROC original, FARPROC replacement);
IatPatchResult hook_import(const char* name, FARPROC replacement, HMODULE target, HMODULE provider);
}
