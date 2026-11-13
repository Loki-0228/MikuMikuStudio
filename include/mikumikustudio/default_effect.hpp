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
// A capture range may enable self-shadow after its first frame.
// Start pending compilation without waiting on the UI thread.
bool DefaultEffectReadyForCapture(MMDApp* app);
void CancelDefaultEffectCompilation(D3DRenderer* renderer);
}
