#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <array>
#include <memory>
#include <Windows.h>
#include <mms/plugin.h>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
class SharedHistory;
// Internal modal authoring session. Begin captures values and identity without
// holding a journal reservation. Apply revalidates after the modal loop, then
// uses the ordinary synchronous pose transaction. Does not own scene objects.
// The MMDApp must outlive this session; scene objects and bindings may change.
class BoneNumericEdit {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    BoneNumericEdit();
    ~BoneNumericEdit();
    MmsResult Begin(MMDApp&, SharedHistory&) noexcept;
    std::array<float, 6> Values() const noexcept;
    // Consumes the session on success, no-op or failure. No UI calls on failure.
    MmsResult Apply(const std::array<float, 6>&) noexcept;
};
// Command 300, sharing the same recursion guard for bone and camera dialogs.
bool TryEditBoundPoseNumeric(MMDApp*, HWND owner) noexcept;
// View-menu command 302 in model mode. Owns rejection and no-op while bound;
// Camera mode is routed through native_global_commands.
bool TryResetBoundBoneRotation(MMDApp*) noexcept;
enum class NativePoseAxisKind { Position, Rotation, Scale };
enum class NativePoseAxisSource { MainWindow, FloatingWindow };
// Ordinary enhanced runtime: preserve the native per-model undo ring and
// handle failures without falling back to the old non-atomic writer.
bool TryResetUnboundBoneRotationAxis(MMDApp*, int axis,
    NativePoseAxisSource source = NativePoseAxisSource::MainWindow) noexcept;
bool TryResetUnboundBoneRotation(MMDApp*) noexcept;
// Internal migration route, enabled only by NativePoseTestBinding. True means
// this command was handled, including no-op, busy and failed edits: callers
// must not fall back to the legacy path. Camera position/rotation axes share
// the camera pose record. All readout messages run after the transaction ends.
bool TryResetBoundPoseAxis(MMDApp*, NativePoseAxisKind, int axis,
    NativePoseAxisSource source = NativePoseAxisSource::MainWindow) noexcept;
}
#endif
