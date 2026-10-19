#pragma once

namespace mikumikustudio {
class MMDApp;
class D3DRenderer;

// Queue during device setup, attach on demand when scene shading is enabled.
// Only the compiler runs on a worker; all device calls remain on the UI thread.
// Full builds embed bytecode. MME retains its resource entry before BeginScene.
bool QueueDefaultEffect(D3DRenderer* renderer, int resource);
void StartDefaultEffectCompilation(D3DRenderer* renderer);
void UpdateDefaultEffect(MMDApp* app);
void CancelDefaultEffectCompilation(D3DRenderer* renderer);
}
