#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <array>
#include <memory>
#include <Windows.h>
#include <mms/plugin.h>
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
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
// Command 300, bone mode only. Camera mode retains its existing path.
bool TryEditBoundBoneNumeric(MMDApp*, HWND owner) noexcept;
// View-menu command 302 in model mode. Owns rejection and no-op while bound;
// camera-mode authoring remains a separate migration task.
bool TryResetBoundBoneRotation(MMDApp*) noexcept;
enum class NativePoseAxisKind { Position, Rotation, Scale };
enum class NativePoseAxisSource { MainWindow, FloatingWindow };
// Internal migration route, enabled only by NativePoseTestBinding. True means
// this model-mode command was handled, including no-op, busy and failed edits:
// callers must not fall back to the legacy undo ring. Camera commands retain
// their existing path. All readout messages run after the pose scope ends.
bool TryResetBoundPoseAxis(MMDApp*, NativePoseAxisKind, int axis,
    NativePoseAxisSource source = NativePoseAxisSource::MainWindow) noexcept;
}
#endif
