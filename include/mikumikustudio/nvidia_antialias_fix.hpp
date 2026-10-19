#pragma once
#include <string>

namespace mikumikustudio::nvidia_antialias_fix {
// File presence alone controls visibility. All driver access belongs to the DLL.
bool ShouldOfferInstall();
bool QueryInstalled(bool& installed, std::wstring& error, int language = 2);
// Called only by the explicit Help menu command. owner is the host HWND.
// Windows loads the optional DLL in a separate elevated registration process.
// Language follows the host: Japanese = 0, English = 1, Chinese = 2.
bool InstallForCurrentExecutable(void* owner, std::wstring& error, int language = 2);
bool UninstallForCurrentExecutable(void* owner, std::wstring& error, int language = 2);
}
