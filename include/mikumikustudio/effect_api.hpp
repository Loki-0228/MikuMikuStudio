#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct IDirect3DDevice9;

namespace mikumikustudio {

class MMDApp;
namespace project_relocation { class RootMappings; }

// Repair the legacy MME English menu and preserve its switch across languages.
void InstallEffectMenuCompatibility(MMDApp* app);
// Localize the live MME popup only when MMEffect.dll is already loaded.
void RefreshEffectMenuLanguage(MMDApp* app);

// MMHack's named export returns plugin-owned UTF-16 text. Preserve the caller's
// preference; consult the other language only when the requested name is empty.
using EffectMaterialNameQuery = const wchar_t* (__cdecl*)(std::uintptr_t, int, int);
const wchar_t* EffectMaterialNameWithFallback(EffectMaterialNameQuery query,
    std::uintptr_t object, int material, int english);
// Optional, in-memory compatibility for MMEffect's named MMHack import.
// Does not load either DLL or write DLL/model files. False means unavailable.
bool InstallEffectMaterialNameCompatibility();
// Resolve legacy EMM path bytes against existing files in native MME only.
void InstallEffectPathCompatibility();
// Retain relative EMM entries before MME expands them against the EXE folder.
// Reads the sidecar only; the user's assignment file is never rewritten.
// A null/empty project retires relative assignments when the workspace resets.
// Notification aliases remain stable until exit for already queued MME work.
void PrepareEffectProjectPaths(const wchar_t* project);
// Only report missing references read from this project's sidecar.
std::vector<std::wstring> MissingProjectEffects(const wchar_t* project);
// Compile with the legacy provider's D3DX compiler on the current device.
bool CompileLegacyMmeRecoveryEffect(IDirect3DDevice9* device, const wchar_t* path);
// Bind confirmed model recovery to the matching EMM after successful PMM load.
void PublishEffectProjectRelocation(const wchar_t* project,
    std::weak_ptr<const project_relocation::RootMappings> roots) noexcept;
// Use a process-local, explicitly mapped EMM alias for retained PMM suffixes.
// Empty means no safe notification alias is available; never use a guessed path.
std::wstring EffectProjectNotificationPath(const wchar_t* project) noexcept;

// MME rebuilds its object/order table inside IDirect3DDevice9::BeginScene.
void BeginEffectObjectRegistration();
// Invalidate repeated-object queries before scene ownership changes.
void InvalidateEffectCurrentObjectCache();

// The modern CRT does not pass through the host import that MME hooks.
void NotifyEffectFileOpen(const wchar_t* path);
// Notify after a successful PMM write; reopens without truncating the file.
void NotifyEffectFileSave(const wchar_t* path);

}  // namespace mikumikustudio
