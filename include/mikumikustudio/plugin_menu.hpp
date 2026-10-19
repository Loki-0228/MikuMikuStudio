#pragma once
namespace mikumikustudio {
class MMDApp;
namespace plugin_menu {
void Refresh(MMDApp*);
void Shutdown(MMDApp*);
bool HandleCommand(MMDApp*,unsigned);
bool HandleShortcut(MMDApp*,unsigned);
}
}
