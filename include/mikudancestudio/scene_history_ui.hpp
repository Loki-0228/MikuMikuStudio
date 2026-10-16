#pragma once

namespace mikudancestudio {
class MMDApp;
namespace mdl { struct AccessoryRecord; }

namespace scene_history_ui {
// Capture immediately before one scene publication and use for its UI refresh.
// The caller must keep these objects alive until refresh finishes. This is not
// a persistent selection token: addresses must not be reused in that interval.
struct Selection {
    const unsigned char* model = nullptr;
    const mdl::AccessoryRecord* accessory = nullptr;
    bool cameraMode = true;
    // Camera-mode registration scope: 0..3, or the accessory below. A removed
    // scope accessory falls back to camera without changing timeline selection.
    int globalScope = 0;
    const mdl::AccessoryRecord* scopeAccessory = nullptr;
};
enum class Result {
    Ok,
    WrongThread,
    MissingControl,
    InvalidOrder,
    InvalidData,
    OutOfMemory,
    ControlFailure,
};

// UI thread only. Does not create scene identities or retain object ownership.
Selection CaptureSelection(const MMDApp&) noexcept;

// Call after native publication, outside its no-fail Publish method. Native
// model comboSelIndex must be 1..N; accessory order must be 0..N-1. The helper
// validates both and never renumbers objects or modifies bindings/poses/keys.
// On success, combo 436 has N+1 rows and combo 471 has one row per accessory,
// in the same order used by native selection handlers and PMM serialization.
//
// Only selection/mode fields and the object/parent/scope combos are refreshed.
// A deleted selected model falls back to camera; a deleted selected accessory
// falls back to the first remaining accessory. Model-mode scope 434, morph/IK
// panels, menu state, panel visibility, and viewport repaint remain caller work.
// Undo/redo buttons use the supplied common-history availability in both modes.
//
// All text and order validation precede control writes. Win32 control updates
// are not atomic: ControlFailure can leave partial UI changes (and the safe
// selection already applied). Retry from native state before allowing a PMM
// save. No application callback, seek, pose evaluation, or message pumping is
// invoked directly. SendMessage still runs Win32 window procedures/subclasses;
// the caller must guard scene lifetime against their synchronous reentry.
Result RefreshObjectLists(MMDApp&, const Selection&, bool canUndo, bool canRedo) noexcept;
} // namespace scene_history_ui
} // namespace mikudancestudio
