#pragma once
#include <Windows.h>

namespace mikumikustudio::menu_initial_paint {
// Handle only this helper's timer; all native menu commands keep their owner.
bool HandleMessage(HWND owner, UINT message, WPARAM wp, LPARAM lp);
void Attach(HWND owner);
}
