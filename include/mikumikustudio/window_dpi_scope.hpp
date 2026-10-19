#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace mikumikustudio {

// Window geometry must use the coordinate system in which the window was
// created. A codec or effect can leave a different DPI context on our thread.
// Resolve these APIs dynamically so older Windows versions still load MMS.
class WindowDpiScope {
public:
    explicit WindowDpiScope(HWND window) noexcept {
        const auto& api = Api();
        if (!window || !api.getWindow || !api.setThread) return;
        const auto context = api.getWindow(window);
        if (context) previous_ = api.setThread(context);
    }
    ~WindowDpiScope() {
        if (previous_) Api().setThread(previous_);
    }
    WindowDpiScope(const WindowDpiScope&) = delete;
    WindowDpiScope& operator=(const WindowDpiScope&) = delete;

private:
    struct Functions {
        using GetWindow = DPI_AWARENESS_CONTEXT (WINAPI*)(HWND);
        using SetThread = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);
        GetWindow getWindow = nullptr;
        SetThread setThread = nullptr;
    };
    static const Functions& Api() noexcept {
        static const Functions functions = [] {
            const auto user32 = GetModuleHandleW(L"user32.dll");
            Functions result;
            if (user32) {
                result.getWindow = reinterpret_cast<Functions::GetWindow>(
                    GetProcAddress(user32, "GetWindowDpiAwarenessContext"));
                result.setThread = reinterpret_cast<Functions::SetThread>(
                    GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
            }
            return result;
        }();
        return functions;
    }
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

}  // namespace mikumikustudio
