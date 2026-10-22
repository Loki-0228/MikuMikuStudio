#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/build_profile.hpp"
#include "menu_initial_paint_detail.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace mikumikustudio::menu_initial_paint {
namespace {

using detail::Platform;
using detail::Popup;

constexpr UINT kPollIntervalMs = 16;
constexpr ULONGLONG kFinalPaintDelayMs = 220;
constexpr ULONGLONG kPollDeadlineMs = 600;

struct Opening {
    std::uint64_t epoch = 0;
    HMENU menu = nullptr;
    ULONGLONG opened = 0;
    ULONGLONG visibleSince = 0;
    bool ready = false;
    bool first = false;
    bool idle = false;
    bool finished = false;
};

struct Owner {
    std::uint64_t epoch = 0;
    bool armed = false;
    std::vector<Opening> openings;
};

struct Registry {
    std::map<HWND, Owner> owners;
};

struct Visit {
    HWND owner = nullptr;
    std::uint64_t ownerEpoch = 0;
    // Largest token handed out before this inspection started. Openings with a
    // larger epoch were created by a synchronous reopen inside describe or a
    // paint callback and must not be treated as the generation we observed.
    std::uint64_t openingCutoff = 0;
};

struct Ticket {
    bool valid = false;
    std::uint64_t ownerEpoch = 0;
    HMENU menu = nullptr;
    std::uint64_t openingEpoch = 0;
};

// Zero-initialized on purpose: a thread_local std::map would be dynamically
// initialized, and its allocation failure could escape the noexcept boundary.
// The registry is allocated from Attach, inside Attach's try block.
thread_local std::unique_ptr<Registry> g_registry;

// Monotonic per-thread token source shared by owner and opening epochs. Zero is
// never handed out. When the counter reaches the largest value the subclass
// reference data can carry, new operations are rejected instead of wrapping.
thread_local std::uint64_t g_nextToken = 1;

thread_local bool g_testingPlatform = false;
thread_local Platform g_platformOverride{};

LRESULT CALLBACK OwnerProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

const Platform& platform() noexcept {
    return g_testingPlatform ? g_platformOverride : detail::NativePlatform();
}

std::uint64_t nextToken() noexcept {
    constexpr std::uint64_t kLimit =
        static_cast<std::uint64_t>((std::numeric_limits<DWORD_PTR>::max)());
    if (g_nextToken == 0 || g_nextToken >= kLimit) return 0;
    return g_nextToken++;
}

std::uint64_t tokenCutoff() noexcept {
    return g_nextToken == 0 ? 0 : g_nextToken - 1;
}

Owner* ownerFor(HWND hwnd, std::uint64_t epoch) noexcept {
    if (!g_registry) return nullptr;
    const auto found = g_registry->owners.find(hwnd);
    if (found == g_registry->owners.end() || found->second.epoch != epoch) return nullptr;
    return &found->second;
}

bool isCurrentOwner(HWND hwnd, std::uint64_t epoch) noexcept {
    return ownerFor(hwnd, epoch) != nullptr;
}

ULONGLONG nativeNow() noexcept {
    return GetTickCount64();
}

void nativeEnumerate(WNDENUMPROC callback, LPARAM parameter) noexcept {
    EnumThreadWindows(GetCurrentThreadId(), callback, parameter);
}

bool nativeDescribe(HWND window, Popup& popup) noexcept {
    popup = Popup{};
    if (window == nullptr || !IsWindow(window)) return false;
    popup.thread = GetWindowThreadProcessId(window, nullptr);
    popup.visible = IsWindowVisible(window) != FALSE;
    wchar_t className[32] = {};
    const int length =
        GetClassNameW(window, className, static_cast<int>(sizeof(className) / sizeof(className[0])));
    popup.menuClass = length > 0 && lstrcmpW(className, L"#32768") == 0;
    MENUBARINFO info = {};
    info.cbSize = sizeof(info);
    if (GetMenuBarInfo(window, OBJID_CLIENT, 0, &info) != FALSE) popup.menu = info.hMenu;
    return true;
}

void nativePaint(HWND popup) noexcept {
    RedrawWindow(popup, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
}

UINT_PTR nativeArm(HWND owner, UINT_PTR timer, UINT interval) noexcept {
    return SetTimer(owner, timer, interval, nullptr);
}

void nativeDisarm(HWND owner, UINT_PTR timer) noexcept {
    KillTimer(owner, timer);
}

bool nativeInstall(HWND owner, SUBCLASSPROC procedure, UINT_PTR id, DWORD_PTR data) noexcept {
    return SetWindowSubclass(owner, procedure, id, data) != FALSE;
}

Ticket beginOpening(HWND hwnd, std::uint64_t ownerEpoch, HMENU menu) noexcept {
    Ticket ticket;
    if (menu == nullptr || !IsMenu(menu)) return ticket;
    try {
        const ULONGLONG opened = platform().now();
        Owner* owner = ownerFor(hwnd, ownerEpoch);
        if (owner == nullptr) return ticket;
        const std::uint64_t openingEpoch = nextToken();
        if (openingEpoch == 0) return ticket;
        Opening opening;
        opening.epoch = openingEpoch;
        opening.menu = menu;
        opening.opened = opened;
        bool replaced = false;
        for (Opening& existing : owner->openings) {
            if (existing.menu != menu) continue;
            existing = opening;
            replaced = true;
            break;
        }
        if (!replaced) {
            try {
                owner->openings.push_back(opening);
            } catch (...) {
                return ticket;
            }
        }
        ticket.valid = true;
        ticket.ownerEpoch = ownerEpoch;
        ticket.menu = menu;
        ticket.openingEpoch = openingEpoch;
    } catch (...) {
        ticket = Ticket{};
    }
    return ticket;
}

void completeOpening(HWND hwnd, const Ticket& ticket) noexcept {
    if (!ticket.valid) return;
    try {
        Owner* owner = ownerFor(hwnd, ticket.ownerEpoch);
        if (owner == nullptr) return;
        Opening* target = nullptr;
        for (Opening& opening : owner->openings) {
            if (opening.epoch == ticket.openingEpoch && opening.menu == ticket.menu) {
                target = &opening;
                break;
            }
        }
        // The original handler may have destroyed, detached, exited, uninitialised
        // or reopened the menu. Only the exact opening observed by Begin may be
        // armed, so a stale Complete can never revive a cancelled generation.
        if (target == nullptr) return;
        target->ready = true;
        const Platform p = platform();
        if (p.arm(hwnd, detail::kTimer, kPollIntervalMs) == 0) {
            // Without a poll timer the opening can never paint. Drop this owner's
            // pending work but keep the attachment for the next menu.
            owner->openings.clear();
            owner->armed = false;
            p.disarm(hwnd, detail::kTimer);
            return;
        }
        owner->armed = true;
    } catch (...) {
    }
}

void forgetMenu(HWND hwnd, std::uint64_t ownerEpoch, HMENU menu) noexcept {
    if (menu == nullptr) return;
    try {
        Owner* owner = ownerFor(hwnd, ownerEpoch);
        if (owner == nullptr) return;
        std::vector<Opening>& openings = owner->openings;
        openings.erase(std::remove_if(openings.begin(), openings.end(),
                                      [menu](const Opening& opening) {
                                          return opening.menu == menu;
                                      }),
                       openings.end());
        if (openings.empty()) {
            owner->armed = false;
            platform().disarm(hwnd, detail::kTimer);
        }
    } catch (...) {
    }
}

void clearOpenings(HWND hwnd, std::uint64_t ownerEpoch) noexcept {
    try {
        Owner* owner = ownerFor(hwnd, ownerEpoch);
        if (owner == nullptr) return;
        owner->openings.clear();
        owner->armed = false;
        platform().disarm(hwnd, detail::kTimer);
    } catch (...) {
    }
}

void forgetOwner(HWND hwnd, std::uint64_t ownerEpoch) noexcept {
    try {
        if (!g_registry) return;
        const auto found = g_registry->owners.find(hwnd);
        if (found == g_registry->owners.end() || found->second.epoch != ownerEpoch) return;
        g_registry->owners.erase(found);
        platform().disarm(hwnd, detail::kTimer);
    } catch (...) {
    }
}

void paintOpening(const Visit& visit, HMENU menu, HWND popup, bool fromIdle) noexcept {
    try {
        if (menu == nullptr) return;
        Owner* owner = ownerFor(visit.owner, visit.ownerEpoch);
        if (owner == nullptr) return;
        Opening* target = nullptr;
        for (Opening& opening : owner->openings) {
            if (opening.menu != menu) continue;
            // A newer generation of the same HMENU was created by a synchronous
            // reopen while describe was running. It belongs to a later visit and
            // must not receive this visit's observation.
            if (opening.epoch > visit.openingCutoff) return;
            if (!opening.ready || opening.finished) return;
            target = &opening;
            break;
        }
        if (target == nullptr) return;
        const Platform p = platform();
        const ULONGLONG now = p.now();
        const bool finalPaint =
            target->first && now >= target->visibleSince &&
            now - target->visibleSince >= kFinalPaintDelayMs;
        const bool paint = !target->first || (fromIdle && !target->idle) || finalPaint;
        if (!paint) return;
        if (!target->first) {
            target->first = true;
            target->visibleSince = now;
            if (fromIdle) target->idle = true;
        } else if (fromIdle) {
            target->idle = true;
        }
        if (finalPaint) target->finished = true;
        // Commit the state first: RedrawWindow may synchronously reenter the owner
        // and invalidate every reference held here.
        p.paint(popup);
    } catch (...) {
    }
}

BOOL CALLBACK paintEnumProc(HWND popup, LPARAM parameter) {
    try {
        const Visit* visit = reinterpret_cast<const Visit*>(parameter);
        if (visit == nullptr) return TRUE;
        if (!isCurrentOwner(visit->owner, visit->ownerEpoch)) return FALSE;
        const Platform p = platform();
        Popup description{};
        if (!p.describe(popup, description)) return TRUE;
        if (!description.visible || !description.menuClass) return TRUE;
        if (description.thread != GetCurrentThreadId()) return TRUE;
        if (description.menu == nullptr) return TRUE;
        // describe may synchronously reenter the owner; verify it again.
        if (!isCurrentOwner(visit->owner, visit->ownerEpoch)) return FALSE;
        paintOpening(*visit, description.menu, popup, false);
    } catch (...) {
        return TRUE;
    }
    return TRUE;
}

void handleIdle(const Visit& visit, HWND popup) noexcept {
    if (popup == nullptr) return;
    try {
        if (!isCurrentOwner(visit.owner, visit.ownerEpoch)) return;
        const Platform p = platform();
        Popup description{};
        if (!p.describe(popup, description)) return;
        if (!description.visible || !description.menuClass) return;
        if (description.thread != GetCurrentThreadId()) return;
        if (description.menu == nullptr) return;
        // describe may synchronously reenter the owner; verify it again.
        if (!isCurrentOwner(visit.owner, visit.ownerEpoch)) return;
        paintOpening(visit, description.menu, popup, true);
    } catch (...) {
    }
}

void handleTimer(HWND owner, std::uint64_t ownerEpoch) noexcept {
    try {
        // A residual delivery for a detached, reattached or disarmed owner must
        // not enumerate popups or touch whatever timer the window now owns.
        Owner* start = ownerFor(owner, ownerEpoch);
        if (start == nullptr || !start->armed) return;
        const Platform p = platform();
        const Visit visit{owner, ownerEpoch, tokenCutoff()};
        p.enumerate(&paintEnumProc, reinterpret_cast<LPARAM>(&visit));
        // Enumeration and painting may detach, destroy, reopen or reattach.
        // Requery the owner instead of trusting state observed before the poll.
        Owner* current = ownerFor(owner, ownerEpoch);
        if (current == nullptr) return;
        const ULONGLONG now = p.now();
        bool polling = false;
        for (const Opening& opening : current->openings) {
            if (!opening.ready || opening.finished) continue;
            const ULONGLONG since = opening.first ? opening.visibleSince : opening.opened;
            if (now >= since && now - since < kPollDeadlineMs) {
                polling = true;
                break;
            }
        }
        if (!polling) {
            // Timeout only stops polling; it never fabricates a paint result.
            current->armed = false;
            p.disarm(owner, detail::kTimer);
        }
    } catch (...) {
    }
}

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                           DWORD_PTR refData) {
    const std::uint64_t ownerEpoch = static_cast<std::uint64_t>(refData);
    if (id == detail::kSubclass) {
        switch (message) {
        case WM_TIMER:
            if (wParam == detail::kTimer) {
                handleTimer(hwnd, ownerEpoch);
                return 0;
            }
            break;
        case WM_DESTROY:
            forgetOwner(hwnd, ownerEpoch);
            return DefSubclassProc(hwnd, message, wParam, lParam);
        case WM_NCDESTROY: {
            // Remove before forwarding so a new attachment created by the
            // original procedure cannot be deleted by this stale tail.
            DWORD_PTR installed = 0;
            if (GetWindowSubclass(hwnd, &OwnerProc, id, &installed) &&
                static_cast<std::uint64_t>(installed) == ownerEpoch) {
                RemoveWindowSubclass(hwnd, &OwnerProc, id);
            }
            forgetOwner(hwnd, ownerEpoch);
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }
        case WM_INITMENUPOPUP: {
            const Ticket ticket =
                beginOpening(hwnd, ownerEpoch, reinterpret_cast<HMENU>(wParam));
            const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
            completeOpening(hwnd, ticket);
            return result;
        }
        case WM_UNINITMENUPOPUP:
            forgetMenu(hwnd, ownerEpoch, reinterpret_cast<HMENU>(wParam));
            break;
        case WM_EXITMENULOOP:
            clearOpenings(hwnd, ownerEpoch);
            break;
        case WM_ENTERIDLE: {
            if (wParam != MSGF_MENU) break;
            // Capture the generation before forwarding: the original procedure
            // may reopen a menu while handling the idle notification.
            const Visit visit{hwnd, ownerEpoch, tokenCutoff()};
            const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
            handleIdle(visit, reinterpret_cast<HWND>(lParam));
            return result;
        }
        default:
            break;
        }
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

}  // namespace

bool Attach(HWND owner) noexcept {
    if (build_profile::original) return false;
    if (owner == nullptr || !IsWindow(owner)) return false;
    if (GetWindowThreadProcessId(owner, nullptr) != GetCurrentThreadId()) return false;
    try {
        if (!g_registry) g_registry = std::make_unique<Registry>();
        const Platform p = platform();
        const auto existing = g_registry->owners.find(owner);
        if (existing != g_registry->owners.end()) {
            // A repeated Attach must not install a second subclass. Confirm the
            // remembered attachment is still live before trusting the mapping.
            DWORD_PTR installed = 0;
            if (GetWindowSubclass(owner, &OwnerProc, detail::kSubclass, &installed) &&
                static_cast<std::uint64_t>(installed) == existing->second.epoch) {
                return true;
            }
            // The subclass is gone or belongs to an older generation. Drop the
            // stale mapping and timer, then reinstall under a fresh owner epoch
            // so tickets from the old generation can never revive.
            const bool armed = existing->second.armed;
            g_registry->owners.erase(existing);
            if (armed) p.disarm(owner, detail::kTimer);
            RemoveWindowSubclass(owner, &OwnerProc, detail::kSubclass);
            const std::uint64_t freshEpoch = nextToken();
            if (freshEpoch == 0) return false;
            Owner fresh;
            fresh.epoch = freshEpoch;
            const auto inserted = g_registry->owners.emplace(owner, std::move(fresh));
            if (!inserted.second) return false;
            if (p.install(owner, &OwnerProc, detail::kSubclass,
                          static_cast<DWORD_PTR>(freshEpoch))) {
                return true;
            }
            g_registry->owners.erase(inserted.first);
            return false;
        }
        const std::uint64_t epoch = nextToken();
        if (epoch == 0) return false;
        Owner fresh;
        fresh.epoch = epoch;
        const auto inserted = g_registry->owners.emplace(owner, std::move(fresh));
        if (!inserted.second) return false;
        if (p.install(owner, &OwnerProc, detail::kSubclass, static_cast<DWORD_PTR>(epoch))) {
            return true;
        }
        g_registry->owners.erase(inserted.first);
        return false;
    } catch (...) {
        return false;
    }
}

void Detach(HWND owner) noexcept {
    if (build_profile::original) return;
    if (owner == nullptr || !g_registry) return;
    if (!IsWindow(owner) || GetWindowThreadProcessId(owner, nullptr) != GetCurrentThreadId()) return;
    try {
        const auto found = g_registry->owners.find(owner);
        if (found == g_registry->owners.end()) return;
        const bool armed = found->second.armed;
        g_registry->owners.erase(found);
        const Platform p = platform();
        if (armed) p.disarm(owner, detail::kTimer);
        RemoveWindowSubclass(owner, &OwnerProc, detail::kSubclass);
    } catch (...) {
    }
}

namespace detail {

const Platform& NativePlatform() noexcept {
    static const Platform platform = {
        &nativeNow, &nativeEnumerate, &nativeDescribe, &nativePaint,
        &nativeArm, &nativeDisarm,     &nativeInstall,
    };
    return platform;
}

bool SetPlatformForTesting(const Platform* replacement) noexcept {
    try {
        if (AttachmentCount() != 0) return false;
        if (replacement == nullptr) {
            g_testingPlatform = false;
            g_platformOverride = Platform{};
            return true;
        }
        if (replacement->now == nullptr || replacement->enumerate == nullptr ||
            replacement->describe == nullptr || replacement->paint == nullptr ||
            replacement->arm == nullptr || replacement->disarm == nullptr ||
            replacement->install == nullptr) {
            return false;
        }
        g_platformOverride = *replacement;
        g_testingPlatform = true;
        return true;
    } catch (...) {
        return false;
    }
}

Snapshot Inspect(HWND owner, HMENU menu) noexcept {
    Snapshot snapshot;
    try {
        if (owner == nullptr || !g_registry) return snapshot;
        const auto found = g_registry->owners.find(owner);
        if (found == g_registry->owners.end()) return snapshot;
        const Owner& ownerState = found->second;
        snapshot.attached = true;
        snapshot.armed = ownerState.armed;
        snapshot.openings = ownerState.openings.size();
        snapshot.ownerEpoch = ownerState.epoch;
        if (menu == nullptr) return snapshot;
        for (const Opening& opening : ownerState.openings) {
            if (opening.menu != menu) continue;
            snapshot.found = true;
            snapshot.ready = opening.ready;
            snapshot.first = opening.first;
            snapshot.idle = opening.idle;
            snapshot.finished = opening.finished;
            snapshot.openingEpoch = opening.epoch;
            snapshot.opened = opening.opened;
            snapshot.visibleSince = opening.visibleSince;
            break;
        }
    } catch (...) {
    }
    return snapshot;
}

std::size_t AttachmentCount() noexcept {
    if (!g_registry) return 0;
    return g_registry->owners.size();
}

}  // namespace detail
}  // namespace mikumikustudio::menu_initial_paint
