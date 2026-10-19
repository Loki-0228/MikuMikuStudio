#pragma once

#include <cstdint>

namespace mikumikustudio {

class MMDApp;

// Repair the legacy MME English menu and preserve its switch across languages.
void PrepareEffectDefaults();
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
void PrepareEffectProjectPaths(const wchar_t* project);

// MME rebuilds its object/order table inside IDirect3DDevice9::BeginScene.
void BeginEffectObjectRegistration();
// Invalidate repeated-object queries before scene ownership changes.
void InvalidateEffectCurrentObjectCache();

// The modern CRT does not pass through the host import that MME hooks.
void NotifyEffectFileOpen(const wchar_t* path);
// Notify after a successful PMM write; reopens without truncating the file.
void NotifyEffectFileSave(const wchar_t* path);

}  // namespace mikumikustudio
