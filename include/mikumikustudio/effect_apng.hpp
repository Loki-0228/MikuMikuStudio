#pragma once

namespace mikumikustudio {

// Call once after the initial D3D device and native MME have loaded, before
// rendering. Only the exact supported MMEffect 0.37 x64 image is accepted.
// Unknown images, existing hooks and failed validation are left unpatched.
// Once a replacement can become visible, the DLL reference and original-call
// context are retained until process exit; there is intentionally no unload API.
bool InstallEffectApngCompatibility();

// True only after successful installation and while slot 4 still points to our
// replacement. This does not install, retry or replace another owner's hook.
bool IsEffectApngCompatibilityInstalled();

} // namespace mikumikustudio
