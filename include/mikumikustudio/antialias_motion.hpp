#pragma once
#include <d3d9.h>

namespace mikumikustudio {
class MMDApp;
namespace antialias::motion {
// Captures the final native vertex-shader positions, including MME deformation.
// Jitter is in clip-space offsets (the amount added to clip.xy / clip.w).
bool Begin(MMDApp*, IDirect3DDevice9*, IDirect3DSurface9* sceneTarget,
           const D3DVIEWPORT9&, const D3DMATRIX& unjitteredProjection,
           float jitterClipX, float jitterClipY);
// RGBA32F: previousUV-currentUV, current view depth, expected previous depth.
// Depth <= 0 marks uncovered pixels or rejected history. No ownership transfer.
IDirect3DTexture9* Resolve(MMDApp*, IDirect3DDevice9*);
// Advance only after both the history colour and history depth were saved.
void Commit(MMDApp*);
void Cancel(MMDApp*);
void Release(MMDApp*);
struct Stats {
    unsigned draws = 0;
    unsigned matchedDraws = 0;
    float maxMotionPixels = 0; // CPU geometry only; GPU fields are never read back for statistics.
    unsigned vertexCaptures = 0;
    unsigned vertexCacheHits = 0;
    unsigned gpuVertexCaptures = 0;
    unsigned cpuVertexReadbacks = 0;
    unsigned coverageReplays = 0;
    unsigned opaqueCoverageSkips = 0;
};
Stats GetStats(MMDApp*);
// Borrow the latest field for GPU consumers or explicit diagnostic capture.
// Does not finish a frame, change history, or read anything back to the CPU.
IDirect3DTexture9* GetTexture(MMDApp*);
}
}
