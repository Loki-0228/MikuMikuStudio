#pragma once
#include "ui_registry.hpp"

namespace mikudancestudio::plugins {
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
    bool DialogMessage(MSG*);
};
}
