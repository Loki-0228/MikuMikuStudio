#pragma once
#include <cstdint>
struct tagMSG;
namespace mikumikustudio {
class MMDApp;
namespace plugins {
void Initialize(MMDApp*);
void Shutdown();
void Refresh(MMDApp*);
void Layout(MMDApp*);
// Internal native-drag handoff. Mount bits: toolbar=1, properties=2,
// left=4, right=8. Success restores geometry before the caller takes its origin.
// A zero token denotes a host with no active plugin UI.
bool BeginLayoutInteraction(MMDApp*, std::uint32_t mounts, std::uint64_t& token) noexcept;
void EndLayoutInteraction(MMDApp*, std::uint64_t token) noexcept;
int ToolbarHeight(MMDApp*);
int PropertyAreaHeight(MMDApp*);
int LeftAreaWidth(MMDApp*);
int RightAreaWidth(MMDApp*);
bool HandleCommand(unsigned id);
bool HandleShortcut(unsigned key, bool viewportFocused);
bool HandleDialogMessage(tagMSG*);
bool InCallback();
// UI-thread query for native close dispatch; does not invoke plugin code.
bool CloseBlocked(MMDApp*) noexcept;
}
}
