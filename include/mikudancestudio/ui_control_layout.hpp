#pragma once
#include <Windows.h>
#include <CommCtrl.h>
#include <vector>
#include "mikudancestudio/build_profile.hpp"
#include "mikudancestudio/window_dpi_scope.hpp"

namespace mikudancestudio::ui {
struct LayoutChange {
    HWND window = nullptr, parent = nullptr;
    RECT before{}, after{}; // Actual collapsed bounds in parent client coordinates.
};
namespace layout_detail {
inline RECT ActualRect(HWND parent, HWND child) {
    const WindowDpiScope dpi(parent ? parent : child);
    RECT rect{};
    if (GetWindowRect(child, &rect))
        MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}
inline bool Combo(HWND window) {
    wchar_t name[32]{};
    return GetClassNameW(window, name, 32) && _wcsicmp(name, L"COMBOBOX") == 0;
}
inline int RequestedHeight(HWND window, const RECT& collapsed) {
    const WindowDpiScope dpi(window);
    RECT dropped{};
    if (Combo(window) && SendMessageW(window, CB_GETDROPPEDCONTROLRECT, 0,
                                     reinterpret_cast<LPARAM>(&dropped)))
        return int(dropped.bottom - dropped.top);
    return int(collapsed.bottom - collapsed.top);
}
inline constexpr wchar_t kComboRequest[] = L"MMS.Internal.LayoutCombo.RequestHeight";
inline constexpr wchar_t kComboActual[] = L"MMS.Internal.LayoutCombo.ActualHeight";
inline bool HeightMatches(HWND window, const RECT& collapsed, int height) {
    const int actual = RequestedHeight(window, collapsed);
    if (actual == height) return true;
    // Combo popups may round the requested height to complete list rows. The
    // properties contain integers, not owned allocations; HWND destruction
    // removes them. A later external resize invalidates the recorded pair.
    const auto requestedProperty = GetPropW(window, kComboRequest);
    const auto actualProperty = GetPropW(window, kComboActual);
    return height >= 0 && actual >= 0 && Combo(window) && requestedProperty && actualProperty &&
        reinterpret_cast<INT_PTR>(requestedProperty) == INT_PTR(height) + 1 &&
        reinterpret_cast<INT_PTR>(actualProperty) == INT_PTR(actual) + 1;
}
inline void RememberHeight(HWND window, int requested) {
    if (!Combo(window)) return;
    const auto rect = ActualRect(GetParent(window), window);
    if (requested < 0) {
        RemovePropW(window, kComboRequest); RemovePropW(window, kComboActual); return;
    }
    SetPropW(window, kComboRequest, reinterpret_cast<HANDLE>(INT_PTR(requested) + 1));
    SetPropW(window, kComboActual, reinterpret_cast<HANDLE>(INT_PTR(RequestedHeight(window, rect)) + 1));
}
inline constexpr UINT kMoveFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS;

// Some native controls resize themselves again from WM_SIZE (notably Combo),
// dropping the outer SWP_NOREDRAW flag. Keep those nested moves and display
// paints inside the same transaction. Never use WM_SETREDRAW here: its default
// implementation changes WS_VISIBLE, which breaks visibility and hit testing.
class PendingPaint {
    HWND window_ = nullptr;
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                      UINT_PTR id, DWORD_PTR data) {
        auto* self = reinterpret_cast<PendingPaint*>(data);
        if (message == WM_NCDESTROY) {
            self->window_ = nullptr; RemoveWindowSubclass(window, Procedure, id);
        } else if (message == WM_WINDOWPOSCHANGING && lp) {
            auto* position = reinterpret_cast<WINDOWPOS*>(lp);
            position->flags |= SWP_NOREDRAW | SWP_NOCOPYBITS;
            const auto result = DefSubclassProc(window, message, wp, lp);
            // An existing subclass or the control itself can rewrite flags.
            position->flags |= SWP_NOREDRAW | SWP_NOCOPYBITS;
            return result;
        } else if ((message == WM_PAINT && !wp) || message == WM_NCPAINT) {
            // Do not BeginPaint/ValidateRect: the final repaint must still
            // consume every invalidated pixel and the nonclient frame.
            return 0;
        } else if (message == WM_ERASEBKGND) {
            const auto dc = reinterpret_cast<HDC>(wp);
            if (dc && GetObjectType(dc) == OBJ_DC && WindowFromDC(dc) == window) return 1;
        }
        return DefSubclassProc(window, message, wp, lp);
    }
public:
    PendingPaint() = default;
    PendingPaint(const PendingPaint&) = delete;
    PendingPaint& operator=(const PendingPaint&) = delete;
    bool Attach(HWND window) noexcept {
        if (!SetWindowSubclass(window, Procedure, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this))) return false;
        window_ = window; return true;
    }
    void Release() noexcept {
        if (window_) RemoveWindowSubclass(window_, Procedure, reinterpret_cast<UINT_PTR>(this));
        window_ = nullptr;
    }
    ~PendingPaint() { Release(); }
};

// Native child painting can enter plugin code and its nested message pump.
// Keep a requested host close queued until the whole synchronous repaint ends.
class PaintCloseGuard {
    HWND window_ = nullptr;
    bool requested_ = false;
    WPARAM wp_ = 0;
    LPARAM lp_ = 0;
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                      UINT_PTR id, DWORD_PTR data) {
        auto& self = *reinterpret_cast<PaintCloseGuard*>(data);
        if (message == WM_CLOSE) {
            if (!self.requested_) { self.requested_ = true; self.wp_ = wp; self.lp_ = lp; }
            return 0;
        }
        if (message == WM_NCDESTROY) {
            self.window_ = nullptr;
            RemoveWindowSubclass(window, Procedure, id);
        }
        return DefSubclassProc(window, message, wp, lp);
    }
public:
    PaintCloseGuard() = default;
    PaintCloseGuard(const PaintCloseGuard&) = delete;
    PaintCloseGuard& operator=(const PaintCloseGuard&) = delete;
    bool Attach(HWND parent) noexcept {
        const HWND root = GetAncestor(parent, GA_ROOT);
        if (!root || !SetWindowSubclass(root, Procedure, reinterpret_cast<UINT_PTR>(this),
                                       reinterpret_cast<DWORD_PTR>(this))) return false;
        window_ = root; return true;
    }
    ~PaintCloseGuard() {
        if (!window_) return;
        RemoveWindowSubclass(window_, Procedure, reinterpret_cast<UINT_PTR>(this));
        if (requested_) PostMessageW(window_, WM_CLOSE, wp_, lp_);
    }
};
}

// UI-thread scope. Windows must remain alive until Commit returns. Destruction
// discards pending requests and never applies a failed batch while unwinding.
// Nested scopes commit independently, but may read outer pending rectangles.
class LayoutBatch {
    struct Move {
        HWND window, parent;
        RECT before;
        int x, y, width, height;
        int popupBefore;
        bool combo;
        bool applied = false;
    };
    inline static thread_local LayoutBatch* active_ = nullptr;
    LayoutBatch* previous_ = nullptr;
    std::vector<Move> pending_;
    struct FontChange { HWND window; HFONT before, after; };
    std::vector<FontChange> fonts_;
    std::vector<LayoutChange> changes_;
    std::vector<LayoutChange> external_;
    bool finished_ = false;
    void RestoreFonts() noexcept {
        // Preserve a different font committed by an independent nested scope.
        for (const auto& font : fonts_)
            if (IsWindow(font.window) && reinterpret_cast<HFONT>(SendMessageW(font.window, WM_GETFONT, 0, 0)) == font.after)
                SendMessageW(font.window, WM_SETFONT, reinterpret_cast<WPARAM>(font.before), FALSE);
        fonts_.clear();
    }
    static LayoutBatch* Collecting() noexcept {
        for (auto* batch = active_; batch; batch = batch->previous_)
            if (!batch->finished_) return batch;
        return nullptr;
    }
    friend RECT ReadLayoutRect(HWND, HWND);
    friend int ReadLayoutRequestedHeight(HWND);
    friend void MoveLayoutWindow(HWND, int, int, int, int, bool);
    friend void SetLayoutFont(HWND, HFONT);
    friend void ReserveLayoutChanges(std::size_t);
    friend bool RecordLayoutChange(const LayoutChange&) noexcept;
    bool Queue(HWND window, int x, int y, int width, int height) {
        if (finished_) return false;
        for (auto& move : pending_) if (move.window == window) {
            move.x = x; move.y = y; move.width = width; move.height = height;
            return true;
        }
        HWND parent = GetParent(window);
        const auto before = layout_detail::ActualRect(parent, window);
        pending_.push_back({window, parent, before, x, y, width, height,
                            layout_detail::RequestedHeight(window, before), layout_detail::Combo(window)});
        return true;
    }
public:
    LayoutBatch() : previous_(active_) { active_ = this; }
    LayoutBatch(const LayoutBatch&) = delete;
    LayoutBatch& operator=(const LayoutBatch&) = delete;
    ~LayoutBatch() {
        active_ = previous_;
        // Measurements use the new font while geometry is still queued. An
        // abandoned transaction must not leave that font on the old layout.
        if (!finished_) RestoreFonts();
    }
    static bool IsCollecting() noexcept { return Collecting() != nullptr; }
    const std::vector<LayoutChange>& Changes() const { return changes_; }
    // False means at least one final move still failed after fallback. Changes
    // reports actual geometry even on partial failure and must still be repainted.
    bool Commit() {
        if (finished_) return false;
        // External hosts have already changed their windows. Preserve their
        // repaint obligations even if allocation or guard installation fails.
        changes_.swap(external_);
        struct Preparation {
            LayoutBatch& owner;
            bool ready = false;
            ~Preparation() {
                if (!ready) { owner.finished_ = true; owner.RestoreFonts(); }
            }
        } preparation{*this};
        changes_.reserve(pending_.size() + fonts_.size() + changes_.size());
        std::vector<std::vector<Move*>> groups;
        for (auto& move : pending_) {
            const RECT current = layout_detail::ActualRect(move.parent, move.window);
            // An inner independent transaction may already have moved this
            // window. Damage starts at its actual position at commit time.
            move.before = current;
            move.popupBefore = layout_detail::RequestedHeight(move.window, current);
            if (current.left == move.x && current.top == move.y &&
                current.right - current.left == move.width &&
                layout_detail::HeightMatches(move.window, current, move.height)) continue;
            bool grouped = false;
            for (auto& group : groups) if (group.front()->parent == move.parent) {
                group.push_back(&move); grouped = true; break;
            }
            if (!grouped) groups.push_back({&move});
        }
        // Allocate and install guards before the first geometry mutation.
        // Installation failure leaves queued geometry untouched and restores
        // fonts before returning; no partial guard is allowed to escape.
        std::size_t guardedCount = 0;
        for (const auto& group : groups) guardedCount += group.size();
        std::vector<layout_detail::PendingPaint> guards(guardedCount);
        std::size_t guardIndex = 0;
        for (const auto& group : groups) for (const auto* move : group)
            if (!guards[guardIndex++].Attach(move->window)) return false;
        // All allocating preparation precedes the first geometry mutation.
        finished_ = true;
        preparation.ready = true;
        bool success = true;
        for (auto& group : groups) {
            const WindowDpiScope dpi(group.front()->parent);
            HDWP deferred = BeginDeferWindowPos(int(group.size()));
            if (deferred) for (const auto* move : group) {
                deferred = DeferWindowPos(deferred, move->window, nullptr,
                    move->x, move->y, move->width, move->height, layout_detail::kMoveFlags);
                if (!deferred) break;
            }
            const bool applied = deferred && EndDeferWindowPos(deferred);
            for (auto* move : group) {
                move->applied = applied || SetWindowPos(move->window, nullptr, move->x, move->y,
                    move->width, move->height, layout_detail::kMoveFlags);
                if (!move->applied) success = false;
                else layout_detail::RememberHeight(move->window, move->height);
            }
            for (const auto* move : group) {
                const RECT after = layout_detail::ActualRect(move->parent, move->window);
                // Popup-only changes may leave collapsed rectangles equal.
                if (!EqualRect(&move->before, &after) ||
                    layout_detail::RequestedHeight(move->window, after) != move->popupBefore)
                    changes_.push_back({move->window, move->parent, move->before, after});
            }
        }
        // A font change can require painting even when final bounds match.
        for (const auto& font : fonts_) {
            const auto window = font.window;
            bool recorded = false;
            for (const auto& change : changes_) if (change.window == window) { recorded = true; break; }
            if (!recorded && IsWindow(window)) {
                const auto parent = GetParent(window);
                const auto rect = layout_detail::ActualRect(parent, window);
                changes_.push_back({window, parent, rect, rect});
            }
        }
        return success;
    }
};

// A host with its own geometry/publication transaction can hand its paint
// obligations to an outer layout scope without delaying its native geometry.
// Reserve before that host mutates any window; recording is allocation-free.
inline void ReserveLayoutChanges(std::size_t count) {
    if (auto* batch = LayoutBatch::Collecting())
        batch->external_.reserve(batch->external_.capacity() + count);
}
inline bool RecordLayoutChange(const LayoutChange& change) noexcept {
    auto* batch = LayoutBatch::Collecting();
    if (!batch || batch->external_.size() == batch->external_.capacity()) return false;
    batch->external_.push_back(change);
    return true;
}

inline RECT ReadLayoutRect(HWND parent, HWND child) {
    const WindowDpiScope dpi(parent ? parent : child);
    for (auto* batch = LayoutBatch::active_; batch; batch = batch->previous_)
        if (!batch->finished_) for (const auto& move : batch->pending_) if (move.window == child) {
            RECT rect{move.x, move.y, move.x + move.width,
                      move.y + (move.combo ? move.before.bottom - move.before.top : move.height)};
            if (parent != move.parent)
                MapWindowPoints(move.parent, parent, reinterpret_cast<POINT*>(&rect), 2);
            return rect;
        }
    return layout_detail::ActualRect(parent, child);
}
// Preserve a queued popup-height request when a later layout pass changes only
// the combo's width. Reading its current rounded dropdown would shrink it again.
inline int ReadLayoutRequestedHeight(HWND child) {
    for (auto* batch = LayoutBatch::active_; batch; batch = batch->previous_)
        if (!batch->finished_) for (const auto& move : batch->pending_)
            if (move.window == child) return move.height;
    const auto rect = layout_detail::ActualRect(GetParent(child), child);
    return layout_detail::RequestedHeight(child, rect);
}
inline void MoveLayoutWindow(HWND control, int x, int y, int width, int height, bool deferPaint) {
    if (!control) return;
    // The transaction owns all layout calls made in its scope, including
    // callers that normally request an immediate repaint. Otherwise an inner
    // helper can erase a control without recording it for the final repaint.
    if (auto* batch = LayoutBatch::Collecting()) {
        batch->Queue(control, x, y, width, height); return;
    }
    const auto parent = GetParent(control);
    const WindowDpiScope dpi(parent ? parent : control);
    const auto rect = layout_detail::ActualRect(parent, control);
    if ((!build_profile::original || deferPaint) && rect.left == x && rect.top == y && rect.right - rect.left == width &&
        layout_detail::HeightMatches(control, rect, height)) return;
    if (deferPaint) {
        if (SetWindowPos(control, nullptr, x, y, width, height, layout_detail::kMoveFlags))
            layout_detail::RememberHeight(control, height);
    } else {
        if (MoveWindow(control, x, y, width, height, TRUE))
            layout_detail::RememberHeight(control, height);
    }
}
inline void SetLayoutFont(HWND control, HFONT font) {
    if (!control) return;
    const auto before = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    if (before == font) return;
    const bool deferred = LayoutBatch::IsCollecting();
    if (deferred) {
        // Reserve the repaint obligation before changing native state.
        auto& fonts = LayoutBatch::Collecting()->fonts_;
        bool found = false;
        for (auto& item : fonts) if (item.window == control) { item.after = font; found = true; break; }
        if (!found) fonts.push_back({control, before, font});
    }
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), !deferred);
}
inline void MoveLayoutControl(HWND parent, int id, int x, int y, int width, int height, bool deferPaint) {
    MoveLayoutWindow(GetDlgItem(parent, id), x, y, width, height, deferPaint);
}
// The parent owns its exposed chrome; only controls whose final geometry changed
// need native erasing/nonclient painting. Never erase every sibling on a drag.
inline void RepaintLayout(HWND parent, const std::vector<LayoutChange>& changes, RECT damage) {
    const WindowDpiScope dpi(parent);
    const auto invalidateParent = [&](HWND owner, RECT bounds) {
        if (!IsWindow(owner)) return;
        const WindowDpiScope ownerDpi(owner);
        RECT client{}; GetClientRect(owner, &client);
        for (const auto& change : changes) if (change.parent == owner) {
            RECT clipped{};
            if (IntersectRect(&clipped, &client, &change.before)) UnionRect(&bounds, &bounds, &clipped);
            if (IntersectRect(&clipped, &client, &change.after)) UnionRect(&bounds, &bounds, &clipped);
        }
        if (!IsRectEmpty(&bounds)) RedrawWindow(owner, &bounds, nullptr, RDW_INVALIDATE | RDW_NOCHILDREN);
    };
    const auto firstParent = [&](std::size_t index) {
        const auto owner = changes[index].parent;
        if (owner == parent) return false;
        for (std::size_t before = 0; before < index; ++before)
            if (changes[before].parent == owner) return false;
        return true;
    };
    // Register the complete damage first. RDW_UPDATENOW sends standalone
    // erase messages before WM_PAINT; that exposes intermediate backgrounds
    // and bypasses buffering around BeginPaint. UpdateWindow lets each native
    // paint consume its pending erase inside the paint transaction instead.
    invalidateParent(parent, damage);
    for (std::size_t i = 0; i < changes.size(); ++i)
        if (firstParent(i)) invalidateParent(changes[i].parent, RECT{});
    for(const auto& change:changes)if(IsWindow(change.window)) {
        const WindowDpiScope childDpi(change.window);
        RedrawWindow(change.window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);
    }
    layout_detail::PaintCloseGuard close;
    // On hook failure retain every update region for the normal message pump;
    // do not synchronously enter arbitrary child code without a lifetime guard.
    if (!build_profile::original && !close.Attach(parent)) return;
    UpdateWindow(parent);
    for (std::size_t i = 0; i < changes.size(); ++i) if (firstParent(i) && IsWindow(changes[i].parent)) {
        const WindowDpiScope ownerDpi(changes[i].parent); UpdateWindow(changes[i].parent);
    }
    for(const auto& change:changes)if(IsWindow(change.window)) {
        const WindowDpiScope childDpi(change.window);
        UpdateWindow(change.window);
        EnumChildWindows(change.window, [](HWND child, LPARAM) -> BOOL {
            const WindowDpiScope dpi(child); UpdateWindow(child); return TRUE;
        }, 0);
    }
}

}
