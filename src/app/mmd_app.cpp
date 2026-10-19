// VA 0x0054593C - the single `Block` application pointer.
// WinMain assigns it right after operator new; ported functions reach the
// state through mikumikustudio::g_Block exactly like the original global.
#include "mikumikustudio/mmd_app.hpp"

namespace mikumikustudio {

MMDApp* g_Block = nullptr;

}  // namespace mikumikustudio
