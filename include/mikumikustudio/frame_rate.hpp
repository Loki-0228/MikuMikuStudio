#pragma once
#include <cstdint>

namespace mikumikustudio {
class MMDApp;
namespace frame_rate {
// CPU-side bookkeeping only. A scene completion can contribute at most one
// successfully submitted display frame, regardless of temporal samples/HUDs.
struct State {
    double epoch = 0;
    double elapsed = 0;
    std::uint32_t frames = 0;
    std::uint64_t presentedFrames = 0;
    int rate = 0;
    bool started = false;
    bool ready = false;

    bool Tick(double now) {
        if (!started || now < epoch) {
            epoch = now; elapsed = 0; frames = 0; started = true;
            return false;
        }
        elapsed = now - epoch;
        if (elapsed < 1) return false;
        rate = int(double(frames) / elapsed + .5);
        epoch = now; elapsed = 0; frames = 0;
        return true;
    }
    void Begin() { ready = false; }
    void Complete() { ready = true; }
    bool Present(bool success) {
        const bool count = ready && success;
        ready = false;
        if (count) { ++frames; ++presentedFrames; }
        return count;
    }
};
void Tick(MMDApp* app);
void BeginScene(MMDApp* app);
void CompleteScene(MMDApp* app);
// Native DX12 calls this after its own successful swap-chain Present; DX9,
// MME and MMV call it after the host's Present. Repeated presents do not count.
void Presented(MMDApp* app, bool success);
void Reset(MMDApp* app);
}
}
