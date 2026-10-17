#pragma once
#include <Windows.h>
#include <vector>

namespace mikudancestudio::ui {
struct LayoutChange {
    HWND window = nullptr, parent = nullptr;
    RECT before{}, after{}; // Actual collapsed bounds in parent client coordinates.
};
namespace layout_detail {
inline RECT ActualRect(HWND parent, HWND child) {
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
    std::vector<LayoutChange> changes_;
    bool finished_ = false;
    friend RECT ReadLayoutRect(HWND, HWND);
    friend int ReadLayoutRequestedHeight(HWND);
    friend void MoveLayoutWindow(HWND, int, int, int, int, bool);
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
    ~LayoutBatch() { active_ = previous_; }
    const std::vector<LayoutChange>& Changes() const { return changes_; }
    // False means at least one final move still failed after fallback. Changes
    // reports actual geometry even on partial failure and must still be repainted.
    bool Commit() {
        if (finished_) return false;
        changes_.reserve(pending_.size());
        std::vector<std::vector<Move*>> groups;
        for (auto& move : pending_) {
            const RECT current = layout_detail::ActualRect(move.parent, move.window);
            if (current.left == move.x && current.top == move.y &&
                current.right - current.left == move.width &&
                layout_detail::HeightMatches(move.window, current, move.height)) continue;
            bool grouped = false;
            for (auto& group : groups) if (group.front()->parent == move.parent) {
                group.push_back(&move); grouped = true; break;
            }
            if (!grouped) groups.push_back({&move});
        }
        // All allocating preparation precedes the first geometry mutation.
        finished_ = true;
        bool success = true;
        for (auto& group : groups) {
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
        return success;
    }
};

inline RECT ReadLayoutRect(HWND parent, HWND child) {
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
    if (deferPaint) {
        if (LayoutBatch::active_ && LayoutBatch::active_->Queue(control, x, y, width, height)) return;
        const auto parent = GetParent(control);
        const auto rect = layout_detail::ActualRect(parent, control);
        if (rect.left == x && rect.top == y && rect.right - rect.left == width &&
            layout_detail::HeightMatches(control, rect, height)) return;
        if (SetWindowPos(control, nullptr, x, y, width, height, layout_detail::kMoveFlags))
            layout_detail::RememberHeight(control, height);
    } else {
        if (MoveWindow(control, x, y, width, height, TRUE))
            layout_detail::RememberHeight(control, height);
    }
}
inline void MoveLayoutControl(HWND parent, int id, int x, int y, int width, int height, bool deferPaint) {
    MoveLayoutWindow(GetDlgItem(parent, id), x, y, width, height, deferPaint);
}
// The parent owns its exposed chrome; only controls whose final geometry changed
// need native erasing/nonclient painting. Never erase every sibling on a drag.
inline void RepaintLayout(HWND parent, const std::vector<LayoutChange>& changes, RECT damage) {
    RECT client{};GetClientRect(parent,&client);
    for(const auto& change:changes)if(change.parent==parent){
        RECT clipped{};if(IntersectRect(&clipped,&client,&change.before))UnionRect(&damage,&damage,&clipped);
        if(IntersectRect(&clipped,&client,&change.after))UnionRect(&damage,&damage,&clipped);
    }
    RedrawWindow(parent,&damage,nullptr,RDW_INVALIDATE|RDW_NOCHILDREN|RDW_UPDATENOW);
    for(const auto& change:changes)if(IsWindow(change.window))
        RedrawWindow(change.window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW);
}

}
