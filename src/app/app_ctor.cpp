// ===========================================================================
// VA 0x0042AE60 - MMDApp constructor  (original: sub_42AE60)
// ===========================================================================
// Original body (complete - 0x12 bytes):
//
//     char *__thiscall sub_42AE60(char *this)
//     {
//         sub_4C46F0(this + 652088);
//         return this;
//     }
//
// sub_4C46F0 is a no-op constructor (`return this;`) - the compiler emitted
// a placement call for a trivially constructible subobject living at
// this+0x9F4F8.  WinMain memsets the whole 0xA4530-byte object right after
// construction, so the only observable effect is returning `this`.
// ===========================================================================
#include "mikumikustudio/mmd_app.hpp"

namespace mikumikustudio {

MMDApp::MMDApp() {
    // this+0x9F4F8 subobject (VA 0x004C46F0) constructs nothing.
}

}  // namespace mikumikustudio
