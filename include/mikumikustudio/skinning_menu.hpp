#pragma once
#include <Windows.h>
#include <string>
namespace mikumikustudio {
class MMDApp;

// The single 运算器 / Processors / 演算器 entry under Help. The host-wide main
// renderer and vertex compute roles are always present; the MMV role is added
// only while an MMV DLL with the global-processor capability is loaded.
// Every choice only updates the per-user processor.ini through
// processor::SaveRequested.  Nothing here changes a device in the running
// process; a change starts on the next launch.
constexpr unsigned kSkinningMenuId = 32000;

// Vertex compute role.  These are the original command ids, kept so existing
// callers and tests stay valid: 32001 automatic, 32002 CPU, 32100..32227 the
// enumerated adapters (kSkinningAdapterLimit of them).
constexpr unsigned kSkinningAutomaticId = 32001;
constexpr unsigned kSkinningCpuId = 32002;
constexpr unsigned kSkinningAdapterFirstId = 32100;
constexpr unsigned kSkinningAdapterLimit = 128;

// Main renderer role (the D3D9 device): 33000..33128.  A full-tree scan of
// 32xxx/33xxx literals found no other command in this range - the only 33xxx
// number in the tree is 33537, an _sopen_s flag in src/features/dialog_gaps.cpp.
constexpr unsigned kRendererAutomaticId = 33000;
constexpr unsigned kRendererAdapterFirstId = 33001;
constexpr unsigned kRendererAdapterLimit = 128;

// MMV role: 33400..33529.
constexpr unsigned kMmvFollowRendererId = 33400;
constexpr unsigned kMmvCpuId = 33401;
constexpr unsigned kMmvAdapterFirstId = 33402;
constexpr unsigned kMmvAdapterLimit = 128;

void RefreshSkinningMenu(MMDApp*);
bool HandleSkinningCommand(MMDApp*, unsigned command);
}
