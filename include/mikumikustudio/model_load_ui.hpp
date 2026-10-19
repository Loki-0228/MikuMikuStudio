#pragma once
#include <Windows.h>

namespace mikumikustudio::model_load_ui {

// Internal loader scope for the owning window's UI thread. Nested scopes are
// supported and affect only their owner windows, which must outlive the scope.
// Destruction never refreshes:
// after a successful scene transaction the caller explicitly calls PostLoadInit
// on the committed model; failure leaves the previous selection UI untouched.
// This only defers the loader's final PostLoadInit, not other loader effects.
class DeferredRefresh final {
    HWND owner_;
    DWORD thread_;
    DeferredRefresh* previous_;
    friend bool Deferred(HWND) noexcept;
public:
    // A null/destroyed window or a call from another thread is rejected before
    // changing the active scope. Scopes must unwind on this thread in LIFO order.
    explicit DeferredRefresh(HWND owner);
    ~DeferredRefresh() noexcept;
    DeferredRefresh(const DeferredRefresh&) = delete;
    DeferredRefresh& operator=(const DeferredRefresh&) = delete;
    DeferredRefresh(DeferredRefresh&&) = delete;
    DeferredRefresh& operator=(DeferredRefresh&&) = delete;
};

// Read-only internal probe; scopes on another thread are not visible here.
bool Deferred(HWND owner) noexcept;
// PMD/PMX completion hook. Without a matching scope this has the same behavior
// as PostLoadInit. An explicit PostLoadInit call always bypasses deferral.
void PostLoadInitOrDefer(unsigned char* model);

}  // namespace mikumikustudio::model_load_ui
