#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"

namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// A synchronous internal type-1 edit. The app/scene owner must outlive the
// scope AND journal. Its resident model resources are budgeted by the caller;
// this participant charges only its new owned pose payload and bookkeeping.
// It does not write legacy rings, app dirty flags, controls or frame position.
// Do not dispatch messages, evaluate the scene or call plugins while active.
class NativePoseScope {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    NativePoseScope();
    ~NativePoseScope();
    NativePoseScope(const NativePoseScope&) = delete;
    NativePoseScope& operator=(const NativePoseScope&) = delete;
    MmsResult Begin(MMDApp&, SharedHistory&, unsigned modelSlot, std::string_view label,
        SharedHistory::NativeIntent = SharedHistory::NativeIntent::Adjustment) noexcept;
    // A no-op cancels its reservation and preserves redo. Every failure first
    // rolls back the native capture, then drops the journal reservation.
    MmsResult Commit(bool& changed) noexcept;
    bool Cancel() noexcept;
};

// Explicit, thread-local test mode for migrated production pose commands and
// their undo/redo buttons and shortcuts.
// No production initialization enables this: legacy writers are not all in
// the journal yet. Binding never exposes mms.edit. App/history lifetimes cover
// the binding, scopes and every retained node; the caller budgets models.
class NativePoseTestBinding {
    MMDApp* app_;
    SharedHistory* history_;
    NativePoseTestBinding* previous_;
    friend SharedHistory* BoundNativePoseHistory(MMDApp*) noexcept;
public:
    NativePoseTestBinding(MMDApp&, SharedHistory&) noexcept;
    ~NativePoseTestBinding();
    NativePoseTestBinding(const NativePoseTestBinding&) = delete;
    NativePoseTestBinding& operator=(const NativePoseTestBinding&) = delete;
};
SharedHistory* BoundNativePoseHistory(MMDApp*) noexcept;
// Changes on every bind/unbind, including a temporary nested binding that
// restores the same pointers. Modal callers must validate before dereferencing
// a journal captured before dispatching messages.
std::uint64_t BoundNativePoseGeneration(MMDApp*) noexcept;
// A registered outline property window holds no writable scene scratch while
// idle. Unregistered dialogs and active/reentrant previews retain the old gate.
bool NativeModelOutlineDialogBlocks(const MMDApp&) noexcept;
}
#endif
