#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <memory>
#include <string>

struct IDirect3DDevice9;

namespace mikudancestudio {
class MMDApp;
namespace antialias {
// Value 6 was the original HIGH-preset SMAA 1x preference; keep it stable.
enum class Algorithm : unsigned {
    MmdOriginal, Msaa2, Msaa4, Msaa8, Fxaa, Taa, Smaa,
    SmaaStandard, SmaaT2xStandard, SmaaT2xEnhanced, Count
};
enum class OutlineMode : unsigned { FxaaAdaptive, Multisample };
inline constexpr unsigned kFirstCommand = 34900;
inline constexpr unsigned kSmaaCommand = 34932;
inline constexpr unsigned kOffCommand = 34933;
inline constexpr unsigned kOutlineCommand = 34934;
inline constexpr unsigned kOutlineFxaaCommand = 34935;
inline constexpr unsigned kOutlineMsaaCommand = 34936;
struct Settings {
    Algorithm algorithm = Algorithm::MmdOriginal;
    Algorithm lastSmaaAlgorithm = Algorithm::Smaa;
    OutlineMode outlineMode = OutlineMode::FxaaAdaptive;
    bool enabled = true;
    bool initialized = false;
    bool projectOverride = false; // Retain loaded preferences until device creation.
    unsigned originalSamples = 0, originalQuality = 0;
};
void Initialize(MMDApp* app);
void ApplyProjectSettings(MMDApp* app, bool enabled, Algorithm algorithm, Algorithm lastSmaa);
void ReloadUserSettings(MMDApp* app);
void RefreshMenu(MMDApp* app);
const wchar_t* DialogTitle(MMDApp* app);
bool Supported(MMDApp* app, Algorithm algorithm);
bool Select(MMDApp* app, Algorithm algorithm, std::wstring& error);
bool SelectOutlineMode(MMDApp* app, OutlineMode mode, std::wstring& error);
bool Toggle(MMDApp* app, std::wstring& error);
void ApplyRenderState(MMDApp* app);
void PrepareFrame(MMDApp* app);
// True while an enabled post-process algorithm is preparing its first frame.
bool Initializing(MMDApp* app);
unsigned AccumulatedSamples(MMDApp* app);
// Number of actual scene submissions needed for the selected temporal mode.
// A failed motion pass reduces this to one until a later submission succeeds.
unsigned RequiredSamples(MMDApp* app);
void ResetHistory(MMDApp* app);
void Release(MMDApp* app);
// Native inverted-hull coverage on the active post-process scene. The caller
// retains the original draw when this returns false (MSAA, disabled, or failure).
bool DrawOutline(MMDApp* app, unsigned char* model, unsigned firstIndex,
                 unsigned indexCount, const float* uniformColor);
// Resolve native outline coverage before the following accessories/MME passes.
void FinishOutlines(MMDApp* app);
// Only this frame's native outline mask may share the scene MRT with motion
// capture. External material/effect MRT outputs are never accepted here.
bool NativeOutlineMaskDraw(MMDApp* app, IDirect3DDevice9* device);
bool HandleMessage(MMDApp* app, UINT message, WPARAM wp, LPARAM lp);
LRESULT CALLBACK MenuFilter(int code, WPARAM wp, LPARAM lp);
// Owns a single-sample scene for post-process AA. MME sees one ordinary frame;
// the final shader runs on the native device after MME's EndScene callbacks.
class FrameScope {
public:
    explicit FrameScope(MMDApp* app);
    ~FrameScope();
    bool Ready() const;
    bool Complete();
    // Also true when this frame did not need a post-process resolve.
    bool Succeeded() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
}
