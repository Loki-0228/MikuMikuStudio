// Interface lock for a running bake.
//
// ScopedUiLock disables every enabled top-level window of the GUI thread except
// the progress dialog, and restores only the windows it disabled itself: already
// disabled windows keep their state, and nothing is hidden, activated, focused
// or re-enabled.  The window list is collected - and a failing allocation is
// carried out of the Win32 enumeration and rethrown - before the first window is
// disabled, so a throwing constructor cannot leave a half-locked interface.
// UiLocked() reports the calling thread's lock, so queued input and menu
// commands are refused while a bake pumps messages.

#pragma once

#include <Windows.h>

#include <exception>
#include <vector>

namespace mikumikustudio {
namespace bake {
namespace detail {

// Lock depth of this thread, shared by every translation unit that includes
// this header.
inline int& UiLockDepth() {
    thread_local int depth = 0;
    return depth;
}

}  // namespace detail

// True while the calling thread holds at least one ScopedUiLock.
inline bool UiLocked() {
    return detail::UiLockDepth() > 0;
}

class ScopedUiLock {
public:
    // `progressDialog` is the window that owns the running bake and stays
    // enabled for the whole run.  A null handle locks every top-level window of
    // the thread.
    explicit ScopedUiLock(HWND progressDialog) : progress_(progressDialog) {
        Collection collection;
        EnumThreadWindows(GetCurrentThreadId(), &Collect,
                          reinterpret_cast<LPARAM>(&collection));
        if (collection.error) std::rethrow_exception(collection.error);
        // Every allocation happens before the first window is disabled.
        disabled_.reserve(collection.windows.size());
        for (HWND window : collection.windows) {
            if (window == progress_ || IsWindowEnabled(window) == FALSE) continue;
            disabled_.push_back(window);
        }
        for (HWND window : disabled_) EnableWindow(window, FALSE);
        ++detail::UiLockDepth();
    }

    ~ScopedUiLock() {
        --detail::UiLockDepth();
        for (HWND window : disabled_)
            if (IsWindow(window)) EnableWindow(window, TRUE);
    }

    ScopedUiLock(const ScopedUiLock&) = delete;
    ScopedUiLock& operator=(const ScopedUiLock&) = delete;
    ScopedUiLock(ScopedUiLock&&) = delete;
    ScopedUiLock& operator=(ScopedUiLock&&) = delete;

private:
    struct Collection {
        std::vector<HWND> windows;
        std::exception_ptr error;
    };

    // A C++ exception must not cross the Win32 enumeration boundary: a failing
    // allocation is remembered here and rethrown by the constructor.
    static BOOL CALLBACK Collect(HWND window, LPARAM context) {
        auto& collection = *reinterpret_cast<Collection*>(context);
        try {
            collection.windows.push_back(window);
        } catch (...) {
            collection.error = std::current_exception();
            return FALSE;
        }
        return TRUE;
    }

    HWND progress_ = nullptr;
    std::vector<HWND> disabled_;
};

}  // namespace bake
}  // namespace mikumikustudio
