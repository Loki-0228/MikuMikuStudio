#pragma once
#include <Windows.h>

namespace mikumikustudio::menu_initial_paint {
// Same-thread, idempotent attachment. Original profile keeps native painting.
// Failure leaves native menu handling intact; no menu commands are consumed.
bool Attach(HWND owner) noexcept;
void Detach(HWND owner) noexcept;
}
