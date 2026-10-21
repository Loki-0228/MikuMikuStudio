#pragma once
struct tagMSG;
namespace mikumikustudio {
class MMDApp;
namespace plugins {
void Initialize(MMDApp*);
void Shutdown();
void Refresh(MMDApp*);
void Layout(MMDApp*);
int ToolbarHeight(MMDApp*);
int PropertyAreaHeight(MMDApp*);
int LeftAreaWidth(MMDApp*);
int RightAreaWidth(MMDApp*);
bool HandleCommand(unsigned id);
bool HandleShortcut(unsigned key, bool viewportFocused);
bool HandleDialogMessage(tagMSG*);
bool InCallback();
}
}
