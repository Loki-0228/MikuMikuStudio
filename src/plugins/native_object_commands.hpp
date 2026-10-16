#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <Windows.h>
#include <mms/plugin.h>
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
class SharedHistory;
// Internal migration routes only. A bound route consumes failure/cancellation;
// the caller must never continue into the legacy immediate-free deletion.
bool TryDeleteBoundModel(MMDApp*, HWND) noexcept;
bool TryDeleteBoundAccessory(MMDApp*, HWND) noexcept;
bool TryImportBoundModel(MMDApp*, const wchar_t*, bool showInfo) noexcept;
bool TryImportBoundAccessory(MMDApp*, const wchar_t*) noexcept;
// deferUi is only for the Manager's already-authorized top-level edit call.
// The caller queues the normal deferred publication notification afterwards.
MmsResult RunBoundObjectHistory(MMDApp&, SharedHistory&, bool redo, bool deferUi = false) noexcept;
// The operation guard covers confirmation and post-publication Win32 calls.
// Only its synchronous, message-free journal section receives authorization.
bool NativeObjectOperationActive(const MMDApp*) noexcept;
bool NativeObjectPublicationAuthorized(const MMDApp*) noexcept;
// Deferred plugin publication uses this same UI-only refresh. Failed control
// writes are retryable; save/reset must not serialize partial object lists.
bool RefreshBoundObjectUi(MMDApp&, SharedHistory&) noexcept;
bool NativeObjectUiPending(const MMDApp*) noexcept;
void ForgetBoundObjectUi(const MMDApp*) noexcept;
}
#endif
