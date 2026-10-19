#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace mikumikustudio {
class MMDApp;
namespace viewport_render_scale {
inline constexpr unsigned kCommand = 32814;
inline constexpr int kDialog = 9140;
enum Control { Slider = 9141, Percent, Automatic, Dimensions, Warning, Apply };
struct Size { unsigned width = 0, height = 0; };
bool ParsePercent(const wchar_t* text, double& percent);
bool CalculateSize(unsigned width, unsigned height, double percent, Size& size);
bool Above4K(Size size);
Size PreviewSize(MMDApp* app, bool automatic, double percent);
// Keep secondary shadow passes within the preview budget, retaining the
// existing power-of-two tiers and 512-pixel floor. Exports keep full quality.
unsigned ShadowSize(MMDApp* app, unsigned fullSize);
bool Configure(MMDApp* app, bool automatic, double percent, std::wstring& error);
void RefreshMenu(MMDApp* app);
bool HandleCommand(MMDApp* app, unsigned command);
INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp);
void Release(MMDApp* app);
bool Rendering(MMDApp* app);

// Render into a separate color/depth pair, then composite into the unchanged
// display viewport. Input projection and window geometry stay in display pixels.
class FrameScope {
public:
    explicit FrameScope(MMDApp* app);
    ~FrameScope();
    FrameScope(const FrameScope&) = delete;
    FrameScope& operator=(const FrameScope&) = delete;
    bool Ready() const { return ready_; }
    // Resolve and restore display coordinates now, before drawing the HUD.
    bool Complete();
private:
    struct BufferBackup {
        IDirect3DVertexBuffer9* buffer;
        std::vector<unsigned char> bytes;
    };
    bool ScaleBuffer(IDirect3DVertexBuffer9* buffer, unsigned count, unsigned stride);
    bool Restore(bool present);
    MMDApp* app_ = nullptr;
    IDirect3DDevice9* device_ = nullptr;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> previous_, depth_;
    IDirect3DSurface9* previousSlot_ = nullptr;
    IDirect3DSurface9* depthSlot_ = nullptr;
    RECT display_{};
    D3DVIEWPORT9 viewport_{};
    float viewScale_ = 1;
    float scaleX_ = 1, scaleY_ = 1;
    DWORD scissorEnabled_ = FALSE;
    RECT scissor_{};
    bool ready_ = true;
    std::vector<BufferBackup> buffers_;
};
}
}
