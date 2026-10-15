#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace mikudancestudio {
class MMDApp;
namespace updates {
// UI-thread adapter between the portable coordinator and the legacy frame
// driver. Preview requests coalesce; ordered recording samples do not.
bool Initialize(MMDApp*);
void Shutdown(MMDApp*);
void SafePoint(MMDApp*);
DWORD WaitMilliseconds(MMDApp*);
void RequestViewport(MMDApp*, bool geometryChanged = false);
void Exposed(MMDApp*, HWND, const RECT& damaged);
void ObserveMessage(MMDApp*, UINT, WPARAM, LPARAM);
bool Running();
bool RepaintOnly();
class MessageScope {
public:
    MessageScope();
    ~MessageScope();
};
class FrameScope {
    bool entered_;
public:
    FrameScope();
    ~FrameScope();
    explicit operator bool() const { return entered_; }
};
}
}
