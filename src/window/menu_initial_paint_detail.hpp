#pragma once
#include "mikumikustudio/menu_initial_paint.hpp"
#include <commctrl.h>
#include <cstddef>
#include <cstdint>

namespace mikumikustudio::menu_initial_paint::detail {
inline constexpr UINT_PTR kTimer = 0x4d4e5054;
inline constexpr UINT_PTR kSubclass = 0x4d4e5054;
struct Popup {
    HMENU menu = nullptr;
    DWORD thread = 0;
    bool visible = false;
    bool menuClass = false;
};
// Private test seam, not a plugin ABI. Only popup discovery/paint is simulated
// by the hidden-window tests; attachment and timers retain native defaults.
// Clock/timer/subclass functions must not reenter and all hooks must not throw.
// Enumeration, description and painting may reenter the owner synchronously.
struct Platform {
    ULONGLONG (*now)() noexcept;
    void (*enumerate)(WNDENUMPROC, LPARAM) noexcept;
    bool (*describe)(HWND, Popup&) noexcept;
    void (*paint)(HWND) noexcept;
    UINT_PTR (*arm)(HWND, UINT_PTR, UINT) noexcept;
    void (*disarm)(HWND, UINT_PTR) noexcept;
    bool (*install)(HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR) noexcept;
};
const Platform& NativePlatform() noexcept;
// Copies the table for this thread. Change/reset only with no attachments.
// nullptr restores defaults. Reject incomplete tables without changing them.
bool SetPlatformForTesting(const Platform*) noexcept;
struct Snapshot {
    bool attached = false;
    bool armed = false;
    std::size_t openings = 0;
    std::uint64_t ownerEpoch = 0;
    bool found = false;
    bool ready = false;
    bool first = false;
    bool idle = false;
    bool finished = false;
    std::uint64_t openingEpoch = 0;
    ULONGLONG opened = 0;
    ULONGLONG visibleSince = 0;
};
Snapshot Inspect(HWND owner, HMENU menu = nullptr) noexcept;
std::size_t AttachmentCount() noexcept;
}
