#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace mikumikustudio::plugins::native_menu_snapshot {
// Read-only observation of the menu tree that an owner window has already
// materialized. Capture queries only the calling UI thread. It sends no window
// messages, runs no menu callbacks, and reads no scene, history, focus, or dirty
// state. The result describes one pass over the live menu, not a transaction
// against concurrent menu mutation.
enum class Status {
    ok,
    invalid_argument,
    invalid_window,
    wrong_thread,
    limit,
    changed,
    no_memory,
    failed,
};

struct Limits {
    // Every budget must be nonzero. Values above this module's internal ceilings
    // are clamped so a very large budget cannot size an allocation or the
    // traversal stack. Hitting an effective budget reports limit; nothing is
    // silently truncated.
    std::uint32_t maxDepth = 32;  // top-level rows are depth 0; the row depth must not exceed this
    std::uint32_t maxItems = 8192;
    std::uint32_t maxTextUnits = 4096;        // UTF-16 code units in one label
    std::uint32_t maxTotalTextUnits = 1048576;
};

struct Item {
    // parent is the snapshot-vector index of the item that owns this row's menu;
    // top-level rows use UINT32_MAX.
    std::uint32_t parent = UINT32_MAX;
    std::uint32_t position = 0;  // real row position in the containing HMENU
    std::uint32_t commandId = 0;
    std::uint32_t type = 0;   // MFT_* value reported by GetMenuItemInfoW
    std::uint32_t state = 0;  // MFS_* value reported by GetMenuItemInfoW
    // Observed HMENU values only. They are not stable object identities and must
    // not be treated as handles to act on after this observation.
    std::uintptr_t menu = 0;
    std::uintptr_t subMenu = 0;
    std::wstring text;
    // False for owner-draw, separators, and legacy MFT_BITMAP rows. Modern
    // MIIM_BITMAP decorations may coexist with readable text; icons are not
    // captured by this private text/placement observer.
    bool textKnown = false;
};

struct Snapshot {
    std::uintptr_t rootMenu = 0;  // 0 when the window currently has no menu
    std::vector<Item> items;      // depth-first; a parent is always before its children
};

// Captures the owner's current menu tree. On every failure output is left
// unchanged. The tree is only published after the owner window, its thread, and
// its root menu have been re-checked at the end of the pass.
// A null owner or a zero budget is invalid_argument; a handle that is not a live
// window of this process is invalid_window; a window owned by another thread of
// this process is wrong_thread. Exceeding a budget is limit, and a pass that saw
// the menu move is changed.
Status Capture(HWND owner, const Limits& limits, Snapshot& output) noexcept;
}
