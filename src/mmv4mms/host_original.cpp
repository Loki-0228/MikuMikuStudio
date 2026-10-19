#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/mmv4mms.hpp"

static_assert(MMD_ORIGINAL_PROFILE, "The original MMV provider belongs to the mmd932 profile");

namespace mikumikustudio::mmv4mms {
// Keep lifecycle calls valid without loading a plugin or registering its menu.
void Initialize(MMDApp*) {}
void Shutdown() {}
void ClearScene() {}
void Forget(const void*) {}
void ProjectOpened(MMDApp*, const wchar_t*) {}
void ProjectSaved(MMDApp*, const wchar_t*) {}
void DeviceLost() {}
void RefreshLanguage(MMDApp*) {}
bool Active() { return false; }
int Render(MMDApp*, const float[16]) { return 0; }
bool Present(MMDApp*) { return false; }
}
