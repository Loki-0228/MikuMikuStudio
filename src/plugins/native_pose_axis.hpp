#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
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
