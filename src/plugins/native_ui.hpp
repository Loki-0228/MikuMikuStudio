#pragma once
#include "ui_registry.hpp"

namespace mikumikustudio::plugins {
class NativeUi {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    NativeUi(UiRegistry&, HWND main, Log);
    ~NativeUi();
    void Refresh();
    bool Command(unsigned id);
    bool Shortcut(unsigned key, unsigned modifiers, bool viewportFocused);
    // Returns reserved height. Coordinates use the parent window's DPI space.
    int Layout(int x, int width, int maximumHeight, bool visible);
    // The refresh LAYOUT path calls this before publishing ready geometry;
    // the application updates dependent viewport geometry without scene work.
    void SetLayoutPublished(std::function<MmsResult(int)>);
    HWND Toolbar() const;
    // Native painting/layout and popup tracking own adapter stacks independently
    // of plugin callback permissions. DeferClose coalesces requests while those
    // stacks are active, then posts one WM_CLOSE after the outermost return.
    // PopupActive reports popup tracking only, not general native work.
    bool PopupActive() const noexcept;
    bool DeferClose(WPARAM, LPARAM) noexcept;
    bool DialogMessage(MSG*);
};
}
