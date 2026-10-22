#include "native_menu_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <utility>

namespace mikumikustudio::plugins::native_menu_snapshot {
namespace {

// Internal ceilings bound allocation and the traversal stack regardless of the
// caller's budgets. Exceeding the effective budget reports limit instead of
// shrinking the published snapshot.
constexpr std::uint32_t kDepthCeiling = 64;
constexpr std::uint32_t kItemsCeiling = 1u << 20;
constexpr std::uint32_t kTextUnitsCeiling = 1u << 20;
constexpr std::uint32_t kTotalTextUnitsCeiling = 1u << 24;
constexpr std::size_t kFrameCapacity = static_cast<std::size_t>(kDepthCeiling) + 1;

// One explicit traversal frame per open menu, so menu nesting never recurses on
// the machine stack. A frame's index is also the depth of the rows it yields.
struct Frame {
    HMENU menu = nullptr;
    int count = 0;
    int next = 0;
    std::uint32_t parent = UINT32_MAX;
};

std::uint32_t ClampBudget(std::uint32_t value, std::uint32_t ceiling) noexcept {
    return value < ceiling ? value : ceiling;
}

// Re-check the observation target after an API call failed. A vanished window,
// a different thread, or a different root menu means the tree moved underneath
// the pass rather than the query being unsupported.
Status FailureStatus(HWND owner, HMENU root) noexcept {
    if (!IsWindow(owner)) return Status::changed;
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(owner, &process);
    if (thread == 0 || process != GetCurrentProcessId() || thread != GetCurrentThreadId())
        return Status::changed;
    if (GetMenu(owner) != root) return Status::changed;
    return Status::failed;
}

Status ItemFailure(HWND owner, HMENU root, HMENU menu, int expectedCount) noexcept {
    const Status status = FailureStatus(owner, root);
    if (status != Status::failed) return status;
    const int count = GetMenuItemCount(menu);
    if (count >= 0 && count != expectedCount) return Status::changed;
    return Status::failed;
}

// Every item query must describe the same item. Text and owner data are
// deliberately excluded; they are compared separately or not read at all.
bool SameItem(const MENUITEMINFOW& first, const MENUITEMINFOW& second) noexcept {
    return first.fType == second.fType && first.fState == second.fState &&
           first.wID == second.wID && first.hSubMenu == second.hSubMenu;
}

Status CaptureInto(HWND owner, const Limits& limits, Snapshot& candidate) {
    const HMENU root = GetMenu(owner);
    candidate.rootMenu = reinterpret_cast<std::uintptr_t>(root);
    if (root == nullptr) return Status::ok;  // no menu is a valid empty observation

    const std::uint32_t maxDepth = ClampBudget(limits.maxDepth, kDepthCeiling);
    const std::uint32_t maxItems = ClampBudget(limits.maxItems, kItemsCeiling);
    const std::uint32_t maxTextUnits = ClampBudget(limits.maxTextUnits, kTextUnitsCeiling);
    const std::uint32_t maxTotalTextUnits =
        ClampBudget(limits.maxTotalTextUnits, kTotalTextUnitsCeiling);

    const int rootCount = GetMenuItemCount(root);
    if (rootCount < 0) return Status::failed;

    Frame frames[kFrameCapacity];
    HMENU ancestors[kFrameCapacity] = {};
    std::size_t top = 0;
    frames[0].menu = root;
    frames[0].count = rootCount;
    frames[0].next = 0;
    frames[0].parent = UINT32_MAX;
    ancestors[0] = root;
    std::uint64_t totalTextUnits = 0;

    for (;;) {
        Frame& frame = frames[top];
        if (frame.next == frame.count) {
            // Rows may have been inserted or removed while their menu was open.
            const int current = GetMenuItemCount(frame.menu);
            if (current < 0) return ItemFailure(owner, root, frame.menu, frame.count);
            if (current != frame.count) return Status::changed;
            if (top == 0) break;
            --top;
            continue;
        }

        const int position = frame.next++;
        if (candidate.items.size() >= static_cast<std::size_t>(maxItems)) return Status::limit;

        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(frame.menu, static_cast<UINT>(position), TRUE, &info))
            return ItemFailure(owner, root, frame.menu, frame.count);

        Item item;
        item.parent = frame.parent;
        item.position = static_cast<std::uint32_t>(position);
        item.commandId = static_cast<std::uint32_t>(info.wID);
        item.type = static_cast<std::uint32_t>(info.fType);
        item.state = static_cast<std::uint32_t>(info.fState);
        item.menu = reinterpret_cast<std::uintptr_t>(frame.menu);
        item.subMenu = reinterpret_cast<std::uintptr_t>(info.hSubMenu);

        // Owner-drawn and bitmap rows carry an application value in place of a
        // string, so no string query is issued for them and the value is never
        // dereferenced. Separators have no label either.
        const bool textReliable =
            (info.fType & (MFT_OWNERDRAW | MFT_BITMAP | MFT_SEPARATOR)) == 0;
        if (textReliable) {
            // The length query repeats the item attributes; cch is the UTF-16
            // length without the terminating NUL. The read query repeats them too.
            MENUITEMINFOW lengthInfo{};
            lengthInfo.cbSize = sizeof(lengthInfo);
            lengthInfo.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU | MIIM_STRING;
            lengthInfo.dwTypeData = nullptr;
            lengthInfo.cch = 0;
            if (!GetMenuItemInfoW(frame.menu, static_cast<UINT>(position), TRUE, &lengthInfo))
                return ItemFailure(owner, root, frame.menu, frame.count);
            if (!SameItem(info, lengthInfo)) return Status::changed;

            const std::uint64_t reported = static_cast<std::uint64_t>(lengthInfo.cch);
            if (reported > static_cast<std::uint64_t>(maxTextUnits)) return Status::limit;
            const std::size_t capacity = static_cast<std::size_t>(reported) + 1;
            std::wstring buffer(capacity, L'\0');

            MENUITEMINFOW textInfo{};
            textInfo.cbSize = sizeof(textInfo);
            textInfo.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU | MIIM_STRING;
            textInfo.dwTypeData = buffer.data();
            textInfo.cch = static_cast<UINT>(buffer.size());
            if (!GetMenuItemInfoW(frame.menu, static_cast<UINT>(position), TRUE, &textInfo)) {
                // A longer label since the length query is an observation change,
                // not a reason to truncate or to synthesize text.
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) return Status::changed;
                return ItemFailure(owner, root, frame.menu, frame.count);
            }
            if (!SameItem(info, textInfo)) return Status::changed;

            std::size_t length = 0;
            while (length < buffer.size() && buffer[length] != L'\0') ++length;
            if (length == buffer.size()) return Status::changed;  // no terminator written
            // The read must report the true NUL length, and that length must still
            // equal the one measured before the read. A one-unit shrink therefore
            // is a change, never a rounding difference.
            if (static_cast<std::uint64_t>(textInfo.cch) != static_cast<std::uint64_t>(length))
                return Status::changed;
            if (static_cast<std::uint64_t>(length) != reported) return Status::changed;

            // A label can grow after the length query and be silently truncated
            // into the old buffer. Measure attributes and length again after the
            // read so such a grow-then-truncate is a change, never published text.
            MENUITEMINFOW afterInfo{};
            afterInfo.cbSize = sizeof(afterInfo);
            afterInfo.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU | MIIM_STRING;
            afterInfo.dwTypeData = nullptr;
            afterInfo.cch = 0;
            if (!GetMenuItemInfoW(frame.menu, static_cast<UINT>(position), TRUE, &afterInfo))
                return ItemFailure(owner, root, frame.menu, frame.count);
            if (!SameItem(textInfo, afterInfo)) return Status::changed;
            if (static_cast<std::uint64_t>(afterInfo.cch) != reported) return Status::changed;

            if (totalTextUnits + length > static_cast<std::uint64_t>(maxTotalTextUnits))
                return Status::limit;
            totalTextUnits += length;
            item.text.assign(buffer.data(), length);
            item.textKnown = true;
        }

        candidate.items.push_back(std::move(item));

        const HMENU subMenu = info.hSubMenu;
        if (subMenu == nullptr) continue;
        // Query the submenu before applying the row-depth budget. An empty
        // submenu contributes no rows, so reaching it at the deepest allowed row
        // is not a limit; only a non-empty submenu needs a deeper row.
        const int subCount = GetMenuItemCount(subMenu);
        if (subCount < 0) return ItemFailure(owner, root, frame.menu, frame.count);
        if (subCount == 0) continue;
        if (top + 1 > static_cast<std::size_t>(maxDepth)) return Status::limit;
        // A menu may legitimately be attached under two branches and is expanded
        // in both. Only a cycle in the current ancestor path is rejected.
        for (std::size_t ancestor = 0; ancestor <= top; ++ancestor)
            if (ancestors[ancestor] == subMenu) return Status::changed;
        ++top;
        frames[top].menu = subMenu;
        frames[top].count = subCount;
        frames[top].next = 0;
        frames[top].parent = static_cast<std::uint32_t>(candidate.items.size() - 1);
        ancestors[top] = subMenu;
    }

    return Status::ok;
}

}  // namespace

Status Capture(HWND owner, const Limits& limits, Snapshot& output) noexcept {
    if (owner == nullptr || limits.maxDepth == 0 || limits.maxItems == 0 ||
        limits.maxTextUnits == 0 || limits.maxTotalTextUnits == 0)
        return Status::invalid_argument;
    try {
        if (!IsWindow(owner)) return Status::invalid_window;
        DWORD process = 0;
        const DWORD ownerThread = GetWindowThreadProcessId(owner, &process);
        if (ownerThread == 0 || process != GetCurrentProcessId()) return Status::invalid_window;
        if (ownerThread != GetCurrentThreadId()) return Status::wrong_thread;

        Snapshot candidate;
        const Status status = CaptureInto(owner, limits, candidate);
        if (status != Status::ok) return status;

        // Publish only if the owner, its thread, and the root menu stayed put for
        // the whole pass.
        if (!IsWindow(owner)) return Status::changed;
        process = 0;
        const DWORD finalThread = GetWindowThreadProcessId(owner, &process);
        if (finalThread != ownerThread || process != GetCurrentProcessId())
            return Status::changed;
        if (GetMenu(owner) != reinterpret_cast<HMENU>(candidate.rootMenu))
            return Status::changed;

        output.rootMenu = candidate.rootMenu;
        output.items.swap(candidate.items);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        return Status::no_memory;
    } catch (...) {
        return Status::failed;
    }
}
}
