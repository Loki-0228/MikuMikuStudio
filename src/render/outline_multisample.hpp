#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d9.h>
#include <string>

namespace mikumikustudio {
class MMDApp;
namespace antialias::outline_multisample {
struct Stats {
    unsigned sampleCount=0;
    unsigned depthReplays=0;
    unsigned outlineDraws=0;
    unsigned composites=0;
    bool observing=false;
    bool observedNativeDevice=false;
    bool frameFailed=false;
    std::string fallbackReason;
};
// Begin must run after binding the single-sample scene but before BeginScene.
// A false result with Failed()==false permits the existing outline fallback.
bool Begin(MMDApp*,IDirect3DDevice9*,IDirect3DSurface9* mainScene,const D3DVIEWPORT9&);
// True means handled, including a poisoned frame that must not be redrawn.
bool Draw(MMDApp*,unsigned char* model,unsigned firstIndex,unsigned indexCount,const float* uniformColor);
// Call at the end of the native outline stage, before MME post-processing.
// False means this frame must be rejected; suppressed outlines cannot be
// silently recovered by switching algorithms after their original draw time.
bool Finish(MMDApp*);
bool Active(MMDApp*);
bool Failed(MMDApp*);
Stats GetStats(MMDApp*);
// Borrowed premultiplied line layer; non-null only after a successful Finish.
IDirect3DTexture9* GetTexture(MMDApp*);
// Raw coverage is the default. Production may opt into layer-only softening.
void SetSoftening(MMDApp*,bool);
// Cancel collection on any early frame exit; retain reusable GPU resources.
void Cancel(MMDApp*);
void Release(MMDApp*);
}
}
