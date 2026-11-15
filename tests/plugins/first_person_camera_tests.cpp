#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/legacy_mmd_plugin.hpp"
#include "../../src/legacy_mmhack/tests/device_probe_base.hpp"
#include <mms/legacy_camera.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <memory>
#include <stdexcept>
using namespace mikumikustudio;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool Near(float a, float b) { return std::abs(a-b) < .0001f; }
}
int main() {
    auto app = std::make_unique<MMDApp>();
    auto renderer = std::make_unique<D3DRenderer>();
    mme_re::mmhack::test::DeviceProbeBase device;
    const auto window = CreateWindowW(L"STATIC", L"Camera input test", WS_OVERLAPPEDWINDOW,
        0,0,400,300,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    const auto edit = CreateWindowW(L"EDIT", L"text", WS_CHILD,0,0,100,20,window,nullptr,GetModuleHandleW(nullptr),nullptr);
    int result = 1;
    try {
        Check(window && edit, "test windows");
        app->MainWindow() = window; app->Renderer() = renderer.get(); renderer->device = &device;
        app->CameraMode() = 1; app->ViewportRect() = {0,30,400,300};
        app->CameraDistance() = -10;
        legacy_mmd_plugin::Start(app.get());
        Check(GetModuleHandleW(L"MMSFirstPersonCamera.dll") != nullptr, "camera adapter factory/start");
        const auto acquire = reinterpret_cast<MmsAcquireLegacyCameraApiFn>(GetProcAddress(GetModuleHandleW(nullptr), MMS_LEGACY_CAMERA_API_EXPORT));
        Check(acquire && !acquire(2), "camera API version negotiation");
        MmsLegacyCameraStateV1 state{}; state.byteSize = sizeof(state); state.version = 1;
        Check(acquire(1)->read(&state) == MMS_E_NOT_READY, "camera API escaped callback lifetime");
        SetFocus(window);
        Check(legacy_mmd_plugin::HandleInput(window,WM_KEYDOWN,'W',0), "W input not dispatched");
        Check(Near(app->CameraPositionZ(),1), "W forward movement");
        legacy_mmd_plugin::HandleInput(window,WM_KEYUP,'W',0);
        Check(Near(app->CameraPositionZ(),1), "key release moved camera");
        legacy_mmd_plugin::HandleInput(window,WM_KEYDOWN,'D',0);
        Check(Near(app->CameraPositionX(),.5f), "D strafe movement");
        SetFocus(edit);
        Check(!legacy_mmd_plugin::HandleInput(window,WM_KEYDOWN,'W',0) && Near(app->CameraPositionZ(),1), "typing moved camera");
        SetFocus(window);
        BYTE keys[256]{}, savedKeys[256]{};
        Check(GetKeyboardState(savedKeys), "read keyboard state");
        for (const int modifier : {VK_CONTROL, VK_MENU, VK_LWIN}) {
            std::memcpy(keys, savedKeys, sizeof(keys)); keys[modifier] |= 0x80;
            Check(SetKeyboardState(keys), "set shortcut modifier");
            Check(!legacy_mmd_plugin::HandleInput(window,WM_KEYDOWN,'S',0) && Near(app->CameraPositionZ(),1), "shortcut moved camera");
        }
        Check(SetKeyboardState(savedKeys), "restore keyboard state");
        EnableWindow(window,FALSE);
        Check(!legacy_mmd_plugin::HandleInput(window,WM_KEYDOWN,'W',0) && Near(app->CameraPositionZ(),1), "disabled window moved camera");
        EnableWindow(window,TRUE);
        app->PlaybackActive() = 1;
        Check(!legacy_mmd_plugin::HandleInput(window,WM_KEYDOWN,'W',0) && Near(app->CameraPositionZ(),1), "playback camera edit");
        app->PlaybackActive() = 0;
        CURSORINFO before{sizeof(before)}, after{sizeof(after)}; GetCursorInfo(&before);
        Check(!legacy_mmd_plugin::HandleInput(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(10,10)), "panel mouse captured");
        Check(legacy_mmd_plugin::HandleInput(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(100,100)), "viewport mouse not captured");
        Check(GetCapture() == window, "capture absent");
        legacy_mmd_plugin::HandleInput(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(150,125));
        Check(Near(app->CameraYaw(),-.1f) && Near(app->CameraPitch(),-.05f) && Near(app->CameraDistance(),0), "mouse look pose");
        legacy_mmd_plugin::HandleInput(window,WM_KILLFOCUS,0,0);
        GetCursorInfo(&after);
        Check(!GetCapture() && before.flags == after.flags, "focus loss did not restore cursor/capture");
        const auto floating = CreateWindowW(L"STATIC",L"Floating viewport",WS_OVERLAPPEDWINDOW,
            0,0,400,300,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(floating != nullptr,"floating viewport window"); app->FloatingWindow() = floating;
        Check(legacy_mmd_plugin::HandleInput(floating,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(100,100)) && GetCapture()==floating,"floating viewport capture");
        legacy_mmd_plugin::HandleInput(floating,WM_CANCELMODE,0,0);
        Check(!GetCapture(),"floating viewport release"); app->FloatingWindow() = nullptr; DestroyWindow(floating);
        legacy_mmd_plugin::HandleInput(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(100,100));
        legacy_mmd_plugin::Stop();
        GetCursorInfo(&after);
        Check(!GetCapture() && before.flags == after.flags, "stop did not restore cursor/capture");
        Check(!GetModuleHandleW(L"MMSFirstPersonCamera.dll"), "adapter did not unload");
        const auto z = app->CameraPositionZ();
        Check(!legacy_mmd_plugin::HandleInput(window,WM_KEYDOWN,'W',0) && app->CameraPositionZ() == z, "input after stop");
        std::puts("PASS camera factory, WASD, key release, text focus, playback gate, mouse look, focus loss and stop");
        result = 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what()); }
    legacy_mmd_plugin::Stop();
    renderer->device = nullptr; app->Renderer() = nullptr; app->MainWindow() = nullptr;
    DestroyWindow(window);
    return result;
}
