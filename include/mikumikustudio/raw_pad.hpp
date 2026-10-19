// ===========================================================================
// MikuMikuStudio - shared raw padding helpers
// ===========================================================================
// One definition for the opaque padding type used by the hand-maintained
// layout headers (app_layout / model_layout / bone_layout).  The mdl
// headers pull this in and re-export it with a using-declaration, so both
// mikumikustudio::RawPad and mikumikustudio::mdl::RawPad name this type.
//
// Architecture-switch style used across these headers: the canonical
// condition form is `#if defined(_M_X64)` / `#if !defined(_M_X64)`;
// MIKUMIKUSTUDIO_X64 (subrecord_layout.hpp) is the separate 0/1 value
// macro for `#if <value>` expression contexts.
// ===========================================================================
#pragma once

#include <cstddef>

namespace mikumikustudio {

template <std::size_t N>
struct RawPad { unsigned char b[N]; };

struct EmptyPad {};

}  // namespace mikumikustudio
