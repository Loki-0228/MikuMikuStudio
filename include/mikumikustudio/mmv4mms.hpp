#pragma once
namespace mikumikustudio {
class MMDApp;
namespace mmv4mms {
void Initialize(MMDApp*);
void Shutdown();
void ClearScene();
void Forget(const void*);
void ProjectOpened(MMDApp*,const wchar_t*);
void ProjectSaved(MMDApp*,const wchar_t*);
void DeviceLost();
void RefreshLanguage(MMDApp*);
bool Active();
// 0: optional renderer inactive; 1: completed frame; -1: current frame failed.
int Render(MMDApp*,const float world[16]);
bool Present(MMDApp*);
}
}
