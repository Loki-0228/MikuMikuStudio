#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <Windows.h>
#include <mms/plugin.h>
#include <memory>
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
class HistoryChange;
// Internal migration routes only. A bound route consumes failure/cancellation;
// the caller must never continue into the legacy immediate-free deletion.
bool TryDeleteBoundModel(MMDApp*, HWND) noexcept;
bool TryDeleteBoundAccessory(MMDApp*, HWND) noexcept;
bool TryImportBoundModel(MMDApp*, const wchar_t*, bool showInfo) noexcept;
bool TryImportBoundAccessory(MMDApp*, const wchar_t*) noexcept;
bool TryEditBoundAccessorySettings(MMDApp*) noexcept;
// Full GUI save captures the source before its file dialog, with or without a
// migration binding. A bound direct writer uses the same guarded publication.
bool TrySaveEnhancedModelDialog(MMDApp*, HWND) noexcept;
bool TrySaveBoundEnhancedModel(MMDApp*, const wchar_t* path) noexcept;
bool InitializeBoundAccessorySettings(MMDApp*, HWND, int& count) noexcept;
bool BoundAccessorySettingsDialog(const MMDApp*, HWND) noexcept;
// Consumes every bound OK, including a refused or stale dialog. The plan is
// read here, but only published by the caller after the modal has closed.
bool AcceptBoundAccessorySettings(MMDApp*, HWND) noexcept;
// deferUi is only for the Manager's already-authorized top-level edit call.
// The caller queues the normal deferred publication notification afterwards.
MmsResult RunBoundObjectHistory(MMDApp&, SharedHistory&, bool redo, bool deferUi = false) noexcept;
// Modal model-order plans are immutable and their scratch editor is closed.
// A successful journal publication remains committed if UI refresh must retry.
MmsResult CommitBoundModelOrder(MMDApp&, SharedHistory&, std::shared_ptr<HistoryChange>, bool calculate) noexcept;
// The operation guard covers confirmation and post-publication Win32 calls.
// Only its synchronous, message-free journal section receives authorization.
bool NativeObjectOperationActive(const MMDApp*) noexcept;
bool NativeObjectPublicationAuthorized(const MMDApp*) noexcept;
// Deferred plugin publication uses this same UI-only refresh. Failed control
// writes are retryable; save/reset must not serialize partial object lists.
bool RefreshBoundObjectUi(MMDApp&, SharedHistory&) noexcept;
// Deferred consumption of one accumulated publication mask. SceneReadout runs
// the original full object/scene refresh above; every other mask only updates
// the history controls and, for Viewport, requests drawing. It never marks the
// project dirty or resets physics: those belong to the publication itself, so an
// old deferred request cannot dirty a scene that was saved afterwards.
bool RefreshBoundHistoryUi(MMDApp&, SharedHistory&, HistoryEffects) noexcept;
bool NativeObjectUiPending(const MMDApp*) noexcept;
void ForgetBoundObjectUi(const MMDApp*) noexcept;
}
#endif
