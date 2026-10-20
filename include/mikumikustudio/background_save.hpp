#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <functional>
#include <string>

namespace mikumikustudio {
class MMDApp;
bool SceneSaveInProgress() noexcept;
namespace background_save {
// Only the worker callbacks run off the UI thread. They must own their data
// and may not access MMDApp, windows, MME, or render resources.
struct Options {
    bool backup = false;
    std::function<bool()> beforeWrite;
    std::function<bool()> afterWrite;
    std::function<void(bool)> completed;
};
enum class Status { Saving, BackingUp, Busy, Saved, BackedUp, Overwritten,
                    Failed, BackupFailed, Full, WaitingToClose };
inline constexpr UINT kComplete = WM_APP + 0x537;
inline constexpr UINT_PTR kStatusTimer = 0x4d535356;
// Success means accepted, not yet on disk. The destination is captured here.
bool Request(MMDApp*, const wchar_t* destination, Options options = {});
void SetStatus(MMDApp*, Status, bool persistent = false);
const wchar_t* StatusText(MMDApp*);
// The backup worker reports its decision without accessing scene/UI state.
void ReportBackupOverwrite() noexcept;
bool HandleMessage(MMDApp*, UINT, WPARAM);
// Join and deliver completion before a scene replacement or orderly shutdown.
void Wait(MMDApp*);
bool DeferClose(MMDApp*);
}
}
